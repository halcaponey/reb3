/* cx_cars_paint.c -- port of tools/extract_bgv_textures.py.
 *
 * Every player vehicle's paint page, ALL colour variants, to
 * <out_root>/build/cars/<CLASS>_<CarN>_p<K>.png (`out_root` is a REPO-ROOT
 * stand-in, so this is the python's own build/cars).
 *
 * The texture record hangs off the .bgv header +0x60 (an ABSOLUTE offset).
 * Layout (format credit: EdnessP's fmt_Burnout3LRD.py boTexXbox, old
 * revision; cross-checked against the game's draw path, which selects the
 * palette via the count byte at record+0x69 and the pointer array at
 * record+0x14):
 *
 *   +0x04 u32 pixel-data offset (relative to the record)
 *   +0x14 u32[palCount] palette-record offsets (relative to the record)
 *   +0x34 u32 format (0xB = 8bpp paletted), +0x38 w, +0x3C h, +0x40 depth
 *   +0x48 char[] texture name
 *   +0x69 u8  palette count -- one palette per car colour variant
 *   palette record: {u16 1, u16 3 (or 0xC003)}, u32 data offset rel TEXTURE
 *   record; data = 256 x BGRA bytes.
 *
 * Pixels are Morton/Z-order swizzled (Xbox): the low bits of x and y
 * interleave and the larger dimension's high bits follow linearly.
 *
 * V-ORIGIN: surface row 0 is written as PNG row 0, no flip -- the repo-wide
 * rule, and the reason tools/extract_bgv.py emits `vt` unflipped too.
 *
 * QUIRK Q10 (reproduced): the `_p<K>` suffix is the index in the list of
 * SUCCESSFULLY DECODED palettes, not the palette slot.  A record whose
 * palette 0 fails the magic check but whose palette 1 passes emits
 * `..._p0.png` from slot 1.  (No shipped car actually skips one, but the
 * numbering rule is the python's.)
 *
 * Output identity is PIXEL identity against PIL, not byte identity: two
 * different deflate encoders never agree byte for byte (see cx_png.c).
 *
 * ============================================================== PARALLELISM
 * This was the slowest stage in the whole pipeline -- 5.20 s wall of which
 * 4.97 s was USER time, one core, 107 cars.  The work per car is a Morton
 * de-swizzle of a 512x512 page and one zlib deflate per colour variant, and
 * no car touches another car's bytes or another car's output file, so the
 * fleet is walked on cx_pool_for() (tools/cextract/cx_pool.h).
 *
 * What that costs in discipline, and it is the whole of it:
 *   * the two nested walks are FLATTENED first, into one item array in
 *     exactly the order the serial loops visited (class ascending, file
 *     ascending), so item `i` is the same car it always was;
 *   * every per-car printf becomes a line stored in that car's own slot and
 *     replayed in index order afterwards -- the transcript is byte-identical
 *     to the serial one, not merely equivalent;
 *   * `ok` / `fail` are summed after the loop from the per-item results.
 * The disc read inside cxd_read_file() is serialised by the source lock; it
 * is microseconds against milliseconds of encode, which is why this pays.
 */
#include "cx_cars.h"
#include "cx_extract.h"
#include "cx_png.h"
#include "cx_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One car: what the walk found, and what the worker made of it. */
typedef struct {
    const char *cls;                 /* borrowed from `classes` */
    char        cdir[4096];
    char        file[128];
    char        car[64];
    /* result */
    int         ok;
    char        line[320];
} paint_item;

typedef struct {
    paint_item *it;
    const char *outdir;
} paint_ctx;

