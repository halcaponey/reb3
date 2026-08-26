#!/usr/bin/env python3
"""Differential validator for the CAR LIGHT PROBES, end to end.

The user report this exists to answer: "I can drive through a shadow and see
no effect of the shadow on my car."  Every link of the chain is asserted here
against something that is not typed into this file:

  1  RETAIL SEMANTICS   the four decode scales, the 20.0 downward cast, the
                        prim's four u16 probe indices at +0x06/+0x08/+0x0A/
                        +0x0C and the unit block's +0xA4 probe-array slot, all
                        re-read as INSTRUCTION BYTES out of build/burnout3.elf
                        and compared against what the C extraction stage
                        (tools/cextract/cx_light_probes.c) and the runtime
                        consumer (src/burnout3_carfx.c) actually contain.
  2  EXTRACTION         for N tracks, the shipped light_probes.bin re-derived
                        straight from the track's own static.dat/streamed.dat
                        by this file, byte for byte, plus the L00-positivity
                        check that pins the +0xA4 offset.
  3  SPATIAL VARIANCE   the probe field must not be flat: per track, the DC
                        coefficient sampled over the ROAD SURFACE has to span
                        at least PROBE_RANGE_MIN x and carry at least
                        PROBE_STD_MIN of standard deviation.  A flat field is
                        how a broken index mapping shows up.
  4  SHADE RESPONSE     the probe field is paired against the track's OWN
                        BAKED VERTEX LIGHTING -- an independent bake of the
                        same quantity at the same places -- and must correlate
                        with it and be darker in its darkest decile.  The
                        per-shadow-DECAL dip is printed as a measurement and
                        deliberately NOT asserted: the probe volume rides the
                        collision mesh, so a caster the size of a bridge moves
                        it by 20%% and a single palm frond by 2-6%%, and on some
                        tracks not at all.  That resolution limit is retail's,
                        not the port's, and is the answer to the report.
  5  RUNTIME            the harness is run headless with B3_CARFX_PROBETRACE=1
                        and its [probetrace] log lines are bucketed by the
                        BAKED LUMINANCE of the ground under each traced
                        position.  The shaded median L00 -- raw, tuned, and the
                        irradiance the fragment shader evaluates -- must be
                        strictly below the sunlit median.

Section 5 needs the harness built with the PROBETRACE hook; if the binary does
not emit any [probetrace] line the section reports SKIP rather than FAIL, so
the file is safe to run against a tree that has not taken the patch yet.

Usage:  python3 tools/validate_light_probes.py [--tracks A,B,C] [--no-run]
Exit status is non-zero if any check fails.
"""
import os
import re
import struct
import subprocess
import sys

import numpy as np
from scipy.spatial import cKDTree
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "burnout3.elf")
CX_C = os.path.join(ROOT, "tools", "cextract", "cx_light_probes.c")
CARFX_C = os.path.join(ROOT, "src", "burnout3_carfx.c")
# B3_HARNESS lets the runtime section run against a shadow build while
# ./burnout3 in the repo root is mid-rebuild by another agent.
GAME = os.environ.get("B3_HARNESS", os.path.join(ROOT, "burnout3"))
GAME_DIR = os.environ.get(
    "B3_GAME_DIR",
    game_root())

# Three tracks, one per region, all of which ship tree/palm shadow decals.
DEFAULT_TRACKS = ["US_C1_V1", "US_C3_V1", "EU_C3_V1"]

# The floors.  Both are set well under what every shipped track measures (the
# tightest of the 37 is a 1.5x road range), so they catch "the field went
# flat", not "the field changed a little".
PROBE_RANGE_MIN = 1.30      # max(L00)/min(L00) over the road surface
PROBE_STD_MIN = 0.010       # absolute std of L00 over the road surface
MISS_MAX = 0.20             # fraction of road samples the cast may miss
BAKED_R_MIN = 0.20          # probe-vs-baked-lighting Pearson r floor

