#!/usr/bin/env python3
"""Burnout 3 (Xbox) .bgv vehicle-geometry reader / WRITER -- no Blender needed.

This module is the pure byte-packing half of the Blender exporter
(tools/blender/io_export_bgv.py).  It is deliberately import-clean so that
tools/validate_bgv.py can exercise every field without Blender installed.

=========================================================================
EVIDENCE.  Every offset below is [C] -- recovered from code, not guessed.
Two independent sources agree:

  (a) the game's own pointer-relocation pass, FUN_000310f0 (file level) +
      FUN_00031010 (per-LOD-section) + FUN_00030120 (per part object),
      reached from the .bgv load completion FUN_0018d0e0.  A relinker is
      an exhaustive list of the pointer fields and their strides, so it is
      the authoritative definition of the container.
  (b) this repo's working reader, tools/extract_bgv.py, whose module
      docstring carries the execution-traced citations for the draw path
      (FUN_000303D0 / FUN_00031AB0 / FUN_000315C0) and the vertex
      declaration at 0x00387558.

Anything I could NOT source is marked [?] and is round-tripped verbatim
from a donor file rather than synthesised.  Search this file for "[?]".

-------------------------------------------------------------------------
CONTAINER MODEL

.bgv is a pointer-relocated container: internal pointers are stored as
offsets and the loader adds the runtime buffer address to a fixed set of
fields.  Because the buffer's first byte is offset 0 == base, the file can
be walked in place with the identical arithmetic.  Little-endian x86.

FILE HEADER -- fixed 0x16F0 bytes
  [C] every one of the 67 shipped player .bgv files has section 0 at
  exactly 0x16F0, so the header is a fixed-size record, not a variable one.

  +0x000 u32   version, 0x17 on all 67 files          [C survey]
  +0x004 u32   0 on all 67                            [C survey]  [?] meaning
  +0x008 u32   FILE SIZE, == len(file) on all 67, and every file is a
               multiple of 0x800                      [C survey]
  +0x00C u8    numBodyParts (detachable panels, <= 6) [C FUN_0018d0e0
               copies *(char*)(model+0x0C) panel matrices from +0xD00]
  +0x00D u8    numWheels (<= 6)                       [C FUN_0018d0e0
               copies *(char*)(model+0x0D) wheel matrices from +0xB80]
  +0x00E u16   0                                                     [?]
  +0x010 u32   3 on all 67                            [C survey]     [?]
  +0x014 f32   body scalar ~2.2..6.6 (grows with car size)           [?]
  +0x018 f32   WHEEL RADIUS                           [C FUN_0018d0e0
               ctx[0x14b] = *(float*)(model+0x18)]
  +0x01C f32[6] per-wheel radius SCALE                 [C FUN_0018d0e0:
               per wheel w, ctx = *(float*)(model+0x1C+4*w) * radius]
  +0x02C..0x04C zero on 64 of 67                       [C survey]    [?]
  +0x04C u32[5] LOD SECTION offsets (rel file base).  Slot 4 is 0 on all
               67; all 67 ship exactly 4 LODs.        [C FUN_000310f0
               relinks exactly 5 slots from +0x4C]
  +0x060 u32   MATERIAL / texture directory offset (rel file base)
               [C FUN_000310f0: += base, then FUN_001c8e20 relinks it]
  +0x064 u32   standalone part object offset          [C FUN_000310f0]
  +0x068 u32   standalone part object offset          [C FUN_000310f0]
  +0x06C u32   0 on all 67                            [C survey]
  +0x070 f32[4][4] x8, stride 0x40: DEFORMATION matrix set
               [C FUN_0012FEE0 copies all 8 to ctx+0x700; the damage body
               is skinned to them -- see the SKIN STREAM below]
  +0x270 f32[4][4] x32, stride 0x40                                  [?]
               (dense matrices; purpose not recovered)
  +0xA70 f32[20]                                                     [?]
  +0xAC4 i32[numBodyParts] PANEL KIND ids   [C FUN_00023DE0 matches them
               against the priority table DAT_00385224 = {3,6,0,1,5,4,2};
               names in src/burnout3_panels.h:109  0=RIGHT DOOR 1=LEFT DOOR
               2=FRONT 3=REAR 4=BONNET 5=BOOT/HATCH 6=extra rear]
  +0xADC u8[numBodyParts] panel hinge axis  [C flying-part ctor
               FUN_001069C0, cited in tools/extract_bgv.py]
  +0xAE2..0xB80 panel hinge limits etc.                              [?]
  +0xB80 f32[4][4] x6, stride 0x40: WHEEL matrices, rows Right/Up/At/Pos
               [C FUN_0018d0e0 + FUN_0012FEE0]
  +0xD00 f32[4][4] x6, stride 0x40: PANEL placement matrices
               [C FUN_0018d0e0 + tools/trace_panels.py; row 3 = the pivot,
               in GAME space -- src/burnout3_panels.h:336]
  +0xE80 f32[4]  collision box MAX corner  [C FUN_00122830 -> veh+0x1D0,
               src/burnout3_carcol.h:88]
  +0xE90 f32[4]  collision box MIN corner  [C  -> veh+0x1E0, carcol.h:89]
  +0xEA0 f32[8] x6, stride 0x20: per-panel pivot-local AABB (max,min)
               [C FUN_001069C0 reads (idx+0x75)*0x20 off the base]
  +0x1060 CONVEX COLLISION HULL, 0x600 bytes  [C FUN_000310f0 relocates
               five self-relative pointers at +0x1060..+0x1070; layout
               asserted in src/burnout3_carcol.h: nverts +0x18, nplanes
               +0x19, nedges +0x1A, planes +0xA0, verts +0x320,
               edges +0x480.  COMP/Car1: V16 - E42 + F28 = 2, a closed
               convex polyhedron.]
  +0x1664 u32[18] pointers, relocated in 3 groups of 6  [C FUN_000310f0]
               contents [?] -- carried verbatim
  +0x16C0 u32   pointer; +0x16C4 u32 pointer; +0x16C8 s8 count of a
               stride-0xC array at +0x16C0 whose +8 field is ALSO
               base-relative                        [C FUN_000310f0]
               0 on all 67 shipped files            [C survey]
  +0x16DC u32[2] pointers, each relinked by FUN_00159470 (self-relative
               internally, so the blob is position independent) [C]

LOD SECTION -- 0x70-byte header
  S+0x00 u32[18] PART OBJECT offsets, rel S            [C FUN_00031010]
         slot 0      deformable aperture body (car minus panels)
         slot 1..6   damage panels, pivot-local
         slot 7/8/9  wheel mesh: slow / blur>25 rad/s / blur>50 rad/s
         slot 10..17 never used by any shipped player .bgv [C survey]
  S+0x48 u32   D3D vertex-buffer resource "Common", 1 on all 268 sections
  S+0x4C u32   POSITION POOL offset, rel S             [C FUN_000310f0
               masks it with 0x0FFFFFFF unless (S+0x48 & 0x70000)==0x20000;
               bound as STREAM 0 stride 0x18 by FUN_000315C0]
  S+0x50 u32   0 on all 268
  S+0x54 u32   second resource "Common", 1 on all 268
  S+0x58 u32   SKIN POOL offset, rel S; bound as STREAM 1 stride 8
               [C FUN_000315C0: FUN_0034edb0(1, S+0x54, 8)]
  S+0x5C u32   0 on all 268
  S+0x60 PART OBJECT, inline: the embedded ONE-PIECE INTACT car.  Its
               record-array offset at S+0x64 is relative to S+0x60.
               [C FUN_000310f0]
  S+0x68/S+0x6C 0 on all 268

PART OBJECT -- 0x10-byte header + records
  +0x00 s8    record count
  +0x01 s8    "last record with mask&0x100, else -1"  [C empirical,
              2962/2970 part objects across all 67 files; the 8 misses are
              HSPC Car24/Car25.  Carried verbatim on rebuild so it is
              never wrong for a real file; derived only for new geometry.]
  +0x02 u16   0 on all
  +0x04 u32   record-array offset, rel the part header (0x10 on all 2572
              slot parts)                              [C FUN_00031010]
  +0x08..0x10 zero on all                               [C survey]

RECORD -- 0x1C bytes                                    [C FUN_00031AB0]
  +0x00 u32   0 on all 8905 records                     [C survey]  [?]
  +0x04 f32   glass tint / alpha: pushed as the .w of a shader constant
              in the glass branch (local_34 = *(u32*)(rec+4))  [C]
  +0x08 f32   same value as +0x04 in every shipped record [C survey] [?]
  +0x0C u32   INDEX STREAM offset, rel the record       [C FUN_00031010]
  +0x10 u16   INDEX COUNT -- FUN_001d7d10(6, *(u16*)(rec+0x10),
              *(u32*)(rec+0x0C)).  Xbox D3D8 primitive 6 ==
              D3DPT_TRIANGLESTRIP.                      [C]
  +0x12 u16   0 on all 8905                             [C survey]
  +0x14 u32   0 or 1: shader variant.  Plain-body branch picks
              DAT_004d6550 when 0 and DAT_004d6554 when 1; the mask&2
              branch stores it in DAT_0075d58c.         [C]
  +0x18 u16   PASS MASK.  Only ever a single bit in shipped data:
                 0x0001  body                (2491 records)
                 0x0002  body, unlit variant (3295)
                 0x0004..0x0080  LIGHT ELEMENT bits 2..7: drawn lit when
                                 the pass byte has the bit  (1941 total)
                 0x0100  glass tier A        (1104)
                 0x0200  glass tier B        (74)
              Branch order in FUN_00031AB0:
                 if (mask & 2)                  -> unlit body
                 else if (mask & passbyte & 0xFC) -> light element
                 else if (mask & 0x300)         -> glass, TWO draw calls
                 else                           -> plain body
              Draw passes: 0x3FF intact car, 0xFF damage body, 0x300 glass.
  +0x1A u8    TEXTURE SLOT, an index into the DRAW CONTEXT's 5-entry
              texture array at ctx+0x334, NOT into the .bgv:
                 0 = this file's own paint page (from header +0x60)
                 1 = "VehicleUnderside"  } global, from Data/Global.txd
                 2 = "UnbrokenGlass"     } [C: car-system init 0x0002F260
                 3 = "CrackedGlass"      }  looks each up by name and
                 4 = "SmashedGlass"      }  FUN_000315C0 fills 1..4]
              Shipped data uses 0 (7129), 1 (598), 2 (1178).   [C survey]
  +0x1B u8    0 on all 8905                             [C survey]

POSITION POOL -- stride 0x18, count = (max index used anywhere) + 1
  [C the pool0->pool1 distance is EXACTLY 0x18*(maxidx+1) in all four
   sections of COMP/Car1, and the validator asserts it on all 67 files]
  +0x00 f32[3] position
  +0x0C u32    D3DVSDT_NORMPACKED3 normal: x = bits 0..10 signed / 1023,
               y = bits 11..21 signed / 1023, z = bits 22..31 signed / 511
               [C car vertex declaration 0x00387558 =
                20000000 40320000 40160002 40220009 FFFFFFFF
                -> stream0 v0=FLOAT3 v2=NORMPACKED3 v9=FLOAT2, stride 0x18]
  +0x10 f32[2] uv

SKIN POOL -- stride 8, count = (max index used by SLOTS 0..17) + 1
  [C exact in all four sections of COMP/Car1: 321/1264/2442/5281 against
   slot maxima 320/1263/2441/5280, and asserted on all 67 by the
   validator.  The embedded intact part indexes past the end of this
   stream, which is why it is shorter than the position pool.]
  +0x00 u8[4]  blend indices, PRE-MULTIPLIED BY 3 (constant registers per
               matrix): observed values 0,3,6,9,12,15  [C empirical]
  +0x04 u8[4]  blend weights, summing to exactly 255   [C empirical: every
               sampled vertex, e.g. 118+93+44+0 == 255]
               The 8 deformation matrices at file+0x70 are the targets.
  Conservative default for new geometry: 00 00 00 00 FF 00 00 00
  (bone 0, full weight) -- which is verbatim the most common shipped
  entry (75 of 321 in COMP/Car1 LOD0).

-------------------------------------------------------------------------
AXIS CONVENTION -- .bgv stores GAME space, LEFT-handed, +Z = NOSE

  +Y is UP        [C tools/extract_bgv.py: the 81 files with a slot-1
                  ("VehicleUnderside") record all have an area-weighted
                  geometric normal of mean n.y -0.70..-0.94, i.e. the
                  belly faces -Y]
  +Z is the NOSE  [C src/burnout3_panels.h:336 "attach[] positions are the
                  .bgv+0xD00 row-3 pivots in GAME space (+Z nose)"; and
                  extract_bgv.py's sanity check is on the Z extent, which
                  is the car's LENGTH]
  +X is the car's RIGHT
                  [C src/burnout3_panels.h:109-110 "kind 0 RIGHT DOOR
                  x = +0.0..1.21" / "kind 1 LEFT DOOR the mirror of kind
                  0"; COMP/Car1 has kind 0 at +0xD00 row3 = (+0.84,0,0.80)
                  and kind 1 at (-0.84,0,0.80)]

  right = +X, up = +Y, forward = +Z is a LEFT-handed basis (a right-handed
  one would need right = cross(forward, up) = cross(Z,Y) = -X).  That
  matches Xbox D3D and matches this repo's own GAME-vs-display split:
  the harness mirrors GAME space into GL/display space by negating z
  (docs/RE_NOTES.md 12, src/burnout3_track_paths.h:9).  .bgv stores the
  GAME (+Z) side, NOT the harness/display (-Z) side.

  WINDING.  With extract_bgv.py's strip decode, the right-handed
  cross(b-a, c-a) of a .bgv triangle agrees with the stored per-vertex
  normals: area-weighted mean dot +0.967 over the 3418 triangles of
  COMP/Car1's highest-LOD embedded part (and the same test on the whole
  pool gives +0.9916, tools/extract_bgv.py).  So on the raw numbers the
  front face is CCW under the right-hand rule -- the same convention
  Blender uses.

  Blender is right-handed Z-up, .bgv is left-handed Y-up, so EVERY
  Blender->bgv basis change has determinant -1 and the triangle winding
  MUST be reversed to keep the same visible facing.  axis_matrix() below
  returns the basis and always pairs it with a winding flip.
========================================================================="""

