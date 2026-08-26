/* cx_track_mesh.c -- port of tools/extract_track.py.
 *
 * Burnout 3 Xbox track geometry, static.dat + streamed.dat -> Wavefront OBJ.
 * The python original is THE SPEC and carries the full recovery; the [C]/[S]
 * citations below are condensed from it.  Format credit: the Burnout Modding
 * community, via EdnessP's Noesis plugin `fmt_Burnout3LRD.py`.
 *
 * static.dat header (little-endian)
 *   +0x00 u32 version (0x27; <= 0x25 is the demo/prealpha layout)
 *   +0x04 u32 file size          +0x08 i32 material table  +0x0C u16 count
 *   +0x0E u16 animated-material count  +0x10 i32 animated index table (u16 ids)
 *   +0x1C u16[4] group counts: backdrop, chevron, water, reflection
 *   +0x24 i32[4] group table offsets (absolute)
 *   +0x54 u16 streamed unit count      +0x58 i32 streamed unit table
 *
 * group entry (8 bytes): u16 submodel count, u16 pad, i32 model offset
 *   RELATIVE to this entry.
 * submodel (stride 0x60: 0x40 of OBB floats then 0x20 of info)
 *   +0x40 u32 must be 1     +0x44 i32 vertex offset, relative to +0x40
 *   +0x4C i32 submesh offset, relative to +0x40   +0x50 u32 submesh count
 *   +0x54 i32 material index array (u16 per submesh), relative to the GROUP
 *         entry -- static groups only; streamed units use the PVS block.
 * submesh entry (stride 0x90)
 *   +0x80 u32 triangle format: 6 = strip, 2 = line   +0x84 u32 index count
 *   +0x88 i32 index offset, relative to the submesh entry
 *   +0x8C i8  NEXT submesh index in this material slot's chain, < 0 = end.
 *         [C] FUN_001AD510 @0x001AD691: the per-slot draw loop reads the head
 *         out of the PVS chunk then walks `movsx eax, byte [submesh+0x8C]`.
 * vertex (stride 0x1C)                                                     [C]
 *   +0x00 f32[3] position
 *   +0x0C u32 D3DVSDT_NORMPACKED3 normal: x = bits 0..10 signed / 1023,
 *         y = bits 11..21 signed / 1023, z = bits 22..31 signed / 511.  The
 *         split is pinned by the D3D vertex declaration at 0x0038758C and by
 *         |n| = 1.0000 +/- 0.0004 over every vertex measured.
 *   +0x10 u8[4] D3DCOLOR diffuse, BGRA.  RGB is HALF RANGE (128 = white; the
 *         world pixel shaders all carry PS_COMBINEROUTPUT_SHIFTLEFT_1 on
 *         stage 0).  A is the specular gate.
 *   +0x14 f32[2] uv -- v is a D3D texture coordinate and is emitted UNCHANGED.
 *         game v == D3D v == GL t == PNG row v*height, row 0 = top.  A 1-v
 *         here is what put every sign in the world upside down.            [C]
 * Vertex data for a model runs from vtxOffset up to the first submesh's index
 * data, so the vertex count is derived from that span.
 *
 * Material record (stride 0x28): +0x00 u32 shader class, +0x04 f32 specular
 * strength, +0x08 f32 specular power, +0x0C ptr -> ptr ARRAY of texture
 * records (one per animation frame; name at rec+0x48 if bit depth +0x40 is
 * 4/8/32, else +0x44), +0x10 u8 frame index, +0x11 u8 frame count, +0x12 u8
 * current label, +0x13 i8 frame step, +0x14 f32 rate/period, +0x18 f32
 * step/hold, +0x1C f32 UV-scroll phase, +0x20 f32 alpha scalar (= C0.a),
 * +0x24 u32 FLAGS.  Flag bits used here, all from the two material-apply
 * functions FUN_0003A3C0 (static) and FUN_000393C0 (streamed):
 *   0x001 D3DRS_ALPHABLENDENABLE (RS 0x3B) @0x00039B21                     [C]
 *   0x010 D3DRS_ALPHATESTENABLE  (RS 0x3C) @0x00039BD4, GREATER 64/255     [C]
 *   0x020 D3DRS_CULLMODE := D3DCULL_NONE   @0x0003A674 -- TWO-SIDED        [C]
 *   0x040 class 1/7 vertex-program swap: oD0.w = v3.w * r1.z @0x000394EA   [C]
 *   0x100 frame ping-pong (FUN_0019B1E0; unset on every shipped material)  [C]
 *   0x200 UV scroll, vertex-shader constant 0x63 @0x00039CE0               [C]
 *   0x400 DECAL: D3DRS_ZWRITEENABLE := 0    @0x00039AF5                    [C]
 * Class 2 is the mirror-only material; FUN_001AD350 SKIPS static submeshes
 * whose material class == 2 (test at 0x001AD40D).  The streamed path has no
 * such test, so the skip is applied to the static groups only.             [C]
 *
 * Streamed units: static.dat +0x54/+0x58 -> 0x10-byte entries {i32 blockA_off,
 * i32 blockB_off, u32 blockA_size, u32 blockB_size} into streamed.dat.  A
 * block's model is one submodel at blockOff + 0x10 + 0x40 (call that `base`);
 * the PVS block follows at X = base + 0x20, and the runtime base is X - 0x70:
 *   pvsRT+0x71 u8  material-slot count            [C] 0x001ADB7E
 *   pvsRT+0x73 u8  FIRST ALPHA SLOT for blockA    [C] 0x001AD8AF
 *   pvsRT+0x74 i8[8] backdrop group indices, <0 = none   [C] 0x001AD976
 *   pvsRT+0x7D/7E/7F i8 chevron / water / reflection group index
 *   pvsRT+0x84 u16[] material id per slot         [C]
 *   pvsRT+0x12A + c*0xA9 3-byte chunk header ([0] = signed unit offset,
 *                        0 = self)                [C] 0x0019D1F7
 *   pvsRT+0x12D + c*0xA9 + slot i8 head submesh index, blockA model
 * 17 chunks, stride 0xA9.
 *
 * FRAME ORDER, from the world draw FUN_001AE340: streamed opaque -> per-cell
 * backdrop groups -> water/reflection -> streamed alpha -> chevron group.  The
 * harness bakes one display list, so the per-cell selection collapses to "all
 * of them", but the pass order is reproduced exactly.  Submeshes are emitted
 * in material-SLOT order within their pass, which is the game's own order --
 * index order put the road decals BEFORE the road they sit on.             [C]
 *
 * The reflection group duplicates nearby geometry for the game's mirror pass;
 * drawn directly it z-fights the real surfaces, so it is skipped.
 */
#include "cx_common_b.h"
#include "cx_extract.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VTX_STRIDE 0x1C
#define SUB_STRIDE 0x90
#define MDL_STRIDE 0x60
#define MAT_STRIDE 0x28
#define TRI_STRIP  0x6

#define MAT_ALPHA_BLEND    0x001u   /* RS 0x3B := 1                       [C] */
#define MAT_TWO_SIDED      0x020u   /* D3DRS_CULLMODE := D3DCULL_NONE     [C] */
#define MAT_ALPHA_TEST     0x010u   /* RS 0x3C := 1                       [C] */
#define MAT_SPEC_GATE      0x040u   /* class 1/7 VS variant: oD0.w *= v3.w [C] */
#define MAT_FRAME_PINGPONG 0x100u   /* FUN_0019B1E0 ping-pong arm         [C] */
#define MAT_UV_SCROLL      0x200u   /* vertex shader constant 0x63        [C] */
#define MAT_DECAL          0x400u   /* D3DRS_ZWRITEENABLE := 0            [C] */
#define MAT_CLASS_MIRROR   2u       /* skipped by FUN_001AD350            [C] */
#define MAT_CLASS_EMISSIVE 10u

/* PVS block field offsets relative to X = submodel + 0x40 + 0x20. */
#define PVS_NSLOT_B       0x01
#define PVS_NSLOT_OPAQUE  0x03
#define PVS_MATTAB        0x14
#define PVS_CHUNK0        0xBA
#define PVS_CHUNK_STRIDE  0xA9
#define PVS_CHUNK_HEADS_A 3
#define PVS_CHUNKS        17
#define SUB_NEXT_OFF      0x8C

static const char *GROUP_NAMES[4] = { "backdrop", "chevron", "water",
                                      "reflection" };

