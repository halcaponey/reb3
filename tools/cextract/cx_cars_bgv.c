/* cx_cars_bgv.c -- port of tools/extract_bgv.py.
 *
 * Every player vehicle's .bgv mesh out of pveh/<CLASS>/, to
 * <out_root>/build/cars/ -- `out_root` being a REPO-ROOT stand-in, the
 * project-wide convention, so the tree matches the python's own build/cars:
 *
 *   <NAME>.obj          whole car, every record of the embedded part object
 *   <NAME>_intact.obj   the pristine car: the embedded one-piece part
 *                       (S+0x60), non-glass records -- exactly the single
 *                       mask-0x3FF draw of the intact state
 *   <NAME>_shell.obj    slot 0 aperture body (car minus panels, interior/
 *                       driver): the damage-path body / wreck end state
 *   <NAME>_glass.obj    embedded-part glass records (mask bits 8/9)
 *   <NAME>_wheel.obj    wheel mesh (slot 7 -- the slow wheel below 25 rad/s)
 *   <NAME>.wheels       radius + per-wheel attach pos + mirror flag + the
 *                       physics body extents/centre
 *   <NAME>.panels       per-panel placement matrices + pivot-local AABBs
 *   parts/<NAME>/panel<K>_kind<D>.obj   per-panel meshes, pivot-local
 *   parts/<NAME>/wheel_slot<S>.obj      all three wheel LOD/blur slots
 *
 * The format citations live in cx_cars_common.c's header comment; the damage-
 * state split is [C deep-traced] via tools/trace_panels.py --deep, executing
 * the real FUN_00031E10 / FUN_00031AB0 / FUN_000315C0.
 *
 * ============================================================ PYTHON QUIRKS
 * Byte identity is the spec, so these are reproduced deliberately:
 *
 *  Q1  `shell` falls back to a COPY OF `intact` when the section carries no
 *      truthy slot 0 -- and the copy keeps intact's group names, so such a
 *      car's _shell.obj says `o body_m<M>`, never `o shell_s0_m<M>`.
 *  Q2  parse_part() returns None (rejecting the WHOLE part, records already
 *      collected included) when a record runs off the end of the file or any
 *      index exceeds 0x8000, but merely SKIPS a record whose index count is
 *      < 3 or whose payload is out of range.
 *  Q3  the index count at rec+0x10 is read as a u32 and masked to its low 16
 *      bits (`isize & 0xFFFF`), not read as a u16.  Identical on real data;
 *      kept because it is what the python does.
 *  Q4  parts/<NAME>/ is created for every car, even one with no panels and no
 *      wheel slots, so the tree carries empty directories.
 *  Q5  <NAME>.panels is written even when numBodyParts is 0 -- header only.
 *  Q6  the whole-car group tag is `part_%02d_tag_%X` over `mask | (slot<<16)`,
 *      so the texture slot is encoded twice: in the name AND in `usemtl`.
 *  Q7  write_obj() writes NOTHING and reports failure when a group set
 *      references no vertex; the caller ignores that for the four body OBJs,
 *      so a file can simply be absent rather than empty.
 *  Q8  _wheel.obj prefers slot 7 whenever it exists, however few triangles it
 *      has, and only falls back to the LARGEST of 7/8/9 when slot 7 is absent.
 *  Q9  read_wheels() takes the mirror flag from row Right's X only, and
 *      `>= 0` maps to +1 -- an exactly-zero Right.x reads as +1.
 */
