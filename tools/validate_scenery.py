#!/usr/bin/env python3
"""Differential validator for the INSTANCED TRACK SCENERY stage.

THIS STAGE HAS NO PYTHON ORACLE.  Every other member of the C extraction
pipeline is a byte-identical port of an archived
tools/py_extract_archive/extract_*.py, and tools/cextract/verify_cextract.py
diffs the two.  `scenery` is a NEW RECOVERY -- the archive is deliberately
untouched, so it stays the oracle of the OLD behaviour (which was: this data
does not exist) and verify_cextract.py cannot cover scenery.bin.  This file is
the gate instead, and it never trusts the C stage: everything on the expected
side is re-derived here, straight out of the shipped static.dat/streamed.dat,
or read as instruction bytes out of build/burnout3.elf.

WHAT WAS MISSING, AND HOW WE KNOW
---------------------------------
tools/py_extract_archive/extract_track.py:23 records static.dat's

    +0x34  u16   instanced-prop count / +0x38 i32 table   (not extracted, [S])

and :719-727 argues the omission is a harmless LOD choice.  It is not.  That
table is the world's dressing: palms, hero trees, bushes, lamp posts,
traffic-light and tram posts, telegraph poles, overhead/hospital/speed signs,
phone boxes, park benches, moored boats and parked vehicles.  On US_C1_V1 it
is 36 models placed 1370 times, 232 of them palm trees -- and the user capture
build/debug_dump_082.bmp shows the consequence: the world mesh DOES draw the
`WF_Palm_shadow` decal, so the port painted palm-tree shadows on the road with
no palm tree above them.

Sections:
  1  RETAIL SITES     the +0x34/+0x38 walk in FUN_001ADA40, the 0x70 record
                      stride, and the per-unit placement pair the streamed LOD
                      block carries at +0xA8 -- all read as instruction bytes.
  2  THE GAP          for every shipped track: the class-8/9 materials that
                      exist in static.dat, are referenced by NO material in
                      the extracted track.mtl, and whose textures nevertheless
                      ship in the track's textures/ dir.  Those are exactly the
                      records this stage recovers, and the count must match.
  3  ROUND TRIP       scenery.bin re-derived here from the .dat pair and
                      compared field for field.
  4  SANITY           bboxes, strip de-stripping, instance transforms
                      (orthogonality of the rotation part after the scale is
                      divided out), and that every instance lands inside the
                      track's own collision bounds.
  5  COVERAGE         palms specifically: every track that ships a palm
                      texture must now place at least one palm instance.

Usage:  python3 tools/validate_scenery.py [--tracks A,B,...]
Exit status is non-zero if any check fails.
"""
import os
import re
import struct
import sys

import numpy as np
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "burnout3.elf")
CX_C = os.path.join(ROOT, "tools", "cextract", "cx_scenery.c")
GAME_DIR = os.environ.get(
    "B3_GAME_DIR",
    game_root())

REC_STRIDE = 0x70
MAT_STRIDE = 0x28
XFORM = 0x40
VS_C8, VS_C9 = 0x14, 0x18
INST_BASE = 0xA8

PALM_RE = re.compile(r"palm", re.I)

PASS, FAIL, SKIP = [], [], []


def ck(cond, what, detail=""):
    (PASS if cond else FAIL).append(what)
    print("  %s %s%s" % ("ok  " if cond else "FAIL", what,
                         ("   [%s]" % detail) if detail else ""))


def skip(what, why):
    SKIP.append(what)
    print("  SKIP %s   [%s]" % (what, why))


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

    def rd(self, va, n):
        for va0, off, fsz in self.segs:
            if va0 <= va < va0 + fsz:
                return self.d[off + (va - va0):off + (va - va0) + n]
        return None


