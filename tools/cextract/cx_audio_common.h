/* cx_audio_common.h -- agent F's private helpers for the AUDIO family
 * (tools/extract_awd.py, extract_rws.py, extract_xwb.py, extract_eatrax.py).
 *
 * Everything here exists to make the C port reproduce the python originals
 * BYTE FOR BYTE, so several helpers are deliberately literal ports of a
 * python stdlib behaviour rather than "the sane C way":
 *
 *   cxf_write_wav()     the CPython `wave` module's writer, exactly.  A wave
 *                       file it writes is ALWAYS a 44-byte canonical header
 *                       (RIFF/WAVE, 16-byte PCM `fmt `, `data`) followed by
 *                       the payload verbatim -- no RIFF pad byte even when
 *                       the data chunk is odd (Wave_write.close() never adds
 *                       one), and the two length fields hold the number of
 *                       bytes ACTUALLY written: _write_header() first stores
 *                       nframes*blockalign and _patchheader() then overwrites
 *                       both with _datawritten.                          [C]
 *   cxf_splitext()      os.path.splitext(), including the "leading dots are
 *                       not an extension" rule.
 *   cxf_cstr/cxf_field  bytes.index(b"\0")/split(b"\0")[0] + latin-1 decode.
 *   cxf_strip_ascii()   bytes.strip(): b" \t\n\r\v\f" from both ends.
 *   cxf_utf16le_to_utf8 str.decode("utf-16-le", "replace") re-encoded UTF-8,
 *                       which is what python writes to a text file under this
 *                       repo's UTF-8 locale.
 *   cxf_pcm16_stats()   the silence/constant test; the accumulator is exact
 *                       64-bit integer, as python's arbitrary-precision int.
 *
 * The checked little-endian loads return -1 where struct.unpack_from() would
 * raise struct.error, so every caller can turn an over-short file into the
 * same "ERROR: ..." line the python tools print.
 */
#ifndef CX_AUDIO_COMMON_H
#define CX_AUDIO_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ blob */
typedef struct {
    unsigned char *d;
    size_t n;
} cxf_blob;

int  cxf_read_file(const char *path, cxf_blob *out);          /* 0 = ok */
int  cxf_read_head(const char *path, size_t max, cxf_blob *out); /* f.read(n) */
void cxf_blob_free(cxf_blob *b);
long long cxf_file_size(const char *path);                    /* -1 = missing */
int  cxf_mkdir_p(const char *path);

/* Checked LE loads.  0 = ok, -1 = the read runs past the blob, i.e. exactly
 * where python's struct.unpack_from() raises struct.error. */
int cxf_u8 (const cxf_blob *b, size_t o, uint8_t  *v);
int cxf_u32(const cxf_blob *b, size_t o, uint32_t *v);
int cxf_u64(const cxf_blob *b, size_t o, uint64_t *v);

/* python `b.index(b"\0", o)` + latin-1 decode.  -1 if no NUL follows. */
int cxf_cstr(const cxf_blob *b, size_t o, char *out, size_t outsz);
/* python `b[o:o+len].split(b"\0")[0]` + latin-1 decode. */
int cxf_field(const cxf_blob *b, size_t o, size_t len, char *out, size_t outsz);
/* python bytes.strip() -- ASCII whitespace only, both ends, in place. */
void cxf_strip_ascii(char *s);

/* ------------------------------------------------------------- file walk */
typedef struct {
    char  **v;
    size_t  n, cap;
} cxf_strlist;

void cxf_strlist_free(cxf_strlist *l);
int  cxf_strlist_push(cxf_strlist *l, const char *s);
void cxf_strlist_sort(cxf_strlist *l);      /* python list.sort() on str */

/* The python tools' input handling: a directory is os.walk()ed and the files
 * whose lower-cased name ends with one of `exts` are collected; anything that
 * is NOT a directory is taken verbatim as a single input, extension filter
 * bypassed.  Symlinked directories are classified as directories but not
 * descended into, exactly as os.walk(followlinks=False) does. */
int cxf_walk_ext(const char *root, const char *const *exts, size_t nexts,
                 cxf_strlist *out);

/* ------------------------------------------------------------- os.path */
void cxf_splitext(const char *p, char *root, size_t rootsz);
/* path relative to `root` (basename if it is not under it), extension
 * stripped, '/' -> '_'.  This is extract_awd.py's path_tag(). */
int  cxf_path_tag(const char *path, const char *root, char *out, size_t outsz);

/* -------------------------------------------------------------- wave I/O */
int cxf_write_wav(const char *path, const unsigned char *pcm, size_t n,
                  uint32_t rate, uint32_t channels, uint32_t sampwidth);
/* The reader half of python's wave module, enough for ffmpeg's output:
 * walk the RIFF chunks (odd chunks padded), take `fmt ` then `data`, and
 * return nframes*framesize bytes.  0 = ok; *pcm is malloc'd. */
int cxf_read_wav(const char *path, uint32_t *rate, uint32_t *channels,
                 uint32_t *sampwidth, unsigned char **pcm, size_t *n);

/* (rms, non_constant) over s16le bytes; a trailing odd byte is ignored. */
void cxf_pcm16_stats(const unsigned char *pcm, size_t n,
                     double *rms, int *nonconst);

/* ------------------------------------------------------------- subprocess */
int cxf_have_ffmpeg(void);                 /* shutil.which("ffmpeg") != None */
int cxf_run(const char *const argv[]);     /* returncode, -1 = spawn failed */

/* ------------------------------------------------------------------ sha1 */
void cxf_sha1(const unsigned char *d, size_t n, unsigned char out[20]);
void cxf_hex(const unsigned char *d, size_t n, char *out); /* 2n+1 bytes */

/* ------------------------------------------------------------ return code */
/* The python tools exit 1 whenever ANY wave failed a per-wave DATA-QUALITY
 * check -- SILENT, BAD-RATE, OUT-OF-RANGE.  Six waves shipped in this game
 * are genuinely all-zero (one `gear` placeholder and the five `steam` waves
 * in the g_crsh banks), so extract_awd.py always exits 1 on retail data.
 * That is a property of the DATA, not a failed extraction, and the stage ABI
 * in cx_extract.h defines 0 as success, so a stage reports only REAL errors
 * (an unparseable file, a failed write).  Set CX_AUDIO_PY_RC=1 to get the
 * python's stricter exit code back for a CLI-level diff. */
int cxf_rc(int hard_errors, int data_fails);

/* -------------------------------------------------------------- game dir */
/* The driver may hand a global stage a NULL/empty game_dir.  Fall back to
 * $B3_GAME_DIR and then to the path the python tools hard-code. */
const char *cxf_game_dir(const char *game_dir);

/* --------------------------------------------------------------- unicode */
/* UTF-16LE -> UTF-8 with python's "replace" error handler (U+FFFD for a
 * lone surrogate or a trailing odd byte).  Returns 0 on success. */
int  cxf_utf16le_to_utf8(const unsigned char *d, size_t n,
                         char *out, size_t outsz);
/* python str.strip(): Unicode whitespace from both ends of a UTF-8 string. */
void cxf_strip_unicode(char *s);

#ifdef __cplusplus
}
#endif

#endif /* CX_AUDIO_COMMON_H */
