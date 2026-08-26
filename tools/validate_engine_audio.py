#!/usr/bin/env python3
"""validate_engine_audio.py -- does the engine voice actually FOLLOW the rpm?

"0 engine loops" was printed by this harness for a long time, on the desktop
as well as on the web, and nothing in the suite would have noticed if it had
gone on printing it.  This is the gate that would have.

WHAT IT PROVES, and why in this shape.  The mixer plays the car's own bank at
a shifted playback rate:

    g_eng_phase += (loop.rate / 44100) * (rpm / loop.rpm) * timescale

(src/burnout3_full.c, audio_callback).  So rpm moves the voice's PITCH, and
loudness is very nearly flat with it -- which means an RMS-vs-rpm plot is the
WEAK test and a pitch-vs-rpm one is the strong test.  Both are reported: RMS
says there is a voice at all, the ZERO-CROSSING RATE says it is the right one
and that it is tracking.  A zero-crossing rate is a crude spectral centroid,
which is exactly what is wanted here: it needs no FFT, it is linear in
playback rate, and it does not care about amplitude.

HOW THE TWO CLOCKS ARE LINED UP, which is the only fiddly part.  SDL's `disk`
driver paces itself at each buffer's real duration, so the PCM's clock is WALL
time.  tools/audio_capture.sh therefore runs WITHOUT B3_FIXED_DT, so the sim's
dt is the frame's real dt and `race_time` in the drive log is wall seconds too.
(With a fixed dt the sim runs about 3x faster than the wall offscreen and the
two files are simply on different clocks -- measured: 26.4 s of drive log
against 7.9 s of PCM.)

One unknown is left: the audio device opens during the LOADING SCREEN, some
seconds before race_time 0, and the callback has been running ever since.  It
is recovered rather than guessed -- the capture is continuous from device open
to exit, so

    lead-in = (whole PCM duration) - (final race_time)

and race_time t sits at sample 44100 * (t + lead-in).  The lead-in is printed;
if it comes out negative or absurd the two files did not come from one run.

Usage:
    tools/audio_capture.sh /tmp/mix.raw /tmp/drive.txt 25
    tools/validate_engine_audio.py /tmp/mix.raw /tmp/drive.txt
"""
import math
import re
import struct
import sys

RATE = 44100
FRAME_RE = re.compile(r"^frame (\d+)\s+race_time ([\d.-]+)\s+sim_dt ([\d.-]+)\s+"
                      r"real_dt ([\d.-]+)")
PLAYER_RE = re.compile(r"^\s+speed ([\d.]+) mph\s+gear (-?\d+)\s+rpm (\d+)")


def read_drive(path):
    """[(race_time, rpm, gear, mph)] for the PLAYER, one per frame."""
    rows, pend = [], None
    want_player = False
    for line in open(path):
        m = FRAME_RE.match(line)
        if m:
            pend = float(m.group(2))
            want_player = False
            continue
        if line.startswith("car 0 (PLAYER)"):
            want_player = True
            continue
        if want_player:
            m = PLAYER_RE.match(line)
            if m and pend is not None:
                rows.append((pend, int(m.group(3)), int(m.group(2)),
                             float(m.group(1))))
                want_player = False
    return rows


def read_pcm(path):
    d = open(path, "rb").read()
    n = len(d) // 2
    return struct.unpack("<%dh" % n, d[:n * 2])


def windows(pcm, win):
    """(rms, zcr) per window of `win` samples."""
    out = []
    for s in range(0, len(pcm) - win, win):
        acc, zc, prev = 0, 0, pcm[s]
        for i in range(s, s + win):
            v = pcm[i]
            acc += v * v
            if (v >= 0) != (prev >= 0):
                zc += 1
            prev = v
        out.append((math.sqrt(acc / win) / 32768.0, zc * RATE / (2.0 * win)))
    return out


