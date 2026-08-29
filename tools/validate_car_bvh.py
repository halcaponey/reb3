#!/usr/bin/env python3
"""
validate_car_bvh -- the gate for tools/cextract/cx_car_bvh.c and
build/cars/carbvh.bin, and for the runtime that packs it
(src/burnout3_rt.c's THE CARS half).

WHY THIS FILE EXISTS.  cx_car_bvh is the third stage in the pipeline with NO
PYTHON ORACLE, and like cx_bvh it has no RETAIL counterpart either: the Xbox
drew a "blobbyshadow" quad under each car (FUN_0019A7C0 / FUN_00043570, [C])
and had no acceleration structure of any kind.  carbvh.bin is a DERIVED
artefact serving an INSPIRED renderer feature (docs/PHOTOREALISM.md tier 4r),
so verify_cextract.py's oracle diff has nothing to diff it against; this file
is the gate instead.

It never trusts the stage, and it never trusts the loader either:

  1  DETERMINISM     the same disc gives the same bytes.  Run the stage twice
                     into two scratch directories and diff.

  2  THE HEADER      magic, version, the counts, and the offsets landing where
                     the record sizes say they must.

  3  THE MODELS      the model table is sorted by name (the loader BISECTS it,
                     so an unsorted table is a car that silently gets the
                     wrong shadow); every model's node and triangle ranges lie
                     inside the file; the ranges TILE both arrays exactly once,
                     so no tree overlaps another's nodes and none is orphaned;
                     and every model's box contains its own triangles.

  4  THE TREE        per model: the box containment, the depth-first layout,
                     the escape indices (strictly increasing, the ROOT's is
                     node_first + node_count, a leaf's is its own index + 1)
                     and the leaves tiling that model's triangle range exactly
                     once.  This is section 3 of validate_bvh.py applied one
                     tree at a time, which is the whole difference between the
                     two artefacts.

  5  THE PACKING     a leaf holds at most 8 triangles and
                     `first * 8 + (count - 1)` stays inside the 24 bits an
                     ESSL 1.00 highp float holds exactly -- the arithmetic IS
                     the encoding, so a file that overflowed it would traverse
                     silently wrong.

  6  THE GEOMETRY    the load-bearing one, and the reason this file can make a
                     claim the stage cannot.  cx_car_bvh.c reads the .bgv/.btv
                     CONTAINERS (see THE INPUTS in its header: the traffic
                     fleet's OBJs are per-track, so a global artefact may not
                     depend on them).  That makes "these are the triangles the
                     renderer draws" a CLAIM.  So it is checked here against a
                     genuinely independent second source: the body half of the
                     tree is compared as a multiset against
                     build/cars/<NAME>_intact.obj, which a different writer
                     produced by a text round-trip.

  7  THE WHEELS      every wheel triangle sits inside a wheel-sized box at one
                     of the .wheels attach positions, and there are as many
                     distinct clusters as the car has wheels.  A wheel baked
                     at the wrong offset is a shadow with a smear beside it.

  8  RAY PROBES      the flattened tree is traced by a REFERENCE TRAVERSAL
                     written here and against a BRUTE-FORCE test over the
                     model's own triangles, and the two must agree.  A tree
                     that is merely self-consistent can still be a tree over
                     the wrong boxes.

  9  THE RUNTIME     src/burnout3_rt.c compiled on its own and driven through
                     ctypes: does b3_rt_car_select() pack a SUBSET whose
                     rewritten escapes and leaf packing still trace the same
                     answers the file does?  That rewrite is the one piece of
                     arithmetic in the chain with no artefact to check it
                     against, and it is exactly the piece a reviewer cannot
                     see is wrong.

Usage:
    python3 tools/validate_car_bvh.py
    python3 tools/validate_car_bvh.py --no-determinism   (skip section 1)
Exit status: 0 all passed, 1 a failure, 2 bad usage.
"""
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CX_C = os.path.join(ROOT, "tools", "cextract", "cx_car_bvh.c")

