#!/usr/bin/env python3
"""
validate_wma.py -- the acceptance gate for the in-process WMA decoder.

=============================================================== WHY NOT A DIFF
Every other cextract stage is gated by BYTE IDENTITY against the archived
python in tools/py_extract_archive/.  The two music stages cannot be, and never
could have been: the python shelled out to `ffmpeg` for the WMA streams, so its
"oracle" output was only ever as reproducible as whichever ffmpeg build
happened to be on PATH.  There is no byte string to compare against.

So the rule for this family is different, and it is stated here rather than
assumed:

    THE ORACLE IS A FRESHLY GENERATED ffmpeg DECODE OF THE SAME PAYLOADS,
    AND THE GATE IS SIGNAL-TO-NOISE RATIO, NOT EQUALITY.

Rockbox's libwma is fixed-point and FFmpeg's wmadec is float; they are
different implementations of the same standard and agreeing to the last bit is
not a thing to expect or want.  What IS expected is that the difference sits at
the 16-bit quantisation floor, and it does -- see the numbers below.

The archive is NOT touched.  tools/py_extract_archive is immutable; this file
compares the C stage against ffmpeg directly, which is what the python was
doing too, one level down.

======================================================= WHAT IS ACTUALLY RUN
Both arms are the REAL STAGE, driven the way the game drives it -- not a
bespoke test harness that could differ from shipping behaviour:

    arm A (default)      cxtract --only eatrax          -> in-process libwma
    arm B (B3_FFMPEG=1)  cxtract --only eatrax          -> the old subprocess

then the 44 track_NN.wav pairs are scored.  Because arm B is the escape hatch
the shipping code still carries, this also proves the escape hatch works.

=================================================================== THE BAR
    SNR         >= 40 dB per track   (measured: 58.4 - 79.9, median 78.2)
    correlation >= 0.999             (measured: 1.000000 on all 44)
    duration    within 100 ms        (measured: ours is 46.4 ms SHORT, always)
    clipping    no MORE clipped samples than the oracle has

THE 46.4 ms.  That is exactly one 2048-sample frame at 44100 Hz.  FFmpeg
flushes its decoder at end of stream and emits the final MDCT overlap; libwma
has no flush entry point and stops when the packets do.  It is the last frame
of a three-minute song, it is consistent across all 44 tracks, and it is
reported rather than hidden.

Usage:
    python3 tools/validate_wma.py                  # both arms, all 44
    python3 tools/validate_wma.py --only 0,1,2     # a subset, for iteration
    python3 tools/validate_wma.py --keep           # leave the wavs behind
"""
import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKS = 44

SNR_BAR = 40.0
CORR_BAR = 0.999
DUR_TOL_MS = 100.0


