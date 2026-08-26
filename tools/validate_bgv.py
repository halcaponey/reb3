#!/usr/bin/env python3
"""Round-trip validator for the .bgv exporter (tools/blender/bgv_write.py).

Deliberately does NOT need Blender: the exporter's byte packing lives in
tools/blender/bgv_write.py and the addon is a thin Blender-side wrapper, so
everything that can be wrong about the FILE can be tested here.

The parser in this file is written independently, straight from the
recovered spec (see bgv_write.py's module docstring for the evidence), so
"the writer agrees with the reader" is a real cross-check and not the same
code talking to itself.  Section 4 additionally transcribes the game's own
relocation pass, FUN_000310f0 + FUN_00031010 + FUN_00030120, and runs it
over the bytes the writer produced -- that is the closest thing to "would
the retail loader accept this file" that can be asserted offline.

  python3 tools/validate_bgv.py [section ...]

Sections: spec strip axis real synth relink port addon.  With no arguments
it runs all of them.  Exit code 0 iff every check passed.
"""

import glob
import math
import os
import random
import struct
import sys
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "blender"))
import bgv_write as W                                        # noqa: E402

PASS = 0
FAIL = 0
_FAILURES = []

#: Where the extracted Xbox disc image lives.  tools/extract_bgv.py points
#: at the same directory.
PVEH = game_path('pveh')


def check(cond, what, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        _FAILURES.append("%s  %s" % (what, detail))
        print("  FAIL %s  %s" % (what, detail))
    return bool(cond)


def note(msg):
    print("  %s" % msg)


# ===========================================================================
# An INDEPENDENT reader, written from the spec rather than from bgv_write.py
# ===========================================================================
class IndepBgv(object):
    pass


def indep_parse(data):
    """Parse a .bgv using only the recovered field table.

    header    +0x00 u32 version | +0x08 u32 file size | +0x0C u8 panels
              +0x0D u8 wheels | +0x4C u32[5] LOD offsets | +0x60 matdir
    section   S+0x00 u32[18] part offsets | S+0x4C pool0 rel S
              S+0x58 pool1 rel S | S+0x60 embedded part (recs rel S+0x60)
    part      +0x00 s8 count | +0x01 s8 glass hint | +0x04 u32 recs rel
    record    0x1C: +0x04/+0x08 f32 | +0x0C idx rel rec | +0x10 u16 count
              +0x14 u32 shader | +0x18 u16 mask | +0x1A u8 tex
    vertex    0x18: f32[3] pos | u32 NORMPACKED3 | f32[2] uv
    skin      0x08: u8[4] blend indices | u8[4] weights
    """
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]     # noqa: E731
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]     # noqa: E731
    s8 = lambda o: struct.unpack_from("<b", data, o)[0]      # noqa: E731

    m = IndepBgv()
    m.size = len(data)
    m.version = u32(0x00)
    m.declared_size = u32(0x08)
    m.num_panels = data[0x0C]
    m.num_wheels = data[0x0D]
    m.kind = u32(0x10)
    m.wheel_radius = struct.unpack_from("<f", data, 0x18)[0]
    m.matdir = u32(0x60)
    m.bbmax = struct.unpack_from("<4f", data, 0xE80)
    m.bbmin = struct.unpack_from("<4f", data, 0xE90)
    m.hull = bytes(data[0x1060:0x1060 + 0x600])
    m.panel_kinds = [struct.unpack_from("<i", data, 0xAC4 + 4 * i)[0]
                     for i in range(m.num_panels)]
    m.wheel_mats = [struct.unpack_from("<16f", data, 0xB80 + 0x40 * i)
                    for i in range(m.num_wheels)]
    m.panel_mats = [struct.unpack_from("<16f", data, 0xD00 + 0x40 * i)
                    for i in range(m.num_panels)]

    def part(p0, rec_base):
        cnt = s8(p0)
        arr = rec_base + u32(p0 + 4)
        recs = []
        for i in range(cnt):
            r = arr + i * 0x1C
            n = u16(r + 0x10)
            src = r + u32(r + 0x0C)
            recs.append({
                "idx": list(struct.unpack_from("<%dH" % n, data, src)) if n
                       else [],
                "mask": u16(r + 0x18),
                "tex": data[r + 0x1A],
                "shader": u32(r + 0x14),
                "a0": struct.unpack_from("<f", data, r + 0x04)[0],
                "a1": struct.unpack_from("<f", data, r + 0x08)[0],
                "u00": u32(r), "u12": u16(r + 0x12), "u1b": data[r + 0x1B],
            })
        return {"hint": s8(p0 + 1), "rel": u32(p0 + 4), "recs": recs}

    m.sections = []
    for si in range(5):
        S = u32(0x4C + 4 * si)
        if S == 0:
            m.sections.append(None)
            continue
        sec = {"S": S, "slots": [None] * 18,
               "vb": u32(S + 0x48), "vb2": u32(S + 0x54),
               "u50": u32(S + 0x50), "u5c": u32(S + 0x5C)}
        for sl in range(18):
            off = u32(S + 4 * sl)
            if off:
                sec["slots"][sl] = part(S + off, S + off)
        sec["emb"] = part(S + 0x60, S + 0x60)

        def maxidx(parts):
            mx = -1
            for p in parts:
                if p is None:
                    continue
                for r in p["recs"]:
                    if r["idx"]:
                        mx = max(mx, max(r["idx"]))
            return mx

        nvert = maxidx(sec["slots"] + [sec["emb"]]) + 1
        nskin = maxidx(sec["slots"]) + 1
        p0 = S + u32(S + 0x4C)
        p1 = S + u32(S + 0x58)
        sec["nvert"], sec["nskin"] = nvert, nskin
        sec["pool0"], sec["pool1"] = p0, p1
        sec["verts"] = []
        for i in range(nvert):
            o = p0 + i * 0x18
            x, y, z = struct.unpack_from("<3f", data, o)
            u, v = struct.unpack_from("<2f", data, o + 0x10)
            sec["verts"].append((x, y, z, u32(o + 0x0C), u, v))
        sec["skins"] = [bytes(data[p1 + i * 8:p1 + i * 8 + 8])
                        for i in range(nskin)]
        m.sections.append(sec)
    return m


