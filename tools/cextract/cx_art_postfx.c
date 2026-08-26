/* cx_art_postfx.c -- port of tools/extract_postfx_art.py.
 *
 * The world post-FX art: the per-track SKY textures out of
 * Tracks/<region>/<track>/enviro.dat -> <out_root>/build/postfx/.
 *
 * WHY THIS EXISTS: the sky is not in static.dat at all -- NO material in
 * either US/C3_V1 (167 records) or AS/C1_V1 (180 records) names a sky/cloud
 * texture.  It is drawn from a procedurally generated dome (FUN_00032020)
 * textured with the images in enviro.dat (FUN_001888f0 @0x001889D5 builds the
 * "<track dir>/enviro.dat" path and loads it).  See docs/RE_POSTFX.md.
 *
 * FORMAT: enviro.dat is a baked memory image whose internal pointers are
 * FILE-RELATIVE offsets the loader relocates.  [C] FUN_00188880 relocates
 * four of them and hands each to the texture registrar FUN_001C8E20:
 *
 *     env+0x98 -> texture 0   [C] 0x00188893      env+0xA0 -> 2  [C] 0x001888BF
 *     env+0x9C -> texture 1   [C] 0x001888A9      env+0xA4 -> 3  [C] 0x001888D5
 *
 * and FUN_001888F0's tail publishes three of them to the sky draw
 * (DAT_0045BC10 := env+0x98, DAT_0045D11C := env+0x9C, DAT_0045D118 :=
 * env+0xA0).  Each points at an ordinary Burnout texture record, so
 * cxb_parse_texrec() -- the port of the python's parse_record(), same guards
 * -- reads them all.  There is no texture TABLE in enviro.dat, so the four
 * record offsets come straight out of the header; `--scan` brute-forces every
 * 4-byte-aligned position as an independent cross-check.
 *
 * The four slots are ROLE-fixed across all shipped tracks even though the
 * artists' texture NAMES are not, so output files are named by ROLE:
 * +0x98 gradients (32x32 DXT5 everywhere), +0x9C clouds (1024-wide DXT5),
 * +0xA0 envmapclouds (absent on 4 tracks), +0xA4 suncorona (128x256
 * paletted -- its palette lives OUTSIDE the record, so it is deliberately not
 * decoded, exactly as the python leaves it).
 *
 * [C] THE SUN VECTOR, env+0x80: FUN_001888F0 @0x00188B40..0x00188B95 copies
 * it into both per-screen 64-byte blocks at env+0xB0 / env+0xF0, and
 * FUN_001891F0 @0x00189243 reads env + 0xB0 + 0x40*screen and hands its first
 * three floats to FUN_00189660, which turns them into the (azimuth,
 * elevation) the sun-glow LUT pass is centred on.  On all shipped tracks it is
 * a UNIT vector with a NEGATIVE y -- the direction the light TRAVELS -- and is
 * written out here exactly as stored.
 *
 * PORT NOTE -- the manifest records os.path.relpath() of each output, i.e.
 * paths relative to the PROCESS CWD.  cxe_relpath() reproduces
 * posixpath.relpath() exactly, so running the C with cwd == out_root
 * reproduces the python run with cwd == the repo root byte for byte.
 */
#include "cx_art_common.h"
#include "cx_extract.h"
#include "cx_png.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* [C] FUN_00188880 -- the four relocated texture-pointer fields. */
static const int ENV_TEX_FIELDS[4] = { 0x98, 0x9C, 0xA0, 0xA4 };
static const char *const SLOT_ROLE[4] = {
    "gradients",      /* the 32x32 sky-gradient LUT source     */
    "clouds",         /* the sky-dome cloud panorama           */
    "envmapclouds",   /* the reflection/env-map cloud sheet    */
    "suncorona"       /* the sun sprite (paletted)             */
};

#define ENV_SUN_DIR 0x80

