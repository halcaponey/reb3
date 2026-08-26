/* cx_art_txd.c -- port of tools/extract_txd.py.
 *
 * Data/Frontend.txd and Data/Global.txd -> <out_root>/build/frontend/<name>.png.
 *
 * Despite the extension these are NOT RenderWare stream TXDs.  They are a flat
 * Criterion container (layout read off the shipped bytes and verified by full
 * extraction -- see cx_common_b.h, which already carries the walk):
 *
 *   +0x00 u32 0x543C0000   +0x04 u32 0xBCDEED81   +0x08 u32 count
 *   +0x0C u32 entrySize = 16
 *   +0x10 count x {u32 id (1-based, sequential), u32 0, u32 absOffset, u32 0}
 *
 * and each offset points at the SAME per-texture record static.dat and the
 * .bgv paints use (format credit: Burnout Modding community / EdnessP's
 * fmt_Burnout3LRD.py).  DXT1/DXT5 payloads are linear S3TC; paletted payloads
 * are Morton/Z-order swizzled exactly like the .bgv paints.  Records are
 * packed back-to-back, so there are no mip chains.
 *
 * =================================== THE V ORIGIN RULE, AND THE ALPHA ==
 *
 *   PNG row 0 = surface row 0 = v 0, for EVERY format, and the alpha channel
 *   is never touched except to move it.  The shipped bytes at +0x04 ARE the
 *   GPU surface, NV2A TX_FORMAT has no origin/flip field, and unswizzling a
 *   Morton-ordered paletted surface restores the same row-0-first linear
 *   image.  `hud_element01`, the in-race HUD plate, is the asymmetric proof:
 *   a 1-texel transparent border, a bright rim top and bottom, a 0.6-alpha
 *   fill between them and a left tip that slants OUTWARD as y increases --
 *   every one of which reverses under a vertical flip or an alpha inversion,
 *   and all of which are the numbers FUN_00048430 samples (v 0.03125..0.9375
 *   = texel rows 1..30).  selfcheck_orientation() asserts exactly that on
 *   every run that decodes the record and fails the run if it ever changes.
 *
 *   CONSEQUENCE FOR CONSUMERS: a HUD element that looks vertically flipped or
 *   alpha-inverted is a consumer bug, not an extractor bug.  See
 *   docs/RE_FRONTEND.md 6.10.                                            [C]
 *
 * PORT NOTE -- the dump-global entry cx_extract_txd() always writes the
 * _p1.._pN palette variants, i.e. the python's `--all-palettes` superset, so
 * that one call covers the tool's COMPLETE output set.  The standalone main
 * exposes the plain mode too.
 */
/* POSIX 2008 for strdup/strtok_r/getcwd/chdir/dirent under -std=c11,
 * which build.sh uses (it defines __STRICT_ANSI__). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cx_art_common.h"
#include "cx_extract.h"
#include "cx_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- *
 * THE V ORIGIN RULE / ALPHA self-check (see the file comment).
 * --------------------------------------------------------------------- */
#define ORIENT_TEX "hud_element01"
#define ORIENT_W   64
#define ORIENT_H   32

typedef struct {
    char bank[64];
    int  eid;
    char name[96];
    char why[256];
} Failure;

typedef struct {
    Failure *v;
    int      n, cap;
} FailList;

static void fail_add(FailList *fl, const char *bank, int eid, const char *name,
                     const char *why)
{
    if (fl->n >= fl->cap) {
        int cap = fl->cap ? fl->cap * 2 : 16;
        Failure *nv = (Failure *)realloc(fl->v, (size_t)cap * sizeof *nv);
        if (!nv)
            return;
        fl->v = nv;
        fl->cap = cap;
    }
    snprintf(fl->v[fl->n].bank, sizeof fl->v[fl->n].bank, "%s", bank);
    fl->v[fl->n].eid = eid;
    snprintf(fl->v[fl->n].name, sizeof fl->v[fl->n].name, "%s", name);
    snprintf(fl->v[fl->n].why, sizeof fl->v[fl->n].why, "%s", why);
    fl->n++;
}

/* ------------------------------------------------------------- taken set */
typedef struct {
    char **v;
    int    n, cap;
} NameSet;

static void lower_copy(char *dst, size_t cap, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < cap && src[i]; i++) {
        char c = src[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    dst[i] = 0;
}

static int set_has(const NameSet *s, const char *lower)
{
    int i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->v[i], lower) == 0)
            return 1;
    }
    return 0;
}