/* ---- the fixed OBJ / MTL preamble text, verbatim from write_obj() ------- */
static const char MTL_HEADER_A[] =
    "# Burnout 3 materials from static.dat.\n"
    "# 'two_sided' records the game material flag +0x24 bit 0x20\n"
    "# (D3DRS_CULLMODE := D3DCULL_NONE, FUN_0003A3C0 @0x0003A674).\n"
    "# It is documentation only: those submeshes are already\n"
    "# emitted with reverse-wound duplicates in the OBJ.\n"
    "# 'decal' records material flag bit 0x400 -- the road\n"
    "# markings/overlay layer, which the game draws with\n"
    "# D3DRS_ZWRITEENABLE (render state 64) := 0\n"
    "# (FUN_000393C0 @0x00039AF5..0x00039B1B).\n"
    "# 'alpha' records that the material sat at or above the\n"
    "# streamed unit's alpha cut (PVS byte pvsRT+0x73), i.e. the\n"
    "# game draws it in the separate transparent pass\n"
    "# FUN_001ADD60 that runs after ALL opaque world geometry.\n"
    "# Submeshes are already emitted here in the game's own pass\n"
    "# and material-slot order; src/burnout3_trackmesh.c reads\n"
    "# the decal line to hoist the decal groups behind the whole\n"
    "# world, because the harness bakes one display list with no\n"
    "# per-cell visibility.\n";

/* split in two: a single string literal of 4134 bytes exceeds the 4095 that
 * C99 requires a compiler to support (-Woverlength-strings). */
static const char MTL_HEADER_A2[] =
    "#\n"
    "# 'class' is the material's shader class (+0x00) -- see the\n"
    "# 'Shader classes' section of tools/extract_track.py for the\n"
    "# recovered per-class colour equation.\n"
    "# 'alpha_blend' / 'alpha_test' are D3DRS_ALPHABLENDENABLE\n"
    "# (RS 0x3B) and D3DRS_ALPHATESTENABLE (RS 0x3C), which\n"
    "# FUN_000393C0 computes as flags&0x001 (@0x00039B21) and\n"
    "# flags&0x010 (@0x00039BD4).  NOTE THE ORDER: the two were\n"
    "# swapped in this file until 2026-08-12 -- see MAT_ALPHA_BLEND\n"
    "# for the render-state identification.\n"
    "# THEY ARE THE ONLY THING THAT MAKES A TEXTURE'S ALPHA MEAN\n"
    "# TRANSPARENCY. On a class 0/1/7/10 material the alpha\n"
    "# channel is a specular/emissive MASK and the surface is\n"
    "# opaque; guessing from texel statistics alpha-tested away\n"
    "# 95% of Silver Lake's dirt road (the 'blue road' bug).\n"
    "# The cut-out test itself is GREATER, 64/255 -- the world\n"
    "# setup FUN_00038D10 writes D3DRS_ALPHAFUNC := D3DCMP_GREATER\n"
    "# (@0x0003901B) and D3DRS_ALPHAREF := 0x40 (@0x00038FEE) and\n"
    "# nothing in either material apply changes them.\n"
    "# 'alpha_scalar' is material +0x20.  It is the ALPHA of\n"
    "# combiner factor C0, and every class puts it in the output:\n"
    "#   class 6 -> out.a = tex.a * alpha_scalar\n"
    "#   class 0/7 -> out.a = alpha_scalar\n"
    "#   class 1 -> out.a = tex.a * vertex.a  (C0.a unused)\n"
    "#   class 10 -> out.a = tex.a\n"
    "# The shadow and road-decal sheets are all class 6 and all\n"
    "# carry alpha_scalar 0.5..0.6, so retail draws them at half\n"
    "# strength; a renderer that blends them at full texture alpha\n"
    "# paints solid black tree shadows over the road.\n"
    "# 'vcolor 0' marks a material whose vertex declaration has no\n"
    "# D3DCOLOR register (classes 8 and 9 -- foliage, props,\n"
    "# cones): the game never reads the stored vertex colour for\n"
    "# them, so a renderer must draw them at full white.\n"
    "# 'shine <strength> <power> <gate>' is the class-1/7/10\n"
    "# additive term tex.a*(vertex.a if gate)*pow(R.V,power)*\n"
    "# light*strength; strength = material +0x04, power = +0x08,\n"
    "# gate = flag bit 0x40.\n"
    "# 'uv_scroll <rate> <step>' is the animated-material ticker\n"
    "# FUN_0019B1E0's scroll branch: phase(+0x1C) advances in\n"
    "# whole steps of +0x18 while phase+step <= rate(+0x14)*T, and\n"
    "# the material apply pushes (1-frac(phase), 0, 0) into vertex\n"
    "# shader constant 0x63, which the world vertex programs add\n"
    "# to v9.xy -- so U scrolls and V never moves.\n"
    "# 'anim_frames <n> <period> <hold0> <label0> <frame0>\n"
    "# <step> <pingpong>' plus one 'anim_frame <k> <label> <tex>'\n"
    "# per frame is the ticker's OTHER arm -- TEXTURE-FRAME\n"
    "# CYCLING, which needs no flag bit, only frame count\n"
    "# (+0x11) >= 2, and which swaps the bound texture instead of\n"
    "# offsetting the UVs. n = +0x11, period = +0x14, hold0 =\n"
    "# +0x18, label0 = +0x12, frame0 = +0x10, step = +0x13,\n"
    "# pingpong = flag bit 0x100 (unset on every shipped\n"
    "# material). <label> is the integer FUN_0019B1E0 atol()s out\n"
    "# of the frame's own texture NAME after skipping frame 0's\n"
    "# name length, and it is a KEYFRAME TIME in periods, not an\n"
    "# ordinal: frame k is held (label[k+1] - label[k]) * period\n"
    "# seconds, and the last frame one period. bk_warnsignb's\n"
    "# 1,4,5,8,9,12 is a board that reads for 3 s and blinks for\n"
    "# 1 s. Frame 0's label field is unused (the wrap arm forces\n"
    "# 1) and is written as 0.\n";

static const char MTL_HEADER_B[] =
    "#\n"
    "# Scene lighting and fog, from this track's enviro.dat --\n"
    "# see parse_enviro() for the byte-level citations. The\n"
    "# light direction is ALREADY NEGATED, i.e. it is exactly\n"
    "# vertex shader constant c[0x61], in the game's own space\n"
    "# (a reader that mirrors the world must mirror it too).\n"
    "# fog_rgb is the value the hardware lerps toward, i.e.\n"
    "# the authored colour halved (FOGCOLOR := f * 127.5).\n"
    "# The fog is linear over [fog_start, fog_end] on a fog\n"
    "# coordinate of min(clip z, fog_far) -- the MIN comes from\n"
    "# the world vertex programs' `min oFog, r12.z, c[120].z`\n"
    "# -- so the fog factor never falls below\n"
    "# (fog_end-fog_far)/(fog_end-fog_start).\n";

static const char OBJ_HEADER[] =
    "# Burnout 3 track geometry\n"
    "# format credit: Burnout Modding community (burnout.wiki)\n"
    "# Single-sided: the game culls backfaces (see the material\n"
    "# flags in tools/extract_track.py). Submeshes whose material\n"
    "# is D3DCULL_NONE carry a reverse-wound duplicate of every\n"
    "# triangle instead. src/burnout3_trackmesh.c enables culling.\n"
    "# Face order is the game's frame order (FUN_001AE340):\n"
    "#   streamed opaque, backdrop groups, water, streamed alpha,\n"
    "#   chevron group.\n"
    "# vt carries the game's own v with NO flip: v=0 is texel row\n"
    "# 0 of the D3D surface = the top row of the extracted PNG,\n"
    "# which is where GL's t=0 lands when the PNG is uploaded\n"
    "# row-0-first. Do not apply a 1-v anywhere downstream.\n"
    "# Extra per-vertex channels, all straight from the game's\n"
    "# 0x1C-byte world vertex (see the module docstring):\n"
    "#   v  x y z r g b   -- the D3DCOLOR diffuse at +0x10, scaled\n"
    "#                       0..1. It is HALF RANGE: 128/255 is\n"
    "#                       white, because the world pixel\n"
    "#                       shaders all carry SHIFTLEFT_1 on\n"
    "#                       stage 0 (rgb = 2*tex*colour).\n"
    "#   vt u v a         -- the vertex ALPHA, 0 or 1: the gate\n"
    "#                       for the class-1 specular term.\n"
    "#   vn nx ny nz      -- the NORMPACKED3 normal at +0x0C.\n";

/* ------------------------------------------------------------- structures */
typedef struct {
    float px, py, pz, u, v;
    int32_t nx, ny, nz;          /* raw NORMPACKED3 components, signed */
    uint8_t r, g, b, a;
} Vtx;

typedef struct {
    Vtx *v;
    long n;
    long emit_off;               /* 1-based OBJ base; 0 = not yet emitted */
} Model;