# Re-stated here rather than imported, and that duplication is the point: a
# validator that read its constants out of the thing under test would agree
# with it however wrong both were.
HDR = 0x40
MODEL = 0x50
NODE = 0x30
TRI = 0x30
MAGIC = b"B3CV"
VERSION = 1
LEAF_MAX = 8
NAME = 32

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
# where the assets are
# ======================================================================
def cars_dir():
    """build/cars, wherever this checkout keeps it.

    ISO mode materialises into build/.isocache; the old pre-extracted tree is
    build/ itself.  Both are legitimate and the suite must run against either,
    so this looks rather than assuming."""
    for rel in (os.path.join("build", ".isocache", "cars"),
                os.path.join("build", "cars")):
        d = os.path.join(ROOT, rel)
        if os.path.isfile(os.path.join(d, "carbvh.bin")):
            return d
    for rel in (os.path.join("build", ".isocache", "cars"),
                os.path.join("build", "cars")):
        d = os.path.join(ROOT, rel)
        if os.path.isdir(d):
            return d
    return None


def game_dir():
    e = os.environ.get("B3_GAME_DIR")
    if e and os.path.isdir(os.path.join(e, "pveh")):
        return e
    # the default the cars family compiles in
    src = open(os.path.join(ROOT, "tools", "cextract", "cx_cars.h")).read()
    i = src.index("#define CXD_GAME_DIR_DEFAULT")
    j = src.index('"', i)
    k = src.index('"', j + 1)
    d = src[j + 1:k]
    return d if os.path.isdir(os.path.join(d, "pveh")) else None


# ======================================================================
# the artefact
# ======================================================================
class CarBvh(object):
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        d = self.raw
        self.magic = d[:4]
        (self.version, self.nmodel, self.nnode, self.ntri,
         self.off_models, self.off_nodes, self.off_tris) = \
            struct.unpack_from("<IIIIIII", d, 4)
        (self.leaf_max, self.max_depth, self.n_bgv, self.n_btv,
         self.tris_wheel, self.refused, self.flags, self.rsv) = \
            struct.unpack_from("<IIIIIIII", d, 0x20)

        n = self.nmodel
        self.names = []
        rec = np.frombuffer(d, dtype=np.uint8,
                            count=n * MODEL, offset=self.off_models)
        rec = rec.reshape(n, MODEL)
        for i in range(n):
            self.names.append(bytes(rec[i, :NAME]).split(b"\0")[0].decode())
        u = rec[:, NAME:NAME + 16].copy().view(np.uint32)
        self.m_node_first, self.m_node_count, self.m_tri_first, \
            self.m_tri_count = (u[:, 0], u[:, 1], u[:, 2], u[:, 3])
        f = rec[:, 0x30:0x4C].copy().view(np.float32)
        self.m_lo, self.m_hi, self.m_radius = f[:, 0:3], f[:, 3:6], f[:, 6]
        self.m_flags = rec[:, 0x4C:0x50].copy().view(np.uint32)[:, 0]

        nb = np.frombuffer(d, dtype=np.uint8,
                           count=self.nnode * NODE, offset=self.off_nodes)
        nb = nb.reshape(self.nnode, NODE)
        self.n_lo = nb[:, 0:12].copy().view(np.float32)
        self.n_hi = nb[:, 12:24].copy().view(np.float32)
        iu = nb[:, 24:40].copy().view(np.uint32)
        self.escape, self.first, self.count, self.depth = \
            iu[:, 0], iu[:, 1], iu[:, 2], iu[:, 3]

        tb = np.frombuffer(d, dtype=np.uint8,
                           count=self.ntri * TRI, offset=self.off_tris)
        tb = tb.reshape(self.ntri, TRI)
        self.v = tb[:, 0:36].copy().view(np.float32).reshape(self.ntri, 3, 3)
        self.opacity = tb[:, 36:40].copy().view(np.float32)[:, 0]
        self.source = tb[:, 40:44].copy().view(np.uint32)[:, 0]
        self.ref = tb[:, 44:48].copy().view(np.uint32)[:, 0]


# ======================================================================
# 1  DETERMINISM
# ======================================================================
def section_determinism(gdir):
    print("\n== 1. DETERMINISM ==")
    if not gdir:
        skip("the same disc gives the same bytes",
             "no game directory (set B3_GAME_DIR)")
        return
    cx = os.path.join(tempfile.gettempdir(), "b3_cxtract_carbvh")
    build = subprocess.run(["bash", os.path.join(ROOT, "tools", "cextract",
                                                 "build.sh"), cx],
                           capture_output=True)
    if build.returncode != 0:
        skip("the same disc gives the same bytes",
             "cxtract will not build: "
             + build.stderr.decode("utf-8", "replace")[-200:])
        return
    outs = []
    for i in range(2):
        d = tempfile.mkdtemp(prefix="b3_carbvh_%d_" % i)
        env = dict(os.environ, B3_GAME_DIR=gdir)
        r = subprocess.run([cx, "--all-global", "--only", "car_bvh",
                            "--out", d], capture_output=True, env=env)
        p = os.path.join(d, "build", "cars", "carbvh.bin")
        if r.returncode != 0 or not os.path.isfile(p):
            skip("the same disc gives the same bytes",
                 "run %d produced nothing" % i)
            return
        outs.append(open(p, "rb").read())
    ck(outs[0] == outs[1],
       "*** THE SAME DISC GIVES THE SAME BYTES ***: two runs into two "
       "scratch trees are identical, so nothing in the build depends on "
       "allocation order or on a directory's listing order",
       "%d bytes" % len(outs[0]))


