/* cx_common_g.c -- see cx_common_g.h. */
#define _POSIX_C_SOURCE 200809L

#include "cx_common_g.h"
#include "cx_src.h"   /* the dump may be a directory OR an ISO */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* =========================================================== text buffer == */
static void cxg_grow(cxg_str *s, size_t need)
{
    size_t cap;
    char  *p;

    if (s->err || s->n + need + 1 <= s->cap)
        return;
    cap = s->cap ? s->cap : 4096;
    while (cap < s->n + need + 1)
        cap *= 2;
    p = (char *)realloc(s->p, cap);
    if (!p) {
        s->err = 1;
        return;
    }
    s->p = p;
    s->cap = cap;
}

void cxg_sputs(cxg_str *s, const char *t)
{
    size_t n = strlen(t);

    cxg_grow(s, n);
    if (s->err)
        return;
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = '\0';
}

void cxg_sputc(cxg_str *s, char c)
{
    cxg_grow(s, 1);
    if (s->err)
        return;
    s->p[s->n++] = c;
    s->p[s->n] = '\0';
}

static void cxg_vappend(cxg_str *s, const char *fmt, va_list ap)
{
    va_list cp;
    int     n;

    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) {
        s->err = 1;
        return;
    }
    cxg_grow(s, (size_t)n);
    if (s->err)
        return;
    vsnprintf(s->p + s->n, (size_t)n + 1, fmt, ap);
    s->n += (size_t)n;
}

void cxg_sprintf(cxg_str *s, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    cxg_vappend(s, fmt, ap);
    va_end(ap);
}

void cxg_sline(cxg_str *s, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    cxg_vappend(s, fmt, ap);
    va_end(ap);
    cxg_sputc(s, '\n');
}

void cxg_str_free(cxg_str *s)
{
    free(s->p);
    s->p = NULL;
    s->n = s->cap = 0;
}

int cxg_str_write(const cxg_str *s, const char *path)
{
    char  dir[4096];
    char *slash;
    FILE *f;

    if (s->err) {
        fprintf(stderr, "cxg: out of memory building %s\n", path);
        return 1;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (cxg_mkdir_p(dir) != 0)
            return 1;
    }
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cxg: cannot write %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (s->n && fwrite(s->p, 1, s->n, f) != s->n) {
        fprintf(stderr, "cxg: short write on %s\n", path);
        fclose(f);
        return 1;
    }
    return fclose(f) != 0;
}

int cxg_str_matches_file(const cxg_str *s, const char *path)
{
    /* Deliberately quiet, and deliberately not cxg_read_text: gen_trackselect
     * --check reads `open(OUT).read() if os.path.exists(OUT) else ""`, so an
     * absent file is a STALE verdict, not an I/O error to complain about. */
    FILE  *f = cx_vfs_fopen(path, "rb");
    long   sz;
    char  *cur;
    int    ok;

    if (!f)
        return s->n == 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    if ((size_t)sz != s->n) {
        fclose(f);
        return 0;
    }
    if (s->n == 0) {
        fclose(f);
        return 1;
    }
    cur = (char *)malloc(s->n);
    if (!cur) {
        fclose(f);
        return 0;
    }
    ok = fread(cur, 1, s->n, f) == s->n && memcmp(cur, s->p, s->n) == 0;
    fclose(f);
    free(cur);
    return ok;
}

/* =================================================================== blob == */
int cxg_blob_load(cxg_blob *b, const char *path)
{
    FILE *f;
    long  sz;

    memset(b, 0, sizeof(*b));
    b->what = path;
    f = cx_vfs_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cxg: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        fprintf(stderr, "cxg: cannot size %s\n", path);
        return 1;
    }
    rewind(f);
    b->d = (unsigned char *)malloc((size_t)sz + 1);
    if (!b->d) {
        fclose(f);
        fprintf(stderr, "cxg: out of memory reading %s\n", path);
        return 1;
    }
    if (sz && fread(b->d, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b->d);
        b->d = NULL;
        fprintf(stderr, "cxg: short read on %s\n", path);
        return 1;
    }
    fclose(f);
    b->n = (size_t)sz;
    b->d[b->n] = 0;
    return 0;
}

