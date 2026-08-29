/* cx_car_bvh.c -- the CARS' ray-tracing acceleration structure
 * -> <out_root>/build/cars/carbvh.bin ('B3CV' v1).
 *
 * ======================================================= NO PYTHON ORACLE ==
 * Like cx_bvh.c and for the same reason: this is a DERIVED artefact serving an
 * INSPIRED renderer feature (docs/PHOTOREALISM.md tier 4r), it has no archived
 * tools/py_extract_archive/extract_*.py counterpart, and it has no retail
 * counterpart either.  verify_cextract.py's oracle diff cannot cover it;
 * tools/validate_car_bvh.py is the gate instead, and it re-derives every
 * triangle from the OBJs cx_cars_bgv.c writes.
 *
 * *** NOTHING IN THIS FILE IS A CLAIM ABOUT BURNOUT 3. ***  The Xbox drew a
 * "blobbyshadow" quad under each car (FUN_0019A7C0 / FUN_00043570, [C]) and
 * nothing else; it had no acceleration structure of any kind.  This is a
 * modern construction, marked INSPIRED, and must never be cited as game
 * behaviour.  See the EVIDENCE MARKS block in src/burnout3_aftereffects.h.
 *
 * ================================================================== WHY
 * bvh.bin is the STATIC world, and it says so in its own header: cars,
 * traffic and knocked props are out of it because a BVH is built once and a
 * car moves every frame.  Rebuilding a tree over six moving cars per frame is
 * not a thing a 3.5 ms budget can pay for.
 *
 * What it CAN pay for is the standard answer: a TWO-LEVEL trace.  A car is a
 * RIGID body, so its triangles never move relative to each other -- only the
 * whole model does.  So the tree is built ONCE, in MODEL SPACE, and the
 * per-frame cost is one 3x4 matrix per instance.  A shadow ray tests the
 * static world as before, then for each car instance transforms itself into
 * that car's model space and walks that car's tree.  Nothing is rebuilt, ever.
 *
 * This file is the bottom level (the "BLAS", one tree per car MODEL).  The top
 * level is six vec4 uniform arrays uploaded per frame by
 * src/burnout3_aftereffects.c -- there is no acceleration structure over the
 * instances at all, because with a handful of them a linear scan behind a
 * bounding-sphere reject is cheaper than anything that would index them.
 *
 * ============================================================= THE INPUTS
 * The .bgv / .btv CONTAINERS, through cx_cars_common.c's reader -- NOT the
 * OBJs, and that is the one place this stage deliberately differs from
 * cx_bvh.c's "read what the renderer draws" rule.  The reason is ORDERING:
 * the player fleet's OBJs are written by a dump-global stage but the traffic
 * fleet's are written PER TRACK, so a global stage that read OBJs would emit a
 * different file depending on which tracks had been extracted first -- and a
 * cached artefact that depends on the order somebody visited tracks in is not
 * a deterministic artefact.  Reading the containers makes this stage depend on
 * the DISC and on nothing else.
 *
 * The cost of that choice is that "does this agree with what the renderer
 * draws" becomes a claim rather than a construction, so it is GATED instead:
 * tools/validate_car_bvh.py re-derives the triangle set from
 * build/cars/<NAME>_intact.obj and compares it as a multiset, which is a
 * genuinely independent second source (a text round-trip through a different
 * writer) rather than a re-run of this code.
 *
 * ================================================================ THE SPACE
 * RAW GAME SPACE -- .bgv space, left-handed, Y up, +Z the NOSE, +X the car's
 * right, metres (tools/blender/bgv_write.py 194-230).  The Z flip into the
 * harness' GL frame is the LOADER's job here as it is for every other geometry
 * artefact, and src/burnout3_rt.c applies exactly the same single reflection
 * to this file that it applies to bvh.bin.  The MODEL ORIGIN is the .bgv's own
 * origin, i.e. the point the renderer puts at
 * (v->pos.x, v->pos.y - 0.5 - g_car_ymin, v->pos.z) -- wheel hubs at y = 0.
 *
 * ============================================================== WHAT IS IN IT
 *   IN   the INTACT body: the embedded one-piece part at S+0x60, records whose
 *        pass mask has neither glass bit (mask & 0x300) == 0.  That is exactly
 *        the record set src/burnout3_full.c draws for a car that is not
 *        wrecked (`g_car_intact_lists`), and exactly what
 *        cx_cars_bgv.c writes to <NAME>_intact.obj.
 *   IN   the WHEELS, slot 7's mesh instanced at the .bgv+0xB80 attach
 *        positions with the mirror flag applied, at REST.  See THE WHEELS.
 *   OUT  GLASS (mask bits 8/9).  Same rule the static tree applies to the
 *        blended pass: a transparent surface that casts a solid shadow is
 *        worse than one that casts none.
 *   OUT  the DAMAGE STATES.  See THE DAMAGE DECISION.
 *
 * =========================================================== THE LOD SECTION
 * The highest-detail one, picked by cxd_best_section() -- the same choice
 * cx_cars_bgv.c makes, so the tree is over the mesh the OBJ carries.  A car
 * is ~2000 triangles at LOD 0 and the whole 107-car fleet fits in about 4 MB
 * of float texture, so there is nothing to gain by tracing a coarser one and
 * a silhouette to lose.
 *
 * ======================================================= THE DAMAGE DECISION
 * v1 TRACES THE INTACT HULL ALWAYS, including for a wreck, and that is a
 * limitation rather than an oversight.
 *
 * Damage in this port is DISCRETE -- record-set swapping, not vertex
 * deformation (the .bgv's skin stream at S+0x58 is not read by anything in
 * this pipeline).  A car is drawn one of exactly two ways
 * (src/burnout3_full.c:16139-16208):
 *
 *     intact   the embedded one-piece part, non-glass records
 *     wrecked  slot 0's aperture body PLUS every panel that has not detached
 *              yet, each at its own pivot from <NAME>.panels
 *
 * So a faithful wreck would need a SECOND tree per car (the shell) and, worse,
 * a per-frame decision about which of up to six panels are still attached --
 * i.e. a tree whose CONTENTS change while the car is being driven, which is
 * the one thing the whole two-level construction exists to avoid.  The shell
 * and the intact body differ by the door and bonnet apertures: a few holes in
 * a silhouette, at the moment the player is watching a car tumble.
 *
 * The intact hull is therefore traced in both states, the header records the
 * choice, and docs/PHOTOREALISM.md says so where a reader will find it.  A v2
 * that wanted the wreck right would emit a second model per car and let the
 * uploader pick -- the format below already has room, because the instance
 * names a MODEL and nothing here says a car may only have one.
 *
 * ================================================================ THE WHEELS
 * BAKED IN, at rest, and the argument is the silhouette.  A car's wheels are
 * the lowest thing on it, so under a low sun they are most of what the shadow
 * is; without them the shadow is a floating slab with four gaps under it.  They
 * are also cheap: slot 7 is the SLOW wheel LOD and runs a couple of hundred
 * triangles, so four of them add well under a fifth to a car's tree.
 *
 * Two things are deliberately not modelled, and both are invisible at this
 * scale: SPIN, because a tyre is very nearly a solid of revolution about its
 * own axis and a rotated rim differs from an unrotated one by the spokes; and
 * STEER, because the front wheels turn a handful of degrees about a 0.3 m
 * chord.  B3_RT_CAR_WHEELS=0 builds hull-only, which is what the cost figure
 * in the report was measured against.
 *
 * ============================================================== carbvh.bin
 * Little-endian, RAW GAME SPACE.  One file for the WHOLE FLEET: the runtime
 * uploads it as a single texture and an instance names a model by index, so
 * 107 files would be 107 uploads and 107 chances to be missing one.
 *
 *     +0x00 'B3CV'   +0x04 u32 version = 1
 *     +0x08 u32 model_count      +0x0C u32 node_count
 *     +0x10 u32 tri_count        +0x14 u32 off_models
 *     +0x18 u32 off_nodes        +0x1C u32 off_tris
 *     +0x20 u32 max_leaf         +0x24 u32 max_depth
 *     +0x28 u32 n_bgv            +0x2C u32 n_btv
 *     +0x30 u32 tris_wheel       +0x34 u32 models_refused
 *     +0x38 u32 flags            bit0 = wheels were baked
 *     +0x3C u32 reserved = 0
 *   model 0x50, sorted by NAME so the loader can bisect:
 *     +0x00 char[32] name        "<CLASS>_<stem>", NUL-padded
 *     +0x20 u32 node_first       this model's ROOT node
 *     +0x24 u32 node_count       the root's escape is node_first + this
 *     +0x28 u32 tri_first        +0x2C u32 tri_count
 *     +0x30 f32[3] bb_min        +0x3C f32[3] bb_max   (model space)
 *     +0x48 f32 radius           about the box CENTRE, for the instance reject
 *     +0x4C u32 flags            bit0 wheels baked, bit1 traffic (.btv)
 *   node 0x30 and triangle 0x30: EXACTLY bvh.bin's records, written by the
 *     shared cxv_put_node() -- one builder, one shader contract
 *     (tools/cextract/cx_bvh_build.h).  A triangle's `source` is 0 for the
 *     body and 1 for a wheel; its `ref` is the MODEL index; its opacity is
 *     1.0, because a car has no cut-out material (the one thing that would
 *     be, the glass, is excluded outright).
 *
 * THE ESCAPES ARE GLOBAL, i.e. already offset by node_first, so the shader's
 * walk needs no per-model addition: it starts at node_first and stops when the
 * index reaches node_first + node_count.  The LEAF PACKING is the shared one
 * -- `first_tri * 8 + (count - 1)` over the GLOBAL triangle index -- and the
 * stage refuses to write a file that would leave a highp float's 24 exact
 * bits, exactly as cx_bvh.c does.  The whole fleet is ~150k triangles against
 * a ceiling of 2,097,151, so the refusal is a guard rail and not a limit.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cx_bvh_build.h"
#include "cx_cars.h"
#include "cx_common_c.h"
#include "cx_extract.h"

#define CXO_HDR        0x40
#define CXO_MODEL_REC  0x50
#define CXO_NAME       32

/* ---------------------------------------------------------------- the OBJ
 * ROUNDING.
 *
 * cxd_write_obj() prints a position as "%.5f" and the game's OBJ loader reads
 * it back with a float scanf, so the number the renderer actually draws is the
 * .bgv's double rounded to five decimals.  This stage reads the .bgv rather
 * than the OBJ (see THE INPUTS), so it has to do the same rounding by hand --
 * otherwise the tree would be over a mesh ten microns away from the drawn one,
 * which nobody would ever see and tools/validate_car_bvh.py could never prove
 * equal.  Five decimals at car scale is 10 um; the point is not the precision,
 * it is that the two sets are the SAME SET. */