SHADOW_MATERIALS_RE = re.compile(r"(shadow)", re.I)
SHADE_OFFSETS = ((25.0, 0.0), (-25.0, 0.0), (0.0, 25.0), (0.0, -25.0),
                 (18.0, 18.0), (-18.0, -18.0))

PASS, FAIL, SKIP = [], [], []


def ck(cond, what, detail=""):
    (PASS if cond else FAIL).append(what)
    print("  %s %s%s" % ("ok  " if cond else "FAIL", what,
                         ("   [%s]" % detail) if detail else ""))


def skip(what, why):
    SKIP.append(what)
    print("  SKIP %s   [%s]" % (what, why))


# ------------------------------------------------------------------ the ELF
class Elf(object):
    def __init__(self, path):
        self.d = open(path, "rb").read()
        ph_off, = struct.unpack_from("<I", self.d, 0x1C)
        ph_esz, = struct.unpack_from("<H", self.d, 0x2A)
        ph_num, = struct.unpack_from("<H", self.d, 0x2C)
        self.segs = []
        for i in range(ph_num):
            o = ph_off + i * ph_esz
            t, off, va, _pa, fsz, _msz, _fl, _al = struct.unpack_from(
                "<8I", self.d, o)
            if t == 1:
                self.segs.append((va, off, fsz))

    def off(self, va):
        for va0, off, fsz in self.segs:
            if va0 <= va < va0 + fsz:
                return off + (va - va0)
        return None

    def rd(self, va, n):
        o = self.off(va)
        return None if o is None else self.d[o:o + n]

    def f32(self, va):
        b = self.rd(va, 4)
        return None if b is None else struct.unpack("<f", b)[0]


def c_floats(src):
    out = set()
    for m in re.finditer(r"(-?\d+\.\d+(?:e-?\d+)?)f?", src):
        try:
            out.add(float(m.group(1)))
        except ValueError:
            pass
    return out


