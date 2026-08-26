/* cx_traffic.c -- <out_dir>/traffic.bin ('B3TR' v4).
 *
 * Port of the TRACK side of tools/extract_traffic.py (the spec).  The
 * vehicle-asset half of that tool (build/cars OBJ + PNG) is NOT here.
 *
 * Where the data comes from (the .bgd walk lives in cx_common_a.c):
 *   * MODE BLOCK ("TDESC"): the event's own {size = param+0x3C4,
 *     offset = param+0x3C8} (`mov eax,[eax+0x3c8]` @0x0018B569).       [C]
 *   * TRAFFIC SET: six independent {ptr,count} lists at TDESC +0x54/+0x60/
 *     +0x6C/+0x78/+0x84/+0x90 -- exactly the pointers the block relocator
 *     FUN_00158B70 fixes up -- of 0x18-byte records with a base-40 packed
 *     vehicle id at +0x00.  The six lists are six vehicle CATEGORY pools:
 *     0 compact, 1 light, 2 bus, 3 truck, 4 TRACTOR, 5 TRAILER; the
 *     tractor<->trailer pairing is the LIST SLOT, not a record field.  [C]
 *   * SPECIAL TRAFFIC: five inline u64 ids at TDESC+0x00.., gated bit by bit
 *     by the flag byte at TDESC+0xB5.                                  [C]
 *   * SPAWN/ENTRY TABLE: {ptr = TDESC+0xAC, count = TDESC+0xB0}, stride
 *     0x20, {pos[3], w, dir[3], w}.  Location [C], "spawn point" reading [S].
 *   * ONCOMING LINE: the drive line that hugs the road corridor, chosen and
 *     oriented by the analysis in cx_common_a.c.                        [S]
 *   * HITCH GEOMETRY: the trailer is the articulated master -- when its
 *     model+0x16BC count is one it supplies its kingpin from +0x16A4; the
 *     tractor partner supplies its fifth-wheel anchor from +0x16A8 (count
 *     +0x16BD).  Raw mesh-space coordinates.                            [C]
 *
 * Z IS NEGATED on every emitted point/direction (GL space, RE_NOTES 12).
 */
#include "cx_extract.h"
#include "cx_common_a.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CX_CAT_TRACTOR = 4, CX_CAT_TRAILER = 5, CX_CAT_SPECIAL = 6 };

typedef struct {
    char id[16];
    char cls[8];
    char car[20];
    int  cat;
} cx_car;

/* ------------------------------------------------------- hitch geometry [C] */

static int hitch_read(const char *game_dir, const char *cls, const char *car,
                      unsigned char **out, size_t *out_len)
{
    char rel[64];
    char *path;
    size_t n = 0;
    snprintf(rel, sizeof rel, "pveh/%s/%s.btv", cls, car);
    path = cxa_join(game_dir, rel);
    if (!path)
        return -1;
    *out = cxa_slurp(path, &n);
    free(path);
    *out_len = n;
    return *out ? 0 : -1;
}

/* first relinked model attach point at {ptr_off, count_off}; 0 = ok */
static int hitch_anchor(const unsigned char *d, size_t len,
                        size_t ptr_off, size_t count_off, cxa_v3 *out)
{
    uint32_t ptr;
    if (count_off >= len || d[count_off] == 0)
        return -1;
    ptr = cxa_u32(d, ptr_off);
    if (ptr == 0 || (size_t)ptr + 16 > len)
        return -1;
    *out = cxa_f3(d, ptr);
    return 0;
}

