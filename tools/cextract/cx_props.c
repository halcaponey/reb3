/* cx_props.c -- C11 port of tools/extract_props.py (THE SPEC).
 *
 * Recover a track's DESTRUCTIBLE PROPS (cones, barrier boards, signposts,
 * bins, benches) out of `static.dat` and emit <out_dir>/props.bin.  Nothing
 * here is a per-track constant: every address comes out of the file's own
 * header.  The condensed provenance below is the python module's; see it for
 * the full derivation.
 *
 * ============================================================ WHERE THEY ARE
 * static.dat's +0x34/+0x38 table is the BACKDROP-LOD table, not this.  The
 * destructible props are a SECOND table of the same 0x70-record shape with its
 * own placement machinery:
 *
 *     hdr +0x36  u16   MODEL count        (0x70-record relocation loop is
 *     hdr +0x3C  i32   MODEL table         FUN_0019B4E0 @0x0019B634..0x0019B6F7)
 *     hdr +0x40  u16   INSTANCE count
 *     hdr +0x44  i32   u8[model_count]    per-model PROP CLASS
 *     hdr +0x48  i32   instance TRANSFORMS -- 0x40 bytes each, a 4x4 matrix
 *     hdr +0x4C  i32   ptr[unit_count]    -> u8[model_count] per-unit counts
 *     hdr +0x50  i32   ptr[unit_count]    -> ptr[model_count] per-unit lists
 *     hdr +0x54  u16   streamed unit count
 *
 * The reader of all of it is the world-object registration loop
 * FUN_00110420 @0x001109CB..0x00110A46 [C]:
 *
 *     0x001109EB  MOV  AL,[EDI + EDX]      ; count[unit][model]
 *     0x00110A00  MOV  EAX,[EDX + model*4] ; the instance list
 *     0x00110A03  LEA  ECX,[EAX + j*4]     ; 4 BYTES PER LIST ENTRY
 *     0x00110A19  MOV  byte [slot],0x5     ; world-slot TYPE 5 = STATIC PROP
 *     0x00110A1C  MOVZX EDX,word [ECX]     ; entry+0x00 u16 = instance index
 *     0x00110A1F  SHL  EDX,0x6             ;   * 0x40
 *     0x00110A22  ADD  EDX,[0x00737678]    ;   + the +0x48 transform table
 *     0x00110A2B  MOVZX ECX,byte [ECX+0x2] ; entry+0x02 u8 = the class byte
 *
 * The list-entry byte at +0x02 equals hdr+0x44[model] on all 37 shipped files,
 * so the two are the same datum.  It is a CLASS (0..7; 1 == cone on every
 * track that has one), NOT a model index -- proof in the python docstring.
 * The model an instance draws is the LIST it lives in, and that is what this
 * emits.
 *
 * ============================================================ MODEL RECORD
 * 0x70 bytes, fixed up by FUN_0019B4E0:
 *     +0x00  f32[4]  bbox MAX (w unused)   [C] FUN_0011A020 @0x0011A0A5:
 *     +0x10  f32[4]  bbox MIN (w unused)       copied to body+0x1D0/+0x1E0
 *     +0x20  mesh block, LOD 0             always present
 *     +0x5C  u16     LOD0 material index   (into the header +0x08 table)
 *     +0x62  u16     LOD-present flags
 *     +0x64  f32     LOD/fade NEAR distance
 *     +0x68  f32     LOD/fade FAR distance
 *
 *   mesh block (0x14 bytes), relocation transcribed from FUN_0019B4E0:
 *     +0x00  u32  Xbox D3DResource Common; the relocated data pointer is
 *                 masked with 0x0FFFFFFF unless (Common & 0x70000) == 0x20000
 *     +0x04  i32  vertex data, RELATIVE to the block
 *     +0x0C  u32  INDEX count
 *     +0x10  i32  index data, RELATIVE to the block
 *
 *   Vertex STRIDE comes from the material's shader class -- the two prop/cone
 *   declarations extract_track.py pins (src/burnout3_trackmesh.h:207):
 *       class 8  0x003875E8: v0=FLOAT3          v9=FLOAT2 = 20 bytes
 *       class 9  0x003875D4: v0=FLOAT3 v2=NPK3  v9=FLOAT2 = 24 bytes
 *   Over all 436 prop models in all 37 tracks (index_ptr - vertex_ptr) is an
 *   exact multiple of the class-derived stride and the resulting vertex count
 *   exceeds the largest index -- which neither stride achieves alone.  Indices
 *   are u16 and form ONE triangle strip; this de-strips to a triangle list.
 *
 * ================================================================ PHYSICS
 * FUN_0011A020 is the prop rigid-body constructor.  It writes [C]:
 *     body+0x1CC  radius = |bbox_max|                     SQRTSS @0x0011A0DE
 *     body+0x1F0  MASS = max(100.0, (max.z-min.z)*(max.x-min.x)*200.0)
 *                 @0x0011A137..0x0011A191; the constants are the image floats
 *                 [0x003A2928] = 100.0 and [0x003A292C] = 200.0
 * so a prop's mass is its FOOTPRINT AREA x 200, floored at 100.
 *
 * ================================================================ props.bin
 * Little-endian.  Positions/normals/transforms are in RAW GAME SPACE -- the
 * Z-negation to harness/GL space is the loader's job (RE_NOTES 12).
 *   +0x00 'B3PP'  +0x04 u32 version=1  +0x08 model_count  +0x0C instance_count
 *   +0x10 vertex_count  +0x14 index_count  +0x18 off_models  +0x1C off_inst
 *   +0x20 off_vertices  +0x24 off_indices  +0x28 unit_count  +0x2C reserved
 *
 *   model record, 0x60 bytes:  bb_min[3] bb_max[3] first_vertex vertex_count
 *     first_index index_count prop_class mass radius lod_near lod_far
 *     material_flags char[32] texture-basename.  Index values are MODEL-LOCAL:
 *     the reader adds first_vertex.  (They were global once, and every model
 *     past the first drew another model's vertices -- a barrier board came out
 *     as a heap of shards.)
 *   instance record, 0x50 bytes: f32 m[16] (row0 right, row1 up, row2 at,
 *     row3 position), u32 model, u32 prop_class, u32 unit, u32 flags.
 *   vertex, 0x20 bytes: f32 pos[3], normal[3], uv[2].   index: u16.
 */
