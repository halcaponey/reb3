/* cx_art_common.c -- see cx_art_common.h for provenance and the output-root
 * convention.  Nothing in this file invents format knowledge: the decoders,
 * the record layout and the container header all come from cx_common_b.[ch],
 * which the per-track port already validated against these very banks. */
/* POSIX 2008 for strdup/strtok_r/getcwd/chdir/dirent under -std=c11,
 * which build.sh uses (it defines __STRICT_ANSI__). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cx_art_common.h"
#include "cx_png.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* What tools/extract_txd.py, extract_carfx_art.py and extract_postfx_art.py
 * resolve as GAME. */
/* NO RETAIL PATH IS COMPILED IN.  This tree ships no game content: point
 * $B3_GAME_ROOT at YOUR OWN dump -- the folder holding default.xbe, or an
 * .xiso image of it.  $B3_ISO and $B3_GAME_DIR name the same source and are
 * checked first.  See docs/ASSETS.md. */
#define CXE_DEFAULT_GAME ""

const char *cxe_game_dir(const char *game_dir)
{
    const char *e;

    if (game_dir && game_dir[0])
        return game_dir;
    /* $B3_ISO first: it names the Xbox image, which cx_src/cx_vfs serve
     * behind the same "%s/Data/..." joins an expanded dump takes. */
    e = getenv(CX_SRC_ENV_ISO);
    if (e && e[0])
        return e;
    e = getenv("B3_GAME_DIR");
    if (e && e[0])
        return e;
    e = getenv("B3_GAME_ROOT");
    if (e && e[0])
        return e;
    return CXE_DEFAULT_GAME;
}

/* ------------------------------------------------------------ path utils */
int cxe_join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int cxe_out_dir(char *out, size_t cap, const char *root, const char *rel)
{
    if (cxe_join(out, cap, root, rel) != 0)
        return -1;
    return cxb_mkdir_p(out);
}

/* posixpath.abspath() + normpath(), split into components: collapse '//',
 * drop '.', pop on '..' (never above the root, exactly as normpath does).
 * `buf` is tokenised in place and must outlive `parts`. */
#define CXE_MAX_COMP 256

static int norm_components(char *buf, char **parts, int cap)
{
    int np = 0;
    char *p, *save;

    for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (strcmp(p, ".") == 0)
            continue;
        if (strcmp(p, "..") == 0) {
            if (np > 0)
                np--;
            continue;
        }
        if (np >= cap)
            return -1;
        parts[np++] = p;
    }
    return np;
}

int cxe_relpath(const char *path, char *out, size_t cap)
{
    char cwd[4096], bufp[8192], bufs[8192];
    char *pp[CXE_MAX_COMP], *sp[CXE_MAX_COMP];
    int npp, nsp, i, k;
    size_t used = 0;

    if (!getcwd(cwd, sizeof cwd))
        return -1;
    if (path[0] == '/') {
        if ((size_t)snprintf(bufp, sizeof bufp, "%s", path) >= sizeof bufp)
            return -1;
    } else if ((size_t)snprintf(bufp, sizeof bufp, "%s/%s", cwd, path)
               >= sizeof bufp) {
        return -1;
    }
    if ((size_t)snprintf(bufs, sizeof bufs, "%s", cwd) >= sizeof bufs)
        return -1;

    npp = norm_components(bufp, pp, CXE_MAX_COMP);
    nsp = norm_components(bufs, sp, CXE_MAX_COMP);
    if (npp < 0 || nsp < 0)
        return -1;

    for (i = 0; i < npp && i < nsp; i++) {
        if (strcmp(pp[i], sp[i]) != 0)
            break;
    }
    out[0] = 0;
    for (k = i; k < nsp; k++) {
        int n = snprintf(out + used, cap - used, "%s..", used ? "/" : "");
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }
    for (k = i; k < npp; k++) {
        int n = snprintf(out + used, cap - used, "%s%s", used ? "/" : "",
                         pp[k]);
        if (n < 0 || (size_t)n >= cap - used)
            return -1;
        used += (size_t)n;
    }
    if (used == 0)                                    /* rel_list empty */
        snprintf(out, cap, ".");
    return 0;
}

double cxe_py_round(double x)
{
    /* CPython floatobject.c float___round___impl with ndigits None. */
    double r = round(x);
    if (fabs(x - r) == 0.5)
        r = 2.0 * round(x / 2.0);
    return r;
}