/* [C] THE SUN COLOUR, env+0x60 == the environment object's +0x60 ==
 * DAT_0060E0A0 == pixel-shader c14.xyz, the car shader's light colour
 * (docs/RE_CARFX.md 2.8).  It is inside the same 0xB0-byte record FUN_00188C00
 * copies wholesale at 0x00188A40, so it is a field of THIS file -- which is
 * why it belongs in the sidecar and not in a table in src/burnout3_carfx.c,
 * whose b3fx_env_light_from_sidecar() reads the `light_rgb` line back.  Every
 * shipped track authors it as an exact 8-bit colour.                       */
#define ENV_LIGHT_RGB 0x60

/* python's round(): nearest, TIES TO EVEN -- which is what the default
 * FE_TONEAREST rounding mode gives rint() on the same double.  (int)(x+0.5)
 * would round 0.5 the other way and is not the oracle's behaviour. */
static int py_round(double x)
{
    return (int)rint(x);
}

/* ------------------------------------------------------- text accumulator */
typedef struct {
    char  *s;
    size_t n, cap;
} Buf;

static int buf_addf(Buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int need;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { va_end(ap2); return -1; }
    if (b->n + (size_t)need + 1u > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        char *p;
        while (cap < b->n + (size_t)need + 1u)
            cap *= 2;
        p = (char *)realloc(b->s, cap);
        if (!p) { va_end(ap2); return -1; }
        b->s = p;
        b->cap = cap;
    }
    vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)need;
    return 0;
}

/* python's "\n".join(lines): a separator BEFORE every line but the first. */
static int buf_line(Buf *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[2048];

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return buf_addf(b, "%s%s", b->n ? "\n" : "", tmp);
}

static void buf_free(Buf *b)
{
    free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
}

/* TexRecord.__repr__ */
static void rec_repr(const cxb_texrec *t, char *out, size_t cap)
{
    snprintf(out, cap, "0x%06X %-14s %4dx%-4d %-9s data@0x%06X",
             (unsigned)t->rec, t->name, t->w, t->h, cxb_fmt_name(t->fmt),
             (unsigned)t->data_off);
}

/* ------------------------------------------------------------- one track */
/* Returns 0 and fills `text` when the track has an enviro.dat, 1 when it does
 * not (python's `return None`), -1 on a hard error. */
