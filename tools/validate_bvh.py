#!/usr/bin/env python3
"""
validate_bvh -- the gate for tools/cextract/cx_bvh.c and build/tracks/*/bvh.bin.

WHY THIS FILE EXISTS.  cx_bvh is the second stage in CX_STAGE_LIST with NO
PYTHON ORACLE, and unlike cx_scenery it has no RETAIL counterpart either: the
Xbox had no ray tracing, no acceleration structure and no shader budget for
one.  bvh.bin is a DERIVED artefact, built out of three earlier stages'
output, and it serves an INSPIRED renderer feature (docs/PHOTOREALISM.md tier
4r).  So verify_cextract.py's oracle diff has nothing to diff it against and
lists it under NO_ORACLE; this file is the gate instead.

It never trusts the stage.  Everything on the expected side is re-derived
here, from the same three artefacts the stage read, with an independent
implementation:

  1  DETERMINISM     the same inputs give the same bytes.  Run twice into two
                     scratch directories and diff.  (Skipped when the
                     standalone `cxtract` cannot be built.)

  2  THE HEADER      magic, version, the counts, and the offsets landing where
                     the record sizes say they must.  A file whose header does
                     not describe its own body is one the loader would read as
                     garbage without ever noticing.

  3  THE TREE        every node's box CONTAINS its children's boxes and its own
                     triangles; the depth-first layout holds (a node's left
                     child is index + 1); the ESCAPE indices are well formed
                     (strictly increasing, the root's is node_count, a leaf's
                     is its own index + 1) -- which is the whole of the
                     shader's stackless walk; and the leaves TILE the triangle
                     array exactly once, so every triangle is reachable and
                     none is reachable twice.

  4  THE PACKING     the contract with the shader: a leaf holds at most 8
                     triangles, and `first * 8 + (count - 1)` stays inside the
                     24 bits an ESSL 1.00 highp float can hold exactly.  ESSL
                     1.00 has no bitwise operators, so that arithmetic IS the
                     encoding, and a file that overflowed it would traverse
                     silently wrong.

  5  THE GEOMETRY    the triangle set is re-derived from track.obj, props.bin
                     and scenery.bin -- including the instance bake and the
                     material exclusions -- and compared as a multiset.  This
                     is the section that would catch a transposed instance
                     matrix, a dropped submesh or an off-by-one index.

  6  THE EXCLUSIONS  no DECAL and no BLENDED material reaches the tree, and
                     no triangle is degenerate.  Each has a reason in
                     cx_bvh.c's header and each is a visible artefact when it
                     is wrong.

  7  RAY PROBES      the load-bearing one.  A grid of rays is traced through
                     the flattened tree by a REFERENCE TRAVERSAL written here
                     (the same stackless escape walk the shader runs) and
                     against a BRUTE-FORCE test over every triangle, and the
                     two must agree on the hit distance to within a float's
                     worth.  A tree that is merely self-consistent can still
                     be a tree over the wrong boxes; this is what makes it a
                     tree over the right ones.

Usage:
    python3 tools/validate_bvh.py
    python3 tools/validate_bvh.py --tracks US_C3_V1,AS_M1_V1
Exit status: 0 all passed, 1 a failure, 2 bad usage.
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CX_C = os.path.join(ROOT, "tools", "cextract", "cx_bvh.c")
# THE BUILDER MOVED, and the checks over it moved with it.  carbvh.bin (the
# per-car model trees, tools/cextract/cx_car_bvh.c) is flattened by the same
# escape-index build this artefact is, and two files that have to agree about
# a leaf packing down to the bit cannot be kept agreeing by review -- so the
# binned-SAH build, the flatten and the node record are shared.  What stays in
# cx_bvh.c is what the two artefacts genuinely differ about: what geometry
# goes in and what the header says.
CX_BUILD_C = os.path.join(ROOT, "tools", "cextract", "cx_bvh_build.c")
CX_BUILD_H = os.path.join(ROOT, "tools", "cextract", "cx_bvh_build.h")


def _tracks_dir():
    """build/tracks, wherever this checkout keeps it.

    ISO MODE MATERIALISES INTO build/.isocache/tracks and the pre-extracted
    debug tree is build/tracks, and this used to look only at the second --
    so on an ISO-mode checkout, which is the default the game ships with, this
    whole gate printed "nothing to validate" and exited 0.  A gate that passes
    by finding nothing is worse than no gate."""
    for rel in (("build", "tracks"), ("build", ".isocache", "tracks")):
        d = os.path.join(ROOT, *rel)
        if os.path.isdir(d) and os.listdir(d):
            return d
    return os.path.join(ROOT, "build", "tracks")


TRACKS = _tracks_dir()

# Re-stated here rather than imported, and that duplication is the point: a
# validator that read its constants out of the thing under test would agree
# with it however wrong both were.
HDR = 0x50
NODE = 0x30
TRI = 0x30
MAGIC = b"B3BV"
VERSION = 1
LEAF_MAX = 8
ALPHA_REF = 64          # D3DRS_ALPHAREF, FUN_00038D10 @0x00038FEE   [C]

PASS, FAIL, SKIP = [], [], []


def ck(cond, what, detail=""):
    (PASS if cond else FAIL).append(what)
    print("  %s %s%s" % ("ok  " if cond else "FAIL", what,
                         ("   [%s]" % detail) if detail else ""))
    return bool(cond)


def skip(what, why):
    SKIP.append(what)
    print("  SKIP %s   [%s]" % (what, why))


# ======================================================================
# the artefact
# ======================================================================
class Bvh(object):
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        d = self.raw
        self.magic = d[:4]
        (self.version, self.nnode, self.ntri, self.off_nodes,
         self.off_tris) = struct.unpack_from("<IIIII", d, 4)
        self.wmin = np.array(struct.unpack_from("<3f", d, 0x18))
        self.wmax = np.array(struct.unpack_from("<3f", d, 0x24))
        (self.n_track, self.n_props, self.n_scen, self.max_depth,
         self.leaf_max, self.leaf_count, self.dropped,
         _r) = struct.unpack_from("<IIIIIIII", d, 0x30)

        n = self.nnode
        nb = np.frombuffer(d, dtype=np.uint8, count=n * NODE,
                           offset=self.off_nodes).reshape(n, NODE)
        self.nlo = nb[:, 0:12].copy().view(np.float32).reshape(n, 3)
        self.nhi = nb[:, 12:24].copy().view(np.float32).reshape(n, 3)
        u = nb[:, 24:40].copy().view(np.uint32).reshape(n, 4)
        self.escape, self.first, self.count, self.depth = (u[:, 0], u[:, 1],
                                                           u[:, 2], u[:, 3])

        t = self.ntri
        tb = np.frombuffer(d, dtype=np.uint8, count=t * TRI,
                           offset=self.off_tris).reshape(t, TRI)
        self.v = tb[:, 0:36].copy().view(np.float32).reshape(t, 3, 3)
        self.opacity = tb[:, 36:40].copy().view(np.float32).reshape(t)
        su = tb[:, 40:48].copy().view(np.uint32).reshape(t, 2)
        self.source, self.ref = su[:, 0], su[:, 1]

    # --- THE REFERENCE TRAVERSAL -------------------------------------------
    # The same stackless escape walk AFX_LIGHT_SHADOW_RT runs, written
    # independently in python.  Returns the nearest hit distance (or inf).
    def trace(self, o, dr, tmax):
        inv = 1.0 / np.where(np.abs(dr) < 1e-12, 1e-12 * np.sign(dr + 1e-30),
                             dr)
        best = tmax
        i = 0
        n = self.nnode
        steps = 0
        while i < n:
            steps += 1
            if steps > 400000:
                raise RuntimeError("traversal did not terminate")
            t0 = (self.nlo[i] - o) * inv
            t1 = (self.nhi[i] - o) * inv
            lo = np.minimum(t0, t1)
            hi = np.maximum(t0, t1)
            tn = max(lo.max(), 0.0)
            tf = min(hi.min(), best)
            if tn <= tf:
                c = int(self.count[i])
                if c == 0:
                    i += 1
                    continue
                f = int(self.first[i])
                for k in range(f, f + c):
                    h = tri_hit(self.v[k], o, dr, best)
                    if h is not None and h < best:
                        best = h
                i = int(self.escape[i])
            else:
                i = int(self.escape[i])
        return best


def tri_hit(tri, o, d, tmax, eps=1e-8):
    """Moller-Trumbore, double-sided (a ray test has no winding)."""
    e1 = tri[1] - tri[0]
    e2 = tri[2] - tri[0]
    pv = np.cross(d, e2)
    det = float(np.dot(e1, pv))
    if abs(det) < eps:
        return None
    idet = 1.0 / det
    tv = o - tri[0]
    u = float(np.dot(tv, pv)) * idet
    if u < 0.0 or u > 1.0:
        return None
    qv = np.cross(tv, e1)
    v = float(np.dot(d, qv)) * idet
    if v < 0.0 or u + v > 1.0:
        return None
    t = float(np.dot(e2, qv)) * idet
    if t <= 1e-4 or t >= tmax:
        return None
    return t


def brute(vs, o, d, tmax):
    """Every triangle, vectorised.  The thing the tree has to agree with."""
    e1 = vs[:, 1] - vs[:, 0]
    e2 = vs[:, 2] - vs[:, 0]
    pv = np.cross(np.broadcast_to(d, e2.shape), e2)
    det = np.einsum("ij,ij->i", e1, pv)
    ok = np.abs(det) >= 1e-8
    idet = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
    tv = o - vs[:, 0]
    u = np.einsum("ij,ij->i", tv, pv) * idet
    ok &= (u >= 0.0) & (u <= 1.0)
    qv = np.cross(tv, e1)
    v = np.einsum("ij,ij->i", np.broadcast_to(d, qv.shape), qv) * idet
    ok &= (v >= 0.0) & (u + v <= 1.0)
    t = np.einsum("ij,ij->i", e2, qv) * idet
    ok &= (t > 1e-4) & (t < tmax)
    if not ok.any():
        return tmax
    return float(t[ok].min())


# ======================================================================
# the independent re-derivation
# ======================================================================
def read_mtl(tdir):
    """name -> dict of the flags cx_bvh's exclusions turn on."""
    p = os.path.join(tdir, "track.mtl")
    mats, cur = {}, None
    if not os.path.exists(p):
        return mats
    for line in open(p):
        line = line.rstrip("\r\n").rstrip()
        if line.startswith("newmtl "):
            cur = line[7:]
            mats[cur] = {"decal": 0, "alpha_blend": 0, "alpha": 0,
                         "alpha_test": 0, "two_sided": 0, "texture": ""}
        elif cur is None:
            continue
        elif line.startswith("map_Kd "):
            mats[cur]["texture"] = os.path.splitext(
                os.path.basename(line[7:]))[0]
        else:
            m = re.match(r"^# (decal|alpha_blend|alpha_test|two_sided|alpha) "
                         r"(-?\d+)$", line)
            if m:
                mats[cur][m.group(1)] = int(m.group(2))
    return mats