/* ----------------------------------------------------- sorted directories */
static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int cxe_listdir_sorted(const char *dir, cxe_names *out)
{
    CxSrcList l;
    char **v = NULL;
    int n = 0, cap = 0, i;

    out->v = NULL;
    out->n = 0;
    /* cx_vfs_listdir() already omits "." and "..", exactly as os.listdir()
     * does, and hands back source order -- which the sort below erases. */
    if (cx_vfs_listdir(dir, &l) != 0)
        return -1;
    for (i = 0; i < l.n; i++) {
        char *s;
        if (n >= cap) {
            char **nv;
            cap = cap ? cap * 2 : 32;
            nv = (char **)realloc(v, (size_t)cap * sizeof *v);
            if (!nv) { cx_src_list_free(&l); goto oom; }
            v = nv;
        }
        s = strdup(l.name[i]);
        if (!s) { cx_src_list_free(&l); goto oom; }
        v[n++] = s;
    }
    cx_src_list_free(&l);
    if (n > 1)
        qsort(v, (size_t)n, sizeof *v, cmp_name);
    out->v = v;
    out->n = n;
    return 0;

oom:
    while (n > 0)
        free(v[--n]);
    free(v);
    return -1;
}

void cxe_names_free(cxe_names *ns)
{
    int i;
    for (i = 0; i < ns->n; i++)
        free(ns->v[i]);
    free(ns->v);
    ns->v = NULL;
    ns->n = 0;
}

int cxe_is_dir(const char *path)
{
    return cx_vfs_is_dir(path);
}

int cxe_is_file(const char *path)
{
    return cx_vfs_is_file(path);
}

/* --------------------------------------------------------------- records */
int cxe_read_name(const cxb_blob *b, int64_t off, char *out, size_t cap)
{
    char tmp[80];
    const char *p;

    /* cxb_read_cstr already reproduces data.find(b'\0'), the 64-char limit
     * and bytes.decode('ascii'); the art tools then require non-empty and
     * every character PRINTABLE (32..126). */
    if (cxb_read_cstr(b, off, 64, tmp, sizeof tmp) != 0)
        return -1;
    if (!tmp[0])
        return -1;
    for (p = tmp; *p; p++) {
        if ((unsigned char)*p < 32u || (unsigned char)*p >= 127u)
            return -1;
    }
    if (strlen(tmp) >= cap)
        return -1;
    memcpy(out, tmp, strlen(tmp) + 1);
    return 0;
}

void cxe_rec_free(cxe_rec *r)
{
    free(r->rgba);
    r->rgba = NULL;
}

int cxe_decode_record(const cxb_blob *b, int64_t rec, cxe_rec *out)
{
    int64_t bmp;
    unsigned fmt;
    int w, h, bd;
    unsigned char *rgba;
    size_t need;

    out->rgba = NULL;
    bmp = rec + (int64_t)cxb_i32(b, (size_t)rec + 0x04);
    fmt = cxb_u32(b, (size_t)rec + 0x34);
    w   = (int)cxb_u32(b, (size_t)rec + 0x38);
    h   = (int)cxb_u32(b, (size_t)rec + 0x3C);
    bd  = (int)cxb_u32(b, (size_t)rec + 0x40);
    if (cxe_read_name(b, rec + ((bd == 4 || bd == 8 || bd == 32) ? 0x48 : 0x44),
                      out->name, sizeof out->name) != 0)
        return -1;                                    /* python: None */
    if (fmt != CXB_FMT_DXT1 && fmt != CXB_FMT_DXT3 && fmt != CXB_FMT_DXT5
        && fmt != CXB_FMT_P8)
        return -1;
    /* The python does NO dimension validation here (the callers filter by
     * name), but a bogus record must not be able to ask for a terabyte. */
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return -1;
    need = (size_t)w * (size_t)h * 4u;
    rgba = (unsigned char *)malloc(need);
    if (!rgba)
        return -1;
    if (fmt == CXB_FMT_P8) {
        if (cxb_decode_paletted(b, (size_t)rec, bmp, w, h, bd, 0, rgba) != 0) {
            free(rgba);
            return -1;
        }
    } else {
        /* LATENT QUIRK, preserved: extract_textures.py's decode_dxt() has no
         * DXT3 branch -- `step = 8 if fmt == 0xC else 16` and the else arm
         * runs the DXT5 alpha block -- so a 0xE record is decoded AS DXT5.
         * extract_postfx_art.py's private copy of that decoder does the same.
         * Passing DXT5 here reproduces it bit for bit.  (No 0xE record exists
         * in Frontend.txd, Global.txd or any shipped enviro.dat, so the quirk
         * is latent on the retail data set.) */
        unsigned use = (fmt == CXB_FMT_DXT1) ? CXB_FMT_DXT1 : CXB_FMT_DXT5;
        if (cxb_decode_dxt(b, bmp, w, h, use, rgba) != 0) {
            /* python returns (name, w, h, None) here and the caller then
             * raises inside Image.frombytes; treat it as "no record". */
            free(rgba);
            return -1;
        }
    }
    out->w = w;
    out->h = h;
    out->fmt = fmt;
    out->depth = bd;
    out->rgba = rgba;
    return 0;
}