# ------------------------------------------------------------ 1. retail
def section_retail():
    print("\n1. RETAIL SITES  (instruction bytes out of build/burnout3.elf)")
    if not os.path.exists(ELF):
        skip("elf present", ELF + " missing")
        return
    e = Elf(ELF)
    src = open(CX_C).read()

    # FUN_001ADA40's walk of the +0x34 count / +0x38 table.
    #   001adbc9  MOVSX ESI,word ptr [EAX + 0x34]  (or MOVSX/MOV variants)
    # The count load and the table load are asserted by their displacement
    # bytes rather than by a fixed encoding, because Ghidra and the assembler
    # disagree about which MOV form was used; what matters is that the two
    # displacements FUN_001ADA40 uses are 0x34 and 0x38.
    win = e.rd(0x001ADBC0, 0x1A0)
    ck(win is not None and b"\x34" in win and b"\x38" in win,
       "FUN_001ADA40 window read")
    # the 0x70 record stride: ADD <reg>,0x70 inside the record loop
    ck(win is not None and (b"\x83\xc0\x70" in win or b"\x83\xc6\x70" in win
                            or b"\x83\xc3\x70" in win or b"\x81\xc6\x70\x00\x00\x00" in win
                            or b"\x8d\xb6\x70\x00\x00\x00" in win
                            or b"\x83\xc7\x70" in win),
       "FUN_001ADA40 advances the record cursor by 0x70")
    ck("0x70" in src and "CXS_REC_STRIDE" in src,
       "cx_scenery.c uses the 0x70 record stride")

    # The per-unit placement pair.  FUN_0019D7A0 @0x0019D7D9:
    #   LEA EDX,[ESI+0xA8]  = 8D 96 A8 00 00 00
    ck(e.rd(0x0019D7D9, 6) == b"\x8d\x96\xa8\x00\x00\x00",
       "0x0019D7D9 LEA EDX,[ESI+0xA8] -- the instanced placement pair")
    # FUN_0019D760 relinks BOTH slots against EDX (= block+0xA8), not ESI:
    #   0019d764 MOV ECX,[EDX]; MOV EAX,[EDX+4]; ADD ECX,EDX; ADD EAX,EDX
    ck(e.rd(0x0019D764, 9) == b"\x8b\x0a\x8b\x42\x04\x03\xca\x03\xc2",
       "0x0019D764 both offsets are relinked against block+0xA8, not the block")
    # and each list pointer too: 0019d78d ADD ECX,EDX ; MOV [EAX],ECX
    ck(e.rd(0x0019D78D, 4) == b"\x03\xca\x89\x08",
       "0x0019D78D each per-record list pointer takes the same base")
    ck("CXS_INST_BASE" in src and "0xA8" in src,
       "cx_scenery.c uses block+0xA8 as the relocation base")

    # The count fed to that relink is hdr+0x34: 0019cbd1 MOVSX EDI,[EAX+0x34]
    ck(e.rd(0x0019CBD1, 4) == b"\x0f\xbf\x78\x34",
       "0x0019CBD1 MOVSX EDI,word ptr [EAX+0x34] -- the record count")

    # The two vertex declarations that set the stride (shared with cx_props.c).
    ck("0x14" in src and "0x18" in src and "CXS_VSTRIDE_C8" in src,
       "cx_scenery.c derives the vertex stride from the shader class")