def indep_fields(m):
    """A comparable tuple of everything indep_parse recovered."""
    # m.declared_size and the section offsets are DERIVED (the writer
    # recomputes both), so they are compared separately, not here.
    out = [m.version, m.num_panels, m.num_wheels, m.kind,
           m.bbmax, m.bbmin, m.hull, tuple(m.panel_kinds),
           tuple(m.wheel_mats), tuple(m.panel_mats)]
    for sec in m.sections:
        if sec is None:
            out.append(None)
            continue
        parts = []
        for sl, p in enumerate(sec["slots"] + [sec["emb"]]):
            if p is None:
                parts.append(None)
                continue
            parts.append((p["hint"], tuple(
                (tuple(r["idx"]), r["mask"], r["tex"], r["shader"],
                 r["a0"], r["a1"], r["u00"], r["u12"], r["u1b"])
                for r in p["recs"])))
        out.append((tuple(parts), tuple(sec["verts"]), tuple(sec["skins"]),
                    sec["vb"], sec["vb2"], sec["u50"], sec["u5c"]))
    return tuple(out)


# ===========================================================================
# The game's relocation pass, transcribed
# ===========================================================================
def simulate_relink(data):
    """Replay FUN_000310f0 / FUN_00031010 / FUN_00030120 over `data` with a
    base of 0 and assert that every pointer it forms lands inside the file.

    That is exactly the walk the retail loader performs on a freshly read
    buffer, so a file that survives it without a stray pointer is one the
    loader will not fault on.  Returns the number of pointers checked."""
    n = len(data)
    seen = 0

    def u32(o):
        if o + 4 > n:
            raise AssertionError("read past EOF at 0x%X" % o)
        return struct.unpack_from("<I", data, o)[0]

    def s8(o):
        return struct.unpack_from("<b", data, o)[0]

    def ptr(o, base=0):
        nonlocal seen
        v = u32(o)
        if v == 0:
            return 0
        p = v + base
        if not (0 <= p < n):
            raise AssertionError("pointer at +0x%X -> 0x%X outside [0,0x%X)"
                                 % (o, p, n))
        seen += 1
        return p

    def relink_records(p0, rec_base):
        cnt = s8(p0)
        if cnt < 0:
            raise AssertionError("negative record count at 0x%X" % p0)
        arr = ptr(p0 + 4, rec_base)
        if cnt and arr + cnt * 0x1C > n:
            raise AssertionError("record array at 0x%X overruns EOF" % arr)
        for i in range(cnt):
            r = arr + i * 0x1C
            src = ptr(r + 0x0C, r)
            cnt_i = struct.unpack_from("<H", data, r + 0x10)[0]
            if src + 2 * cnt_i > n:
                raise AssertionError("index stream at 0x%X overruns EOF" % src)

    # FUN_000310f0, in order
    md = ptr(0x60)                       # material dir + FUN_001c8e20
    if md:
        # FUN_001c8e20 is entirely self-relative; just bound-check its head.
        if md + 0x6A > n:
            raise AssertionError("material directory head overruns EOF")
    for i in range(5):                   # the 5 LOD slots
        S = ptr(0x4C + 4 * i)
        if not S:
            continue
        # FUN_00031010: slot 0 (+FUN_00030120), slots 1..6, 7..9, 10..17
        for sl in range(18):
            p0 = ptr(S + 4 * sl, S)
            if p0:
                relink_records(p0, p0)
        if u32(S + 0x4C) + S >= n:
            raise AssertionError("stream 0 pool outside the file")
        if u32(S + 0x58) + S >= n:
            raise AssertionError("stream 1 pool outside the file")
        relink_records(S + 0x60, S + 0x60)
    for off in (0x64, 0x68):             # the two standalone part objects
        p0 = ptr(off)
        if p0:
            relink_records(p0, p0)
    for i in range(18):                  # 3 groups of 6
        ptr(0x1664 + 4 * i)
    ptr(0x16C4)
    a = ptr(0x16C0)
    if a:
        cnt = s8(0x16C8)
        for i in range(cnt):
            ptr(a + i * 0xC + 8)
    for i in range(2):                   # FUN_00159470 pair
        ptr(0x16DC + 4 * i)
    return seen


# ===========================================================================
# Sections
# ===========================================================================
def sec_spec():
    """Constants and packing primitives."""
    print("\n1. spec constants and primitives")
    check(W.HEADER_SIZE == 0x16F0, "header size 0x16F0",
          "[C] all 67 shipped files put section 0 there")
    check(W.RECORD_SIZE == 0x1C and W.PART_HDR_SIZE == 0x10,
          "record 0x1C / part header 0x10")
    check(W.VERTEX_STRIDE == 0x18 and W.SKIN_STRIDE == 8,
          "stream strides 0x18 / 8", "[C] FUN_000315C0")

    # NORMPACKED3 round trip over the whole representable grid, sparsely.
    bad = 0
    for _ in range(4000):
        xi = random.randint(-1023, 1023)
        yi = random.randint(-1023, 1023)
        zi = random.randint(-511, 511)
        w = (xi & 0x7FF) | ((yi & 0x7FF) << 11) | ((zi & 0x3FF) << 22)
        x, y, z = W.unpack_normal(w)
        if W.pack_normal(x, y, z) != w:
            # only legitimate when the vector needed renormalising
            ln = math.sqrt(x * x + y * y + z * z)
            if abs(ln - 1.0) < 1e-6:
                bad += 1
    check(bad == 0, "pack_normal is the exact inverse of unpack_normal for "
                    "unit vectors", "%d mismatches" % bad)

    err = 0.0
    for _ in range(4000):
        v = [random.gauss(0, 1) for _ in range(3)]
        ln = math.sqrt(sum(x * x for x in v)) or 1.0
        v = [x / ln for x in v]
        d = W.unpack_normal(W.pack_normal(*v))
        err = max(err, max(abs(a - b) for a, b in zip(v, d)))
    check(err < 2e-3, "packed normal quantisation error < 2e-3",
          "max %.2e (11/11/10 bits)" % err)