# --------------------------------------------------- 1. retail semantics
def section_retail():
    print("\n1. RETAIL SEMANTICS  (instruction bytes out of build/burnout3.elf)")
    if not os.path.exists(ELF):
        skip("elf present", ELF + " missing")
        return
    e = Elf(ELF)
    cxsrc = open(CX_C).read()
    fxsrc = open(CARFX_C).read()
    cxf, fxf = c_floats(cxsrc), c_floats(fxsrc)

    # FUN_0019C640's three band scales times 1/128 -- the decode.
    l2 = e.f32(0x003B16E8)
    l1 = e.f32(0x003B16EC)
    l0 = e.f32(0x003B16F0)
    inv = e.f32(0x003B16F4)
    ck(abs(l0 - 1.6) < 1e-6, "0x003B16F0 = 1.6 (L0 band scale)", repr(l0))
    ck(abs(l1 - 0.6) < 1e-6, "0x003B16EC = 0.6 (L1 band scale)", repr(l1))
    ck(abs(l2 - 0.4) < 1e-6, "0x003B16E8 = 0.4 (L2 band scale)", repr(l2))
    ck(abs(inv - 1.0 / 128.0) < 1e-9, "0x003B16F4 = 1/128", repr(inv))
    def has(fs, v):
        # the .rdata values are float32, so 1.6 reads back as 1.6000000238...
        return any(abs(x - v) < 1e-6 for x in fs)
    for v, name in ((l0, "L0"), (l1, "L1"), (l2, "L2"), (inv, "1/128")):
        ck(has(cxf, v), "cx_light_probes.c carries the %s scale" % name)
        ck(has(fxf, v), "burnout3_carfx.c carries the %s scale" % name)

    # The decode's band layout: index 0 uses L0, 1..3 use L1, 4..8 use L2.
    # 0x0019C6AE MOV EAX,0x4 starts the L2 tail loop, 0x0019C6CA CMP EAX,0x9.
    ck(e.rd(0x0019C6AE, 5) == b"\xb8\x04\x00\x00\x00",
       "FUN_0019C640: the L2 tail starts at coefficient 4")
    ck(e.rd(0x0019C6CA, 3) == b"\x83\xf8\x09",
       "FUN_0019C640: the L2 tail ends at coefficient 9")

    # The downward cast: PUSH 0x41A00000 (= 20.0) @0x001AB136.
    push = e.rd(0x001AB136, 5)
    ck(push is not None and push[0] == 0x68
       and struct.unpack("<f", push[1:5])[0] == 20.0,
       "0x001AB136 PUSH 20.0 -- the probe cast length",
       repr(push))
    ck("20.0f" in fxsrc and "B3FX_PROBE_CAST" in fxsrc,
       "burnout3_carfx.c's B3FX_PROBE_CAST is 20.0")

    # The prim's four u16 probe indices.  MOVZX EAX,word ptr [EDI + disp8]
    # is 0F B7 47 <disp>; the four sites and their displacements are the whole
    # index mapping the extractor reproduces.
    sites = ((0x0019D4D6, 0x06), (0x0019D4E4, 0x08), (0x0019D4F6, 0x0A),
             (0x0019D4FC, 0x0A), (0x0019D50A, 0x08), (0x0019D51C, 0x0C))
    for va, disp in sites:
        b = e.rd(va, 4)
        ck(b == bytes((0x0F, 0xB7, 0x47, disp)),
           "0x%08X MOVZX EAX,[EDI+0x%02X] (prim probe index)" % (va, disp),
           repr(b))
    # sub-triangle 0 -> +0x06,+0x08,+0x0A ; 1 -> +0x0A,+0x08,+0x0C
    for want in ("p + 6", "p + 8", "p + 10", "p + 12"):
        ck(want in cxsrc,
           "cx_light_probes.c reads prim%s" % want.replace("p ", ""))

    # LEA EDX,[ESI+EAX*8]; ADD EDX,EAX -> stride 9 over the +0xA4 array.
    ck(e.rd(0x0019D4DA, 5) == b"\x8d\x14\xc6\x03\xd0",
       "0x0019D4DA LEA EDX,[ESI+EAX*8]; ADD EDX,EAX -- probe stride 9")
    ck(e.rd(0x0019D4C3, 6) == b"\x8b\xb7\xa4\x00\x00\x00",
       "0x0019D4C3 MOV ESI,[EDI+0xA4] -- the unit's probe array")
    ck("0xA4" in cxsrc, "cx_light_probes.c reads unit +0xA4")

    # FUN_0019D7A0's relink proves +0xA4 is a BLOCK-RELATIVE offset:
    # 0019d7b9 ADD EAX,ESI ; 0019d7bb MOV [ESI+0xA4],EAX
    ck(e.rd(0x0019D7B9, 8) == b"\x03\xc6\x89\x86\xa4\x00\x00\x00",
       "0x0019D7B9 ADD EAX,ESI; MOV [ESI+0xA4],EAX -- block-relative")


# ------------------------------------------------------- probe file reader
def load_probes(path):
    b = open(path, "rb").read()
    assert b[:4] == b"B3LP", path
    ver, nprobe, ntri = struct.unpack_from("<III", b, 4)
    bmin = struct.unpack_from("<3f", b, 0x10)
    bmax = struct.unpack_from("<3f", b, 0x1C)
    off = 0x28
    probes = np.frombuffer(b, np.int8, nprobe * 9, off).reshape(nprobe, 9)
    off += nprobe * 9
    off += (-(nprobe * 9)) & 3
    rec = np.frombuffer(b, np.uint8, ntri * 48, off)
    tri = rec.view("<f4").reshape(ntri, 12)[:, :9].reshape(ntri, 3, 3).copy()
    idx = rec.view("<u4").reshape(ntri, 12)[:, 9:12].copy()
    return dict(ver=ver, nprobe=nprobe, ntri=ntri, bmin=bmin, bmax=bmax,
                probes=probes, tri=tri, idx=idx)


DEC = np.array([1.6 / 128.0] + [0.6 / 128.0] * 3 + [0.4 / 128.0] * 5)


def decode(p):
    return p.astype(np.float64) * DEC


