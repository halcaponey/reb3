/* cx_light_probes.c -- C11 port of tools/extract_light_probes.py (THE SPEC).
 *
 * Extract the per-position car light probes into <out_dir>/light_probes.bin.
 *
 * WHAT THE PROBES ARE  [C]
 * ------------------------
 * The nine SH coefficients the car body shader consumes (modelInstance+0x5C ->
 * vertex constants c0..c3, docs/RE_CARFX.md) are rewritten EVERY FRAME, PER
 * CAR, by FUN_0019D400.  That function
 *
 *   * takes the car's own world position (carObj+0x40) and casts a segment
 *     straight DOWN by 20.0 (`PUSH 0x41A00000` @0x001AB136)                [C]
 *   * hands it to FUN_0019D360 -> FUN_001AF980, i.e. the ORDINARY COLLISION
 *     BSP of the streamed unit (the same tree extract_collision.py parses) [C]
 *   * takes the prim record it hit (hit+0x60) plus the sub-triangle flag
 *     (hit+0x64) and reads THREE u16 probe indices out of the prim:
 *         sub-triangle 0 -> prim+0x06, prim+0x08, prim+0x0A
 *         sub-triangle 1 -> prim+0x0A, prim+0x08, prim+0x0C
 *     (0x0019D4D6..0x0019D525)                                             [C]
 *   * decodes each with FUN_0019C640(out float[9], in s8[9]) from
 *     `unit+0xA4 + idx*9` (`LEA EDX,[ESI+EAX*8]; ADD EDX,EAX` @0x0019D4DA ->
 *     stride 9)                                                            [C]
 *   * blends them with the hit's barycentrics (hit+0x54 = u, hit+0x58 = v):
 *         L[i] = p0[i] + (p1[i]-p0[i])*u + (p2[i]-p0[i])*v                 [C]
 *   * on a miss the call is skipped entirely, so the car KEEPS the previous
 *     frame's nine.                                                        [C]
 *
 * So the "probe volume" is not a grid: it is the collision mesh itself, with a
 * 9-byte quantised SH probe per collision vertex.  The prim record's u16[4] at
 * +0x06 (which extract_collision.py logs as "extra (unused)") are the four
 * per-corner probe indices, exactly parallel to its u8 corner indices at +0x00.
 *
 * CONTAINER  [C, byte-verified on every shipped track]
 * ----------------------------------------------------
 * Unit LOD block (static.dat unit table at +0x54/+0x58):
 *     +0xA0  u32 offset of the collision header (relinked by FUN_0019D7A0)
 *     +0xA4  u32 offset of the PROBE ARRAY      (relinked at 0x0019D7A8-BB)
 * The probe array is probe_count * 9 signed bytes; index space is per unit and
 * starts at 0.  On every shipped file it sits at block+0xB0, immediately after
 * the block header, and every decoded L00 byte is positive -- which is what an
 * irradiance L00 must be, and is the check that the offset is right.
 *
 * OUTPUT  <out_dir>/light_probes.bin (little-endian, raw GAME coordinates --
 * the loader in src/burnout3_carfx.c negates Z at query time the same way
 * trackmesh_load does for geometry):
 *     +0x00  'B3LP'
 *     +0x04  u32 version = 1
 *     +0x08  u32 probe_count      total, units concatenated in unit order
 *     +0x0C  u32 tri_count
 *     +0x10  f32 min[3]           probe-mesh bounds, game space
 *     +0x1C  f32 max[3]
 *     +0x28  probe_count * 9 s8   raw quantised probes (undecoded)
 *            pad to 4
 *            tri_count * { f32 v0[3], v1[3], v2[3]; u32 i0, i1, i2 }
 */
#include "cx_common_c.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FUN_0019C640's three band scales, byte-read from .rdata at the VAs below.
 * Not applied here -- the .bin carries the raw quantised bytes -- but kept
 * because they are what makes the L00 sanity check below meaningful.  [C] */
