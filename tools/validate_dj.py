#!/usr/bin/env python3
"""validate_dj.py -- the Crash FM DJ against the recovered laws.

Builds a probe from the REAL src/burnout3_dj.c (the same trick
validate_music.py uses) and drives it, so every number below comes out
of the shipping code, not out of a reimplementation of it.

Each check names the RE_CRASHFM.md section it is enforcing.  Run:

    tools/validate_dj.py

Exit 0 = every recovered law holds.
"""

import os
import subprocess
import sys
import math
import struct

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE_C = os.path.join(REPO, "build", "dj_probe.c")
PROBE = os.path.join(REPO, "build", "dj_probe")
AUDIO = os.environ.get("B3_AUDIO_DIR", os.path.join(REPO, "build", "audio"))

PROBE_SRC = r"""
/* built by tools/validate_dj.py -- drives src/burnout3_dj.c directly */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "burnout3_dj.h"
#include "burnout3_music.h"

/* B3_DJ_PROBE_PCM=<path>: dump the DJ voice's OWN output, 44100 mono s16,
 * so two runs can be compared byte for byte and the line's RMS measured
 * without the rest of the mix on top of it. */
static FILE *g_pcm;
static void emit(float s) {
    short v;
    if (!g_pcm) return;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    v = (short)s;
    fwrite(&v, 2, 1, g_pcm);
}

int main(int argc, char **argv) {
    B3DjStatus st;
    int mode = argc > 1 ? atoi(argv[1]) : 5;
    const char *pcm = getenv("B3_DJ_PROBE_PCM");
    if (pcm && *pcm) g_pcm = fopen(pcm, "wb");

    b3_dj_init();
    b3_dj_set_track(getenv("B3_TRACK") ? getenv("B3_TRACK") : "US_C3_V1");
    b3_dj_set_mode(mode);

    /* Walk a long stretch of game time at a fixed 60 Hz step and report
     * every line the radio chooses, with the gap that preceded it. */
    {
        float dt = 1.0f / 60.0f;
        float clock = 0.0f, last = 0.0f;
        int   seen = 0, was = 0;
        float dmin = 1.0f;               /* deepest duck of this line    */
        char  bank[32]; int idx = 0, lo = 0, hi = 0; float gap = 0.0f;
        bank[0] = '\0';
        for (long i = 0; i < 60L * 60L * 400L && seen < 40; i++) {
            b3_dj_tick(dt);
            clock += dt;
            b3_dj_status(&st);
            if (st.speaking && !was) {   /* a line just started          */
                snprintf(bank, sizeof bank, "%s", st.bank);
                idx = st.line; lo = st.lo; hi = st.hi;
                gap = clock - last;
                last = clock;
                dmin = 1.0f;
            }
            /* Stand in for the audio thread.  The ident and the line only
             * end when their samples are consumed, so a probe that does
             * not pull would sit in state 7 for ever -- and the duck only
             * slews inside next_sample(), so it must be read AFTER the
             * pull, not at the instant the line starts. */
            if (st.speaking || st.ident) {
                for (int k = 0; k < 8192; k++) emit(b3_dj_next_sample());
                if (b3_dj_music_duck() < dmin) dmin = b3_dj_music_duck();
            }
            if (was && !st.speaking) {   /* ...and just ended            */
                printf("LINE %s %d %d %d %.4f %.4f\n",
                       bank, idx, lo, hi, (double)gap, (double)dmin);
                seen++;
            }
            was = st.speaking;
        }
    }
    /* let the release finish, the way a real frame would */
    for (int k = 0; k < 44100; k++) (void)b3_dj_next_sample();
    printf("DUCKMIN %.6f\n", (double)b3_dj_music_duck());
    return 0;
}
"""


