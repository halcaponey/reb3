/* cx_cars_lights.c -- port of tools/extract_traffic_lights.py.
 *
 * The corona light tables of the TRAFFIC vehicles (the .btv fleet), to
 * <out_root>/build/cars/<CLASS>_<CarN>.lights -- the SAME directory agent
 * E's carfx_art stage writes the 67 .bgv .lights into, which is the point:
 * src/burnout3_carfx.c reads both fleets with one parser, so the two halves
 * must land together.  (`out_root` is a REPO-ROOT stand-in.)
 *
 * WHY the .btv table is the same table [C], condensed from the python
 * docstring: FUN_001A4260 appends ".btv" and loads each traffic car through
 * the *same* .bgv relinker the player cars go through (docs/RE_BGD.md 4-6),
 * so a .btv IS a .bgv-format model file.  That predicts the corona table sits
 * at the two offsets the emitter FUN_001879E0 / FUN_00187AC0 reads for player
 * cars --
 *
 *     offsets  *(u32*)(model + 0x1664 + type*4)      12 types
 *     counts   *(u8 *)(model + 0x16AC + type)
 *     records  0x30 bytes = { float4 pos, float4 normal, float4 aux }   [C]
 *
 * -- and it does.  This module re-runs the python's four consistency checks
 * on every shipped .btv before writing anything:
 *
 *   1. offset == 0  <=>  count == 0                     (no dangling pointers)
 *   2. offset + count*0x30 <= filesize                  (every block in range)
 *   3. |pos| and |normal| finite and plausible          (< 100 m, normal != 0)
 *   4. headlights (type 0) sit forward of tail lights (type 1) in Z whenever a
 *      model carries both, and their normals point in opposite Z directions
 *
 * Over the 40 shipped .btv files, 40/40 pass 1-3 and every model carrying
 * both type 0 and type 1 passes 4.  A model that fails ANY check is reported
 * and NOT written, and the stage returns failure -- the python's SystemExit.
 *
 * The sidecar is byte-for-byte the format tools/extract_carfx_art.py writes
 * for the .bgv fleet, so src/burnout3_carfx.c reads both with one parser.
 * Names cannot collide: within a class the player cars are Car1..Car10
 * (+Car36) as .bgv and the traffic fleet is Car11.. as .btv.
 */
#include "cx_cars.h"
#include "cx_extract.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE_OFF 0x1664     /* u32[12] record offsets      [C] FUN_001879E0 */
#define TABLE_CNT 0x16AC     /* u8 [12] record counts       [C] FUN_00187AC0 */
#define STRIDE    0x30       /* { float4 pos, float4 nrm, float4 aux }       */

/* identical to tools/extract_carfx_art.py's -- same table, same dispatch */
static const char *const LIGHT_TYPES[12] = {
    "headlight", "tail", "brake", "unused3", "reverse",
    "indicator_r", "indicator_l", "unused7",
    "aux8", "aux9", "aux10", "aux11"
};

typedef struct {
    int    type;
    double p[3], n[3];
} cxd_light;