/* --------------------------------------------------------- the txd banks */
int cxe_txd_open(const char *path, cxb_blob *b, uint32_t *count)
{
    if (cxb_read_file(path, b) != 0)
        return -1;
    if (cxb_txd_count(b, count) != 0) {
        cxb_blob_free(b);
        return -1;
    }
    return 0;
}

int cxe_txd_offset_loose(const cxb_blob *b, uint32_t i, int64_t *off)
{
    uint32_t o = cxb_u32(b, 0x10u + (size_t)i * 16u + 8u);

    if (b->n < 0x80u)
        return -1;
    if (!(o > 0x10u && (uint64_t)o < (uint64_t)b->n - 0x80u))
        return -1;
    *off = (int64_t)o;
    return 0;
}

/* PIL: img.getchannel('A').getextrema() and img.convert('RGB').getextrema().
 * RGBA -> RGB in PIL simply drops the alpha byte, no premultiply. */
static void extrema(const unsigned char *rgba, int w, int h, int ch,
                    int *lo, int *hi)
{
    size_t i, n = (size_t)w * (size_t)h;
    int mn = 255, mx = 0;

    for (i = 0; i < n; i++) {
        int v = rgba[i * 4 + (size_t)ch];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    *lo = mn;
    *hi = mx;
}

int cxe_pull_named(const char *txd_path, const char *outdir,
                   const char *const *wanted, int nwanted,
                   const char *const *required, int nrequired,
                   cxe_report_style style)
{
    cxb_blob d = { NULL, 0 };
    uint32_t count, i;
    int k, rc = 0;
    /* per wanted name: found?  plus the reported stats */
    int   *found;
    int   *gw, *gh, *alo, *ahi, *rlo, *rhi, *glo, *ghi, *blo, *bhi;
    char   path[4352];

    if (cxe_txd_open(txd_path, &d, &count) != 0) {
        fprintf(stderr, "%s: not a Burnout 3 txd container\n", txd_path);
        return 1;
    }
    if (cxb_mkdir_p(outdir) != 0) {
        cxb_blob_free(&d);
        fprintf(stderr, "cannot create %s\n", outdir);
        return 1;
    }
    found = (int *)calloc((size_t)nwanted * 11u, sizeof(int));
    if (!found) { cxb_blob_free(&d); return 1; }
    gw  = found + nwanted;      gh  = found + nwanted * 2;
    alo = found + nwanted * 3;  ahi = found + nwanted * 4;
    rlo = found + nwanted * 5;  rhi = found + nwanted * 6;
    glo = found + nwanted * 7;  ghi = found + nwanted * 8;
    blo = found + nwanted * 9;  bhi = found + nwanted * 10;

    for (i = 0; i < count; i++) {
        int64_t off;
        cxe_rec r;

        if (cxe_txd_offset_loose(&d, i, &off) != 0)
            continue;
        if (cxe_decode_record(&d, off, &r) != 0)
            continue;
        for (k = 0; k < nwanted; k++) {
            if (strcmp(r.name, wanted[k]) != 0)
                continue;
            if (cxe_join(path, sizeof path, outdir, r.name) == 0) {
                size_t l = strlen(path);
                if (l + 5u < sizeof path) {
                    memcpy(path + l, ".png", 5);
                    if (cx_png_write_rgba8(path, r.rgba, r.w, r.h) != 0) {
                        fprintf(stderr, "cannot write %s\n", path);
                        rc = 1;
                    }
                }
            }
            found[k] = 1;
            gw[k] = r.w;
            gh[k] = r.h;
            extrema(r.rgba, r.w, r.h, 3, &alo[k], &ahi[k]);
            extrema(r.rgba, r.w, r.h, 0, &rlo[k], &rhi[k]);
            extrema(r.rgba, r.w, r.h, 1, &glo[k], &ghi[k]);
            extrema(r.rgba, r.w, r.h, 2, &blo[k], &bhi[k]);
            break;
        }
        cxe_rec_free(&r);
    }

    for (k = 0; k < nwanted; k++) {
        int req = 1;
        if (required) {
            int j;
            req = 0;
            for (j = 0; j < nrequired; j++) {
                if (strcmp(required[j], wanted[k]) == 0) { req = 1; break; }
            }
        }
        if (!found[k]) {
            if (req) {
                fprintf(stderr, "FAIL: %s not found in %s\n", wanted[k],
                        txd_path);
                rc = 1;
            } else {
                printf("  (optional) %s not found in %s\n", wanted[k],
                       txd_path);
            }
            continue;
        }
        switch (style) {
        case CXE_REPORT_CARFX:
            printf("  %-14s %dx%d  alpha range (%d, %d) -> %s/%s.png\n",
                   wanted[k], gw[k], gh[k], alo[k], ahi[k], outdir, wanted[k]);
            break;
        case CXE_REPORT_BOOSTFX:
            printf("  %-16s %dx%d  alpha (%d, %d)  rgb ((%d, %d), (%d, %d), "
                   "(%d, %d)) -> %s/%s.png\n",
                   wanted[k], gw[k], gh[k], alo[k], ahi[k], rlo[k], rhi[k],
                   glo[k], ghi[k], blo[k], bhi[k], outdir, wanted[k]);
            break;
        default:
            printf("  %-18s %dx%d  alpha (%d, %d)  rgb ((%d, %d), (%d, %d), "
                   "(%d, %d)) -> %s/%s.png\n",
                   wanted[k], gw[k], gh[k], alo[k], ahi[k], rlo[k], rhi[k],
                   glo[k], ghi[k], blo[k], bhi[k], outdir, wanted[k]);
            break;
        }
    }
    free(found);
    cxb_blob_free(&d);
    return rc;
}

/* ------------------------------------------------- extract_font's Image32 */
int cxe_elf_open(const char *path, cxe_elf *e)
{
    uint32_t phoff;
    uint16_t phnum;
    int i;

    e->nseg = 0;
    if (cxb_read_file(path, &e->f) != 0)
        return -1;
    if (e->f.n < 0x34u || memcmp(e->f.d, "\x7f" "ELF", 4) != 0) {
        cxb_blob_free(&e->f);
        return -1;
    }
    phoff = cxb_u32(&e->f, 0x1C);
    phnum = cxb_u16(&e->f, 0x2C);
    for (i = 0; i < (int)phnum && e->nseg < 64; i++) {
        size_t p = (size_t)phoff + (size_t)i * 32u;
        if (cxb_u32(&e->f, p) != 1u)                  /* PT_LOAD */
            continue;
        e->seg[e->nseg].off = cxb_u32(&e->f, p + 4);
        e->seg[e->nseg].va  = cxb_u32(&e->f, p + 8);
        e->seg[e->nseg].fsz = cxb_u32(&e->f, p + 16);
        e->seg[e->nseg].msz = cxb_u32(&e->f, p + 20);
        e->nseg++;
    }
    return 0;
}

void cxe_elf_close(cxe_elf *e)
{
    cxb_blob_free(&e->f);
    e->nseg = 0;
}

int cxe_elf_read(const cxe_elf *e, uint32_t va, size_t n, unsigned char *out)
{
    int i;

    for (i = 0; i < e->nseg; i++) {
        const cxe_elf_seg *s = &e->seg[i];
        uint64_t off;
        size_t avail, take;

        if (!(va >= s->va && (uint64_t)va < (uint64_t)s->va + s->msz))
            continue;
        off = (uint64_t)va - s->va;
        /* python: data[o+off : o+off+n] when off+n <= fs, else
         * data[o+off : o+fs] zero-padded to n (a short slice, never an
         * error, and empty when off > fs). */
        avail = (off < (uint64_t)s->fsz) ? (size_t)((uint64_t)s->fsz - off) : 0u;
        take = (avail < n) ? avail : n;
        memset(out, 0, n);
        if (take) {
            uint64_t fo = (uint64_t)s->off + off;
            if (fo + take > (uint64_t)e->f.n)
                take = (fo < (uint64_t)e->f.n)
                       ? (size_t)((uint64_t)e->f.n - fo) : 0u;
            if (take)
                memcpy(out, e->f.d + (size_t)fo, take);
        }
        return 0;
    }
    return -1;                                        /* VA %#x unmapped */
}

uint32_t cxe_elf_u32(const cxe_elf *e, uint32_t va, int *ok)
{
    unsigned char b[4];

    if (cxe_elf_read(e, va, 4, b) != 0) {
        if (ok) *ok = 0;
        return 0u;
    }
    if (ok) *ok = 1;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}