static float cxo_objf(double v)
{
    char b[40];
    snprintf(b, sizeof b, "%.5f", v);
    return (float)atof(b);
}

/* ------------------------------------------------------------ one model */

typedef struct {
    char     name[CXO_NAME];
    char     path[4096];
    int      traffic;          /* .btv */
    /* filled by the build */
    uint32_t node_first, node_count, tri_first, tri_count;
    float    lo[3], hi[3], radius;
    uint32_t flags;
    uint32_t wheel_tris;
    int      ok;
    char     why[96];
} CxoModel;

static int cxo_name_cmp(const void *a, const void *b)
{
    return strcmp(((const CxoModel *)a)->name, ((const CxoModel *)b)->name);
}

/* Push one record's triangles through `xform` (NULL = identity) into `B`. */
static int cxo_push_rec(CxvBuild *B, const cxd_rec *r, const cxd_vert *verts,
                        int nv, uint32_t source, uint32_t ref,
                        const float mirror_xz, const float off[3])
{
    int t;
    for (t = 0; t < r->ntris; t++) {
        float p[3][3];
        int   k, e, bad = 0;
        for (k = 0; k < 3; k++) {
            unsigned vi = r->tris[t * 3 + k];
            const cxd_vert *v;
            if ((int)vi >= nv) { bad = 1; break; }
            v = &verts[vi];
            p[k][0] = cxo_objf(v->x);
            p[k][1] = cxo_objf(v->y);
            p[k][2] = cxo_objf(v->z);
            /* the mirror flag is a 180-degree turn about Y, which on a
             * position is exactly "negate x and z" -- the same thing the
             * renderer's b3r_rotate(180, 0,1,0) does to the wheel it is
             * about to draw (src/burnout3_full.c wheel loop) */
            if (mirror_xz < 0.0f) { p[k][0] = -p[k][0]; p[k][2] = -p[k][2]; }
            for (e = 0; e < 3; e++) p[k][e] += off[e];
        }
        if (bad) continue;
        if (cxv_tri_push(B, p[0], p[1], p[2], 1.0f, source, ref) != 0)
            return -1;
    }
    return 0;
}