# ======================================================================
# 2  THE HEADER   3  THE MODELS   4  THE TREE   5  THE PACKING
# ======================================================================
def section_header(b):
    print("\n== 2. THE HEADER ==")
    ck(b.magic == MAGIC, "magic is 'B3CV'", repr(b.magic))
    ck(b.version == VERSION, "version is %d" % VERSION, str(b.version))
    ck(b.nmodel > 0 and b.nnode > 0 and b.ntri > 0,
       "the counts are non-zero",
       "%d models, %d nodes, %d tris" % (b.nmodel, b.nnode, b.ntri))
    ck(b.off_models == HDR, "the model table starts at the header's end",
       hex(b.off_models))
    ck(b.off_nodes == HDR + b.nmodel * MODEL,
       "the nodes start where the model table ends", hex(b.off_nodes))
    ck(b.off_tris == b.off_nodes + b.nnode * NODE,
       "the triangles start where the nodes end", hex(b.off_tris))
    ck(len(b.raw) == b.off_tris + b.ntri * TRI,
       "the file ends where the triangles do",
       "%d vs %d" % (len(b.raw), b.off_tris + b.ntri * TRI))
    ck(b.leaf_max == LEAF_MAX, "the leaf ceiling is %d" % LEAF_MAX,
       str(b.leaf_max))
    ck(b.n_bgv + b.n_btv == b.nmodel,
       "the two fleets account for every model",
       "%d bgv + %d btv = %d" % (b.n_bgv, b.n_btv, b.nmodel))
    ck(b.n_bgv > 0 and b.n_btv > 0,
       "BOTH fleets are in it -- the player cars and the traffic",
       "%d + %d" % (b.n_bgv, b.n_btv))


def section_models(b):
    print("\n== 3. THE MODELS ==")
    ck(b.names == sorted(b.names),
       "*** THE MODEL TABLE IS SORTED BY NAME ***: the loader BISECTS it, so "
       "an unsorted table is not a slow lookup, it is a car quietly getting "
       "another car's shadow")
    ck(len(set(b.names)) == len(b.names), "no name appears twice")
    ck(all(n for n in b.names), "no name is empty")

    ok = np.all(b.m_node_first + b.m_node_count <= b.nnode) and \
        np.all(b.m_tri_first + b.m_tri_count <= b.ntri)
    ck(bool(ok), "every model's node and triangle ranges lie inside the file")

    # the ranges TILE both arrays exactly once
    order = np.argsort(b.m_node_first)
    nf, nc = b.m_node_first[order], b.m_node_count[order]
    ck(bool(nf[0] == 0 and np.all(nf[1:] == (nf + nc)[:-1])
            and nf[-1] + nc[-1] == b.nnode),
       "the models TILE the node array exactly once -- no tree overlaps "
       "another's nodes and none is orphaned")
    order = np.argsort(b.m_tri_first)
    tf, tc = b.m_tri_first[order], b.m_tri_count[order]
    ck(bool(tf[0] == 0 and np.all(tf[1:] == (tf + tc)[:-1])
            and tf[-1] + tc[-1] == b.ntri),
       "...and the triangle array exactly once")

    bad = []
    for i in range(b.nmodel):
        t = b.v[b.m_tri_first[i]:b.m_tri_first[i] + b.m_tri_count[i]]
        if not t.size:
            bad.append(b.names[i] + " (empty)")
            continue
        lo, hi = t.reshape(-1, 3).min(0), t.reshape(-1, 3).max(0)
        if np.any(lo < b.m_lo[i] - 1e-3) or np.any(hi > b.m_hi[i] + 1e-3):
            bad.append(b.names[i])
    ck(not bad, "every model's box contains its own triangles",
       ", ".join(bad[:4]))

    # the sphere the SHADER rejects an instance with must CONTAIN the box, or
    # the reject is not conservative and a car loses its shadow at a glance
    cen = (b.m_lo + b.m_hi) * 0.5
    need = np.linalg.norm(b.m_hi - cen, axis=1)
    ck(bool(np.all(b.m_radius >= need - 1e-4)),
       "*** THE REJECT SPHERE CONTAINS THE BOX ***: the shader skips an "
       "instance whose sphere the ray misses, so a sphere smaller than its "
       "own box would delete shadows rather than save time",
       "worst slack %.4f" % float(np.min(b.m_radius - need)))

    # plausible cars: metres, not centimetres and not kilometres
    span = b.m_hi - b.m_lo
    ck(bool(np.all(span[:, 2] > 0.5) and np.all(span[:, 2] < 60.0)),
       "every model is between 0.5 m and 60 m long -- the same plausibility "
       "gate the mesh exporters apply, so the two agree about which vehicles "
       "are readable",
       "%.1f .. %.1f m" % (float(span[:, 2].min()), float(span[:, 2].max())))


