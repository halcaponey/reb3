/* cx_collision.c -- port of tools/extract_collision.py.
 *
 * Burnout 3's OWN collision world, out of the per-unit kd-tree soups that live
 * in streamed.dat's LOD blocks.  The python original is the spec; the [C]
 * citations below are condensed from its docstring.
 *
 * Where the collision lives [C]:
 *   static.dat's streamed-unit table (u16 count +0x54, i32 ptr +0x58) has
 *   0x10-byte entries {i32 blockA_off, i32 blockB_off, u32 blockA_size,
 *   u32 blockB_size} into streamed.dat.  The LOD (blockB) block is a
 *   self-contained record:
 *     +0x00 u32 state (1 on disk; the streamer stamps 2 when resident)
 *     +0x04 u32 unit index          +0x08 u32 block size
 *     +0x50 {u32 1, u32 offA, u32 0, u32 offB}   render far-LOD sections
 *     +0x70 f32[12]  four XZ half-planes bounding the unit (FUN_0019D7F0)
 *     +0xA0 u32 offset of the COLLISION HEADER   (relinked by FUN_0019D7A0)
 *     +0xA4 u32 offset of a second section       (relinked, not collision)
 *
 *   Collision header (relinked by FUN_001B02B0), offsets header-relative:
 *     +0x00 f32[3] bbox max        +0x10 f32[3] bbox min
 *     +0x20 u32 kd-node array offset (0x10-stride nodes)
 *     +0x24 u32 leaf array offset  +0x28 u16 leaf count  +0x2A u16 node count
 *     +0x2C u32 format flag (1 = quantized-vertex leaves on every retail unit)
 *
 *   Leaf record (0x10 bytes; FUN_001B02B0 relinks +0x00/+0x04 self-relative):
 *     +0x00 i32 prim data offset (relative to this record)
 *     +0x04 i32 vertex data offset (relative to this record)
 *     +0x08 u16 ?                  +0x0A s8[3] cell offset (x,y,z)
 *     +0x0D u8 prim stride (0x0E)  +0x0E u8 prim count  +0x0F u8 vertex count
 *
 *   Vertex: u16[3], stride 6.  world = u16/65536*1000 + cell*500 (FUN_001B0F00)
 *
 *   Prim record (stride 0x0E):
 *     +0x00 u8 i0,i1,i2,i3 (i3 == 0xFF -> triangle, else quad)
 *     +0x04 u16 surface type      +0x06 u16[4] extra (unused by the queries)
 *   Quad decomposition (FUN_001B2940, execution-verified): tri1 = (i0,i1,i2),
 *   tri2 = (i2,i1,i3).
 *
 *   The per-frame gather callback 0x00109CE0 SKIPS prims whose surface-type low
 *   byte is 0x20/0x22/0x23/0x24 -- those never reach the vehicle soup at
 *   veh+0x200 (they are FLAGGED here, not dropped).                        [C]
 *
 * Output <out_dir>/collision.bin (little-endian, raw GAME coordinates):
 *   +0x00 magic 'B3CL'  +0x04 u32 version=1  +0x08 u32 tri_count
 *   +0x0C u32 unit_count  +0x10 f32 min[3]  +0x1C f32 max[3]
 *   +0x28 tri[tri_count]: {f32 v0[3],v1[3],v2[3]; u16 type; u8 unit; u8 flags}
 *         flags bit0 = excluded from the vehicle soup by the gather callback
 */
#include "cx_common_b.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Surface-type low bytes the gather callback 0x00109CE0 refuses to append. */
static int excluded_low(unsigned styp)
{
    unsigned lo = styp & 0xFFu;
    return lo == 0x20u || lo == 0x22u || lo == 0x23u || lo == 0x24u;
}

typedef struct {
    double v[9];
    uint16_t type;
    uint8_t unit;
    uint8_t flags;
} CTri;

typedef struct {
    CTri *t;
    size_t n, cap;
} CTriVec;

static int tv_push(CTriVec *v, const CTri *t)
{
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 4096;
        CTri *p = (CTri *)realloc(v->t, nc * sizeof *p);
        if (!p)
            return -1;
        v->t = p;
        v->cap = nc;
    }
    v->t[v->n++] = *t;
    return 0;
}

/* world = u16/65536*1000 + cell*500 -- computed in double, exactly as the
 * python does, and narrowed to f32 only when it is packed. */
static int leaf_vertex(const cxb_blob *blk, int64_t vert_base, int idx,
                       int cx, int cy, int cz, double out[3])
{
    int64_t o = vert_base + (int64_t)idx * 6;

    /* python's struct.unpack_from raises here; the bounds-checked readers
     * would quietly yield zeros, so fail loudly instead. */
    if (o < 0 || (uint64_t)o + 6u > (uint64_t)blk->n)
        return -1;
    out[0] = (double)cxb_u16(blk, (size_t)o) / 65536.0 * 1000.0
           + (double)cx * 500.0;
    out[1] = (double)cxb_u16(blk, (size_t)o + 2) / 65536.0 * 1000.0
           + (double)cy * 500.0;
    out[2] = (double)cxb_u16(blk, (size_t)o + 4) / 65536.0 * 1000.0
           + (double)cz * 500.0;
    return 0;
}