from __future__ import annotations

import math
import struct

# --------------------------------------------------------------------------
# Layout constants  (all [C]; see the module docstring for the citation)
# --------------------------------------------------------------------------
BGV_VERSION      = 0x17
HEADER_SIZE      = 0x16F0
SECTION_HDR_SIZE = 0x70
PART_HDR_SIZE    = 0x10
RECORD_SIZE      = 0x1C
VERTEX_STRIDE    = 0x18
SKIN_STRIDE      = 0x08
NUM_SLOTS        = 18
MAX_SECTIONS     = 5
MAX_PANELS       = 6
MAX_WHEELS       = 6
FILE_ALIGN       = 0x800

H_VERSION        = 0x000
H_UNK04          = 0x004
H_FILESIZE       = 0x008
H_NUM_PANELS     = 0x00C
H_NUM_WHEELS     = 0x00D
H_KIND           = 0x010
H_BODY_SCALAR    = 0x014
H_WHEEL_RADIUS   = 0x018
H_WHEEL_SCALE    = 0x01C
H_SECTIONS       = 0x04C
H_MATDIR         = 0x060
H_AUX_PART       = (0x064, 0x068)
H_DEFORM_MATS    = 0x070          # 8 x 0x40
H_PANEL_KIND     = 0xAC4          # i32[numPanels]
H_PANEL_AXIS     = 0xADC          # u8[numPanels]
H_WHEEL_MATS     = 0xB80          # 6 x 0x40
H_PANEL_MATS     = 0xD00          # 6 x 0x40
H_BBMAX          = 0xE80          # f32[4]
H_BBMIN          = 0xE90          # f32[4]
H_PANEL_BB       = 0xEA0          # 6 x 0x20  (max[4], min[4])
H_HULL           = 0x1060         # 0x600 bytes
H_HULL_SIZE      = 0x600
H_LIGHT_PTRS     = 0x1664         # u32[18]
H_TAIL_PTR_A     = 0x16C0
H_TAIL_PTR_B     = 0x16C4
H_TAIL_COUNT     = 0x16C8
H_EXTRA_PTRS     = 0x16DC         # u32[2]