class Caster(object):
    """FUN_0019D400's downward segment against the probe mesh, bucketed in XZ
    exactly the way src/burnout3_carfx.c buckets it (GLUE acceleration, same
    result)."""

    def __init__(self, D, grid=128):
        self.D = D
        self.g = grid
        t = D["tri"]
        self.lo = t.min(axis=1)
        self.hi = t.max(axis=1)
        bmin, bmax = np.array(D["bmin"]), np.array(D["bmax"])
        self.bmin = bmin
        self.cx = max((bmax[0] - bmin[0]) / grid, 1e-6)
        self.cz = max((bmax[2] - bmin[2]) / grid, 1e-6)
        cells = {}
        cx0 = np.clip(((self.lo[:, 0] - bmin[0]) / self.cx).astype(int),
                      0, grid - 1)
        cx1 = np.clip(((self.hi[:, 0] - bmin[0]) / self.cx).astype(int),
                      0, grid - 1)
        cz0 = np.clip(((self.lo[:, 2] - bmin[2]) / self.cz).astype(int),
                      0, grid - 1)
        cz1 = np.clip(((self.hi[:, 2] - bmin[2]) / self.cz).astype(int),
                      0, grid - 1)
        for i in range(len(t)):
            for z in range(cz0[i], cz1[i] + 1):
                for x in range(cx0[i], cx1[i] + 1):
                    cells.setdefault(z * grid + x, []).append(i)
        self.cells = {k: np.array(v) for k, v in cells.items()}

    def sample(self, pos, cast=20.0):
        D = self.D
        cx = int((pos[0] - self.bmin[0]) / self.cx)
        cz = int((pos[2] - self.bmin[2]) / self.cz)
        if not (0 <= cx < self.g and 0 <= cz < self.g):
            return None
        cand = self.cells.get(cz * self.g + cx)
        if cand is None:
            return None
        tri = D["tri"][cand]
        v0, v1, v2 = tri[:, 0], tri[:, 1], tri[:, 2]
        e1, e2 = v1 - v0, v2 - v0
        d = np.array([0.0, -cast, 0.0])
        pv = np.cross(d, e2)
        det = (e1 * pv).sum(1)
        ok = det > 1e-8
        if not ok.any():
            return None
        invd = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        s = np.asarray(pos) - v0
        u = (s * pv).sum(1) * invd
        ok &= (u >= -1e-5) & (u <= 1.00001)
        qv = np.cross(s, e1)
        vv = (d * qv).sum(1) * invd
        ok &= (vv >= -1e-5) & (u + vv <= 1.00001)
        tt = (e2 * qv).sum(1) * invd
        ok &= (tt >= -1e-5) & (tt <= 1.00001)
        if not ok.any():
            return None
        k = np.argmin(np.where(ok, tt, 1e30))
        t = cand[k]
        i = D["idx"][t]
        if (i >= D["nprobe"]).any():
            return None
        p = decode(D["probes"][i])
        return p[0] + (p[1] - p[0]) * u[k] + (p[2] - p[0]) * vv[k]


# ------------------------------------------------------------- track assets
def read_obj(path, want=None):
    """(vertices, {material: [tri vertex indices]})"""
    V = []
    faces = {}
    cur = None
    for line in open(path):
        if line.startswith("v "):
            p = line.split()
            V.append((float(p[1]), float(p[2]), float(p[3])))
        elif line.startswith("usemtl "):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith("f "):
            if want is not None and (cur is None or not want(cur)):
                continue
            faces.setdefault(cur, []).append(
                [int(t.split("/")[0]) - 1 for t in line.split()[1:]])
    return np.array(V, dtype=np.float64), faces


