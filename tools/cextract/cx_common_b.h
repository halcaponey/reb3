/* cx_common_b.h -- agent B's private helpers: file/blob access, the Burnout 3
 * texture-record walk (static.dat, enviro.dat and the Data/<name>.txd banks all use
 * the SAME record) and the image decoders.
 *
 * Provenance, condensed from tools/extract_textures.py and tools/extract_txd.py
 * (format credit: Burnout Modding community / EdnessP's fmt_Burnout3LRD.py):
 *
 *   texture record, little-endian
 *     +0x00  u32  D3DResource.Common            [C] it IS a D3DPixelContainer
 *     +0x04  i32  bitmap data offset, RELATIVE to the record
 *     +0x0C  u32  NV2A TX_FORMAT word (COLOR bits 8..15 == +0x34)
 *     +0x14  u32[n] palette record offsets, relative to the record (fmt 0xB)
 *     +0x34  u32  format: 0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA
 *     +0x38  u32  width      +0x3C  u32  height
 *     +0x40  u32  bit depth -- 4/8/32 => name at +0x48, else name at +0x44
 *     +0x69  u8   palette count
 *   palette record: {u16 1, u16 3 or 0xC003}, u32 data offset rel. the TEXTURE
 *     record; data = 256 (bd 8) or 16 (bd 4) x BGRA.
 *
 *   V-ORIGIN RULE: the shipped bytes at +0x04 ARE the GPU surface and NV2A
 *   TX_FORMAT has no origin/flip field, so surface row 0 == v 0 == PNG row 0.
 *   No decoder here flips, and no consumer may.                            [C]
 */
#ifndef CX_COMMON_B_H
#define CX_COMMON_B_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ blob */
typedef struct {
    unsigned char *d;
    size_t n;
} cxb_blob;

int  cxb_read_file(const char *path, cxb_blob *out);   /* 0 = ok */
void cxb_blob_free(cxb_blob *b);
int  cxb_mkdir_p(const char *path);
int  cxb_file_exists(const char *path);

/* Bounds-checked little-endian accessors.  Out-of-range reads yield 0; the
 * callers replicate the python parsers' own explicit range guards. */
uint8_t  cxb_u8 (const cxb_blob *b, size_t o);
int8_t   cxb_i8 (const cxb_blob *b, size_t o);
uint16_t cxb_u16(const cxb_blob *b, size_t o);
uint32_t cxb_u32(const cxb_blob *b, size_t o);
int32_t  cxb_i32(const cxb_blob *b, size_t o);
float    cxb_f32(const cxb_blob *b, size_t o);
/* Reader.ptr(): a relative pointer -- zero stays zero, otherwise add `rel`. */
int64_t  cxb_ptr(const cxb_blob *b, size_t o, int64_t rel);

/* read_cstr(): NUL-terminated ASCII, at most `limit` characters, and NOT a
 * name at all if any byte is >= 0x80 (python's bytes.decode('ascii') raises).
 * Returns 0 on success. */
int cxb_read_cstr(const cxb_blob *b, int64_t off, size_t limit,
                  char *out, size_t outsz);
/* os.path.basename(name.replace('\\', '/')) */
void cxb_basename(const char *in, char *out, size_t outsz);

/* --------------------------------------------------------------- formats */
#define CXB_FMT_P8   0x0Bu
#define CXB_FMT_DXT1 0x0Cu
#define CXB_FMT_DXT3 0x0Eu
#define CXB_FMT_DXT5 0x0Fu
#define CXB_FMT_RGBA 0x3Au
const char *cxb_fmt_name(unsigned fmt);

/* One parsed texture record. */
typedef struct {
    size_t   rec;          /* record offset in the blob */
    unsigned fmt;
    int      w, h;
    int      depth;        /* +0x40 */
    int      npal;         /* +0x69 */
    int64_t  data_off;     /* absolute bitmap offset */
    char     name[80];     /* the STORED name, not the basename */
} cxb_texrec;

/* Parse + validate a record the way tools/extract_envmap.py's parse_record()
 * does (format known, power-of-two dims <= 4096, alnum/_/. name, in-range
 * bitmap offset).  Returns 0 on success. */
int cxb_parse_texrec(const cxb_blob *b, int64_t rec, cxb_texrec *out);

/* -------------------------------------------------------------- decoders */
/* All write w*h*4 RGBA8, row 0 first.  Return 0 on success. */
int cxb_decode_dxt(const cxb_blob *b, int64_t off, int w, int h, unsigned fmt,
                   unsigned char *out);
int cxb_decode_rgba(const cxb_blob *b, int64_t off, int w, int h,
                    unsigned char *out);
/* fmt 0xB: Morton/Z-order 8bpp indices + BGRA palette. */
void cxb_unswizzle8(const unsigned char *in, int w, int h, unsigned char *out);
int  cxb_decode_paletted(const cxb_blob *b, size_t rec, int64_t bmp,
                         int w, int h, int bd, int which_pal,
                         unsigned char *out);

/* --------------------------------------------------------------- txd walk */
/* Data/Frontend.txd, Data/Global.txd: a flat Criterion container, NOT a
 * RenderWare stream TXD.
 *   +0x00 u32 0x543C0000  +0x04 u32 0xBCDEED81  +0x08 u32 count
 *   +0x0C u32 entrySize = 16
 *   +0x10 count x {u32 id (1-based), u32 0, u32 absoluteOffset, u32 0}
 * The offsets point at the same texture record as above.               [S] */
int cxb_txd_count(const cxb_blob *b, uint32_t *count_out);   /* 0 = ok */
int cxb_txd_entry_offset(const cxb_blob *b, uint32_t i, int64_t *off_out);

#ifdef __cplusplus
}
#endif

#endif /* CX_COMMON_B_H */
