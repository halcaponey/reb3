/* cx_png.c -- PNG writer over system zlib.  See cx_png.h.
 *
 * Deliberately minimal and self-contained: no external/downloaded code.  The
 * acceptance gate for the ported extractors is PIXEL identity against PIL's
 * output (two different deflate encoders never agree byte for byte), so the
 * only requirements here are a correct container and correct pixels.
 *
 * The file is now in two halves: cx_png_encode() builds the whole PNG in
 * memory and touches no file, and everything else is about where those bytes
 * go -- straight out (cx_png_write_*) or through the deferred queue whose
 * contract cx_png.h states.
 */
/* -std=c11 hides POSIX; pthread_self/pthread_equal guard the queue's owner. */
#define _POSIX_C_SOURCE 200809L

#include "cx_png.h"
#include "cx_pool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* How many bytes of RAW PIXELS may sit in the queue before it self-flushes.
 * 808 frontend textures decoded to RGBA are well past this; 256 MB is a
 * couple of hundred 512x512 pages, which is more than enough to keep every
 * worker fed and small enough to be invisible next to the game's own heap. */
#define CX_PNG_QUEUE_BUDGET (256u * 1024u * 1024u)

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* Append one chunk to `dst` at *o: length, type, payload, CRC. */
static void put_chunk(unsigned char *dst, size_t *o, const char *type,
                      const unsigned char *data, size_t len)
{
    uLong crc;
    put_be32(dst + *o, (uint32_t)len);
    memcpy(dst + *o + 4, type, 4);
    if (len)
        memcpy(dst + *o + 8, data, len);
    crc = crc32(0L, (const Bytef *)(dst + *o + 4), (uInt)(4u + len));
    put_be32(dst + *o + 8 + len, (uint32_t)crc);
    *o += 12u + len;
}

int cx_png_encode(const unsigned char *px, int w, int h, int channels,
                  unsigned char **out, size_t *out_len)
{
    static const unsigned char sig[8] =
        { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char ihdr[13];
    unsigned char *raw = NULL, *comp = NULL, *png = NULL;
    size_t stride, rawlen, o = 0;
    uLongf complen;
    int y, rc = -1;

    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!px || !out || !out_len || w <= 0 || h <= 0
        || (channels != 3 && channels != 4))
        return -1;

    stride = (size_t)w * (size_t)channels;
    rawlen = (stride + 1u) * (size_t)h;
    raw = (unsigned char *)malloc(rawlen);
    if (!raw)
        goto done;
    for (y = 0; y < h; y++) {
        raw[(stride + 1u) * (size_t)y] = 0;         /* filter type 0 = None */
        memcpy(raw + (stride + 1u) * (size_t)y + 1u,
               px + stride * (size_t)y, stride);
    }

    complen = compressBound((uLong)rawlen);
    comp = (unsigned char *)malloc(complen);
    if (!comp)
        goto done;
    if (compress2(comp, &complen, raw, (uLong)rawlen, Z_DEFAULT_COMPRESSION)
        != Z_OK)
        goto done;

    /* 8 signature + three chunks (12 bytes of framing each) */
    png = (unsigned char *)malloc(8u + (12u + sizeof ihdr)
                                  + (12u + (size_t)complen) + 12u);
    if (!png)
        goto done;
    memcpy(png, sig, 8);
    o = 8;

    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;                                    /* bit depth */
    ihdr[9] = (unsigned char)(channels == 4 ? 6 : 2); /* RGBA : RGB */
    ihdr[10] = 0;                                   /* deflate */
    ihdr[11] = 0;                                   /* filter method 0 */
    ihdr[12] = 0;                                   /* no interlace */
    put_chunk(png, &o, "IHDR", ihdr, sizeof ihdr);
    put_chunk(png, &o, "IDAT", comp, (size_t)complen);
    put_chunk(png, &o, "IEND", NULL, 0);

    *out = png;
    *out_len = o;
    png = NULL;
    rc = 0;

done:
    free(png);
    free(raw);
    free(comp);
    return rc;
}

