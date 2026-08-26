/* cx_common_c.c -- agent C's private helpers.  See cx_common_c.h. */
#define _POSIX_C_SOURCE 200809L

#include "cx_common_c.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ------------------------------------------------------------------ blob */

int cxc_blob_load(cxc_blob *b, const char *path)
{
    FILE *f;
    long  sz;
    uint8_t *buf;

    memset(b, 0, sizeof(*b));
    f = cx_vfs_fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cextract: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fprintf(stderr, "cextract: %s: not seekable\n", path);
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1u);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "cextract: out of memory reading %s\n", path);
        return -1;
    }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cextract: %s: short read\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    b->d = buf;
    b->n = (size_t)sz;
    b->what = path;
    b->oob = 0;
    return 0;
}

void cxc_blob_free(cxc_blob *b)
{
    if (b && b->d) {
        free((void *)b->d);
        b->d = NULL;
        b->n = 0;
    }
}

cxc_blob cxc_blob_slice(const cxc_blob *b, int64_t off, int64_t len)
{
    cxc_blob s;
    int64_t end;

    memset(&s, 0, sizeof(s));
    s.what = b->what;
    if (off < 0)
        off = 0;
    if ((uint64_t)off > (uint64_t)b->n)
        off = (int64_t)b->n;
    end = off + (len < 0 ? 0 : len);
    if ((uint64_t)end > (uint64_t)b->n)
        end = (int64_t)b->n;
    s.d = b->d + off;
    s.n = (size_t)(end - off);
    return s;
}

/* The one gate every accessor goes through. */
static const uint8_t *at(cxc_blob *b, int64_t o, size_t need)
{
    if (o < 0 || (uint64_t)o > (uint64_t)b->n ||
        (uint64_t)(b->n - (uint64_t)o) < (uint64_t)need) {
        if (b->oob++ == 0)
            fprintf(stderr, "cextract: %s: read of %zu byte(s) at %lld is "
                            "outside the %zu-byte file\n",
                    b->what ? b->what : "(blob)", need, (long long)o, b->n);
        return NULL;
    }
    return b->d + o;
}

uint8_t cxc_u8(cxc_blob *b, int64_t o)
{
    const uint8_t *p = at(b, o, 1);
    return p ? p[0] : 0;
}

int8_t cxc_i8(cxc_blob *b, int64_t o) { return (int8_t)cxc_u8(b, o); }

uint16_t cxc_u16(cxc_blob *b, int64_t o)
{
    const uint8_t *p = at(b, o, 2);
    return p ? (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)) : 0;
}

int16_t cxc_i16(cxc_blob *b, int64_t o) { return (int16_t)cxc_u16(b, o); }