static int extract_track(const char *tracks_root, const char *track,
                         const char *outdir, int do_scan, Buf *text,
                         int *written_out)
{
    char path[4608], tag[128], out[4864], rel[4864], repr[256];
    cxb_blob d = { NULL, 0 };
    cxb_texrec hdr[4];
    int hdr_field[4];
    int nhdr = 0, i, written = 0, rc = -1;
    float sx, sy, sz, lr, lg, lb;
    double sn;
    FILE *fp;
    unsigned char *rgba = NULL;

    if ((size_t)snprintf(path, sizeof path, "%s/%s/enviro.dat", tracks_root,
                         track) >= sizeof path)
        return 1;
    if (!cxb_file_exists(path))
        return 1;
    if (cxb_read_file(path, &d) != 0)
        return 1;

    buf_line(text, "%s  enviro.dat  %zu bytes", track, d.n);
    for (i = 0; i < 4; i++) {
        int32_t off = cxb_i32(&d, (size_t)ENV_TEX_FIELDS[i]);
        if (off == 0)
            continue;
        if (cxb_parse_texrec(&d, off, &hdr[nhdr]) != 0)
            continue;
        hdr_field[nhdr] = i;
        rec_repr(&hdr[nhdr], repr, sizeof repr);
        buf_line(text, "  env+0x%02X %-13s -> %s", ENV_TEX_FIELDS[i],
                 SLOT_ROLE[i], repr);
        nhdr++;
    }

    if (do_scan) {
        /* independent cross-check: brute-force every 4-byte-aligned position,
         * keeping the FIRST record for each distinct name */
        cxb_texrec *sc = NULL;
        int nsc = 0, cap = 0, same = 1, j;
        size_t rec;

        if (d.n > 0x70u) {
            for (rec = 0; rec + 0x70u <= d.n; rec += 4) {
                cxb_texrec t;
                int dup = 0;
                if (cxb_parse_texrec(&d, (int64_t)rec, &t) != 0)
                    continue;
                for (j = 0; j < nsc; j++) {
                    if (strcmp(sc[j].name, t.name) == 0) { dup = 1; break; }
                }
                if (dup)
                    continue;
                if (nsc >= cap) {
                    int nc = cap ? cap * 2 : 32;
                    void *nv = realloc(sc, (size_t)nc * sizeof *sc);
                    if (!nv) break;
                    sc = nv;
                    cap = nc;
                }
                sc[nsc++] = t;
            }
        }
        buf_line(text, "  --scan found %d record(s):", nsc);
        for (j = 0; j < nsc; j++) {
            rec_repr(&sc[j], repr, sizeof repr);
            buf_line(text, "      %s", repr);
        }
        /* set(hdr recs) == set(scan recs) */
        {
            int k;
            for (i = 0; i < nhdr && same; i++) {
                int hit = 0;
                for (k = 0; k < nsc; k++)
                    if (sc[k].rec == hdr[i].rec) { hit = 1; break; }
                if (!hit) same = 0;
            }
            for (k = 0; k < nsc && same; k++) {
                int hit = 0;
                for (i = 0; i < nhdr; i++)
                    if (sc[k].rec == hdr[i].rec) { hit = 1; break; }
                if (!hit) same = 0;
            }
        }
        buf_line(text, "  scan == header: %s", same ? "True" : "False");
        free(sc);
    }

    if (cxb_mkdir_p(outdir) != 0)
        goto done;
    {   /* tag = track.replace('/', '_') */
        size_t k;
        if ((size_t)snprintf(tag, sizeof tag, "%s", track) >= sizeof tag)
            goto done;
        for (k = 0; tag[k]; k++)
            if (tag[k] == '/')
                tag[k] = '_';
    }

    /* the per-track environment sidecar the runtime reads */
    sx = cxb_f32(&d, ENV_SUN_DIR);
    sy = cxb_f32(&d, ENV_SUN_DIR + 4);
    sz = cxb_f32(&d, ENV_SUN_DIR + 8);
    /* python: (x*x + y*y + z*z) ** 0.5 -- the same libm pow() call */
    sn = pow((double)sx * sx + (double)sy * sy + (double)sz * sz, 0.5);
    lr = cxb_f32(&d, ENV_LIGHT_RGB);
    lg = cxb_f32(&d, ENV_LIGHT_RGB + 4);
    lb = cxb_f32(&d, ENV_LIGHT_RGB + 8);

    if ((size_t)snprintf(out, sizeof out, "%s/%s_env.txt", outdir, tag)
        >= sizeof out)
        goto done;
    fp = fopen(out, "w");
    if (!fp)
        goto done;
    fprintf(fp, "# enviro.dat %s -- see docs/RE_POSTFX.md 3.4\n", track);
    fprintf(fp, "# env+0x80: the SUN vector as stored (direction of travel,\n");
    fprintf(fp, "#           y < 0); burnout3_postfx.c negates it.\n");
    fprintf(fp, "sun_dir %.8f %.8f %.8f\n", (double)sx, (double)sy, (double)sz);
    fprintf(fp, "sun_len %.8f\n", sn);
    fprintf(fp, "# env+0x60: the environment object's +0x60 == DAT_0060E0A0\n");
    fprintf(fp, "#           == pixel-shader c14.xyz, the track's sun\n");
    fprintf(fp, "#           colour, which src/burnout3_carfx.c reads from\n");
    fprintf(fp, "#           here (%d,%d,%d as authored).\n",
            py_round((double)lr * 255.0), py_round((double)lg * 255.0),
            py_round((double)lb * 255.0));
    fprintf(fp, "light_rgb %.8f %.8f %.8f\n", (double)lr, (double)lg,
            (double)lb);
    fprintf(fp, "# The 13th glow quad's source v span is DAT_0060E1C0/C4 =\n");
    fprintf(fp, "# env+0x180/0x184 -- a .bss pair with no initialiser found,\n");
    fprintf(fp, "# i.e. 0.0; it is a runtime field, NOT a field of this file.\n");
    fclose(fp);
    if (cxe_relpath(out, rel, sizeof rel) != 0)
        snprintf(rel, sizeof rel, "%s", out);
    buf_line(text, "  env+0x80 sun (%.4f, %.4f, %.4f) |v|=%.4f -> %s",
             (double)sx, (double)sy, (double)sz, sn, rel);
    buf_line(text, "  env+0x60 light (%.4f, %.4f, %.4f) = %d,%d,%d -> the same",
             (double)lr, (double)lg, (double)lb,
             py_round((double)lr * 255.0), py_round((double)lg * 255.0),
             py_round((double)lb * 255.0));

    for (i = 0; i < nhdr; i++) {
        const cxb_texrec *t = &hdr[i];
        const char *role = SLOT_ROLE[hdr_field[i]];

        if (t->fmt == CXB_FMT_DXT1 || t->fmt == CXB_FMT_DXT3
            || t->fmt == CXB_FMT_DXT5) {
            size_t need = (size_t)t->w * (size_t)t->h * 4u;
            unsigned char *p = (unsigned char *)realloc(rgba, need);
            unsigned use;
            if (!p)
                goto done;
            rgba = p;
            /* LATENT QUIRK, preserved: extract_postfx_art.py's decode_dxt()
             * has no DXT3 branch (`step = 8 if fmt == 0xC else 16`, else arm
             * runs the DXT5 alpha block), so a 0xE record decodes AS DXT5.
             * No shipped enviro.dat carries one, so the quirk is latent. */
            use = (t->fmt == CXB_FMT_DXT1) ? CXB_FMT_DXT1 : CXB_FMT_DXT5;
            if (cxb_decode_dxt(&d, t->data_off, t->w, t->h, use, rgba) != 0)
                continue;                          /* python: `if px:` */
            if ((size_t)snprintf(out, sizeof out, "%s/%s_%s.png", outdir, tag,
                                 role) >= sizeof out)
                goto done;
            if (cx_png_write_rgba8(out, rgba, t->w, t->h) != 0)
                goto done;
            written++;
            if (cxe_relpath(out, rel, sizeof rel) != 0)
                snprintf(rel, sizeof rel, "%s", out);
            buf_line(text, "  wrote %s", rel);
        } else if (t->fmt == CXB_FMT_P8) {
            buf_line(text, "  %s is PALETTED (0xB) -- palette lives outside "
                           "the record; not decoded here [?]", t->name);
        } else {
            buf_line(text, "  %s format 0x%X not decoded", t->name, t->fmt);
        }
    }
    *written_out = written;
    rc = 0;

done:
    free(rgba);
    cxb_blob_free(&d);
    return rc;
}