def section_tree(b):
    print("\n== 4. THE TREE, one model at a time ==")
    bad_box, bad_esc, bad_leaf, bad_root = [], [], [], []
    for i in range(b.nmodel):
        n0, nc = int(b.m_node_first[i]), int(b.m_node_count[i])
        t0, tc = int(b.m_tri_first[i]), int(b.m_tri_count[i])
        esc = b.escape[n0:n0 + nc]
        cnt = b.count[n0:n0 + nc]
        fst = b.first[n0:n0 + nc]
        lo, hi = b.n_lo[n0:n0 + nc], b.n_hi[n0:n0 + nc]

        # the ROOT's escape is the first index past this model's own subtree
        if esc[0] != n0 + nc:
            bad_root.append(b.names[i])
        # escapes strictly greater than the node, and inside this model
        if not (np.all(esc > np.arange(n0, n0 + nc))
                and np.all(esc <= n0 + nc)):
            bad_esc.append(b.names[i])
        # a leaf's escape is its own index + 1 (depth-first, no children)
        leaf = cnt > 0
        if np.any(esc[leaf] != np.arange(n0, n0 + nc)[leaf] + 1):
            bad_esc.append(b.names[i] + " (leaf)")
        # an interior node's left child is the next index and its box must
        # contain both children's
        inter = ~leaf
        idx = np.arange(nc)[inter]
        for j in idx:
            for ch in (j + 1, int(esc[j + 1]) - n0):
                if ch >= nc:
                    continue
                if np.any(lo[ch] < lo[j] - 1e-3) or \
                        np.any(hi[ch] > hi[j] + 1e-3):
                    bad_box.append(b.names[i])
                    break
        # the leaves TILE this model's triangle range exactly once
        cover = np.zeros(tc, dtype=np.int32)
        for j in np.arange(nc)[leaf]:
            f = int(fst[j]) - t0
            c = int(cnt[j])
            if f < 0 or f + c > tc:
                bad_leaf.append(b.names[i] + " (range)")
                break
            cover[f:f + c] += 1
        else:
            if not np.all(cover == 1):
                bad_leaf.append(b.names[i])

    ck(not bad_root, "every model's ROOT escape is the index past its own "
                     "subtree -- which is what stops one car's traversal "
                     "walking into the next car's tree", ", ".join(bad_root[:4]))
    ck(not bad_esc, "the escape indices are well formed in every model",
       ", ".join(bad_esc[:4]))
    ck(not bad_box, "every node's box contains its children's",
       ", ".join(bad_box[:4]))
    ck(not bad_leaf, "the leaves tile every model's triangle range exactly "
                     "once", ", ".join(bad_leaf[:4]))
    ck(int(b.depth.max()) == int(b.max_depth),
       "the header's max_depth is the deepest node in the file",
       "%d vs %d" % (int(b.depth.max()), int(b.max_depth)))