uint32_t cxc_u32(cxc_blob *b, int64_t o)
{
    const uint8_t *p = at(b, o, 4);
    if (!p)
        return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t cxc_i32(cxc_blob *b, int64_t o) { return (int32_t)cxc_u32(b, o); }

float cxc_f32(cxc_blob *b, int64_t o)
{
    union { uint32_t u; float f; } c;
    c.u = cxc_u32(b, o);
    return c.f;
}

int64_t cxc_ptr(cxc_blob *b, int64_t o, int64_t rel)
{
    int32_t v = cxc_i32(b, o);
    return v ? (int64_t)v + rel : 0;
}

int cxc_cstr(cxc_blob *b, int64_t o, int limit, char *out, size_t cap)
{
    int64_t e;
    size_t  len;

    out[0] = '\0';
    /* python `d.find(b'\0', o)`: a start past the end simply finds nothing. */
    if (o < 0 || (uint64_t)o > (uint64_t)b->n)
        return 0;
    for (e = o; (uint64_t)e < (uint64_t)b->n; e++)
        if (b->d[e] == 0)
            break;
    if ((uint64_t)e >= (uint64_t)b->n)
        return 0;                       /* no NUL -> find() == -1 */
    if (e - o > (int64_t)limit)
        return 0;                       /* read_cstr's limit */
    len = (size_t)(e - o);
    /* .decode('ascii') refuses anything with the high bit set. */
    for (size_t i = 0; i < len; i++)
        if (b->d[o + (int64_t)i] & 0x80u)
            return 0;
    if (len + 1 > cap)
        len = cap - 1;
    memcpy(out, b->d + o, len);
    out[len] = '\0';
    return 1;
}

/* ---------------------------------------------------------------- buffer */

static void buf_need(cxc_buf *b, size_t extra)
{
    size_t want;
    uint8_t *p;

    if (b->err)
        return;
    if (b->cap - b->n >= extra)
        return;
    want = b->cap ? b->cap : 4096;
    while (want - b->n < extra) {
        if (want > (size_t)-1 / 2) {
            b->err = 1;
            return;
        }
        want *= 2;
    }
    p = (uint8_t *)realloc(b->p, want);
    if (!p) {
        b->err = 1;
        return;
    }
    b->p = p;
    b->cap = want;
}

void cxc_buf_free(cxc_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void cxc_put_bytes(cxc_buf *b, const void *src, size_t n)
{
    buf_need(b, n);
    if (b->err)
        return;
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

void cxc_put_zero(cxc_buf *b, size_t n)
{
    buf_need(b, n);
    if (b->err)
        return;
    memset(b->p + b->n, 0, n);
    b->n += n;
}

void cxc_put_u8(cxc_buf *b, uint8_t v)
{
    buf_need(b, 1);
    if (b->err)
        return;
    b->p[b->n++] = v;
}

void cxc_put_u16(cxc_buf *b, uint16_t v)
{
    buf_need(b, 2);
    if (b->err)
        return;
    b->p[b->n++] = (uint8_t)(v & 0xFFu);
    b->p[b->n++] = (uint8_t)((v >> 8) & 0xFFu);
}

void cxc_put_u32(cxc_buf *b, uint32_t v)
{
    buf_need(b, 4);
    if (b->err)
        return;
    b->p[b->n++] = (uint8_t)(v & 0xFFu);
    b->p[b->n++] = (uint8_t)((v >> 8) & 0xFFu);
    b->p[b->n++] = (uint8_t)((v >> 16) & 0xFFu);
    b->p[b->n++] = (uint8_t)((v >> 24) & 0xFFu);
}

void cxc_put_i32(cxc_buf *b, int32_t v) { cxc_put_u32(b, (uint32_t)v); }

void cxc_put_f32(cxc_buf *b, float v)
{
    union { float f; uint32_t u; } c;
    c.f = v;
    cxc_put_u32(b, c.u);
}

void cxc_pad_to(cxc_buf *b, size_t off)
{
    if (off > b->n)
        cxc_put_zero(b, off - b->n);
}

int cxc_buf_write(const cxc_buf *b, const char *path)
{
    FILE *f;

    if (b->err) {
        fprintf(stderr, "cextract: out of memory building %s\n", path);
        return -1;
    }
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cextract: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (b->n && fwrite(b->p, 1, b->n, f) != b->n) {
        fprintf(stderr, "cextract: %s: short write\n", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "cextract: %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ misc glue */

void cxc_basename(const char *s, char *out, size_t cap)
{
    const char *p = s, *last = s;
    size_t len;

    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            last = p + 1;
    len = strlen(last);
    if (len + 1 > cap)
        len = cap - 1;
    memcpy(out, last, len);
    out[len] = '\0';
}

void cxc_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la && (a[la - 1] == '/'))
        snprintf(out, cap, "%s%s", a, b);
    else
        snprintf(out, cap, "%s/%s", a, b);
}

int cxc_mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len, i;

    len = strlen(path);
    if (len == 0 || len + 1 > sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    for (i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

double cxc_now_s(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int cxc_resolve_track(const char *game_dir, const char *spec,
                      char *out_id, size_t id_sz,
                      char *out_dir, size_t dir_sz)
{
    char id[32];
    size_t i, len;
    struct stat st;

    if (!spec || !*spec)
        return -1;
    len = strlen(spec);
    while (len && (spec[len - 1] == '/' || spec[len - 1] == '\\'))
        len--;
    if (len + 1 > sizeof(id))
        return -1;
    /* resolve()'s normalisation: separators fold to '_', then upper-case. */
    for (i = 0; i < len; i++) {
        char c = spec[i];
        if (c == '/' || c == '\\')
            c = '_';
        id[i] = (char)toupper((unsigned char)c);
    }
    id[len] = '\0';

    /* FUN_001574F0's positional split -- "<REG>_<Cn>_<Vn>". */
    if (len != 8 || id[2] != '_' || id[5] != '_') {
        fprintf(stderr, "cextract: %s is not a <REG>_<Cn>_<Vn> track id, and "
                        "the fallback resolver does not decode tlist.bin "
                        "(agent A's cx_resolve_track is not linked in yet)\n",
                spec);
        return -1;
    }
    if (snprintf(out_dir, dir_sz, "%s/Tracks/%c%c/%c%c_%c%c", game_dir,
                 id[0], id[1], id[3], id[4], id[6], id[7]) >= (int)dir_sz)
        return -1;
    if (snprintf(out_id, id_sz, "%s", id) >= (int)id_sz)
        return -1;
    if (stat(out_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cextract: no such track directory: %s\n", out_dir);
        return -1;
    }
    return 0;
}
