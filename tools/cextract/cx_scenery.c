/* cx_scenery.c -- INSTANCED TRACK SCENERY out of static.dat's FIRST 0x70-record
 * table (hdr +0x34 count / +0x38 table).  -> <out_dir>/scenery.bin ('B3SC').
 *
 * ======================================================================= NEW
 * THIS STAGE HAS NO PYTHON ORACLE.  Every other member of CX_STAGE_LIST is a
 * byte-identical port of an archived tools/py_extract_archive/extract_*.py;
 * this one is a NEW RECOVERY and the archive is deliberately left alone, so
 * tools/cextract/verify_cextract.py's oracle diff cannot cover scenery.bin --
 * it is asserted by tools/validate_scenery.py instead, which re-derives every
 * offset below straight out of the shipped static.dat/streamed.dat.
 *
 * WHY IT EXISTS
 * -------------
 * tools/py_extract_archive/extract_track.py:23 records `+0x34 u16 instanced-
 * prop count / +0x38 i32 table  (not extracted, [S])` and :719-727 argues the
 * omission is a harmless LOD choice.  It is not.  On US_C1_V1 that table is 36
 * model records -- palms, hero trees, bushes, lamp posts, traffic-light posts,
 * tram posts, telegraph poles, overhead/hospital/speed signs, phone boxes,
 * park benches, moored boats and PARKED VEHICLES (bus / Suv_1 / PeopleCarrier
 * / FedEx_Van) -- placed 1370 times, and NONE of it was on screen.  The tell:
 * the track ships `WF_TreePALM1` / `WF_tree_palm` textures and a
 * `WF_Palm_shadow` DECAL that the world mesh does draw, so the port painted
 * palm-tree shadows on the road with no palm tree above them.
 *
 * The 36 records' materials are exactly the shader classes 8 and 9 that
 * extract_track.py:957-984 calls "the foliage/prop/cone families" and then
 * notes no shipped track uses -- because the geometry that uses them is not in
 * the world mesh at all.  It is here.
 *
 * ============================================================== WHERE THEY ARE
 *   hdr +0x34  u16   RECORD count                                        [C]
 *   hdr +0x38  i32   RECORD table, 0x70 bytes each -- the SAME record shape
 *                    as the destructible-prop table at +0x36/+0x3C that
 *                    cx_props.c reads (FUN_0019B4E0's relocation loop).
 *
 * The reader is FUN_001ADA40 @0x001ADBC0..0x001ADD40 [C-disasm]:
 *     sVar5 = *(short *)(hdr + 0x34);          ; the record count
 *     iVar14 = *(int *)(hdr + 0x38);           ; the record table
 *     uVar6  = *(u16 *)(rec + 0x62);           ; LOD-present flags
 *     if (uVar6 & 2) { ... FUN_0003a740(rec, mattab + rec[0x60]*0x28, ...) }
 *     iVar16 += 0x70;                          ; <-- the 0x70 stride
 *
 * PLACEMENT is PER STREAMED UNIT, in the unit's own LOD block -- not in
 * static.dat like the destructible props.  The relocation is
 * FUN_0019D7A0 @0x0019D7D9 -> FUN_0019D760 [C-disasm]:
 *
 *     0019d7d9  LEA  EDX,[ESI + 0xa8]      ; ESI = the unit LOD block
 *     0019d7df  JMP  0x0019d760
 *     0019d760  TEST EDI,EDI               ; EDI = hdr+0x34, the record count,
 *                                          ;   loaded at 0x0019cbd1
 *                                          ;   MOVSX EDI,word ptr [EAX + 0x34]
 *     0019d764  MOV  ECX,[EDX]     ADD ECX,EDX   MOV [EDX],ECX
 *     0019d766  MOV  EAX,[EDX + 4] ADD EAX,EDX   MOV [EDX + 4],EAX
 *     0019d780  MOV  EAX,[EDX + 4] MOV ECX,[EAX + i*4]
 *               TEST ECX,ECX  JZ skip   ADD ECX,EDX   MOV [EAX],ECX
 *
 * so, with `B = unit_block + 0xA8` as the base for ALL THREE relocations:
 *     B[0]  i32  offset (from B) of  u8  counts[record_count]
 *     B[4]  i32  offset (from B) of  i32 lists [record_count]
 *     lists[k]  i32  offset (from B) of counts[k] instance transforms, or 0
 *
 * Note the base: retail adds EDX (= block+0xA8), NOT the block start the way
 * it does for +0xA0 (collision) and +0xA4 (light probes) at 0x0019d7b9 /
 * 0x0019d7cb.  Decoding these two against the block start yields index-buffer
 * garbage, which is the check that the +0xA8 base is right.            [C]
 *
 * The consumer of counts/lists is FUN_001ADA40 @0x001ADCB0..0x001ADCF0:
 *     piVar8 = *(int **)(ctx + 0x14c + unit*4);      ; the pair above
 *     count  = *(u8  *)(piVar8[0] + record);
 *     list   = *(u32 *)(piVar8[1] + record*4);
 * and FUN_0003A840 walks `list` reading param_3[0xc..0xe] as the instance
 * POSITION and copying 0x10 dwords out of it -- i.e. 0x40 bytes, a 4x4
 * row-major matrix with the position in row 3, exactly the destructible-prop
 * instance record.  The file agrees: every list offset is 0x40-aligned and
 * consecutive lists differ by exactly count*0x40.                      [C]
 *
 * ============================================================ THE MODEL RECORD
 * Identical to cx_props.c's (that file carries the full derivation):
 *     +0x00 f32[4] bbox MAX     +0x10 f32[4] bbox MIN
 *     +0x20 mesh block LOD0     +0x34 LOD1 (iff +0x62 & 1)  +0x48 LOD2 (& 2)
 *     +0x5C/+0x5E/+0x60 u16 LOD0/LOD1/LOD2 material index
 *     +0x62 u16 LOD-present flags   +0x64 f32 near   +0x68 f32 far
 *     +0x6C i32 extra (FUN_001ADA40 reads it into the cull MODE iff +0x62 & 4)
 * mesh block (0x14): u32 Common, i32 vtx (rel to the block), u32 lock,
 *                    u32 index count, i32 idx (rel to the block).
 * Vertex stride from the material's shader class: 8 -> 0x14 (pos+uv),
 * 9 -> 0x18 (pos + NORMPACKED3 + uv).  Indices are ONE u16 triangle strip.
 *
 * ONLY LOD0 IS EMITTED, the same choice cx_props.c makes.  LOD1/LOD2 are
 * reported per model so a future runtime can switch; retail's own draw of this
 * table is the LOD2/impostor pass, and which distance gates which LOD is [?]
 * (FUN_0003a740's stack argument order does not decompile cleanly), so the
 * runtime is given retail's raw +0x64/+0x68 pair and does its own cull.
 *
 * ================================================================ scenery.bin
 * Little-endian, RAW GAME SPACE (the Z flip to the harness' GL frame is the
 * loader's job, exactly as for props.bin).
 *     +0x00 'B3SC'   +0x04 u32 version=1
 *     +0x08 u32 model_count      +0x0C u32 instance_count
 *     +0x10 u32 vertex_count     +0x14 u32 index_count
 *     +0x18 u32 off_models       +0x1C u32 off_instances
 *     +0x20 u32 off_vertices     +0x24 u32 off_indices
 *     +0x28 u32 unit_count       +0x2C u32 reserved
 *   model 0x60:
 *     +0x00 f32[3] bb_min        +0x0C f32[3] bb_max
 *     +0x18 u32 first_vertex     +0x1C u32 vertex_count
 *     +0x20 u32 first_index      +0x24 u32 index_count  (values MODEL-LOCAL)
 *     +0x28 u32 record_index     +0x2C f32 lod_near     +0x30 f32 lod_far
 *     +0x34 u32 material_flags   +0x38 u32 shader_class +0x3C u32 lod_flags
 *     +0x40 char[32] texture basename
 *   instance 0x50:
 *     +0x00 f32[16] 4x4 row-major (rows right/up/at, row3 position)
 *     +0x40 u32 model  +0x44 u32 unit  +0x48 u32 record  +0x4C u32 reserved
 *   vertex 0x20: f32 pos[3], f32 normal[3], f32 uv[2]   (class 8 -> +Y normal)
 *   index: u16
 */