static int png_put(const char *path, const unsigned char *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    int rc = 0;
    if (!f)
        return -1;
    if (fwrite(bytes, 1, n, f) != n)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

/* ================================================================ the queue */

typedef struct {
    char          *path;
    unsigned char *px;              /* the caller's pixels, copied */
    int            w, h, channels;
    unsigned char *enc;             /* filled by the flush's workers */
    size_t         enclen;
    int            rc;
} png_job;

static int       g_q_depth;         /* begin/flush nesting */
static png_job  *g_q;
static int       g_q_n, g_q_cap;
static size_t    g_q_bytes;
static pthread_t g_q_owner;         /* the thread that opened it */

void cx_png_queue_begin(void)
{
    if (g_q_depth == 0)
        g_q_owner = pthread_self();
    g_q_depth++;
}

static void png_job_free(png_job *j)
{
    free(j->path);
    free(j->px);
    free(j->enc);
    memset(j, 0, sizeof *j);
}

static void png_encode_job(void *ctx, int i)
{
    png_job *j = &((png_job *)ctx)[i];
    j->rc = cx_png_encode(j->px, j->w, j->h, j->channels, &j->enc, &j->enclen);
    free(j->px);                    /* the pixels are spent; the bytes are not */
    j->px = NULL;
}

/* Encode in parallel, write serially in queue order.  Used by the public
 * flush and by the two self-flush triggers below, so there is one drain. */
static int queue_drain(void)
{
    int i, rc = 0;

    if (g_q_n <= 0) {
        g_q_n = 0;
        g_q_bytes = 0;
        return 0;
    }
    cx_pool_for(g_q_n, png_encode_job, g_q);
    for (i = 0; i < g_q_n; i++) {
        png_job *j = &g_q[i];
        if (j->rc != 0 || !j->enc) {
            rc = -1;
        } else if (png_put(j->path, j->enc, j->enclen) != 0) {
            fprintf(stderr, "cannot write %s\n", j->path);
            rc = -1;
        }
        png_job_free(j);
    }
    g_q_n = 0;
    g_q_bytes = 0;
    return rc;
}

int cx_png_queue_flush(void)
{
    int rc = queue_drain();
    if (g_q_depth > 0)
        g_q_depth--;
    if (g_q_depth == 0) {
        free(g_q);
        g_q = NULL;
        g_q_cap = 0;
    }
    return rc;
}

/* Returns 1 when the image was taken by the queue, 0 when the caller should
 * write it itself. */
static int queue_push(const char *path, const unsigned char *px,
                      int w, int h, int channels)
{
    size_t n = (size_t)w * (size_t)h * (size_t)channels;
    png_job *j;
    int i;

    /* THE QUEUE BELONGS TO ONE THREAD.  A stage either walks its fleet on
     * cx_pool_for() (cx_cars_paint.c) or wraps its serial loop in the queue
     * (cx_art_txd.c) -- never both, because the queue's own state is not
     * synchronised.  Rather than trust that by convention, a write arriving
     * from any thread but the one that opened the queue simply takes the
     * direct path, which is always correct. */
    if (g_q_depth <= 0 || !pthread_equal(g_q_owner, pthread_self()))
        return 0;

    /* Rule 1: never let a second write of the same path race the first. */
    for (i = 0; i < g_q_n; i++)
        if (strcmp(g_q[i].path, path) == 0) {
            queue_drain();
            break;
        }
    /* Rule 2: the resident pixel budget. */
    if (g_q_n > 0 && g_q_bytes + n > CX_PNG_QUEUE_BUDGET)
        queue_drain();

    if (g_q_n == g_q_cap) {
        int cap = g_q_cap ? g_q_cap * 2 : 64;
        png_job *nv = (png_job *)realloc(g_q, (size_t)cap * sizeof *nv);
        if (!nv)
            return 0;               /* fall back to the direct write */
        g_q = nv;
        g_q_cap = cap;
    }
    j = &g_q[g_q_n];
    memset(j, 0, sizeof *j);
    j->path = (char *)malloc(strlen(path) + 1u);
    j->px = (unsigned char *)malloc(n ? n : 1u);
    if (!j->path || !j->px) {
        free(j->path);
        free(j->px);
        return 0;
    }
    memcpy(j->path, path, strlen(path) + 1u);
    memcpy(j->px, px, n);
    j->w = w;
    j->h = h;
    j->channels = channels;
    g_q_n++;
    g_q_bytes += n;
    return 1;
}

/* ================================================================ the faces */

static int png_write(const char *path, const unsigned char *px,
                     int w, int h, int channels)
{
    unsigned char *enc = NULL;
    size_t n = 0;
    int rc;

    if (w <= 0 || h <= 0 || (channels != 3 && channels != 4))
        return -1;
    if (queue_push(path, px, w, h, channels))
        return 0;
    if (cx_png_encode(px, w, h, channels, &enc, &n) != 0)
        return -1;
    rc = png_put(path, enc, n);
    free(enc);
    return rc;
}

int cx_png_write_rgba8(const char *path, const unsigned char *pixels,
                       int w, int h)
{
    return png_write(path, pixels, w, h, 4);
}

int cx_png_write_rgb8(const char *path, const unsigned char *pixels,
                      int w, int h)
{
    return png_write(path, pixels, w, h, 3);
}