def sec_strip():
    """Triangle-strip encode/decode."""
    print("\n2. triangle strips (D3DPT_TRIANGLESTRIP, primitive 6)")
    # the decoder must match tools/extract_bgv.py exactly
    s = [0, 1, 2, 3, 4]
    check(W.strip_to_triangles(s) == [(0, 1, 2), (1, 3, 2), (2, 3, 4)],
          "strip decode matches tools/extract_bgv.py",
          str(W.strip_to_triangles(s)))

    rng = random.Random(0x8664)
    worst = {False: 0.0, True: 0.0}
    for trial in range(400):
        nv = rng.randint(3, 40)
        tris = []
        for _ in range(rng.randint(1, 60)):
            a = rng.randrange(nv)
            b = rng.randrange(nv)
            c = rng.randrange(nv)
            if a == b or b == c or a == c:
                continue
            t = (a, b, c)
            if t not in tris:
                tris.append(t)
        if not tris:
            continue
        for reorder in (False, True):
            strip = W.triangles_to_strip(tris, reorder=reorder)
            back = W.strip_to_triangles(strip)
            if sorted(map(_norm, back)) != sorted(map(_norm, tris)):
                check(False, "strip round trip trial %d (reorder=%s)"
                             % (trial, reorder),
                      "%d in, %d out" % (len(tris), len(back)))
                return
            if not reorder and any(
                    _norm(a) != _norm(b) for a, b in zip(tris, back)):
                check(False, "reorder=False must preserve triangle order",
                      "trial %d" % trial)
                return
            worst[reorder] = max(worst[reorder],
                                 len(strip) / float(3 * len(tris)))
    check(True, "400 random meshes survive triangles_to_strip -> "
                "strip_to_triangles unchanged, in both ordering modes")
    note("worst-case strip expansion vs a raw triangle list: "
         "%.2fx ordered, %.2fx reordered" % (worst[False], worst[True]))

    ribbon = []
    for i in range(0, 30, 2):
        ribbon.append((i, i + 1, i + 2))
        ribbon.append((i + 1, i + 3, i + 2))
    strip = W.triangles_to_strip(ribbon)
    check(len(strip) == len(ribbon) + 2,
          "a real strip compresses to n+2 indices",
          "%d indices for %d triangles" % (len(strip), len(ribbon)))

    # Re-stripify REAL car geometry: decode every shipped strip, re-encode,
    # and require the triangles back out unchanged.  This is the case an
    # export actually hits, and it also measures how much bigger the
    # exporter's index streams are than the ones the shipping tool made.
    car = os.path.join(PVEH, "COMP", "Car1.bgv")
    if not os.path.exists(car):
        note("SKIP the real-geometry restrip check: %s not present" % car)
        return
    m = W.read_bgv_file(car)
    src_n = 0
    out_n = {False: 0, True: 0}
    mismatch = 0
    for sec in m.sections:
        if sec is None:
            continue
        for _, part in sec.parts():
            for r in part.records:
                tris = r.triangles
                if not tris:
                    continue
                src_n += len(r.indices)
                for reorder in (False, True):
                    strip = W.triangles_to_strip(tris, reorder=reorder)
                    back = W.strip_to_triangles(strip)
                    if sorted(map(_norm, back)) != sorted(map(_norm, tris)):
                        mismatch += 1
                    out_n[reorder] += len(strip)
    check(mismatch == 0,
          "every record of COMP/Car1 re-stripifies to the same triangles",
          "%d records changed" % mismatch)
    note("re-stripified index count %d ordered / %d reordered vs the "
         "shipped %d (%.2fx / %.2fx)"
         % (out_n[False], out_n[True], src_n,
            out_n[False] / float(src_n), out_n[True] / float(src_n)))


def _rot(t):
    a, b, c = t
    return ((a, b, c), (b, c, a), (c, a, b))


def _norm(t):
    """Orientation-preserving canonical form: the smallest rotation."""
    return min(_rot(t))