static void paint_one(void *vctx, int i)
{
    paint_ctx      *c = (paint_ctx *)vctx;
    paint_item     *t = &c->it[i];
    char            src[4096], texname[128], p[4096];
    const char     *err = NULL;
    cxd_blob        d = { NULL, 0 };
    unsigned char **imgs = NULL;
    int             n = 0, w = 0, h = 0, k;

    cxd_path(src, sizeof src, t->cdir, "/", t->file, NULL);
    if (cxd_read_file(src, &d) != 0) {
        snprintf(t->line, sizeof t->line, "FAIL %s_%s: cannot read\n",
                 t->cls, t->car);
        return;
    }
    if (cxd_extract_paint(&d, texname, sizeof texname, &w, &h,
                          &imgs, &n, &err) != 0) {
        snprintf(t->line, sizeof t->line, "FAIL %s_%s: %s\n",
                 t->cls, t->car, err ? err : "?");
        cxd_blob_free(&d);
        return;
    }
    for (k = 0; k < n; k++) {
        char leaf[160];
        snprintf(leaf, sizeof leaf, "_p%d.png", k);
        cxd_path(p, sizeof p, c->outdir, "/", t->cls, "_", t->car, leaf, NULL);
        cx_png_write_rgba8(p, imgs[k], w, h);
    }
    snprintf(t->line, sizeof t->line, "ok   %s_%-8s '%s' %dx%d %d palette(s)\n",
             t->cls, t->car, texname, w, h, n);
    cxd_free_paint(imgs, n);
    cxd_blob_free(&d);
    t->ok = 1;
}

int cx_extract_car_paint(const char *game_dir, const char *out_root)
{
    char pveh[4096], outdir[4096];
    cxd_names classes;
    paint_item *items = NULL;
    paint_ctx ctx;
    int ci, n = 0, cap = 0, i, ok = 0, fail = 0;

    cxd_path(pveh, sizeof pveh, game_dir, "/pveh", NULL);
    cxd_path(outdir, sizeof outdir, out_root, "/build/cars", NULL);
    if (cxd_mkdir_p(outdir) != 0) {
        fprintf(stderr, "[cx_cars_paint] cannot create %s\n", outdir);
        return 1;
    }
    if (cxd_list_dir(pveh, NULL, 1, &classes) != 0) {
        fprintf(stderr, "[cx_cars_paint] cannot list %s (set B3_GAME_DIR)\n",
                pveh);
        return 1;
    }

    /* THE FLATTEN.  Same order as the nested loops it replaces. */
    for (ci = 0; ci < classes.n; ci++) {
        char cdir[4096];
        cxd_names files;
        int fi;
        cxd_path(cdir, sizeof cdir, pveh, "/", classes.v[ci], NULL);
        if (cxd_list_dir(cdir, ".bgv", 0, &files) != 0)
            continue;
        for (fi = 0; fi < files.n; fi++) {
            const char *dot = strrchr(files.v[fi], '.');
            paint_item *t;
            if (n == cap) {
                int nc = cap ? cap * 2 : 128;
                paint_item *nv = (paint_item *)realloc(items,
                                                       (size_t)nc * sizeof *nv);
                if (!nv) { cxd_names_free(&files); goto oom; }
                items = nv;
                cap = nc;
            }
            t = &items[n++];
            memset(t, 0, sizeof *t);
            t->cls = classes.v[ci];
            snprintf(t->cdir, sizeof t->cdir, "%s", cdir);
            snprintf(t->file, sizeof t->file, "%s", files.v[fi]);
            snprintf(t->car, sizeof t->car, "%.*s",
                     dot ? (int)(dot - files.v[fi]) : (int)strlen(files.v[fi]),
                     files.v[fi]);
        }
        cxd_names_free(&files);
    }

    ctx.it = items;
    ctx.outdir = outdir;
    cx_pool_for(n, paint_one, &ctx);

    for (i = 0; i < n; i++) {
        if (items[i].line[0])
            fputs(items[i].line, stdout);
        if (items[i].ok) ok++; else fail++;
    }
oom:
    free(items);
    cxd_names_free(&classes);
    printf("\n%d cars textured, %d failed -> %s\n", ok, fail, outdir);
    return ok ? 0 : 1;
}