#define CXL_DEC_L0 (1.6 * 0.0078125)   /* 0x003B16F0 * 0x003B16F4 = 0.0125    */
#define CXL_DEC_L1 (0.6 * 0.0078125)   /* 0x003B16EC * 0x003B16F4 = 0.0046875 */
#define CXL_DEC_L2 (0.4 * 0.0078125)   /* 0x003B16E8 * 0x003B16F4 = 0.003125  */

#define CXL_PRIM_STRIDE 0x0E
#define CXL_TRI_REC     48             /* 9 f32 + 3 u32 */

typedef struct {
    float    v[9];                     /* v0[3] v1[3] v2[3] */
    uint32_t i[3];
} cxl_tri;

typedef struct {
    cxl_tri *v;
    size_t   n, cap;
} cxl_tris;

static int tris_push(cxl_tris *t, const double p0[3], const double p1[3],
                     const double p2[3], uint32_t a, uint32_t b, uint32_t c)
{
    cxl_tri *T;

    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 4096;
        cxl_tri *nv = (cxl_tri *)realloc(t->v, nc * sizeof(*nv));
        if (!nv)
            return -1;
        t->v = nv;
        t->cap = nc;
    }
    T = &t->v[t->n++];
    for (int k = 0; k < 3; k++) {
        T->v[k]     = (float)p0[k];
        T->v[3 + k] = (float)p1[k];
        T->v[6 + k] = (float)p2[k];
    }
    T->i[0] = a;
    T->i[1] = b;
    T->i[2] = c;
    return 0;
}

typedef struct {
    unsigned char *p;
    size_t         n, cap;
} cxl_bytes;

static int bytes_append(cxl_bytes *b, const uint8_t *src, size_t n)
{
    if (b->cap - b->n < n) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc - b->n < n)
            nc *= 2;
        unsigned char *nv = (unsigned char *)realloc(b->p, nc);
        if (!nv)
            return -1;
        b->p = nv;
        b->cap = nc;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
    return 0;
}

/* One leaf's quantised vertex: world = u16/65536*1000 + cell*500.  Kept in
 * double all the way to the pack, exactly as the python does -- the bounds are
 * taken from these values, not from their f32 roundings. */
static void leaf_vert(cxc_blob *blk, int64_t vert_base, uint32_t idx,
                      int cellx, int celly, int cellz, double out[3])
{
    uint16_t vx = cxc_u16(blk, vert_base + (int64_t)idx * 6);
    uint16_t vy = cxc_u16(blk, vert_base + (int64_t)idx * 6 + 2);
    uint16_t vz = cxc_u16(blk, vert_base + (int64_t)idx * 6 + 4);

    out[0] = (double)vx / 65536.0 * 1000.0 + (double)cellx * 500.0;
    out[1] = (double)vy / 65536.0 * 1000.0 + (double)celly * 500.0;
    out[2] = (double)vz / 65536.0 * 1000.0 + (double)cellz * 500.0;
}

/* (probes, tris) for one unit's LOD block.  tris index probes LOCALLY; the
 * caller rebases them onto the concatenated array.
 * Returns 1 on success, 0 when the unit carries no probes (python's None),
 * -1 on a structural violation (python's AssertionError). */