def sec_axis():
    """Axis convention, derived and then checked against real car data."""
    print("\n3. axis convention (.bgv = GAME space, LH, +X right +Y up "
          "+Z nose)")
    for nose in W.AXIS_MODES:
        B = W.axis_matrix(nose)
        check(abs(W.axis_det(B) + 1.0) < 1e-12,
              "nose %s: det(basis) == -1 (Blender RH -> .bgv LH)" % nose,
              "det %.3f" % W.axis_det(B))
        check(W.winding_flipped(B),
              "nose %s: exporter must reverse winding" % nose)

    B = W.axis_matrix("+Y")
    check(W.blender_to_bgv((1, 0, 0), "+Y") == (1.0, 0.0, 0.0),
          "nose +Y: Blender +X (the car's right) -> .bgv +X")
    check(W.blender_to_bgv((0, 0, 1), "+Y") == (0.0, 1.0, 0.0),
          "nose +Y: Blender +Z (up) -> .bgv +Y")
    check(W.blender_to_bgv((0, 1, 0), "+Y") == (0.0, 0.0, 1.0),
          "nose +Y: Blender +Y (the nose) -> .bgv +Z")
    check(W.blender_to_bgv((1, 0, 0), "-Y") == (-1.0, 0.0, 0.0),
          "nose -Y: the car's right is Blender -X")

    # A reversed winding under a det=-1 basis must keep the face pointing
    # the same way -- which is the whole reason the flip exists.
    tri = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)]
    n_bl = _cross(_sub(tri[1], tri[0]), _sub(tri[2], tri[0]))
    t = [W.apply_axis(B, p) for p in tri]
    n_naive = _cross(_sub(t[1], t[0]), _sub(t[2], t[0]))
    n_flip = _cross(_sub(t[2], t[0]), _sub(t[1], t[0]))
    n_want = W.apply_axis(B, n_bl)
    check(_dot(n_flip, n_want) > 0 > _dot(n_naive, n_want),
          "reversing winding preserves the facing; not reversing inverts it")

    # And the real data agrees: COMP/Car1's kind-0 panel (RIGHT DOOR, per
    # src/burnout3_panels.h:109) sits at +x and kind 1 (LEFT DOOR) at -x.
    car = os.path.join(PVEH, "COMP", "Car1.bgv")
    if not os.path.exists(car):
        note("SKIP the real-data axis check: %s not present" % car)
        return
    m = W.read_bgv_file(car)
    kinds = m.panel_kinds()
    if 0 in kinds and 1 in kinds:
        r = m.panel_matrix(kinds.index(0))
        l = m.panel_matrix(kinds.index(1))
        check(r[12] > 0 > l[12],
              "kind 0 (RIGHT DOOR) pivot is at +X, kind 1 (LEFT DOOR) at -X",
              "right x=%.3f  left x=%.3f" % (r[12], l[12]))
    # bonnet (kind 4) forward of the boot (kind 5) => +Z is the nose
    if 4 in kinds and 5 in kinds:
        b = m.panel_matrix(kinds.index(4))
        h = m.panel_matrix(kinds.index(5))
        check(b[14] > h[14],
              "kind 4 (BONNET) is at greater Z than kind 5 (BOOT) => "
              "+Z is the nose",
              "bonnet z=%.3f  boot z=%.3f" % (b[14], h[14]))
    # the body box is taller in +Y than -Y: +Y is up
    check(m.bbmax()[1] > 0 > m.bbmin()[1] and
          m.bbmax()[1] > abs(m.bbmin()[1]),
          "body box straddles the ground with more above than below "
          "=> +Y is up",
          "max.y %.3f  min.y %.3f" % (m.bbmax()[1], m.bbmin()[1]))

    # winding vs stored normals, on the real highest-LOD mesh
    sec = max((s for s in m.sections if s), key=lambda s: len(s.verts))
    acc = area = 0.0
    ntri = 0
    for r in sec.embedded.records:
        for a, b, c in r.triangles:
            pa, pb, pc = sec.verts[a], sec.verts[b], sec.verts[c]
            cr = _cross(_sub(pb[:3], pa[:3]), _sub(pc[:3], pa[:3]))
            ar = math.sqrt(_dot(cr, cr))
            if ar < 1e-9:
                continue
            vn = (0.0, 0.0, 0.0)
            for vi in (a, b, c):
                vn = _add(vn, W.unpack_normal(sec.verts[vi][3]))
            ln = math.sqrt(_dot(vn, vn)) or 1.0
            acc += ar * _dot([x / ar for x in cr], [x / ln for x in vn])
            area += ar
            ntri += 1
    check(area > 0 and acc / area > 0.9,
          "right-handed cross(b-a, c-a) of a decoded .bgv triangle agrees "
          "with the stored NORMPACKED3 normals",
          "area-weighted mean dot %.4f over %d triangles" % (acc / area, ntri))


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def sec_real():
    """Real shipped .bgv files: independent parse, re-export, compare."""
    print("\n4. real .bgv assets")
    files = sorted(glob.glob(os.path.join(PVEH, "*", "*.bgv")))
    if not files:
        note("SKIP: no .bgv assets found under %s" % PVEH)
        note("      (nothing was fabricated to stand in for them)")
        return
    note("%d shipped player .bgv files under %s" % (len(files), PVEH))

    agree = byte_exact = fresh_exact = 0
    residual_total = 0
    grew = []
    errs = []
    for path in files:
        raw = open(path, "rb").read()
        name = "/".join(path.split(os.sep)[-2:])
        try:
            mine = W.read_bgv(raw)
            theirs = indep_parse(raw)
        except Exception as exc:                            # noqa: BLE001
            errs.append((name, "parse: %s" % exc))
            continue

        # (a) the two readers must see the same thing
        if _cross_check(mine, theirs):
            agree += 1
        else:
            errs.append((name, "readers disagree"))

        residual_total += sum(len(c) for _, c in mine.residual)

        # (b) structural re-serialisation at the donor's own offsets must
        #     reproduce the file BYTE FOR BYTE
        out = W.write_bgv(mine, preserve_layout=True)
        if out == raw:
            byte_exact += 1
        else:
            diff = next((i for i in range(min(len(out), len(raw)))
                         if out[i] != raw[i]), min(len(out), len(raw)))
            errs.append((name, "byte compare: first difference at 0x%X "
                               "(len %d vs %d)" % (diff, len(out), len(raw))))

        # (c) re-serialisation with a FRESH layout -- every pointer, every
        #     offset and the file size recomputed from nothing -- must give
        #     a file the independent reader sees as field-identical
        out2 = W.write_bgv(mine, preserve_layout=False)
        try:
            regot = indep_parse(out2)
            if regot.declared_size != len(out2):
                errs.append((name, "fresh layout: +0x08 != file length"))
            elif indep_fields(regot) == indep_fields(theirs):
                fresh_exact += 1
            else:
                errs.append((name, "fresh layout: field mismatch"))
        except Exception as exc:                            # noqa: BLE001
            errs.append((name, "fresh layout: %s" % exc))
        grew.append(len(out2) / float(len(raw)))

    n = len(files)
    check(agree == n, "independent reader agrees with bgv_write's reader",
          "%d/%d" % (agree, n))
    check(residual_total == 0,
          "the structural model claims every non-zero byte of every file",
          "%d unclaimed non-zero bytes" % residual_total)
    check(byte_exact == n, "parse -> re-emit is BYTE-EXACT (preserve layout)",
          "%d/%d" % (byte_exact, n))
    check(fresh_exact == n,
          "parse -> re-emit with a regenerated layout is FIELD-EXACT",
          "%d/%d" % (fresh_exact, n))
    if grew:
        note("fresh-layout file size vs original: min %.3fx  mean %.3fx  "
             "max %.3fx" % (min(grew), sum(grew) / len(grew), max(grew)))
    for name, msg in errs[:10]:
        note("  %s: %s" % (name, msg))


def _cross_check(mine, theirs):
    if mine.version != theirs.version:
        return False
    if mine.num_panels != theirs.num_panels:
        return False
    if mine.num_wheels != theirs.num_wheels:
        return False
    if bytes(mine.hull_bytes()) != theirs.hull:
        return False
    if list(mine.bbmax()) != list(theirs.bbmax):
        return False
    if list(mine.bbmin()) != list(theirs.bbmin):
        return False
    for a, b in zip(mine.sections, theirs.sections):
        if (a is None) != (b is None):
            return False
        if a is None:
            continue
        if len(a.verts) != b["nvert"] or len(a.skins) != b["nskin"]:
            return False
        if tuple(a.verts) != tuple(b["verts"]):
            return False
        if tuple(a.skins) != tuple(b["skins"]):
            return False
        for sl in range(18):
            pa, pb = a.slots[sl], b["slots"][sl]
            if (pa is None) != (pb is None):
                return False
            if pa is None:
                continue
            if not _cmp_part(pa, pb):
                return False
        if not _cmp_part(a.embedded, b["emb"]):
            return False
    return True