typedef struct {
    char name[64];
    long model;                  /* index into Ctx.models */
    int32_t *tri;                /* 3 indices per triangle */
    long ntri;
    int mat;                     /* material index, or -1 */
    int two_sided, decal, is_alpha;
} Mesh;

typedef struct {
    Mesh *m;
    long n, cap;
} MeshVec;

typedef struct {
    int present;
    char texture[80];            /* basename of frame 0's stored name */
    uint32_t flags, cls;
    float spec_strength, spec_power, alpha_scalar, anim_rate, anim_limit;
    int anim_frames;             /* +0x11 */
    int animated;                /* listed in the header+0x10 index table */
    int anim_index;              /* +0x10 */
    int anim_label;              /* +0x12 */
    int anim_step;               /* +0x13, signed */
    int nframe;                  /* max(1, anim_frames) */
    char (*frame_tex)[80];
    long *frame_label;
} Mat;

typedef struct {
    double xmin, xmax, zmin, zmax;
    int backdrop[8], nbackdrop;
} Cell;

typedef struct {
    int present;
    double light_dir[3], light_rgb[3], fog_rgb[3];
    double fog_start, fog_end, fog_far, fog_div;
    int fog_enabled;
} Scene;

typedef struct {
    cxb_blob sd, st;
    Mat *mats;
    int nmats;
    Model *models;
    long nmodels, cap_models;
    Cell *cells;
    long ncells, cap_cells;
    MeshVec stream_opaque, stream_alpha, backdrop, water, chevron;
    Scene scene;
    /* stats, printed only */
    long n_groups, n_models, n_submeshes, n_skipped, n_textured;
    long n_two_sided, n_dup_tris, n_mirror_skipped, n_decal, n_decal_tris;
    long n_alpha_sub, n_chained, n_unmapped, n_units, n_backdrop_dropped;
} Ctx;

/* ------------------------------------------------------------- containers */
static int mv_push(MeshVec *v, const Mesh *m)
{
    if (v->n == v->cap) {
        long nc = v->cap ? v->cap * 2 : 256;
        Mesh *p = (Mesh *)realloc(v->m, (size_t)nc * sizeof *p);
        if (!p)
            return -1;
        v->m = p;
        v->cap = nc;
    }
    v->m[v->n++] = *m;
    return 0;
}

static void mv_free(MeshVec *v)
{
    long i;
    for (i = 0; i < v->n; i++)
        free(v->m[i].tri);
    free(v->m);
    v->m = NULL;
    v->n = v->cap = 0;
}

static long ctx_add_model(Ctx *c, Vtx *v, long n)
{
    if (c->nmodels == c->cap_models) {
        long nc = c->cap_models ? c->cap_models * 2 : 256;
        Model *p = (Model *)realloc(c->models, (size_t)nc * sizeof *p);
        if (!p)
            return -1;
        c->models = p;
        c->cap_models = nc;
    }
    c->models[c->nmodels].v = v;
    c->models[c->nmodels].n = n;
    c->models[c->nmodels].emit_off = 0;
    return c->nmodels++;
}

/* ------------------------------------------------------------- primitives */
/* D3DVSDT_NORMPACKED3 -> the three signed integer components.  The division
 * (1023/1023/511) is applied only where the value is printed, so the arithmetic
 * matches the python exactly. */
static void unpack_normal(uint32_t w, int32_t *x, int32_t *y, int32_t *z)
{
    int32_t a = (int32_t)(w & 0x7FFu);
    int32_t b = (int32_t)((w >> 11) & 0x7FFu);
    int32_t c = (int32_t)((w >> 22) & 0x3FFu);
    if (a & 0x400) a -= 0x800;
    if (b & 0x400) b -= 0x800;
    if (c & 0x200) c -= 0x400;
    *x = a; *y = b; *z = c;
}

static Vtx *read_vertices(const cxb_blob *b, int64_t off, long count)
{
    Vtx *out;
    long i;

    if (off < 0 || count <= 0)
        return NULL;
    if ((uint64_t)off + (uint64_t)count * VTX_STRIDE > (uint64_t)b->n)
        return NULL;
    out = (Vtx *)malloc((size_t)count * sizeof *out);
    if (!out)
        return NULL;
    for (i = 0; i < count; i++) {
        size_t o = (size_t)off + (size_t)i * VTX_STRIDE;
        out[i].px = cxb_f32(b, o);
        out[i].py = cxb_f32(b, o + 4);
        out[i].pz = cxb_f32(b, o + 8);
        unpack_normal(cxb_u32(b, o + 0x0C),
                      &out[i].nx, &out[i].ny, &out[i].nz);
        out[i].b = cxb_u8(b, o + 0x10);          /* D3DCOLOR is BGRA */
        out[i].g = cxb_u8(b, o + 0x11);
        out[i].r = cxb_u8(b, o + 0x12);
        out[i].a = cxb_u8(b, o + 0x13);
        out[i].u = cxb_f32(b, o + 0x14);
        out[i].v = cxb_f32(b, o + 0x18);
    }
    return out;
}

/* Triangle strip -> triangle list, dropping degenerates; then bake
 * D3DCULL_NONE by appending a reverse-wound duplicate of every triangle.
 * Returns the triangle count, or -1. */
static long strip_emit(const cxb_blob *b, int64_t idx_off, long count,
                       int two_sided, int32_t **out)
{
    long i, n = 0;
    int32_t *t;
    long cap = count > 2 ? (count - 2) : 0;

    if (cap <= 0) {
        *out = NULL;
        return 0;
    }
    t = (int32_t *)malloc((size_t)cap * 2u * 3u * sizeof *t);
    if (!t)
        return -1;
    for (i = 0; i + 2 < count; i++) {
        int32_t a = (int32_t)cxb_u16(b, (size_t)idx_off + (size_t)i * 2u);
        int32_t bb = (int32_t)cxb_u16(b, (size_t)idx_off + (size_t)(i + 1) * 2u);
        int32_t cc = (int32_t)cxb_u16(b, (size_t)idx_off + (size_t)(i + 2) * 2u);
        if (a == bb || bb == cc || a == cc)
            continue;
        if ((i & 1) == 0) {
            t[n * 3] = a; t[n * 3 + 1] = bb; t[n * 3 + 2] = cc;
        } else {
            t[n * 3] = a; t[n * 3 + 1] = cc; t[n * 3 + 2] = bb;
        }
        n++;
    }
    if (n == 0) {
        free(t);
        *out = NULL;
        return 0;
    }
    if (two_sided) {
        for (i = 0; i < n; i++) {
            t[(n + i) * 3]     = t[i * 3];
            t[(n + i) * 3 + 1] = t[i * 3 + 2];
            t[(n + i) * 3 + 2] = t[i * 3 + 1];
        }
    }
    *out = t;
    return n;                       /* base triangle count, before doubling */
}

/* ------------------------------------------------------------- materials */
/* The C library's atol, which is what FUN_0019B1E0 calls on a texture name's
 * tail: skip leading blanks, take an optional sign and then digits, stop at
 * the first character that is not one.  Anything else is 0. */
static long c_atol_py(const char *s)
{
    size_t i = 0, j, k;
    long sign = 1, val = 0;

    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'
           || s[i] == '\f' || s[i] == '\v')
        i++;
    j = i;
    if (s[j] == '+' || s[j] == '-') {
        if (s[j] == '-')
            sign = -1;
        j++;
    }
    k = j;
    while (isdigit((unsigned char)s[k]))
        k++;
    if (k == j)
        return 0;
    for (; j < k; j++)
        val = val * 10 + (s[j] - '0');
    return sign * val;
}

static int name_at_record(const cxb_blob *b, int64_t rec, char *out,
                          size_t outsz)
{
    uint32_t bd = cxb_u32(b, (size_t)rec + 0x40);
    int64_t no = rec + ((bd == 4u || bd == 8u || bd == 32u) ? 0x48 : 0x44);
    return cxb_read_cstr(b, no, 64, out, outsz);
}

/* +0x0C points at an ARRAY of texture record pointers, one per animation
 * frame; the ticker indexes it as `(*(int**)(mat+0xC))[frame]`.  The label is
 * what its atol yields on the NEXT frame's name with as many characters
 * skipped as frame 0's whole (STORED) name is long.                       [C] */