#include "cx_common_c.h"
#include "cx_extract.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CXP_MAGIC        "B3PP"
#define CXP_VERSION      1u
#define CXP_VSTRIDE_C9   0x18   /* shader class 9: pos + NORMPACKED3 + uv */
#define CXP_VSTRIDE_C8   0x14   /* shader class 8: pos + uv (no normal)   */
#define CXP_MODEL_REC    0x60
#define CXP_INST_REC     0x50
#define CXP_OFF_MODELS   0x30
#define CXP_CONE_CLASS   1      /* the recovered constant: class 1 is the cone */

#define CXP_MAT_STRIDE    0x28  /* extract_track.py MAT_STRIDE    */
#define CXP_MAT_FLAGS_OFF 0x24  /* extract_track.py MAT_FLAGS_OFF */

/* ------------------------------------------------------------------ notes
 * extract_props.py accumulates a `notes` list of structural surprises and
 * prints them under the report.  Kept: they are the tool's own self-check. */
typedef struct {
    char **v;
    size_t n, cap;
} cxp_notes;

static void note(cxp_notes *ns, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void note(cxp_notes *ns, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    char *dup;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ns->n == ns->cap) {
        size_t cap = ns->cap ? ns->cap * 2 : 16;
        char **v = (char **)realloc(ns->v, cap * sizeof(*v));
        if (!v)
            return;
        ns->v = v;
        ns->cap = cap;
    }
    dup = (char *)malloc(strlen(buf) + 1);
    if (!dup)
        return;
    strcpy(dup, buf);
    ns->v[ns->n++] = dup;
}