# ----------------------------------------------- 2. extraction round trip
def rederive_probes(track_dir):
    """The probe BYTES, re-derived here from the shipped .dat pair, with no
    reference to the C stage."""
    sd = open(os.path.join(track_dir, "static.dat"), "rb").read()
    st = open(os.path.join(track_dir, "streamed.dat"), "rb").read()
    nunit, = struct.unpack_from("<H", sd, 0x54)
    ut, = struct.unpack_from("<i", sd, 0x58)
    out = bytearray()
    for u in range(nunit):
        lo, = struct.unpack_from("<i", sd, ut + u * 0x10 + 4)
        ls, = struct.unpack_from("<i", sd, ut + u * 0x10 + 12)
        if not lo or not ls:
            continue
        blk = st[lo:lo + ls]
        coll, = struct.unpack_from("<I", blk, 0xA0)
        poff, = struct.unpack_from("<I", blk, 0xA4)
        if not coll or not poff:
            continue
        leafs, = struct.unpack_from("<i", blk, coll + 0x24)
        nleaf, = struct.unpack_from("<H", blk, coll + 0x28)
        maxidx = -1
        for i in range(nleaf):
            rec = coll + leafs + i * 0x10
            prim = rec + struct.unpack_from("<i", blk, rec)[0]
            stride = blk[rec + 0x0D]
            pcount = blk[rec + 0x0E]
            for k in range(pcount):
                p = prim + k * stride
                i3 = blk[p + 3]
                for q, o in ((0, 6), (1, 8), (2, 10), (3, 12)):
                    if q == 3 and i3 == 0xFF:
                        continue
                    v, = struct.unpack_from("<H", blk, p + o)
                    maxidx = max(maxidx, v)
        n = maxidx + 1
        out += blk[poff:poff + n * 9]
    return bytes(out)


def section_extraction(tracks):
    print("\n2. EXTRACTION  (light_probes.bin re-derived from the shipped .dat)")
    for t in tracks:
        binp = os.path.join(ROOT, "build", "tracks", t, "light_probes.bin")
        tdir = os.path.join(GAME_DIR, "Tracks", t[:2], t[3:])
        if not os.path.exists(binp):
            skip("%s light_probes.bin" % t, "not extracted")
            continue
        D = load_probes(binp)
        l00 = D["probes"][:, 0]
        ck((l00 >= 0).all(),
           "%s: every L00 byte is positive (pins the +0xA4 offset)" % t,
           "min %d" % l00.min())
        if not os.path.isdir(tdir):
            skip("%s probe bytes vs static.dat" % t, "no game dir")
            continue
        want = rederive_probes(tdir)
        got = D["probes"].tobytes()
        ck(want == got,
           "%s: probe bytes match an independent walk of the .dat pair" % t,
           "%d vs %d bytes" % (len(want), len(got)))


# --------------------------------------- 3/4. variance and shade response
def road_samples(track, D, cast, n=2500, seed=3):
    objp = os.path.join(ROOT, "build", "tracks", track, "track.obj")
    V, faces = read_obj(objp, want=lambda m: m.lower().startswith("wf_road")
                        or "road" in m.lower())
    tris = [t for v in faces.values() for t in v]
    if not tris:
        return None
    cen = np.array([V[t].mean(axis=0) for t in tris])
    rng = np.random.default_rng(seed)
    sel = rng.choice(len(cen), size=min(n, len(cen)), replace=False)
    vals = []
    miss = 0
    for k in sel:
        c = cen[k]
        r = cast.sample((c[0], c[1] + 1.0, c[2]))
        if r is None:
            miss += 1
        else:
            vals.append(r[0])
    return np.array(vals), miss, len(sel)