def die(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def load_wav(path):
    """-> (samples as a list of ints, rate, channels).  Any header length."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("%s is not 16-bit" % path)
        n = w.getnframes()
        raw = w.readframes(n)
        rate = w.getframerate()
        ch = w.getnchannels()
    import array
    a = array.array("h")
    a.frombytes(raw)
    if sys.byteorder == "big":
        a.byteswap()
    return a, rate, ch


def score(ref_path, test_path):
    """SNR (dB), correlation, and the length delta in ms."""
    a, ra, ca = load_wav(ref_path)
    b, rb, cb = load_wav(test_path)
    if ra != rb or ca != cb:
        return None, None, None, "format differs: %d/%dch vs %d/%dch" % (
            ra, ca, rb, cb)
    n = min(len(a), len(b))
    if n == 0:
        return None, None, None, "empty"
    dms = (len(b) - len(a)) / float(ca) / rb * 1000.0
    # A track is ~9 M samples and there are 44 of them, so the scalar version
    # of this is minutes per track.  numpy when it is there, and a chunked
    # scalar fallback when it is not, so the gate never becomes un-runnable
    # just because a checkout lacks numpy.
    try:
        import numpy as np
    except ImportError:
        return _score_scalar(a, b, n) + (dms, None)
    x = np.frombuffer(a, dtype=np.int16)[:n].astype(np.float64)
    y = np.frombuffer(b, dtype=np.int16)[:n].astype(np.float64)
    d = x - y
    ep = float((d * d).sum())
    rp = float((x * x).sum())
    snr = float("inf") if ep == 0 else (10.0 * math.log10(rp / ep) if rp else 0.0)
    if x.std() == 0 or y.std() == 0:
        corr = 1.0 if ep == 0 else 0.0
    else:
        corr = float(np.corrcoef(x, y)[0, 1])
    return snr, corr, dms, None


def _score_scalar(a, b, n):
    """The no-numpy path: same three numbers, chunked to stay honest on RAM."""
    sr = se = sa = sb = saa = sbb = sab = 0
    for i in range(n):
        x = a[i]
        y = b[i]
        d = x - y
        sr += x * x
        se += d * d
        sa += x
        sb += y
        saa += x * x
        sbb += y * y
        sab += x * y
    snr = float("inf") if se == 0 else (10.0 * math.log10(sr / se) if sr else 0.0)
    den = math.sqrt(max(n * saa - sa * sa, 0)) * math.sqrt(max(n * sbb - sb * sb, 0))
    corr = 1.0 if den == 0 else (n * sab - sa * sb) / den
    return snr, corr


def clipped(path):
    a, _, _ = load_wav(path)
    try:
        import numpy as np
        x = np.frombuffer(a, dtype=np.int16)
        return int(((x >= 32767) | (x <= -32768)).sum())
    except ImportError:
        return sum(1 for v in a if v >= 32767 or v <= -32768)


def run_stage(out_dir, iso, globalus, only, use_ffmpeg):
    env = dict(os.environ)
    env["B3_ISO"] = iso
    env["B3_GLOBALUS"] = globalus
    if only:
        env["B3_EATRAX_ONLY"] = only
    if use_ffmpeg:
        env["B3_FFMPEG"] = "1"
    else:
        env.pop("B3_FFMPEG", None)
    cx = os.path.join(out_dir, "..", "cxtract")
    r = subprocess.run([cx, "--all-global", "--only", "eatrax",
                        "--out", out_dir],
                       env=env, cwd=REPO, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        die("cxtract --only eatrax failed (%s arm)"
            % ("ffmpeg" if use_ffmpeg else "libwma"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="comma-separated track indices")
    ap.add_argument("--keep", action="store_true",
                    help="leave the decoded wavs in place")
    ap.add_argument("--iso", default=None)
    args = ap.parse_args()

    iso = args.iso
    if not iso:
        p = os.path.join(REPO, "build", "iso_path.txt")
        if os.path.exists(p):
            iso = open(p).read().strip()
    if not iso or not os.path.exists(iso):
        die("no disc image (build/iso_path.txt or --iso)")

    globalus = os.path.join(REPO, "build", "Globalus.bin")
    if not os.path.exists(globalus):
        die("build/Globalus.bin missing (boot once in iso mode, or "
            "cxtract it)")

    if not shutil.which("ffmpeg"):
        die("ffmpeg is not on PATH -- it is the ORACLE for this gate, so "
            "there is nothing to compare against")

    want = list(range(TRACKS))
    if args.only:
        want = [int(x) for x in args.only.split(",") if x.strip() != ""]

    print("[1] the disc")
    print("    %s" % iso)

    work = tempfile.mkdtemp(prefix="b3_wma_gate_")
    try:
        cx = os.path.join(work, "cxtract")
        print("[2] building cxtract")
        r = subprocess.run(["bash", os.path.join(REPO, "tools", "cextract",
                                                 "build.sh"), cx],
                           capture_output=True, text=True, cwd=REPO)
        if r.returncode != 0:
            print(r.stdout[-2000:], r.stderr[-2000:])
            die("cannot build cxtract")
        if "linking the WMA stub" in (r.stdout + r.stderr):
            die("this tree has no WMA decoder -- run `sh tools/fetch_wma.sh`")

        only = ",".join(str(i) for i in want) if args.only else None
        ours = os.path.join(work, "ours")
        orac = os.path.join(work, "oracle")
        os.makedirs(ours, exist_ok=True)
        os.makedirs(orac, exist_ok=True)

        print("[3] arm A: the in-process decoder")
        run_stage(ours, iso, globalus, only, use_ffmpeg=False)
        print("[4] arm B: the ffmpeg oracle (B3_FFMPEG=1)")
        run_stage(orac, iso, globalus, only, use_ffmpeg=True)

        print("[5] scoring")
        print("    %-4s %10s %11s %10s %10s" %
              ("#", "SNR dB", "corr", "dlen ms", "clip o/r"))
        bad = []
        snrs = []
        for i in want:
            name = "track_%02d.wav" % i
            pa = os.path.join(orac, name)
            pb = os.path.join(ours, name)
            if not os.path.exists(pa) or not os.path.exists(pb):
                bad.append("%d: missing output" % i)
                continue
            snr, corr, dms, err = score(pa, pb)
            if err:
                bad.append("%d: %s" % (i, err))
                continue
            co, cr = clipped(pb), clipped(pa)
            flag = ""
            if snr < SNR_BAR:
                flag += "  << SNR"
                bad.append("%d: SNR %.2f dB < %.1f" % (i, snr, SNR_BAR))
            if corr < CORR_BAR:
                flag += "  << CORR"
                bad.append("%d: correlation %.6f < %.3f" % (i, corr, CORR_BAR))
            if abs(dms) > DUR_TOL_MS:
                flag += "  << LEN"
                bad.append("%d: length differs by %.1f ms" % (i, dms))
            if co > cr:
                flag += "  << CLIP"
                bad.append("%d: %d clipped samples vs the oracle's %d"
                           % (i, co, cr))
            snrs.append(snr)
            print("    %-4d %10.2f %11.6f %10.1f %10s%s"
                  % (i, snr, corr, dms, "%d/%d" % (co, cr), flag))

        print()
        if snrs:
            snrs.sort()
            print("    %d tracks: SNR min %.2f  median %.2f  max %.2f dB"
                  % (len(snrs), snrs[0], snrs[len(snrs) // 2], snrs[-1]))
        if bad:
            for b in bad:
                print("    " + b)
            die("%d track(s) under the bar" % len(bad))
        print("PASS: %d/%d tracks within %.0f dB SNR of the ffmpeg oracle"
              % (len(snrs), len(want), SNR_BAR))
    finally:
        if args.keep:
            print("kept: %s" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