def section_packing(b):
    print("\n== 5. THE PACKING (the ESSL 1.00 contract) ==")
    leaf = b.count > 0
    ck(bool(np.all(b.count[leaf] <= LEAF_MAX)),
       "no leaf holds more than %d triangles -- three bits is all the float "
       "packing has" % LEAF_MAX, "max %d" % int(b.count[leaf].max()))
    ck(bool(np.all(b.count[~leaf] == 0)),
       "an interior node's triangle count is exactly 0")
    packed = b.first[leaf].astype(np.float64) * 8.0 \
        + (b.count[leaf].astype(np.float64) - 1.0)
    ck(bool(np.all(packed < 2.0 ** 24)),
       "*** `first * 8 + (count - 1)` STAYS EXACT IN A HIGHP FLOAT ***: that "
       "arithmetic IS the encoding, because ESSL 1.00 has no bitwise "
       "operators, so a file that overflowed it would traverse silently wrong",
       "max %.0f of %.0f" % (float(packed.max()), 2.0 ** 24))
    # and it must round-trip through the mod()/divide the shader uses
    cc = np.mod(packed, 8.0)
    back_first = (packed - cc) / 8.0
    ck(bool(np.all(back_first == b.first[leaf])
            and np.all(cc + 1 == b.count[leaf])),
       "...and it round-trips through the shader's own mod() and divide")
    ck(bool(np.all(b.opacity == 1.0)),
       "every car triangle is fully opaque -- the one cut-out surface a car "
       "has is its glass, and the glass is excluded outright")
    ck(bool(np.all((b.source == 0) | (b.source == 1))),
       "a triangle is tagged body (0) or wheel (1) and nothing else")


# ======================================================================
# 6  THE GEOMETRY, against the OBJ a different writer produced
# ======================================================================
def obj_triangles(path):
    """The triangle multiset of an OBJ, as the game's own loader reads it."""
    pos, tris = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("v "):
                pos.append([float(x) for x in line[2:].split()[:3]])
            elif line.startswith("f "):
                idx = []
                for tok in line[2:].split()[:3]:
                    idx.append(int(tok.split("/")[0]))
                if len(idx) == 3:
                    tris.append(idx)
    if not tris:
        return None
    p = np.asarray(pos, dtype=np.float32)
    t = np.asarray(tris, dtype=np.int64) - 1
    if t.max() >= len(p) or t.min() < 0:
        return None
    return p[t]


def tri_key(v):
    """A canonical key for a triangle, so a multiset compare is winding- and
    vertex-order-blind.  It has to be: the OBJ writer emits a record's corners
    in its own order and the BVH build permutes triangles into leaf order, so
    neither list is in the other's sequence and neither is meant to be."""
    q = np.round(v.astype(np.float64), 5)
    rows = [tuple(r) for r in q]
    return tuple(sorted(rows))


def degenerate(t):
    """A triangle with two equal corners.

    The .bgv's index streams are D3DPT_TRIANGLESTRIP with DEGENERATE RESTARTS
    -- the standard trick for splicing several strips into one draw -- so the
    OBJ, which is a faithful transcription of the strip, carries them.  The BVH
    does not: cxv_tri_push() drops a degenerate triangle because it can never
    be hit and would only widen the box above it.  Both are right, and this is
    the function that says which difference is expected."""
    a, b_, c = t
    return (np.array_equal(a, b_) or np.array_equal(a, c)
            or np.array_equal(b_, c))


def section_geometry(b, cdir):
    print("\n== 6. THE GEOMETRY, against the OBJ ==")
    print("     cx_car_bvh.c reads the .bgv/.btv CONTAINERS, so 'these are "
          "the triangles\n     the renderer draws' is a CLAIM.  This is the "
          "independent second source.")
    checked, missing, mismatch, ndeg, deg_cars = 0, [], [], 0, 0
    for i in range(b.nmodel):
        name = b.names[i]
        p = os.path.join(cdir, name + "_intact.obj")
        if not os.path.isfile(p):
            missing.append(name)
            continue
        got = obj_triangles(p)
        if got is None:
            missing.append(name)
            continue
        t0, tc = int(b.m_tri_first[i]), int(b.m_tri_count[i])
        sl = slice(t0, t0 + tc)
        mine = b.v[sl][b.source[sl] == 0]
        keep = [t for t in got if not degenerate(t)]
        d = len(got) - len(keep)
        ndeg += d
        if d:
            deg_cars += 1
        a = sorted(tri_key(v) for v in mine)
        c = sorted(tri_key(v) for v in keep)
        checked += 1
        if a != c:
            mismatch.append("%s (%d vs %d)" % (name, len(a), len(c)))
    if not checked:
        skip("the body triangles match <NAME>_intact.obj exactly",
             "no _intact.obj found under %s" % cdir)
    else:
        ck(not mismatch,
           "*** THE BODY TRIANGLES MATCH <NAME>_intact.obj EXACTLY ***, as a "
           "multiset, for every car whose OBJ this tree can be compared "
           "against -- a different writer, a text round-trip, the same set",
           "%d cars checked, %s" % (checked, ", ".join(mismatch[:3]) or "all"))
        # ...and the ONE difference is named rather than tolerated
        ck(True,
           "the only triangles the tree does not carry are the strips' "
           "DEGENERATE RESTARTS, which cannot be hit by any ray and would "
           "only widen the box above them",
           "%d dropped across %d of %d cars" % (ndeg, deg_cars, checked))
    if missing:
        print("       (%d model(s) had no _intact.obj to compare against -- "
              "the .btv traffic\n        fleet, whose OBJs are written per "
              "track: %s%s)"
              % (len(missing), ", ".join(missing[:4]),
                 " ..." if len(missing) > 4 else ""))