#include "cx_common_c.h"
#include "cx_extract.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CXS_REC_STRIDE   0x70
#define CXS_MAT_STRIDE   0x28
#define CXS_XFORM_STRIDE 0x40
#define CXS_VSTRIDE_C9   0x18
#define CXS_VSTRIDE_C8   0x14
#define CXS_INST_BASE    0xA8   /* the unit block offset AND relocation base */

/* --------------------------------------------------------------- materials */
typedef struct {
    int      present;
    uint32_t cls, flags;
    char     texture[64];
} cxs_mat;

typedef struct { cxs_mat *v; size_t n; } cxs_mats;

/* Same fields cx_props.c's parse_materials takes, from the same record. */
static void cxs_parse_materials(cxc_blob *d, cxs_mats *out)
{
    int64_t  mat_off = cxc_i32(d, 0x08);
    uint32_t mat_count = cxc_u16(d, 0x0C);

    out->v = NULL;
    out->n = 0;
    if (!(mat_off > 0 && (uint64_t)mat_off < (uint64_t)d->n))
        return;
    if (!(mat_count > 0 && mat_count < 4096))
        return;
    out->v = (cxs_mat *)calloc(mat_count, sizeof(cxs_mat));
    if (!out->v)
        return;
    out->n = mat_count;

    for (uint32_t i = 0; i < mat_count; i++) {
        int64_t m = mat_off + (int64_t)i * CXS_MAT_STRIDE;
        int64_t tex_ptr, tex_rec;
        uint32_t bd;
        char name[256], base[64];

        if (m + CXS_MAT_STRIDE > (int64_t)d->n)
            break;
        tex_ptr = cxc_ptr(d, m + 0x0C, m);
        if (!(tex_ptr > 0 && (uint64_t)tex_ptr < (uint64_t)d->n - 4))
            continue;
        tex_rec = cxc_ptr(d, tex_ptr, m);
        if (!(tex_rec > 0 && (uint64_t)tex_rec < (uint64_t)d->n - 0x70))
            continue;
        bd = cxc_u32(d, tex_rec + 0x40);
        /* the texture record's name is at +0x48 (old revision) or +0x44 */
        if (!cxc_cstr(d, tex_rec + ((bd == 4 || bd == 8 || bd == 32) ? 0x48 : 0x44),
                      64, name, sizeof(name)))
            continue;
        cxc_basename(name, base, sizeof(base));
        out->v[i].present = 1;
        out->v[i].cls   = cxc_u32(d, m);
        out->v[i].flags = cxc_u32(d, m + 0x24);
        snprintf(out->v[i].texture, sizeof(out->v[i].texture), "%s", base);
    }
}