void cxg_blob_free(cxg_blob *b)
{
    free(b->d);
    b->d = NULL;
    b->n = 0;
}

static const unsigned char *cxg_at(cxg_blob *b, int64_t o, size_t n)
{
    if (o < 0 || (uint64_t)o + n > b->n) {
        b->oob++;
        return NULL;
    }
    return b->d + o;
}

uint8_t cxg_u8(cxg_blob *b, int64_t o)
{
    const unsigned char *p = cxg_at(b, o, 1);
    return p ? p[0] : 0;
}

uint32_t cxg_u32(cxg_blob *b, int64_t o)
{
    const unsigned char *p = cxg_at(b, o, 4);
    if (!p)
        return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t cxg_u64(cxg_blob *b, int64_t o)
{
    const unsigned char *p = cxg_at(b, o, 8);
    uint64_t v = 0;
    int i;

    if (!p)
        return 0;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

float cxg_f32(cxg_blob *b, int64_t o)
{
    uint32_t u = cxg_u32(b, o);
    float    f;

    memcpy(&f, &u, 4);
    return f;
}

char *cxg_read_text(const char *path, size_t *n_out)
{
    cxg_blob b;

    if (cxg_blob_load(&b, path) != 0)
        return NULL;
    if (n_out)
        *n_out = b.n;
    return (char *)b.d;          /* NUL-terminated by cxg_blob_load */
}

/* ================================================================== image == */
/* The raw XBE, for the case where the port's build/burnout3.elf is not around.
 * tools/xbe2elf.py's own mapping, and it is the identity on the VA: an XBE
 * section header is 0x38 bytes of { flags, vaddr, vsize, raw, rsize, ... } at
 * (+0x120 - image_base) with the count at +0x11C, and the ELF the repo builds
 * carries exactly these VAs -- checked section by section, .rdata included,
 * which is where every table gen_trackselect reads lives. */
static int cxg_image_open_xbe(cxg_image *im, const char *path)
{
    uint32_t base, nsec, shdr;
    uint32_t i;

    base = cxg_u32(&im->raw, 0x104);
    nsec = cxg_u32(&im->raw, 0x11C);
    shdr = cxg_u32(&im->raw, 0x120);
    if (shdr < base) {
        fprintf(stderr, "cxg: %s has an implausible section table\n", path);
        return 1;
    }
    shdr -= base;
    for (i = 0; i < nsec; i++) {
        int64_t o = (int64_t)shdr + (int64_t)i * 0x38;

        if (im->nseg == (int)(sizeof(im->seg) / sizeof(im->seg[0])))
            break;
        im->seg[im->nseg].va  = cxg_u32(&im->raw, o + 0x04);
        im->seg[im->nseg].off = cxg_u32(&im->raw, o + 0x0C);
        im->seg[im->nseg].fsz = cxg_u32(&im->raw, o + 0x10);
        im->nseg++;
    }
    if (im->raw.oob || im->nseg == 0) {
        fprintf(stderr, "cxg: %s has a truncated section table\n", path);
        return 1;
    }
    return 0;
}

int cxg_image_open(cxg_image *im, const char *path)
{
    uint32_t phoff;
    uint16_t entsz, num, i;

    memset(im, 0, sizeof(*im));
    if (cxg_blob_load(&im->raw, path) != 0)
        return 1;
    if (im->raw.n >= 0x180 && memcmp(im->raw.d, "XBEH", 4) == 0) {
        if (cxg_image_open_xbe(im, path) != 0) {
            cxg_blob_free(&im->raw);
            return 1;
        }
        return 0;
    }
    if (im->raw.n < 0x34 || memcmp(im->raw.d, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "cxg: %s is neither an ELF nor an XBE image\n", path);
        cxg_blob_free(&im->raw);
        return 1;
    }
    phoff = cxg_u32(&im->raw, 0x1C);
    entsz = (uint16_t)(im->raw.d[0x2A] | (im->raw.d[0x2B] << 8));
    num   = (uint16_t)(im->raw.d[0x2C] | (im->raw.d[0x2D] << 8));
    for (i = 0; i < num; i++) {
        int64_t  o = (int64_t)phoff + (int64_t)i * entsz;
        uint32_t typ = cxg_u32(&im->raw, o);

        if (typ != 1)            /* PT_LOAD only */
            continue;
        if (im->nseg == (int)(sizeof(im->seg) / sizeof(im->seg[0])))
            break;
        im->seg[im->nseg].off = cxg_u32(&im->raw, o + 4);
        im->seg[im->nseg].va  = cxg_u32(&im->raw, o + 8);
        im->seg[im->nseg].fsz = cxg_u32(&im->raw, o + 16);
        im->nseg++;
    }
    if (im->raw.oob) {
        fprintf(stderr, "cxg: %s has a truncated program header table\n", path);
        cxg_blob_free(&im->raw);
        return 1;
    }
    return 0;
}

void cxg_image_close(cxg_image *im)
{
    cxg_blob_free(&im->raw);
    im->nseg = 0;
}

const unsigned char *cxg_image_read(cxg_image *im, uint32_t va, size_t n)
{
    int i;

    for (i = 0; i < im->nseg; i++) {
        const cxg_seg *s = &im->seg[i];

        if (va >= s->va && va < (uint64_t)s->va + s->fsz) {
            uint64_t k = (uint64_t)va - s->va;

            if (k + n <= s->fsz)
                return cxg_at(&im->raw, (int64_t)s->off + (int64_t)k, n);
            return NULL;
        }
    }
    return NULL;
}

/* =============================================================== globalus == */
int cxg_globalus_open(cxg_globalus *g, const char *path)
{
    memset(g, 0, sizeof(*g));
    if (cxg_blob_load(&g->raw, path) != 0)
        return 1;
    g->count = cxg_u32(&g->raw, 8);
    return 0;
}

void cxg_globalus_close(cxg_globalus *g)
{
    cxg_blob_free(&g->raw);
}

/* One code point out to UTF-8; returns the bytes written. */
static size_t cxg_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *cxg_globalus_get(cxg_globalus *g, uint32_t idx, char *out, size_t cap)
{
    int64_t off = (int64_t)cxg_u32(&g->raw, 0x10 + (int64_t)idx * 4);
    int64_t end = off;
    size_t  w = 0;

    /* python: advance in u16 steps while the pair is not 00 00, stopping when
     * fewer than two bytes remain. */
    while (end + 1 < (int64_t)g->raw.n &&
           !(g->raw.d[end] == 0 && g->raw.d[end + 1] == 0))
        end += 2;

    while (off < end && w + 5 < cap) {
        uint32_t u;

        if (off + 1 >= (int64_t)g->raw.n)
            break;
        u = (uint32_t)g->raw.d[off] | ((uint32_t)g->raw.d[off + 1] << 8);
        off += 2;
        if (u >= 0xD800 && u < 0xDC00) {
            uint32_t lo = 0;

            if (off + 1 < (int64_t)g->raw.n && off < end)
                lo = (uint32_t)g->raw.d[off] | ((uint32_t)g->raw.d[off + 1] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                off += 2;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                u = 0xFFFD;      /* 'replace': an unpaired high surrogate */
            }
        } else if (u >= 0xDC00 && u < 0xE000) {
            u = 0xFFFD;          /* 'replace': an unpaired low surrogate */
        }
        w += cxg_utf8(u, out + w);
    }
    out[w] = '\0';
    return out;
}

/* ================================================================ base-40 == */
static const char CXG_CS[] = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";

/* python's str.strip(): whitespace off both ends.  The charset's only
 * whitespace is the space at index 0. */
static char *cxg_strip(char *s)
{
    size_t n = strlen(s);
    size_t a = 0;

    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
                     s[n - 1] == '\r' || s[n - 1] == '\v' || s[n - 1] == '\f'))
        n--;
    while (a < n && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' ||
                     s[a] == '\r' || s[a] == '\v' || s[a] == '\f'))
        a++;
    memmove(s, s + a, n - a);
    s[n - a] = '\0';
    return s;
}