static void notes_free(cxp_notes *ns)
{
    for (size_t i = 0; i < ns->n; i++)
        free(ns->v[i]);
    free(ns->v);
    ns->v = NULL;
    ns->n = ns->cap = 0;
}

/* --------------------------------------------------------------- materials
 * The three fields extract_props.py reads off a Material: `cls` (record +0x00,
 * which picks the vertex stride), `flags` (+0x24) and the texture BASENAME.
 * parse_materials' animation half (+0x0C's per-frame pointer array, the atol
 * keyframe labels) has no bearing on any of them and is agent B's business in
 * cx_extract_track; the acceptance criterion is that a material either lands
 * in the table with exactly these three, or is absent -- the same admission
 * test, in the same order. */
typedef struct {
    int      valid;
    uint32_t cls;
    uint32_t flags;
    char     texture[64];
} cxp_mat;

typedef struct {
    cxp_mat *v;
    size_t   n;
} cxp_mats;

static void parse_materials(cxc_blob *d, cxp_mats *out)
{
    int64_t  mat_off = cxc_i32(d, 0x08);
    uint32_t mat_count = cxc_u16(d, 0x0C);
    char     name[128], base[64];

    out->v = NULL;
    out->n = 0;
    if (!(mat_off > 0 && (uint64_t)mat_off < (uint64_t)d->n))
        return;
    if (!(mat_count > 0 && mat_count < 4096))
        return;
    out->v = (cxp_mat *)calloc(mat_count, sizeof(cxp_mat));
    if (!out->v)
        return;
    out->n = mat_count;

    for (uint32_t i = 0; i < mat_count; i++) {
        int64_t m = mat_off + (int64_t)i * CXP_MAT_STRIDE;
        int64_t tex_ptr, tex_rec;
        uint32_t bd;

        if ((uint64_t)(m + CXP_MAT_STRIDE) > (uint64_t)d->n)
            break;
        tex_ptr = cxc_ptr(d, m + 0x0C, m);
        if (!(tex_ptr > 0 && (uint64_t)tex_ptr < (uint64_t)d->n - 4))
            continue;
        tex_rec = cxc_ptr(d, tex_ptr, m);
        if (!(tex_rec > 0 && (uint64_t)tex_rec < (uint64_t)d->n - 0x70))
            continue;
        bd = cxc_u32(d, tex_rec + 0x40);
        /* the texture record's name sits at +0x48 (old revision) or +0x44 */
        if (!cxc_cstr(d, tex_rec + ((bd == 4 || bd == 8 || bd == 32) ? 0x48 : 0x44),
                      64, name, sizeof(name)))
            continue;
        cxc_basename(name, base, sizeof(base));
        out->v[i].valid = 1;
        out->v[i].cls = cxc_u32(d, m);
        out->v[i].flags = cxc_u32(d, m + CXP_MAT_FLAGS_OFF);
        snprintf(out->v[i].texture, sizeof(out->v[i].texture), "%s", base);
    }
}

static const cxp_mat *mat_get(const cxp_mats *m, uint32_t idx)
{
    if (idx < m->n && m->v[idx].valid)
        return &m->v[idx];
    return NULL;                       /* python's dict .get() -> None */
}

/* ------------------------------------------------------------------ model */
typedef struct {
    float px, py, pz, nx, ny, nz, u, v;
} cxp_vertex;

typedef struct {
    int         index;
    float       bb_min[3], bb_max[3];
    uint32_t    cls;
    float       mass, radius, lod_near, lod_far;
    uint32_t    mat, mat_flags;
    char        texture[64];
    cxp_vertex *verts;
    size_t      nverts;
    uint16_t   *tris;              /* de-stripped index list, model-local */
    size_t      ntris;             /* number of INDICES, not triangles */
    uint32_t    first_vertex, first_index;
} cxp_model;

typedef struct {
    float    m[16];
    uint32_t model, cls, unit, flags, index;
} cxp_inst;