static const cxs_mat *cxs_mat_get(const cxs_mats *m, uint32_t idx)
{
    if (!m->v || idx >= m->n || !m->v[idx].present)
        return NULL;
    return &m->v[idx];
}

/* ----------------------------------------------------------------- geometry */
static void cxs_unpack_normal(uint32_t v, float *nx, float *ny, float *nz)
{
    /* D3DVSDT_NORMPACKED3: x/y 11-bit signed, z 10-bit signed. */
    int32_t x = (int32_t)(v & 0x7FF);
    int32_t y = (int32_t)((v >> 11) & 0x7FF);
    int32_t z = (int32_t)((v >> 22) & 0x3FF);

    if (x >= 1024) x -= 2048;
    if (y >= 1024) y -= 2048;
    if (z >= 512)  z -= 1024;
    *nx = (float)x / 1023.0f;
    *ny = (float)y / 1023.0f;
    *nz = (float)z / 511.0f;
}

/* The 0x14-byte mesh block, relocated the way FUN_0019B4E0 does it. */
static void cxs_mesh_block(cxc_blob *d, int64_t base,
                           int64_t *voff, int64_t *ioff, uint32_t *n)
{
    uint32_t common = cxc_u32(d, base);
    int64_t  v = (int64_t)cxc_i32(d, base + 0x04) + base;

    if ((common & 0x70000) != 0x20000)
        v &= 0x0FFFFFFF;
    *voff = v;
    *ioff = (int64_t)cxc_i32(d, base + 0x10) + base;
    *n = cxc_u32(d, base + 0x0C);
}

/* ------------------------------------------------------------- accumulators */
typedef struct {
    float bb_min[3], bb_max[3];
    uint32_t first_vertex, vertex_count, first_index, index_count;
    uint32_t record, mat_flags, cls, lod_flags;
    float    lod_near, lod_far;
    char     texture[64];
} cxs_model;

typedef struct {
    float    m[16];
    uint32_t model, unit, record;
} cxs_inst;

typedef struct { float p[3], n[3], uv[2]; } cxs_vtx;

#define CXS_VEC(T, name)                                                      \
    typedef struct { T *v; size_t n, cap; } name;                             \
    static int name##_push(name *a, const T *e)                               \
    {                                                                         \
        if (a->n == a->cap) {                                                 \
            size_t nc = a->cap ? a->cap * 2 : 256;                            \
            T *nv = (T *)realloc(a->v, nc * sizeof(T));                       \
            if (!nv) return -1;                                               \
            a->v = nv; a->cap = nc;                                           \
        }                                                                     \
        a->v[a->n++] = *e;                                                    \
        return 0;                                                             \
    }