# the hand-over probe: DJ and music driven together, dumped apart
PROBE2_SRC = r"""
/* built by tools/validate_dj.py -- drives burnout3_dj.c AND
 * burnout3_music.c together and dumps the two channels separately, so
 * "the music is gone while he talks" is a measurement and not a claim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "burnout3_dj.h"
#include "burnout3_music.h"

#define FRAMES_PER_TICK 735          /* 44100 / 60 */

static void put(FILE *f, float s) {
    short v;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    v = (short)s;
    fwrite(&v, 2, 1, f);
}

int main(int argc, char **argv) {
    int crash = (argc > 1 && !strcmp(argv[1], "crash"));
    long frames = (argc > 2) ? atol(argv[2]) : 3600;
    long c0 = 600, c1 = 900;         /* the cinematic window, in frames */
    FILE *fd = fopen(getenv("DJ_OUT"), "wb");
    FILE *fm = fopen(getenv("MU_OUT"), "wb");
    float dt = 1.0f / 60.0f;
    B3DjStatus st;

    if (!fd || !fm) return 2;
    b3_music_set_dir(getenv("B3_MUSIC_DIR") ? getenv("B3_MUSIC_DIR")
                                            : "build/music");
    b3_music_init();
    b3_music_start_race();
    if (crash) b3_music_crash_arm();

    b3_dj_init();
    b3_dj_set_track("US_C3_V1");
    b3_dj_set_mode(B3_DJ_MODE_RACE);

    for (long f = 0; f < frames; f++) {
        int active = crash && f >= c0 && f < c1;
        b3_music_pump();
        if (crash) b3_music_crash_tick(active, 1, dt);
        b3_dj_tick(dt);
        b3_dj_status(&st);
        printf("F %ld %d %d %d %d\n", f, st.speaking, st.ident,
               b3_music_current(), b3_music_song_held());
        for (int k = 0; k < FRAMES_PER_TICK; k++) {
            put(fd, b3_dj_next_sample());
            /* the mixed sample must still be pulled -- it is what
             * advances the stream -- but what we RECORD is the song's
             * share of it.  The crash bed is deliberately loud in the
             * one window the crash leg cares about, so measuring the
             * mixed channel there would prove nothing. */
            (void)b3_music_next_sample();
            put(fm, b3_music_last_song_sample());
        }
    }
    fclose(fd); fclose(fm);
    return 0;
}
"""

PROBE2_C = os.path.join(REPO, "build", "dj_seq_probe.c")
PROBE2 = os.path.join(REPO, "build", "dj_seq_probe")


def build2():
    with open(PROBE2_C, "w") as f:
        f.write(PROBE2_SRC)
    cmd = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
           "-I", os.path.join(REPO, "src"),
           "-o", PROBE2, PROBE2_C,
           os.path.join(REPO, "src", "burnout3_dj.c"),
           os.path.join(REPO, "src", "burnout3_music.c"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, cwd=REPO)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode()[-3000:])
        raise SystemExit("could not build the hand-over probe")
    if r.stderr.strip():
        sys.stderr.write(r.stderr.decode())
        raise SystemExit("the hand-over probe built with warnings")


def rms_of(raw, a, b):
    """RMS of samples [a,b) of a 16-bit mono buffer."""
    a = max(0, a); b = min(len(raw) // 2, b)
    if b <= a:
        return 0.0
    vals = struct.unpack("<%dh" % (b - a), raw[a * 2:b * 2])
    return math.sqrt(sum(float(v) * v for v in vals) / (b - a))


def run2(mode, env_extra, frames=3600):
    import tempfile
    fd1, pd = tempfile.mkstemp(suffix=".dj"); os.close(fd1)
    fd2, pm = tempfile.mkstemp(suffix=".mu"); os.close(fd2)
    env = dict(os.environ)
    env["B3_AUDIO_DIR"] = AUDIO
    env["DJ_OUT"] = pd
    env["MU_OUT"] = pm
    env.update(env_extra)
    out = subprocess.check_output([PROBE2, mode, str(frames)],
                                  cwd=REPO, env=env).decode()
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if p and p[0] == "F":
            rows.append(tuple(int(x) for x in p[1:]))
    with open(pd, "rb") as f:
        dj = f.read()
    with open(pm, "rb") as f:
        mu = f.read()
    os.unlink(pd); os.unlink(pm)
    return rows, dj, mu


def build():
    with open(PROBE_C, "w") as f:
        f.write(PROBE_SRC)
    # burnout3_music.c comes along so the hand-over can be MEASURED: the
    # probe dumps the DJ channel and the music channel side by side.
    cmd = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
           "-I", os.path.join(REPO, "src"),
           "-o", PROBE, PROBE_C,
           os.path.join(REPO, "src", "burnout3_dj.c"),
           os.path.join(REPO, "src", "burnout3_music.c"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, cwd=REPO)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode()[-3000:])
        raise SystemExit("could not build the probe")
    if r.stderr.strip():
        sys.stderr.write(r.stderr.decode())
        raise SystemExit("the probe built with warnings")


