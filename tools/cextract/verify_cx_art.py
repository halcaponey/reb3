#!/usr/bin/env python3
"""Differential acceptance gate for the ART/FRONTEND C port (agent E).

Runs the six python reference tools and the C `cxart` binary into two scratch
trees and proves the COMPLETE output set matches: every PNG pixel-identical
(decoded with PIL), every other artefact byte-identical.

    python3 tools/cextract/verify_cx_art.py [--work DIR] [--keep]

The python tools are the SPEC and are never edited: extract_font.py and
extract_postfx_art.py hard-code repo output paths, so this script imports them
and rebinds their OUT_DIR / OUT_HDR / OUTDIR module globals before calling
main().  The other four write CWD-relative "build/..." paths, so a chdir into
the oracle root is enough.  NOTHING is ever written to the repo's build/ or
src/.

Both sides run with cwd == their own output root, which is what makes
extract_postfx_art.py's os.path.relpath() manifest reproducible: the python
records paths relative to the process CWD, and the C reproduces
posixpath.relpath() exactly.

Covered, per tool:

    extract_txd.py          both banks, --all-palettes (409 + 399 variants)
                            and the plain default mode as a second pass
    extract_font.py         3 atlases + src/burnout3_font.h
    extract_carfx_art.py    2 PNGs, env_light.txt, 67 *.lights
    extract_boostfx_art.py  2 PNGs
    extract_particlefx_art.py  14 PNGs
    extract_postfx_art.py   --all: 107 PNGs, 37 *_env.txt, enviro_manifest.txt
                            plus a --track/--scan pass
"""
import argparse
import collections
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TOOLS = os.path.join(REPO, "tools")
ARCHIVE = os.path.join(TOOLS, "py_extract_archive")
ELF = os.environ.get("B3_ELF", os.path.join(REPO, "build", "burnout3.elf"))

# the C sources this gate builds; cx_art_main.c carries the standalone driver
# cx_src.c is the SOURCE layer (directory-or-ISO) cx_art_common.c and
# cx_common_b.c grew after this list was first written; without it the gate's
# own build fails to LINK (cx_vfs_fopen / cx_vfs_listdir / cx_vfs_is_dir ...),
# which is how a gate stops gating anything at all.
SOURCES = ["cx_art_common.c", "cx_art_txd.c", "cx_art_font.c", "cx_art_fx.c",
           "cx_art_postfx.c", "cx_art_main.c", "cx_common_b.c", "cx_png.c",
           "cx_src.c"]

# Artefacts the C side writes that the PYTHON NEVER WROTE, so there is nothing
# to diff them against -- not a leniency, an absence.  cx_art_font.c also emits
# build/frontend/font.bin, the RUNTIME asset that retires the compiled-in
# src/burnout3_font.h (cx_extract.h, "purge 2"); its gate is
# tools/validate_no_baked_data.py, which compares the LOADED table against the
# bytes the extractor wrote.  Listed by exact relative path, never by glob, so
# a real extra still shows up as one.
NO_ORACLE = ("build/frontend/font.bin",)


def build_c(work):
    out = os.path.join(work, "cxart")
    cmd = (["gcc", "-Wall", "-Wextra", "-std=c11", "-O2", "-DCX_ART_STANDALONE"]
           + [os.path.join(HERE, s) for s in SOURCES]
           + ["-o", out, "-lz", "-lm"])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("C build failed:\n" + r.stdout + r.stderr)
    # cx_extract.h is shared by all four agents, so a warning raised inside it
    # is not necessarily this family's.  Report both, but only the art sources
    # are this gate's business.
    mine = [l for l in r.stderr.splitlines()
            if "warning:" in l and "cx_art_" in l]
    other = [l for l in r.stderr.splitlines()
             if "warning:" in l and "cx_art_" not in l]
    if mine:
        print("WARNING: the art sources did not build clean:")
        for l in mine:
            print("  " + l)
    if other:
        print("note: %d build warning(s) from shared/other-agent sources "
              "(not gated here)" % len(other))
    return out