CXS_VEC(cxs_model, cxs_models)
CXS_VEC(cxs_inst,  cxs_insts)
CXS_VEC(cxs_vtx,   cxs_verts)
CXS_VEC(uint16_t,  cxs_idx)

/* -------------------------------------------------------------------- stage */
int cx_extract_scenery(const char *game_dir, const char *track_dir,
                       const char *track_id, const char *out_dir)
{
    cxc_blob   sd, st;
    cxs_mats   mats;
    cxs_models models;
    cxs_insts  insts;
    cxs_verts  verts;
    cxs_idx    idx;
    cxc_buf    out;
    char       path[4096];
    int32_t   *model_of_rec = NULL;
    uint32_t   nrec, nunit;
    int64_t    rec_tbl, unit_tbl;
    size_t     off_models, off_inst, off_vtx, off_idx;
    int        rc = -1, notes = 0;

    (void)game_dir;

    memset(&sd, 0, sizeof sd);   memset(&st, 0, sizeof st);
    memset(&mats, 0, sizeof mats);
    memset(&models, 0, sizeof models);  memset(&insts, 0, sizeof insts);
    memset(&verts, 0, sizeof verts);    memset(&idx, 0, sizeof idx);
    memset(&out, 0, sizeof out);

    cxc_join(path, sizeof path, track_dir, "static.dat");
    if (cxc_blob_load(&sd, path) != 0)
        return -1;
    cxc_join(path, sizeof path, track_dir, "streamed.dat");
    if (cxc_blob_load(&st, path) != 0)
        goto done;

    cxs_parse_materials(&sd, &mats);

    nrec     = cxc_u16(&sd, 0x34);
    rec_tbl  = cxc_i32(&sd, 0x38);
    nunit    = cxc_u16(&sd, 0x54);
    unit_tbl = cxc_i32(&sd, 0x58);

    if (!nrec || rec_tbl <= 0 || (uint64_t)rec_tbl >= (uint64_t)sd.n) {
        /* A track with no instanced scenery is legal; say so and write
         * nothing, exactly as the envmap stage does for its four tracks. */
        printf("[scenery] %-9s no +0x34 table -- nothing to write\n", track_id);
        rc = 0;
        goto done;
    }

    model_of_rec = (int32_t *)malloc((size_t)nrec * sizeof(int32_t));
    if (!model_of_rec)
        goto done;

    /* ---- the 0x70 model records ---- */
    for (uint32_t i = 0; i < nrec; i++) {
        int64_t  r = rec_tbl + (int64_t)i * CXS_REC_STRIDE;
        int64_t  voff, ioff;
        uint32_t n = 0, mat, stride;
        const cxs_mat *m;
        cxs_model M;
        int64_t  span;
        uint32_t nv;

        model_of_rec[i] = -1;
        if (r + CXS_REC_STRIDE > (int64_t)sd.n)
            break;

        cxs_mesh_block(&sd, r + 0x20, &voff, &ioff, &n);
        mat = cxc_u16(&sd, r + 0x5C);
        m   = cxs_mat_get(&mats, mat);
        /* class 8 has no normal; anything else takes the class-9 stride, the
         * same defaulting cx_props.c uses. */
        stride = (m && m->cls == 8) ? CXS_VSTRIDE_C8 : CXS_VSTRIDE_C9;
        span   = ioff - voff;
        if (span <= 0 || n < 3 || (span % (int64_t)stride) != 0 ||
            voff <= 0 || (uint64_t)ioff + (uint64_t)n * 2 > (uint64_t)sd.n) {
            fprintf(stderr, "cextract: scenery %s: record %u: LOD0 span %lld "
                            "not a class-%u mesh (n=%u) -- skipped\n",
                    track_id, i, (long long)span, m ? m->cls : 0u, n);
            notes++;
            continue;
        }
        nv = (uint32_t)(span / (int64_t)stride);

        memset(&M, 0, sizeof M);
        for (int k = 0; k < 3; k++) {
            M.bb_max[k] = cxc_f32(&sd, r + (int64_t)k * 4);
            M.bb_min[k] = cxc_f32(&sd, r + 0x10 + (int64_t)k * 4);
        }
        M.first_vertex = (uint32_t)verts.n;
        M.vertex_count = nv;
        M.first_index  = (uint32_t)idx.n;
        M.record       = i;
        M.mat_flags    = m ? m->flags : 0u;
        M.cls          = m ? m->cls : 0u;
        M.lod_flags    = cxc_u16(&sd, r + 0x62);
        M.lod_near     = cxc_f32(&sd, r + 0x64);
        M.lod_far      = cxc_f32(&sd, r + 0x68);
        if (m) snprintf(M.texture, sizeof M.texture, "%s", m->texture);

        for (uint32_t k = 0; k < nv; k++) {
            int64_t  o = voff + (int64_t)k * stride;
            cxs_vtx  V;

            V.p[0] = cxc_f32(&sd, o);
            V.p[1] = cxc_f32(&sd, o + 4);
            V.p[2] = cxc_f32(&sd, o + 8);
            if (stride == CXS_VSTRIDE_C9) {
                cxs_unpack_normal(cxc_u32(&sd, o + 12),
                                  &V.n[0], &V.n[1], &V.n[2]);
                V.uv[0] = cxc_f32(&sd, o + 16);
                V.uv[1] = cxc_f32(&sd, o + 20);
            } else {
                V.n[0] = 0.0f; V.n[1] = 1.0f; V.n[2] = 0.0f;
                V.uv[0] = cxc_f32(&sd, o + 12);
                V.uv[1] = cxc_f32(&sd, o + 16);
            }
            if (cxs_verts_push(&verts, &V) != 0)
                goto done;
        }

        /* ONE u16 triangle strip -> a triangle list, degenerates dropped and
         * the winding kept consistent (cx_props.c's destrip). */
        for (uint32_t k = 0; k + 2 < n; k++) {
            uint16_t a = cxc_u16(&sd, ioff + (int64_t)k * 2);
            uint16_t b = cxc_u16(&sd, ioff + (int64_t)(k + 1) * 2);
            uint16_t c = cxc_u16(&sd, ioff + (int64_t)(k + 2) * 2);
            uint16_t t[3];

            if (a == b || b == c || a == c)
                continue;
            if (k & 1) { t[0] = a; t[1] = c; t[2] = b; }
            else       { t[0] = a; t[1] = b; t[2] = c; }
            for (int q = 0; q < 3; q++) {
                if (t[q] >= nv) { t[0] = t[1] = t[2] = 0; break; }
                if (cxs_idx_push(&idx, &t[q]) != 0)
                    goto done;
            }
        }
        M.index_count = (uint32_t)idx.n - M.first_index;
        if (!M.index_count) {
            fprintf(stderr, "cextract: scenery %s: record %u: empty strip\n",
                    track_id, i);
            notes++;
            verts.n = M.first_vertex;
            continue;
        }
        model_of_rec[i] = (int32_t)models.n;
        if (cxs_models_push(&models, &M) != 0)
            goto done;
    }

    /* ---- the per-unit placement lists ---- */
    for (uint32_t u = 0; u < nunit; u++) {
        int64_t  bo = cxc_i32(&sd, unit_tbl + (int64_t)u * 0x10 + 4);
        int64_t  bs = cxc_i32(&sd, unit_tbl + (int64_t)u * 0x10 + 12);
        cxc_blob blk;
        int64_t  cbase, lbase;

        if (bs <= 0)
            continue;
        blk = cxc_blob_slice(&st, bo, bs);
        /* B = block + 0xA8, and every offset below is relative to B.   [C] */
        cbase = CXS_INST_BASE + (int64_t)cxc_i32(&blk, CXS_INST_BASE);
        lbase = CXS_INST_BASE + (int64_t)cxc_i32(&blk, CXS_INST_BASE + 4);
        if (cbase <= 0 || lbase <= 0 ||
            cbase + (int64_t)nrec > bs || lbase + (int64_t)nrec * 4 > bs)
            continue;

        for (uint32_t k = 0; k < nrec; k++) {
            uint32_t cnt = cxc_u8(&blk, cbase + (int64_t)k);
            int32_t  rel = cxc_i32(&blk, lbase + (int64_t)k * 4);
            int64_t  lo;

            if (!cnt || !rel)
                continue;
            lo = CXS_INST_BASE + (int64_t)rel;
            if (lo <= 0 || lo + (int64_t)cnt * CXS_XFORM_STRIDE > bs) {
                fprintf(stderr, "cextract: scenery %s: unit %u record %u: "
                                "instance list out of block\n", track_id, u, k);
                notes++;
                continue;
            }
            if (k >= nrec || model_of_rec[k] < 0)
                continue;      /* a record whose LOD0 mesh did not parse */
            for (uint32_t j = 0; j < cnt; j++) {
                cxs_inst I;
                int64_t o = lo + (int64_t)j * CXS_XFORM_STRIDE;

                for (int q = 0; q < 16; q++)
                    I.m[q] = cxc_f32(&blk, o + (int64_t)q * 4);
                I.model  = (uint32_t)model_of_rec[k];
                I.unit   = u;
                I.record = k;
                if (cxs_insts_push(&insts, &I) != 0)
                    goto done;
            }
        }
        if (cxc_oob(&blk)) {
            fprintf(stderr, "cextract: scenery %s: unit %u read outside its "
                            "%lld-byte LOD block\n",
                    track_id, u, (long long)bs);
            notes++;
        }
    }

    /* ---- write ---- */
    off_models = 0x30;
    off_inst   = off_models + models.n * 0x60;
    off_vtx    = off_inst + insts.n * 0x50;
    off_idx    = off_vtx + verts.n * 0x20;

    cxc_put_bytes(&out, "B3SC", 4);
    cxc_put_u32(&out, 1u);
    cxc_put_u32(&out, (uint32_t)models.n);
    cxc_put_u32(&out, (uint32_t)insts.n);
    cxc_put_u32(&out, (uint32_t)verts.n);
    cxc_put_u32(&out, (uint32_t)idx.n);
    cxc_put_u32(&out, (uint32_t)off_models);
    cxc_put_u32(&out, (uint32_t)off_inst);
    cxc_put_u32(&out, (uint32_t)off_vtx);
    cxc_put_u32(&out, (uint32_t)off_idx);
    cxc_put_u32(&out, nunit);
    cxc_put_u32(&out, 0u);

    for (size_t i = 0; i < models.n; i++) {
        const cxs_model *M = &models.v[i];
        char base[32];

        for (int k = 0; k < 3; k++) cxc_put_f32(&out, M->bb_min[k]);
        for (int k = 0; k < 3; k++) cxc_put_f32(&out, M->bb_max[k]);
        cxc_put_u32(&out, M->first_vertex);
        cxc_put_u32(&out, M->vertex_count);
        cxc_put_u32(&out, M->first_index);
        cxc_put_u32(&out, M->index_count);
        cxc_put_u32(&out, M->record);
        cxc_put_f32(&out, M->lod_near);
        cxc_put_f32(&out, M->lod_far);
        cxc_put_u32(&out, M->mat_flags);
        cxc_put_u32(&out, M->cls);
        cxc_put_u32(&out, M->lod_flags);
        memset(base, 0, sizeof base);
        snprintf(base, sizeof base, "%s", M->texture);
        cxc_put_bytes(&out, base, sizeof base);
    }
    for (size_t i = 0; i < insts.n; i++) {
        for (int k = 0; k < 16; k++) cxc_put_f32(&out, insts.v[i].m[k]);
        cxc_put_u32(&out, insts.v[i].model);
        cxc_put_u32(&out, insts.v[i].unit);
        cxc_put_u32(&out, insts.v[i].record);
        cxc_put_u32(&out, 0u);
    }
    for (size_t i = 0; i < verts.n; i++) {
        for (int k = 0; k < 3; k++) cxc_put_f32(&out, verts.v[i].p[k]);
        for (int k = 0; k < 3; k++) cxc_put_f32(&out, verts.v[i].n[k]);
        for (int k = 0; k < 2; k++) cxc_put_f32(&out, verts.v[i].uv[k]);
    }
    for (size_t i = 0; i < idx.n; i++)
        cxc_put_u16(&out, idx.v[i]);

    if (cxc_oob(&sd) || cxc_oob(&st)) {
        fprintf(stderr, "cextract: scenery %s: out-of-range read\n", track_id);
        goto done;
    }

    cxc_join(path, sizeof path, out_dir, "scenery.bin");
    if (cxc_buf_write(&out, path) != 0)
        goto done;

    printf("[scenery] %-9s records %2u  models %2zu  instances %5zu  "
           "verts %6zu  tris %6zu  notes %d  -> %s\n",
           track_id, nrec, models.n, insts.n, verts.n, idx.n / 3, notes, path);
    rc = 0;

done:
    cxc_buf_free(&out);
    free(model_of_rec);
    free(idx.v);
    free(verts.v);
    free(insts.v);
    free(models.v);
    free(mats.v);
    cxc_blob_free(&st);
    cxc_blob_free(&sd);
    return rc;
}