/* D3DVSDT_NORMPACKED3: x = bits 0..10 /1023, y = 11..21 /1023, z = 22..31
 * /511, all signed.  Same decode extract_track.py pins for the world. */
static void unpack_normal(uint32_t v, float *nx, float *ny, float *nz)
{
    int32_t x = (int32_t)(v & 0x7FFu);
    int32_t y = (int32_t)((v >> 11) & 0x7FFu);
    int32_t z = (int32_t)((v >> 22) & 0x3FFu);

    if (x > 1023) x -= 2048;
    if (y > 1023) y -= 2048;
    if (z > 511)  z -= 1024;
    *nx = (float)((double)x / 1023.0);
    *ny = (float)((double)y / 1023.0);
    *nz = (float)((double)z / 511.0);
}

/* (vertex_off, index_off, index_count) for the 0x14-byte block at `base`,
 * relocated the way FUN_0019B4E0 does it. */
static void mesh_block(cxc_blob *d, int64_t base,
                       int64_t *voff, int64_t *ioff, uint32_t *ncount)
{
    uint32_t common = cxc_u32(d, base);
    int32_t  vrel = cxc_i32(d, base + 0x04);
    int32_t  irel = cxc_i32(d, base + 0x10);
    int64_t  v;

    *ncount = cxc_u32(d, base + 0x0C);
    v = (int64_t)vrel + base;
    if ((common & 0x70000u) != 0x20000u)
        v &= 0x0FFFFFFF;
    *voff = v;
    *ioff = (int64_t)irel + base;
}

/* python's floor division / floored modulo, which differ from C's truncation
 * once `span` goes negative -- and the guard below depends on both. */
static int64_t floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        q--;
    return q;
}

static int64_t floormod(int64_t a, int64_t b)
{
    return a - floordiv(a, b) * b;
}

/* One triangle strip of u16 indices -> a triangle list, degenerates out. */
static uint16_t *destrip(cxc_blob *d, int64_t ioff, uint32_t n, size_t *out_n)
{
    uint16_t *idx, *out;
    size_t    k, o = 0;

    *out_n = 0;
    if (n < 3)
        return NULL;
    idx = (uint16_t *)malloc((size_t)n * sizeof(uint16_t));
    out = (uint16_t *)malloc((size_t)(n - 2) * 3u * sizeof(uint16_t));
    if (!idx || !out) {
        free(idx);
        free(out);
        return NULL;
    }
    for (k = 0; k < n; k++)
        idx[k] = cxc_u16(d, ioff + (int64_t)k * 2);
    for (k = 0; k + 2 < n; k++) {
        uint16_t a = idx[k], b = idx[k + 1], c = idx[k + 2];
        if (a == b || b == c || a == c)
            continue;
        /* keep a consistent winding across the strip */
        if (k & 1) {
            out[o++] = a; out[o++] = c; out[o++] = b;
        } else {
            out[o++] = a; out[o++] = b; out[o++] = c;
        }
    }
    free(idx);
    *out_n = o;
    return out;
}

/* --------------------------------------------------------------- the track */
typedef struct {
    cxc_blob d;
    cxp_mats mats;
    uint32_t n_models, n_inst, n_units;
    int64_t  t_models, t_class, t_xform, t_counts, t_lists;
    size_t   n_classes;            /* python's `list(d[t_class:t_class+n])` */
    cxp_notes notes;
} cxp_track;

static void track_check(cxp_track *t)
{
    double  stride;
    int64_t end;

    if (t->n_models == 0 || t->n_inst == 0)
        return;
    /* The +0x48 stride is not assumed: (hdr+0x4C - hdr+0x48) / hdr+0x40 is
     * exactly 64.000 on all 37 shipped static.dat files. */
    stride = (double)(t->t_counts - t->t_xform) / (double)t->n_inst;
    if (fabs(stride - 64.0) > 1e-6)
        note(&t->notes, "transform stride %.3f != 64", stride);
    /* Nor is the +0x44 length: hdr+0x44 + model_count rounded up to 16 lands
     * exactly on hdr+0x48 on every one of them. */
    end = t->t_class + (int64_t)t->n_models;
    if (!(t->t_xform - 16 < end && end <= t->t_xform))
        note(&t->notes, "class array does not abut the transform table");
}