static int hitch_geometry(const char *game_dir, const char *cls,
                          const char *car, int cat,
                          cxa_v3 *tow, cxa_v3 *king, int32_t *spring)
{
    unsigned char *d = NULL;
    size_t len = 0;
    int rc = 0;

    tow->x = tow->y = tow->z = 0.0;
    king->x = king->y = king->z = 0.0;
    *spring = 0;
    if (cat != CX_CAT_TRACTOR && cat != CX_CAT_TRAILER)
        return 0;
    if (hitch_read(game_dir, cls, car, &d, &len)) {
        /* python's open() failure path returns None -> ValueError */
        return -1;
    }
    if (cat == CX_CAT_TRACTOR) {
        if (hitch_anchor(d, len, 0x16A8, 0x16BD, tow))
            rc = -1;                    /* no tractor fifth-wheel anchor */
    } else {
        unsigned count = (0x16BC < len) ? d[0x16BC] : 0;
        size_t ptr_off   = (count == 1) ? 0x16A4 : 0x16A8;
        size_t count_off = (count == 1) ? 0x16BC : 0x16BD;
        if (hitch_anchor(d, len, ptr_off, count_off, king))
            rc = -1;                    /* no selected trailer anchor */
        *spring = (count == 1) ? 1 : 0;
    }
    free(d);
    return rc;
}

/* ------------------------------------------------------------------ lanes
 * Traffic LANES recovered from the spawn/entry table [S]: each spawn record
 * is projected onto the traffic route polyline, the signed lateral offset
 * says which lane the entry sits in and the record's own `dir` says which
 * way that lane runs.  The polyline itself sits at lateral 0, so it is the
 * road EDGE / median line and NOT a driving lane.
 *
 * The spawn record's dir[3] is the instance matrix's Z COLUMN = the car's
 * BACKWARD axis [C, AI-DRIVE wave], hence the sign flip below.
 */
typedef struct { double lat; int dir; } cx_lane;

static void lane_proj(const cxa_v3 *line, int n, double x, double z,
                      double *out_lat, double *out_tx, double *out_tz)
{
    double best = 1e30, bcx = 0, bcz = 0, btx = 0, btz = 0, L, s;
    int i;
    for (i = 0; i < n; i++) {
        cxa_v3 p = line[i], q = line[(i + 1) % n];
        double ax = q.x - p.x, az = q.z - p.z;
        double l2 = ax * ax + az * az, t, cx, cz, d2;
        t = (l2 > 1e-9) ? ((x - p.x) * ax + (z - p.z) * az) / l2 : 0.0;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        cx = p.x + ax * t;
        cz = p.z + az * t;
        d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d2 < best) {
            best = d2;
            bcx = cx; bcz = cz; btx = ax; btz = az;
        }
    }
    L = hypot(btx, btz);
    if (L == 0.0)
        L = 1.0;
    btx /= L;
    btz /= L;
    s = btx * (z - bcz) - btz * (x - bcx);
    *out_lat = (s >= 0) ? sqrt(best) : -sqrt(best);
    *out_tx = btx;
    *out_tz = btz;
}

static int cmp_hit(const void *pa, const void *pb)
{
    const cx_lane *a = (const cx_lane *)pa, *b = (const cx_lane *)pb;
    if (a->lat != b->lat) return a->lat < b->lat ? -1 : 1;
    return a->dir < b->dir ? -1 : (a->dir > b->dir ? 1 : 0);
}

