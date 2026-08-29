#!/usr/bin/env python3
"""THE ACCEPTANCE GATE for the C asset-extraction port.

The C pipeline in tools/cextract/ is a port of the per-track python extractors
in tools/, and the bar is not "looks right" -- it is BYTE-FOR-BYTE identical
output.  This tool diffs a C run against an oracle run of the python tools and
exits 0 only if every artefact on both sides matches.

    python3 tools/cextract/verify_cextract.py \\
        --track US_C3_V1 --c-out <dir> --oracle <dir>

  * binary assets (.bin, .mtl, .obj, and anything else not a PNG) must be
    byte-identical.  On a divergence the report gives the offset and a hexdump
    of both sides around it, so the failing FIELD is identifiable without
    re-running anything.
  * .png assets must be PIXEL-identical: same mode, same size, and the same
    bytes once converted to RGBA.  Encoder differences (filter choice, zlib
    level, ancillary chunks) are not failures; a single changed texel is.  The
    report names the first differing pixel and both RGBA values.
  * a file present on only one side is a FAIL, listed by name -- silence about
    a missing artefact is how a port ships half a pipeline.

Both --c-out and --oracle accept either the per-track directory itself or a
root above it (<dir>/<ID>, <dir>/tracks/<ID>, <dir>/build/tracks/<ID>).

--------------------------------------------------------------- GLOBAL MODE
The pipeline also has GLOBAL stages -- the car, art, audio and generator
families, which run once for the whole dump rather than once per track
(cxtract --all-global --out <dir>).  `--global` diffs one of those runs:

    python3 tools/cextract/verify_cextract.py \\
        --global --c-out <dir> --oracle <dir>

Identical rules -- PNG compared as pixels, everything else (.wav, .bin, .h,
.txt, ...) as bytes, a file on only one side a FAIL -- but the two roots are
compared AS GIVEN.  There is no per-track directory to find under them, and
probing for one would silently compare the wrong subtree.  `--track` mode is
untouched by this flag.
"""
import argparse
import filecmp
import fnmatch
import os
import sys

PNG_EXT = ".png"
# Extensions the pipeline emits that are compared as raw bytes.  Anything not
# a PNG falls in here too; the list is for the report's category column only.
BINARY_EXT = (".bin", ".mtl", ".obj", ".h", ".txt", ".wav")

# Artefacts the C pipeline writes that the PYTHON TOOLS NEVER WROTE.  There is
# no oracle for them because there is no original -- they are the RUNTIME
# ASSETS that retire the last compiled-in headers (cx_extract.h, "purge 2"),
# emitted by the same stages that generate those headers.  The headers
# themselves are still written and still diffed, so the numbers ARE covered;
# what is skipped is only the second encoding of them.  Their own gate is
# tools/validate_no_baked_data.py section 3, which compares the LOADED table
# against these bytes and against the game's own files.
#
# Exact relative paths, never globs: a genuinely unexpected extra still fails.
#
# `scenery.bin` is the second kind: a NEW RECOVERY.  static.dat's first
# 0x70-record table (hdr +0x34/+0x38 -- the world's palms, hero trees, lamp
# posts, signage, benches, boats and parked vehicles) was never read by any
# python tool; tools/py_extract_archive/extract_track.py:23 marks it
# "(not extracted, [S])".  The archive stays untouched as the oracle of the OLD
# behaviour, so there is nothing here to diff against, and the gate for this
# artefact is tools/validate_scenery.py -- which re-derives every record and
# every instance transform straight out of the shipped static.dat/streamed.dat
# rather than trusting the C stage.
#
# `bvh.bin` is a FOURTH kind, and the only one so far: a DERIVED artefact with
# neither a python original nor a retail counterpart.  cx_bvh.c builds a BVH
# over the static world out of what the TRACK, PROPS and SCENERY stages have
# already written, to serve the INSPIRED ray-traced sun shadow
# (docs/PHOTOREALISM.md tier 4r).  The Xbox had no acceleration structure of
# any kind, so there is nothing on the disc to diff it against either; its
# gate is tools/validate_bvh.py, which re-derives the geometry from the same
# three artefacts and re-traces a grid of rays through the flattened tree
# against a brute-force test over every triangle.
NO_ORACLE = (
    "build/cars/car_physics.bin",
    "build/cars/roster.bin",
    "build/frontend/font.bin",
    "scenery.bin",
    "bvh.bin",
)

