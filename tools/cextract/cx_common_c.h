/* cx_common_c.h -- agent C's private helpers for the cextract port.
 *
 * Everything here is prefixed `cxc_`.  Two jobs:
 *
 *   1. A BOUNDS-CHECKED little-endian reader over a whole file.  The python
 *      spec reads with struct.unpack_from, which RAISES on a short read; a C
 *      port that silently reads past the end would produce a plausible-looking
 *      but wrong .bin.  So every accessor takes a signed 64-bit offset (the
 *      file's own relative pointers are i32 and can go negative), returns 0 if
 *      the span is not fully inside the blob, and bumps `oob`.  Callers check
 *      cxc_oob() once at the end and fail the stage -- the loud equivalent of
 *      python's traceback.
 *
 *   2. A growable output buffer whose put_* are FIELD-BY-FIELD little-endian
 *      with explicit-width types.  No struct is ever fwrite()n: the .bin
 *      layouts are the python struct.pack format strings, and they must not
 *      pick up host padding or host endianness.
 */
#ifndef CX_COMMON_C_H
#define CX_COMMON_C_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ blob */
typedef struct {
    const uint8_t *d;
    size_t         n;
    const char    *what;   /* file name, for the out-of-range message */
    int            oob;    /* count of refused reads */
} cxc_blob;

/* Read `path` whole.  0 on success; blob->d must be free()d via cxc_blob_free. */
int  cxc_blob_load(cxc_blob *b, const char *path);
void cxc_blob_free(cxc_blob *b);
/* A window onto an existing blob -- python's `st[lo:lo+ls]`.  Shares storage,
 * so it must NOT be freed.  Clamps to the parent, exactly as a slice does. */
cxc_blob cxc_blob_slice(const cxc_blob *b, int64_t off, int64_t len);
static inline int cxc_oob(const cxc_blob *b) { return b->oob; }

uint8_t  cxc_u8 (cxc_blob *b, int64_t o);
int8_t   cxc_i8 (cxc_blob *b, int64_t o);
uint16_t cxc_u16(cxc_blob *b, int64_t o);
int16_t  cxc_i16(cxc_blob *b, int64_t o);
uint32_t cxc_u32(cxc_blob *b, int64_t o);
int32_t  cxc_i32(cxc_blob *b, int64_t o);
float    cxc_f32(cxc_blob *b, int64_t o);
/* extract_track.py Reader.ptr: a relative pointer; zero stays zero. */
int64_t  cxc_ptr(cxc_blob *b, int64_t o, int64_t rel);
/* python `d.find(b'\0', o)` + `.decode('ascii')`, with read_cstr's limit.
 * Writes at most `cap` bytes incl. NUL into `out`.  Returns 0 if the string is
 * absent, longer than `limit`, or not 7-bit ASCII -- read_cstr's None. */
int cxc_cstr(cxc_blob *b, int64_t o, int limit, char *out, size_t cap);

/* ---------------------------------------------------------------- buffer */
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    int      err;          /* sticky: an allocation failed */
} cxc_buf;

void cxc_buf_free(cxc_buf *b);
void cxc_put_bytes(cxc_buf *b, const void *src, size_t n);
void cxc_put_zero (cxc_buf *b, size_t n);
void cxc_put_u8   (cxc_buf *b, uint8_t  v);
void cxc_put_u16  (cxc_buf *b, uint16_t v);
void cxc_put_u32  (cxc_buf *b, uint32_t v);
void cxc_put_i32  (cxc_buf *b, int32_t  v);
void cxc_put_f32  (cxc_buf *b, float    v);
/* Zero-fill until the buffer is exactly `off` long (python's
 * `out += b'\0' * (off - len(out))`). */
void cxc_pad_to   (cxc_buf *b, size_t off);
int  cxc_buf_write(const cxc_buf *b, const char *path);

/* ------------------------------------------------------------ misc glue */
/* os.path.basename(s.replace('\\', '/')) */
void   cxc_basename(const char *s, char *out, size_t cap);
/* "a/b", into a caller buffer. */
void   cxc_join(char *out, size_t cap, const char *a, const char *b);
int    cxc_mkdir_p(const char *path);
double cxc_now_s(void);

/* The id -> directory half of tools/extract_tlist.py's resolve(), used only
 * until agent A's cx_resolve_track lands.  FUN_001574F0 [C]: the packed id
 * "<REG>_<Cn>_<Vn>" is split into "Tracks/<REG>/<Cn>_<Vn>/" by position --
 * chars 0..1, 3..4, 6..7.  No tlist.bin decode (that is agent A's module). */
int cxc_resolve_track(const char *game_dir, const char *spec,
                      char *out_id, size_t id_sz,
                      char *out_dir, size_t dir_sz);

#endif /* CX_COMMON_C_H */