/* read_table(): -> count, or -1 with *err set to the python's own message. */
static int read_table(const cxd_blob *d, cxd_light *out, int cap,
                      const char **err)
{
    static char msg[160];
    int t, n = 0;

    *err = NULL;
    if (d->n < (size_t)TABLE_CNT + 12u) {
        *err = "too small for a light table";
        return -1;
    }
    for (t = 0; t < 12; t++) {
        int64_t off = (int64_t)cxd_u32(d, (size_t)TABLE_OFF + (size_t)t * 4u);
        int cnt = cxd_u8(d, (size_t)TABLE_CNT + (size_t)t);
        int k;
        if (cnt == 0) {
            if (off != 0) {
                snprintf(msg, sizeof msg, "type %d: count 0 but offset 0x%X",
                         t, (unsigned)off);
                *err = msg;
                return -1;
            }
            continue;
        }
        if (off == 0 || off + (int64_t)cnt * STRIDE > (int64_t)d->n) {
            snprintf(msg, sizeof msg,
                     "type %d: %d records at 0x%X out of range", t, cnt,
                     (unsigned)off);
            *err = msg;
            return -1;
        }
        for (k = 0; k < cnt; k++) {
            size_t b = (size_t)off + (size_t)k * STRIDE;
            double v[6], amax;
            int j;
            v[0] = cxd_f32(d, b);
            v[1] = cxd_f32(d, b + 4);
            v[2] = cxd_f32(d, b + 8);
            v[3] = cxd_f32(d, b + 0x10);
            v[4] = cxd_f32(d, b + 0x14);
            v[5] = cxd_f32(d, b + 0x18);
            for (j = 0; j < 6; j++) {
                if (v[j] != v[j] || fabs(v[j]) > 1e30) {
                    snprintf(msg, sizeof msg, "type %d rec %d: non-finite",
                             t, k);
                    *err = msg;
                    return -1;
                }
            }
            amax = fabs(v[0]);
            if (fabs(v[1]) > amax) amax = fabs(v[1]);
            if (fabs(v[2]) > amax) amax = fabs(v[2]);
            if (amax > 100.0) {
                snprintf(msg, sizeof msg,
                         "type %d rec %d: pos %.1f out of plausible range",
                         t, k, amax);
                *err = msg;
                return -1;
            }
            amax = fabs(v[3]);
            if (fabs(v[4]) > amax) amax = fabs(v[4]);
            if (fabs(v[5]) > amax) amax = fabs(v[5]);
            if (amax < 1e-6) {
                snprintf(msg, sizeof msg, "type %d rec %d: zero normal", t, k);
                *err = msg;
                return -1;
            }
            if (n < cap) {
                out[n].type = t;
                out[n].p[0] = v[0]; out[n].p[1] = v[1]; out[n].p[2] = v[2];
                out[n].n[0] = v[3]; out[n].n[1] = v[4]; out[n].n[2] = v[5];
                n++;
            }
        }
    }
    return n;
}

/* Check 4: heads in front of tails, normals opposed.  NULL = pass. */
static const char *geometry_check(const cxd_light *r, int n)
{
    static char msg[160];
    int i, nh = 0, nt = 0;
    double hz = 0.0, tz = 0.0, hn = 0.0, tn = 0.0;

    for (i = 0; i < n; i++) {
        if (r[i].type == 0) { hz += r[i].p[2]; hn += r[i].n[2]; nh++; }
        if (r[i].type == 1) { tz += r[i].p[2]; tn += r[i].n[2]; nt++; }
    }
    if (!nh || !nt)
        return NULL;
    hz /= nh; tz /= nt; hn /= nh; tn /= nt;
    if (hz <= tz) {
        snprintf(msg, sizeof msg,
                 "headlight mean z %.3f is not forward of tail mean z %.3f",
                 hz, tz);
        return msg;
    }
    if (hn * tn >= 0.0) {
        snprintf(msg, sizeof msg,
                 "headlight/tail normals not opposed (%.2f, %.2f)", hn, tn);
        return msg;
    }
    return NULL;
}