def derive_track(tdir, mats):
    """The opaque track submeshes, with the two-sided duplicate dropped."""
    p = os.path.join(tdir, "track.obj")
    if not os.path.exists(p):
        return np.zeros((0, 3, 3), np.float32)
    pos, out = [], []
    skipping, dedup, seen, cur = False, False, set(), None
    for line in open(p):
        if line.startswith("v "):
            pos.append([float(x) for x in line[2:].split()[:3]])
        elif line.startswith("usemtl "):
            cur = mats.get(line[7:].rstrip("\r\n").rstrip())
            skipping = bool(cur and (cur["decal"] or cur["alpha_blend"]
                                     or cur["alpha"]))
            dedup = bool(cur and cur["two_sided"] and not skipping)
            seen = set()
        elif line.startswith("f ") and not skipping:
            f = [int(w.split("/")[0]) for w in line[2:].split()[:3]]
            if len(f) != 3:
                continue
            if dedup:
                k = tuple(sorted(f))
                if k in seen:
                    continue
                seen.add(k)
            out.append([pos[f[0] - 1], pos[f[1] - 1], pos[f[2] - 1]])
    a = np.asarray(out, np.float32) if out else np.zeros((0, 3, 3), np.float32)
    return drop_degenerate(a)


def derive_inst(tdir, name, magic):
    p = os.path.join(tdir, name)
    if not os.path.exists(p):
        return np.zeros((0, 3, 3), np.float32)
    d = open(p, "rb").read()
    if d[:4] != magic:
        return np.zeros((0, 3, 3), np.float32)
    mc, ic, vc, idc = struct.unpack_from("<IIII", d, 8)
    om, oi, ov, oidx = struct.unpack_from("<IIII", d, 0x18)
    verts = np.frombuffer(d, np.float32, count=vc * 8, offset=ov).reshape(vc, 8)
    idx = np.frombuffer(d, np.uint16, count=idc, offset=oidx)
    models = [struct.unpack_from("<IIII", d, om + k * 0x60 + 0x18)
              for k in range(mc)]
    out = []
    for k in range(ic):
        r = oi + k * 0x50
        m = np.array(struct.unpack_from("<16f", d, r), np.float32)
        mi, = struct.unpack_from("<I", d, r + 0x40)
        if mi >= mc:
            continue
        m[3] = m[7] = m[11] = 0.0
        fv, nv, fi, ni = models[mi]
        if nv == 0 or ni < 3:
            continue
        if fv > vc or nv > vc - fv or fi > idc or ni > idc - fi:
            continue
        li = idx[fi:fi + ni - (ni % 3)].astype(np.int64)
        if (li >= nv).any():
            good = (li.reshape(-1, 3) < nv).all(axis=1)
            li = li.reshape(-1, 3)[good].reshape(-1)
        lv = verts[fv + li, 0:3].astype(np.float32)
        # The same column-major apply b3r_inst_build() bakes the draw with,
        # and in the same WIDTH: a float64 bake of an 8 km world disagrees
        # with the stage's float32 one by about half a millimetre, which is
        # under any threshold worth setting and over a bit-compare.
        w = np.empty_like(lv)
        w[:, 0] = m[0]*lv[:, 0] + m[4]*lv[:, 1] + m[8]*lv[:, 2] + m[12]
        w[:, 1] = m[1]*lv[:, 0] + m[5]*lv[:, 1] + m[9]*lv[:, 2] + m[13]
        w[:, 2] = m[2]*lv[:, 0] + m[6]*lv[:, 1] + m[10]*lv[:, 2] + m[14]
        out.append(w.astype(np.float32).reshape(-1, 3, 3))
    if not out:
        return np.zeros((0, 3, 3), np.float32)
    return drop_degenerate(np.concatenate(out))