S_SLOTS          = 0x00
S_VB_COMMON      = 0x48
S_POOL0          = 0x4C
S_UNK50          = 0x50
S_VB2_COMMON     = 0x54
S_POOL1          = 0x58
S_UNK5C          = 0x5C
S_EMB            = 0x60           # inline part header
S_EMB_RECS       = 0x64

R_UNK00          = 0x00
R_ALPHA0         = 0x04
R_ALPHA1         = 0x08
R_INDEX_OFF      = 0x0C
R_INDEX_COUNT    = 0x10
R_UNK12          = 0x12
R_SHADER         = 0x14
R_MASK           = 0x18
R_TEXSLOT        = 0x1A
R_UNK1B          = 0x1B

# pass-mask bits [C FUN_00031AB0]
MASK_BODY        = 0x0001
MASK_BODY_UNLIT  = 0x0002
MASK_LIGHTS      = 0x00FC
MASK_GLASS_A     = 0x0100
MASK_GLASS_B     = 0x0200
MASK_GLASS       = 0x0300
PASS_INTACT      = 0x03FF
PASS_DAMAGE_BODY = 0x00FF
PASS_GLASS       = 0x0300

# texture slots [C FUN_00031AB0 + car init 0x0002F260]
TEX_PAINT        = 0
TEX_UNDERSIDE    = 1
TEX_GLASS_OK     = 2
TEX_GLASS_CRACK  = 3
TEX_GLASS_SMASH  = 4

SLOT_APERTURE_BODY = 0
SLOT_PANEL_FIRST   = 1
SLOT_PANEL_LAST    = 6
SLOT_WHEEL_SLOW    = 7
SLOT_WHEEL_BLUR1   = 8
SLOT_WHEEL_BLUR2   = 9

# The most common shipped skin entry: bone 0, full weight.  [C survey]
DEFAULT_SKIN = b"\x00\x00\x00\x00\xff\x00\x00\x00"


class BgvError(Exception):
    pass


def _align(x, a):
    return (x + a - 1) & ~(a - 1)


# --------------------------------------------------------------------------
# Packed normal  (D3DVSDT_NORMPACKED3)  [C decl 0x00387558]
# --------------------------------------------------------------------------
def unpack_normal(w):
    """u32 -> (x, y, z).  Identical arithmetic to tools/extract_bgv.py."""
    x = w & 0x7FF
    y = (w >> 11) & 0x7FF
    z = (w >> 22) & 0x3FF
    if x & 0x400:
        x -= 0x800
    if y & 0x400:
        y -= 0x800
    if z & 0x200:
        z -= 0x400
    return (x / 1023.0, y / 1023.0, z / 511.0)


def pack_normal(nx, ny, nz):
    """(x, y, z) -> u32.  Inverse of unpack_normal.

    The x/y fields are 11-bit signed so they hold [-1024, 1023], but the
    decode divides by 1023, so the writer clamps to [-1023, 1023] (and z to
    [-511, 511]); that keeps pack(unpack(w)) == w for every value a decoder
    can produce and never emits a magnitude the game would read as > 1.0."""
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln < 1e-12:
        nx, ny, nz = 0.0, 1.0, 0.0
    else:
        nx, ny, nz = nx / ln, ny / ln, nz / ln
    xi = max(-1023, min(1023, int(round(nx * 1023.0))))
    yi = max(-1023, min(1023, int(round(ny * 1023.0))))
    zi = max(-511, min(511, int(round(nz * 511.0))))
    return ((xi & 0x7FF) | ((yi & 0x7FF) << 11) | ((zi & 0x3FF) << 22)) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# Triangle strips  (D3DPT_TRIANGLESTRIP, primitive 6)  [C FUN_001d7d10]
# --------------------------------------------------------------------------
def strip_to_triangles(indices):
    """The game's strip semantics, byte-for-byte the rule in
    tools/extract_bgv.py: degenerate triples are restarts, and every other
    triangle has its winding flipped by the hardware."""
    tris = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (a, c, b))
    return tris


def _rotations(t):
    a, b, c = t
    return ((a, b, c), (b, c, a), (c, a, b))


def triangles_to_strip(tris, reorder=False):
    """Inverse of strip_to_triangles: emit an index stream that decodes back
    to exactly `tris` -- same triangles, same orientation, and (with
    reorder=False) the same order.

    Extend the running strip whenever the next triangle shares the trailing
    edge in the orientation the current parity demands, otherwise bridge
    with degenerate triples.  Correctness never depends on finding an
    extension -- the bridge alone is always valid -- so validate_bgv.py
    asserts round-trip equality on random meshes as well as on every record
    of every shipped car.

    reorder=True lets the walk pick ANY not-yet-emitted triangle that
    continues the strip, which is what keeps the index stream near the size
    the shipping tool produced.  The triangle SET and every orientation are
    unchanged; only the order within the record moves, and a strip's order
    is fixed by the strip anyway."""
    if reorder:
        return _strip_greedy(tris)
    s = []
    for tri in tris:
        if not s:
            s.extend(tri)
            continue
        want = _extension(s, tri)
        if want is not None:
            s.append(want)
            continue
        _bridge(s, tri)
    return s


def _extension(s, tri):
    """The single index that appends `tri` to strip `s`, or None.

    The appended index lands at position len(s), so the new triangle starts
    at i = len(s) - 2 and strip_to_triangles will read it as (a, b, c) when
    i is even and (a, c, b) when i is odd."""
    i = len(s) - 2
    p, q = s[-2], s[-1]
    for a, b, c in _rotations(tri):
        if i % 2 == 0:
            if a == p and b == q:
                return c
        else:
            if a == p and c == q:
                return b
    return None


def _bridge(s, tri):
    """Restart the strip at `tri`.  Duplicating the tail and then the new
    head makes every intervening triple degenerate, which the decoder
    skips."""
    a, b, c = tri
    if s:
        s.append(s[-1])
        s.append(a)
        if len(s) % 2 == 0:
            s.extend((a, b, c))
        else:
            s.extend((a, c, b))
    else:
        s.extend((a, b, c))