static int parse_unit(cxc_blob *blk, uint32_t unit, cxl_tris *tris,
                      size_t tri_base, const uint8_t **probes, size_t *nprobe)
{
    int64_t  coll, probe_off, leafs_off;
    uint32_t nleaf, flag;
    int64_t  maxidx = -1;
    size_t   n;

    *probes = NULL;
    *nprobe = 0;

    coll = (int64_t)cxc_u32(blk, 0xA0);
    probe_off = (int64_t)cxc_u32(blk, 0xA4);
    if (coll == 0 || probe_off == 0)
        return 0;

    /* nodes_off at coll+0x20 is read by the python but unused here. */
    leafs_off = (int64_t)cxc_u32(blk, coll + 0x24);
    nleaf = cxc_u16(blk, coll + 0x28);
    /* nnode at coll+0x2A likewise unused. */
    flag = cxc_u32(blk, coll + 0x2C);
    if (flag != 1) {
        fprintf(stderr, "cextract: unit %u: leaf format flag %u\n", unit, flag);
        return -1;
    }

    for (uint32_t i = 0; i < nleaf; i++) {
        int64_t rec = coll + leafs_off + (int64_t)i * 0x10;
        int64_t prim_base = rec + cxc_i32(blk, rec);
        int64_t vert_base = rec + cxc_i32(blk, rec + 4);
        int     cellx = cxc_i8(blk, rec + 0x0A);
        int     celly = cxc_i8(blk, rec + 0x0B);
        int     cellz = cxc_i8(blk, rec + 0x0C);
        uint8_t stride = cxc_u8(blk, rec + 0x0D);
        uint8_t pcount = cxc_u8(blk, rec + 0x0E);

        if (stride != CXL_PRIM_STRIDE) {
            fprintf(stderr, "cextract: unit %u leaf %u: prim stride %#x\n",
                    unit, i, stride);
            return -1;
        }
        for (uint32_t k = 0; k < pcount; k++) {
            int64_t  p = prim_base + (int64_t)k * stride;
            uint8_t  i0 = cxc_u8(blk, p), i1 = cxc_u8(blk, p + 1);
            uint8_t  i2 = cxc_u8(blk, p + 2), i3 = cxc_u8(blk, p + 3);
            uint16_t q0 = cxc_u16(blk, p + 6), q1 = cxc_u16(blk, p + 8);
            uint16_t q2 = cxc_u16(blk, p + 10), q3 = cxc_u16(blk, p + 12);
            double   v0[3], v1[3], v2[3], v3[3];

            if ((int64_t)q0 > maxidx) maxidx = q0;
            if ((int64_t)q1 > maxidx) maxidx = q1;
            if ((int64_t)q2 > maxidx) maxidx = q2;
            if (i3 != 0xFF && (int64_t)q3 > maxidx) maxidx = q3;

            leaf_vert(blk, vert_base, i0, cellx, celly, cellz, v0);
            leaf_vert(blk, vert_base, i1, cellx, celly, cellz, v1);
            leaf_vert(blk, vert_base, i2, cellx, celly, cellz, v2);
            if (tris_push(tris, v0, v1, v2,
                          (uint32_t)(tri_base + q0), (uint32_t)(tri_base + q1),
                          (uint32_t)(tri_base + q2)) != 0)
                return -1;
            if (i3 != 0xFF) {
                /* FUN_001B2940's quad split, and FUN_0019D400's matching index
                 * pick for the second sub-triangle.                       [C] */
                leaf_vert(blk, vert_base, i3, cellx, celly, cellz, v3);
                if (tris_push(tris, v2, v1, v3,
                              (uint32_t)(tri_base + q2),
                              (uint32_t)(tri_base + q1),
                              (uint32_t)(tri_base + q3)) != 0)
                    return -1;
            }
        }
    }

    n = (size_t)(maxidx + 1);
    if (probe_off < 0 || (uint64_t)probe_off > (uint64_t)blk->n ||
        blk->n - (size_t)probe_off < n * 9) {
        fprintf(stderr, "cextract: unit %u: probe array truncated\n", unit);
        return -1;
    }
    *probes = blk->d + probe_off;
    *nprobe = n;
    return 1;
}