int cx_extract_traffic_lights(const char *game_dir, const char *out_root)
{
    char pveh[4096], outdir[4096];
    cxd_names classes;
    int ci, files = 0, nlights = 0, ngeom = 0, nbad = 0;
    char bad[64][256];

    cxd_path(pveh, sizeof pveh, game_dir, "/pveh", NULL);
    cxd_path(outdir, sizeof outdir, out_root, "/build/cars", NULL);
    if (cxd_list_dir(pveh, NULL, 1, &classes) != 0) {
        fprintf(stderr, "FAIL: %s missing (set B3_GAME_DIR)\n", pveh);
        return 1;
    }
    if (cxd_mkdir_p(outdir) != 0) {
        fprintf(stderr, "[cx_cars_lights] cannot create %s\n", outdir);
        cxd_names_free(&classes);
        return 1;
    }
    for (ci = 0; ci < classes.n; ci++) {
        char cdir[4096];
        cxd_names fl;
        int fi;
        cxd_path(cdir, sizeof cdir, pveh, "/", classes.v[ci], NULL);
        /* the python matches ".btv" case-SENSITIVELY here (endswith), and
         * every shipped file is lower case, so the distinction is moot */
        if (cxd_list_dir(cdir, ".btv", 0, &fl) != 0)
            continue;
        for (fi = 0; fi < fl.n; fi++) {
            char src[4096], base[64], p[4096];
            const char *dot = strrchr(fl.v[fi], '.');
            const char *err = NULL, *gerr;
            cxd_blob d = { NULL, 0 };
            cxd_light recs[512];
            int n, i, seen[12], t, first = 1, has0 = 0, has1 = 0;
            FILE *f;

            cxd_path(src, sizeof src, cdir, "/", fl.v[fi], NULL);
            if (cxd_read_file(src, &d) != 0)
                continue;
            n = read_table(&d, recs, 512, &err);
            if (n < 0) {
                if (nbad < 64)
                    snprintf(bad[nbad], sizeof bad[0], "%s/%s: %s",
                             classes.v[ci], fl.v[fi], err);
                nbad++;
                cxd_blob_free(&d);
                continue;
            }
            gerr = geometry_check(recs, n);
            if (gerr) {
                if (nbad < 64)
                    snprintf(bad[nbad], sizeof bad[0], "%s/%s: %s",
                             classes.v[ci], fl.v[fi], gerr);
                nbad++;
                cxd_blob_free(&d);
                continue;
            }
            for (i = 0; i < n; i++) {
                if (recs[i].type == 0) has0 = 1;
                if (recs[i].type == 1) has1 = 1;
            }
            if (has0 && has1)
                ngeom++;
            files++;
            nlights += n;

            memset(seen, 0, sizeof seen);
            for (i = 0; i < n; i++)
                seen[recs[i].type] = 1;
            printf("  %-5s %-10s %2d lights  types [", classes.v[ci],
                   fl.v[fi], n);
            for (t = 0; t < 12; t++)
                if (seen[t]) {
                    if (!first)
                        printf(", ");
                    printf("%d", t);
                    first = 0;
                }
            printf("]\n");

            snprintf(base, sizeof base, "%.*s",
                     dot ? (int)(dot - fl.v[fi]) : (int)strlen(fl.v[fi]),
                     fl.v[fi]);
            cxd_path(p, sizeof p, outdir, "/", classes.v[ci], "_", base,
                     ".lights", NULL);
            f = fopen(p, "wb");
            if (f) {
                fprintf(f, "# Traffic corona table, extracted by "
                        "tools/extract_traffic_lights.py from %s/%s.\n",
                        classes.v[ci], fl.v[fi]);
                fputs("# Same table as the player cars': records at\n"
                      "#   *(u32*)(model+0x1664+type*4), count *(u8*)"
                      "(model+0x16AC+type),\n"
                      "#   stride 0x30 = {float4 pos, float4 normal, "
                      "float4 aux} [C]\n"
                      "# No shadow rows: the traffic renderer draws no blob "
                      "shadow.\n", f);
                for (i = 0; i < n; i++)
                    fprintf(f, "light %d %s %.6f %.6f %.6f %.6f %.6f %.6f\n",
                            recs[i].type, LIGHT_TYPES[recs[i].type],
                            recs[i].p[0], recs[i].p[1], recs[i].p[2],
                            recs[i].n[0], recs[i].n[1], recs[i].n[2]);
                fclose(f);
            }
            cxd_blob_free(&d);
        }
        cxd_names_free(&fl);
    }
    cxd_names_free(&classes);
    printf("%d/%d .btv files carry a consistent light table, %d lights; "
           "%d also pass the front/rear geometry check\n",
           files, files + nbad, nlights, ngeom);
    if (nbad) {
        int i;
        for (i = 0; i < nbad && i < 64; i++)
            printf("  FAIL %s\n", bad[i]);
        return 1;
    }
    return 0;
}