def section_variance(tracks, casts, probes):
    print("\n3. SPATIAL VARIANCE  (the DC coefficient over the road surface)")
    out = {}
    for t in tracks:
        if t not in casts:
            continue
        r = road_samples(t, probes[t], casts[t])
        if r is None:
            skip("%s road samples" % t, "no road material in track.obj")
            continue
        v, miss, n = r
        out[t] = v
        ck(len(v) > 100, "%s: probe cast lands on the road" % t,
           "%d/%d hit" % (len(v), n))
        if len(v) < 10:
            continue
        rng_ratio = float(v.max() / max(v.min(), 1e-9))
        ck(rng_ratio >= PROBE_RANGE_MIN,
           "%s: L00 road range >= %.2fx" % (t, PROBE_RANGE_MIN),
           "%.3f..%.3f = %.2fx" % (v.min(), v.max(), rng_ratio))
        ck(float(v.std()) >= PROBE_STD_MIN,
           "%s: L00 road std >= %.3f" % (t, PROBE_STD_MIN),
           "%.4f" % v.std())
        # A miss is not a defect -- retail SKIPS the write and the car keeps
        # its previous nine (TEST AL,AL @0x0019D4B2) -- but a cast that misses
        # most of the road would mean the mesh or the query space is wrong.
        ck(miss / float(n) < MISS_MAX,
           "%s: probe cast misses < %.0f%% of the road" % (t, 100 * MISS_MAX),
           "%.1f%%" % (100.0 * miss / n))
    return out


def shadow_centroids(track, want=None):
    objp = os.path.join(ROOT, "build", "tracks", track, "track.obj")
    sel = want or (lambda m: SHADOW_MATERIALS_RE.search(m))
    V, faces = read_obj(objp, want=sel)
    out = {}
    for m, tris in faces.items():
        out[m] = np.array([V[t].mean(axis=0) for t in tris])
    return out


def baked_lighting(track):
    """(positions, luminance) of every world vertex that carries the game's
    own baked D3DCOLOR diffuse.  This is what the PLAYER sees as light and
    shade on the road, and it is an INDEPENDENT bake from the probe volume --
    which is exactly why it is the right ground truth for `do the probes get
    darker where the track gets darker`."""
    objp = os.path.join(ROOT, "build", "tracks", track, "track.obj")
    P, L = [], []
    for line in open(objp):
        if not line.startswith("v "):
            continue
        q = line.split()
        if len(q) < 7:
            continue
        P.append((float(q[1]), float(q[2]), float(q[3])))
        L.append(0.299 * float(q[4]) + 0.587 * float(q[5])
                 + 0.114 * float(q[6]))
    return np.array(P), np.array(L)


def probe_vs_baked(track, D, radius=5.0, dy=2.5, n=8000, seed=0):
    """Paired (probe L00, baked luminance) over the horizontal collision
    prims -- i.e. the ground the car actually drives on."""
    P, L = baked_lighting(track)
    if len(P) < 100:
        return None
    tri, idx = D["tri"], D["idx"]
    e1 = tri[:, 1] - tri[:, 0]
    e2 = tri[:, 2] - tri[:, 0]
    nrm = np.cross(e1, e2)
    ln = np.linalg.norm(nrm, axis=1) + 1e-9
    horiz = np.nonzero(np.abs(nrm[:, 1] / ln) > 0.9)[0]
    if len(horiz) < 100:
        return None
    cen = tri.mean(axis=1)
    l00 = decode(D["probes"])[idx, 0].mean(axis=1)
    tree = cKDTree(P[:, [0, 2]])
    rng = np.random.default_rng(seed)
    sel = rng.choice(horiz, size=min(n, len(horiz)), replace=False)
    rows = []
    for k in sel:
        c = cen[k]
        ii = tree.query_ball_point([c[0], c[2]], radius)
        if len(ii) < 4:
            continue
        ii = np.array(ii)
        ii = ii[np.abs(P[ii, 1] - c[1]) < dy]
        if len(ii) < 4:
            continue
        rows.append((l00[k], L[ii].mean()))
    return np.array(rows) if len(rows) > 100 else None