char *cxg_b40_raw(uint64_t v, char *out)
{
    int i;

    for (i = 0; i < 12; i++) {
        out[i] = CXG_CS[v % 40];
        v /= 40;
    }
    out[12] = '\0';
    return cxg_strip(out);
}

char *cxg_b40(uint64_t v, char *out)
{
    char tmp[13];
    int  i;

    for (i = 0; i < 12; i++) {
        tmp[i] = CXG_CS[v % 40];
        v /= 40;
    }
    for (i = 0; i < 12; i++)
        out[i] = tmp[11 - i];
    out[12] = '\0';
    return cxg_strip(out);
}

/* =================================================================== paths = */
int cxg_mkdir_p(const char *path)
{
    char   tmp[4096];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp))
        return 1;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return 1;
        if (i < n)
            tmp[i] = '/';
    }
    return 0;
}

void cxg_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n = strlen(a);

    if (n && a[n - 1] == '/')
        snprintf(out, cap, "%s%s", a, b);
    else
        snprintf(out, cap, "%s/%s", a, b);
}

void cxg_gen_path(char *out, size_t cap, const char *out_root,
                  const char *filename)
{
    const char *over = getenv("B3_GEN_OUT");
    char        dir[4096];

    if (over && *over) {
        cxg_join(out, cap, over, filename);
        return;
    }
    cxg_join(dir, sizeof(dir), out_root, "gen");
    cxg_join(out, cap, dir, filename);
}