def _cmp_part(pa, pb):
    if pa.glass_hint != pb["hint"]:
        return False
    if len(pa.records) != len(pb["recs"]):
        return False
    for ra, rb in zip(pa.records, pb["recs"]):
        if ra.indices != rb["idx"]:
            return False
        if (ra.mask, ra.tex_slot, ra.shader) != (rb["mask"], rb["tex"],
                                                 rb["shader"]):
            return False
        if (ra.unk00, ra.unk12, ra.unk1b) != (rb["u00"], rb["u12"], rb["u1b"]):
            return False
    return True


# ---------------------------------------------------------------------------
def _cube(size=1.0, cx=0.0, cy=0.0, cz=0.0):
    """A unit cube as (verts, tris) with outward CCW winding under the
    right-hand rule -- the convention .bgv stores (see sec_axis)."""
    s = size * 0.5
    corners = [(cx + x * s, cy + y * s, cz + z * s)
               for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    faces = [((0, 1, 3, 2), (-1, 0, 0)), ((4, 6, 7, 5), (1, 0, 0)),
             ((0, 4, 5, 1), (0, -1, 0)), ((2, 3, 7, 6), (0, 1, 0)),
             ((0, 2, 6, 4), (0, 0, -1)), ((1, 5, 7, 3), (0, 0, 1))]
    verts = []
    tris = []
    for quad, nrm in faces:
        base = len(verts)
        for k, ci in enumerate(quad):
            p = corners[ci]
            verts.append((p[0], p[1], p[2], nrm[0], nrm[1], nrm[2],
                          float(k & 1), float((k >> 1) & 1)))
        tris.append((base, base + 1, base + 2))
        tris.append((base, base + 2, base + 3))
    return verts, tris


def sec_synth():
    """Build a .bgv from a synthetic mesh with no Blender and no donor."""
    print("\n5. synthetic mesh -> writer -> independent reader")

    body_v, body_t = _cube(2.0)
    glass_v, glass_t = _cube(0.8, cy=1.2)
    wheel_v, wheel_t = _cube(0.6, cx=0.9, cz=1.2)
    groups = [
        W.MeshGroup("body", body_v, body_t, slot=W.SLOT_APERTURE_BODY,
                    mask=W.MASK_BODY, tex_slot=W.TEX_PAINT),
        W.MeshGroup("glass", glass_v, glass_t, slot=W.SLOT_APERTURE_BODY,
                    mask=W.MASK_GLASS_A, tex_slot=W.TEX_GLASS_OK,
                    alpha0=0.41, alpha1=0.41),
        W.MeshGroup("wheel", wheel_v, wheel_t, slot=W.SLOT_WHEEL_SLOW,
                    mask=W.MASK_BODY_UNLIT, tex_slot=W.TEX_PAINT),
    ]
    m = W.build_bgv(groups, template=None, lod_count=4)
    data = W.write_bgv(m)
    check(len(data) % W.FILE_ALIGN == 0,
          "file length is a multiple of 0x800", "0x%X" % len(data))
    check(struct.unpack_from("<I", data, 0x08)[0] == len(data),
          "header +0x08 carries the padded file size")

    g = indep_parse(data)
    check(g.version == W.BGV_VERSION, "version 0x17")
    check(g.declared_size == len(data), "declared size matches")
    check(sum(1 for s in g.sections if s) == 4, "4 LOD sections written")

    sec = g.sections[0]
    nv = len(body_v) + len(glass_v) + len(wheel_v)
    check(sec["nvert"] == nv, "position pool holds every vertex",
          "%d, expected %d" % (sec["nvert"], nv))
    check(sec["pool1"] - sec["pool0"] == sec["nvert"] * 0x18,
          "pool1 starts exactly 0x18*nvert after pool0")
    check(sec["nskin"] == nv,
          "skin stream covers the slot vertex range", str(sec["nskin"]))
    check(all(s == W.DEFAULT_SKIN for s in sec["skins"]),
          "default skin entry is the shipped bone-0/full-weight pattern")

    # geometry survives exactly
    for i, v in enumerate(body_v + glass_v + wheel_v):
        got = sec["verts"][i]
        if not (abs(got[0] - v[0]) < 1e-6 and abs(got[1] - v[1]) < 1e-6
                and abs(got[2] - v[2]) < 1e-6):
            check(False, "vertex %d position survives" % i, str((got, v)))
            break
        if not (abs(got[4] - v[6]) < 1e-6 and abs(got[5] - v[7]) < 1e-6):
            check(False, "vertex %d uv survives" % i, str((got, v)))
            break
        n = W.unpack_normal(got[3])
        if max(abs(n[k] - v[3 + k]) for k in range(3)) > 2e-3:
            check(False, "vertex %d normal survives" % i, str((n, v[3:6])))
            break
    else:
        check(True, "every vertex position, normal and uv survives the "
                    "round trip (%d vertices)" % nv)

    # records
    slot0 = sec["slots"][W.SLOT_APERTURE_BODY]
    slot7 = sec["slots"][W.SLOT_WHEEL_SLOW]
    check(slot0 is not None and len(slot0["recs"]) == 2,
          "slot 0 (aperture body) has the body and glass records")
    check(slot7 is not None and len(slot7["recs"]) == 1,
          "slot 7 (slow wheel) has the wheel record")
    check(len(sec["emb"]["recs"]) == 3,
          "the embedded intact part carries all three",
          "%d records" % len(sec["emb"]["recs"]))
    check(slot0["rel"] == 0x10, "part record array is at part+0x10")

    got_masks = [r["mask"] for r in sec["emb"]["recs"]]
    check(got_masks == [W.MASK_BODY, W.MASK_GLASS_A, W.MASK_BODY_UNLIT],
          "pass masks survive", str([hex(x) for x in got_masks]))
    got_tex = [r["tex"] for r in sec["emb"]["recs"]]
    check(got_tex == [W.TEX_PAINT, W.TEX_GLASS_OK, W.TEX_PAINT],
          "texture slots survive", str(got_tex))
    check(abs(sec["emb"]["recs"][1]["a0"] - 0.41) < 1e-6,
          "glass tint at record +0x04 survives")
    check(sec["emb"]["hint"] == 1,
          "part+0x01 glass hint = index of the last mask&0x100 record",
          str(sec["emb"]["hint"]))

    # triangles
    for name, part, want in (("body", slot0["recs"][0], body_t),
                             ("glass", slot0["recs"][1], glass_t),
                             ("wheel", slot7["recs"][0], wheel_t)):
        decoded = W.strip_to_triangles(part["idx"])
        base = min(min(t) for t in decoded)
        back = [tuple(i - base for i in t) for t in decoded]
        check(sorted(map(_norm, back)) == sorted(map(_norm, want)),
              "%s triangles decode back unchanged (same set, same "
              "orientation)" % name,
              "%d in, %d out" % (len(want), len(back)))

    # the body box the collision code reads (.bgv +0xE80 / +0xE90 ->
    # veh+0x1D0 / +0x1E0, src/burnout3_carcol.h:88-89)
    lo = [min(v[k] for v in body_v + glass_v + wheel_v) for k in range(3)]
    hi = [max(v[k] for v in body_v + glass_v + wheel_v) for k in range(3)]
    check(all(abs(g.bbmax[k] - hi[k]) < 1e-6 for k in range(3))
          and all(abs(g.bbmin[k] - lo[k]) < 1e-6 for k in range(3)),
          ".bgv +0xE80/+0xE90 body box tracks the exported mesh bounds",
          "max %s want %s / min %s want %s"
          % (g.bbmax[:3], hi, g.bbmin[:3], lo))

    # and the same build with a real donor keeps everything it cannot author
    donor_path = os.path.join(PVEH, "COMP", "Car1.bgv")
    if os.path.exists(donor_path):
        donor = W.read_bgv_file(donor_path)
        m2 = W.build_bgv(groups, template=donor, lod_count=4)
        d2 = W.write_bgv(m2)
        g2 = indep_parse(d2)
        check(g2.hull == donor.hull_bytes(),
              "template mode preserves the convex collision hull "
              "(.bgv +0x1060) byte for byte")
        check(g2.num_panels == donor.num_panels
              and g2.num_wheels == donor.num_wheels,
              "template mode preserves the panel / wheel counts")
        check(g2.panel_kinds == donor.panel_kinds(),
              "template mode preserves the panel kind ids (.bgv +0xAC4)")
        check(g2.matdir != 0 and W.read_bgv(d2).matdir_blob ==
              donor.matdir_blob,
              "template mode relocates the paint/texture directory intact",
              "%d bytes" % len(donor.matdir_blob))
        check(abs(g2.wheel_radius - donor.wheel_radius) < 1e-9,
              "template mode preserves the wheel radius (.bgv +0x18)")
    else:
        note("SKIP the template-mode checks: %s not present" % donor_path)


def sec_relink():
    """Replay the retail relocation pass over everything we write."""
    print("\n6. the game's own relocation pass (FUN_000310f0)")
    body_v, body_t = _cube(2.0)
    m = W.build_bgv([W.MeshGroup("body", body_v, body_t)], lod_count=4)
    data = W.write_bgv(m)
    try:
        n = simulate_relink(data)
        check(True, "template-free synthetic file survives the relinker",
              "%d pointers formed, all inside the file" % n)
    except AssertionError as exc:
        check(False, "template-free synthetic file survives the relinker",
              str(exc))

    files = sorted(glob.glob(os.path.join(PVEH, "*", "*.bgv")))
    if not files:
        note("SKIP the real-asset relink checks: no .bgv files present")
        return
    ok = 0
    errs = []
    for path in files:
        raw = open(path, "rb").read()
        try:
            mine = W.read_bgv(raw)
            simulate_relink(W.write_bgv(mine, preserve_layout=False))
            ok += 1
        except AssertionError as exc:
            errs.append(("/".join(path.split(os.sep)[-2:]), str(exc)))
        except W.BgvError as exc:
            errs.append(("/".join(path.split(os.sep)[-2:]), str(exc)))
    check(ok == len(files),
          "every re-laid-out real car survives the relinker",
          "%d/%d" % (ok, len(files)))
    for name, msg in errs[:5]:
        note("  %s: %s" % (name, msg))

    donor = os.path.join(PVEH, "COMP", "Car1.bgv")
    if os.path.exists(donor):
        t = W.read_bgv_file(donor)
        m2 = W.build_bgv([W.MeshGroup("body", body_v, body_t)], template=t)
        try:
            n = simulate_relink(W.write_bgv(m2))
            check(True, "template-mode export survives the relinker",
                  "%d pointers" % n)
        except AssertionError as exc:
            check(False, "template-mode export survives the relinker",
                  str(exc))


def sec_port():
    """End to end through the PORT's own reader, tools/extract_bgv.py.

    A third, pre-existing implementation neither the writer nor this file
    shares any code with: re-export a shipped car with a regenerated layout,
    run the repo's extractor over both the original and the re-export, and
    require every emitted artefact -- the OBJs, the .panels and .wheels
    sidecars -- to come out byte-identical."""
    print("\n7. end to end through tools/extract_bgv.py (the port's reader)")
    import importlib.util
    import shutil
    import tempfile

    ex_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "extract_bgv.py")
    if not os.path.exists(ex_path):
        note("SKIP: %s not present" % ex_path)
        return
    files = sorted(glob.glob(os.path.join(PVEH, "*", "*.bgv")))
    if not files:
        note("SKIP: no .bgv assets present")
        return
    spec = importlib.util.spec_from_file_location("extract_bgv", ex_path)
    ex = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ex)

    tmp = tempfile.mkdtemp(prefix="bgvcheck")
    try:
        same = 0
        checked = 0
        errs = []
        for path in files[:12]:                # a dozen cars, all classes
            name = "/".join(path.split(os.sep)[-2:])
            model = W.read_bgv_file(path)
            rewritten = os.path.join(tmp, "rewritten.bgv")
            with open(rewritten, "wb") as f:
                f.write(W.write_bgv(model, preserve_layout=False))
            outs = []
            for tag, src in (("a", path), ("b", rewritten)):
                d = os.path.join(tmp, tag)
                shutil.rmtree(d, ignore_errors=True)
                os.makedirs(os.path.join(d, "parts"))
                res, err = ex.extract(src, d, os.path.join(d, "parts"), "car")
                if err:
                    errs.append((name, "extract_bgv: %s" % err))
                outs.append(d)
            a, b = outs
            names_a = _tree(a)
            names_b = _tree(b)
            if names_a != names_b:
                errs.append((name, "different artefact sets"))
                continue
            bad = [n for n in names_a
                   if open(os.path.join(a, n), "rb").read()
                   != open(os.path.join(b, n), "rb").read()]
            checked += len(names_a)
            if bad:
                errs.append((name, "differing artefacts: %s" % bad[:3]))
            else:
                same += 1
        check(same == len(files[:12]) and not errs,
              "the port's extractor gets byte-identical output from the "
              "original and the re-exported car",
              "%d/%d cars, %d artefacts compared" % (same, len(files[:12]),
                                                     checked))
        for n, msg in errs[:5]:
            note("  %s: %s" % (n, msg))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _tree(root):
    out = []
    for dirpath, _dirs, names in os.walk(root):
        for n in names:
            out.append(os.path.relpath(os.path.join(dirpath, n), root))
    return sorted(out)