static int read_anim_frames(const cxb_blob *b, int64_t m, int64_t tex_ptr,
                            const char *name0, int nframes, Mat *mat)
{
    int nframe = nframes > 1 ? nframes : 1;
    size_t skip = strlen(name0);
    int fi;

    mat->nframe = nframe;
    mat->frame_tex = (char (*)[80])calloc((size_t)nframe, 80);
    mat->frame_label = (long *)calloc((size_t)nframe, sizeof(long));
    if (!mat->frame_tex || !mat->frame_label)
        return -1;
    for (fi = 0; fi < nframe; fi++) {
        char raw[80];
        int have = 0;
        int64_t po = tex_ptr + (int64_t)fi * 4;

        if (po > 0 && (uint64_t)po < (uint64_t)b->n - 4u) {
            int64_t rec = cxb_ptr(b, (size_t)po, m);
            if (rec > 0 && (uint64_t)rec < (uint64_t)b->n - 0x70u) {
                if (name_at_record(b, rec, raw, sizeof raw) == 0)
                    have = 1;
            }
        }
        if (!have)
            snprintf(raw, sizeof raw, "%s", name0);
        cxb_basename(raw, mat->frame_tex[fi], 80);
        mat->frame_label[fi] =
            (fi == 0) ? 0
                      : c_atol_py(skip < strlen(raw) ? raw + skip : "");
    }
    return 0;
}

static int parse_materials(Ctx *c)
{
    const cxb_blob *b = &c->sd;
    int64_t mat_off = cxb_i32(b, 0x08);
    int mat_count = (int)cxb_u16(b, 0x0C);
    unsigned char *animated = NULL;
    int anim_count, i;
    int64_t anim_tbl;

    c->mats = NULL;
    c->nmats = 0;
    if (!(mat_off > 0 && (uint64_t)mat_off < (uint64_t)b->n))
        return 0;
    if (!(mat_count > 0 && mat_count < 4096))
        return 0;

    c->mats = (Mat *)calloc((size_t)mat_count, sizeof(Mat));
    if (!c->mats)
        return -1;
    c->nmats = mat_count;
    animated = (unsigned char *)calloc((size_t)mat_count, 1);
    if (!animated)
        return -1;

    /* The animated-material index table at header +0x10, count at +0x0E:
     * exactly the materials FUN_0019B1E0 ticks each frame.                [C] */
    anim_count = (int)cxb_u16(b, 0x0E);
    anim_tbl = cxb_i32(b, 0x10);
    if (anim_tbl > 0
        && (uint64_t)anim_tbl < (uint64_t)b->n - (uint64_t)anim_count * 2u) {
        int k;
        for (k = 0; k < anim_count; k++) {
            unsigned id = cxb_u16(b, (size_t)anim_tbl + (size_t)k * 2u);
            if (id < (unsigned)mat_count)
                animated[id] = 1;
        }
    }

    for (i = 0; i < mat_count; i++) {
        int64_t m = mat_off + (int64_t)i * MAT_STRIDE;
        int64_t tex_ptr, tex_rec;
        char name[80];
        Mat *mt;

        if ((uint64_t)m + MAT_STRIDE > (uint64_t)b->n)
            break;
        tex_ptr = cxb_ptr(b, (size_t)m + 0x0C, m);
        if (!(tex_ptr > 0 && (uint64_t)tex_ptr < (uint64_t)b->n - 4u))
            continue;
        tex_rec = cxb_ptr(b, (size_t)tex_ptr, m);
        if (!(tex_rec > 0 && (uint64_t)tex_rec < (uint64_t)b->n - 0x70u))
            continue;
        if (name_at_record(b, tex_rec, name, sizeof name) != 0 || !name[0])
            continue;

        mt = &c->mats[i];
        mt->present = 1;
        cxb_basename(name, mt->texture, sizeof mt->texture);
        mt->cls = cxb_u32(b, (size_t)m);
        mt->flags = cxb_u32(b, (size_t)m + 0x24);
        mt->spec_strength = cxb_f32(b, (size_t)m + 0x04);
        mt->spec_power = cxb_f32(b, (size_t)m + 0x08);
        mt->anim_rate = cxb_f32(b, (size_t)m + 0x14);
        mt->anim_limit = cxb_f32(b, (size_t)m + 0x18);
        mt->alpha_scalar = cxb_f32(b, (size_t)m + 0x20);
        mt->anim_frames = (int)cxb_u8(b, (size_t)m + 0x11);
        mt->animated = animated[i];
        mt->anim_index = (int)cxb_u8(b, (size_t)m + 0x10);
        mt->anim_label = (int)cxb_u8(b, (size_t)m + 0x12);
        mt->anim_step = (int)cxb_i8(b, (size_t)m + 0x13);
        if (read_anim_frames(b, m, tex_ptr, name, mt->anim_frames, mt) != 0) {
            free(animated);
            return -1;
        }
    }
    free(animated);
    return 0;
}

static const Mat *mat_get(const Ctx *c, int idx)
{
    if (idx < 0 || idx >= c->nmats || !c->mats[idx].present)
        return NULL;
    return &c->mats[idx];
}

static int mat_two_sided(const Mat *m) { return (m->flags & MAT_TWO_SIDED) != 0; }
static int mat_decal(const Mat *m) { return (m->flags & MAT_DECAL) != 0; }
static int mat_alpha_test(const Mat *m) { return (m->flags & MAT_ALPHA_TEST) != 0; }
static int mat_alpha_blend(const Mat *m) { return (m->flags & MAT_ALPHA_BLEND) != 0; }
static int mat_uses_vcolor(const Mat *m) { return m->cls != 8u && m->cls != 9u; }
static int mat_uv_scroll(const Mat *m)
{
    return m->animated && (m->flags & MAT_UV_SCROLL) && m->anim_frames < 2;
}
static int mat_frame_cycle(const Mat *m)
{
    return m->animated && m->anim_frames >= 2;
}
/* (strength, power, gated_by_vertex_alpha) for the classes whose pixel shader
 * adds `tex.a * C0.rgb`.  Class 2 has ONE vertex program (0x003E8B10), so the
 * flag-0x40 program swap that gates classes 1/7 does not apply to it; its
 * stage-0 alpha word 0xD4D81010 multiplies V0.a in unconditionally.       [C]
 * Class 10 adds tex.a * C0.rgb with C0.rgb the scene light UNSCALED and no
 * vertex-alpha gate and no specular factor.                               [C] */
