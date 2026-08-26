/* cx_cars_traffic.c -- port of the VEHICLE-ASSET half of
 * tools/extract_traffic.py (extract_btv() + export_assets()).
 *
 * This is the last cars/ gap that kept tools/extract_traffic.py alive after
 * agent A ported its TRACK half: the per-track .btv mesh + paint export,
 * written to <out_dir>/cars/ AND mirrored into the dump-global build/cars/
 * (the path the harness's load_car_meshes expects).
 *
 * `out_dir` is the ordinary per-track artefact directory, i.e.
 * <repo>/build/tracks/<ID> under the project-wide repo-root convention, so
 * the two levels up that the mirror is derived from land exactly on
 * <repo>/build/cars -- the same directory cx_extract_car_meshes() and
 * cx_extract_traffic_lights() write into.
 *
 * Per traffic vehicle, in BOTH directories:
 *   <CLS>_<CarN>.obj         the intact record set, glass dropped
 *   <CLS>_<CarN>_wheel.obj   wheel slot 7 (slow), 8/9 as fallback
 *   <CLS>_<CarN>.wheels      radius + attach positions
 *   <CLS>_<CarN>_p<K>.png    every paint variant
 *
 * WHY a .btv reads as a .bgv [C]: FUN_001A4260 appends ".btv" and loads each
 * traffic car through the SAME relinker FUN_000310F0 the player cars go
 * through, so the section choice, the 0x18-byte vertex layout, the wheel part
 * slots (7 slow / 8,9 blur, FUN_000303D0) and the attach matrices at +0xB80 /
 * radius at +0x18 are all the records tools/extract_bgv.py reads.
 *
 * ROSTER: the python gets the car list from the event mode block it has just
 * decoded.  That decode is agent A's cx_extract_traffic(), whose artefact
 * <out_dir>/traffic.bin ('B3TR' v4) carries the same list in the same order,
 * so this stage reads it back -- the pattern cx_extract_nav_edges() already
 * uses for route.bin.  Run it AFTER the TRAFFIC stage.
 *
 * ============================================================ PYTHON QUIRKS
 *  Q13 the .btv OBJ is written through write_obj()'s LEGACY 2-tuple group
 *      path, so it carries NO `usemtl` line at all -- unlike every .bgv OBJ.
 *  Q14 the group names differ from extract_bgv.py's: `body_m<M>` for the
 *      intact set, but `m<M>` when the intact filter came up empty and the
 *      exporter fell back to every record.
 *  Q15 the length gate is widened from 20 m to 60 m for the long TSPC
 *      specials, and nothing else about the section choice changes.
 *  Q16 no .panels, no parts/, no _intact/_shell/_glass, and the .wheels
 *      sidecar carries a DIFFERENT header and omits the ext/center rows.
 *  Q17 a mesh failure does not skip the paint export: the two halves are
 *      independent, and the per-car counters are independent too.
 */
#include "cx_cars.h"
#include "cx_extract.h"
#include "cx_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* traffic.bin ('B3TR' v4): 'B3TR', u32 version, car/spawn/line counts, the
 * special count and the lane count; then one 0x48-byte record per car
 * carrying id[16], class[8], file stem[16] and the hitch geometry. */
#define TR_HDR   28
#define TR_CAR   72

typedef struct {
    char cls[16];
    char car[24];
} cxd_tcar;

static int read_roster(const char *out_dir, cxd_tcar **out, int *n_out)
{
    char p[4096];
    cxd_blob d = { NULL, 0 };
    uint32_t ncars;
    cxd_tcar *v;
    uint32_t i;

    cxd_path(p, sizeof p, out_dir, "/traffic.bin", NULL);
    if (cxd_read_file(p, &d) != 0)
        return -1;
    if (d.n < TR_HDR || memcmp(d.d, "B3TR", 4) != 0) {
        cxd_blob_free(&d);
        return -1;
    }
    ncars = cxd_u32(&d, 8);
    if (d.n < (size_t)TR_HDR + (size_t)ncars * TR_CAR) {
        cxd_blob_free(&d);
        return -1;
    }
    v = (cxd_tcar *)calloc(ncars ? ncars : 1, sizeof *v);
    if (!v) { cxd_blob_free(&d); return -1; }
    for (i = 0; i < ncars; i++) {
        size_t o = (size_t)TR_HDR + (size_t)i * TR_CAR;
        memcpy(v[i].cls, d.d + o + 16, 8);
        v[i].cls[8] = 0;
        memcpy(v[i].car, d.d + o + 24, 16);
        v[i].car[16] = 0;
    }
    cxd_blob_free(&d);
    *out = v;
    *n_out = (int)ncars;
    return 0;
}

/* extract_traffic.py extract_btv(): extract_bgv.extract()'s section choice
 * and vertex layout with the player-car length gate widened to `max_len`. */