def drop_degenerate(a):
    if a.shape[0] == 0:
        return a
    d01 = (a[:, 0] == a[:, 1]).all(axis=1)
    d02 = (a[:, 0] == a[:, 2]).all(axis=1)
    d12 = (a[:, 1] == a[:, 2]).all(axis=1)
    return a[~(d01 | d02 | d12)]


def tri_sorted(a, q=1e-3):
    """The triangle set as a lexsorted array of QUANTISED coordinates.

    Not a byte compare: the stage bakes the instance transform in float32 and
    numpy bakes it in float64, so the two agree to about a micrometre and not
    to a bit.  A millimetre grid is four orders of magnitude below anything
    that could hide a transposed matrix or a wrong index and is well above
    the disagreement two float widths can produce.
    """
    if a.shape[0] == 0:
        return np.zeros((0, 9), np.int64)
    k = np.rint(a.astype(np.float64).reshape(-1, 9) / q).astype(np.int64)
    order = np.lexsort(tuple(k[:, i] for i in range(8, -1, -1)))
    return k[order]


# ======================================================================
# sections
# ======================================================================
def section_source():
    print("\n== 1. THE SOURCE ==")
    if not os.path.exists(CX_C):
        ck(False, "tools/cextract/cx_bvh.c exists")
        return
    src = open(CX_C).read()
    bld = ((open(CX_BUILD_C).read() if os.path.exists(CX_BUILD_C) else "")
           + (open(CX_BUILD_H).read() if os.path.exists(CX_BUILD_H) else ""))
    ck(bool(bld), "the shared builder (cx_bvh_build.c/.h) is present -- one "
                  "builder, so bvh.bin and carbvh.bin cannot drift apart from "
                  "the one traversal that walks them both")
    ck("NO PYTHON ORACLE" in src,
       "cx_bvh.c carries the NO-PYTHON-ORACLE banner every unoracled stage "
       "in this pipeline carries")
    ck("INSPIRED" in src and "NOTHING IN THIS FILE IS A CLAIM ABOUT" in src,
       "...and says in as many words that it is not a claim about the game")
    ck("B3BV" in src and "version=1" in src,
       "...and documents its magic and version")
    ck(re.search(r"#define\s+CXV_LEAF_MAX\s+8", bld) is not None,
       "the leaf ceiling is 8 -- the three bits the float packing has room "
       "for")
    ck(re.search(r"#define\s+CXV_ALPHA_REF\s+64", src) is not None,
       "the cut-out coverage uses the GAME'S own alpha ref (64/255, "
       "D3DRS_ALPHAREF @0x00038FEE [C]), not a threshold of its own")
    ck("cxv_dup_seen" in src and "two_sided" in src,
       "the two-sided reverse-wound duplicate is dropped, and by a "
       "first-occurrence rule so the drop is deterministic")
    ck("decal" in src and "alpha_blend" in src and "alpha_pass" in src,
       "the decal layer and the blended pass are excluded")
    # the stage may not be recursive: it is linked into the game and
    # materialises in-process on the web, where the stack is 64 KB.
    ck("TWO PHASES, and both are ITERATIVE" in bld and
       "cxv_build_range" not in bld,
       "the build is ITERATIVE -- a SAH split may be arbitrarily lopsided, "
       "so the depth is not bounded by log2(n), and these stages run "
       "in-process inside the game")

    reg = open(os.path.join(ROOT, "tools", "cextract", "cx_extract.h")).read()
    ck("X(BVH,          bvh)" in reg or re.search(r"X\(BVH,\s+bvh\)", reg),
       "the stage is registered in CX_STAGE_LIST")
    ck(reg.index("X(BVH") > reg.index("X(SCENERY"),
       "...AFTER scenery, props and track: it reads their artefacts")
    ver = open(os.path.join(ROOT, "tools", "cextract",
                            "verify_cextract.py")).read()
    ck('"bvh.bin"' in ver,
       "bvh.bin is in verify_cextract.py's NO_ORACLE list")
    iso = open(os.path.join(ROOT, "src", "burnout3_isodata.c")).read()
    ck('"bvh.bin"' in iso,
       "...and in burnout3_isodata.c's TRACK_MAP, so an ISO boot "
       "materialises it")
    ck(re.search(r'"tlist track textures.*?"\s*\n\s*"[^"]*bvh', iso,
                 re.S) is not None,
       "...and in the map's catch-all stage list")