static int lane_table(const double *spawns, int nspawns,
                      const cxa_v3 *line, int n, cx_lane **out)
{
    cx_lane *hits, *lanes;
    int nhits = 0, nlanes = 0, i, nout = 0;
    double *acc;
    int *accn;

    *out = NULL;
    if (n < 8 || !nspawns)
        return 0;
    hits = (cx_lane *)calloc((size_t)nspawns, sizeof(cx_lane));
    lanes = (cx_lane *)calloc((size_t)nspawns, sizeof(cx_lane));
    acc = (double *)calloc((size_t)nspawns, sizeof(double));
    accn = (int *)calloc((size_t)nspawns, sizeof(int));
    if (!hits || !lanes || !acc || !accn) {
        free(hits); free(lanes); free(acc); free(accn);
        return 0;
    }
    for (i = 0; i < nspawns; i++) {
        const double *s = spawns + i * 6;
        double lat, tx, tz, dot;
        lane_proj(line, n, s[0], s[2], &lat, &tx, &tz);
        dot = s[3] * tx + s[5] * tz;
        if (fabs(dot) < 0.85)           /* slip roads / junction mouths */
            continue;
        hits[nhits].lat = lat;
        hits[nhits].dir = (dot > 0) ? -1 : 1;
        nhits++;
    }
    qsort(hits, (size_t)nhits, sizeof(cx_lane), cmp_hit);
    /* cluster on lateral offset (lanes ~6 m apart, entries scatter ~1 m) */
    for (i = 0; i < nhits; i++) {
        if (nlanes && fabs(lanes[nlanes - 1].lat - hits[i].lat) < 3.0
            && lanes[nlanes - 1].dir == hits[i].dir) {
            acc[nlanes - 1] += hits[i].lat;
            accn[nlanes - 1]++;
        } else {
            lanes[nlanes].lat = hits[i].lat;   /* cluster head, the compare key */
            lanes[nlanes].dir = hits[i].dir;
            acc[nlanes] = hits[i].lat;
            accn[nlanes] = 1;
            nlanes++;
        }
    }
    for (i = 0; i < nlanes; i++) {
        if (accn[i] < 2)
            continue;
        lanes[nout].lat = acc[i] / accn[i];
        lanes[nout].dir = lanes[i].dir;
        nout++;
    }
    free(hits); free(acc); free(accn);
    *out = lanes;
    return nout;
}

/* --------------------------------------------------------------- the stage */