static int export_one(const char *src, const char *name,
                      char dirs[2][4096], int ndirs,
                      int *got_mesh, int *got_tex)
{
    cxd_blob d = { NULL, 0 };
    cxd_section sec;
    cxd_vert *verts = NULL;
    cxd_group *groups = NULL;
    unsigned char **imgs = NULL;
    const char *err = NULL;
    char p[4096], texname[128];
    uint32_t ver;
    int nv = 0, i, dd, ng = 0, nimg = 0, w = 0, h = 0;
    double zmin, zmax;

    memset(&sec, 0, sizeof sec);
    if (cxd_read_file(src, &d) != 0) {
        printf("  mesh FAIL %-12s cannot read\n", name);
        printf("  tex  FAIL %-12s cannot read\n", name);
        return -1;
    }

    ver = d.n >= 4 ? cxd_u32(&d, 0) : 0;
    if (!(ver >= 0x14 && ver <= 0x25)) {
        printf("  mesh FAIL %-12s version 0x%X unsupported\n", name,
               (unsigned)ver);
        goto textures;
    }
    if (cxd_best_section(&d, &sec) != 0) {
        printf("  mesh FAIL %-12s no valid LOD section\n", name);
        goto textures;
    }
    nv = sec.maxidx + 1;
    verts = cxd_read_verts(&d, sec.pool, nv);
    if (!verts)
        goto textures;
    zmin = zmax = verts[0].z;
    for (i = 1; i < nv; i++) {
        if (verts[i].z < zmin) zmin = verts[i].z;
        if (verts[i].z > zmax) zmax = verts[i].z;
    }
    /* Q15: 60 m, not extract_bgv.py's 20 m */
    if (!(zmax - zmin > 0.5 && zmax - zmin < 60.0)) {
        printf("  mesh FAIL %-12s implausible length %.1f\n", name,
               zmax - zmin);
        goto textures;
    }

    groups = (cxd_group *)malloc((size_t)(sec.body.n + 1) * sizeof *groups);
    if (!groups)
        goto textures;
    /* the intact set: the single mask-0x3FF draw, glass records dropped
     * (traffic is drawn opaque by the harness) */
    for (i = 0; i < sec.body.n; i++) {
        if ((sec.body.rec[i].mask & 0x300) != 0)
            continue;
        snprintf(groups[ng].name, sizeof groups[ng].name, "body_m%X",
                 sec.body.rec[i].mask);
        groups[ng].tex = -1;                     /* Q13: legacy, no usemtl */
        groups[ng].rec = &sec.body.rec[i];
        ng++;
    }
    if (ng == 0) {                               /* Q14: the fallback names */
        for (i = 0; i < sec.body.n; i++) {
            snprintf(groups[ng].name, sizeof groups[ng].name, "m%X",
                     sec.body.rec[i].mask);
            groups[ng].tex = -1;
            groups[ng].rec = &sec.body.rec[i];
            ng++;
        }
    }
    for (dd = 0; dd < ndirs; dd++) {
        cxd_path(p, sizeof p, dirs[dd], "/", name, ".obj", NULL);
        cxd_write_obj(p, verts, nv, groups, ng, "traffic vehicle, intact"
                      " record set");
    }

    /* wheel: slot 7 when present, else the largest of 7/8/9 */
    {
        const cxd_part *best = NULL, *fb = NULL;
        int fb_size = -1, slot;
        for (slot = 7; slot <= 9; slot++) {
            const cxd_part *pr = cxd_slot(&sec, slot);
            int size = 0, j;
            if (!pr)
                continue;
            for (j = 0; j < pr->n; j++)
                size += pr->rec[j].ntris;
            if (slot == 7 && best == NULL)
                best = pr;
            if (size > fb_size) { fb_size = size; fb = pr; }
        }
        if (best == NULL)
            best = fb;
        if (best) {
            cxd_group *wg = (cxd_group *)malloc((size_t)(best->n + 1)
                                                * sizeof *wg);
            if (wg) {
                int n = 0, j;
                for (j = 0; j < best->n; j++) {
                    snprintf(wg[n].name, sizeof wg[n].name, "m%X",
                             best->rec[j].mask);
                    wg[n].tex = -1;
                    wg[n].rec = &best->rec[j];
                    n++;
                }
                for (dd = 0; dd < ndirs; dd++) {
                    cxd_path(p, sizeof p, dirs[dd], "/", name, "_wheel.obj", NULL);
                    cxd_write_obj(p, verts, nv, wg, n,
                                  "traffic wheel (slot 7 slow, 8/9 blur"
                                  " fallback), same records as .bgv");
                }
                free(wg);
            }
        }
    }

    /* Q16: a shorter .wheels than extract_bgv.py's -- no ext/center rows */
    {
        cxd_wheel wh[6];
        double radius;
        int n = cxd_read_wheels(&d, &radius, wh, 6);
        for (dd = 0; dd < ndirs; dd++) {
            FILE *f;
            cxd_path(p, sizeof p, dirs[dd], "/", name, ".wheels", NULL);
            f = fopen(p, "wb");
            if (!f)
                continue;
            fputs("# wheel radius (file+0x18) + attach matrices"
                  " (file+0xB80) [C],\n# same layout as .bgv"
                  " (shared relinker FUN_000310F0)\n", f);
            fprintf(f, "radius %.4f\n", radius);
            for (i = 0; i < n; i++)
                fprintf(f, "wheel %.4f %.4f %.4f %d\n", wh[i].pos[0],
                        wh[i].pos[1], wh[i].pos[2], wh[i].mirror);
            fclose(f);
        }
    }
    (*got_mesh)++;

textures:                                        /* Q17: independent halves */
    if (cxd_extract_paint(&d, texname, sizeof texname, &w, &h, &imgs, &nimg,
                          &err) != 0) {
        printf("  tex  FAIL %-12s %s\n", name, err ? err : "?");
    } else {
        int k;
        for (k = 0; k < nimg; k++)
            for (dd = 0; dd < ndirs; dd++) {
                char leaf[160];
                snprintf(leaf, sizeof leaf, "%s_p%d.png", name, k);
                cxd_path(p, sizeof p, dirs[dd], "/", leaf, NULL);
                cx_png_write_rgba8(p, imgs[k], w, h);
            }
        cxd_free_paint(imgs, nimg);
        (*got_tex)++;
    }
    free(groups);
    free(verts);
    cxd_section_free(&sec);
    cxd_blob_free(&d);
    return 0;
}