def section_determinism(tracks):
    print("\n== 2. DETERMINISM ==")
    exe = os.path.join(tempfile.gettempdir(), "b3_validate_bvh", "cxtract")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    r = subprocess.run(["bash", os.path.join(ROOT, "tools", "cextract",
                                             "build.sh"), exe],
                       cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(exe):
        skip("the stage is deterministic (double run, byte diff)",
             "cxtract would not build")
        return
    t = tracks[0]
    tdir = os.path.join(TRACKS, t)
    hashes = []
    for i in range(2):
        out = os.path.join(tempfile.gettempdir(), "b3_validate_bvh",
                           "run%d" % i)
        os.makedirs(out, exist_ok=True)
        for f in ("track.obj", "track.mtl", "props.bin", "scenery.bin"):
            s = os.path.join(tdir, f)
            if os.path.exists(s) and not os.path.exists(os.path.join(out, f)):
                os.symlink(s, os.path.join(out, f))
        tx = os.path.join(out, "textures")
        if os.path.isdir(os.path.join(tdir, "textures")) and \
                not os.path.exists(tx):
            os.symlink(os.path.join(tdir, "textures"), tx)
        rr = subprocess.run([exe, "--only", "bvh", "--track", t,
                             "--out", out], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        p = os.path.join(out, "bvh.bin")
        if not os.path.exists(p):
            ck(False, "the stage ran (%s)" % t,
               rr.stdout.decode("utf-8", "replace")[-300:])
            return
        hashes.append(hashlib.sha256(open(p, "rb").read()).hexdigest())
    ck(hashes[0] == hashes[1],
       "the stage is deterministic: two runs over %s produce the SAME bytes"
       % t, hashes[0][:16])


def section_track(t):
    tdir = os.path.join(TRACKS, t)
    p = os.path.join(tdir, "bvh.bin")
    if not os.path.exists(p):
        skip("bvh.bin for %s" % t, "not extracted")
        return
    b = Bvh(p)

    # ---- 3. the header -------------------------------------------------
    ck(b.magic == MAGIC and b.version == VERSION,
       "%s: magic and version" % t, "%r v%d" % (b.magic, b.version))
    ck(b.off_nodes == HDR and
       b.off_tris == HDR + b.nnode * NODE and
       len(b.raw) == b.off_tris + b.ntri * TRI,
       "%s: the header describes the body it is attached to" % t,
       "%d nodes + %d tris = %d bytes" % (b.nnode, b.ntri, len(b.raw)))
    ck(b.n_track + b.n_props + b.n_scen == b.ntri,
       "%s: the source tally adds up" % t,
       "%d + %d + %d = %d" % (b.n_track, b.n_props, b.n_scen, b.ntri))
    ck(b.leaf_count == int((b.count > 0).sum()),
       "%s: the header's leaf count is the file's leaf count" % t)
    ck(int(b.depth.max()) == b.max_depth,
       "%s: the header's max depth is the tree's max depth" % t,
       "%d" % b.max_depth)

    # ---- 4. the packing contract ---------------------------------------
    ck(int(b.count.max()) <= LEAF_MAX,
       "%s: no leaf holds more than %d triangles -- the three bits the "
       "float packing has" % (t, LEAF_MAX), "max %d" % int(b.count.max()))
    leaf = b.count > 0
    packed = b.first[leaf].astype(np.float64) * 8.0 + (b.count[leaf] - 1)
    ck(bool((packed < 2.0 ** 24).all()),
       "%s: `first * 8 + (count - 1)` stays inside a highp float's 24 exact "
       "bits -- ESSL 1.00 has no bitwise operators, so that arithmetic IS "
       "the encoding" % t, "max %.0f" % (packed.max() if packed.size else 0))
    ck(bool((b.escape.astype(np.int64) <= b.nnode).all()),
       "%s: no escape index points past the array" % t)

    # ---- 5. the tree ---------------------------------------------------
    interior = ~leaf
    idx = np.arange(b.nnode)
    ck(bool((b.escape[leaf] == idx[leaf] + 1).all()),
       "%s: a LEAF's escape is its own index + 1 -- it has no subtree" % t)
    ck(bool((b.escape[interior] > idx[interior] + 1).all()),
       "%s: an INTERIOR node's escape is past its left child, which sits at "
       "index + 1" % t)
    ck(b.escape[0] == b.nnode,
       "%s: the ROOT's escape is node_count -- a ray that misses the world "
       "is done" % t)
    # the left child is index+1 and the right child is where the left
    # subtree ended; both must lie inside this node's own span.
    ok = True
    for i in np.nonzero(interior)[0]:
        r = int(b.escape[i + 1])
        if not (i + 1 < r < b.escape[i] + 1):
            ok = False
            break
    ck(ok, "%s: every interior node's two subtrees tile its own span" % t)

    # boxes contain children
    lo, hi = b.nlo.astype(np.float64), b.nhi.astype(np.float64)
    ci = np.nonzero(interior)[0]
    if ci.size:
        l = ci + 1
        r = b.escape[l].astype(np.int64)
        eps = 1e-3
        good = ((lo[ci] <= lo[l] + eps).all(axis=1) &
                (hi[ci] >= hi[l] - eps).all(axis=1) &
                (lo[ci] <= lo[r] + eps).all(axis=1) &
                (hi[ci] >= hi[r] - eps).all(axis=1))
        ck(bool(good.all()),
           "%s: every node's box CONTAINS both of its children's boxes" % t,
           "%d of %d violate it" % (int((~good).sum()), ci.size))

    # leaves tile the triangle array exactly once
    order = np.argsort(b.first[leaf], kind="stable")
    f = b.first[leaf][order].astype(np.int64)
    c = b.count[leaf][order].astype(np.int64)
    tiled = f.size and f[0] == 0 and bool((f[1:] == (f + c)[:-1]).all()) and \
        (f[-1] + c[-1]) == b.ntri
    ck(bool(tiled),
       "%s: the leaves TILE the triangle array exactly once -- every "
       "triangle is reachable and none twice" % t)

    # a leaf's box contains its own triangles
    if f.size:
        li = np.nonzero(leaf)[0]
        bad = 0
        for i in li[:4000]:
            s, n = int(b.first[i]), int(b.count[i])
            v = b.v[s:s + n].reshape(-1, 3)
            if (v.min(axis=0) < lo[i] - 1e-2).any() or \
               (v.max(axis=0) > hi[i] + 1e-2).any():
                bad += 1
        ck(bad == 0,
           "%s: a leaf's box contains its own triangles" % t,
           "%d of %d sampled leaves violate it" % (bad, min(4000, li.size)))

    ck(bool((b.nlo[0] <= b.v.reshape(-1, 3).min(axis=0) + 1e-2).all() and
            (b.nhi[0] >= b.v.reshape(-1, 3).max(axis=0) - 1e-2).all()),
       "%s: the ROOT box contains the whole world" % t)

    # ---- 6. the exclusions ---------------------------------------------
    ck(bool((b.opacity > 0.0).all()) and bool((b.opacity <= 1.0).all()),
       "%s: every opacity is in (0, 1]" % t,
       "min %.3f" % float(b.opacity.min()))
    e1 = b.v[:, 1] - b.v[:, 0]
    e2 = b.v[:, 2] - b.v[:, 0]
    area = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
    ck(bool((area > 0.0).all()),
       "%s: no degenerate triangle reached the tree" % t,
       "%d zero-area" % int((area <= 0.0).sum()))
    ck(bool(((b.source == 0) | (b.source == 1) | (b.source == 2)).all()),
       "%s: every triangle names a source the loader knows" % t)

    # ---- 7. the geometry, re-derived -----------------------------------
    mats = read_mtl(tdir)
    want_t = derive_track(tdir, mats)
    want_p = derive_inst(tdir, "props.bin", b"B3PP")
    want_s = derive_inst(tdir, "scenery.bin", b"B3SC")
    ck(want_t.shape[0] == b.n_track,
       "%s: the TRACK triangle count matches an independent OBJ walk" % t,
       "%d re-derived vs %d in the file" % (want_t.shape[0], b.n_track))
    ck(want_p.shape[0] == b.n_props,
       "%s: the PROP triangle count matches an independent props.bin bake"
       % t, "%d vs %d" % (want_p.shape[0], b.n_props))
    ck(want_s.shape[0] == b.n_scen,
       "%s: the SCENERY triangle count matches an independent scenery.bin "
       "bake" % t, "%d vs %d" % (want_s.shape[0], b.n_scen))
    want = np.concatenate([x for x in (want_t, want_p, want_s)
                           if x.shape[0]]) if b.ntri else want_t
    if want.shape[0] == b.ntri and b.ntri:
        kw = tri_sorted(want)
        kg = tri_sorted(b.v)
        ck(bool(np.array_equal(kw, kg)),
           "%s: the triangle MULTISET is exactly the one re-derived here -- "
           "the tree only permutes, it never invents or drops" % t,
           "%d triangles" % b.ntri)

    # ---- 8. the ray probes ---------------------------------------------
    # A grid over the world box, straight down and along the diagonal, plus
    # a fan out of the middle: the tree's answer against every triangle's.
    rng = np.random.RandomState(12345)
    lo3, hi3 = b.nlo[0].astype(np.float64), b.nhi[0].astype(np.float64)
    span = hi3 - lo3
    n_probe = int(os.environ.get("B3_BVH_PROBES", "48"))
    vs = b.v.astype(np.float64)
    bad = 0
    worst = 0.0
    for k in range(n_probe):
        if k % 3 == 0:
            o = lo3 + span * np.array([rng.rand(), 1.05, rng.rand()])
            d = np.array([0.0, -1.0, 0.0])
        elif k % 3 == 1:
            o = lo3 + span * rng.rand(3)
            d = rng.randn(3)
            d /= np.linalg.norm(d)
        else:
            o = lo3 + span * np.array([rng.rand(), rng.rand() * 0.4 + 0.1,
                                       rng.rand()])
            d = np.array([rng.randn(), abs(rng.randn()) + 0.3, rng.randn()])
            d /= np.linalg.norm(d)
        tmax = float(np.linalg.norm(span)) * 1.5
        a = b.trace(o, d, tmax)
        c2 = brute(vs, o, d, tmax)
        e = abs(a - c2)
        worst = max(worst, e if e < 1e18 else 0.0)
        if e > max(1e-3, 1e-5 * max(abs(a), abs(c2))):
            bad += 1
    ck(bad == 0,
       "%s: %d ray probes -- the flattened tree's stackless escape walk "
       "agrees with a brute-force test over every triangle" % (t, n_probe),
       "%d disagree, worst %.5f m" % (bad, worst))


def main():
    args = sys.argv[1:]
    if not os.path.isdir(TRACKS):
        print("no build/tracks -- nothing to validate")
        return 0
    tracks = sorted(d for d in os.listdir(TRACKS)
                    if re.match(r"^[A-Z]{2}_[A-Z]\d_V\d$", d) and
                    os.path.exists(os.path.join(TRACKS, d, "bvh.bin")))
    named = False
    if len(args) >= 2 and args[0] == "--tracks":
        tracks = args[1].split(",")
        named = True
    elif args:
        print(__doc__)
        return 2

    section_source()
    if not tracks:
        print("\n(no track carries a bvh.bin yet -- run the stage first)")
    else:
        section_determinism(tracks)
        deep = tracks if named else (tracks[:2] + tracks[-2:])
        for t in sorted(set(deep)):
            print("\n== %s ==" % t)
            section_track(t)

    print("\n%d passed, %d failed, %d skipped"
          % (len(PASS), len(FAIL), len(SKIP)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
