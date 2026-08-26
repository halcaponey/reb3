/* cx_src.h -- THE SOURCE-ACCESS LAYER: read the game dump either from an
 * expanded directory or straight out of the Xbox ISO, with the same calls.
 *
 * =========================================================== THE CONTRACT ==
 * The five functions agent B links against are EXACTLY as specified.  They
 * are the whole of the public promise; everything below them in this header
 * is additive and is called out as such:
 *
 *     typedef struct CxSrc CxSrc;
 *     CxSrc* cx_src_open (const char *path_or_dir);
 *     void   cx_src_close(CxSrc *);
 *     int    cx_src_stat (CxSrc *, const char *rel, unsigned long long *sz);
 *     const void* cx_src_map(CxSrc *, const char *rel, unsigned long long *n);
 *     long   cx_src_read (CxSrc *, const char *rel, unsigned long long off,
 *                         void *buf, long len);
 *
 * >>> DEVIATIONS FROM THE HANDED-DOWN CONTRACT, STATED LOUDLY <<<
 *
 *   (1) NOTHING WAS REMOVED OR RESHAPED.  The five signatures above are
 *       byte-for-byte what was specified, including argument order and the
 *       0-on-success convention of cx_src_stat.
 *
 *   (2) cx_src_map IS IMPLEMENTED FOR **BOTH** BACKENDS.  The contract says
 *       it "may return NULL for dir backend"; it does not.  The directory
 *       backend mmap()s the individual file and caches the mapping for the
 *       life of the CxSrc, so a caller can rely on map() succeeding whatever
 *       the source is.  Returning NULL was permitted, not required, and a
 *       uniform zero-copy path is worth more than the permission.
 *       (It still returns NULL for a directory, a missing path, or an
 *       mmap failure -- the failure contract is unchanged.)
 *
 *   (3) ADDITIONS, all clearly namespaced and all optional:
 *         cx_src_kind / cx_src_root  -- ask what was opened
 *         cx_src_is_dir              -- stat() cannot say "directory"
 *         cx_src_list / cx_src_list_free
 *                                    -- the pipeline walks pveh/ and Tracks/
 *                                       with opendir(); an image needs an
 *                                       equivalent, so one is provided
 *         cx_vfs_*                   -- the PATH-PREFIX SHIM (see below)
 *       Agent B may ignore every one of them and still link.
 *
 * ============================================================== THE SHIM ==
 * The extraction stages take `const char *game_dir` and build absolute paths
 * out of it ("%s/pveh/vlist.bin").  Rewriting ~60 join sites into rel-path
 * calls would be a large, risky diff for no gain, so the stage seam is:
 *
 *     THE STRING ABI STAYS.  `game_dir` may now name an ISO instead of a
 *     directory.  The driver opens ONE CxSrc for it and binds it with
 *     cx_vfs_bind(); the low-level primitives (file slurp, stat, opendir)
 *     then ask cx_vfs_* instead of libc.  cx_vfs_* strips the bound root
 *     prefix off the path and serves the remainder out of the CxSrc; a path
 *     that is NOT under the bound root -- an output file, a repo header,
 *     build/burnout3.elf -- falls through to plain libc untouched.
 *
 * Consequences worth stating, because the byte-identity gate rests on them:
 *   * When the source IS a directory, cx_vfs_bind() binds nothing and every
 *     cx_vfs_* call is a straight libc passthrough.  Directory extraction
 *     therefore executes exactly the code it executed before this layer
 *     existed -- it cannot have changed.
 *   * cx_vfs_listdir() returns entries in SOURCE order (readdir order for a
 *     directory, in-order dirent-tree order for an image) and never returns
 *     "." or "..".  Every walk in the pipeline already skipped those two and
 *     already sorts its results, so order is not observable in any output.
 *
 * ====================================================== THE IMAGE FORMAT ==
 * [S] XISO, as verified against this dump (all 716 files and 55 directories
 *     byte-identical to the expanded dump):
 *
 *   sector = 2048 bytes.
 *   Volume descriptor at sector 32 (0x10000) of the partition:
 *       +0x00  char[20] "MICROSOFT*XBOX*MEDIA"
 *       +0x14  u32      root directory start sector
 *       +0x18  u32      root directory size in bytes
 *   A directory extent is a BINARY TREE of dirents, each 4-byte aligned:
 *       +0x00  u16  left  subtree offset, in DWORDS from the extent start
 *       +0x02  u16  right subtree offset, in DWORDS
 *       +0x04  u32  start sector
 *       +0x08  u32  size in bytes
 *       +0x0C  u8   attributes (0x10 = directory)
 *       +0x0D  u8   name length
 *       +0x0E  char[name length] name (no NUL)
 *   0xFFFF (and 0, which is the extent's own root) means "no child".
 *   Files are CONTIGUOUS extents: start_sector*2048, size bytes.  There is
 *   no fragmentation and no run list, which is what makes mmap + pointer
 *   arithmetic the whole of the read path.
 *
 * [S] DELIBERATELY NOT IMPLEMENTED: binary-tree DESCENT.  Microsoft's
 *     collation order for these trees is subtle, and a descent with the
 *     wrong comparator takes a wrong branch and reports "not found" for a
 *     file that is right there -- silently.  Every directory here is
 *     flattened in-order in full and searched with a linear
 *     case-insensitive scan.  The largest directory in this dump is a few
 *     hundred entries and each is cached after its first read, so the scan
 *     costs nothing measurable.
 *
 * House style: C11, explicit-width types, -Wall -Wextra clean.
 */