def _strip_greedy(tris):
    """Same encoder, but free to choose which unemitted triangle comes next.

    Keyed on directed edges: at an even start position the next triangle
    must carry the directed edge (p, q) of the strip tail, at an odd one the
    reversed edge (q, p) -- both read straight off _extension above."""
    edge = {}
    for i, t in enumerate(tris):
        for a, b, c in _rotations(t):
            edge.setdefault((a, b), []).append(i)
    used = [False] * len(tris)
    s = []
    nxt = 0
    left = len(tris)
    while left > 0:
        if s:
            key = (s[-2], s[-1]) if (len(s) - 2) % 2 == 0 else (s[-1], s[-2])
            pick = None
            for i in edge.get(key, ()):
                if not used[i]:
                    pick = i
                    break
            if pick is not None:
                for a, b, c in _rotations(tris[pick]):
                    if (a, b) == key:
                        s.append(c)
                        break
                used[pick] = True
                left -= 1
                continue
        while nxt < len(tris) and used[nxt]:
            nxt += 1
        if nxt >= len(tris):
            break
        used[nxt] = True
        left -= 1
        _bridge(s, tris[nxt])
    return s


# --------------------------------------------------------------------------
# Axis conversion  (see the module docstring for the full derivation)
# --------------------------------------------------------------------------
#: nose direction, expressed in Blender's right-handed Z-up world.
AXIS_MODES = ("+Y", "-Y", "+X", "-X")


def axis_matrix(nose="+Y"):
    """Return the 3x3 row-major basis B with bgv = B @ blender.

    up_b    = (0,0,1)                 Blender is Z-up
    fwd_b   = the chosen nose direction
    right_b = cross(fwd_b, up_b)      the car's right, right-hand rule

    bgv.x = right_b . v,  bgv.y = up_b . v,  bgv.z = fwd_b . v

    det(B) == -1 for every mode (Blender RH -> .bgv LH), so the caller MUST
    reverse triangle winding; winding_flipped() says so explicitly."""
    if nose == "+Y":
        fwd = (0.0, 1.0, 0.0)
    elif nose == "-Y":
        fwd = (0.0, -1.0, 0.0)
    elif nose == "+X":
        fwd = (1.0, 0.0, 0.0)
    elif nose == "-X":
        fwd = (-1.0, 0.0, 0.0)
    else:
        raise BgvError("unknown nose axis %r (expected one of %s)"
                       % (nose, ", ".join(AXIS_MODES)))
    up = (0.0, 0.0, 1.0)
    right = (fwd[1] * up[2] - fwd[2] * up[1],
             fwd[2] * up[0] - fwd[0] * up[2],
             fwd[0] * up[1] - fwd[1] * up[0])
    return (right, up, fwd)


def axis_det(B):
    (a, b, c), (d, e, f), (g, h, i) = B
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def winding_flipped(B):
    """True when the basis reverses handedness, i.e. the exporter must emit
    (a, c, b) for a Blender triangle (a, b, c)."""
    return axis_det(B) < 0.0


def apply_axis(B, v):
    return (B[0][0] * v[0] + B[0][1] * v[1] + B[0][2] * v[2],
            B[1][0] * v[0] + B[1][1] * v[1] + B[1][2] * v[2],
            B[2][0] * v[0] + B[2][1] * v[1] + B[2][2] * v[2])


def blender_to_bgv(v, nose="+Y", scale=1.0):
    B = axis_matrix(nose)
    x, y, z = apply_axis(B, v)
    return (x * scale, y * scale, z * scale)


# --------------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------------
class Record(object):
    """One draw record: an index stream plus its shader/pass/texture keys."""

    __slots__ = ("indices", "mask", "tex_slot", "shader", "alpha0", "alpha1",
                 "unk00", "unk12", "unk1b")

    def __init__(self, indices, mask=MASK_BODY, tex_slot=TEX_PAINT,
                 shader=0, alpha0=1.0, alpha1=0.0,
                 unk00=0, unk12=0, unk1b=0):
        self.indices = list(indices)
        self.mask = mask
        self.tex_slot = tex_slot
        self.shader = shader
        self.alpha0 = alpha0
        self.alpha1 = alpha1
        self.unk00 = unk00
        self.unk12 = unk12
        self.unk1b = unk1b

    @property
    def triangles(self):
        return strip_to_triangles(self.indices)

    def fields(self):
        return (tuple(self.indices), self.mask, self.tex_slot, self.shader,
                _f32(self.alpha0), _f32(self.alpha1),
                self.unk00, self.unk12, self.unk1b)


class Part(object):
    """A part object: 0x10-byte header + N records."""

    __slots__ = ("records", "glass_hint")

    def __init__(self, records=None, glass_hint=None):
        self.records = list(records or ())
        #: the s8 at part+0x01.  None => derive it on write.
        self.glass_hint = glass_hint

    def derived_glass_hint(self):
        last = -1
        for i, r in enumerate(self.records):
            if r.mask & MASK_GLASS_A:
                last = i
        return last

    def hint(self):
        return self.derived_glass_hint() if self.glass_hint is None \
            else self.glass_hint

    def fields(self):
        return (self.hint(), tuple(r.fields() for r in self.records))


class Section(object):
    """One LOD."""

    __slots__ = ("slots", "embedded", "verts", "skins",
                 "vb_common", "vb2_common", "unk50", "unk5c")

    def __init__(self):
        self.slots = [None] * NUM_SLOTS        # Part or None
        self.embedded = Part()
        #: (x, y, z, packed_normal_u32, u, v)
        self.verts = []
        #: 8 raw bytes each
        self.skins = []
        self.vb_common = 1
        self.vb2_common = 1
        self.unk50 = 0
        self.unk5c = 0

    def parts(self):
        """Every part object, in the order the writer lays them out."""
        out = [(i, p) for i, p in enumerate(self.slots) if p is not None]
        out.append((-1, self.embedded))
        return out

    def max_index(self):
        m = -1
        for _, p in self.parts():
            for r in p.records:
                if r.indices:
                    m = max(m, max(r.indices))
        return m

    def max_slot_index(self):
        m = -1
        for p in self.slots:
            if p is None:
                continue
            for r in p.records:
                if r.indices:
                    m = max(m, max(r.indices))
        return m

    def fields(self):
        return (tuple((i, p.fields()) for i, p in self.parts()),
                tuple(self.verts), tuple(self.skins),
                self.vb_common, self.vb2_common, self.unk50, self.unk5c)