static int track_open(cxp_track *t, const char *static_dat)
{
    memset(t, 0, sizeof(*t));
    if (cxc_blob_load(&t->d, static_dat) != 0)
        return -1;
    parse_materials(&t->d, &t->mats);
    t->n_models = cxc_u16(&t->d, 0x36);
    t->t_models = cxc_i32(&t->d, 0x3C);
    t->n_inst   = cxc_u16(&t->d, 0x40);
    t->t_class  = cxc_i32(&t->d, 0x44);
    t->t_xform  = cxc_i32(&t->d, 0x48);
    t->t_counts = cxc_i32(&t->d, 0x4C);
    t->t_lists  = cxc_i32(&t->d, 0x50);
    t->n_units  = cxc_u16(&t->d, 0x54);
    /* python slices the class array, so it silently truncates at EOF. */
    t->n_classes = 0;
    if (t->t_class >= 0 && (uint64_t)t->t_class < (uint64_t)t->d.n) {
        size_t avail = t->d.n - (size_t)t->t_class;
        t->n_classes = avail < t->n_models ? avail : t->n_models;
    }
    track_check(t);
    return 0;
}

static void track_close(cxp_track *t)
{
    free(t->mats.v);
    cxc_blob_free(&t->d);
    notes_free(&t->notes);
}

static cxp_model *track_models(cxp_track *t)
{
    cxp_model *out = (cxp_model *)calloc(t->n_models ? t->n_models : 1,
                                         sizeof(cxp_model));
    if (!out)
        return NULL;

    for (uint32_t i = 0; i < t->n_models; i++) {
        cxp_model *M = &out[i];
        int64_t    r = t->t_models + (int64_t)i * 0x70;
        int64_t    voff, ioff, span, nv;
        uint32_t   n, mat;
        const cxp_mat *m;
        int        has_n, stride;
        double     mass, r2;

        M->index = (int)i;
        for (int k = 0; k < 3; k++) {
            M->bb_max[k] = cxc_f32(&t->d, r + k * 4);
            M->bb_min[k] = cxc_f32(&t->d, r + 0x10 + k * 4);
        }
        mesh_block(&t->d, r + 0x20, &voff, &ioff, &n);
        mat = cxc_u16(&t->d, r + 0x5C);
        m = mat_get(&t->mats, mat);
        has_n = (m ? m->cls : 9u) != 8u;
        stride = has_n ? CXP_VSTRIDE_C9 : CXP_VSTRIDE_C8;
        span = ioff - voff;
        nv = floordiv(span, stride);
        if (span < 0 || nv <= 0 || n == 0 || floormod(span, stride) != 0) {
            char cls[16];
            if (m)
                snprintf(cls, sizeof(cls), "%u", m->cls);
            else
                snprintf(cls, sizeof(cls), "?");
            note(&t->notes, "model %u: LOD0 span %lld not a multiple of the "
                            "class-%s stride %d",
                 i, (long long)span, cls, stride);
            if (nv < 0)
                nv = 0;
        }

        M->nverts = (size_t)nv;
        if (M->nverts) {
            M->verts = (cxp_vertex *)calloc(M->nverts, sizeof(cxp_vertex));
            if (!M->verts) {
                M->nverts = 0;
            } else {
                for (size_t k = 0; k < M->nverts; k++) {
                    int64_t o = voff + (int64_t)k * stride;
                    cxp_vertex *V = &M->verts[k];
                    V->px = cxc_f32(&t->d, o);
                    V->py = cxc_f32(&t->d, o + 4);
                    V->pz = cxc_f32(&t->d, o + 8);
                    if (has_n) {
                        unpack_normal(cxc_u32(&t->d, o + 12),
                                      &V->nx, &V->ny, &V->nz);
                        V->u = cxc_f32(&t->d, o + 16);
                        V->v = cxc_f32(&t->d, o + 20);
                    } else {
                        V->nx = 0.0f; V->ny = 1.0f; V->nz = 0.0f;
                        V->u = cxc_f32(&t->d, o + 12);
                        V->v = cxc_f32(&t->d, o + 16);
                    }
                }
            }
        }
        if (n)
            M->tris = destrip(&t->d, ioff, n, &M->ntris);

        /* FUN_0011A020 @0x0011A137..0x0011A191 [C] -- footprint area x 200,
         * floored at 100.  Computed in double, exactly as the python does,
         * then rounded once on the way into the record. */
        mass = ((double)M->bb_max[2] - (double)M->bb_min[2]) *
               ((double)M->bb_max[0] - (double)M->bb_min[0]) * 200.0;
        if (mass <= 100.0)
            mass = 100.0;
        M->mass = (float)mass;
        r2 = (double)M->bb_max[0] * (double)M->bb_max[0] +
             (double)M->bb_max[1] * (double)M->bb_max[1] +
             (double)M->bb_max[2] * (double)M->bb_max[2];
        M->radius = (float)sqrt(r2);            /* SQRTSS @0x0011A0DE */

        M->cls = (i < t->n_classes) ? cxc_u8(&t->d, t->t_class + (int64_t)i) : 0u;
        M->lod_near = cxc_f32(&t->d, r + 0x64);
        M->lod_far  = cxc_f32(&t->d, r + 0x68);
        M->mat = mat;
        M->mat_flags = m ? m->flags : 0u;
        if (m)
            snprintf(M->texture, sizeof(M->texture), "%s", m->texture);
        else
            M->texture[0] = '\0';
    }
    return out;
}