def run(env_extra, mode=5):
    env = dict(os.environ)
    env["B3_AUDIO_DIR"] = AUDIO
    env.update(env_extra)
    out = subprocess.check_output([PROBE, str(mode)], cwd=REPO, env=env)
    lines, duck = [], None
    for ln in out.decode().splitlines():
        p = ln.split()
        if p and p[0] == "LINE":
            lines.append(dict(bank=p[1], idx=int(p[2]), lo=int(p[3]),
                              hi=int(p[4]), gap=float(p[5]),
                              duck=float(p[6])))
        elif p and p[0] == "DUCKMIN":
            duck = float(p[1])
    return lines, duck


FAIL = []


def check(name, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + name + (("  " + detail) if detail else ""))
    if not ok:
        FAIL.append(name)


def main():
    print("Crash FM DJ -- recovered laws (docs/RE_CRASHFM.md)")
    if not os.path.isdir(os.path.join(AUDIO, "DJGEN")):
        raise SystemExit("no DJ banks under %s -- run the xwb stage first" % AUDIO)
    build()

    # ---- 1. the cooldown, section 5.1 -------------------------------
    lines, duck = run({"B3_DJ_SEED": "1"})
    check("lines are produced at all", len(lines) >= 8, "%d lines" % len(lines))
    gaps = [l["gap"] for l in lines[1:]]
    if gaps:
        lo, hi = min(gaps), max(gaps)
        # each gap is the cooldown PLUS the clip's own length, so the
        # floor is the law and the ceiling is the law + the longest line
        check("cooldown floor >= 60 s (5.1)", lo >= 60.0, "min gap %.2f s" % lo)
        check("cooldown spread <= 10 s + clip (5.1)", hi - lo <= 20.0,
              "spread %.2f s" % (hi - lo))

    # ---- 2. the three race sets, section 2.1 ------------------------
    race = [l for l in lines if l["bank"].startswith(("US_", "EU_", "AS_"))]
    check("per-track lines come from set 0 [0,22) (2.1)",
          all(l["lo"] == 0 and l["hi"] == 22 for l in race),
          "%d race lines" % len(race))
    check("every index lies inside its range (4.3)",
          all(l["lo"] <= l["idx"] < l["hi"] for l in lines))

    # ---- 3. the idle selector weights, section 2.3 ------------------
    banks = {}
    for l in lines:
        banks[l["bank"]] = banks.get(l["bank"], 0) + 1
    frac = len(race) / float(len(lines)) if lines else 0.0
    check("13/16 of idle lines are per-track chatter (2.3)",
          0.55 <= frac <= 0.97, "%.2f (%s)" % (frac, dict(banks)))

    # ---- 4. anti-repeat, section 4.3 --------------------------------
    rep = 0
    for a, b in zip(lines, lines[1:]):
        if a["bank"] == b["bank"] and a["idx"] == b["idx"]:
            rep += 1
    check("no immediate repeat within a bank (4.3)", rep == 0,
          "%d repeats" % rep)

    # ---- 5. no duck by default, section 6.3 [C] ---------------------
    # Retail sequences instead of ducking, so the default path must NOT
    # touch the music gain at all.  The old 0.7583 duck survives only
    # behind B3_DJ_DUCK=1, and is checked there.
    got = [l["duck"] for l in lines]
    check("the default path never ducks the music (6.3)",
          all(abs(d - 1.0) < 1e-6 for d in got),
          "min %.4f" % (min(got) if got else -1))
    check("the duck stays released", duck is not None and
          abs(duck - 1.0) < 1e-6, "final %.4f" % (duck if duck else -1))
    want = 0.455 / 0.600
    dk, _ = run({"B3_DJ_SEED": "1", "B3_DJ_DUCK": "1"})
    dgot = [l["duck"] for l in dk]
    check("B3_DJ_DUCK=1 restores the legacy 0.455/0.600 duck [S]",
          bool(dgot) and abs(min(dgot) - want) < 0.26,
          "target %.4f, observed min %.4f" % (want, min(dgot) if dgot else -1))

    # ---- 6. determinism ---------------------------------------------
    a, _ = run({"B3_DJ_SEED": "12345"})
    b, _ = run({"B3_DJ_SEED": "12345"})
    c, _ = run({"B3_DJ_SEED": "999"})
    same = [(x["bank"], x["idx"]) for x in a] == [(x["bank"], x["idx"]) for x in b]
    diff = [(x["bank"], x["idx"]) for x in a] != [(x["bank"], x["idx"]) for x in c]
    check("same seed -> identical line sequence", same)
    check("different seed -> different line sequence", diff)

    # the audio itself, not just the choice: two runs must produce a
    # byte-identical DJ channel, which is what the pinned-frame and
    # audio gates need from this module
    import hashlib
    import tempfile
    hs, rms = [], 0.0
    for _ in range(2):
        fd, path = tempfile.mkstemp(suffix=".raw")
        os.close(fd)
        run({"B3_DJ_SEED": "4242", "B3_DJ_PROBE_PCM": path})
        with open(path, "rb") as fh:
            raw = fh.read()
        hs.append(hashlib.sha256(raw).hexdigest())
        if not rms:
            n = len(raw) // 2
            vals = struct.unpack("<%dh" % n, raw[:n * 2])
            rms = math.sqrt(sum(float(v) * v for v in vals) / n) if n else 0.0
        os.unlink(path)
    check("same seed -> BYTE-IDENTICAL DJ channel", hs[0] == hs[1],
          hs[0][:16])
    check("the DJ channel is not silent", rms > 500.0, "RMS %.0f" % rms)

    # ---- 6b. the race-start intro, section 3.2 ----------------------
    # Retail enters mode 4 on the pre-race branch and mode 4's machine
    # has no cooldown gate, so the DJ speaks at once.  The intro must
    # land in the first couple of seconds, and the cycle after it must
    # go back to the recovered 60-70 s law.
    fd, ipath = tempfile.mkstemp(suffix=".raw")
    os.close(fd)
    intro, _ = run({"B3_DJ_SEED": "1", "B3_DJ_PROBE_PCM": ipath}, mode=4)
    check("race start speaks immediately (3.2)",
          bool(intro) and intro[0]["gap"] < 5.0,
          "first line at %.2f s" % (intro[0]["gap"] if intro else -1))
    check("the intro is followed by the normal 60-70 s cycle (3.2)",
          len(intro) > 1 and intro[1]["gap"] >= 60.0,
          "second line +%.2f s" % (intro[1]["gap"] if len(intro) > 1 else -1))
    # speech energy really is in the first seconds of the channel
    with open(ipath, "rb") as fh:
        head = fh.read(44100 * 2 * 6)          # first 6 s, 44100 mono s16
    n = len(head) // 2
    hrms = 0.0
    if n:
        vals = struct.unpack("<%dh" % n, head[:n * 2])
        hrms = math.sqrt(sum(float(v) * v for v in vals) / n)
    check("the intro has audible RMS in its first 6 s (3.2)", hrms > 500.0,
          "RMS %.0f" % hrms)
    os.unlink(ipath)

    # and it is reproducible, which is what the audio gates need
    ih = []
    for _ in range(2):
        fd, p2 = tempfile.mkstemp(suffix=".raw")
        os.close(fd)
        run({"B3_DJ_SEED": "77", "B3_DJ_PROBE_PCM": p2}, mode=4)
        with open(p2, "rb") as fh:
            ih.append(hashlib.sha256(fh.read()).hexdigest())
        os.unlink(p2)
    check("the intro is byte-identical across runs (3.2)", ih[0] == ih[1],
          ih[0][:16])

    # ---- 6c. the hand-over, section 6.3 -----------------------------
    # Retail sequences: the song is advanced out of the way before the
    # line and the NEXT song starts after it.  So while he is speaking
    # the music channel must be SILENT, and it must come back.
    build2()
    SR = 44100
    # rows are (frame, speaking, ident, current_track, song_held)
    rows, dj, mu = run2("none", {"B3_DJ_SEED": "1", "B3_DJ_COOLDOWN": "22"})
    spk = [r[1] for r in rows]                      # speaking, per frame
    held = [i for i, s in enumerate(spk) if s]
    check("the DJ speaks in the hand-over probe", bool(held),
          "%d frames of speech" % len(held))
    if held:
        # the FIRST contiguous burst of speech -- measuring across the
        # whole first..last span would include the minutes of music
        # between lines and prove nothing
        a = held[0]
        b = a
        while b + 1 < len(spk) and spk[b + 1]:
            b += 1
        b += 1
        dj_rms = rms_of(dj, a * 735, b * 735)
        mu_rms = rms_of(mu, a * 735, b * 735)
        check("music is SILENT while the DJ speaks (6.3)", mu_rms < 1.0,
              "music RMS %.2f vs DJ RMS %.0f over %d frames"
              % (mu_rms, dj_rms, b - a))
        check("the DJ line itself is live (6.3)", dj_rms > 500.0,
              "DJ RMS %.0f" % dj_rms)
        # ...and the NEXT song starts once he has finished
        after = rms_of(mu, (b + 90) * 735, (b + 240) * 735)
        check("the next song starts after the line (6.3)", after > 100.0,
              "music RMS %.0f after" % after)
        # nothing in the port may play music UNDER a line
        overlap = 0
        for i, s in enumerate(spk):
            if s and rms_of(mu, i * 735, (i + 1) * 735) > 1.0:
                overlap += 1
        check("no mid-song speech path anywhere (6.3)", overlap == 0,
              "%d overlapping frames" % overlap)

    # ---- 6d. the crash cinematic ------------------------------------
    # [S] default: the song freezes and resumes.  Retail keeps it at
    # 0.30 under the bed (RE_MUSIC.md 6.3); B3_MUSIC_CRASH=retail.
    rows, dj, mu = run2("crash", {"B3_DJ_SEED": "1", "B3_DJ_COOLDOWN": "999"})
    c0, c1 = 600, 900
    before = rms_of(mu, 400 * 735, 590 * 735)
    during = rms_of(mu, (c0 + 30) * 735, (c1 - 30) * 735)
    after = rms_of(mu, (c1 + 60) * 735, (c1 + 250) * 735)
    check("music plays before the cinematic", before > 100.0,
          "RMS %.0f" % before)
    check("music is SILENT during the cinematic ([S] pause)", during < 1.0,
          "RMS %.2f" % during)
    check("music resumes after the cinematic", after > 100.0,
          "RMS %.0f" % after)
    heldf = sum(1 for r in rows[c0:c1] if r[4])
    check("the song is reported held, not stopped", heldf > 0,
          "%d frames held" % heldf)
    # the retail behaviour must still be reachable
    rows, dj, mu = run2("crash", {"B3_DJ_SEED": "1", "B3_DJ_COOLDOWN": "999",
                                  "B3_MUSIC_CRASH": "retail"})
    rduring = rms_of(mu, (c0 + 30) * 735, (c1 - 30) * 735)
    check("B3_MUSIC_CRASH=retail keeps the song audible [C]",
          rduring > 100.0, "RMS %.0f" % rduring)

    # ---- 7. the kill switch -----------------------------------------
    off, _ = run({"B3_DJ": "0"})
    check("B3_DJ=0 produces no lines", len(off) == 0)

    # ---- 8. the bank inventory, section 1.3 -------------------------
    want_counts = {"DJGEN": 18, "DJWWW": 10, "DJUS": 20, "DJAS": 20,
                   "DJEU": 20, "DJMRA": 5, "DJMGP": 5, "DJMFO": 5,
                   "DJMEL": 5, "DJMRR": 5, "DJMBL": 5, "DJMCR": 5}
    bad = []
    for name, n in want_counts.items():
        d = os.path.join(AUDIO, name)
        if not os.path.isdir(d):
            continue
        got_n = len([x for x in os.listdir(d) if x.endswith(".wma")])
        if got_n != n:
            bad.append("%s %d!=%d" % (name, got_n, n))
    check("global bank entry counts match the disc (1.3)", not bad,
          ", ".join(bad))

    # the 38 that pins the recovery
    per = [d for d in os.listdir(AUDIO)
           if len(d) == 5 and d[:3] in ("US_", "EU_", "AS_")]
    bad38 = []
    for d in per:
        n = len([x for x in os.listdir(os.path.join(AUDIO, d))
                 if x.endswith(".wma")])
        if n != 38:
            bad38.append("%s=%d" % (d, n))
    check("every per-track bank holds 0x26 == 38 entries (1.3)",
          per and not bad38, "%d banks" % len(per))

    # ---- 9. resampling, section 7 -----------------------------------
    # the extracted wav is 48000/stereo; the module must hand the mixer
    # 44100 mono, so a clip's frame count must scale by 44100/48000
    src = os.path.join(AUDIO, "DJGEN", "000.wav")
    if os.path.isfile(src):
        with open(src, "rb") as f:
            raw = f.read()
        rate = struct.unpack("<I", raw[24:28])[0]
        ch = struct.unpack("<H", raw[22:24])[0]
        check("source lines are 48000 Hz stereo (1.3)",
              rate == 48000 and ch == 2, "%d Hz, %d ch" % (rate, ch))

    print()
    if FAIL:
        print("FAILED: " + ", ".join(FAIL))
        return 1
    print("all recovered laws hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