class Bgv(object):
    """A whole .bgv.

    `header` is the raw fixed 0x16F0-byte header; the accessors below give
    named views on it.  Regions I could not source ([?] in the docstring)
    survive verbatim through read -> write, which is why template mode can
    produce an in-game-loadable file for a mesh authored from nothing."""

    def __init__(self):
        self.header = bytearray(HEADER_SIZE)
        self.sections = []
        #: opaque, position-independent blobs.  Both are relocated by
        #: whole-blob shift only, which is safe because their internal
        #: relinkers (FUN_001c8e20 for the material directory, FUN_00159470
        #: for the +0x16DC pair) use SELF-relative offsets throughout. [C]
        self.matdir_blob = b""
        self.tail_blob = b""
        self.tail_origin = 0          # where tail_blob started in the donor
        #: zero bytes trimmed off the end of tail_blob (file padding to
        #: 0x800).  Re-emitted on write, so the trim is loss-free.
        self.tail_pad = 0
        #: non-zero bytes the structural model did not claim.  Empty for
        #: every shipped file; the validator reports the count.
        self.residual = []
        self._layout = None           # captured on read, for preserve mode

    # ---- header accessors -------------------------------------------
    def _u32(self, off):
        return struct.unpack_from("<I", self.header, off)[0]

    def _set_u32(self, off, v):
        struct.pack_into("<I", self.header, off, v & 0xFFFFFFFF)

    def _f32(self, off):
        return struct.unpack_from("<f", self.header, off)[0]

    @property
    def version(self):
        return self._u32(H_VERSION)

    @property
    def num_panels(self):
        return self.header[H_NUM_PANELS]

    @num_panels.setter
    def num_panels(self, v):
        self.header[H_NUM_PANELS] = v & 0xFF

    @property
    def num_wheels(self):
        return self.header[H_NUM_WHEELS]

    @num_wheels.setter
    def num_wheels(self, v):
        self.header[H_NUM_WHEELS] = v & 0xFF

    @property
    def wheel_radius(self):
        return self._f32(H_WHEEL_RADIUS)

    def panel_kinds(self):
        return [struct.unpack_from("<i", self.header, H_PANEL_KIND + 4 * i)[0]
                for i in range(self.num_panels)]

    def panel_axes(self):
        return list(self.header[H_PANEL_AXIS:H_PANEL_AXIS + self.num_panels])

    def wheel_matrix(self, w):
        return struct.unpack_from("<16f", self.header, H_WHEEL_MATS + w * 0x40)

    def panel_matrix(self, k):
        return struct.unpack_from("<16f", self.header, H_PANEL_MATS + k * 0x40)

    def bbmax(self):
        return struct.unpack_from("<4f", self.header, H_BBMAX)

    def bbmin(self):
        return struct.unpack_from("<4f", self.header, H_BBMIN)

    def set_body_box(self, bbmin, bbmax):
        """The collision box the game copies to veh+0x1D0 / +0x1E0.
        [C FUN_00122830; src/burnout3_carcol.h:88-89.  This IS the ".bgv
        body box" the AI's racecar+0x2444 car width comes from.]"""
        struct.pack_into("<4f", self.header, H_BBMAX,
                         bbmax[0], bbmax[1], bbmax[2],
                         bbmax[3] if len(bbmax) > 3 else 0.0)
        struct.pack_into("<4f", self.header, H_BBMIN,
                         bbmin[0], bbmin[1], bbmin[2],
                         bbmin[3] if len(bbmin) > 3 else 0.0)

    def hull_bytes(self):
        return bytes(self.header[H_HULL:H_HULL + H_HULL_SIZE])

    def hull_counts(self):
        """(nverts, nplanes, nedges) -- src/burnout3_carcol.h asserts these
        offsets against the retail struct."""
        b = H_HULL
        return (self.header[b + 0x18], self.header[b + 0x19],
                self.header[b + 0x1A])

    def fields(self):
        """Everything the recovered spec defines, as a comparable tuple.

        +0x08 (file size) and +0x4C.. (the section offsets) are DERIVED --
        the writer recomputes both -- so they are excluded; a re-laid-out
        file is field-identical while being a different length."""
        return (bytes(self.header[:H_FILESIZE])
                + bytes(self.header[H_FILESIZE + 4:H_SECTIONS]),
                bytes(self.header[H_DEFORM_MATS:H_LIGHT_PTRS]),
                bytes(self.header[H_HULL:H_HULL + H_HULL_SIZE]),
                tuple(None if s is None else s.fields()
                      for s in self.sections),
                self.matdir_blob, self.tail_blob)