/* os.path.normpath, absolute input only: collapse //, drop "." and fold "..".
 * The generators' paths are absolute by construction (ROOT is derived from
 * abspath(__file__)), so no cwd is consulted here either. */
static void cxg_normpath(char *out, size_t cap, const char *p)
{
    const char *seg[256];
    size_t      len[256];
    int         n = 0;
    const char *q = p;
    size_t      w = 0;

    while (*q) {
        const char *e;

        while (*q == '/')
            q++;
        if (!*q)
            break;
        e = strchr(q, '/');
        if (!e)
            e = q + strlen(q);
        if (e - q == 1 && q[0] == '.') {
            /* drop */
        } else if (e - q == 2 && q[0] == '.' && q[1] == '.') {
            if (n)
                n--;
        } else if (n < (int)(sizeof(seg) / sizeof(seg[0]))) {
            seg[n] = q;
            len[n] = (size_t)(e - q);
            n++;
        }
        q = e;
    }
    if (cap == 0)
        return;
    out[w++] = '/';
    for (int i = 0; i < n; i++) {
        if (i && w + 1 < cap)
            out[w++] = '/';
        for (size_t k = 0; k < len[i] && w + 1 < cap; k++)
            out[w++] = seg[i][k];
    }
    out[w < cap ? w : cap - 1] = '\0';
}