/* Derive the dump-global mirror (python's CARS_OUT = build/cars) from the
 * per-track out_dir: <repo>/build/tracks/<ID> two levels up is <repo>/build,
 * then /cars -- which is where the dump-global car stages write under the
 * repo-root convention.  $B3_CARS_OUT overrides. */
static void mirror_dir(const char *out_dir, char *dst, size_t cap)
{
    const char *e = getenv("B3_CARS_OUT");
    char t[4096];
    size_t n;
    char *s;

    if (e && *e) {
        cxd_path(dst, cap, e, NULL);
        return;
    }
    cxd_path(t, sizeof t, out_dir, NULL);
    n = strlen(t);
    while (n > 1 && t[n - 1] == '/')
        t[--n] = 0;
    s = strrchr(t, '/');
    if (s) *s = 0;                                /* <repo>/build/tracks    */
    s = strrchr(t, '/');
    if (s) *s = 0;                                /* <repo>/build           */
    cxd_path(dst, cap, t, "/cars", NULL);
}

int cx_extract_traffic_cars(const char *game_dir, const char *track_dir,
                            const char *track_id, const char *out_dir)
{
    char dirs[2][4096];
    cxd_tcar *cars = NULL;
    int ncars = 0, i, ndirs = 0, got_mesh = 0, got_tex = 0;

    (void)track_dir;
    (void)track_id;

    if (read_roster(out_dir, &cars, &ncars) != 0) {
        fprintf(stderr, "[cx_cars_traffic] %s/traffic.bin missing or short --"
                " run the TRAFFIC stage first\n", out_dir);
        return 1;
    }
    cxd_path(dirs[ndirs], sizeof dirs[0], out_dir, "/cars", NULL);
    ndirs++;
    mirror_dir(out_dir, dirs[ndirs], sizeof dirs[0]);
    ndirs++;
    for (i = 0; i < ndirs; i++)
        cxd_mkdir_p(dirs[i]);

    /* The paint half of this stage is the same 512x512 deflate cx_cars_paint.c
     * pays for, doubled because every page is written into BOTH the per-track
     * cars/ and the dump-global mirror.  The walk stays serial -- export_one()
     * prints per-car lines and bumps the two counters in visit order -- and
     * the encode goes to the pool (THE QUEUE, cx_png.h). */
    cx_png_queue_begin();
    for (i = 0; i < ncars; i++) {
        char src[4096], name[128];
        cxd_path(src, sizeof src, game_dir, "/pveh/", cars[i].cls, "/",
                 cars[i].car, ".btv", NULL);
        cxd_path(name, sizeof name, cars[i].cls, "_", cars[i].car, NULL);
        export_one(src, name, dirs, ndirs, &got_mesh, &got_tex);
    }
    if (cx_png_queue_flush() != 0)
        fprintf(stderr, "[cx_cars_traffic] one or more PNGs failed to write\n");
    printf("traffic assets: %d/%d meshes, %d/%d paint sets -> %s, %s\n",
           got_mesh, ncars, got_tex, ncars, dirs[0], dirs[1]);
    free(cars);
    return 0;
}