/* ------------------------------------------------------------ the driver */
int cx_extract_postfx_art_ex(const char *game_dir, const char *out_root,
                             const char *only_track, int do_scan)
{
    const char *game = cxe_game_dir(game_dir);
    char tracks_root[4352], outdir[4096], manifest_path[4352];
    cxe_names regions = { NULL, 0 };
    Buf manifest = { NULL, 0, 0 };
    int total = 0, ri, rc = 1;
    FILE *fp;

    if ((size_t)snprintf(tracks_root, sizeof tracks_root, "%s/Tracks", game)
        >= sizeof tracks_root)
        return 1;
    if (cxe_out_dir(outdir, sizeof outdir, out_root, "build/postfx") != 0) {
        fprintf(stderr, "[cx_art_postfx] cannot create %s/build/postfx\n",
                out_root);
        return 1;
    }

    /* 1.61 s wall, 1.57 s of it user CPU, for 145 files -- the DXT decode and
     * the deflate behind them.  The WALK has to stay serial: enviro_manifest
     * .txt is the per-track text blocks concatenated in walk order, and the
     * whole point of the file is that order.  So the encode is deferred to
     * the pool instead and the walk is untouched (THE QUEUE, cx_png.h). */
    cx_png_queue_begin();

    if (only_track) {
        Buf text = { NULL, 0, 0 };
        int written = 0;
        int r = extract_track(tracks_root, only_track, outdir, do_scan, &text,
                              &written);
        if (r == 1) {
            printf("no enviro.dat for %s\n", only_track);
        } else if (r == 0) {
            total += written;
            printf("%s\n", text.s ? text.s : "");
            buf_addf(&manifest, "%s%s", manifest.n ? "\n" : "",
                     text.s ? text.s : "");
        } else {
            buf_free(&text);
            goto done;
        }
        buf_free(&text);
    } else {
        if (cxe_listdir_sorted(tracks_root, &regions) != 0) {
            fprintf(stderr, "[cx_art_postfx] cannot list %s\n", tracks_root);
            goto done;
        }
        for (ri = 0; ri < regions.n; ri++) {
            char rd[4480];
            cxe_names trks = { NULL, 0 };
            int ti;

            if ((size_t)snprintf(rd, sizeof rd, "%s/%s", tracks_root,
                                 regions.v[ri]) >= sizeof rd)
                continue;
            if (!cxe_is_dir(rd))
                continue;
            if (cxe_listdir_sorted(rd, &trks) != 0)
                continue;
            for (ti = 0; ti < trks.n; ti++) {
                char id[256], probe[4736];
                Buf text = { NULL, 0, 0 };
                int written = 0, r;

                if ((size_t)snprintf(probe, sizeof probe, "%s/%s/enviro.dat",
                                     rd, trks.v[ti]) >= sizeof probe)
                    continue;
                if (!cxb_file_exists(probe))
                    continue;
                if ((size_t)snprintf(id, sizeof id, "%s/%s", regions.v[ri],
                                     trks.v[ti]) >= sizeof id)
                    continue;
                r = extract_track(tracks_root, id, outdir, do_scan, &text,
                                  &written);
                if (r == 0) {
                    total += written;
                    printf("%s\n", text.s ? text.s : "");
                    buf_addf(&manifest, "%s%s", manifest.n ? "\n" : "",
                             text.s ? text.s : "");
                } else if (r == 1) {
                    printf("no enviro.dat for %s\n", id);
                }
                buf_free(&text);
            }
            cxe_names_free(&trks);
        }
    }

    if (cx_png_queue_flush() != 0)
        fprintf(stderr, "[cx_art_postfx] one or more PNGs failed to write\n");

    if ((size_t)snprintf(manifest_path, sizeof manifest_path,
                         "%s/enviro_manifest.txt", outdir)
        >= sizeof manifest_path)
        goto done;
    fp = fopen(manifest_path, "w");
    if (!fp) {
        fprintf(stderr, "[cx_art_postfx] cannot write %s\n", manifest_path);
        goto done;
    }
    fprintf(fp, "%s\n", manifest.s ? manifest.s : "");
    fclose(fp);
    printf("\n%d PNG(s) -> %s\n", total, outdir);
    rc = 0;

done:
    /* Every `goto done` above jumps past the flush; a second one is a no-op
     * on an already-drained queue and is what closes it on the failure arms. */
    cx_png_queue_flush();
    buf_free(&manifest);
    cxe_names_free(&regions);
    return rc;
}

int cx_extract_postfx_art(const char *game_dir, const char *out_root)
{
    /* the dump-global entry covers the tool's COMPLETE output set, i.e. the
     * python's `--all` (every track that ships an enviro.dat). */
    return cx_extract_postfx_art_ex(game_dir, out_root, NULL, 0);
}