# THE THIRD KIND: an artefact whose ORACLE IS NOT A BYTE STRING.
#
# The two music stages decode WMA.  The archived python decoded it by shelling
# out to `ffmpeg`, so its output was never reproducible in the way every other
# stage's is -- it was whatever the ffmpeg build on that machine produced.  The
# C stages now decode in process with Rockbox's fixed-point libwma
# (tools/cextract/wma/), which is a different implementation of the same
# standard: it agrees with ffmpeg to the 16-bit quantisation floor and NOT to
# the bit.  Diffing bytes here would fail for a correct decoder, which makes it
# a worse gate than none.
#
# So these are skipped HERE and gated THERE:
#
#     tools/validate_wma.py -- runs the real stage twice, once in process and
#     once with B3_FFMPEG=1 (the escape hatch the shipping code still carries),
#     and scores the 44 pairs.  Bar: SNR >= 40 dB, correlation >= 0.999,
#     duration within 100 ms, no clipping introduced.
#     Measured: SNR 58.4-79.9 dB (median 78.2), correlation 1.000000 on all 44.
#
# Globs, not exact paths, because the family is 44 songs plus a manifest plus
# 885 bank entries across 33 directories -- and unlike the entries above, the
# SET is not a fixed list this file should be asserting.
#
# ONLY THE DECODED AUDIO IS EXEMPT.  The .wma files the xwb stage writes are
# carved VERBATIM out of the bank's ENTRYWAVEDATA segment -- no decoder is
# involved -- so they are still byte-identical to the python's and are still
# compared here.  That is worth keeping: it is the check that proves the
# container parsing agrees, which is the half of these stages that a byte diff
# can still speak to.
NO_ORACLE_GLOBS = (
    "build/music/*",            # track_NN.wav + eatrax.txt   (eatrax stage)
    "*/[0-9][0-9][0-9].wav",    # <bankName>/NNN.wav          (xwb stage)
)

HEX_CONTEXT = 32          # bytes either side of the first divergence


# --------------------------------------------------------------- locating
def resolve_side(path, track):
    """Accept the per-track dir or any of the usual roots above it.

    MOST SPECIFIC FIRST: an oracle root also holds the extractor scripts, so
    testing the bare path first would compare tools/*.py against nothing and
    drown the report in phantom failures.
    """
    cands = [os.path.join(path, "build", "tracks", track),
             os.path.join(path, "tracks", track),
             os.path.join(path, track),
             path]
    for c in cands:
        if os.path.isdir(c) and any(fs for _, _, fs in os.walk(c) if fs):
            return os.path.normpath(c)
    return os.path.normpath(path)


def walk(root):
    """relative path -> absolute path, for every file under root."""
    out = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            ap = os.path.join(dirpath, fn)
            out[os.path.relpath(ap, root)] = ap
    return out


# ---------------------------------------------------------------- reports
def hexdump_context(a_path, b_path, off):
    """The bytes around the first divergence, both sides, marked."""
    lo = max(0, off - HEX_CONTEXT)
    hi = off + HEX_CONTEXT
    lines = []
    with open(a_path, "rb") as f:
        f.seek(lo)
        a = f.read(hi - lo)
    with open(b_path, "rb") as f:
        f.seek(lo)
        b = f.read(hi - lo)
    for label, buf in (("C     ", a), ("oracle", b)):
        for i in range(0, len(buf), 16):
            base = lo + i
            chunk = buf[i:i + 16]
            cells = []
            for k, byte in enumerate(chunk):
                mark = "*" if base + k == off else " "
                cells.append("%02x%s" % (byte, mark))
            txt = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            lines.append("      %s %08x  %-48s |%s|"
                         % (label, base, "".join(cells), txt))
        lines.append("      %s" % ("-" * 6))
    return lines


def cmp_bytes(a_path, b_path):
    """(ok, [detail lines])"""
    sa, sb = os.path.getsize(a_path), os.path.getsize(b_path)
    if filecmp.cmp(a_path, b_path, shallow=False):
        return True, []
    # locate the first difference (sizes may differ; report that too)
    off = None
    with open(a_path, "rb") as fa, open(b_path, "rb") as fb:
        pos = 0
        while True:
            ca, cb = fa.read(65536), fb.read(65536)
            if not ca and not cb:
                break
            n = min(len(ca), len(cb))
            for i in range(n):
                if ca[i] != cb[i]:
                    off = pos + i
                    break
            if off is not None:
                break
            if len(ca) != len(cb):
                off = pos + n
                break
            pos += n
    detail = ["      size C=%d oracle=%d" % (sa, sb)]
    if off is None:
        off = min(sa, sb)
    if off < min(sa, sb):
        detail.append("      first difference at offset 0x%X (%d)" % (off, off))
        detail += hexdump_context(a_path, b_path, off)
    else:
        detail.append("      identical up to 0x%X (%d); one side is truncated"
                      % (off, off))
    return False, detail