# ===========================================================================
# The Blender addon, driven through a stub bpy
# ===========================================================================
class _V3(object):
    def __init__(self, x, y, z):
        self.x, self.y, self.z = float(x), float(y), float(z)

    def __iter__(self):
        return iter((self.x, self.y, self.z))


class _M4(object):
    """Just enough mathutils.Matrix for io_export_bgv.collect_groups."""

    def __init__(self, rows):
        self.rows = [list(r) for r in rows]

    def __matmul__(self, v):
        r = self.rows
        return _V3(r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z + r[0][3],
                   r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z + r[1][3],
                   r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z + r[2][3])

    def __getitem__(self, i):
        return self.rows[i]

    def to_3x3(self):
        return _M4([r[:3] + [0.0] for r in self.rows[:3]] + [[0, 0, 0, 1]])

    def transposed(self):
        return _M4([[self.rows[j][i] for j in range(4)] for i in range(4)])

    def inverted_safe(self):
        # only ever called on the affine matrices this test builds
        m = [r[:] for r in self.rows]
        inv = [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]
        for c in range(4):
            piv = max(range(c, 4), key=lambda r: abs(m[r][c]))
            m[c], m[piv] = m[piv], m[c]
            inv[c], inv[piv] = inv[piv], inv[c]
            d = m[c][c]
            m[c] = [x / d for x in m[c]]
            inv[c] = [x / d for x in inv[c]]
            for r in range(4):
                if r == c:
                    continue
                f = m[r][c]
                m[r] = [a - f * b for a, b in zip(m[r], m[c])]
                inv[r] = [a - f * b for a, b in zip(inv[r], inv[c])]
        return _M4(inv)

    def determinant(self):
        r = self.rows
        return (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1])
                - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0])
                + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))


