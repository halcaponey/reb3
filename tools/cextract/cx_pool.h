/* cx_pool.h -- the extraction pipeline's parallel-for, and the one lock that
 * makes it legal.
 *
 * ============================================================ WHY IT EXISTS
 * A cold ISO boot is dominated by CPU inside the extraction stages, not by
 * disc I/O.  Measured on this checkout (desktop, warm page cache, the
 * standalone `cxtract --only <stage>` driver, wall / user / sys):
 *
 *     car_paint    5.20 s   4.97 u   0.21 s     107 cars -> paletted PNGs
 *     postfx_art   1.61 s   1.57 u   0.03 s
 *     txd          1.54 s   1.51 u   0.02 s
 *     car_meshes   1.51 s   1.45 u   0.06 s     107 cars -> OBJ/panels/wheels
 *     rws          0.89 s   0.51 u   0.35 s
 *     awd          0.21 s   0.09 u   0.11 s
 *
 * User time IS the wall clock in the top four: those stages are a Morton
 * de-swizzle and a zlib deflate per image, or a few thousand fprintf()s of
 * OBJ text per car.  One core does all of it, on a machine with 36.
 *
 * Every one of those loops is a fleet of INDEPENDENT items -- one car, one
 * texture, one panel set -- writing to its own file.  That is the shape this
 * header parallelises, and nothing else.
 *
 * ======================================================= THE THREE RULES
 * 1. THE ITEM FUNCTION MUST BE ORDER-FREE.  cx_pool_for() hands out indices
 *    in whatever order workers pick them up.  An item may read shared inputs
 *    and write ITS OWN output file; it may not append to a shared list, bump
 *    a shared counter, or printf.  Per-item results go into caller-owned
 *    slot `i` of an array, and the caller consumes that array IN INDEX ORDER
 *    afterwards -- which is why the stdout transcript of a parallel stage is
 *    byte-identical to the serial one, not merely equivalent.
 *
 * 2. THE SOURCE IS NOT THREAD-SAFE, so it is locked.  CxSrc caches directory
 *    extents and mmap'd regions lazily (cx_src.c, cxs_dcache / cxs_ext), and
 *    two threads growing those caches at once corrupts them.  cx_src.c takes
 *    cx_pool_lock() at every cx_vfs_* and cx_src_* entry point; an item that
 *    reads the disc through cxd_read_file() is therefore already safe, and
 *    the read is serialised on purpose -- it is the cheap half.
 *
 * 3. ONE THREAD MEANS NO THREAD.  With `B3_JOBS=1`, a single-item loop, or a
 *    build without pthreads, cx_pool_for() calls `fn` inline on the calling
 *    thread and no thread is ever created.  That keeps the serial path -- the
 *    one the byte-identity gate was written against -- literally the same
 *    code, and gives every stage a one-line way to bisect a suspicion.
 *
 * ============================================================ THE OUTPUTS
 * Byte identity is unaffected BY CONSTRUCTION, not by luck: each item writes
 * files no other item names, zlib is deterministic, and the summary lines are
 * replayed in index order.  `tools/cextract/verify_cextract.py` is still the
 * gate; two consecutive cold extractions still diff clean.
 *
 * =================================================================== WEB
 * Under Emscripten the worker count defaults to 3 rather than nproc: the link
 * runs -sPROXY_TO_PTHREAD with a pool of 8, one of which is main(), and
 * growing past the pre-spawned pool needs the browser's main thread to build
 * a worker while this thread waits on it.  CPU work on already-read buffers
 * is what parallelises there; file I/O still proxies to the main thread (see
 * docs/web/ and git cb7f8d2), so a stage that wants the web win keeps the
 * WRITE on the calling thread and puts only the encode in the item.
 */
#ifndef CX_POOL_H
#define CX_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* How many threads cx_pool_for() will really use.  1 means the inline path.
 *   $B3_JOBS   explicit count (1 = serial), clamped to [1, CX_POOL_MAX]
 *   unset      the machine's CPU count, capped at 8 (3 under Emscripten)
 * Resolved once, then cached. */
int cx_pool_workers(void);

/* Run fn(ctx, i) for i in [0, n), on cx_pool_workers() threads, and return
 * when every one of them has finished.  `fn` must obey rule 1 above. */
void cx_pool_for(int n, void (*fn)(void *ctx, int i), void *ctx);

/* THE SOURCE LOCK (rule 2).  Recursive: cx_vfs_fopen() calls cx_src_map(),
 * and both take it.  Held only around the source layer -- never around an
 * item's decode or its write, or the parallelism would be for nothing. */
void cx_pool_lock(void);
void cx_pool_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* CX_POOL_H */
