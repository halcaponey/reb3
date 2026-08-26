/* cx_png.h -- minimal PNG writer (RGBA8 / RGB8) over system zlib.
 *
 * Hand-written IHDR/IDAT/IEND chunks; scanlines carry filter type 0 and are
 * deflated with zlib's compress2().  CRCs come from zlib's crc32().
 * Row 0 of `pixels` is written as row 0 of the PNG -- the repo-wide V-origin
 * rule (tools/extract_textures.py: the shipped bytes ARE the GPU surface and
 * NV2A TX_FORMAT has no origin bit, so surface row 0 == v 0 == PNG row 0).
 *
 * ============================================================== THE QUEUE
 * The deflate is where the art stages spend their time -- txd is 1.54 s of
 * which 1.51 s is user CPU for 808 images, postfx_art 1.61 s for 145 -- and
 * it is pure computation over a buffer the caller already has.  But those
 * stages cannot simply run their loops on a worker pool: extract_bank() in
 * cx_art_txd.c resolves cross-bank NAME COLLISIONS in visit order and appends
 * to a FAILURES list, and both would come out differently under a race.
 *
 * So the parallelism goes UNDER them instead of around them.  Between
 * cx_png_queue_begin() and cx_png_queue_flush(), cx_png_write_*() COPIES the
 * pixels, records the path, and returns success immediately; the flush then
 * encodes every queued image on cx_pool_for() and writes the finished byte
 * streams out afterwards, IN QUEUE ORDER, one fwrite each.  The stage's own
 * loop stays exactly the serial code the byte-identity gate was written
 * against, and the encoder is deterministic, so the files are identical.
 *
 * Three rules the queue enforces so that "identical" is by construction:
 *   * a path queued TWICE flushes first, so a later write can never overtake
 *     the earlier one it was meant to replace;
 *   * the resident pixel budget is capped (CX_PNG_QUEUE_BUDGET), and hitting
 *     it flushes -- an unbounded queue over 808 images is hundreds of MB;
 *   * begin/flush nest by count, so a stage that wraps a helper which also
 *     wraps cannot flush its caller's queue out from under it.
 * A write with no queue open behaves exactly as it always did.
 *
 * WEB: the encode is what runs on workers; the fwrite stays on the calling
 * thread, because under -sPROXY_TO_PTHREAD a write() from a worker is a
 * blocking round trip to the browser's main thread (git cb7f8d2).  One
 * fwrite per file also replaces the three the chunk writer used to make.
 */
#ifndef CX_PNG_H
#define CX_PNG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pixels: w*h*4 bytes, R,G,B,A.  Returns 0 on success. */
int cx_png_write_rgba8(const char *path, const unsigned char *pixels,
                       int w, int h);
/* pixels: w*h*3 bytes, R,G,B.  Returns 0 on success. */
int cx_png_write_rgb8(const char *path, const unsigned char *pixels,
                      int w, int h);

/* THE QUEUE (see the header comment).  Nestable; every begin needs a flush.
 * A queued write returns 0 for "accepted", and a failure to encode or write
 * is reported by the flush that does it -- which is why a stage that must
 * distinguish per-file failure should not queue. */
void cx_png_queue_begin(void);
/* Encode every queued image (in parallel), write them in queue order, and
 * empty the queue.  Returns 0 when all of them succeeded. */
int  cx_png_queue_flush(void);

/* Pure encode, no file: the queue's own worker body, exposed because it is
 * the only part that is safe to run off the calling thread.  *out is
 * malloc()ed and belongs to the caller. */
int cx_png_encode(const unsigned char *pixels, int w, int h, int channels,
                  unsigned char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* CX_PNG_H */