def _install_bpy_stub():
    """A bpy just real enough to import io_export_bgv and run its mesh
    conversion.  The addon is a thin wrapper, but it owns the axis change
    and the winding decision, and neither should go untested just because
    Blender is not installed."""
    import types

    bpy = types.ModuleType("bpy")
    props = types.ModuleType("bpy.props")
    for name in ("BoolProperty", "EnumProperty", "FloatProperty",
                 "IntProperty", "StringProperty"):
        setattr(props, name, lambda **kw: kw)
    types_mod = types.ModuleType("bpy.types")

    class _Operator(object):
        pass

    class _Menu(object):
        @staticmethod
        def append(fn):
            pass

        @staticmethod
        def remove(fn):
            pass

    types_mod.Operator = _Operator
    types_mod.TOPBAR_MT_file_export = _Menu
    path = types.SimpleNamespace(abspath=lambda p: p)
    utils = types.SimpleNamespace(register_class=lambda c: None,
                                  unregister_class=lambda c: None)
    bpy.props = props
    bpy.types = types_mod
    bpy.path = path
    bpy.utils = utils
    bpy.app = types.SimpleNamespace(debug=False)
    extras = types.ModuleType("bpy_extras")
    io_utils = types.ModuleType("bpy_extras.io_utils")

    class _ExportHelper(object):
        pass

    io_utils.ExportHelper = _ExportHelper
    extras.io_utils = io_utils
    for name, mod in (("bpy", bpy), ("bpy.props", props),
                      ("bpy.types", types_mod), ("bpy_extras", extras),
                      ("bpy_extras.io_utils", io_utils)):
        sys.modules[name] = mod


def _fake_object(name, verts, tris, normals, uvs, matrix, props=None,
                 material_names=()):
    import types as _t

    loops = []
    corner = []
    uv_items = []
    tri_recs = []
    for t, ns, us in zip(tris, normals, uvs):
        base = len(loops)
        for k in range(3):
            loops.append(_t.SimpleNamespace(vertex_index=t[k],
                                            normal=ns[k]))
            corner.append(_t.SimpleNamespace(vector=_V3(*ns[k])))
            uv_items.append(_t.SimpleNamespace(uv=us[k]))
        tri_recs.append(_t.SimpleNamespace(
            loops=(base, base + 1, base + 2), material_index=0))

    class _UVLayers(list):
        def get(self, n):
            return None

    layer = _t.SimpleNamespace(data=uv_items, name="UVMap")
    uvl = _UVLayers([layer])
    uvl.active = layer

    mats = [_t.SimpleNamespace(name=n, get=lambda k, _n=n: None)
            for n in material_names]
    mesh = _t.SimpleNamespace(
        calc_loop_triangles=lambda: None,
        corner_normals=corner,
        loops=loops,
        vertices=[_t.SimpleNamespace(co=_V3(*v)) for v in verts],
        uv_layers=uvl,
        loop_triangles=tri_recs,
        materials=mats)

    obj = _t.SimpleNamespace(type="MESH", name=name, matrix_world=matrix,
                             data=mesh)
    obj.get = (props or {}).get
    obj.evaluated_get = lambda dg: obj
    obj.to_mesh = lambda: mesh
    obj.to_mesh_clear = lambda: None
    return obj