static void models_free(cxp_model *models, uint32_t n)
{
    if (!models)
        return;
    for (uint32_t i = 0; i < n; i++) {
        free(models[i].verts);
        free(models[i].tris);
    }
    free(models);
}

/* Walk the per-unit lists -- the only unambiguous model<->instance binding in
 * the file (FUN_00110420 @0x001109E8..0x00110A03). */
static cxp_inst *track_instances(cxp_track *t, size_t *out_n)
{
    cxp_inst *out = NULL;
    size_t    n = 0, cap = 0, nseen = 0;
    unsigned char *seen;

    *out_n = 0;
    seen = (unsigned char *)calloc(t->n_inst ? t->n_inst : 1, 1);
    if (!seen)
        return NULL;

    for (uint32_t unit = 0; unit < t->n_units; unit++) {
        int64_t cbase = cxc_i32(&t->d, t->t_counts + (int64_t)unit * 4);
        int64_t lbase = cxc_i32(&t->d, t->t_lists + (int64_t)unit * 4);
        size_t  ncounts;

        if (cbase <= 0 || lbase <= 0)
            continue;
        /* `counts = d[cbase:cbase + n_models]` -- a slice, so short at EOF. */
        ncounts = 0;
        if ((uint64_t)cbase < (uint64_t)t->d.n) {
            size_t avail = t->d.n - (size_t)cbase;
            ncounts = avail < t->n_models ? avail : t->n_models;
        }
        for (uint32_t mi = 0; mi < t->n_models; mi++) {
            uint32_t cnt = (mi < ncounts)
                           ? cxc_u8(&t->d, cbase + (int64_t)mi) : 0u;
            int64_t  s;

            if (!cnt)
                continue;
            s = cxc_i32(&t->d, lbase + (int64_t)mi * 4);
            if (s <= 0) {
                note(&t->notes, "unit %u model %u: %u entries, null list",
                     unit, mi, cnt);
                continue;
            }
            for (uint32_t j = 0; j < cnt; j++) {
                int64_t  e = s + (int64_t)j * 4;   /* 4 BYTES PER LIST ENTRY */
                uint16_t idx = cxc_u16(&t->d, e);
                uint8_t  cls = cxc_u8(&t->d, e + 2);
                uint8_t  flags = cxc_u8(&t->d, e + 3);
                cxp_inst *I;

                if (idx >= t->n_inst) {
                    note(&t->notes, "instance index %u out of range", idx);
                    continue;
                }
                if (seen[idx])
                    note(&t->notes, "instance %u listed twice", idx);
                else
                    nseen++;
                seen[idx] = 1;

                if (n == cap) {
                    size_t nc = cap ? cap * 2 : 64;
                    cxp_inst *nv = (cxp_inst *)realloc(out, nc * sizeof(*out));
                    if (!nv) {
                        free(seen);
                        free(out);
                        return NULL;
                    }
                    out = nv;
                    cap = nc;
                }
                I = &out[n++];
                for (int k = 0; k < 16; k++)
                    I->m[k] = cxc_f32(&t->d,
                                      t->t_xform + (int64_t)idx * 0x40 + k * 4);
                I->model = mi;
                I->cls = cls;
                I->unit = unit;
                I->flags = flags;
                I->index = idx;
            }
        }
    }
    if (nseen != t->n_inst)
        note(&t->notes, "%zu of %u instances reached by the unit lists",
             nseen, t->n_inst);
    free(seen);
    *out_n = n;
    return out;
}

