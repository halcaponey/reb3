/* cx_common_b.c -- see cx_common_b.h for the format provenance. */
#include "cx_common_b.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ blob */
int cxb_read_file(const char *path, cxb_blob *out)
{
    FILE *f;
    long len;
    unsigned char *buf;

    out->d = NULL;
    out->n = 0;
    f = cx_vfs_fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    len = ftell(f);
    if (len < 0) { fclose(f); return -1; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)len + 1u);
    if (!buf) { fclose(f); return -1; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[len] = 0;
    out->d = buf;
    out->n = (size_t)len;
    return 0;
}

void cxb_blob_free(cxb_blob *b)
{
    free(b->d);
    b->d = NULL;
    b->n = 0;
}

int cxb_mkdir_p(const char *path)
{
    char tmp[4096];
    size_t i, n;

    n = strlen(path);
    if (n == 0 || n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = 0;
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0777) != 0) {
                struct stat st;
                if (stat(tmp, &st) != 0)
                    return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0) {
        struct stat st;
        if (stat(tmp, &st) != 0)
            return -1;
    }
    return 0;
}

int cxb_file_exists(const char *path)
{
    return cx_vfs_exists(path);
}

/* --------------------------------------------------------------- readers */
static int in_range(const cxb_blob *b, size_t o, size_t n)
{
    return o <= b->n && b->n - o >= n;
}

uint8_t cxb_u8(const cxb_blob *b, size_t o)
{
    return in_range(b, o, 1) ? b->d[o] : 0u;
}

int8_t cxb_i8(const cxb_blob *b, size_t o)
{
    return in_range(b, o, 1) ? (int8_t)b->d[o] : (int8_t)0;
}

uint16_t cxb_u16(const cxb_blob *b, size_t o)
{
    if (!in_range(b, o, 2))
        return 0u;
    return (uint16_t)(b->d[o] | ((uint16_t)b->d[o + 1] << 8));
}

uint32_t cxb_u32(const cxb_blob *b, size_t o)
{
    if (!in_range(b, o, 4))
        return 0u;
    return (uint32_t)b->d[o] | ((uint32_t)b->d[o + 1] << 8)
         | ((uint32_t)b->d[o + 2] << 16) | ((uint32_t)b->d[o + 3] << 24);
}

int32_t cxb_i32(const cxb_blob *b, size_t o)
{
    return (int32_t)cxb_u32(b, o);
}