static int mat_specular(const Mat *m, double *strength, double *power, int *gate)
{
    if (m->cls == 1u || m->cls == MAT_CLASS_MIRROR || m->cls == 7u) {
        *strength = (double)m->spec_strength;
        *power = (double)m->spec_power;
        *gate = (m->cls == MAT_CLASS_MIRROR) ? 1
                                             : ((m->flags & MAT_SPEC_GATE) != 0);
        return 1;
    }
    if (m->cls == MAT_CLASS_EMISSIVE) {
        *strength = 1.0;
        *power = 0.0;
        *gate = 0;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- streamed */
static int parse_streamed(Ctx *c)
{
    const cxb_blob *b = &c->sd;
    const cxb_blob *s = &c->st;
    unsigned unit_count = cxb_u16(b, 0x54);
    int64_t unit_table = cxb_i32(b, 0x58);
    unsigned u;

    for (u = 0; u < unit_count; u++) {
        int64_t e = unit_table + (int64_t)u * 0x10;
        int32_t sub_off;
        uint32_t sub_size, nsub;
        int64_t base, vtx_off, sub2, X;
        int n_slots, n_opaque;
        uint16_t *mat_tbl = NULL;
        int8_t *heads = NULL;
        int have_heads = 0;
        /* subs[k] = {fmt, count, index offset, next-in-chain} */
        struct { uint32_t fmt, count; int64_t off; int next; } *subs = NULL;
        long nsubs = 0;
        long vtx_bytes, nverts, model;
        Vtx *verts = NULL;
        unsigned char *seen = NULL;
        long *order_k = NULL;
        int *order_mat = NULL;
        unsigned char *order_alpha = NULL;
        long norder = 0;
        long k;
        int slot;
        int rc = -1;

        if ((uint64_t)e + 0x10u > (uint64_t)b->n)
            break;
        sub_off = cxb_i32(b, (size_t)e);
        sub_size = cxb_u32(b, (size_t)e + 8);
        /* offset 0 is a legitimate block position (unit 0 lives there) */
        if (sub_off < 0 || !sub_size)
            continue;
        base = (int64_t)sub_off + 0x10 + 0x40;
        if ((uint64_t)base + 0x20u > (uint64_t)s->n
            || cxb_u32(s, (size_t)base) != 1u) {
            c->n_skipped++;
            continue;
        }
        vtx_off = cxb_ptr(s, (size_t)base + 0x04, base);
        sub2 = cxb_ptr(s, (size_t)base + 0x0C, base);
        nsub = cxb_u32(s, (size_t)base + 0x10);
        if (!(vtx_off > 0 && (uint64_t)vtx_off < (uint64_t)s->n)
            || !(sub2 > 0 && (uint64_t)sub2 < (uint64_t)s->n)
            || !(nsub > 0 && nsub < 4096u)) {
            c->n_skipped++;
            continue;
        }

        /* --- PVS block: material table, slot split, per-slot chain heads */
        X = base + 0x20;
        n_slots = (int)cxb_u8(s, (size_t)X + PVS_NSLOT_B);
        n_opaque = (int)cxb_u8(s, (size_t)X + PVS_NSLOT_OPAQUE);
        if (n_opaque > n_slots)
            n_opaque = n_slots;
        if (n_slots > 0) {
            mat_tbl = (uint16_t *)malloc((size_t)n_slots * sizeof *mat_tbl);
            heads = (int8_t *)malloc((size_t)n_slots);
            if (!mat_tbl || !heads)
                goto unit_done;
            for (k = 0; k < n_slots; k++)
                mat_tbl[k] = cxb_u16(s, (size_t)X + PVS_MATTAB + (size_t)k * 2u);
        }
        /* the chunk describing THIS unit is the one whose relative unit index
         * is 0 (FUN_0019D100 @0x0019D1F7 reads that byte); chunk 0 in every
         * shipped unit, but search for it rather than assume.              [C] */
        {
            int chnk;
            for (chnk = 0; chnk < PVS_CHUNKS; chnk++) {
                int64_t o = X + PVS_CHUNK0 + (int64_t)PVS_CHUNK_STRIDE * chnk;
                if ((uint64_t)o + PVS_CHUNK_STRIDE > (uint64_t)s->n)
                    break;
                if (cxb_i8(s, (size_t)o) == 0) {
                    int64_t h = o + PVS_CHUNK_HEADS_A;
                    for (k = 0; k < n_slots; k++)
                        heads[k] = cxb_i8(s, (size_t)(h + k));
                    have_heads = 1;
                    break;
                }
            }
        }

        subs = malloc((size_t)nsub * sizeof *subs);
        if (!subs)
            goto unit_done;
        for (k = 0; k < (long)nsub; k++) {
            int64_t se = sub2 + (int64_t)k * SUB_STRIDE;
            if ((uint64_t)se + SUB_STRIDE > (uint64_t)s->n)
                break;
            subs[k].fmt = cxb_u32(s, (size_t)se + 0x80);
            subs[k].count = cxb_u32(s, (size_t)se + 0x84);
            subs[k].off = cxb_ptr(s, (size_t)se + 0x88, se);
            subs[k].next = cxb_i8(s, (size_t)se + SUB_NEXT_OFF);
            nsubs++;
        }
        if (nsubs == 0 || !subs[0].off) {
            c->n_skipped++;
            rc = 0;
            goto unit_done;
        }
        vtx_bytes = (long)(subs[0].off - vtx_off);
        if (vtx_bytes <= 0 || (vtx_bytes % VTX_STRIDE)) {
            c->n_skipped++;
            rc = 0;
            goto unit_done;
        }
        nverts = vtx_bytes / VTX_STRIDE;
        verts = read_vertices(s, vtx_off, nverts);
        if (!verts)
            goto unit_done;
        model = ctx_add_model(c, verts, nverts);
        if (model < 0)
            goto unit_done;
        verts = NULL;              /* owned by the model table now */

        /* This cell's own footprint plus the static groups it names, for the
         * backdrop filter in parse_static_group(). */
        {
            const Vtx *vv = c->models[model].v;
            Cell cell;
            long ii;
            cell.xmin = cell.xmax = (double)vv[0].px;
            cell.zmin = cell.zmax = (double)vv[0].pz;
            for (ii = 1; ii < nverts; ii++) {
                double x = (double)vv[ii].px, z = (double)vv[ii].pz;
                if (x < cell.xmin) cell.xmin = x;
                if (x > cell.xmax) cell.xmax = x;
                if (z < cell.zmin) cell.zmin = z;
                if (z > cell.zmax) cell.zmax = z;
            }
            cell.nbackdrop = 0;
            for (ii = 0; ii < 8; ii++) {
                int bg = cxb_i8(s, (size_t)(X + 0x04 + ii));
                if (bg >= 0) {
                    int dup = 0, q;
                    for (q = 0; q < cell.nbackdrop; q++)
                        if (cell.backdrop[q] == bg)
                            dup = 1;
                    if (!dup)
                        cell.backdrop[cell.nbackdrop++] = bg;
                }
            }
            if (c->ncells == c->cap_cells) {
                long nc = c->cap_cells ? c->cap_cells * 2 : 64;
                Cell *p = (Cell *)realloc(c->cells, (size_t)nc * sizeof *p);
                if (!p)
                    goto unit_done;
                c->cells = p;
                c->cap_cells = nc;
            }
            c->cells[c->ncells++] = cell;
        }

        c->n_units++;
        c->n_models++;

        /* --- walk the slots in the game's own order, splitting the passes */
        seen = (unsigned char *)calloc((size_t)nsubs, 1);
        order_k = (long *)malloc((size_t)nsubs * sizeof *order_k);
        order_mat = (int *)malloc((size_t)nsubs * sizeof *order_mat);
        order_alpha = (unsigned char *)malloc((size_t)nsubs);
        if (!seen || !order_k || !order_mat || !order_alpha)
            goto unit_done;
        if (have_heads) {
            for (slot = 0; slot < n_slots; slot++) {
                long g = heads[slot];
                long guard = 0;
                while (g >= 0 && g < nsubs && !seen[g] && guard <= nsubs) {
                    seen[g] = 1;
                    order_k[norder] = g;
                    order_mat[norder] = (int)mat_tbl[slot];
                    order_alpha[norder] = (unsigned char)(slot >= n_opaque);
                    norder++;
                    if (slot >= n_opaque)
                        c->n_alpha_sub++;
                    g = subs[g].next;
                    guard++;
                    if (guard > 1)
                        c->n_chained++;
                }
            }
        }
        /* anything the table does not reach keeps index order, ahead of the
         * mapped passes -- python does order.insert(0, ...) per k ascending,
         * which lands them REVERSED at the front. */
        for (k = 0; k < nsubs; k++) {
            if (!seen[k]) {
                memmove(order_k + 1, order_k, (size_t)norder * sizeof *order_k);
                memmove(order_mat + 1, order_mat,
                        (size_t)norder * sizeof *order_mat);
                memmove(order_alpha + 1, order_alpha, (size_t)norder);
                order_k[0] = k;
                order_mat[0] = -1;
                order_alpha[0] = 0;
                norder++;
                c->n_unmapped++;
            }
        }

        for (k = 0; k < norder; k++) {
            long si = order_k[k];
            int mat_id = order_mat[k];
            int is_alpha = order_alpha[k];
            const Mat *mat;
            int two_sided, decal;
            int32_t *tri = NULL;
            long ntri;
            Mesh mesh;
            long ii;
            int bad = 0;

            if (subs[si].fmt != TRI_STRIP || !subs[si].off || !subs[si].count)
                continue;
            if ((uint64_t)subs[si].off + (uint64_t)subs[si].count * 2u
                > (uint64_t)s->n)
                continue;
            for (ii = 0; ii < (long)subs[si].count; ii++) {
                if (cxb_u16(s, (size_t)subs[si].off + (size_t)ii * 2u)
                    >= (unsigned)nverts) {
                    bad = 1;
                    break;
                }
            }
            if (bad)
                continue;
            mat = (mat_id >= 0) ? mat_get(c, mat_id) : NULL;
            /* NOTE: the mirror-class (material class 2) skip is NOT applied
             * here.  It is confirmed only for the static-group draw
             * (FUN_001AD350 @0x001AD40D); the streamed draw path has no such
             * test.                                                       [C] */
            two_sided = mat && mat_two_sided(mat);
            decal = mat && mat_decal(mat);
            ntri = strip_emit(s, subs[si].off, (long)subs[si].count, two_sided,
                              &tri);
            if (ntri < 0)
                goto unit_done;
            if (ntri == 0)
                continue;
            if (mat)
                c->n_textured++;
            if (two_sided) {
                c->n_two_sided++;
                c->n_dup_tris += ntri;
            }
            if (decal) {
                c->n_decal++;
                c->n_decal_tris += ntri;
            }
            snprintf(mesh.name, sizeof mesh.name, "unit_%03u_s%03ld", u, si);
            mesh.model = model;
            mesh.tri = tri;
            mesh.ntri = two_sided ? ntri * 2 : ntri;
            mesh.mat = mat ? mat_id : -1;
            mesh.two_sided = two_sided;
            mesh.decal = decal;
            mesh.is_alpha = is_alpha;
            if (mv_push(is_alpha ? &c->stream_alpha : &c->stream_opaque,
                        &mesh) != 0)
                goto unit_done;
            c->n_submeshes++;
        }
        rc = 0;

unit_done:
        free(mat_tbl);
        free(heads);
        free(subs);
        free(verts);
        free(seen);
        free(order_k);
        free(order_mat);
        free(order_alpha);
        if (rc != 0)
            return -1;
    }
    return 0;
}

/* ---------------------------------------------------------- static groups */
/* The game does not draw all backdrop groups: FUN_001AD7A0 @0x001AD976 draws
 * only the (up to 8) named by the current cell's PVS bytes pvsRT+0x74..0x7B.
 * The harness bakes one display list with no per-cell visibility, so drawing
 * the union puts distant-only backdrop cards in front of the detailed streamed
 * geometry they stand in for.  STRICT filter: a backdrop submodel is dropped
 * as soon as ANY streamed unit it overlaps omits its group.  Submodels that
 * overlap no unit at all are always kept.                                 [C] */
static int parse_static_group(Ctx *c, int gi, const char *name, MeshVec *out,
                              int use_cells)
{
    const cxb_blob *b = &c->sd;
    unsigned n = cxb_u16(b, 0x1C + (size_t)gi * 2u);
    int64_t table = cxb_i32(b, 0x24 + (size_t)gi * 4u);
    unsigned g;

    if (!n || !(table > 0 && (uint64_t)table < (uint64_t)b->n))
        return 0;
    for (g = 0; g < n; g++) {
        int64_t entry = table + (int64_t)g * 8;
        unsigned sub_count;
        int64_t mdl;
        unsigned s;

        if ((uint64_t)entry + 8u > (uint64_t)b->n)
            break;
        sub_count = cxb_u16(b, (size_t)entry);
        mdl = cxb_ptr(b, (size_t)entry + 4, entry);
        if (!mdl || !(mdl > 0 && (uint64_t)mdl < (uint64_t)b->n) || !sub_count)
            continue;
        c->n_groups++;
        for (s = 0; s < sub_count; s++) {
            int64_t base = mdl + (int64_t)s * MDL_STRIDE + 0x40;
            int64_t vtx_off, sub_off, mi_ptr;
            uint32_t nsub;
            uint16_t *mat_idx = NULL;
            long n_mat_idx = 0;
            struct { uint32_t fmt, count; int64_t off; } *subs = NULL;
            long nsubs = 0, k, vtx_bytes, nverts, model;
            Vtx *verts = NULL;
            int rc = -1;

            if ((uint64_t)base + 0x20u > (uint64_t)b->n)
                break;
            if (cxb_u32(b, (size_t)base) != 1u) {
                c->n_skipped++;
                continue;
            }
            vtx_off = cxb_ptr(b, (size_t)base + 0x04, base);
            sub_off = cxb_ptr(b, (size_t)base + 0x0C, base);
            nsub = cxb_u32(b, (size_t)base + 0x10);
            if (!(vtx_off > 0 && (uint64_t)vtx_off < (uint64_t)b->n)
                || !(sub_off > 0 && (uint64_t)sub_off < (uint64_t)b->n)) {
                c->n_skipped++;
                continue;
            }
            if (!(nsub > 0 && nsub < 4096u)) {
                c->n_skipped++;
                continue;
            }

            /* Material indices: pointer at +0x14, relative to the GROUP entry
             * for version <= 0x30.  One u16 per submesh. */
            mi_ptr = cxb_ptr(b, (size_t)base + 0x14, entry);
            if (mi_ptr > 0
                && (uint64_t)mi_ptr < (uint64_t)b->n - (uint64_t)nsub * 2u) {
                mat_idx = (uint16_t *)malloc((size_t)nsub * sizeof *mat_idx);
                if (!mat_idx)
                    goto model_done;
                for (k = 0; k < (long)nsub; k++)
                    mat_idx[k] = cxb_u16(b, (size_t)mi_ptr + (size_t)k * 2u);
                n_mat_idx = (long)nsub;
            }

            subs = malloc((size_t)nsub * sizeof *subs);
            if (!subs)
                goto model_done;
            for (k = 0; k < (long)nsub; k++) {
                int64_t se = sub_off + (int64_t)k * SUB_STRIDE;
                if ((uint64_t)se + SUB_STRIDE > (uint64_t)b->n)
                    break;
                subs[k].fmt = cxb_u32(b, (size_t)se + 0x80);
                subs[k].count = cxb_u32(b, (size_t)se + 0x84);
                subs[k].off = cxb_ptr(b, (size_t)se + 0x88, se);
                nsubs++;
            }
            if (nsubs == 0 || !subs[0].off) {
                c->n_skipped++;
                rc = 0;
                goto model_done;
            }
            vtx_bytes = (long)(subs[0].off - vtx_off);
            if (vtx_bytes <= 0 || (vtx_bytes % VTX_STRIDE)) {
                c->n_skipped++;
                rc = 0;
                goto model_done;
            }
            nverts = vtx_bytes / VTX_STRIDE;
            verts = read_vertices(b, vtx_off, nverts);
            if (!verts)
                goto model_done;

            if (use_cells && c->ncells > 0) {
                double xmn = verts[0].px, xmx = verts[0].px;
                double zmn = verts[0].pz, zmx = verts[0].pz;
                long ii;
                int over = 0, all_named = 1;
                for (ii = 1; ii < nverts; ii++) {
                    double x = (double)verts[ii].px, z = (double)verts[ii].pz;
                    if (x < xmn) xmn = x;
                    if (x > xmx) xmx = x;
                    if (z < zmn) zmn = z;
                    if (z > zmx) zmx = z;
                }
                for (ii = 0; ii < c->ncells; ii++) {
                    const Cell *cl = &c->cells[ii];
                    int q, named = 0;
                    if (!(cl->xmin <= xmx && cl->xmax >= xmn
                          && cl->zmin <= zmx && cl->zmax >= zmn))
                        continue;
                    over = 1;
                    for (q = 0; q < cl->nbackdrop; q++)
                        if (cl->backdrop[q] == (int)g)
                            named = 1;
                    if (!named)
                        all_named = 0;
                }
                if (over && !all_named) {
                    c->n_backdrop_dropped++;
                    rc = 0;
                    goto model_done;
                }
            }

            model = ctx_add_model(c, verts, nverts);
            if (model < 0)
                goto model_done;
            verts = NULL;
            c->n_models++;

            for (k = 0; k < nsubs; k++) {
                const Mat *mat;
                int two_sided, decal;
                int32_t *tri = NULL;
                long ntri, ii;
                int bad = 0;
                Mesh mesh;

                if (subs[k].fmt != TRI_STRIP || !subs[k].off || !subs[k].count)
                    continue;
                if ((uint64_t)subs[k].off + (uint64_t)subs[k].count * 2u
                    > (uint64_t)b->n)
                    continue;
                for (ii = 0; ii < (long)subs[k].count; ii++) {
                    if (cxb_u16(b, (size_t)subs[k].off + (size_t)ii * 2u)
                        >= (unsigned)nverts) {
                        bad = 1;
                        break;
                    }
                }
                if (bad)
                    continue;
                mat = (k < n_mat_idx) ? mat_get(c, (int)mat_idx[k]) : NULL;
                if (mat && mat->cls == MAT_CLASS_MIRROR) {
                    c->n_mirror_skipped++;
                    continue;
                }
                two_sided = mat && mat_two_sided(mat);
                decal = mat && mat_decal(mat);
                ntri = strip_emit(b, subs[k].off, (long)subs[k].count,
                                  two_sided, &tri);
                if (ntri < 0)
                    goto model_done;
                if (ntri == 0)
                    continue;
                if (mat)
                    c->n_textured++;
                if (two_sided) {
                    c->n_two_sided++;
                    c->n_dup_tris += ntri;
                }
                if (decal) {
                    c->n_decal++;
                    c->n_decal_tris += ntri;
                }
                snprintf(mesh.name, sizeof mesh.name, "%s_g%03u_m%03u_s%03ld",
                         name, g, s, k);
                mesh.model = model;
                mesh.tri = tri;
                mesh.ntri = two_sided ? ntri * 2 : ntri;
                mesh.mat = mat ? (int)mat_idx[k] : -1;
                mesh.two_sided = two_sided;
                mesh.decal = decal;
                mesh.is_alpha = 0;
                if (mv_push(out, &mesh) != 0)
                    goto model_done;
                c->n_submeshes++;
            }
            rc = 0;

model_done:
            free(mat_idx);
            free(subs);
            free(verts);
            if (rc != 0)
                return -1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- enviro */
/* FUN_001888F0 loads "<track dir>/enviro.dat" and FUN_00188C00 copies its
 * FIRST 0xB0 BYTES verbatim over the environment object at 0x0060E040.  The
 * light direction at +0x80 is normalised in place (FUN_00011570 @0x00188A47)
 * and vertex-shader constant c[0x61] is its NEGATION (the XORPS at
 * 0x00038D62).  FOGCOLOR := (env +0x00) * 127.5 -- NOT 255 (the constant at
 * 0x003B1E68).  FOGSTART := (rec+0x20) * 0.05, FOGEND := (far - start)/div +
 * start, FOGTABLEMODE := 3 (D3DFOG_LINEAR).                               [C] */
static void parse_enviro(Ctx *c, const char *track_dir)
{
    char path[4096];
    cxb_blob d = { NULL, 0 };
    double fog[3], ldir[3], n;
    double fog_far, fog_div;
    int i;

    c->scene.present = 0;
    snprintf(path, sizeof path, "%s/enviro.dat", track_dir);
    if (!cxb_file_exists(path))
        return;
    if (cxb_read_file(path, &d) != 0)
        return;
    if (d.n < 0xB0) {
        cxb_blob_free(&d);
        return;
    }
    for (i = 0; i < 3; i++) {
        fog[i] = (double)cxb_f32(&d, (size_t)(0x00 + i * 4));
        c->scene.light_rgb[i] = (double)cxb_f32(&d, (size_t)(0x60 + i * 4));
        ldir[i] = (double)cxb_f32(&d, (size_t)(0x80 + i * 4));
    }
    fog_far = (double)cxb_f32(&d, 0x10);
    fog_div = (double)cxb_f32(&d, 0x14);
    n = sqrt(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
    if (n > 1e-6) {
        for (i = 0; i < 3; i++)
            ldir[i] = ldir[i] / n;
    }
    for (i = 0; i < 3; i++) {
        c->scene.light_dir[i] = -ldir[i];
        c->scene.fog_rgb[i] = fog[i] * 0.5;
    }
    c->scene.fog_far = fog_far;
    c->scene.fog_div = fog_div;
    c->scene.fog_start = fog_far * 0.05;
    c->scene.fog_end = (fog_div > 0.0)
                     ? ((fog_far - c->scene.fog_start) / fog_div
                        + c->scene.fog_start)
                     : 0.0;
    c->scene.fog_enabled = fog_div > 0.0;
    c->scene.present = 1;
    cxb_blob_free(&d);
}

/* ------------------------------------------------------------ MTL identity */
/* One `newmtl` per distinct (texture, class, flags).  A texture is NOT a
 * material: US_C3_V1 ships GL_road5 / GL_road7 / GL_road7grass twice, once
 * unshaded (class 0) and once specular (class 1).  Distinct records get
 * `<texture>.mNN` suffixes; the first keeps the bare name. */
typedef struct {
    char texture[80];
    uint32_t cls, flags;
    int mat;                    /* first material index carrying this key */
    char name[96];
    int ts, dc, al;
} MatUse;

static int matuse_find(const MatUse *u, int n, const Mat *m)
{
    int i;
    for (i = 0; i < n; i++) {
        if (u[i].cls == m->cls && u[i].flags == m->flags
            && strcmp(u[i].texture, m->texture) == 0)
            return i;
    }
    return -1;
}

/* --------------------------------------------------------------- emission */
static int write_outputs(Ctx *c, const Mesh **meshes, long nmesh,
                         const char *out_dir)
{
    char objpath[4096], mtlpath[4096];
    FILE *f;
    MatUse *used;
    char *iobuf = NULL;
    int nused = 0;
    long i;
    int k;
    static const char *texdir = "textures";

    used = (MatUse *)calloc((size_t)(c->nmats + 1), sizeof *used);
    if (!used)
        return -1;

    /* material_names(): first-seen key order, `.mNN` per repeated texture */
    for (i = 0; i < nmesh; i++) {
        const Mat *m = (meshes[i]->mat >= 0) ? mat_get(c, meshes[i]->mat) : NULL;
        int idx;
        if (!m)
            continue;
        idx = matuse_find(used, nused, m);
        if (idx >= 0)
            continue;
        {
            int per_tex = 0;
            for (k = 0; k < nused; k++)
                if (strcmp(used[k].texture, m->texture) == 0)
                    per_tex++;
            snprintf(used[nused].texture, sizeof used[nused].texture, "%s",
                     m->texture);
            used[nused].cls = m->cls;
            used[nused].flags = m->flags;
            used[nused].mat = meshes[i]->mat;
            if (per_tex == 0)
                snprintf(used[nused].name, sizeof used[nused].name, "%s",
                         m->texture);
            else
                snprintf(used[nused].name, sizeof used[nused].name, "%s.m%d",
                         m->texture, per_tex);
            nused++;
        }
    }
    /* the two_sided / decal / alpha annotation sets */
    for (i = 0; i < nmesh; i++) {
        const Mat *m = (meshes[i]->mat >= 0) ? mat_get(c, meshes[i]->mat) : NULL;
        int idx;
        if (!m)
            continue;
        idx = matuse_find(used, nused, m);
        if (idx < 0)
            continue;
        if (meshes[i]->two_sided) used[idx].ts = 1;
        if (meshes[i]->decal)     used[idx].dc = 1;
        if (meshes[i]->is_alpha)  used[idx].al = 1;
    }

    /* ---------------------------------------------------------- track.mtl */
    snprintf(mtlpath, sizeof mtlpath, "%s/track.mtl", out_dir);
    f = fopen(mtlpath, "w");
    if (!f) {
        free(used);
        return -1;
    }
    fputs(MTL_HEADER_A, f);
    fputs(MTL_HEADER_A2, f);
    if (c->scene.present) {
        fputs(MTL_HEADER_B, f);
        fprintf(f, "# scene_light_dir %.6f %.6f %.6f\n",
                c->scene.light_dir[0], c->scene.light_dir[1],
                c->scene.light_dir[2]);
        fprintf(f, "# scene_light_rgb %.6f %.6f %.6f\n",
                c->scene.light_rgb[0], c->scene.light_rgb[1],
                c->scene.light_rgb[2]);
        fprintf(f, "# scene_fog_rgb %.6f %.6f %.6f\n",
                c->scene.fog_rgb[0], c->scene.fog_rgb[1], c->scene.fog_rgb[2]);
        fprintf(f, "# scene_fog_range %.4f %.4f %.4f\n",
                c->scene.fog_start, c->scene.fog_end, c->scene.fog_far);
    }
    fputs("\n", f);
    for (k = 0; k < nused; k++) {
        const Mat *m = mat_get(c, used[k].mat);
        double sstr, spow;
        int gate, cyc;

        fprintf(f, "newmtl %s\nKd 1 1 1\nmap_Kd %s/%s.png\n",
                used[k].name, texdir, m->texture);
        fprintf(f, "# class %u\n# flags 0x%04X\n", m->cls, m->flags);
        if (mat_alpha_test(m))
            fputs("# alpha_test 1\n", f);
        if (mat_alpha_blend(m))
            fputs("# alpha_blend 1\n", f);
        fprintf(f, "# alpha_scalar %.4f\n", (double)m->alpha_scalar);
        if (mat_uv_scroll(m))
            fprintf(f, "# uv_scroll %.6f %.6f\n",
                    (double)m->anim_rate, (double)m->anim_limit);
        cyc = mat_frame_cycle(m);
        if (cyc) {
            int fi;
            fprintf(f, "# anim_frames %d %.6f %.6f %d %d %d %d\n",
                    m->anim_frames, (double)m->anim_rate,
                    (double)m->anim_limit, m->anim_label, m->anim_index,
                    m->anim_step,
                    (m->flags & MAT_FRAME_PINGPONG) ? 1 : 0);
            for (fi = 0; fi < m->anim_frames; fi++)
                fprintf(f, "# anim_frame %d %ld %s/%s.png\n", fi,
                        fi < m->nframe ? m->frame_label[fi] : 0L, texdir,
                        fi < m->nframe ? m->frame_tex[fi] : m->texture);
        }
        if (!mat_uses_vcolor(m))
            fputs("# vcolor 0\n", f);
        if (mat_specular(m, &sstr, &spow, &gate) && sstr > 0.0)
            fprintf(f, "# shine %.4f %.4f %d\n", sstr, spow, gate ? 1 : 0);
        /* NOTE: python's write_obj() rebinds `n` -- the newmtl NAME -- when it
         * unpacks the frame_cycle tuple, so for a frame-cycling material the
         * following three membership tests compare an INT against sets of
         * strings and are always False.  The two_sided/decal/alpha lines are
         * therefore suppressed on exactly those materials, and this port
         * reproduces that verbatim (byte identity is the gate). */
        if (!cyc) {
            if (used[k].ts) fputs("# two_sided 1\n", f);
            if (used[k].dc) fputs("# decal 1\n", f);
            if (used[k].al) fputs("# alpha 1\n", f);
        }
        fputs("\n", f);
    }
    if (fclose(f) != 0) {
        free(used);
        return -1;
    }

    /* ---------------------------------------------------------- track.obj */
    snprintf(objpath, sizeof objpath, "%s/track.obj", out_dir);
    f = fopen(objpath, "w");
    if (!f) {
        free(used);
        return -1;
    }
    iobuf = (char *)malloc(1u << 22);
    if (iobuf)
        setvbuf(f, iobuf, _IOFBF, 1u << 22);
    fputs(OBJ_HEADER, f);
    fputs("mtllib track.mtl\n", f);
    {
        long base = 1;
        for (i = 0; i < c->nmodels; i++)
            c->models[i].emit_off = 0;
        for (i = 0; i < nmesh; i++) {
            const Mesh *ms = meshes[i];
            Model *md = &c->models[ms->model];
            long off, t;

            fprintf(f, "o %s\n", ms->name);
            if (md->emit_off == 0) {
                long q;
                md->emit_off = base;
                for (q = 0; q < md->n; q++) {
                    const Vtx *v = &md->v[q];
                    fprintf(f, "v %.6f %.6f %.6f %.5f %.5f %.5f\n",
                            (double)v->px, (double)v->py, (double)v->pz,
                            (double)v->r / 255.0, (double)v->g / 255.0,
                            (double)v->b / 255.0);
                }
                for (q = 0; q < md->n; q++) {
                    const Vtx *v = &md->v[q];
                    /* v is written UNCHANGED -- the game's v and GL's t are
                     * the same number.  A 1-v here is what put every sign in
                     * the world upside down.                              [C] */
                    fprintf(f, "vt %.6f %.6f %.3f\n",
                            (double)v->u, (double)v->v, (double)v->a / 255.0);
                }
                for (q = 0; q < md->n; q++) {
                    const Vtx *v = &md->v[q];
                    fprintf(f, "vn %.4f %.4f %.4f\n",
                            (double)v->nx / 1023.0, (double)v->ny / 1023.0,
                            (double)v->nz / 511.0);
                }
                base += md->n;
            }
            off = md->emit_off;
            if (ms->mat >= 0) {
                const Mat *m = mat_get(c, ms->mat);
                int idx = m ? matuse_find(used, nused, m) : -1;
                if (idx >= 0)
                    fprintf(f, "usemtl %s\n", used[idx].name);
            }
            for (t = 0; t < ms->ntri; t++) {
                long a = off + ms->tri[t * 3];
                long bb = off + ms->tri[t * 3 + 1];
                long cc = off + ms->tri[t * 3 + 2];
                fprintf(f, "f %ld/%ld/%ld %ld/%ld/%ld %ld/%ld/%ld\n",
                        a, a, a, bb, bb, bb, cc, cc, cc);
            }
        }
    }
    free(used);
    {
        int ok = fclose(f) == 0;
        free(iobuf);                /* only valid once the stream is closed */
        return ok ? 0 : -1;
    }
}

/* ------------------------------------------------------------------ entry */
int cx_extract_track_mesh(const char *game_dir, const char *track_dir,
                          const char *track_id, const char *out_dir)
{
    char path[4096];
    Ctx c;
    const Mesh **order = NULL;
    long nmesh = 0, i;
    uint32_t ver, size;
    int rc = 1, gi;

    (void)game_dir;
    memset(&c, 0, sizeof c);

    snprintf(path, sizeof path, "%s/static.dat", track_dir);
    if (cxb_read_file(path, &c.sd) != 0) {
        fprintf(stderr, "[cx_track] cannot read %s\n", path);
        goto done;
    }
    ver = cxb_u32(&c.sd, 0);
    size = cxb_u32(&c.sd, 4);
    if (size != (uint32_t)c.sd.n)
        fprintf(stderr, "warning: header size 0x%X != file size 0x%X\n",
                size, (unsigned)c.sd.n);
    if (ver <= 0x25u) {
        fprintf(stderr, "version 0x%X uses the older layout; not implemented\n",
                ver);
        goto done;
    }

    if (parse_materials(&c) != 0)
        goto done;

    snprintf(path, sizeof path, "%s/streamed.dat", track_dir);
    if (cxb_file_exists(path)) {
        if (cxb_read_file(path, &c.st) != 0) {
            fprintf(stderr, "[cx_track] cannot read %s\n", path);
            goto done;
        }
        if (parse_streamed(&c) != 0)
            goto done;
    }

    /* want_groups defaults to {backdrop, chevron, water}: the reflection group
     * duplicates nearby geometry for the game's mirror pass and z-fights the
     * real surfaces if drawn directly. */
    for (gi = 0; gi < 3; gi++) {
        MeshVec *dst = (gi == 0) ? &c.backdrop
                     : (gi == 1) ? &c.chevron : &c.water;
        if (parse_static_group(&c, gi, GROUP_NAMES[gi], dst, gi == 0) != 0)
            goto done;
    }

    parse_enviro(&c, track_dir);

    /* FRAME ORDER, from FUN_001AE340: streamed opaque -> per-cell backdrop
     * groups -> water/reflection -> streamed alpha -> chevron group. */
    nmesh = c.stream_opaque.n + c.backdrop.n + c.water.n + c.stream_alpha.n
          + c.chevron.n;
    order = (const Mesh **)malloc((size_t)(nmesh ? nmesh : 1)
                                  * sizeof *order);
    if (!order)
        goto done;
    {
        long k = 0;
        MeshVec *vs[5] = { &c.stream_opaque, &c.backdrop, &c.water,
                           &c.stream_alpha, &c.chevron };
        int q;
        for (q = 0; q < 5; q++)
            for (i = 0; i < vs[q]->n; i++)
                order[k++] = &vs[q]->m[i];
    }
    if (nmesh == 0) {
        fprintf(stderr, "no geometry recovered\n");
        goto done;
    }

    if (cxb_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "[cx_track] cannot create %s\n", out_dir);
        goto done;
    }
    if (write_outputs(&c, order, nmesh, out_dir) != 0) {
        fprintf(stderr, "[cx_track] cannot write %s/track.obj\n", out_dir);
        goto done;
    }

    {
        long nt = 0;
        for (i = 0; i < nmesh; i++)
            nt += order[i]->ntri;
        printf("[cx_track] track %s (%s)\n", track_id, track_dir);
        printf("static.dat version 0x%X\n", ver);
        printf("parsed: %ld groups, %ld models, %ld submeshes (%ld skipped), "
               "%ld units\n", c.n_groups, c.n_models, c.n_submeshes,
               c.n_skipped, c.n_units);
        {
            int npresent = 0, q;
            for (q = 0; q < c.nmats; q++)
                if (c.mats[q].present)
                    npresent++;
            printf("materials: %d in table, %ld/%ld submeshes textured\n",
                   npresent, c.n_textured, c.n_submeshes);
        }
        printf("two-sided: %ld submeshes, %ld duplicate tris; %ld mirror-class "
               "submeshes skipped\n", c.n_two_sided, c.n_dup_tris,
               c.n_mirror_skipped);
        printf("decals   : %ld submeshes / %ld tris\n", c.n_decal,
               c.n_decal_tris);
        printf("passes   : %ld streamed submeshes in the alpha pass, %ld "
               "chained, %ld unmapped\n", c.n_alpha_sub, c.n_chained,
               c.n_unmapped);
        printf("backdrop : %ld submodels dropped by the local-cell filter\n",
               c.n_backdrop_dropped);
        printf("geometry: %ld triangles -> %s/track.obj\n", nt, out_dir);
    }
    rc = 0;

done:
    free(order);
    mv_free(&c.stream_opaque);
    mv_free(&c.stream_alpha);
    mv_free(&c.backdrop);
    mv_free(&c.water);
    mv_free(&c.chevron);
    for (i = 0; i < c.nmodels; i++)
        free(c.models[i].v);
    free(c.models);
    free(c.cells);
    for (i = 0; i < c.nmats; i++) {
        free(c.mats[i].frame_tex);
        free(c.mats[i].frame_label);
    }
    free(c.mats);
    cxb_blob_free(&c.sd);
    cxb_blob_free(&c.st);
    return rc;
}

int cx_extract_track(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir)
{
    return cx_extract_track_mesh(game_dir, track_dir, track_id, out_dir);
}