def _f32(x):
    """Round-trip a Python float through f32 so field comparisons are exact."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


# --------------------------------------------------------------------------
# Reader
# --------------------------------------------------------------------------
def read_bgv(data):
    """Parse a .bgv into a Bgv.  Raises BgvError on anything the recovered
    spec says cannot happen."""
    data = bytes(data)
    if len(data) < HEADER_SIZE:
        raise BgvError("file shorter than the 0x16F0 header")

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    def u16(o):
        return struct.unpack_from("<H", data, o)[0]

    def s8(o):
        return struct.unpack_from("<b", data, o)[0]

    m = Bgv()
    m.header = bytearray(data[:HEADER_SIZE])
    if m.version != BGV_VERSION:
        raise BgvError("version 0x%X, expected 0x%X" % (m.version, BGV_VERSION))
    if u32(H_FILESIZE) != len(data):
        raise BgvError("header size 0x%X != file length 0x%X"
                       % (u32(H_FILESIZE), len(data)))

    covered = []                       # (start, end) claimed by the model
    layout = {"sections": [], "size": len(data)}

    def claim(a, b):
        if b > a:
            covered.append((a, b))

    claim(0, HEADER_SIZE)

    def read_part(p0, rec_base, key):
        """rec_base is what the record-array offset at +4 is relative to:
        the part header for normal parts, S+0x60 for the embedded one. [C]"""
        cnt = s8(p0)
        if cnt < 0:
            raise BgvError("negative record count at 0x%X" % p0)
        rel = u32(p0 + 4)
        arr = rec_base + rel
        part = Part(glass_hint=s8(p0 + 1))
        claim(p0, p0 + PART_HDR_SIZE)
        claim(arr, arr + cnt * RECORD_SIZE)
        lay = {"hdr": p0, "recs": arr, "rel": rel, "idx": []}
        for i in range(cnt):
            r = arr + i * RECORD_SIZE
            if r + RECORD_SIZE > len(data):
                raise BgvError("record %d of part 0x%X out of file" % (i, p0))
            n = u16(r + R_INDEX_COUNT)
            ioff = u32(r + R_INDEX_OFF)
            src = r + ioff
            if src + 2 * n > len(data):
                raise BgvError("index stream of record %d at 0x%X out of file"
                               % (i, r))
            idx = list(struct.unpack_from("<%dH" % n, data, src)) if n else []
            part.records.append(Record(
                idx,
                mask=u16(r + R_MASK),
                tex_slot=data[r + R_TEXSLOT],
                shader=u32(r + R_SHADER),
                alpha0=struct.unpack_from("<f", data, r + R_ALPHA0)[0],
                alpha1=struct.unpack_from("<f", data, r + R_ALPHA1)[0],
                unk00=u32(r + R_UNK00),
                unk12=u16(r + R_UNK12),
                unk1b=data[r + R_UNK1B]))
            claim(src, src + 2 * n)
            lay["idx"].append(src)
        layout[key] = lay
        return part

    sec_offs = [u32(H_SECTIONS + 4 * i) for i in range(MAX_SECTIONS)]
    for si, S in enumerate(sec_offs):
        if S == 0:
            m.sections.append(None)
            layout["sections"].append(None)
            continue
        if S + SECTION_HDR_SIZE > len(data):
            raise BgvError("section %d header out of file" % si)
        sec = Section()
        sec.vb_common = u32(S + S_VB_COMMON)
        sec.vb2_common = u32(S + S_VB2_COMMON)
        sec.unk50 = u32(S + S_UNK50)
        sec.unk5c = u32(S + S_UNK5C)
        claim(S, S + SECTION_HDR_SIZE)
        for sl in range(NUM_SLOTS):
            off = u32(S + S_SLOTS + 4 * sl)
            if off:
                sec.slots[sl] = read_part(S + off, S + off, ("sec", si, sl))
        sec.embedded = read_part(S + S_EMB, S + S_EMB, ("sec", si, "emb"))

        nvert = sec.max_index() + 1
        nskin = sec.max_slot_index() + 1
        pool0 = S + u32(S + S_POOL0)
        pool1 = S + u32(S + S_POOL1)
        if pool0 + nvert * VERTEX_STRIDE > len(data):
            raise BgvError("section %d position pool out of file" % si)
        if pool1 + nskin * SKIN_STRIDE > len(data):
            raise BgvError("section %d skin pool out of file" % si)
        # The pool0->pool1 distance is exactly 0x18 * nvert in every shipped
        # section; treat a mismatch as a spec violation so the validator
        # catches a bad derivation rather than silently mis-sizing.
        if pool1 - pool0 != nvert * VERTEX_STRIDE:
            raise BgvError(
                "section %d: pool1-pool0 = 0x%X, expected 0x%X for %d verts"
                % (si, pool1 - pool0, nvert * VERTEX_STRIDE, nvert))
        for i in range(nvert):
            o = pool0 + i * VERTEX_STRIDE
            x, y, z = struct.unpack_from("<3f", data, o)
            nw = u32(o + 0x0C)
            u, v = struct.unpack_from("<2f", data, o + 0x10)
            sec.verts.append((x, y, z, nw, u, v))
        for i in range(nskin):
            o = pool1 + i * SKIN_STRIDE
            sec.skins.append(data[o:o + SKIN_STRIDE])
        claim(pool0, pool0 + nvert * VERTEX_STRIDE)
        claim(pool1, pool1 + nskin * SKIN_STRIDE)
        layout["sections"].append({"S": S, "pool0": pool0, "pool1": pool1})
        m.sections.append(sec)

    # ---- the two opaque, position-independent blobs -------------------
    covered.sort()
    merged = []
    for a, b in covered:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))

    matdir = u32(H_MATDIR)
    if matdir:
        # extends to the next claimed structure (or EOF)
        end = len(data)
        for a, b in merged:
            if a >= matdir:
                end = a
                break
        m.matdir_blob = data[matdir:end]
        claim(matdir, end)
        layout["matdir"] = matdir

    body_end = max(b for _, b in merged)
    # Advance over zero padding to a 4-byte boundary so the tail blob keeps
    # the internal alignment of the part objects and index streams it
    # contains: relocating it is a whole-blob shift, and the shift must be a
    # multiple of 4.  COMP/Car1's last index stream ends at 0x8DABE and its
    # first tail structure is at 0x8DAC0.
    while body_end % 4 and body_end < len(data) and data[body_end] == 0:
        body_end += 1
    if body_end % 4:
        raise BgvError("the tail blob starts at 0x%X, which is not 4-aligned; "
                       "relocating it would misalign its part objects"
                       % body_end)
    m.tail_origin = body_end
    tail = data[body_end:]
    stripped = tail.rstrip(b"\x00")
    m.tail_blob = stripped
    m.tail_pad = len(tail) - len(stripped)
    claim(body_end, len(data))

    # everything reachable from the header past the sections must live in
    # the tail blob, or a whole-blob shift would break it.
    tail_ptrs = [H_AUX_PART[0], H_AUX_PART[1], H_TAIL_PTR_A, H_TAIL_PTR_B,
                 H_EXTRA_PTRS, H_EXTRA_PTRS + 4] + \
                [H_LIGHT_PTRS + 4 * i for i in range(18)]
    for off in tail_ptrs:
        p = u32(off)
        if p and not (body_end <= p < len(data)):
            raise BgvError("pointer at +0x%X -> 0x%X is outside the tail blob "
                           "[0x%X, 0x%X)" % (off, p, body_end, len(data)))
    if u32(H_TAIL_PTR_A):
        # [C FUN_000310f0] the stride-0xC array's +8 field is base-relative
        # too.  Never exercised by a shipped file (0 on all 67); refuse
        # rather than silently emit a broken relocation.
        raise BgvError("+0x16C0 sub-array is non-zero; its +8 pointers are "
                       "base-relative and this writer does not fix them up")

    # ---- residual (non-zero bytes the model did not claim) ------------
    covered.sort()
    merged = []
    for a, b in covered:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    prev = 0
    for a, b in merged:
        if a > prev:
            chunk = data[prev:a]
            if any(chunk):
                m.residual.append((prev, chunk))
        prev = max(prev, b)
    if prev < len(data) and any(data[prev:]):
        m.residual.append((prev, data[prev:]))

    m._layout = layout
    return m


# --------------------------------------------------------------------------
# Writer
# --------------------------------------------------------------------------
def write_bgv(m, preserve_layout=False):
    """Serialise a Bgv.  Every relocatable offset is recomputed from scratch.

    preserve_layout=True reuses the byte offsets captured by read_bgv, which
    reproduces a donor file exactly; it is only valid when nothing about the
    geometry changed and is what validate_bgv.py's byte-compare test uses.
    The default (False) regenerates the layout with the policy below, which
    is what an export from Blender takes."""
    if preserve_layout and m._layout is None:
        raise BgvError("preserve_layout needs a model produced by read_bgv")

    for si, sec in enumerate(m.sections):
        if sec is None:
            continue
        nvert = sec.max_index() + 1
        if nvert > len(sec.verts):
            raise BgvError("section %d indexes vertex %d but the pool holds %d"
                           % (si, nvert - 1, len(sec.verts)))
        if len(sec.verts) > 0x10000:
            raise BgvError("section %d has %d vertices; the u16 index stream "
                           "tops out at 65536" % (si, len(sec.verts)))
        nskin = sec.max_slot_index() + 1
        if nskin > len(sec.skins):
            raise BgvError("section %d slots index skin %d but the stream "
                           "holds %d" % (si, nskin - 1, len(sec.skins)))
        for _, p in sec.parts():
            if len(p.records) > 127:
                raise BgvError("part in section %d has %d records; the count "
                               "is an s8" % (si, len(p.records)))
            for r in p.records:
                if len(r.indices) > 0xFFFF:
                    raise BgvError("record index count %d exceeds the u16 "
                                   "field" % len(r.indices))

    plan = _plan_layout(m, preserve_layout)
    buf = bytearray(plan["size"])
    buf[:HEADER_SIZE] = m.header

    def pu32(o, v):
        struct.pack_into("<I", buf, o, v & 0xFFFFFFFF)

    for si in range(MAX_SECTIONS):
        pu32(H_SECTIONS + 4 * si, 0)

    for si, sec in enumerate(m.sections):
        if sec is None:
            continue
        L = plan["sections"][si]
        S = L["S"]
        pu32(H_SECTIONS + 4 * si, S)
        for sl in range(NUM_SLOTS):
            pu32(S + S_SLOTS + 4 * sl,
                 (L["parts"][sl]["hdr"] - S) if sec.slots[sl] else 0)
        pu32(S + S_VB_COMMON, sec.vb_common)
        pu32(S + S_POOL0, L["pool0"] - S)
        pu32(S + S_UNK50, sec.unk50)
        pu32(S + S_VB2_COMMON, sec.vb2_common)
        pu32(S + S_POOL1, L["pool1"] - S)
        pu32(S + S_UNK5C, sec.unk5c)
        pu32(S + 0x68, 0)
        pu32(S + 0x6C, 0)

        for sl in range(NUM_SLOTS):
            if sec.slots[sl] is not None:
                _emit_part(buf, sec.slots[sl], L["parts"][sl],
                           rec_base=L["parts"][sl]["hdr"])
        _emit_part(buf, sec.embedded, L["emb"], rec_base=S + S_EMB)

        p0 = L["pool0"]
        for i, v in enumerate(sec.verts):
            o = p0 + i * VERTEX_STRIDE
            struct.pack_into("<3fI2f", buf, o,
                             v[0], v[1], v[2], v[3] & 0xFFFFFFFF, v[4], v[5])
        p1 = L["pool1"]
        for i, sk in enumerate(sec.skins):
            if len(sk) != SKIN_STRIDE:
                raise BgvError("skin entry %d is %d bytes, expected 8"
                               % (i, len(sk)))
            buf[p1 + i * SKIN_STRIDE:p1 + (i + 1) * SKIN_STRIDE] = sk

    # blobs
    if m.matdir_blob:
        o = plan["matdir"]
        buf[o:o + len(m.matdir_blob)] = m.matdir_blob
        pu32(H_MATDIR, o)
    else:
        pu32(H_MATDIR, 0)

    to = plan["tail"]
    buf[to:to + len(m.tail_blob)] = m.tail_blob
    delta = to - m.tail_origin
    for off in [H_AUX_PART[0], H_AUX_PART[1], H_TAIL_PTR_A, H_TAIL_PTR_B,
                H_EXTRA_PTRS, H_EXTRA_PTRS + 4] + \
               [H_LIGHT_PTRS + 4 * i for i in range(18)]:
        p = struct.unpack_from("<I", m.header, off)[0]
        pu32(off, p + delta if p else 0)

    if preserve_layout:
        for off, chunk in m.residual:
            buf[off:off + len(chunk)] = chunk

    pu32(H_FILESIZE, len(buf))
    return bytes(buf)


def _emit_part(buf, part, lay, rec_base):
    p0 = lay["hdr"]
    arr = lay["recs"]
    buf[p0] = len(part.records) & 0xFF
    struct.pack_into("<b", buf, p0 + 1, part.hint())
    buf[p0 + 2] = 0
    buf[p0 + 3] = 0
    struct.pack_into("<I", buf, p0 + 4, arr - rec_base)
    for i in range(8):
        buf[p0 + 8 + i] = 0
    for i, r in enumerate(part.records):
        o = arr + i * RECORD_SIZE
        src = lay["idx"][i]
        struct.pack_into("<I", buf, o + R_UNK00, r.unk00 & 0xFFFFFFFF)
        struct.pack_into("<f", buf, o + R_ALPHA0, r.alpha0)
        struct.pack_into("<f", buf, o + R_ALPHA1, r.alpha1)
        struct.pack_into("<I", buf, o + R_INDEX_OFF, (src - o) & 0xFFFFFFFF)
        struct.pack_into("<H", buf, o + R_INDEX_COUNT, len(r.indices))
        struct.pack_into("<H", buf, o + R_UNK12, r.unk12 & 0xFFFF)
        struct.pack_into("<I", buf, o + R_SHADER, r.shader & 0xFFFFFFFF)
        struct.pack_into("<H", buf, o + R_MASK, r.mask & 0xFFFF)
        buf[o + R_TEXSLOT] = r.tex_slot & 0xFF
        buf[o + R_UNK1B] = r.unk1b & 0xFF
        if r.indices:
            struct.pack_into("<%dH" % len(r.indices), buf, src, *r.indices)


def _plan_layout(m, preserve):
    """Decide where everything goes.

    Fresh policy, per section, in this order -- which is the order the
    shipped files use inside a section (verified on COMP/Car1 LOD1..3):
        section header (0x70, embedded part header inline at S+0x60)
        slot part headers + record arrays, ascending slot
        embedded record array
        every index stream, 4-byte aligned, in part then record order
        position pool  (4-byte aligned)
        skin pool
    then the material-directory blob, then the tail blob, then pad the file
    to 0x800 -- every shipped file's length is a multiple of 0x800 and
    +0x08 carries that padded length. [C survey]"""
    if preserve:
        lay = m._layout
        plan = {"size": lay["size"], "sections": [None] * MAX_SECTIONS,
                "matdir": lay.get("matdir", 0), "tail": m.tail_origin}
        for si, sec in enumerate(m.sections):
            if sec is None:
                continue
            sl = lay["sections"][si]
            entry = {"S": sl["S"], "pool0": sl["pool0"], "pool1": sl["pool1"],
                     "parts": [None] * NUM_SLOTS,
                     "emb": lay[("sec", si, "emb")]}
            for k in range(NUM_SLOTS):
                if sec.slots[k] is not None:
                    entry["parts"][k] = lay[("sec", si, k)]
            plan["sections"][si] = entry
        return plan

    off = HEADER_SIZE
    plan = {"sections": [None] * MAX_SECTIONS}
    first_section = next((i for i, s in enumerate(m.sections) if s is not None),
                         None)
    for si, sec in enumerate(m.sections):
        if sec is None:
            continue
        S = _align(off, 4)
        entry = {"S": S, "parts": [None] * NUM_SLOTS}
        cur = S + SECTION_HDR_SIZE
        for sl in range(NUM_SLOTS):
            p = sec.slots[sl]
            if p is None:
                continue
            entry["parts"][sl] = {"hdr": cur, "recs": cur + PART_HDR_SIZE,
                                  "idx": []}
            cur += PART_HDR_SIZE + len(p.records) * RECORD_SIZE
        entry["emb"] = {"hdr": S + S_EMB, "recs": cur, "idx": []}
        cur += len(sec.embedded.records) * RECORD_SIZE
        for sl in range(NUM_SLOTS):
            p = sec.slots[sl]
            if p is None:
                continue
            for r in p.records:
                cur = _align(cur, 4)
                entry["parts"][sl]["idx"].append(cur)
                cur += 2 * len(r.indices)
        for r in sec.embedded.records:
            cur = _align(cur, 4)
            entry["emb"]["idx"].append(cur)
            cur += 2 * len(r.indices)
        if si == first_section and m.matdir_blob:
            # Keep the material/texture blob where retail keeps it: inside
            # the first LOD's span, between its index streams and its vertex
            # pool (COMP/Car1 has it at 0x1C80, section 0 runs 0x16F0..
            # 0x2764C).  Placing it here also makes the extent unambiguous
            # on re-read -- the blob runs to the next parsed structure,
            # which is pool0, with no gap.
            # retail puts it on a 0x80 boundary (COMP/Car1: 0x1C80), which
            # matters for swizzled texture data; the trailing adjustment
            # keeps pool0 4-aligned when the blob length is odd.
            cur = _align(cur, 0x80) + (-len(m.matdir_blob)) % 4
            plan["matdir"] = cur
            cur += len(m.matdir_blob)
        entry["pool0"] = cur
        cur += len(sec.verts) * VERTEX_STRIDE
        entry["pool1"] = cur
        cur += len(sec.skins) * SKIN_STRIDE
        plan["sections"][si] = entry
        off = cur

    if m.matdir_blob and "matdir" not in plan:
        raise BgvError("a material directory blob needs at least one section")
    plan.setdefault("matdir", 0)
    # The tail must start exactly where read_bgv will look for it: the end
    # of the last parsed structure, rounded up to 4 (see read_bgv).
    off = _align(off, 4)
    plan["tail"] = off
    off += len(m.tail_blob)
    plan["size"] = max(_align(off, FILE_ALIGN), off + m.tail_pad)
    plan["size"] = _align(plan["size"], FILE_ALIGN)
    return plan


# --------------------------------------------------------------------------
# Building a model from mesh data
# --------------------------------------------------------------------------
class MeshGroup(object):
    """One export group: a triangle soup plus the record keys it becomes.

    verts is a list of (x, y, z, nx, ny, nz, u, v) ALREADY in .bgv space.
    tris  is a list of (i, j, k) into verts, already in .bgv winding."""

    __slots__ = ("name", "verts", "tris", "slot", "mask", "tex_slot",
                 "shader", "alpha0", "alpha1")

    def __init__(self, name, verts, tris, slot=SLOT_APERTURE_BODY,
                 mask=MASK_BODY, tex_slot=TEX_PAINT, shader=0,
                 alpha0=1.0, alpha1=0.0):
        self.name = name
        self.verts = verts
        self.tris = tris
        self.slot = slot
        self.mask = mask
        self.tex_slot = tex_slot
        self.shader = shader
        self.alpha0 = alpha0
        self.alpha1 = alpha1


def build_section(groups, skin=None, reorder=True):
    """Assemble one LOD from MeshGroups.

    Vertex numbering follows the shipped files: every slot's vertices come
    first, contiguously and in ascending slot order, and only then the
    vertices that exist solely in the embedded intact part.  That ordering
    is what makes the skin stream (which covers exactly the slot range) a
    prefix of the position pool. [C: COMP/Car1 slot ranges are
    0..156 / 157..172 / ... / 296..320, then the embedded part adds
    321..409]"""
    sec = Section()
    pool = []
    per_group = []
    ordered = sorted(groups, key=lambda g: (g.slot if g.slot >= 0 else 99))
    for g in ordered:
        base = len(pool)
        pool.extend(g.verts)
        per_group.append((g, base))

    for g, base in per_group:
        idx = triangles_to_strip([(a + base, b + base, c + base)
                                  for (a, b, c) in g.tris], reorder=reorder)
        rec = Record(idx, mask=g.mask, tex_slot=g.tex_slot, shader=g.shader,
                     alpha0=g.alpha0, alpha1=g.alpha1)
        if g.slot < 0:
            sec.embedded.records.append(rec)
        else:
            if sec.slots[g.slot] is None:
                sec.slots[g.slot] = Part()
            sec.slots[g.slot].records.append(rec)
        # Every slot group is also drawn as part of the one-piece intact
        # car, which is the single mask-0x3FF draw the game makes for an
        # undamaged vehicle. [C tools/extract_bgv.py, trace_panels.py --deep]
        if g.slot >= 0:
            sec.embedded.records.append(Record(
                list(idx), mask=g.mask, tex_slot=g.tex_slot,
                shader=g.shader, alpha0=g.alpha0, alpha1=g.alpha1))

    for v in pool:
        sec.verts.append((v[0], v[1], v[2],
                          pack_normal(v[3], v[4], v[5]), v[6], v[7]))
    nskin = sec.max_slot_index() + 1
    for i in range(nskin):
        sec.skins.append(skin[i] if skin and i < len(skin) else DEFAULT_SKIN)
    return sec


def build_bgv(groups, template=None, lod_count=4, num_panels=None,
              num_wheels=None, body_box=True):
    """Build a whole .bgv from MeshGroups.

    template: a Bgv from read_bgv().  Everything the exporter cannot author
    -- the material/texture directory, the collision hull, the deformation,
    wheel and panel matrices, the panel kinds and hinge data, and the [?]
    header regions -- is taken from it verbatim.  Without a template those
    become the conservative defaults documented below, and the resulting
    file is structurally valid but has no paint texture and only a box
    collision hull."""
    m = Bgv()
    if template is not None:
        m.header = bytearray(template.header)
        m.matdir_blob = template.matdir_blob
        m.tail_blob = template.tail_blob
        m.tail_origin = template.tail_origin
        m.tail_pad = template.tail_pad
    else:
        _init_default_header(m)

    if num_panels is not None:
        m.num_panels = num_panels
    if num_wheels is not None:
        m.num_wheels = num_wheels

    sec = build_section(groups)
    m.sections = [sec]
    for _ in range(1, max(1, lod_count)):
        m.sections.append(_clone_section(sec))
    while len(m.sections) < MAX_SECTIONS:
        m.sections.append(None)

    if body_box:
        lo = [1e30] * 3
        hi = [-1e30] * 3
        for v in sec.verts:
            for k in range(3):
                lo[k] = min(lo[k], v[k])
                hi[k] = max(hi[k], v[k])
        if lo[0] <= hi[0]:
            m.set_body_box(lo + [0.0], hi + [0.0])
    return m


def _clone_section(sec):
    out = Section()
    out.vb_common, out.vb2_common = sec.vb_common, sec.vb2_common
    out.unk50, out.unk5c = sec.unk50, sec.unk5c
    out.verts = list(sec.verts)
    out.skins = list(sec.skins)
    for i, p in enumerate(sec.slots):
        if p is None:
            continue
        out.slots[i] = Part([_clone_record(r) for r in p.records],
                            glass_hint=p.glass_hint)
    out.embedded = Part([_clone_record(r) for r in sec.embedded.records],
                        glass_hint=sec.embedded.glass_hint)
    return out


def _clone_record(r):
    return Record(list(r.indices), mask=r.mask, tex_slot=r.tex_slot,
                  shader=r.shader, alpha0=r.alpha0, alpha1=r.alpha1,
                  unk00=r.unk00, unk12=r.unk12, unk1b=r.unk1b)


def _init_default_header(m):
    """Template-free defaults.  Everything here is either [C] (a value the
    survey found identical in all 67 shipped files) or a documented
    conservative choice for a field I could not source -- never a guess
    dressed up as data."""
    h = m.header
    struct.pack_into("<I", h, H_VERSION, BGV_VERSION)       # [C] 0x17 on 67/67
    struct.pack_into("<I", h, H_UNK04, 0)                   # [C] 0 on 67/67
    struct.pack_into("<I", h, H_KIND, 3)                    # [C] 3 on 67/67
    h[H_NUM_PANELS] = 0                                     # no damage panels
    h[H_NUM_WHEELS] = 0
    struct.pack_into("<f", h, H_BODY_SCALAR, 0.0)           # [?] unsourced
    struct.pack_into("<f", h, H_WHEEL_RADIUS, 0.0)
    for w in range(MAX_WHEELS):
        struct.pack_into("<f", h, H_WHEEL_SCALE + 4 * w, 1.0)  # [C] 1.0 shipped
    # identity for every matrix bank; a non-identity guess would silently
    # deform or displace the model.
    for base, n in ((H_DEFORM_MATS, 8), (H_WHEEL_MATS, MAX_WHEELS),
                    (H_PANEL_MATS, MAX_PANELS)):
        for i in range(n):
            struct.pack_into("<16f", h, base + i * 0x40,
                             1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)
    # The collision hull at +0x1060: five SELF-relative pointers to the same
    # sub-arrays the retail record uses (src/burnout3_carcol.h asserts the
    # offsets).  Counts left at 0 -- an empty hull, which is exactly the
    # case b3_carcol_hull_from_extents() covers by falling back to the
    # +0xE80/+0xE90 box.  Fabricating plane/vertex data here would be
    # inventing collision geometry.
    for i, rel in enumerate((0x1C, 0xA0, 0x320, 0x480, 0x4F8)):
        struct.pack_into("<I", h, H_HULL + 4 * i, rel)


# --------------------------------------------------------------------------
def read_bgv_file(path):
    with open(path, "rb") as f:
        return read_bgv(f.read())


def write_bgv_file(path, m, preserve_layout=False):
    data = write_bgv(m, preserve_layout=preserve_layout)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)