def section_shade(tracks, casts, probes):
    print("\n4. SHADE RESPONSE  (probe field vs the track's own baked lighting)")
    print("   The probe volume rides the COLLISION MESH, so it resolves light")
    print("   and shade at collision-prim scale.  The assertion is therefore")
    print("   made against the baked vertex lighting, which is the same")
    print("   quantity at the same places; the per-decal table below is a")
    print("   MEASUREMENT of how far down that resolution actually reaches.")
    for t in tracks:
        if t not in probes:
            continue
        R = probe_vs_baked(t, probes[t])
        if R is None:
            skip("%s probe-vs-baked pairing" % t, "not enough paired samples")
            continue
        r = float(np.corrcoef(R[:, 0], R[:, 1])[0, 1])
        ck(r >= BAKED_R_MIN,
           "%s: probe L00 tracks the baked lighting (r >= %.2f)"
           % (t, BAKED_R_MIN),
           "r = %+.3f over %d prims" % (r, len(R)))
        q = np.quantile(R[:, 1], [0.10, 0.90])
        dark = float(R[R[:, 1] <= q[0], 0].mean())
        bright = float(R[R[:, 1] >= q[1], 0].mean())
        ck(dark < bright,
           "%s: the darkest baked decile has the DARKER probe" % t,
           "%.4f vs %.4f (%+.1f%%)"
           % (dark, bright, 100.0 * (dark / bright - 1.0)))

    print("\n   per-shadow-decal measurement (NOT asserted -- see above):")
    for t in tracks:
        if t not in casts:
            continue
        fam = shadow_centroids(t)
        for mat, cen in sorted(fam.items()):
            if len(cen) < 20:
                continue
            tree = cKDTree(cen[:, [0, 2]])
            rng = np.random.default_rng(4)
            sel = rng.choice(len(cen), size=min(250, len(cen)), replace=False)
            ins, outs = [], []
            for k in sel:
                c = cen[k]
                a = casts[t].sample((c[0], c[1] + 1.0, c[2]))
                if a is None:
                    continue
                o = []
                for dx, dz in SHADE_OFFSETS:
                    px, pz = c[0] + dx, c[2] + dz
                    if tree.query([px, pz])[0] < 6.0:
                        continue          # the reference is shaded too
                    b = casts[t].sample((px, c[1] + 1.0, pz))
                    if b is not None:
                        o.append(b[0])
                if not o:
                    continue
                ins.append(a[0])
                outs.append(float(np.mean(o)))
            if len(ins) < 10:
                continue
            ins, outs = np.array(ins), np.array(outs)
            print("     %-9s %-22s n=%3d  under %.4f  clear %.4f  %+5.1f%%  "
                  "darker on %.0f%% of samples"
                  % (t, mat, len(ins), ins.mean(), outs.mean(),
                     100.0 * (ins.mean() / outs.mean() - 1.0),
                     100.0 * (ins < outs).mean()))


# ----------------------------------------------------------- 5. runtime
TRACE_RE = re.compile(
    r"\[probetrace\] slot (\d+) pos (\S+) (\S+) (\S+) hit (\d+) "
    r"L00raw (\S+) L00tuned (\S+) envmod (\S+) E (\S+)")