def sec_addon():
    """Import and drive tools/blender/io_export_bgv.py without Blender."""
    print("\n8. the Blender addon's mesh conversion (stub bpy)")
    _install_bpy_stub()
    import importlib.util
    import types as _t

    mod_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "blender", "io_export_bgv.py")
    spec = importlib.util.spec_from_file_location("io_export_bgv", mod_path)
    addon = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(addon)
        check(True, "io_export_bgv imports and defines its operator")
    except Exception as exc:                                # noqa: BLE001
        check(False, "io_export_bgv imports", repr(exc))
        return
    check(addon.EXPORT_OT_bgv.bl_idname == "export_scene.burnout3_bgv",
          "operator id registered under File > Export")
    check(addon.bl_info["blender"] >= (4, 0, 0), "bl_info targets Blender 4.x")

    # object -> part slot
    mk = lambda n, p=None: _t.SimpleNamespace(   # noqa: E731
        name=n, get=(p or {}).get)
    check(addon.slot_for_object(mk("Car_body"), 0) == W.SLOT_APERTURE_BODY,
          "an object named *body* lands in slot 0")
    check(addon.slot_for_object(mk("front_wheel_L"), 0) == W.SLOT_WHEEL_SLOW,
          "an object named *wheel* lands in slot 7")
    check(addon.slot_for_object(mk("panel3_door"), 0) == 4,
          "panel3 lands in slot 4 (panel k -> slot k+1)")
    check(addon.slot_for_object(mk("thing", {"b3_slot": 9}), 0) == 9,
          "the b3_slot custom property wins")

    # material -> record keys
    m = lambda n, p=None: _t.SimpleNamespace(   # noqa: E731
        name=n, get=(p or {}).get)
    check(addon.keys_for_material(m("b3tex1"), 1, 0) == (1, 1, 0, 1.0),
          "material b3tex1 -> texture slot 1 (VehicleUnderside), which is "
          "what tools/extract_bgv.py writes as usemtl")
    mask, tex, _sh, _a = addon.keys_for_material(m("windscreen_glass"), 1, 0)
    check(mask == W.MASK_GLASS_A and tex == W.TEX_GLASS_OK,
          "a *glass* material defaults to mask 0x100 / texture slot 2")
    check(addon.keys_for_material(
        m("x", {"b3_mask": 2, "b3_texslot": 3, "b3_shader": 1,
                "b3_alpha": 0.5}), 1, 0) == (2, 3, 1, 0.5),
          "b3_mask / b3_texslot / b3_shader / b3_alpha override everything")

    # a real conversion: one Blender triangle, nose along +Y
    ident = _M4([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
    verts = [(1.0, 2.0, 3.0), (4.0, 5.0, 6.0), (0.0, 0.0, 1.0)]
    tris = [(0, 1, 2)]
    nrm = [[(0.0, 0.0, 1.0)] * 3]
    uvs = [[(0.25, 0.75), (0.5, 0.5), (0.0, 1.0)]]
    obj = _fake_object("body", verts, tris, nrm, uvs, ident)
    ctx = _t.SimpleNamespace(evaluated_depsgraph_get=lambda: None)
    opts = {"nose": "+Y", "scale": 1.0, "uv_layer": "", "flip_v": True,
            "apply_modifiers": False, "default_slot": 0,
            "default_mask": W.MASK_BODY, "default_tex": W.TEX_PAINT}
    groups, stats = addon.collect_groups(ctx, [obj], opts)
    check(len(groups) == 1 and stats["tris"] == 1,
          "one object, one material -> one group, one triangle")
    g = groups[0]
    got = {tuple(round(x, 6) for x in v[:3]) for v in g.verts}
    want = {(1.0, 3.0, 2.0), (4.0, 6.0, 5.0), (0.0, 1.0, 0.0)}
    check(got == want,
          "nose +Y: (x, y, z)_blender -> (x, z, y)_bgv", str(sorted(got)))
    check(g.tris[0] == (0, 2, 1),
          "winding is reversed for the right-handed -> left-handed change",
          str(g.tris[0]))
    uv = {(round(v[6], 4), round(v[7], 4)) for v in g.verts}
    check(uv == {(0.25, 0.25), (0.5, 0.5), (0.0, 0.0)},
          "flip_v turns Blender's bottom-left UV origin into the top-left "
          "origin the game samples with", str(sorted(uv)))
    ny = {(round(v[3], 4), round(v[4], 4), round(v[5], 4))
          for v in g.verts}
    check(ny == {(0.0, 1.0, 0.0)},
          "a Blender +Z normal becomes .bgv +Y (up)", str(ny))

    # a mirrored object must NOT get a second winding flip
    mirror = _M4([[-1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]])
    obj2 = _fake_object("body", verts, tris, nrm, uvs, mirror)
    g2 = addon.collect_groups(ctx, [obj2], opts)[0][0]
    check(g2.tris[0] == (0, 1, 2),
          "a negatively scaled object cancels the flip instead of "
          "double-flipping", str(g2.tris[0]))

    # and the groups the addon produces really do build a file
    body_v, body_t = _cube(2.0)
    grp = W.MeshGroup("body", body_v, body_t)
    data = W.write_bgv(W.build_bgv([grp]))
    check(len(data) > W.HEADER_SIZE, "addon-shaped groups serialise")


SECTIONS = {"spec": sec_spec, "strip": sec_strip, "axis": sec_axis,
            "real": sec_real, "synth": sec_synth, "relink": sec_relink,
            "port": sec_port, "addon": sec_addon}
ORDER = ["spec", "strip", "axis", "real", "synth", "relink", "port", "addon"]


def main():
    want = sys.argv[1:] or ORDER
    for name in want:
        if name not in SECTIONS:
            print("unknown section %r; known: %s" % (name, " ".join(ORDER)))
            return 2
        SECTIONS[name]()
    print("\n%d/%d passed" % (PASS, PASS + FAIL))
    if _FAILURES:
        print("failures:")
        for f in _FAILURES:
            print("  " + f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