float cxb_f32(const cxb_blob *b, size_t o)
{
    uint32_t v = cxb_u32(b, o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

int64_t cxb_ptr(const cxb_blob *b, size_t o, int64_t rel)
{
    int32_t v = cxb_i32(b, o);
    return v ? (int64_t)v + rel : 0;
}

int cxb_read_cstr(const cxb_blob *b, int64_t off, size_t limit,
                  char *out, size_t outsz)
{
    size_t o, e;

    out[0] = 0;
    if (off < 0 || (uint64_t)off >= (uint64_t)b->n)
        return -1;
    o = (size_t)off;
    for (e = o; e < b->n && b->d[e]; e++)
        ;
    if (e >= b->n)                       /* python: data.find(b'\0') == -1 */
        return -1;
    if (e - o > limit)
        return -1;
    if (e - o >= outsz)
        return -1;
    for (size_t i = o; i < e; i++) {
        if (b->d[i] >= 0x80)             /* bytes.decode('ascii') would raise */
            return -1;
    }
    memcpy(out, b->d + o, e - o);
    out[e - o] = 0;
    return 0;
}

void cxb_basename(const char *in, char *out, size_t outsz)
{
    const char *p, *last = in;

    for (p = in; *p; p++) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    snprintf(out, outsz, "%s", last);
}

/* --------------------------------------------------------------- formats */
const char *cxb_fmt_name(unsigned fmt)
{
    switch (fmt) {
    case CXB_FMT_P8:   return "Paletted";
    case CXB_FMT_DXT1: return "DXT1";
    case CXB_FMT_DXT3: return "DXT3";
    case CXB_FMT_DXT5: return "DXT5";
    case CXB_FMT_RGBA: return "RGBA";
    default:           return "?";
    }
}

int cxb_parse_texrec(const cxb_blob *b, int64_t rec, cxb_texrec *out)
{
    unsigned fmt;
    int w, h, depth;
    int64_t rel, data_off;
    char name[80];
    const char *p;

    if (rec < 0 || (uint64_t)rec + 0x70u > (uint64_t)b->n)
        return -1;
    fmt = cxb_u32(b, (size_t)rec + 0x34);
    w = (int)cxb_u32(b, (size_t)rec + 0x38);
    h = (int)cxb_u32(b, (size_t)rec + 0x3C);
    depth = (int)cxb_u32(b, (size_t)rec + 0x40);
    if (fmt != CXB_FMT_P8 && fmt != CXB_FMT_DXT1 && fmt != CXB_FMT_DXT3
        && fmt != CXB_FMT_DXT5 && fmt != CXB_FMT_RGBA)
        return -1;
    if (!(w > 0 && w <= 4096) || !(h > 0 && h <= 4096))
        return -1;
    if ((w & (w - 1)) || (h & (h - 1)))
        return -1;
    if (cxb_read_cstr(b, rec + ((depth == 4 || depth == 8 || depth == 32)
                                ? 0x48 : 0x44), 64, name, sizeof name) != 0)
        return -1;
    /* python: name.replace('_','').replace('.','').isalnum() -- which is FALSE
     * for the empty string, so a name of only '_' and '.' is rejected too. */
    {
        int any = 0;
        for (p = name; *p; p++) {
            if (*p == '_' || *p == '.')
                continue;
            if (!isalnum((unsigned char)*p))
                return -1;
            any = 1;
        }
        if (!any)
            return -1;
    }
    rel = (int64_t)cxb_i32(b, (size_t)rec + 0x04);
    data_off = rec + rel;
    if (!(data_off > 0 && (uint64_t)data_off < (uint64_t)b->n))
        return -1;

    out->rec = (size_t)rec;
    out->fmt = fmt;
    out->w = w;
    out->h = h;
    out->depth = depth;
    out->npal = (int)cxb_u8(b, (size_t)rec + 0x69);
    out->data_off = data_off;
    snprintf(out->name, sizeof out->name, "%s", name);
    return 0;
}

/* -------------------------------------------------------------- decoders */
static void rgb565(unsigned c, int *r, int *g, int *bb)
{
    int rr = (int)((c >> 11) & 0x1F);
    int gg = (int)((c >> 5) & 0x3F);
    int b5 = (int)(c & 0x1F);
    *r = (rr << 3) | (rr >> 2);
    *g = (gg << 2) | (gg >> 4);
    *bb = (b5 << 3) | (b5 >> 2);
}

static void colour_block(const unsigned char *d, size_t o, unsigned char *out,
                         int ox, int oy, int w, int h, int dxt1)
{
    unsigned c0 = (unsigned)(d[o] | (d[o + 1] << 8));
    unsigned c1 = (unsigned)(d[o + 2] | (d[o + 3] << 8));
    uint32_t bits = (uint32_t)d[o + 4] | ((uint32_t)d[o + 5] << 8)
                  | ((uint32_t)d[o + 6] << 16) | ((uint32_t)d[o + 7] << 24);
    int r0, g0, b0, r1, g1, b1;
    unsigned char pal[4][4];
    int py, px;

    rgb565(c0, &r0, &g0, &b0);
    rgb565(c1, &r1, &g1, &b1);
    pal[0][0] = (unsigned char)r0; pal[0][1] = (unsigned char)g0;
    pal[0][2] = (unsigned char)b0; pal[0][3] = 255;
    pal[1][0] = (unsigned char)r1; pal[1][1] = (unsigned char)g1;
    pal[1][2] = (unsigned char)b1; pal[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        pal[2][0] = (unsigned char)((2 * r0 + r1) / 3);
        pal[2][1] = (unsigned char)((2 * g0 + g1) / 3);
        pal[2][2] = (unsigned char)((2 * b0 + b1) / 3);
        pal[2][3] = 255;
        pal[3][0] = (unsigned char)((r0 + 2 * r1) / 3);
        pal[3][1] = (unsigned char)((g0 + 2 * g1) / 3);
        pal[3][2] = (unsigned char)((b0 + 2 * b1) / 3);
        pal[3][3] = 255;
    } else {
        /* DXT1 with c0 <= c1: third colour is a 50% blend, fourth transparent */
        pal[2][0] = (unsigned char)((r0 + r1) / 2);
        pal[2][1] = (unsigned char)((g0 + g1) / 2);
        pal[2][2] = (unsigned char)((b0 + b1) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (py = 0; py < 4; py++) {
        for (px = 0; px < 4; px++) {
            int x = ox + px, y = oy + py;
            size_t k;
            if (x >= w || y >= h)
                continue;
            k = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            memcpy(out + k, pal[(bits >> (2 * (py * 4 + px))) & 3u], 4);
        }
    }
}

static void dxt5_alpha_block(const unsigned char *d, size_t o,
                             unsigned char *out, int ox, int oy, int w, int h)
{
    int a0 = d[o], a1 = d[o + 1];
    int al[8];
    uint64_t bits = 0;
    int i, py, px;

    al[0] = a0;
    al[1] = a1;
    /* the weights are (6-i)/(i+1) over 7 and (4-i)/(i+1) over 5 -- using
     * 7-i / 5-i overflows past 255. */
    if (a0 > a1) {
        for (i = 0; i < 6; i++)
            al[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
    } else {
        for (i = 0; i < 4; i++)
            al[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
        al[6] = 0;
        al[7] = 255;
    }
    for (i = 0; i < 6; i++)
        bits |= (uint64_t)d[o + 2 + i] << (8 * i);
    for (py = 0; py < 4; py++) {
        for (px = 0; px < 4; px++) {
            int x = ox + px, y = oy + py;
            if (x >= w || y >= h)
                continue;
            out[(((size_t)y * (size_t)w + (size_t)x) * 4u) + 3] =
                (unsigned char)al[(bits >> (3 * (py * 4 + px))) & 7u];
        }
    }
}

static void dxt3_alpha_block(const unsigned char *d, size_t o,
                             unsigned char *out, int ox, int oy, int w, int h)
{
    uint64_t bits = 0;
    int i, py, px;

    for (i = 0; i < 8; i++)
        bits |= (uint64_t)d[o + i] << (8 * i);
    for (py = 0; py < 4; py++) {
        for (px = 0; px < 4; px++) {
            int x = ox + px, y = oy + py;
            int a;
            if (x >= w || y >= h)
                continue;
            a = (int)((bits >> (4 * (py * 4 + px))) & 0xFu);
            out[(((size_t)y * (size_t)w + (size_t)x) * 4u) + 3] =
                (unsigned char)(a * 17);
        }
    }
}

int cxb_decode_dxt(const cxb_blob *b, int64_t off, int w, int h, unsigned fmt,
                   unsigned char *out)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int step = (fmt == CXB_FMT_DXT1) ? 8 : 16;
    int by, bx;
    uint64_t need = (uint64_t)bw * (uint64_t)bh * (uint64_t)step;

    if (off < 0 || (uint64_t)off + need > (uint64_t)b->n)
        return -1;
    memset(out, 0, (size_t)w * (size_t)h * 4u);
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            size_t o = (size_t)off + ((size_t)by * (size_t)bw + (size_t)bx)
                       * (size_t)step;
            if (fmt == CXB_FMT_DXT1) {
                colour_block(b->d, o, out, bx * 4, by * 4, w, h, 1);
            } else {
                colour_block(b->d, o + 8, out, bx * 4, by * 4, w, h, 0);
                if (fmt == CXB_FMT_DXT5)
                    dxt5_alpha_block(b->d, o, out, bx * 4, by * 4, w, h);
                else
                    dxt3_alpha_block(b->d, o, out, bx * 4, by * 4, w, h);
            }
        }
    }
    return 0;
}

int cxb_decode_rgba(const cxb_blob *b, int64_t off, int w, int h,
                    unsigned char *out)
{
    size_t n = (size_t)w * (size_t)h * 4u, i;
    const unsigned char *src;

    if (off < 0 || (uint64_t)off + n > (uint64_t)b->n)
        return -1;
    src = b->d + (size_t)off;
    for (i = 0; i < n; i += 4) {              /* BGRA -> RGBA */
        out[i]     = src[i + 2];
        out[i + 1] = src[i + 1];
        out[i + 2] = src[i];
        out[i + 3] = src[i + 3];
    }
    return 0;
}

static int bit_length(unsigned v)
{
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

void cxb_unswizzle8(const unsigned char *in, int w, int h, unsigned char *out)
{
    int xbits = bit_length((unsigned)w) - 1;
    int ybits = bit_length((unsigned)h) - 1;
    int shared = xbits < ybits ? xbits : ybits;
    long i, total = (long)w * (long)h;

    for (i = 0; i < total; i++) {
        unsigned x = 0, y = 0, v = (unsigned)i;
        int bpos;
        for (bpos = 0; bpos < shared; bpos++) {
            x |= (v & 1u) << bpos; v >>= 1;
            y |= (v & 1u) << bpos; v >>= 1;
        }
        if (xbits > ybits)
            x |= v << shared;
        else
            y |= v << shared;
        out[(size_t)y * (size_t)w + x] = in[i];
    }
}

int cxb_decode_paletted(const cxb_blob *b, size_t rec, int64_t bmp,
                        int w, int h, int bd, int which_pal,
                        unsigned char *out)
{
    size_t npix = (size_t)w * (size_t)h;
    unsigned char *idx;
    int64_t prec, pdata;
    int ncol = (bd == 4) ? 16 : 256;
    const unsigned char *pal;
    size_t i;

    if (bmp < 0 || (uint64_t)bmp + npix > (uint64_t)b->n)
        return -1;                                     /* pixels truncated */
    idx = (unsigned char *)malloc(npix);
    if (!idx)
        return -1;
    cxb_unswizzle8(b->d + (size_t)bmp, w, h, idx);
    prec = (int64_t)rec + (int64_t)cxb_u32(b, rec + 0x14 + (size_t)which_pal * 4);
    if (prec < 0 || (uint64_t)prec + 8u > (uint64_t)b->n) { free(idx); return -1; }
    /* palette record magic {u16 1, u16 3 or 0xC003} */
    if (!(b->d[prec] == 0x01 && b->d[prec + 1] == 0x00
          && ((b->d[prec + 2] == 0x03 && b->d[prec + 3] == 0x00)
              || (b->d[prec + 2] == 0x03 && b->d[prec + 3] == 0xC0)))) {
        free(idx);
        return -1;                                     /* bad palette record */
    }
    pdata = (int64_t)rec + (int64_t)cxb_u32(b, (size_t)prec + 4);
    if (pdata < 0 || (uint64_t)pdata + (uint64_t)ncol * 4u > (uint64_t)b->n) {
        free(idx);
        return -1;                                     /* palette truncated */
    }
    pal = b->d + (size_t)pdata;
    for (i = 0; i < npix; i++) {
        int pi = idx[i];
        if (pi >= ncol) { free(idx); return -1; }
        out[i * 4]     = pal[pi * 4 + 2];               /* BGRA -> RGBA */
        out[i * 4 + 1] = pal[pi * 4 + 1];
        out[i * 4 + 2] = pal[pi * 4];
        out[i * 4 + 3] = pal[pi * 4 + 3];
    }
    free(idx);
    return 0;
}

/* --------------------------------------------------------------- txd walk */
int cxb_txd_count(const cxb_blob *b, uint32_t *count_out)
{
    uint32_t m1, m2, count, esz;

    if (b->n < 0x10)
        return -1;
    m1 = cxb_u32(b, 0);
    m2 = cxb_u32(b, 4);
    count = cxb_u32(b, 8);
    esz = cxb_u32(b, 0x0C);
    if (m1 != 0x543C0000u || m2 != 0xBCDEED81u || esz != 16u)
        return -1;
    *count_out = count;
    return 0;
}

int cxb_txd_entry_offset(const cxb_blob *b, uint32_t i, int64_t *off_out)
{
    size_t e = 0x10u + (size_t)i * 16u;
    uint32_t eid = cxb_u32(b, e);
    uint32_t z0 = cxb_u32(b, e + 4);
    uint32_t off = cxb_u32(b, e + 8);
    uint32_t z1 = cxb_u32(b, e + 12);

    if (eid != i + 1u || z0 || z1)
        return -1;
    if (!(off > 0x10u && (uint64_t)off < (uint64_t)b->n - 0x80u))
        return -1;
    *off_out = (int64_t)off;
    return 0;
}