/* ----------------------------------------------------------------- build */
static int build(cxp_track *t, cxp_model *models, cxp_inst *insts,
                 size_t n_inst, cxc_buf *out)
{
    size_t   total_v = 0, total_i = 0;
    uint32_t off_models, off_inst, off_vtx, off_idx;

    for (uint32_t i = 0; i < t->n_models; i++) {
        models[i].first_vertex = (uint32_t)total_v;
        models[i].first_index = (uint32_t)total_i;
        total_v += models[i].nverts;
        total_i += models[i].ntris;
        for (size_t k = 0; k < models[i].ntris; k++) {
            if (models[i].tris[k] >= models[i].nverts) {
                note(&t->notes, "model %u index out of range", i);
                break;
            }
        }
    }

    off_models = CXP_OFF_MODELS;
    off_inst = off_models + (uint32_t)(t->n_models * CXP_MODEL_REC);
    off_vtx = off_inst + (uint32_t)(n_inst * CXP_INST_REC);
    off_idx = off_vtx + (uint32_t)(total_v * 0x20);

    cxc_put_bytes(out, CXP_MAGIC, 4);
    cxc_put_u32(out, CXP_VERSION);
    cxc_put_u32(out, t->n_models);
    cxc_put_u32(out, (uint32_t)n_inst);
    cxc_put_u32(out, (uint32_t)total_v);
    cxc_put_u32(out, (uint32_t)total_i);
    cxc_put_u32(out, off_models);
    cxc_put_u32(out, off_inst);
    cxc_put_u32(out, off_vtx);
    cxc_put_u32(out, off_idx);
    cxc_put_u32(out, t->n_units);
    cxc_pad_to(out, off_models);

    for (uint32_t i = 0; i < t->n_models; i++) {
        cxp_model *M = &models[i];
        char base[64];
        size_t len;
        size_t start = out->n;

        for (int k = 0; k < 3; k++)
            cxc_put_f32(out, M->bb_min[k]);
        for (int k = 0; k < 3; k++)
            cxc_put_f32(out, M->bb_max[k]);
        cxc_put_u32(out, M->first_vertex);
        cxc_put_u32(out, (uint32_t)M->nverts);
        cxc_put_u32(out, M->first_index);
        cxc_put_u32(out, (uint32_t)M->ntris);
        cxc_put_u32(out, M->cls);
        cxc_put_f32(out, M->mass);
        cxc_put_f32(out, M->radius);
        cxc_put_f32(out, M->lod_near);
        cxc_put_f32(out, M->lod_far);
        cxc_put_u32(out, M->mat_flags);
        /* char[32] texture basename, truncated to 31 and NUL padded. */
        cxc_basename(M->texture, base, sizeof(base));
        len = strlen(base);
        if (len > 31)
            len = 31;
        cxc_put_bytes(out, base, len);
        cxc_put_zero(out, 32 - len);
        if (out->n - start != CXP_MODEL_REC) {
            fprintf(stderr, "cextract: props model record is %zu bytes\n",
                    out->n - start);
            return -1;
        }
    }

    for (size_t i = 0; i < n_inst; i++) {
        for (int k = 0; k < 16; k++)
            cxc_put_f32(out, insts[i].m[k]);
        cxc_put_u32(out, insts[i].model);
        cxc_put_u32(out, insts[i].cls);
        cxc_put_u32(out, insts[i].unit);
        cxc_put_u32(out, insts[i].flags);
    }
    for (uint32_t i = 0; i < t->n_models; i++) {
        for (size_t k = 0; k < models[i].nverts; k++) {
            cxp_vertex *V = &models[i].verts[k];
            cxc_put_f32(out, V->px);
            cxc_put_f32(out, V->py);
            cxc_put_f32(out, V->pz);
            cxc_put_f32(out, V->nx);
            cxc_put_f32(out, V->ny);
            cxc_put_f32(out, V->nz);
            cxc_put_f32(out, V->u);
            cxc_put_f32(out, V->v);
        }
    }
    for (uint32_t i = 0; i < t->n_models; i++)
        for (size_t k = 0; k < models[i].ntris; k++)
            cxc_put_u16(out, models[i].tris[k]);
    return 0;
}