def cmp_png(a_path, b_path):
    """Pixel identity: mode + size + the RGBA-converted raw bytes."""
    try:
        from PIL import Image
    except ImportError:
        ok = filecmp.cmp(a_path, b_path, shallow=False)
        return ok, [] if ok else [
            "      PIL is not installed, so this fell back to a byte compare "
            "and the bytes differ; install Pillow for a pixel verdict"]
    with Image.open(a_path) as ia, Image.open(b_path) as ib:
        ia.load()
        ib.load()
        if ia.size != ib.size:
            return False, ["      size C=%dx%d oracle=%dx%d"
                           % (ia.size[0], ia.size[1], ib.size[0], ib.size[1])]
        detail = []
        if ia.mode != ib.mode:
            detail.append("      mode C=%s oracle=%s (comparing as RGBA)"
                          % (ia.mode, ib.mode))
        ra = ia.convert("RGBA").tobytes()
        rb = ib.convert("RGBA").tobytes()
        if ra == rb:
            # A mode difference with identical RGBA is still a divergence from
            # the spec's output, so say so, but it is not a pixel failure.
            return (not detail), detail
        w = ia.size[0]
        for i in range(0, min(len(ra), len(rb)), 4):
            if ra[i:i + 4] != rb[i:i + 4]:
                p = i // 4
                detail.append(
                    "      first differing pixel (%d, %d): C=%s oracle=%s"
                    % (p % w, p // w, tuple(ra[i:i + 4]), tuple(rb[i:i + 4])))
                break
        diff = sum(1 for i in range(0, min(len(ra), len(rb)), 4)
                   if ra[i:i + 4] != rb[i:i + 4])
        detail.append("      %d of %d pixels differ"
                      % (diff, ia.size[0] * ia.size[1]))
        return False, detail


# ------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(
        description="Byte/pixel-diff a C extraction run against the python "
                    "oracle.  Exits 0 only when every artefact matches.")
    ap.add_argument("--track", default=os.environ.get("B3_TRACK", "US_C3_V1"),
                    help="track id, for the report and for locating the "
                         "per-track directory under a root (default US_C3_V1)")
    ap.add_argument("--global", dest="is_global", action="store_true",
                    help="compare two GLOBAL output roots (cxtract "
                         "--all-global) as given, instead of hunting for a "
                         "per-track directory under them.  Same byte/pixel "
                         "rules; --track is ignored.")
    ap.add_argument("--c-out", required=True,
                    help="output tree of the C pipeline (cxtract --out)")
    ap.add_argument("--oracle", required=True,
                    help="output tree of the python extractors")
    ap.add_argument("--only-fail", action="store_true",
                    help="print only failures, not every PASS line")
    ap.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                    help="skip relative paths matching GLOB (repeatable). "
                         "For artefacts a module DELIBERATELY does not port, "
                         "e.g. --exclude 'cars/*' for the vehicle-asset half "
                         "of extract_traffic.py.  Opt-in only: the default is "
                         "strict, so an out-of-scope subtree has to be named "
                         "on the command line rather than quietly passing.")
    args = ap.parse_args()

    if args.is_global:
        c_root = os.path.normpath(args.c_out)
        o_root = os.path.normpath(args.oracle)
        print("mode   : global")
    else:
        c_root = resolve_side(args.c_out, args.track)
        o_root = resolve_side(args.oracle, args.track)
        print("track  : %s" % args.track)
    print("C      : %s" % c_root)
    print("oracle : %s" % o_root)
    print()

    c_files, o_files = walk(c_root), walk(o_root)
    names = sorted(set(c_files) | set(o_files))
    if not names:
        print("FAIL: neither tree contains any file")
        return 1

    npass = nfail = nskip = 0
    width = max(len(n) for n in names)
    for name in names:
        if any(fnmatch.fnmatch(name, g) for g in args.exclude):
            nskip += 1
            continue
        if name.replace(os.sep, "/") in NO_ORACLE and name in c_files \
                and name not in o_files:
            nskip += 1
            print("SKIP %-*s  %-6s no python oracle (runtime asset)"
                  % (width, name, "bytes"))
            continue
        # Decoded audio: the oracle is an SNR comparison, not a byte string.
        # Skipped whichever side it appears on -- unlike the NO_ORACLE entries
        # above, both trees legitimately contain these, and they legitimately
        # differ.  See NO_ORACLE_GLOBS for the rule and the gate.
        if any(fnmatch.fnmatch(name.replace(os.sep, "/"), g)
               for g in NO_ORACLE_GLOBS):
            nskip += 1
            print("SKIP %-*s  %-6s decoded audio: gated by SNR, not bytes "
                  "(tools/validate_wma.py)" % (width, name, "audio"))
            continue
        ext = os.path.splitext(name)[1].lower()
        kind = "png" if ext == PNG_EXT else (
            "bytes" if ext in BINARY_EXT else "bytes")
        if name not in c_files:
            print("FAIL %-*s  %-6s missing on the C side" % (width, name, kind))
            nfail += 1
            continue
        if name not in o_files:
            print("FAIL %-*s  %-6s missing on the oracle side"
                  % (width, name, kind))
            nfail += 1
            continue
        if ext == PNG_EXT:
            ok, detail = cmp_png(c_files[name], o_files[name])
        else:
            ok, detail = cmp_bytes(c_files[name], o_files[name])
        if ok:
            npass += 1
            if not args.only_fail:
                print("PASS %-*s  %-6s %d B"
                      % (width, name, kind, os.path.getsize(o_files[name])))
        else:
            nfail += 1
            print("FAIL %-*s  %-6s" % (width, name, kind))
            for line in detail:
                print(line)

    print()
    print("=" * 60)
    print("%d PASS, %d FAIL, %d compared%s"
          % (npass, nfail, npass + nfail,
             ", %d excluded by --exclude" % nskip if nskip else ""))
    if nfail:
        print("VERDICT: FAIL -- the C port is not byte-identical to the "
              "python spec")
    else:
        print("VERDICT: PASS -- every artefact is identical")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