int cx_extract_light_probes(const char *game_dir, const char *track_dir,
                            const char *track_id, const char *out_dir)
{
    cxc_blob  sd, st;
    cxl_tris  tris;
    cxl_bytes all_probes;
    cxc_buf   out;
    char      path[4096];
    double    gmin[3] = {1e30, 1e30, 1e30};
    double    gmax[3] = {-1e30, -1e30, -1e30};
    uint32_t  unit_count, units_with = 0;
    int64_t   unit_table;
    size_t    nprobe, pad;
    int       rc = -1, l00_lo = 0, l00_hi = 0, neg = 0;

    (void)game_dir;

    memset(&sd, 0, sizeof(sd));
    memset(&st, 0, sizeof(st));
    memset(&tris, 0, sizeof(tris));
    memset(&all_probes, 0, sizeof(all_probes));
    memset(&out, 0, sizeof(out));

    cxc_join(path, sizeof(path), track_dir, "static.dat");
    if (cxc_blob_load(&sd, path) != 0)
        return -1;
    cxc_join(path, sizeof(path), track_dir, "streamed.dat");
    if (cxc_blob_load(&st, path) != 0)
        goto done;

    unit_count = cxc_u16(&sd, 0x54);
    unit_table = cxc_i32(&sd, 0x58);

    for (uint32_t u = 0; u < unit_count; u++) {
        /* so, lo, ss, ls -- the static/LOD offset+size pair per unit. */
        int64_t  lo = cxc_i32(&sd, unit_table + (int64_t)u * 0x10 + 4);
        int64_t  ls = cxc_i32(&sd, unit_table + (int64_t)u * 0x10 + 12);
        cxc_blob blk;
        const uint8_t *probes;
        size_t   n, base;
        int      got;

        if (!lo || !ls)
            continue;
        blk = cxc_blob_slice(&st, lo, ls);
        base = all_probes.n / 9;
        got = parse_unit(&blk, u, &tris, base, &probes, &n);
        if (got < 0)
            goto done;
        if (got == 0)
            continue;
        if (cxc_oob(&blk)) {
            fprintf(stderr, "cextract: light_probes: unit %u read outside its "
                            "%zu-byte LOD block\n", u, blk.n);
            goto done;
        }
        if (bytes_append(&all_probes, probes, n * 9) != 0)
            goto done;
        units_with++;
    }

    /* Bounds come from the triangles, which is what the python walks; taken in
     * double before the f32 rounding.  (Rounding is monotonic, so either order
     * agrees -- but this is the one the spec does.) */
    for (size_t i = 0; i < tris.n; i++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                double v = (double)tris.v[i].v[c * 3 + k];
                if (v < gmin[k]) gmin[k] = v;
                if (v > gmax[k]) gmax[k] = v;
            }

    nprobe = all_probes.n / 9;

    cxc_put_bytes(&out, "B3LP", 4);
    cxc_put_u32(&out, 1u);
    cxc_put_u32(&out, (uint32_t)nprobe);
    cxc_put_u32(&out, (uint32_t)tris.n);
    for (int k = 0; k < 3; k++)
        cxc_put_f32(&out, (float)gmin[k]);
    for (int k = 0; k < 3; k++)
        cxc_put_f32(&out, (float)gmax[k]);
    cxc_put_bytes(&out, all_probes.p, all_probes.n);
    pad = ((size_t)0 - all_probes.n) & 3u;      /* python (-len) & 3 */
    cxc_put_zero(&out, pad);
    for (size_t i = 0; i < tris.n; i++) {
        for (int k = 0; k < 9; k++)
            cxc_put_f32(&out, tris.v[i].v[k]);
        for (int k = 0; k < 3; k++)
            cxc_put_u32(&out, tris.v[i].i[k]);
    }

    if (cxc_oob(&sd) || cxc_oob(&st)) {
        fprintf(stderr, "cextract: light_probes: out-of-range read -- the "
                        "python spec would have raised here\n");
        goto done;
    }

    cxc_join(path, sizeof(path), out_dir, "light_probes.bin");
    if (cxc_buf_write(&out, path) != 0)
        goto done;

    /* The offset check the python prints: an irradiance L00 must be positive. */
    for (size_t i = 0; i < nprobe; i++) {
        int v = (int)(int8_t)all_probes.p[i * 9];
        if (i == 0 || v < l00_lo) l00_lo = v;
        if (i == 0 || v > l00_hi) l00_hi = v;
        if (v < 0) neg++;
    }
    printf("[probes] %-9s units %2u/%2u  probes %6zu  tris %6zu  "
           "L00 %d..%d (neg %d)  -> %s\n",
           track_id, units_with, unit_count, nprobe, tris.n,
           l00_lo, l00_hi, neg, path);
    if (neg)
        printf("[probes]   WARNING: %d negative L00 bytes -- offset suspect\n",
               neg);
    rc = neg ? -1 : 0;      /* python's main() exits 1 when any track is bad */

done:
    cxc_buf_free(&out);
    free(all_probes.p);
    free(tris.v);
    cxc_blob_free(&st);
    cxc_blob_free(&sd);
    return rc;
}