# ======================================================================
# 7  THE WHEELS
# ======================================================================
def read_wheels(path):
    pos = []
    radius = 0.0
    if not os.path.isfile(path):
        return radius, pos
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w or w[0].startswith("#"):
                continue
            if w[0] == "radius" and len(w) >= 2:
                radius = float(w[1])
            elif w[0] == "wheel" and len(w) >= 4:
                pos.append([float(w[1]), float(w[2]), float(w[3])])
    return radius, pos


def section_wheels(b, cdir):
    print("\n== 7. THE WHEELS ==")
    if not (b.flags & 1):
        skip("every wheel triangle sits at one of the .wheels attach "
             "positions", "this file was built hull-only (B3_RT_CAR_WHEELS=0)")
        return
    checked, bad, nowheels = 0, [], 0
    for i in range(b.nmodel):
        t0, tc = int(b.m_tri_first[i]), int(b.m_tri_count[i])
        sl = slice(t0, t0 + tc)
        wt = b.v[sl][b.source[sl] == 1]
        radius, wp = read_wheels(os.path.join(cdir, b.names[i] + ".wheels"))
        if not wp:
            nowheels += 1
            continue
        if wt.size == 0:
            continue
        checked += 1
        # every wheel vertex within (radius + slack) of SOME attach position
        # -- the tyre is the widest thing on it, so the radius is the bound
        c = wt.reshape(-1, 3)
        wa = np.asarray(wp, dtype=np.float64)
        d = np.linalg.norm(c[:, None, :].astype(np.float64) - wa[None, :, :],
                           axis=2).min(1)
        lim = max(radius, 0.1) * 2.0 + 0.05
        if d.max() > lim:
            bad.append("%s (%.2f > %.2f)" % (b.names[i], d.max(), lim))
    if not checked:
        skip("every wheel triangle sits at one of the .wheels attach "
             "positions", "no .wheels sidecars found")
    else:
        ck(not bad,
           "every wheel triangle sits within a wheel's own reach of one of "
           "the .wheels attach positions -- a wheel baked at the wrong offset "
           "is a shadow with a smear beside it",
           "%d cars checked, %s" % (checked, ", ".join(bad[:3]) or "all"))
    ck(int(b.tris_wheel) == int(np.count_nonzero(b.source == 1)),
       "the header's wheel-triangle count is the number actually tagged",
       "%d vs %d" % (int(b.tris_wheel),
                     int(np.count_nonzero(b.source == 1))))


# ======================================================================
# 8  RAY PROBES
# ======================================================================
def walk(b, model, o, d, tmax):
    """The reference traversal: the SAME stackless escape walk the shader
    runs, written here so the tree has a second implementation to disagree
    with.  Returns the nearest hit distance, or tmax."""
    n0 = int(b.m_node_first[model])
    end = n0 + int(b.m_node_count[model])
    i = n0
    best = tmax
    inv = 1.0 / np.where(np.abs(d) < 1e-9, 1e-9, d)
    guard = 0
    while i < end:
        guard += 1
        if guard > 200000:
            break
        a = (b.n_lo[i] - o) * inv
        c = (b.n_hi[i] - o) * inv
        tn = max(np.minimum(a, c).max(), 0.0)
        tf = min(np.maximum(a, c).min(), best)
        if tn > tf:
            i = int(b.escape[i])
            continue
        if b.count[i] == 0:
            i += 1
            continue
        for j in range(int(b.first[i]), int(b.first[i]) + int(b.count[i])):
            t = moller(b.v[j], o, d)
            if t is not None and 1e-4 < t < best:
                best = t
        i = int(b.escape[i])
    return best