int cx_extract_traffic(const char *game_dir, const char *track_dir,
                       const char *track_id, const char *out_dir)
{
    cxa_analysis a;
    cxa_tdesc td;
    char *bgd_path, *out = NULL;
    const char *event = getenv("B3_EVENT");
    cx_car *cars = NULL;
    int ncars = 0, nspecial = 0, i, k, nlanes = 0, nline = 0;
    double *spawns = NULL;
    cxa_v3 *line = NULL;
    cx_lane *lanes = NULL;
    FILE *f;
    int rc = -1;

    (void)track_id;
    bgd_path = cxa_join(track_dir, "Gamedata.bgd");
    if (!bgd_path)
        return -1;
    if (cxa_analyse(&a, bgd_path, event && *event ? event : CXA_DEFAULT_EVENT,
                    out_dir)) {
        free(bgd_path);
        cxa_analysis_free(&a);
        return -1;
    }
    free(bgd_path);
    if (cxa_read_tdesc(a.bgd, a.ev, &td)) {
        cxa_analysis_free(&a);
        return -1;
    }

    /* the traffic set: the six lists in slot order, then the gated specials,
     * de-duplicated, keeping only <CLS>CAR<n> ids */
    {
        size_t cap = 5;
        for (i = 0; i < 6; i++)
            cap += td.lists[i].nrecords;
        cars = (cx_car *)calloc(cap, sizeof(cx_car));
    }
    if (!cars)
        goto done;
    for (i = 0; i < 6; i++) {
        for (k = 0; k < (int)td.lists[i].nrecords; k++) {
            const char *cid = td.lists[i].records[k].id;
            int dup = 0, j;
            if (strncmp(cid + 4, "CAR", 3))     /* cid[4:7] == 'CAR' */
                continue;
            for (j = 0; j < ncars; j++)
                if (!strcmp(cars[j].id, cid))
                    dup = 1;
            if (dup)
                continue;
            snprintf(cars[ncars].id, sizeof cars[ncars].id, "%.15s", cid);
            snprintf(cars[ncars].cls, sizeof cars[ncars].cls, "%.4s", cid);
            snprintf(cars[ncars].car, sizeof cars[ncars].car, "Car%.12s", cid + 7);
            cars[ncars].cat = i;
            ncars++;
        }
    }
    for (i = 0; i < td.nspecials; i++) {
        const char *cid = td.specials[i];
        int dup = 0, j;
        if (strlen(cid) < 7 || strncmp(cid + 4, "CAR", 3))
            continue;
        for (j = 0; j < ncars; j++)
            if (!strcmp(cars[j].id, cid))
                dup = 1;
        if (dup)
            continue;
        snprintf(cars[ncars].id, sizeof cars[ncars].id, "%.15s", cid);
        snprintf(cars[ncars].cls, sizeof cars[ncars].cls, "%.4s", cid);
        snprintf(cars[ncars].car, sizeof cars[ncars].car, "Car%.12s", cid + 7);
        cars[ncars].cat = CX_CAT_SPECIAL;
        ncars++;
        nspecial++;
    }
    if (!ncars)                         /* no traffic ids in the mode block */
        goto done;

    if (td.nspawns) {
        spawns = (double *)malloc((size_t)td.nspawns * 6 * sizeof(double));
        if (!spawns)
            goto done;
        for (i = 0; i < (int)td.nspawns; i++) {
            spawns[i * 6 + 0] =  td.spawns[i * 6 + 0];
            spawns[i * 6 + 1] =  td.spawns[i * 6 + 1];
            spawns[i * 6 + 2] = -td.spawns[i * 6 + 2];
            spawns[i * 6 + 3] =  td.spawns[i * 6 + 3];
            spawns[i * 6 + 4] =  td.spawns[i * 6 + 4];
            spawns[i * 6 + 5] = -td.spawns[i * 6 + 5];
        }
    }
    if (a.oncoming) {
        nline = a.oncoming->count;
        line = (cxa_v3 *)malloc((size_t)nline * sizeof(cxa_v3));
        if (!line)
            goto done;
        for (i = 0; i < nline; i++) {
            line[i].x =  a.oncoming->pts[i].x;
            line[i].y =  a.oncoming->pts[i].y;
            line[i].z = -a.oncoming->pts[i].z;
        }
    }
    nlanes = lane_table(spawns, (int)td.nspawns, line, nline, &lanes);

    out = cxa_join(out_dir, "traffic.bin");
    if (!out)
        goto done;
    f = fopen(out, "wb");
    if (!f)
        goto done;
    cxa_w_bytes(f, "B3TR", 4);
    cxa_w_u32(f, 4);
    cxa_w_u32(f, (uint32_t)ncars);
    cxa_w_u32(f, td.nspawns);
    cxa_w_u32(f, (uint32_t)nline);
    cxa_w_u32(f, (uint32_t)nspecial);
    cxa_w_u32(f, (uint32_t)nlanes);
    for (i = 0; i < ncars; i++) {
        cxa_v3 tow, king;
        int32_t spring;
        if (hitch_geometry(game_dir, cars[i].cls, cars[i].car, cars[i].cat,
                           &tow, &king, &spring)) {
            fclose(f);
            goto done;
        }
        cxa_w_pad(f, cars[i].id, 16);
        cxa_w_pad(f, cars[i].cls, 8);
        cxa_w_pad(f, cars[i].car, 16);
        cxa_w_i32(f, cars[i].cat);
        cxa_w_i32(f, spring);
        cxa_w_f32(f, tow.x);  cxa_w_f32(f, tow.y);  cxa_w_f32(f, tow.z);
        cxa_w_f32(f, king.x); cxa_w_f32(f, king.y); cxa_w_f32(f, king.z);
    }
    for (i = 0; i < (int)td.nspawns; i++)
        for (k = 0; k < 6; k++)
            cxa_w_f32(f, spawns[i * 6 + k]);
    for (i = 0; i < nline; i++) {
        cxa_w_f32(f, line[i].x);
        cxa_w_f32(f, line[i].y);
        cxa_w_f32(f, line[i].z);        /* already GL space */
    }
    for (i = 0; i < nlanes; i++) {
        cxa_w_f32(f, lanes[i].lat);
        cxa_w_i32(f, lanes[i].dir);
    }
    fclose(f);
    rc = 0;

done:
    free(out);
    free(cars);
    free(spawns);
    free(line);
    free(lanes);
    cxa_tdesc_free(&td);
    cxa_analysis_free(&a);
    return rc;
}