/* Collect one car's triangles into `B`, which the caller has already primed
 * with everything emitted so far.  Returns 0 and fills `m`'s counts, or -1
 * with `m->why` set (and B left holding whatever it had before the call --
 * the caller rewinds). */
static int cxo_collect(CxoModel *m, CxvBuild *B, int wheels)
{
    cxd_blob    d = { NULL, 0 };
    cxd_section sec;
    cxd_vert   *verts = NULL;
    int         nv, i, rc = -1;
    uint32_t    ver;
    double      zmin, zmax;

    memset(&sec, 0, sizeof sec);
    if (cxd_read_file(m->path, &d) != 0) {
        snprintf(m->why, sizeof m->why, "cannot read");
        return -1;
    }
    if (d.n < 0x1000) { snprintf(m->why, sizeof m->why, "too small"); goto done; }
    ver = cxd_u32(&d, 0);
    if (!(ver >= 0x14 && ver <= 0x25)) {
        snprintf(m->why, sizeof m->why, "version 0x%X unsupported",
                 (unsigned)ver);
        goto done;
    }
    if (cxd_best_section(&d, &sec) != 0) {
        snprintf(m->why, sizeof m->why, "no valid LOD section");
        goto done;
    }
    nv = sec.maxidx + 1;
    verts = cxd_read_verts(&d, sec.pool, nv);
    if (!verts) { snprintf(m->why, sizeof m->why, "no vertex pool"); goto done; }

    /* THE SAME PLAUSIBILITY GATE cx_cars_bgv.c applies, and for the same
     * reason: a file whose "car" is 400 m long is a file this reader has
     * misparsed, and a misparsed car in a shadow tree is a black rectangle
     * travelling down the road.  The traffic fleet's TSPC specials are long
     * (a coach), so the ceiling is the wider one cx_cars_traffic.c uses. */
    zmin = zmax = verts[0].z;
    for (i = 1; i < nv; i++) {
        if (verts[i].z < zmin) zmin = verts[i].z;
        if (verts[i].z > zmax) zmax = verts[i].z;
    }
    if (!(zmax - zmin > 0.5 && zmax - zmin < 60.0)) {
        snprintf(m->why, sizeof m->why, "implausible length %.1f",
                 zmax - zmin);
        goto done;
    }

    /* ---- the intact body: non-glass records of the embedded one-piece
     * part, which is the record set the renderer draws for a car that is
     * not wrecked */
    {
        static const float NOOFF[3] = { 0.0f, 0.0f, 0.0f };
        for (i = 0; i < sec.body.n; i++) {
            const cxd_rec *r = &sec.body.rec[i];
            if ((r->mask & 0x300) != 0) continue;         /* glass */
            if (cxo_push_rec(B, r, verts, nv, 0u, 0u, 1.0f, NOOFF) != 0)
                goto done;
        }
    }

    /* ---- the wheels, at rest, one instance per attach matrix ---------- */
    if (wheels) {
        const cxd_part *wp = cxd_slot(&sec, 7);
        cxd_wheel       wh[8];
        double          radius = 0.0;
        int             nw = cxd_read_wheels(&d, &radius, wh, 8);
        size_t          before = B->ntri;
        if (wp && nw > 0) {
            int w;
            for (w = 0; w < nw; w++) {
                float off[3];
                off[0] = cxo_objf(wh[w].pos[0]);
                off[1] = cxo_objf(wh[w].pos[1]);
                off[2] = cxo_objf(wh[w].pos[2]);
                for (i = 0; i < wp->n; i++)
                    if (cxo_push_rec(B, &wp->rec[i], verts, nv, 1u, 0u,
                                     wh[w].mirror < 0 ? -1.0f : 1.0f,
                                     off) != 0) goto done;
            }
            m->flags |= 1u;
        }
        m->wheel_tris = (uint32_t)(B->ntri - before);
    }
    rc = 0;

done:
    free(verts);
    cxd_section_free(&sec);
    cxd_blob_free(&d);
    return rc;
}