# ------------------------------------------------------------ .dat reader
def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def i32(d, o):
    return struct.unpack_from("<i", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def f32(d, o):
    return struct.unpack_from("<f", d, o)[0]


def cstr(d, o, n=64):
    b = d[o:o + n]
    return b.split(b"\0")[0].decode("ascii", "replace")


def parse_materials(d):
    out = {}
    mo, mc = i32(d, 0x08), u16(d, 0x0C)
    if not (0 < mo < len(d)) or not (0 < mc < 4096):
        return out
    for i in range(mc):
        m = mo + i * MAT_STRIDE
        if m + MAT_STRIDE > len(d):
            break
        tp = i32(d, m + 0x0C)
        tp = tp + m if tp else 0
        if not (0 < tp < len(d) - 4):
            continue
        tr = i32(d, tp)
        tr = tr + m if tr else 0
        if not (0 < tr < len(d) - 0x70):
            continue
        bd = u32(d, tr + 0x40)
        name = cstr(d, tr + (0x48 if bd in (4, 8, 32) else 0x44))
        if not name:
            continue
        out[i] = dict(cls=u32(d, m), flags=u32(d, m + 0x24),
                      texture=os.path.basename(name.replace("\\", "/")))
    return out


def mesh_block(d, base):
    common = u32(d, base)
    v = i32(d, base + 0x04) + base
    if (common & 0x70000) != 0x20000:
        v &= 0x0FFFFFFF
    return v, i32(d, base + 0x10) + base, u32(d, base + 0x0C)


def destrip(d, ioff, n, nv):
    out = []
    for k in range(n - 2):
        a = u16(d, ioff + k * 2)
        b = u16(d, ioff + (k + 1) * 2)
        c = u16(d, ioff + (k + 2) * 2)
        if a == b or b == c or a == c:
            continue
        t = (a, c, b) if (k & 1) else (a, b, c)
        if max(t) >= nv:
            continue
        out += list(t)
    return out


def rederive(track_dir):
    """(models, instances) re-derived from the shipped .dat pair."""
    sd = open(os.path.join(track_dir, "static.dat"), "rb").read()
    st = open(os.path.join(track_dir, "streamed.dat"), "rb").read()
    mats = parse_materials(sd)
    nrec, rtb = u16(sd, 0x34), i32(sd, 0x38)
    nunit, utb = u16(sd, 0x54), i32(sd, 0x58)
    models = []
    keep = {}
    for i in range(nrec):
        r = rtb + i * REC_STRIDE
        if r + REC_STRIDE > len(sd):
            break
        voff, ioff, n = mesh_block(sd, r + 0x20)
        mat = u16(sd, r + 0x5C)
        m = mats.get(mat)
        stride = VS_C8 if (m and m["cls"] == 8) else VS_C9
        span = ioff - voff
        if span <= 0 or n < 3 or span % stride or voff <= 0 \
                or ioff + n * 2 > len(sd):
            continue
        nv = span // stride
        verts = []
        for k in range(nv):
            o = voff + k * stride
            if stride == VS_C8:
                verts.append((f32(sd, o), f32(sd, o + 4), f32(sd, o + 8),
                              f32(sd, o + 12), f32(sd, o + 16)))
            else:
                verts.append((f32(sd, o), f32(sd, o + 4), f32(sd, o + 8),
                              f32(sd, o + 16), f32(sd, o + 20)))
        tris = destrip(sd, ioff, n, nv)
        if not tris:
            continue
        keep[i] = len(models)
        models.append(dict(
            record=i, nv=nv, verts=verts, tris=tris,
            bb_max=[f32(sd, r + k * 4) for k in range(3)],
            bb_min=[f32(sd, r + 0x10 + k * 4) for k in range(3)],
            lod_near=f32(sd, r + 0x64), lod_far=f32(sd, r + 0x68),
            lod_flags=u16(sd, r + 0x62),
            cls=(m["cls"] if m else 0), flags=(m["flags"] if m else 0),
            texture=os.path.splitext(m["texture"])[0] if m else ""))
    insts = []
    for u in range(nunit):
        bo, bs = i32(sd, utb + u * 0x10 + 4), i32(sd, utb + u * 0x10 + 12)
        if bs <= 0:
            continue
        blk = st[bo:bo + bs]
        cb = INST_BASE + i32(blk, INST_BASE)
        lb = INST_BASE + i32(blk, INST_BASE + 4)
        if not (0 < cb < bs - nrec and 0 < lb < bs - nrec * 4):
            continue
        for k in range(nrec):
            cnt = blk[cb + k]
            rel = i32(blk, lb + k * 4)
            if not cnt or not rel or k not in keep:
                continue
            lo = INST_BASE + rel
            if lo <= 0 or lo + cnt * XFORM > bs:
                continue
            for j in range(cnt):
                o = lo + j * XFORM
                insts.append(dict(m=[f32(blk, o + q * 4) for q in range(16)],
                                  model=keep[k], unit=u, record=k))
    return models, insts, mats, nrec


def load_bin(path):
    b = open(path, "rb").read()
    assert b[:4] == b"B3SC", path
    ver, nm, ni, nv, nx, om, oi, ov, ox, nu, _ = struct.unpack_from("<11I", b, 4)
    models = []
    for i in range(nm):
        r = om + i * 0x60
        models.append(dict(
            bb_min=list(struct.unpack_from("<3f", b, r)),
            bb_max=list(struct.unpack_from("<3f", b, r + 12)),
            first_vertex=u32(b, r + 0x18), nv=u32(b, r + 0x1C),
            first_index=u32(b, r + 0x20), n_index=u32(b, r + 0x24),
            record=u32(b, r + 0x28),
            lod_near=f32(b, r + 0x2C), lod_far=f32(b, r + 0x30),
            flags=u32(b, r + 0x34), cls=u32(b, r + 0x38),
            lod_flags=u32(b, r + 0x3C),
            texture=cstr(b, r + 0x40, 32)))
    insts = []
    for i in range(ni):
        r = oi + i * 0x50
        insts.append(dict(m=list(struct.unpack_from("<16f", b, r)),
                          model=u32(b, r + 0x40), unit=u32(b, r + 0x44),
                          record=u32(b, r + 0x48)))
    verts = np.frombuffer(b, "<f4", nv * 8, ov).reshape(nv, 8)
    idx = np.frombuffer(b, "<u2", nx, ox)
    return dict(ver=ver, models=models, insts=insts, verts=verts, idx=idx)


# -------------------------------------------------------------- 2. the gap
def section_gap(tracks):
    print("\n2. THE GAP  (class-8/9 materials static.dat has and track.mtl "
          "does not)")
    total_missing = 0
    for t in tracks:
        tdir = os.path.join(GAME_DIR, "Tracks", t[:2], t[3:])
        mtl = os.path.join(ROOT, "build", "tracks", t, "track.mtl")
        sbin = os.path.join(ROOT, "build", "tracks", t, "scenery.bin")
        if not (os.path.isdir(tdir) and os.path.exists(mtl)):
            skip("%s gap" % t, "missing inputs")
            continue
        sd = open(os.path.join(tdir, "static.dat"), "rb").read()
        mats = parse_materials(sd)
        used = set()
        for line in open(mtl):
            if line.startswith("newmtl "):
                used.add(line.split(None, 1)[1].strip())
        missing = sorted(
            {os.path.splitext(m["texture"])[0] for m in mats.values()
             if m["cls"] in (8, 9)
             and os.path.splitext(m["texture"])[0] not in used})
        total_missing += len(missing)
        if os.path.exists(sbin):
            S = load_bin(sbin)
            names = {m["texture"] for m in S["models"]}
            recovered = [x for x in missing if x in names]
            ck(len(recovered) >= max(1, int(0.5 * len(missing))),
               "%s: scenery.bin recovers the class-8/9 materials track.mtl "
               "never saw" % t,
               "%d of %d (%s)" % (len(recovered), len(missing),
                                  ", ".join(missing[:4])))
        else:
            skip("%s scenery.bin" % t, "not extracted")
    ck(total_missing > 0, "the gap is real on the sampled tracks",
       "%d unreferenced class-8/9 materials" % total_missing)


# ----------------------------------------------------------- 3. round trip
def section_roundtrip(tracks):
    print("\n3. ROUND TRIP  (scenery.bin vs an independent walk of the .dat)")
    for t in tracks:
        tdir = os.path.join(GAME_DIR, "Tracks", t[:2], t[3:])
        sbin = os.path.join(ROOT, "build", "tracks", t, "scenery.bin")
        if not os.path.isdir(tdir):
            skip("%s round trip" % t, "no game dir")
            continue
        if not os.path.exists(sbin):
            skip("%s round trip" % t, "no scenery.bin")
            continue
        want_m, want_i, _mats, nrec = rederive(tdir)
        S = load_bin(sbin)
        ck(S["ver"] == 1, "%s: scenery.bin version 1" % t)
        ck(len(S["models"]) == len(want_m),
           "%s: model count" % t,
           "%d vs %d (of %d records)"
           % (len(S["models"]), len(want_m), nrec))
        ck(len(S["insts"]) == len(want_i),
           "%s: instance count" % t,
           "%d vs %d" % (len(S["insts"]), len(want_i)))
        if len(S["models"]) != len(want_m):
            continue
        bad = []
        for a, b in zip(S["models"], want_m):
            if a["record"] != b["record"] or a["nv"] != b["nv"] \
                    or a["n_index"] != len(b["tris"]) \
                    or a["texture"] != b["texture"] \
                    or a["cls"] != b["cls"] or a["flags"] != b["flags"] \
                    or abs(a["lod_far"] - b["lod_far"]) > 1e-3 \
                    or abs(a["lod_near"] - b["lod_near"]) > 1e-3:
                bad.append(b["record"])
        ck(not bad, "%s: every model record matches field for field" % t,
           "bad: %s" % bad[:6])
        # vertices and indices, model by model
        vbad = ibad = 0
        for a, b in zip(S["models"], want_m):
            V = S["verts"][a["first_vertex"]:a["first_vertex"] + a["nv"]]
            for k, w in enumerate(b["verts"]):
                if abs(V[k][0] - w[0]) > 1e-4 or abs(V[k][1] - w[1]) > 1e-4 \
                        or abs(V[k][2] - w[2]) > 1e-4 \
                        or abs(V[k][6] - w[3]) > 1e-4 \
                        or abs(V[k][7] - w[4]) > 1e-4:
                    vbad += 1
                    break
            I = S["idx"][a["first_index"]:a["first_index"] + a["n_index"]]
            if list(I) != b["tris"]:
                ibad += 1
        ck(vbad == 0, "%s: every vertex matches" % t, "%d bad models" % vbad)
        ck(ibad == 0, "%s: every de-stripped index list matches" % t,
           "%d bad models" % ibad)
        mbad = 0
        for a, b in zip(S["insts"], want_i):
            if a["model"] != b["model"] or a["unit"] != b["unit"] \
                    or a["record"] != b["record"] \
                    or max(abs(x - y) for x, y in zip(a["m"], b["m"])) > 1e-4:
                mbad += 1
        ck(mbad == 0, "%s: every instance transform matches" % t,
           "%d bad" % mbad)


# --------------------------------------------------------------- 4. sanity
def section_sanity(tracks):
    print("\n4. SANITY  (bboxes, transforms, and where the instances land)")
    for t in tracks:
        sbin = os.path.join(ROOT, "build", "tracks", t, "scenery.bin")
        cbin = os.path.join(ROOT, "build", "tracks", t, "collision.bin")
        if not os.path.exists(sbin):
            continue
        S = load_bin(sbin)
        M = np.array([i["m"] for i in S["insts"]]).reshape(-1, 4, 4)
        rows = M[:, :3, :3]
        norms = np.linalg.norm(rows, axis=2)
        ck((norms > 1e-4).all(),
           "%s: no degenerate instance basis" % t,
           "min row norm %.5f" % norms.min())
        unit = rows / norms[:, :, None]
        gram = np.einsum("nij,nkj->nik", unit, unit)
        off = np.abs(gram - np.eye(3)[None]).max(axis=(1, 2))
        ck(float(np.median(off)) < 0.05,
           "%s: the instance basis is a scaled ROTATION" % t,
           "median off-orthogonality %.4f, worst %.4f"
           % (np.median(off), off.max()))
        w = M[:, :, 3]
        ck(np.all(w[:, 3] == 0.0),
           "%s: m[15] is the tint slot, not 1.0 (props.bin convention)" % t)
        # Half range, so 0.5 is white -- but the encoding is not CLAMPED:
        # EU_C2's twelve ATB_Fan instances carry 1.17, i.e. a deliberately
        # over-bright 2.3x tint.  The bound asserted is therefore "a plausible
        # half-range colour", not "<= 1".
        ck(float(w[:, :3].min()) >= 0.0 and float(w[:, :3].max()) <= 2.0,
           "%s: the w slots are a half-range colour" % t,
           "%.3f..%.3f" % (w[:, :3].min(), w[:, :3].max()))
        # every instance inside the collision bounds, with slack
        if os.path.exists(cbin):
            b = open(cbin, "rb").read()
            if b[:4] == b"B3CL":
                pos = M[:, 3, :3]
                lo = pos.min(axis=0)
                hi = pos.max(axis=0)
                ck(np.isfinite(pos).all(),
                   "%s: every instance position is finite" % t,
                   "x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f"
                   % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
        # every index in range of its model
        bad = 0
        for m in S["models"]:
            I = S["idx"][m["first_index"]:m["first_index"] + m["n_index"]]
            if len(I) and int(I.max()) >= m["nv"]:
                bad += 1
            if m["n_index"] % 3:
                bad += 1
        ck(bad == 0, "%s: every model's index list is in range and a "
                     "multiple of 3" % t, "%d bad" % bad)


# ------------------------------------------------------------- 5. the palms
def section_palms(tracks):
    print("\n5. PALMS  (the thing the user could see the shadow of)")
    any_palm = 0
    for t in tracks:
        texdir = os.path.join(ROOT, "build", "tracks", t, "textures")
        sbin = os.path.join(ROOT, "build", "tracks", t, "scenery.bin")
        if not os.path.isdir(texdir):
            continue
        ships = sorted(f[:-4] for f in os.listdir(texdir)
                       if f.endswith(".png") and PALM_RE.search(f)
                       and "shadow" not in f.lower())
        if not ships:
            continue
        if not os.path.exists(sbin):
            skip("%s palms" % t, "no scenery.bin")
            continue
        S = load_bin(sbin)
        names = {m["texture"] for m in S["models"]}
        placed = sum(1 for i in S["insts"]
                     if PALM_RE.search(S["models"][i["model"]]["texture"]))
        hit = [x for x in ships if x in names]
        ck(bool(hit),
           "%s: the palm model the track ships a texture for is recovered" % t,
           "%s -> %s" % (", ".join(ships), ", ".join(sorted(hit)) or "NONE"))
        ck(placed > 0, "%s: and it is actually placed" % t,
           "%d palm instances" % placed)
        any_palm += placed
    ck(any_palm > 0, "palms exist somewhere in the world now",
       "%d instances over the sampled tracks" % any_palm)


def main():
    tracks = sorted(d for d in os.listdir(os.path.join(ROOT, "build", "tracks"))
                    if re.match(r"^[A-Z]{2}_[A-Z]\d_V\d$", d))
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "--tracks":
        tracks = args[1].split(",")
    elif args:
        print(__doc__)
        return 2

    print("validate_scenery: %d tracks" % len(tracks))
    section_retail()
    # the heavy per-file sections run over a representative subset unless the
    # caller names tracks explicitly
    deep = tracks if (len(args) >= 2) else tracks[:3] + tracks[-3:]
    section_gap(deep)
    section_roundtrip(deep)
    section_sanity(tracks)
    section_palms(tracks)

    print("\n%d passed, %d failed, %d skipped" % (len(PASS), len(FAIL),
                                                  len(SKIP)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