#ifndef CX_SRC_H
#define CX_SRC_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================== THE CONTRACT == */

typedef struct CxSrc CxSrc;

/* Auto-detects: a directory opens the directory backend, a regular file is
 * probed for the XISO volume descriptor and opens the image backend.
 * Returns NULL if the path is neither. */
CxSrc *cx_src_open(const char *path_or_dir);

void   cx_src_close(CxSrc *s);

/* 0 on success, non-zero if `rel_path` does not resolve.  `out_size` may be
 * NULL.  Path matching is case-insensitive and '/'-separated; a leading '/'
 * and "." components are ignored.  A directory stats fine, with size 0 --
 * use cx_src_is_dir() to tell the two apart. */
int    cx_src_stat(CxSrc *s, const char *rel_path,
                   unsigned long long *out_size);

/* Zero-copy pointer to the whole file, valid until cx_src_close().  NULL if
 * the path does not resolve or names a directory.  `out_len` may be NULL.
 * A zero-length file maps to a non-NULL pointer with *out_len == 0. */
const void *cx_src_map(CxSrc *s, const char *rel_path,
                       unsigned long long *out_len);

/* Bytes actually copied (short at EOF), or -1 on error. */
long   cx_src_read(CxSrc *s, const char *rel_path, unsigned long long off,
                   void *buf, long len);

/* ========================================================== ADDITIONS == */

enum { CX_SRC_DIR = 0, CX_SRC_XISO = 1 };

int         cx_src_kind(const CxSrc *s);       /* CX_SRC_DIR / CX_SRC_XISO */
const char *cx_src_root(const CxSrc *s);       /* the spec it was opened on */
int         cx_src_is_dir(CxSrc *s, const char *rel_path);   /* 1 = yes */

/* One directory listing.  `is_dir[i]` matches `name[i]`.  Entries come back
 * in SOURCE order and never include "." or "..". */
typedef struct {
    char    **name;
    uint8_t  *is_dir;
    int       n;
    int       cap;
} CxSrcList;

int  cx_src_list(CxSrc *s, const char *rel_dir, CxSrcList *out);   /* 0 = ok */
void cx_src_list_free(CxSrcList *l);

/* =============================================================== SHIM == */

/* Bind `src` as the process-wide source rooted at the string `root_spec`
 * (whatever the driver put in `game_dir`).  A CX_SRC_DIR source binds
 * nothing and leaves every cx_vfs_* call a libc passthrough.  Returns 0. */
int   cx_vfs_bind(CxSrc *src, const char *root_spec);

/* The bound image source, or NULL when nothing is bound (i.e. passthrough).
 * Also performs the one-shot autobind from $B3_ISO described below. */
CxSrc *cx_vfs_src(void);

/* If `path` lies under the bound root, the remainder ('/'-relative, possibly
 * ""); otherwise NULL.  NULL whenever nothing is bound. */
const char *cx_vfs_rel(const char *path);

/* libc-shaped primitives.  Each serves the bound image when `path` is under
 * the bound root, and calls libc otherwise.
 *
 * cx_vfs_fopen honours READ modes only ("r"/"rb"); any mode that can write
 * is passed to fopen() unconditionally, because nothing writes into the
 * source.  An image-backed read stream is an fmemopen() over the mapped
 * extent, so fseek/fseeko/ftell/fread behave exactly as on a real file. */
FILE *cx_vfs_fopen(const char *path, const char *mode);
/* 0 on success.  Either out pointer may be NULL. */
int   cx_vfs_stat(const char *path, unsigned long long *out_size,
                  int *out_is_dir);
int   cx_vfs_is_dir(const char *path);
int   cx_vfs_is_file(const char *path);         /* regular file */
int   cx_vfs_exists(const char *path);

/* Source order, no "." / "..".  0 = ok (a non-directory is an error, as
 * opendir() would be). */
int   cx_vfs_listdir(const char *path, CxSrcList *out);

/* $B3_ISO, consulted lazily by every cx_vfs_* call the first time one runs
 * without a binding, so a module driven outside cxtract still reads the
 * image.  $B3_GAME_DIR is accepted too when it names a regular file rather
 * than a directory.  An explicit cx_vfs_bind() always wins. */
#define CX_SRC_ENV_ISO  "B3_ISO"
#define CX_SRC_ENV_GAME "B3_GAME_DIR"

#ifdef __cplusplus
}
#endif

#endif /* CX_SRC_H */