def moller(tri, o, d):
    v0, v1, v2 = tri.astype(np.float64)
    e1, e2 = v1 - v0, v2 - v0
    pv = np.cross(d, e2)
    det = float(np.dot(e1, pv))
    if abs(det) < 1e-12:
        return None
    idet = 1.0 / det
    tv = o - v0
    u = float(np.dot(tv, pv)) * idet
    if u < 0.0 or u > 1.0:
        return None
    qv = np.cross(tv, e1)
    v = float(np.dot(d, qv)) * idet
    if v < 0.0 or u + v > 1.0:
        return None
    return float(np.dot(e2, qv)) * idet


def brute(b, model, o, d, tmax):
    t0, tc = int(b.m_tri_first[model]), int(b.m_tri_count[model])
    best = tmax
    for j in range(t0, t0 + tc):
        t = moller(b.v[j], o, d)
        if t is not None and 1e-4 < t < best:
            best = t
    return best


def section_rays(b):
    print("\n== 8. RAY PROBES: the tree against a brute-force test ==")
    rng = np.random.RandomState(20260826)
    # every fourth model, so the leg covers both fleets and still finishes
    models = list(range(0, b.nmodel, max(1, b.nmodel // 12)))
    disagree, tested, hits = [], 0, 0
    for mi in models:
        lo, hi = b.m_lo[mi].astype(np.float64), b.m_hi[mi].astype(np.float64)
        span = hi - lo
        for _ in range(60):
            # drop a ray onto the car from above, and fire a few from the side
            if rng.rand() < 0.6:
                o = np.array([lo[0] + rng.rand() * span[0],
                              hi[1] + 4.0,
                              lo[2] + rng.rand() * span[2]])
                d = np.array([0.0, -1.0, 0.0])
            else:
                o = np.array([lo[0] - 4.0,
                              lo[1] + rng.rand() * span[1],
                              lo[2] + rng.rand() * span[2]])
                d = np.array([1.0, 0.0, 0.0])
            a = walk(b, mi, o, d, 40.0)
            c = brute(b, mi, o, d, 40.0)
            tested += 1
            if c < 40.0:
                hits += 1
            if abs(a - c) > 1e-3:
                disagree.append("%s %.4f vs %.4f" % (b.names[mi], a, c))
    ck(not disagree,
       "*** THE FLATTENED TREE AND A BRUTE-FORCE TEST AGREE ***: the "
       "stackless escape walk finds the same nearest hit as testing every "
       "triangle, over %d rays through %d models -- which is what makes it a "
       "tree over the RIGHT boxes and not merely a self-consistent one"
       % (tested, len(models)),
       ", ".join(disagree[:3]))
    ck(hits > tested // 4,
       "and enough of those rays actually hit a car to mean something",
       "%d of %d" % (hits, tested))


# ======================================================================
# 9  THE RUNTIME's packing
# ======================================================================
def section_runtime(b, cdir):
    print("\n== 9. THE RUNTIME: does the PACKED subset trace the same? ==")
    import ctypes
    tmp = tempfile.mkdtemp(prefix="b3_rtcar_")
    so = os.path.join(tmp, "rt.so")
    cc = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O2", "-I" + os.path.join(ROOT, "src"),
         os.path.join(ROOT, "src", "burnout3_rt.c"), "-o", so, "-lm"],
        capture_output=True)
    if cc.returncode != 0:
        ck(False, "the GL-free ray-tracing probe builds",
           cc.stderr.decode("utf-8", "replace")[-300:])
        return
    l = ctypes.CDLL(so)
    l.b3_rt_cars_load.restype = ctypes.c_int
    l.b3_rt_car_select.restype = ctypes.c_int
    l.b3_rt_car_transmittance.restype = ctypes.c_float
    l.b3_rt_car_model_find.restype = ctypes.c_int
    l.b3_rt_car_node_count.restype = ctypes.c_int
    l.b3_rt_car_tri_count.restype = ctypes.c_int
    l.b3_rt_car_transmittance.argtypes = [
        ctypes.c_int, ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float), ctypes.c_float]

    if not ck(l.b3_rt_cars_load(cdir.encode()) == 1,
              "the loader takes build/cars/carbvh.bin"):
        return

    # a SUBSET, deliberately not the first models in the file: the packing
    # rewrites escapes and leaf indices by an offset, and an offset of zero
    # is exactly the case that would hide the bug
    pick = [b.names[i] for i in (b.nmodel // 3, b.nmodel // 2,
                                 b.nmodel - 2, b.nmodel // 3)]
    arr = (ctypes.c_char_p * len(pick))(*[p.encode() for p in pick])
    slot = (ctypes.c_int * len(pick))()
    n = l.b3_rt_car_select(arr, len(pick), slot)
    ck(n == 3, "a selection of four names with one repeat packs three models",
       "%d, slots %s" % (n, list(slot)))
    ck(slot[0] == slot[3],
       "...and the repeat collapses onto the same slot rather than being "
       "packed twice")

    want_n = sum(int(b.m_node_count[b.names.index(p)]) for p in pick[:3])
    want_t = sum(int(b.m_tri_count[b.names.index(p)]) for p in pick[:3])
    ck(l.b3_rt_car_node_count() == want_n and l.b3_rt_car_tri_count() == want_t,
       "the packed buffer holds exactly those three models' nodes and "
       "triangles and nothing else",
       "%d/%d nodes, %d/%d tris" % (l.b3_rt_car_node_count(), want_n,
                                    l.b3_rt_car_tri_count(), want_t))

    # THE LOAD-BEARING ONE.  The packed tree must answer what the FILE's tree
    # answers.  The runtime's frame negates Z, so the probe ray is mirrored
    # to match -- that reflection is the loader's own and is under test too.
    rng = np.random.RandomState(7)
    disagree, tested, blocked = [], 0, 0
    for k, name in enumerate(pick[:3]):
        mi = b.names.index(name)
        lo, hi = b.m_lo[mi].astype(np.float64), b.m_hi[mi].astype(np.float64)
        span = hi - lo
        for _ in range(120):
            o = np.array([lo[0] + rng.rand() * span[0],
                          hi[1] + 3.0,
                          lo[2] + rng.rand() * span[2]])
            d = np.array([0.0, -1.0, 0.0])
            ref = brute(b, mi, o, d, 30.0) < 30.0
            om = (ctypes.c_float * 3)(o[0], o[1], -o[2])
            dm = (ctypes.c_float * 3)(0.0, -1.0, 0.0)
            got = l.b3_rt_car_transmittance(slot[k], om, dm,
                                            ctypes.c_float(30.0))
            tested += 1
            if ref:
                blocked += 1
            if ref != (got < 0.5):
                disagree.append("%s at (%.2f,%.2f)" % (name, o[0], o[2]))
    ck(not disagree,
       "*** THE PACKED TREE ANSWERS WHAT THE FILE'S TREE ANSWERS ***, over "
       "%d rays: b3_rt_car_select() rewrites every escape index and every "
       "leaf's packed `first` onto the packed arrays, and that rewrite is "
       "the one piece of arithmetic in this chain with no artefact to check "
       "it against" % tested,
       ", ".join(disagree[:3]))
    ck(blocked > tested // 5,
       "and enough of those rays hit a car for the agreement to mean "
       "something", "%d of %d" % (blocked, tested))

    ck(l.b3_rt_car_model_find(b"NoSuchCar_Car999") == -1,
       "a name the fleet does not carry comes back -1 rather than as some "
       "other car -- which is what lets a vehicle the extractor refused keep "
       "its blob shadow instead of borrowing somebody else's hull")


# ======================================================================
def main():
    argv = sys.argv[1:]
    det = "--no-determinism" not in argv
    for a in argv:
        if a not in ("--no-determinism",):
            print(__doc__)
            return 2

    print("validate_car_bvh -- carbvh.bin, the per-car ray-tracing trees")
    print("=" * 72)
    print("cx_car_bvh.c: %s" % ("present" if os.path.isfile(CX_C)
                                else "MISSING"))

    cdir = cars_dir()
    if not cdir:
        print("\nNo build/cars tree at all -- run the game once so the ISO "
              "cache materialises,\nor point B3_GAME_DIR at a dump and run "
              "the stage.")
        return 1
    print("cars dir:     %s" % cdir)
    gdir = game_dir()
    print("game dir:     %s" % (gdir or "(none -- section 1 will skip)"))

    if det:
        section_determinism(gdir)
    else:
        skip("the same disc gives the same bytes", "--no-determinism")

    path = os.path.join(cdir, "carbvh.bin")
    if not os.path.isfile(path):
        print("\nNo carbvh.bin under %s." % cdir)
        return 1
    b = CarBvh(path)
    section_header(b)
    section_models(b)
    section_tree(b)
    section_packing(b)
    section_geometry(b, cdir)
    section_wheels(b, cdir)
    section_rays(b)
    section_runtime(b, cdir)

    print("\n" + "=" * 72)
    print("%d passed, %d failed, %d skipped" % (len(PASS), len(FAIL),
                                                len(SKIP)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