def pearson(a, b):
    n = len(a)
    if n < 3:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((x - mb) ** 2 for x in b)
    if va <= 0 or vb <= 0:
        return 0.0
    cov = sum((a[i] - ma) * (b[i] - mb) for i in range(n))
    return cov / math.sqrt(va * vb)


def main():
    raw = sys.argv[1] if len(sys.argv) > 1 else "/tmp/b3_mix.raw"
    log = sys.argv[2] if len(sys.argv) > 2 else "/tmp/b3_drive.txt"
    pcm = read_pcm(raw)
    drive = read_drive(log)
    if not pcm or not drive:
        sys.exit("no PCM (%d samples) or no drive rows (%d)"
                 % (len(pcm), len(drive)))

    win = 2048                       # 46.4 ms -- two mixer callbacks
    w = windows(pcm, win)
    dur = len(pcm) / float(RATE)
    lead = dur - drive[-1][0]        # device open -> race_time 0
    print("capture: %.2f s of 44100 Hz mono PCM (%d samples); drive log %d "
          "frames, race_time %.2f -> %.2f s"
          % (dur, len(pcm), len(drive), drive[0][0], drive[-1][0]))
    print("lead-in: %.2f s of audio before race_time 0 (the loading screen)"
          % lead)
    print("windows: %d x %d samples (%.1f ms)" % (len(w), win,
                                                  win * 1000.0 / RATE))
    if lead < -0.5:
        sys.exit("the PCM is SHORTER than the race -- these two files are not "
                 "from one run, or the capture was truncated")

    # pair each window with the drive row nearest its centre, in audio time
    pairs, di = [], 0
    for k, (rms, zcr) in enumerate(w):
        t = (k * win + win / 2.0) / RATE - lead      # -> race_time
        if t < drive[0][0]:
            continue                                 # still on the loading screen
        while di + 1 < len(drive) and drive[di + 1][0] <= t:
            di += 1
        if abs(drive[di][0] - t) > 0.5:
            continue
        pairs.append((drive[di][1], drive[di][2], rms, zcr))
    if not pairs:
        sys.exit("no overlap between the audio and the drive log")

    rpm = [p[0] for p in pairs]
    rms = [p[2] for p in pairs]
    zcr = [p[3] for p in pairs]
    nonzero = sum(1 for v in rms if v > 0.0)
    gears = sorted(set(p[1] for p in pairs))

    print("rpm    %d .. %d over %d paired windows, gears %s"
          % (min(rpm), max(rpm), len(pairs), gears))
    print("rms    min %.4f  mean %.4f  max %.4f   (%d/%d windows non-silent)"
          % (min(rms), sum(rms) / len(rms), max(rms), nonzero, len(rms)))
    print("zcr    min %.0f Hz  mean %.0f Hz  max %.0f Hz"
          % (min(zcr), sum(zcr) / len(zcr), max(zcr)))
    print("pearson r(rpm, rms) = %+.3f    r(rpm, zcr) = %+.3f"
          % (pearson(rpm, rms), pearson(rpm, zcr)))

    # the same thing as a table, which is what a human reads
    lo, hi = min(rpm), max(rpm)
    nb = 6
    print("\n  rpm band          windows   mean rms   mean zcr")
    for b in range(nb):
        a = lo + (hi - lo) * b / nb
        z = lo + (hi - lo) * (b + 1) / nb
        sel = [p for p in pairs if a <= p[0] <= z]
        if not sel:
            continue
        print("  %5.0f - %5.0f rpm  %7d   %8.4f   %7.0f Hz"
              % (a, z, len(sel),
                 sum(p[2] for p in sel) / len(sel),
                 sum(p[3] for p in sel) / len(sel)))

    ok = [
        ("the mix is not silent", nonzero > 0.9 * len(rms)),
        ("the engine voice tracks rpm (pitch)", pearson(rpm, zcr) > 0.5),
        ("more than one gear was used", len(gears) > 1),
    ]
    print()
    bad = 0
    for name, passed in ok:
        print("  %-38s %s" % (name, "PASS" if passed else "FAIL"))
        bad += 0 if passed else 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