void cxg_relpath(char *out, size_t cap, const char *path, const char *start)
{
    char  a[4096], b[4096];
    char *pa[256], *pb[256];
    int   na = 0, nb = 0, i, common = 0;
    size_t w = 0;
    char *tok, *sv;

    cxg_normpath(a, sizeof(a), path);
    cxg_normpath(b, sizeof(b), start);
    for (tok = strtok_r(a, "/", &sv); tok && na < 256;
         tok = strtok_r(NULL, "/", &sv))
        pa[na++] = tok;
    for (tok = strtok_r(b, "/", &sv); tok && nb < 256;
         tok = strtok_r(NULL, "/", &sv))
        pb[nb++] = tok;
    while (common < na && common < nb && !strcmp(pa[common], pb[common]))
        common++;
    if (na == common && nb == common) {
        snprintf(out, cap, ".");
        return;
    }
    for (i = common; i < nb; i++) {
        int k = snprintf(out + w, w < cap ? cap - w : 0, "%s..",
                         w ? "/" : "");
        if (k > 0)
            w += (size_t)k;
    }
    for (i = common; i < na; i++) {
        int k = snprintf(out + w, w < cap ? cap - w : 0, "%s%s",
                         w ? "/" : "", pa[i]);
        if (k > 0)
            w += (size_t)k;
    }
    if (w == 0 && cap)
        snprintf(out, cap, ".");
}

const char *cxg_repo_root(void)
{
    const char *e = getenv("B3_REPO_DIR");

    if (e && *e)
        return e;
    return CXG_DEFAULT_REPO_DIR;
}

/* ============================================================ C literals == */
char *cxg_cstr(const char *s, char *out, size_t cap)
{
    size_t w = 0;

    if (cap < 3) {
        if (cap)
            out[0] = '\0';
        return out;
    }
    out[w++] = '"';
    for (; *s && w + 3 < cap; s++) {
        if (*s == '\\' || *s == '"')
            out[w++] = '\\';
        out[w++] = *s;
    }
    out[w++] = '"';
    out[w] = '\0';
    return out;
}

/* ========================================================= text scanning == */
static int cxg_isw(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int cxg_isspace_re(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

static int cxg_ishex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static const char *cxg_skip_ws(const char *p)
{
    while (cxg_isspace_re(*p))
        p++;
    return p;
}

int cxg_scan_assert(const char *src, const char *struct_name, int need_0x,
                    cxg_assert_hit *out, int cap)
{
    char        head[160];
    size_t      hl;
    const char *p = src;
    int         n = 0;

    /* `_Static_assert\(offsetof\(<struct>,` -- the literal prefix. */
    snprintf(head, sizeof(head), "_Static_assert(offsetof(%s,", struct_name);
    hl = strlen(head);

    while ((p = strstr(p, head)) != NULL) {
        const char *q = cxg_skip_ws(p + hl);   /* `\s*` before the name */
        const char *nm = q;
        uint32_t    off = 0;
        size_t      len;

        while (cxg_isw(*q))                    /* `(\w+)` */
            q++;
        len = (size_t)(q - nm);
        if (len == 0 || len >= sizeof(out[0].name)) {
            p += hl;
            continue;
        }
        if (*q != ')') {
            p += hl;
            continue;
        }
        q = cxg_skip_ws(q + 1);                /* `\)\s*` */
        if (q[0] != '=' || q[1] != '=') {
            p += hl;
            continue;
        }
        q = cxg_skip_ws(q + 2);                /* `==\s*` */
        if (q[0] == '0' && q[1] == 'x') {
            q += 2;
        } else if (need_0x) {
            p += hl;
            continue;
        }
        if (!cxg_ishex(*q)) {
            p += hl;
            continue;
        }
        while (cxg_ishex(*q)) {                /* `([0-9A-Fa-f]+)` */
            int d = (*q <= '9') ? *q - '0' : ((*q | 32) - 'a' + 10);

            off = off * 16u + (uint32_t)d;
            q++;
        }
        if (n < cap) {
            memcpy(out[n].name, nm, len);
            out[n].name[len] = '\0';
            out[n].off = off;
            n++;
        }
        p += hl;
    }
    return n;
}

int cxg_has_assert_for(const char *src, const char *struct_name,
                       const char *member)
{
    char needle[192];

    snprintf(needle, sizeof(needle), "offsetof(%s, %s)", struct_name, member);
    return strstr(src, needle) != NULL;
}