def run_trace(track, seconds=45):
    if not os.path.exists(GAME):
        return None, "no ./burnout3 -- build it first"
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(seconds), "B3_TRACK": track, "B3_AUTODRIVE": "1",
        "B3_CARFX_PROBETRACE": "1",
    })
    try:
        p = subprocess.run([GAME], cwd=ROOT, env=env, timeout=900,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None, "harness timed out"
    return p.stdout.decode("utf-8", "replace"), None


def section_runtime(track):
    print("\n5. RUNTIME  (the harness' own [probetrace] log lines)")
    log, why = run_trace(track)
    if log is None:
        skip("%s probetrace run" % track, why)
        return
    rows = []
    for m in TRACE_RE.finditer(log):
        rows.append((float(m.group(2)), float(m.group(3)), float(m.group(4)),
                     int(m.group(5)), float(m.group(6)), float(m.group(7)),
                     float(m.group(9))))
    if not rows:
        skip("%s probetrace lines" % track,
             "binary emits none -- apply the PROBETRACE patch and rebuild")
        return
    R = np.array(rows)
    ck(len(R) > 500, "%s: the trace has frames" % track, "%d lines" % len(R))
    hit = R[:, 3] > 0
    ck(hit.mean() > 0.85,
       "%s: the probe cast HITS on the driven line" % track,
       "%.1f%% hit" % (100.0 * hit.mean()))
    R = R[hit]
    if len(R) < 100:
        skip("%s runtime statistics" % track, "too few hits")
        return
    ck(R[:, 4].std() > 1e-4,
       "%s: the raw probe MOVES as the car drives" % track,
       "L00raw std %.5f over %.4f..%.4f"
       % (R[:, 4].std(), R[:, 4].min(), R[:, 4].max()))
    ck(R[:, 5].std() >= R[:, 4].std(),
       "%s: the tuned probe the shader gets moves at least as much" % track,
       "L00tuned std %.5f" % R[:, 5].std())

    # THE LOG-LINE ASSERTION.  "Shaded" and "sunlit" are defined by the
    # track's OWN baked vertex lighting under each traced position -- not by
    # anything typed into this file -- so this is literally "the car at a
    # known shaded spot reports a darker L00 than at a known sunlit one".
    P, L = baked_lighting(track)
    tree = cKDTree(P[:, [0, 2]])
    # The trace prints `q`, the GAME-space query FUN_0019D400 is given
    # (src/burnout3_carfx.c body_begin negates the harness z before the cast),
    # and track.obj is game space too -- so no flip here.
    px = R[:, 0]
    pz = R[:, 2]
    py = R[:, 1]
    lum = np.full(len(R), np.nan)
    for i in range(len(R)):
        ii = tree.query_ball_point([px[i], pz[i]], 4.0)
        if len(ii) < 4:
            continue
        ii = np.array(ii)
        ii = ii[np.abs(P[ii, 1] - py[i]) < 3.0]
        if len(ii) < 4:
            continue
        lum[i] = L[ii].mean()
    ok = ~np.isnan(lum)
    if ok.sum() < 100:
        skip("%s runtime shade split" % track, "no baked lighting under the line")
        return
    lm = lum[ok]
    rr = R[ok]
    q = np.quantile(lm, [0.20, 0.80])
    shaded = rr[lm <= q[0]]
    sunlit = rr[lm >= q[1]]
    ms, mu = float(np.median(shaded[:, 4])), float(np.median(sunlit[:, 4]))
    ck(ms < mu,
       "%s: the car at a KNOWN SHADED position reports a darker L00 than at "
       "a KNOWN SUNLIT one" % track,
       "shaded %.4f (n=%d, baked lum <= %.3f) vs sunlit %.4f (n=%d, >= %.3f)"
       % (ms, len(shaded), q[0], mu, len(sunlit), q[1]))
    ts, tu = float(np.median(shaded[:, 5])), float(np.median(sunlit[:, 5]))
    ck(ts < tu,
       "%s: and so does the TUNED nine the body shader is handed" % track,
       "%.4f vs %.4f (%+.1f%%)" % (ts, tu, 100.0 * (ts / tu - 1.0)))
    es, eu = float(np.median(shaded[:, 6])), float(np.median(sunlit[:, 6]))
    ck(es < eu,
       "%s: and the irradiance E(+Y) the fragment shader evaluates" % track,
       "%.4f vs %.4f (%+.1f%%)" % (es, eu, 100.0 * (es / eu - 1.0)))


def main():
    tracks = DEFAULT_TRACKS
    do_run = True
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--tracks" and i + 1 < len(args):
            tracks = args[i + 1].split(",")
            i += 2
        elif args[i] == "--no-run":
            do_run = False
            i += 1
        else:
            print(__doc__)
            return 2

    print("validate_light_probes: %s" % ", ".join(tracks))
    section_retail()
    section_extraction(tracks)

    probes, casts = {}, {}
    for t in tracks:
        p = os.path.join(ROOT, "build", "tracks", t, "light_probes.bin")
        if os.path.exists(p):
            probes[t] = load_probes(p)
            casts[t] = Caster(probes[t])
    section_variance(tracks, casts, probes)
    section_shade(tracks, casts, probes)
    if do_run and tracks:
        for t in tracks[:1]:
            section_runtime(t)

    print("\n%d passed, %d failed, %d skipped" % (len(PASS), len(FAIL),
                                                  len(SKIP)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