/* Triangles from one unit's LOD block.  Returns 0 = ok, 1 = no collision
 * header (python's `return None`), -1 = hard error. */
static int parse_unit_collision(const cxb_blob *blk, unsigned unit,
                                CTriVec *out, const char *track_id)
{
    uint32_t hdr, nodes_off, leafs_off, flag;
    unsigned nleaf;
    unsigned i;

    (void)track_id;
    hdr = cxb_u32(blk, 0xA0);
    if (hdr == 0)
        return 1;
    if ((uint64_t)hdr + 0x30u > (uint64_t)blk->n) {
        fprintf(stderr, "unit %u: collision header past end of block\n", unit);
        return -1;
    }
    nodes_off = cxb_u32(blk, hdr + 0x20);
    leafs_off = cxb_u32(blk, hdr + 0x24);
    nleaf = cxb_u16(blk, hdr + 0x28);
    flag = cxb_u32(blk, hdr + 0x2C);
    (void)nodes_off;
    if (flag != 1u) {
        fprintf(stderr, "unit %u: unexpected leaf format flag %u\n", unit, flag);
        return -1;
    }
    for (i = 0; i < nleaf; i++) {
        int64_t rec = (int64_t)hdr + (int64_t)leafs_off + (int64_t)i * 0x10;
        int64_t prim_base, vert_base;
        int cellx, celly, cellz;
        unsigned stride, pcount, k;

        if (rec < 0 || (uint64_t)rec + 0x10u > (uint64_t)blk->n) {
            fprintf(stderr, "unit %u leaf %u: record past end of block\n",
                    unit, i);
            return -1;
        }
        prim_base = rec + (int64_t)cxb_i32(blk, (size_t)rec);
        vert_base = rec + (int64_t)cxb_i32(blk, (size_t)rec + 4);
        cellx = cxb_i8(blk, (size_t)rec + 0xA);
        celly = cxb_i8(blk, (size_t)rec + 0xB);
        cellz = cxb_i8(blk, (size_t)rec + 0xC);
        stride = cxb_u8(blk, (size_t)rec + 0xD);
        pcount = cxb_u8(blk, (size_t)rec + 0xE);
        if (stride != 0x0Eu) {
            fprintf(stderr, "unit %u leaf %u: prim stride %#x\n",
                    unit, i, stride);
            return -1;
        }
        for (k = 0; k < pcount; k++) {
            int64_t p = prim_base + (int64_t)k * (int64_t)stride;
            int i0, i1, i2, i3;
            unsigned styp;
            uint8_t fl;
            CTri t;
            double v0[3], v1[3], v2[3], v3[3];

            if (p < 0 || (uint64_t)p + stride > (uint64_t)blk->n) {
                fprintf(stderr, "unit %u leaf %u: prim past end of block\n",
                        unit, i);
                return -1;
            }
            i0 = cxb_u8(blk, (size_t)p);
            i1 = cxb_u8(blk, (size_t)p + 1);
            i2 = cxb_u8(blk, (size_t)p + 2);
            i3 = cxb_u8(blk, (size_t)p + 3);
            styp = cxb_u16(blk, (size_t)p + 4);
            fl = excluded_low(styp) ? 1u : 0u;
            if (leaf_vertex(blk, vert_base, i0, cellx, celly, cellz, v0)
                || leaf_vertex(blk, vert_base, i1, cellx, celly, cellz, v1)
                || leaf_vertex(blk, vert_base, i2, cellx, celly, cellz, v2)) {
                fprintf(stderr, "unit %u leaf %u: vertex past end of block\n",
                        unit, i);
                return -1;
            }
            memcpy(t.v, v0, sizeof v0);
            memcpy(t.v + 3, v1, sizeof v1);
            memcpy(t.v + 6, v2, sizeof v2);
            t.type = (uint16_t)styp;
            t.unit = (uint8_t)unit;
            t.flags = fl;
            if (tv_push(out, &t) != 0)
                return -1;
            if (i3 != 0xFF) {
                if (leaf_vertex(blk, vert_base, i3, cellx, celly, cellz, v3)) {
                    fprintf(stderr,
                            "unit %u leaf %u: quad vertex past end of block\n",
                            unit, i);
                    return -1;
                }
                memcpy(t.v, v2, sizeof v2);
                memcpy(t.v + 3, v1, sizeof v1);
                memcpy(t.v + 6, v3, sizeof v3);
                if (tv_push(out, &t) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

static void put_u32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put_f32le(unsigned char *p, double d)
{
    float f = (float)d;
    uint32_t u;
    memcpy(&u, &f, 4);
    put_u32le(p, u);
}

int cx_extract_collision(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir)
{
    char path[4096];
    cxb_blob sd = { NULL, 0 }, st = { NULL, 0 };
    CTriVec tris = { NULL, 0, 0 };
    double gmin[3], gmax[3];
    unsigned unit_count, u;
    int64_t unit_table;
    FILE *f = NULL;
    unsigned char *rec = NULL;
    size_t i;
    int rc = 1, n_units = 0;

    (void)game_dir;

    snprintf(path, sizeof path, "%s/static.dat", track_dir);
    if (cxb_read_file(path, &sd) != 0) {
        fprintf(stderr, "[cx_collision] cannot read %s\n", path);
        goto done;
    }
    snprintf(path, sizeof path, "%s/streamed.dat", track_dir);
    if (cxb_read_file(path, &st) != 0) {
        fprintf(stderr, "[cx_collision] cannot read %s\n", path);
        goto done;
    }

    unit_count = cxb_u16(&sd, 0x54);
    unit_table = cxb_i32(&sd, 0x58);

    for (i = 0; i < 3; i++) {
        gmin[i] = 1e30;
        gmax[i] = -1e30;
    }

    for (u = 0; u < unit_count; u++) {
        size_t e = (size_t)unit_table + (size_t)u * 0x10u;
        int32_t lo = cxb_i32(&sd, e + 4);      /* blockB (LOD) offset */
        int32_t ls = cxb_i32(&sd, e + 12);     /* blockB size */
        cxb_blob blk;
        size_t before;
        int r;

        if (!lo || !ls)
            continue;
        if (lo < 0 || ls < 0 || (uint64_t)lo + (uint64_t)ls > (uint64_t)st.n) {
            fprintf(stderr, "[cx_collision] unit %u: LOD block out of range\n",
                    u);
            goto done;
        }
        blk.d = st.d + (size_t)lo;
        blk.n = (size_t)ls;
        before = tris.n;
        r = parse_unit_collision(&blk, u, &tris, track_id);
        if (r < 0)
            goto done;
        if (r > 0)
            continue;
        if (tris.n > before)     /* python counts a unit only when it has tris */
            n_units++;
    }

    for (i = 0; i < tris.n; i++) {
        int c;
        for (c = 0; c < 9; c++) {
            int ax = c % 3;
            double v = tris.t[i].v[c];
            if (v < gmin[ax]) gmin[ax] = v;
            if (v > gmax[ax]) gmax[ax] = v;
        }
    }

    if (cxb_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "[cx_collision] cannot create %s\n", out_dir);
        goto done;
    }
    snprintf(path, sizeof path, "%s/collision.bin", out_dir);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[cx_collision] cannot write %s\n", path);
        goto done;
    }
    {
        unsigned char hdr[0x28];
        memcpy(hdr, "B3CL", 4);
        put_u32le(hdr + 4, 1u);
        put_u32le(hdr + 8, (uint32_t)tris.n);
        put_u32le(hdr + 12, unit_count);
        for (i = 0; i < 3; i++) {
            put_f32le(hdr + 0x10 + i * 4, gmin[i]);
            put_f32le(hdr + 0x1C + i * 4, gmax[i]);
        }
        if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr)
            goto done;
    }
    rec = (unsigned char *)malloc(40u * (tris.n ? tris.n : 1u));
    if (!rec)
        goto done;
    for (i = 0; i < tris.n; i++) {
        unsigned char *p = rec + i * 40u;
        int c;
        for (c = 0; c < 9; c++)
            put_f32le(p + c * 4, tris.t[i].v[c]);
        p[36] = (unsigned char)tris.t[i].type;
        p[37] = (unsigned char)(tris.t[i].type >> 8);
        p[38] = tris.t[i].unit;
        p[39] = tris.t[i].flags;
    }
    if (tris.n && fwrite(rec, 40u, tris.n, f) != tris.n)
        goto done;
    if (fclose(f) != 0) {
        f = NULL;
        goto done;
    }
    f = NULL;

    printf("[cx_collision] track %s (%s)\n", track_id, track_dir);
    printf("[cx_collision] units with collision: %d/%u\n", n_units, unit_count);
    printf("[cx_collision] triangles: %zu\n", tris.n);
    printf("[cx_collision] bounds  min (%.1f, %.1f, %.1f)  max (%.1f, %.1f, %.1f)\n",
           gmin[0], gmin[1], gmin[2], gmax[0], gmax[1], gmax[2]);
    printf("[cx_collision] wrote %s/collision.bin\n", out_dir);
    rc = 0;

done:
    if (f)
        fclose(f);
    free(rec);
    free(tris.t);
    cxb_blob_free(&sd);
    cxb_blob_free(&st);
    return rc;
}