#include "cx_cars.h"
#include "cx_extract.h"
#include "cx_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
static int build_groups(const cxd_part *p, cxd_group *g, int cap,
                        const char *kind)
{
    int i, n = 0;

    for (i = 0; i < p->n && n < cap; i++) {
        const cxd_rec *r = &p->rec[i];
        if (!strcmp(kind, "whole")) {
            snprintf(g[n].name, sizeof g[n].name, "part_%02d_tag_%X", i,
                     (unsigned)(r->mask | ((unsigned)r->tex << 16)));
        } else if (!strcmp(kind, "intact")) {
            if ((r->mask & 0x300) != 0)
                continue;
            snprintf(g[n].name, sizeof g[n].name, "body_m%X", r->mask);
        } else if (!strcmp(kind, "glass")) {
            if ((r->mask & 0x300) == 0)
                continue;
            snprintf(g[n].name, sizeof g[n].name, "glass_m%X", r->mask);
        } else if (!strcmp(kind, "shell")) {
            if ((r->mask & 0x300) != 0)
                continue;
            snprintf(g[n].name, sizeof g[n].name, "shell_s0_m%X", r->mask);
        } else if (!strcmp(kind, "panel")) {
            snprintf(g[n].name, sizeof g[n].name, "m%X_t%d", r->mask,
                     (int)r->tex);
        } else {                                 /* "wheel" */
            snprintf(g[n].name, sizeof g[n].name, "m%X", r->mask);
        }
        g[n].tex = (int)r->tex;
        g[n].rec = r;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
struct cxd_bgv_stats {
    int nverts, ntris, nbody, npanels, nwheel_mesh, nwheelpos;
};

/* tools/extract_bgv.py extract().  Returns 0, or -1 with *err set to the
 * python's own failure string. */
static int extract_one(const char *path, const char *outdir,
                       const char *partsdir, const char *name,
                       struct cxd_bgv_stats *st, const char **err)
{
    /* THREAD-LOCAL: the fleet walk below runs this on a worker pool and *err
     * points into this buffer after the call returns.  See the same note in
     * cxd_extract_paint(). */
    static _Thread_local char msg[128];
    cxd_blob d = { NULL, 0 };
    cxd_section sec;
    cxd_vert *verts = NULL;
    cxd_group *groups = NULL;
    int nv = 0, i, k, rc = -1;
    uint32_t ver;
    int nb;
    double zmin = 0.0, zmax = 0.0;
    char p[4096], pdir[4096], note[512], leaf[160];
    FILE *f;
    const cxd_part *slot0;
    int32_t *kinds = NULL;

    memset(&sec, 0, sizeof sec);
    memset(st, 0, sizeof *st);
    *err = NULL;

    if (cxd_read_file(path, &d) != 0) { *err = "cannot read"; return -1; }
    if (d.n < 0x1000) { *err = "too small"; goto done; }

    ver = cxd_u32(&d, 0);
    if (!(ver >= 0x14 && ver <= 0x25)) {
        snprintf(msg, sizeof msg, "version 0x%X unsupported", (unsigned)ver);
        *err = msg;
        goto done;
    }
    nb = cxd_u8(&d, 0x0C);
    /* The python would raise straight out of struct.unpack_from here; every
     * shipped file has numBodyParts <= 6, so this only guards against UB. */
    if ((size_t)0xEA0 + (size_t)nb * 0x20u > d.n
        || (size_t)0xD00 + (size_t)nb * 0x40u > d.n) {
        *err = "panel tables out of range";
        goto done;
    }
    /* +0xAC4 i32[numBodyParts] panel kind ids [C: FUN_00023DE0] */
    kinds = (int32_t *)malloc((size_t)(nb ? nb : 1) * sizeof *kinds);
    if (!kinds) goto done;
    for (i = 0; i < nb; i++)
        kinds[i] = (int32_t)((size_t)0xAC4 + 4u * (size_t)i + 4u <= d.n
                             ? (int32_t)cxd_u32(&d, 0xAC4 + 4u * (size_t)i)
                             : -1);

    if (cxd_best_section(&d, &sec) != 0) { *err = "no valid LOD section"; goto done; }
    nv = sec.maxidx + 1;
    verts = cxd_read_verts(&d, sec.pool, nv);
    if (!verts) goto done;

    zmin = zmax = verts[0].z;
    for (i = 1; i < nv; i++) {
        if (verts[i].z < zmin) zmin = verts[i].z;
        if (verts[i].z > zmax) zmax = verts[i].z;
    }
    if (!(zmax - zmin > 0.5 && zmax - zmin < 20.0)) {
        snprintf(msg, sizeof msg, "implausible length %.1f", zmax - zmin);
        *err = msg;
        goto done;
    }

    groups = (cxd_group *)malloc((size_t)(sec.body.n + 64) * sizeof *groups);
    if (!groups) goto done;

    /* 1. whole-car OBJ (all body records), each tagged with its texture slot */
    cxd_path(p, sizeof p, outdir, "/", name, ".obj", NULL);
    i = build_groups(&sec.body, groups, sec.body.n, "whole");
    cxd_write_obj(p, verts, nv, groups, i,
                  "whole car: every record of the embedded part object");

    /* 2. damage-state variants of the body */
    slot0 = cxd_slot(&sec, 0);
    cxd_path(p, sizeof p, outdir, "/", name, "_intact.obj", NULL);
    i = build_groups(&sec.body, groups, sec.body.n, "intact");
    cxd_write_obj(p, verts, nv, groups, i,
                  "intact car = the embedded one-piece part (S+0x60),"
                  " non-glass records: the single mask-0x3FF draw of the"
                  " intact state (trace_panels.py --deep [C])");

    /* Q1: no truthy slot 0 -> shell is a copy of `intact`, group names and
     * all, so the file says `o body_m<M>`. */
    cxd_path(p, sizeof p, outdir, "/", name, "_shell.obj", NULL);
    if (slot0) {
        cxd_group *sg = (cxd_group *)malloc((size_t)(slot0->n + 1) * sizeof *sg);
        if (sg) {
            int n = build_groups(slot0, sg, slot0->n, "shell");
            cxd_write_obj(p, verts, nv, sg, n,
                          "slot 0 aperture body (car minus panels, interior/"
                          "driver): the damage-path body / wreck end state [C]");
            free(sg);
        }
    } else {
        cxd_write_obj(p, verts, nv, groups, i,
                      "slot 0 aperture body (car minus panels, interior/"
                      "driver): the damage-path body / wreck end state [C]");
    }

    cxd_path(p, sizeof p, outdir, "/", name, "_glass.obj", NULL);
    k = build_groups(&sec.body, groups, sec.body.n, "glass");
    cxd_write_obj(p, verts, nv, groups, k,
                  "embedded-part glass records, mask bits 8/9 (drawn in the"
                  " intact pass; FUN_000300A0 retints by damage tier)");

    /* 3. per-panel meshes (slots 1..numBodyParts, pivot-local) + the sidecar.
     * [C] tools/trace_panels.py executed the draw path: panel k = slot k+1
     * placed at (file+0xD00 + k*0x40) x carframe, row-vector v' = v.A.F. */
    cxd_path(pdir, sizeof pdir, partsdir, "/", name, NULL);
    cxd_mkdir_p(pdir);                           /* Q4: even when empty */
    cxd_path(p, sizeof p, outdir, "/", name, ".panels", NULL);
    f = fopen(p, "wb");                          /* Q5: written even for nb=0 */
    if (!f) goto done;
    fputs("# per-panel placement matrices, file+0xD00 + k*0x40 [C:\n"
          "# execution-traced, tools/trace_panels.py -- FUN_0012FEE0\n"
          "# copies them to ctx+0x180; FUN_000303D0 composes panel\n"
          "# slot k+1 at (ctx+0x180+k*0x40) x frame, row-vector].\n"
          "# panel <k> <kind> <16 floats: rows Right,Up,At,Pos>\n"
          "# panelbb <k> <axis> <max.xyzw> <min.xyzw>  -- the\n"
          "#   PIVOT-LOCAL AABB the flying-part activation ctor\n"
          "#   FUN_001069C0 seeds a detached panel from [C]:\n"
          "#     piece+0x1D0 (bbMAX) = file+0xEA0 + k*0x20 + 0x00\n"
          "#     piece+0x1E0 (bbMIN) = file+0xEA0 + k*0x20 + 0x10\n"
          "#   (the ctor reads them as (idx+0x75)*0x20 off the .bgv\n"
          "#   base -- the same base as the file+0xADC byte below).\n"
          "#   <axis> = file+0xADC+k, the hinge axis that decides\n"
          "#   which component of the box centre is zeroed before\n"
          "#   the recentring (0 -> y, 1 -> x, 2 -> y and z).\n", f);
    for (k = 0; k < nb; k++) {
        const cxd_part *pr;
        int j;
        fprintf(f, "panel %d %d", k, (int)kinds[k]);
        for (j = 0; j < 16; j++)
            fprintf(f, " %.6f", (double)cxd_f32(&d, 0xD00 + (size_t)k * 0x40u
                                                + (size_t)j * 4u));
        fputc('\n', f);
        fprintf(f, "panelbb %d %d", k, (int)cxd_u8(&d, 0xADC + (size_t)k));
        for (j = 0; j < 8; j++)
            fprintf(f, " %.6f", (double)cxd_f32(&d, 0xEA0 + (size_t)k * 0x20u
                                                + (size_t)j * 4u));
        fputc('\n', f);

        pr = cxd_slot(&sec, 1 + k);
        if (!pr)
            continue;
        {
            cxd_group *pg = (cxd_group *)malloc((size_t)(pr->n + 1) * sizeof *pg);
            int n;
            if (!pg)
                continue;
            n = build_groups(pr, pg, pr->n, "panel");
            snprintf(leaf, sizeof leaf, "panel%d_kind%d.obj", k,
                     (int)kinds[k]);
            cxd_path(p, sizeof p, pdir, "/", leaf, NULL);
            snprintf(note, sizeof note,
                     "panel slot %d, kind %d, PIVOT-LOCAL space; placement = "
                     "row-vector matrix file+0x%X (see %s.panels) [C]",
                     1 + k, (int)kinds[k],
                     (unsigned)(0xD00 + (unsigned)k * 0x40u), name);
            if (cxd_write_obj(p, verts, nv, pg, n, note))
                st->npanels++;
            free(pg);
        }
    }
    fclose(f);

    /* 4. wheel mesh.  Q8: slot 7 wins whenever present, else the largest. */
    {
        const cxd_part *best = NULL, *fb = NULL;
        int best_slot = -1, fb_slot = -1, fb_size = -1, slot;
        for (slot = 7; slot <= 9; slot++) {
            const cxd_part *pr = cxd_slot(&sec, slot);
            int size = 0, j;
            if (!pr)
                continue;
            for (j = 0; j < pr->n; j++)
                size += pr->rec[j].ntris;
            if (slot == 7 && best == NULL) { best = pr; best_slot = slot; }
            if (size > fb_size) { fb_size = size; fb = pr; fb_slot = slot; }
        }
        if (best == NULL && fb != NULL) { best = fb; best_slot = fb_slot; }
        if (best) {
            cxd_group *wg = (cxd_group *)malloc((size_t)(best->n + 1) * sizeof *wg);
            if (wg) {
                int n = build_groups(best, wg, best->n, "wheel");
                cxd_path(p, sizeof p, outdir, "/", name, "_wheel.obj", NULL);
                snprintf(note, sizeof note,
                         "wheel slot %d (draw path: 7 slow / 8 / 9 blur), all"
                         " records", best_slot);
                if (cxd_write_obj(p, verts, nv, wg, n, note))
                    st->nwheel_mesh++;
                free(wg);
            }
        }
        for (slot = 7; slot <= 9; slot++) {
            const cxd_part *pr = cxd_slot(&sec, slot);
            cxd_group *wg;
            int n;
            if (!pr)
                continue;
            wg = (cxd_group *)malloc((size_t)(pr->n + 1) * sizeof *wg);
            if (!wg)
                continue;
            n = build_groups(pr, wg, pr->n, "wheel");
            snprintf(leaf, sizeof leaf, "wheel_slot%d.obj", slot);
            cxd_path(p, sizeof p, pdir, "/", leaf, NULL);
            snprintf(note, sizeof note, "wheel slot %d, all records", slot);
            if (cxd_write_obj(p, verts, nv, wg, n, note))
                st->nwheel_mesh++;
            free(wg);
        }
    }

    /* 5. wheel placement (+0xB80 matrices [C] + +0x18 radius) + the physics
     * body extents at +0xE80/+0xE90 [C: FUN_00122830 -> live vehicle
     * +0x1D0/+0x1E0]. */
    {
        cxd_wheel wh[6];
        double radius;
        int n = cxd_read_wheels(&d, &radius, wh, 6);
        cxd_path(p, sizeof p, outdir, "/", name, ".wheels", NULL);
        f = fopen(p, "wb");
        if (!f) goto done;
        fputs("# wheel radius (file+0x18) + attach matrices (file+0xB80,\n"
              "# 4x4 stride 0x40, rows Right/Up/At/Pos) [C: FUN_0012FEE0\n"
              "# copies them into the damage ctx the draw path reads]\n", f);
        fprintf(f, "radius %.4f\n", radius);
        for (i = 0; i < n; i++)
            fprintf(f, "wheel %.4f %.4f %.4f %d\n", wh[i].pos[0], wh[i].pos[1],
                    wh[i].pos[2], wh[i].mirror);
        fprintf(f, "ext %.4f %.4f %.4f %.4f\n",
                (double)cxd_f32(&d, 0xE80), (double)cxd_f32(&d, 0xE84),
                (double)cxd_f32(&d, 0xE88), (double)cxd_f32(&d, 0xE8C));
        fprintf(f, "center %.4f %.4f %.4f %.4f\n",
                (double)cxd_f32(&d, 0xE90), (double)cxd_f32(&d, 0xE94),
                (double)cxd_f32(&d, 0xE98), (double)cxd_f32(&d, 0xE9C));
        fclose(f);
        st->nwheelpos = n;
    }

    st->nverts = nv;
    for (i = 0; i < sec.body.n; i++)
        st->ntris += sec.body.rec[i].ntris;
    st->nbody = nb;
    rc = 0;

done:
    free(kinds);
    free(groups);
    free(verts);
    cxd_section_free(&sec);
    cxd_blob_free(&d);
    return rc;
}

/* ------------------------------------------------------------------------ *
 * PARALLELISM.  107 cars, 1.51 s wall of which 1.45 s is USER time: parsing
 * one .bgv and writing its four body OBJs, its panel and wheel-slot OBJs and
 * its two text sidecars.  extract_one() reads one car's bytes and writes only
 * files named after that car, so the fleet is walked on cx_pool_for()
 * (tools/cextract/cx_pool.h) under exactly the discipline cx_cars_paint.c
 * states: the two nested walks are flattened into one item array in the
 * serial visit order, each item's summary line is stored in its own slot and
 * replayed in index order, and ok/fail are summed afterwards.  The transcript
 * and every output file are what they were.
 * ------------------------------------------------------------------------ */
typedef struct {
    char        cdir[4096];
    char        file[128];
    char        name[128];           /* "<CLASS>_<stem>" -- the output stem */
    int         ok;
    char        line[320];
} mesh_item;

typedef struct {
    mesh_item  *it;
    const char *outdir;
    const char *partsdir;
} mesh_ctx;

static void mesh_one(void *vctx, int i)
{
    mesh_ctx  *c = (mesh_ctx *)vctx;
    mesh_item *t = &c->it[i];
    char       src[4096];
    struct cxd_bgv_stats st;
    const char *err = NULL;

    cxd_path(src, sizeof src, t->cdir, "/", t->file, NULL);
    if (extract_one(src, c->outdir, c->partsdir, t->name, &st, &err) != 0) {
        snprintf(t->line, sizeof t->line, "FAIL %-24s %s\n",
                 t->name, err ? err : "?");
        return;
    }
    snprintf(t->line, sizeof t->line,
             "ok   %-24s %5d verts %5d tris  %d panels->%d objs  "
             "%d wheel meshes  %d wheel pos\n",
             t->name, st.nverts, st.ntris, st.nbody, st.npanels,
             st.nwheel_mesh, st.nwheelpos);
    t->ok = 1;
}

int cx_extract_car_meshes(const char *game_dir, const char *out_root)
{
    char pveh[4096], outdir[4096], partsdir[4096];
    cxd_names classes;
    mesh_item *items = NULL;
    mesh_ctx ctx;
    int ci, n = 0, cap = 0, i, ok = 0, fail = 0;

    cxd_path(pveh, sizeof pveh, game_dir, "/pveh", NULL);
    cxd_path(outdir, sizeof outdir, out_root, "/build/cars", NULL);
    cxd_path(partsdir, sizeof partsdir, outdir, "/parts", NULL);
    if (cxd_mkdir_p(partsdir) != 0) {
        fprintf(stderr, "[cx_cars_bgv] cannot create %s\n", partsdir);
        return 1;
    }
    if (cxd_list_dir(pveh, NULL, 1, &classes) != 0) {
        fprintf(stderr, "[cx_cars_bgv] cannot list %s (set B3_GAME_DIR)\n", pveh);
        return 1;
    }
    for (ci = 0; ci < classes.n; ci++) {
        char cdir[4096];
        cxd_names files;
        int fi;
        cxd_path(cdir, sizeof cdir, pveh, "/", classes.v[ci], NULL);
        if (cxd_list_dir(cdir, ".bgv", 0, &files) != 0)
            continue;
        for (fi = 0; fi < files.n; fi++) {
            char stem[64];
            const char *dot = strrchr(files.v[fi], '.');
            mesh_item *t;
            if (n == cap) {
                int nc = cap ? cap * 2 : 128;
                mesh_item *nv = (mesh_item *)realloc(items,
                                                     (size_t)nc * sizeof *nv);
                if (!nv) { cxd_names_free(&files); goto oom; }
                items = nv;
                cap = nc;
            }
            t = &items[n++];
            memset(t, 0, sizeof *t);
            snprintf(t->cdir, sizeof t->cdir, "%s", cdir);
            snprintf(t->file, sizeof t->file, "%s", files.v[fi]);
            snprintf(stem, sizeof stem, "%.*s",
                     dot ? (int)(dot - files.v[fi]) : (int)strlen(files.v[fi]),
                     files.v[fi]);
            cxd_path(t->name, sizeof t->name, classes.v[ci], "_", stem, NULL);
        }
        cxd_names_free(&files);
    }

    ctx.it = items;
    ctx.outdir = outdir;
    ctx.partsdir = partsdir;
    cx_pool_for(n, mesh_one, &ctx);

    for (i = 0; i < n; i++) {
        if (items[i].line[0])
            fputs(items[i].line, stdout);
        if (items[i].ok) ok++; else fail++;
    }
oom:
    free(items);
    cxd_names_free(&classes);
    printf("\n%d extracted, %d failed -> %s\n", ok, fail, outdir);
    return ok ? 0 : 1;
}