static void report(const char *tid, cxp_track *t, cxp_model *models,
                   cxp_inst *insts, size_t n_inst, size_t size)
{
    printf("%-9s %3u models  %4zu instances  %4u units  props.bin %7zu B\n",
           tid, t->n_models, n_inst, t->n_units, size);
    for (uint32_t i = 0; i < t->n_models; i++) {
        cxp_model *M = &models[i];
        size_t placed = 0;
        for (size_t k = 0; k < n_inst; k++)
            if (insts[k].model == i)
                placed++;
        printf("    m%-2u cls=%u %-20.20s %6.1f kg  %5.2f x %5.2f x %5.2f  "
               "%4zu v %4zu tri  lod %.0f/%.0f%s\n",
               i, M->cls, M->texture, (double)M->mass,
               (double)(M->bb_max[0] - M->bb_min[0]),
               (double)(M->bb_max[1] - M->bb_min[1]),
               (double)(M->bb_max[2] - M->bb_min[2]),
               M->nverts, M->ntris / 3, (double)M->lod_near,
               (double)M->lod_far,
               M->cls == CXP_CONE_CLASS ? "  <-- CONE" : "");
        printf("        %zu placed\n", placed);
    }
    for (size_t i = 0; i < t->notes.n; i++)
        printf("    NOTE: %s\n", t->notes.v[i]);
}

int cx_extract_props(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir)
{
    cxp_track  t;
    cxp_model *models = NULL;
    cxp_inst  *insts = NULL;
    size_t     n_inst = 0;
    cxc_buf    out;
    char       path[4096];
    int        rc = -1;

    (void)game_dir;                 /* the props live entirely in static.dat */

    memset(&out, 0, sizeof(out));
    cxc_join(path, sizeof(path), track_dir, "static.dat");
    if (track_open(&t, path) != 0)
        return -1;

    models = track_models(&t);
    if (!models)
        goto done;
    insts = track_instances(&t, &n_inst);
    if (!insts && n_inst)
        goto done;
    if (build(&t, models, insts, n_inst, &out) != 0)
        goto done;
    if (cxc_oob(&t.d)) {
        fprintf(stderr, "cextract: props: %d out-of-range read(s) in %s -- the "
                        "python spec would have raised here\n",
                cxc_oob(&t.d), path);
        goto done;
    }
    cxc_join(path, sizeof(path), out_dir, "props.bin");
    if (cxc_buf_write(&out, path) != 0)
        goto done;
    report(track_id, &t, models, insts, n_inst, out.n);
    rc = 0;

done:
    cxc_buf_free(&out);
    free(insts);
    models_free(models, t.n_models);
    track_close(&t);
    return rc;
}