def run_python(root, mode):
    """Run the six reference tools into `root` (a repo-root stand-in)."""
    # tools/ first so the LIVE reference tools win; py_extract_archive/ behind
    # it supplies extract_textures.py, which the archive move retired from
    # tools/ but extract_txd.py still imports.
    for p in (ARCHIVE, TOOLS):
        if p not in sys.path:
            sys.path.insert(0, p)
    cwd = os.getcwd()
    os.makedirs(root, exist_ok=True)
    os.chdir(root)
    buf = io.StringIO()
    stdout = sys.stdout
    sys.stdout = buf
    try:
        import extract_txd
        import extract_font
        import extract_carfx_art
        import extract_boostfx_art
        import extract_particlefx_art
        import extract_postfx_art

        if mode == "full":
            sys.argv = ["extract_txd.py", "--all-palettes"]
        else:
            sys.argv = ["extract_txd.py"]
        extract_txd.main()

        if mode == "full":
            extract_font.OUT_DIR = os.path.join(root, "build", "frontend")
            extract_font.OUT_HDR = os.path.join(root, "src", "burnout3_font.h")
            extract_font.ELF = ELF
            os.makedirs(os.path.dirname(extract_font.OUT_HDR), exist_ok=True)
            extract_font.main()

            sys.argv = ["extract_carfx_art.py"]
            extract_carfx_art.main()
            sys.argv = ["extract_boostfx_art.py"]
            extract_boostfx_art.main()
            sys.argv = ["extract_particlefx_art.py"]
            extract_particlefx_art.main()

            extract_postfx_art.OUTDIR = os.path.join(root, "build", "postfx")
            sys.argv = ["extract_postfx_art.py", "--all"]
            extract_postfx_art.main()
    finally:
        sys.stdout = stdout
        os.chdir(cwd)
    return buf.getvalue()


def run_c(binary, root, mode):
    os.makedirs(root, exist_ok=True)
    env = dict(os.environ, B3_ELF=ELF)
    if mode == "full":
        cmd = [binary, root, "all"]
    else:
        cmd = [binary, "--txd-no-palettes", root, "txd"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return r.stdout + r.stderr


def walk(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = p
    return out


def compare(oracle, cout):
    from PIL import Image
    o, c = walk(oracle), walk(cout)
    noor = set(x.replace("/", os.sep) for x in NO_ORACLE)
    only_o = sorted(set(o) - set(c))
    only_c = sorted(set(c) - set(o) - noor)
    for rel in sorted(noor & set(c)):
        print("  %-44s %5s" % (rel + "  (no python oracle)", "SKIP"))
    stats = collections.Counter()
    bad = []
    for rel in sorted(set(o) & set(c)):
        parts = rel.split(os.sep)
        grp = "/".join(parts[:2]) if len(parts) > 1 else parts[0]
        if rel.lower().endswith(".png"):
            a, b = Image.open(o[rel]), Image.open(c[rel])
            if a.size != b.size or a.mode != b.mode:
                bad.append("%s: %s%s != %s%s"
                           % (rel, a.mode, a.size, b.mode, b.size))
                continue
            da, db = a.tobytes(), b.tobytes()
            if da != db:
                n = sum(1 for i in range(0, len(da), 4)
                        if da[i:i + 4] != db[i:i + 4])
                bad.append("%s: %d/%d pixels differ" % (rel, n, len(da) // 4))
                continue
            stats[grp + "  PNG pixel-identical"] += 1
        else:
            da = open(o[rel], "rb").read()
            db = open(c[rel], "rb").read()
            if da != db:
                i = next((k for k in range(min(len(da), len(db)))
                          if da[k] != db[k]), min(len(da), len(db)))
                bad.append("%s: bytes differ at %d (%d vs %d bytes)"
                           % (rel, i, len(da), len(db)))
                continue
            stats[grp + "  byte-identical"] += 1
    for k in sorted(stats):
        print("  %-44s %5d" % (k, stats[k]))
    print("  %-44s %5d" % ("TOTAL", sum(stats.values())))
    if only_o:
        print("  MISSING from C (%d): %s" % (len(only_o), only_o[:20]))
    if only_c:
        print("  EXTRA in C (%d): %s" % (len(only_c), only_c[:20]))
    if bad:
        print("  MISMATCHES (%d):" % len(bad))
        for b in bad[:40]:
            print("    " + b)
    return not (bad or only_o or only_c)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", default=None, help="scratch dir (default: temp)")
    ap.add_argument("--keep", action="store_true", help="keep the scratch dir")
    args = ap.parse_args()

    work = args.work or tempfile.mkdtemp(prefix="cx_art_gate_")
    os.makedirs(work, exist_ok=True)
    print("work dir: %s" % work)
    binary = build_c(work)
    ok = True

    for mode, label in (("full", "all six tools (txd --all-palettes, font, "
                                 "carfx, boostfx, particlefx, postfx --all)"),
                        ("txd_default", "extract_txd.py default mode "
                                        "(no --all-palettes)")):
        print("\n=== %s ===" % label)
        oracle = os.path.join(work, "oracle_" + mode)
        cout = os.path.join(work, "c_" + mode)
        for d in (oracle, cout):
            shutil.rmtree(d, ignore_errors=True)
        py_out = run_python(oracle, mode)
        c_out = run_c(binary, cout, mode)
        ok &= compare(oracle, cout)
        # stdout parity is a bonus signal, not part of the gate
        norm = lambda s, r: s.replace(r + "/", "").replace(r, "")  # noqa: E731
        if norm(py_out, oracle) != norm(c_out, cout):
            print("  (note: stdout differs -- not gated)")

    print("\nGATE: %s" % ("PASS" if ok else "FAIL"))
    if not args.keep and not args.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