static void set_add(NameSet *s, const char *lower)
{
    char *d;
    if (s->n >= s->cap) {
        int cap = s->cap ? s->cap * 2 : 64;
        char **nv = (char **)realloc(s->v, (size_t)cap * sizeof *nv);
        if (!nv)
            return;
        s->v = nv;
        s->cap = cap;
    }
    d = strdup(lower);
    if (!d)
        return;
    s->v[s->n++] = d;
}

static void set_free(NameSet *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* ------------------------------------------------------------ statistics */
static unsigned char alpha_at(const unsigned char *rgba, int w, int x, int y)
{
    return rgba[((size_t)y * (size_t)w + (size_t)x) * 4u + 3u];
}

/* (mean |horizontal luma delta| over opaque runs, is_constant).  Random noise
 * scores ~85; real art (incl. DXT) stays well below 60.  Informational only:
 * legitimate hi-frequency art scores high too, so only structural errors ever
 * fail the run. */
static double image_stats(const unsigned char *rgba, int w, int h, int *is_const)
{
    long tot = 0, n = 0;
    int ystep = (h / 64) > 0 ? h / 64 : 1;
    int xstep = (w / 64) > 0 ? w / 64 : 1;
    int y, x, konst = 1;
    const unsigned char *first = rgba;

    for (y = 0; y < h; y += ystep) {
        size_t row = (size_t)y * (size_t)w * 4u;
        int have_prev = 0, prev = 0;
        for (x = 0; x < w; x += xstep) {
            const unsigned char *px = rgba + row + (size_t)x * 4u;
            int lum;
            if (memcmp(px, first, 4) != 0)
                konst = 0;
            if (px[3] < 8) { have_prev = 0; continue; }
            lum = (px[0] * 3 + px[1] * 6 + px[2]) / 10;
            if (have_prev) {
                tot += labs((long)lum - (long)prev);
                n++;
            }
            prev = lum;
            have_prev = 1;
        }
    }
    *is_const = konst;
    return n ? (double)tot / (double)n : 0.0;
}

/* decode_paletted()'s four failure strings.  cxb_decode_paletted() reports
 * only pass/fail, so re-run its three structural guards here to recover the
 * python's exact wording; a failure that passes all three can only be the
 * fourth ("palette index out of range").  None of the four is reachable on
 * the retail banks (409/409 decode). */
static const char *pal_why(const cxb_blob *b, size_t rec, int64_t bmp,
                           int w, int h, int bd, int which_pal)
{
    size_t npix = (size_t)w * (size_t)h;
    int ncol = (bd == 4) ? 16 : 256;
    int64_t prec, pdata;

    if (bmp < 0 || (uint64_t)bmp + npix > (uint64_t)b->n)
        return "pixels truncated";
    prec = (int64_t)rec
         + (int64_t)cxb_u32(b, rec + 0x14 + (size_t)which_pal * 4u);
    if (prec < 0 || (uint64_t)prec + 8u > (uint64_t)b->n)
        return "bad palette record";
    if (!(b->d[prec] == 0x01 && b->d[prec + 1] == 0x00
          && b->d[prec + 2] == 0x03
          && (b->d[prec + 3] == 0x00 || b->d[prec + 3] == 0xC0)))
        return "bad palette record";
    pdata = (int64_t)rec + (int64_t)cxb_u32(b, (size_t)prec + 4);
    if (pdata < 0 || (uint64_t)pdata + (uint64_t)ncol * 4u > (uint64_t)b->n)
        return "palette truncated";
    return "palette index out of range";
}

/* Returns the number of failures appended. */
static int selfcheck_orientation(const char *name, const unsigned char *rgba,
                                 int w, int h, FailList *fl, const char *bank,
                                 int eid, const char *label)
{
    char lower[96], why[256];
    int added = 0, y, fill, rim, first_edge = -1, last_edge = -1;

    lower_copy(lower, sizeof lower, name);
    if (strcmp(lower, ORIENT_TEX) != 0 || w != ORIENT_W || h != ORIENT_H)
        return 0;

    if (alpha_at(rgba, w, 40, 0) != 0 || alpha_at(rgba, w, 40, h - 1) != 0) {
        snprintf(why, sizeof why,
                 "V-ORIGIN/ALPHA self-check: row 0 / row %d are not the "
                 "transparent border (got %d / %d) -- flipped or "
                 "alpha-inverted", h - 1, alpha_at(rgba, w, 40, 0),
                 alpha_at(rgba, w, 40, h - 1));
        fail_add(fl, bank, eid, label, why);
        added++;
    }
    fill = alpha_at(rgba, w, 40, 16);
    if (!(fill >= 140 && fill <= 170)) {
        snprintf(why, sizeof why,
                 "V-ORIGIN/ALPHA self-check: mid-blade fill alpha %d, "
                 "expected ~153 (0.6) -- an inverted alpha channel reads ~102",
                 fill);
        fail_add(fl, bank, eid, label, why);
        added++;
    }
    rim = alpha_at(rgba, w, 40, 2);
    if (rim < 190) {
        snprintf(why, sizeof why,
                 "V-ORIGIN/ALPHA self-check: top rim alpha %d, expected >= 190",
                 rim);
        fail_add(fl, bank, eid, label, why);
        added++;
    }
    /* the left ink edge must march OUTWARD as y increases */
    for (y = 2; y < h - 2; y++) {
        int x, e = w;
        for (x = 0; x < w; x++) {
            if (alpha_at(rgba, w, x, y) > 32) { e = x; break; }
        }
        if (first_edge < 0)
            first_edge = e;
        last_edge = e;
    }
    if (!(first_edge > last_edge + 20)) {
        snprintf(why, sizeof why,
                 "V-ORIGIN/ALPHA self-check: left ink edge does not slant "
                 "outward downward (top %d, bottom %d) -- the image is "
                 "vertically flipped", first_edge, last_edge);
        fail_add(fl, bank, eid, label, why);
        added++;
    }
    return added;
}

/* ----------------------------------------------------------- one bank */
static int extract_bank(const char *path, const char *outdir, NameSet *taken,
                        int all_palettes, int verbose, FailList *fl,
                        uint32_t *count_out, unsigned *written_out)
{
    cxb_blob d = { NULL, 0 };
    uint32_t count, i;
    char bank[64], outpath[4352];
    unsigned char *rgba = NULL;
    size_t rgba_cap = 0;
    unsigned written = 0;
    int checked_orientation = 0;
    /* the printed table, held so the header can be emitted first */
    struct Row {
        int eid; char nm[160]; unsigned fmt; int w, h, bd, npal;
        double grad; const char *status;
    } *rows = NULL;
    int nrows = 0, rowcap = 0, k;

    {   /* bank = os.path.splitext(os.path.basename(path))[0] */
        char base[128], *dot;
        cxb_basename(path, base, sizeof base);
        dot = strrchr(base, '.');
        if (dot)
            *dot = 0;
        snprintf(bank, sizeof bank, "%.63s", base);
    }

    if (cxb_read_file(path, &d) != 0) {
        fprintf(stderr, "%s: cannot read\n", path);
        return -1;
    }
    if (cxb_txd_count(&d, &count) != 0) {
        fprintf(stderr, "%s: not a Burnout 3 txd container\n", path);
        cxb_blob_free(&d);
        return -1;
    }

    for (i = 0; i < count; i++) {
        int64_t rec, bmp;
        uint32_t fmt, w, h, bd;
        int npal, eid = (int)i + 1;
        char name[80], label[96], fname[160], lower[160], why[128];
        size_t need;
        int have_name;
        double grad;
        int konst;
        const char *status;

        if (cxb_txd_entry_offset(&d, i, &rec) != 0) {
            fail_add(fl, bank, eid, "?", "bad TOC entry");
            continue;
        }
        /* extract_txd.py adds the relative bitmap offset UNCONDITIONALLY --
         * this is not extract_textures.py's zero-preserving Reader.ptr(). */
        bmp = rec + (int64_t)cxb_i32(&d, (size_t)rec + 4);
        fmt = cxb_u32(&d, (size_t)rec + 0x34);
        w   = cxb_u32(&d, (size_t)rec + 0x38);
        h   = cxb_u32(&d, (size_t)rec + 0x3C);
        bd  = cxb_u32(&d, (size_t)rec + 0x40);
        npal = (int)cxb_u8(&d, (size_t)rec + 0x69);

        have_name = cxe_read_name(&d, rec + ((bd == 4u || bd == 8u || bd == 32u)
                                             ? 0x48 : 0x44),
                                  name, sizeof name) == 0;
        if (have_name)
            snprintf(label, sizeof label, "%s", name);
        else
            snprintf(label, sizeof label, "tex_%03d", eid);
        if (!have_name) {
            fail_add(fl, bank, eid, label, "unnamed");
            continue;
        }
        if (!(w > 0u && w <= 2048u && h > 0u && h <= 2048u)) {
            snprintf(why, sizeof why, "bad dims %ux%u", w, h);
            fail_add(fl, bank, eid, label, why);
            continue;
        }

        need = (size_t)w * (size_t)h * 4u;
        if (need > rgba_cap) {
            unsigned char *p = (unsigned char *)realloc(rgba, need);
            if (!p)
                goto oom;
            rgba = p;
            rgba_cap = need;
        }

        if (fmt == CXB_FMT_DXT1 || fmt == CXB_FMT_DXT5) {
            if (cxb_decode_dxt(&d, bmp, (int)w, (int)h, fmt, rgba) != 0) {
                fail_add(fl, bank, eid, label, "DXT data past EOF");
                continue;
            }
        } else if (fmt == CXB_FMT_P8) {
            if (cxb_decode_paletted(&d, (size_t)rec, bmp, (int)w, (int)h,
                                    (int)bd, 0, rgba) != 0) {
                fail_add(fl, bank, eid, label,
                         pal_why(&d, (size_t)rec, bmp, (int)w, (int)h,
                                 (int)bd, 0));
                continue;
            }
        } else {
            snprintf(why, sizeof why, "unhandled fmt 0x%X (%s)", fmt,
                     cxb_fmt_name(fmt));
            fail_add(fl, bank, eid, label, why);
            continue;
        }

        selfcheck_orientation(name, rgba, (int)w, (int)h, fl, bank, eid, label);
        grad = image_stats(rgba, (int)w, (int)h, &konst);
        status = "ok";
        if (grad > 60.0)
            status = "hi-freq";
        else if (konst)
            status = "flat";                    /* constant colour: legal */

        /* the single cross-bank name collision ("Takedown") gets a _<bank>
         * suffix on the later-processed bank */
        snprintf(fname, sizeof fname, "%s", name);
        lower_copy(lower, sizeof lower, fname);
        if (set_has(taken, lower))
            snprintf(fname, sizeof fname, "%s_%s", name, bank);
        lower_copy(lower, sizeof lower, fname);
        set_add(taken, lower);

        if ((size_t)snprintf(outpath, sizeof outpath, "%s/%s.png", outdir,
                             fname) >= sizeof outpath)
            goto oom;
        if (cx_png_write_rgba8(outpath, rgba, (int)w, (int)h) != 0) {
            fprintf(stderr, "cannot write %s\n", outpath);
            goto oom;
        }
        written++;

        if (fmt == CXB_FMT_P8 && all_palettes) {
            int p;
            for (p = 1; p < npal; p++) {
                if (cxb_decode_paletted(&d, (size_t)rec, bmp, (int)w, (int)h,
                                        (int)bd, p, rgba) != 0) {
                    char plabel[160];
                    snprintf(plabel, sizeof plabel, "%s_p%d", label, p);
                    fail_add(fl, bank, eid, plabel,
                             pal_why(&d, (size_t)rec, bmp, (int)w, (int)h,
                                     (int)bd, p));
                    continue;
                }
                if ((size_t)snprintf(outpath, sizeof outpath, "%s/%s_p%d.png",
                                     outdir, fname, p) >= sizeof outpath)
                    goto oom;
                if (cx_png_write_rgba8(outpath, rgba, (int)w, (int)h) != 0) {
                    fprintf(stderr, "cannot write %s\n", outpath);
                    goto oom;
                }
            }
        }

        lower_copy(lower, sizeof lower, name);
        if (strcmp(lower, ORIENT_TEX) == 0 && w == ORIENT_W && h == ORIENT_H)
            checked_orientation = 1;

        if (nrows >= rowcap) {
            int cap = rowcap ? rowcap * 2 : 128;
            void *nv = realloc(rows, (size_t)cap * sizeof *rows);
            if (!nv)
                goto oom;
            rows = nv;
            rowcap = cap;
        }
        rows[nrows].eid = eid;
        snprintf(rows[nrows].nm, sizeof rows[nrows].nm, "%s", fname);
        rows[nrows].fmt = fmt;
        rows[nrows].w = (int)w;
        rows[nrows].h = (int)h;
        rows[nrows].bd = (int)bd;
        rows[nrows].npal = npal;
        rows[nrows].grad = grad;
        rows[nrows].status = status;
        nrows++;
    }

    if (verbose) {
        printf("\n%s -- %u entries\n", path, count);
        if (checked_orientation)
            printf("  V-ORIGIN/ALPHA self-check: %s decodes with the "
                   "recovered row order and alpha profile [ok]\n", ORIENT_TEX);
        printf("%4s %-26s %-5s %9s %3s %4s %6s %s\n",
               "id", "name", "fmt", "dims", "bd", "npal", "grad", "status");
        for (k = 0; k < nrows; k++)
            printf("%4d %-26s %-5s %4dx%-4d %3d %4d %6.1f %s\n",
                   rows[k].eid, rows[k].nm, cxb_fmt_name(rows[k].fmt),
                   rows[k].w, rows[k].h, rows[k].bd, rows[k].npal,
                   rows[k].grad, rows[k].status);
    }

    free(rows);
    free(rgba);
    cxb_blob_free(&d);
    *count_out = count;
    *written_out = written;
    return 0;

oom:
    free(rows);
    free(rgba);
    cxb_blob_free(&d);
    return -1;
}

/* ------------------------------------------------------------ the driver */
int cx_extract_txd_banks(const char *const *inputs, int ninputs,
                         const char *outdir, int all_palettes, int verbose)
{
    NameSet taken = { NULL, 0, 0 };
    FailList fl = { NULL, 0, 0 };
    uint32_t total = 0;
    unsigned wrote = 0;
    int i, rc;

    if (cxb_mkdir_p(outdir) != 0) {
        fprintf(stderr, "cannot create %s\n", outdir);
        return 1;
    }
    /* THE DEFLATE, OFF THE CRITICAL PATH.  This stage was 1.54 s wall of
     * which 1.51 s was user CPU, 808 images, one core.  The loop below cannot
     * itself be parallelised -- extract_bank() resolves the cross-bank name
     * collision ("Takedown") in VISIT ORDER against `taken`, and appends to
     * `fl` -- so the encode moves instead: every cx_png_write_rgba8() inside
     * is deferred, encoded on the pool at the flush, and written out in queue
     * order.  Not one line of the walk changes.  See THE QUEUE in cx_png.h. */
    cx_png_queue_begin();
    for (i = 0; i < ninputs; i++) {
        uint32_t count = 0;
        unsigned written = 0;
        if (extract_bank(inputs[i], outdir, &taken, all_palettes, verbose,
                         &fl, &count, &written) != 0) {
            cx_png_queue_flush();
            set_free(&taken);
            free(fl.v);
            return 1;
        }
        total += count;
        wrote += written;
    }
    if (cx_png_queue_flush() != 0)
        fprintf(stderr, "[cx_art_txd] one or more PNGs failed to write\n");

    printf("\ntotal: %u/%u textures -> %s\n", wrote, total, outdir);
    if (fl.n) {
        printf("FAILURES (%d):\n", fl.n);
        for (i = 0; i < fl.n; i++)
            printf("  %s [%3d] %-24s %s\n", fl.v[i].bank, fl.v[i].eid,
                   fl.v[i].name, fl.v[i].why);
    }
    rc = fl.n ? 1 : 0;
    set_free(&taken);
    free(fl.v);
    return rc;
}

int cx_extract_txd(const char *game_dir, const char *out_root)
{
    const char *game = cxe_game_dir(game_dir);
    char fe[4352], gl[4352], outdir[4096];
    const char *inputs[2];

    if ((size_t)snprintf(fe, sizeof fe, "%s/Data/Frontend.txd", game) >= sizeof fe)
        return 1;
    if ((size_t)snprintf(gl, sizeof gl, "%s/Data/Global.txd", game) >= sizeof gl)
        return 1;
    if (cxe_out_dir(outdir, sizeof outdir, out_root, "build/frontend") != 0) {
        fprintf(stderr, "[cx_art_txd] cannot create %s/build/frontend\n",
                out_root);
        return 1;
    }
    inputs[0] = fe;
    inputs[1] = gl;
    return cx_extract_txd_banks(inputs, 2, outdir, 1, 1);
}