/* --------------------------------------------------------------- the walk */

static int cxo_add(CxoModel **items, int *n, int *cap, const char *cls,
                   const char *file, const char *cdir, int traffic)
{
    CxoModel   *t;
    const char *dot = strrchr(file, '.');
    char        stem[64];

    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 128;
        CxoModel *nv = (CxoModel *)realloc(*items, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        *items = nv;
        *cap = nc;
    }
    t = &(*items)[(*n)++];
    memset(t, 0, sizeof *t);
    snprintf(stem, sizeof stem, "%.*s",
             dot ? (int)(dot - file) : (int)strlen(file), file);
    cxd_path(t->name, sizeof t->name, cls, "_", stem, NULL);
    cxd_path(t->path, sizeof t->path, cdir, "/", file, NULL);
    t->traffic = traffic;
    if (traffic) t->flags |= 2u;
    return 0;
}

int cx_extract_car_bvh(const char *game_dir, const char *out_root)
{
    char        pveh[4096], outdir[4096], path[4096];
    cxd_names   classes;
    CxoModel   *items = NULL;
    CxvBuild    B;
    cxc_buf     out;
    int         n = 0, cap = 0, ci, i, wheels = 1;
    int         ok = 0, refused = 0, n_bgv = 0, n_btv = 0;
    uint32_t    maxdepth = 0, wheel_tris = 0;
    const char *e;
    int         rc = 1;

    memset(&B, 0, sizeof B);
    memset(&out, 0, sizeof out);

    e = getenv("B3_RT_CAR_WHEELS");
    if (e && *e) wheels = (*e != '0');

    cxd_path(pveh, sizeof pveh, game_dir, "/pveh", NULL);
    cxd_path(outdir, sizeof outdir, out_root, "/build/cars", NULL);
    if (cxd_mkdir_p(outdir) != 0) {
        fprintf(stderr, "[cx car_bvh] cannot create %s\n", outdir);
        return 1;
    }
    if (cxd_list_dir(pveh, NULL, 1, &classes) != 0) {
        fprintf(stderr, "[cx car_bvh] cannot list %s (set B3_GAME_DIR)\n",
                pveh);
        return 1;
    }
    /* BOTH FLEETS, from the disc, and only from the disc.  cx_cars_bgv.c walks
     * the .bgv here and cx_cars_lights.c walks the .btv; between them that is
     * every vehicle the game can put on a road, and enumerating them the same
     * way is what makes this artefact a function of the disc alone. */
    for (ci = 0; ci < classes.n; ci++) {
        char      cdir[4096];
        cxd_names fl;
        int       fi;
        cxd_path(cdir, sizeof cdir, pveh, "/", classes.v[ci], NULL);
        if (cxd_list_dir(cdir, ".bgv", 0, &fl) == 0) {
            for (fi = 0; fi < fl.n; fi++)
                if (cxo_add(&items, &n, &cap, classes.v[ci], fl.v[fi],
                            cdir, 0) != 0) { cxd_names_free(&fl); goto done; }
            cxd_names_free(&fl);
        }
        if (cxd_list_dir(cdir, ".btv", 0, &fl) == 0) {
            for (fi = 0; fi < fl.n; fi++)
                if (cxo_add(&items, &n, &cap, classes.v[ci], fl.v[fi],
                            cdir, 1) != 0) { cxd_names_free(&fl); goto done; }
            cxd_names_free(&fl);
        }
    }
    if (n == 0) {
        printf("[cx car_bvh] no vehicles under %s -- nothing written\n", pveh);
        rc = 0;
        goto done;
    }
    /* SORTED BY NAME, so the loader can bisect and so the file does not
     * depend on the order a directory happened to come back in. */
    qsort(items, (size_t)n, sizeof *items, cxo_name_cmp);

    /* ---- one tree per model, appended into one node/triangle array ----
     *
     * SINGLE-THREADED and strictly in order, because the escapes and the leaf
     * packing are both GLOBAL indices: a worker that finished second would
     * have built a tree over the wrong offsets.  The whole fleet is a couple
     * of seconds. */
    for (i = 0; i < n; i++) {
        CxoModel *m = &items[i];
        size_t    tri0 = B.ntri, node0 = B.nnode;
        size_t    j;
        uint32_t  d = 0;
        CxvBuild  sub;

        if (cxo_collect(m, &B, wheels) != 0 || B.err) {
            B.ntri = tri0;                       /* rewind, keep the file */
            refused++;
            continue;
        }
        if (B.ntri == tri0) {
            snprintf(m->why, sizeof m->why, "no triangles");
            refused++;
            continue;
        }
        if (B.ntri > CXV_TRI_MAX) {
            fprintf(stderr, "[cx car_bvh] %s: %zu triangles exceeds the leaf "
                            "packing's ceiling of %u -- refusing to write a "
                            "file the shader would mis-read\n",
                    m->name, B.ntri, (unsigned)CXV_TRI_MAX);
            goto done;
        }

        /* Build THIS model's tree over its own slice.  cxv_build() owns
         * `ord` and `node` for one tree, so it is handed a view of the slice
         * and its output is appended; the escapes and the leaf `first` are
         * then shifted onto the global arrays, which is the whole of what
         * makes one file hold many trees. */
        memset(&sub, 0, sizeof sub);
        sub.tri  = B.tri + tri0;
        sub.ntri = B.ntri - tri0;
        sub.ord  = (uint32_t *)malloc(sub.ntri * sizeof *sub.ord);
        if (!sub.ord) goto done;
        for (j = 0; j < sub.ntri; j++) sub.ord[j] = (uint32_t)j;
        if (cxv_build(&sub, &d) != 0 || sub.err) {
            free(sub.ord); free(sub.node);
            goto done;
        }
        if (d > maxdepth) maxdepth = d;

        /* permute this model's triangles into leaf order, in place */
        {
            CxvTri *sorted = (CxvTri *)malloc(sub.ntri * sizeof *sorted);
            if (!sorted) { free(sub.ord); free(sub.node); goto done; }
            for (j = 0; j < sub.ntri; j++) sorted[j] = sub.tri[sub.ord[j]];
            memcpy(B.tri + tri0, sorted, sub.ntri * sizeof *sorted);
            free(sorted);
        }
        /* shift the tree onto the global arrays */
        if (B.nnode + sub.nnode > B.capnode) {
            size_t c = B.capnode ? B.capnode : 4096;
            CxvNode *nn;
            while (c < B.nnode + sub.nnode) c *= 2;
            nn = (CxvNode *)realloc(B.node, c * sizeof *nn);
            if (!nn) { free(sub.ord); free(sub.node); goto done; }
            B.node = nn; B.capnode = c;
        }
        for (j = 0; j < sub.nnode; j++) {
            CxvNode nd = sub.node[j];
            nd.escape += (uint32_t)node0;
            if (nd.count) nd.first += (uint32_t)tri0;
            B.node[node0 + j] = nd;
        }
        B.nnode += sub.nnode;

        m->node_first = (uint32_t)node0;
        m->node_count = (uint32_t)sub.nnode;
        m->tri_first  = (uint32_t)tri0;
        m->tri_count  = (uint32_t)(B.ntri - tri0);
        memcpy(m->lo, sub.node[0].lo, sizeof m->lo);
        memcpy(m->hi, sub.node[0].hi, sizeof m->hi);
        {
            /* the sphere the SHADER rejects an instance with: centred on the
             * box, big enough to contain it, so a miss against it is a
             * genuine miss and not an approximation */
            float dx = (m->hi[0] - m->lo[0]) * 0.5f;
            float dy = (m->hi[1] - m->lo[1]) * 0.5f;
            float dz = (m->hi[2] - m->lo[2]) * 0.5f;
            m->radius = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        wheel_tris += m->wheel_tris;
        m->ok = 1;
        ok++;
        if (m->traffic) n_btv++; else n_bgv++;
        free(sub.ord);
        free(sub.node);
    }

    if (!ok) {
        fprintf(stderr, "[cx car_bvh] no vehicle produced a tree\n");
        goto done;
    }

    /* ------------------------------------------------------------- write */
    cxc_put_bytes(&out, "B3CV", 4);
    cxc_put_u32(&out, 1u);
    cxc_put_u32(&out, (uint32_t)ok);
    cxc_put_u32(&out, (uint32_t)B.nnode);
    cxc_put_u32(&out, (uint32_t)B.ntri);
    cxc_put_u32(&out, (uint32_t)CXO_HDR);
    cxc_put_u32(&out, (uint32_t)(CXO_HDR + (size_t)ok * CXO_MODEL_REC));
    cxc_put_u32(&out, (uint32_t)(CXO_HDR + (size_t)ok * CXO_MODEL_REC
                                 + B.nnode * CXV_NODE_REC));
    cxc_put_u32(&out, (uint32_t)CXV_LEAF_MAX);
    cxc_put_u32(&out, maxdepth);
    cxc_put_u32(&out, (uint32_t)n_bgv);
    cxc_put_u32(&out, (uint32_t)n_btv);
    cxc_put_u32(&out, wheel_tris);
    cxc_put_u32(&out, (uint32_t)refused);
    cxc_put_u32(&out, wheels ? 1u : 0u);
    cxc_put_u32(&out, 0u);
    cxc_pad_to(&out, CXO_HDR);

    for (i = 0; i < n; i++) {
        const CxoModel *m = &items[i];
        int k;
        if (!m->ok) continue;
        cxc_put_bytes(&out, m->name, CXO_NAME);
        cxc_put_u32(&out, m->node_first);
        cxc_put_u32(&out, m->node_count);
        cxc_put_u32(&out, m->tri_first);
        cxc_put_u32(&out, m->tri_count);
        for (k = 0; k < 3; k++) cxc_put_f32(&out, m->lo[k]);
        for (k = 0; k < 3; k++) cxc_put_f32(&out, m->hi[k]);
        cxc_put_f32(&out, m->radius);
        cxc_put_u32(&out, m->flags);
    }
    for (i = 0; i < (int)B.nnode; i++) cxv_put_node(&out, &B.node[i]);
    for (i = 0; i < (int)B.ntri; i++) {
        const CxvTri *t = &B.tri[i];
        int k;
        for (k = 0; k < 9; k++) cxc_put_f32(&out, t->v[k]);
        cxc_put_f32(&out, t->opacity);
        cxc_put_u32(&out, t->source);
        cxc_put_u32(&out, t->ref);
    }
    if (out.err) goto done;

    cxd_path(path, sizeof path, outdir, "/carbvh.bin", NULL);
    if (cxc_buf_write(&out, path) != 0) goto done;

    for (i = 0; i < n; i++)
        if (!items[i].ok)
            printf("[cx car_bvh] skip %-24s %s\n", items[i].name,
                   items[i].why[0] ? items[i].why : "?");
    printf("[cx car_bvh] %d models (%d bgv + %d btv, %d refused)  %u tris "
           "(%u wheel)  %u nodes  depth %u  %.2f MB\n",
           ok, n_bgv, n_btv, refused, (unsigned)B.ntri, wheel_tris,
           (unsigned)B.nnode, maxdepth,
           (double)out.n / (1024.0 * 1024.0));
    rc = 0;

done:
    /* B.tri is ours; B.node is ours; B.ord was never used at this level */
    cxv_build_free(&B);
    free(items);
    cxd_names_free(&classes);
    cxc_buf_free(&out);
    return rc;
}
