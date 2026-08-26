/* cx_common_g.h -- agent G's private helpers for the GENERATOR family.
 *
 * Everything here is prefixed `cxg_`.  The generators under tools/gen_*.py are
 * not asset extractors: they read the retail image, the shipped .bgd files and
 * the PORT'S OWN SOURCE ANNOTATIONS, and they emit C headers plus a stdout
 * report.  The bar is the same as the rest of tools/cextract: the emitted text
 * must be BYTE-IDENTICAL to the python original's, comment text included.
 *
 * Four jobs, in the order the generators need them:
 *
 *   1. A GROWABLE TEXT BUFFER.  python builds a list of lines and joins it;
 *      the C side appends formatted lines to a cxg_str and writes it once.
 *      Every generator's output is assembled this way so the trailing-newline
 *      rule of each python original ("\n".join(L) vs + "\n") is explicit.
 *
 *   2. A WHOLE-FILE READER, text and binary.  python's open().read() either
 *      succeeds or raises; a short read that silently yields a truncated
 *      header is exactly the failure mode this port must not have, so every
 *      accessor is bounds-checked and refuses rather than reading past the end.
 *
 *   3. THE MAPPED IMAGE.  tools/py_extract_archive/extract_tlist.py's Image:
 *      the ELF program headers are walked and a VA is resolved through the
 *      PT_LOAD segment that contains it.  Never a flat load -- HANDOFF.md
 *      section 2: a flat load silently reads the wrong bytes.
 *
 *   4. GLOBALUS.BIN.  u32 count at +0x08, u32 offset table at +0x10, UTF-16LE
 *      payload terminated by a u16 0.  Decoded to UTF-8 the way python's
 *      .decode('utf-16-le', 'replace') then a UTF-8 write() does.
 *
 * Plus the two path utilities the reports need: os.path.relpath (the generators
 * print relative paths, so the message text depends on it) and mkdir -p.
 */
#ifndef CX_COMMON_G_H
#define CX_COMMON_G_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------- text */
typedef struct {
    char  *p;
    size_t n, cap;
    int    err;                 /* sticky: an allocation failed */
} cxg_str;

void cxg_sputs(cxg_str *s, const char *t);
void cxg_sputc(cxg_str *s, char c);
/* Appends the formatted text.  No newline is added -- the python originals are
 * explicit about where their newlines are, and so is this. */
void cxg_sprintf(cxg_str *s, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* python's `w(line)` on a list later joined with "\n": the text plus one \n. */
void cxg_sline(cxg_str *s, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* python's `w("")` -- one empty line.  Spelled out rather than
 * cxg_sline(s, ""), which is a zero-length printf format and warns. */
static inline void cxg_nl(cxg_str *s) { cxg_sputc(s, '\n'); }
void cxg_str_free(cxg_str *s);
/* 0 on success.  Creates the parent directory chain. */
int  cxg_str_write(const cxg_str *s, const char *path);
/* Byte-compare the buffer against the file at `path`; 1 if identical.  A
 * missing file compares against "" -- gen_trackselect.py --check's
 * `open(OUT).read() if os.path.exists(OUT) else ""`. */
int  cxg_str_matches_file(const cxg_str *s, const char *path);

/* ------------------------------------------------------------------- blob */
typedef struct {
    unsigned char *d;
    size_t         n;
    const char    *what;
    int            oob;         /* count of refused reads */
} cxg_blob;

int  cxg_blob_load(cxg_blob *b, const char *path);
void cxg_blob_free(cxg_blob *b);
/* Non-zero once any accessor has refused a read -- the loud equivalent of
 * python's struct.error, checked once at the end of a stage. */
static inline int cxg_oob(const cxg_blob *b) { return b->oob; }
uint8_t  cxg_u8 (cxg_blob *b, int64_t o);
uint32_t cxg_u32(cxg_blob *b, int64_t o);
uint64_t cxg_u64(cxg_blob *b, int64_t o);
float    cxg_f32(cxg_blob *b, int64_t o);
/* Read the whole file as NUL-terminated text.  Caller free()s.  NULL on
 * failure; *n_out (optional) gets the byte count, excluding the NUL. */
char *cxg_read_text(const char *path, size_t *n_out);

/* ------------------------------------------------------------------ image */
typedef struct {
    uint32_t va, off, fsz;
} cxg_seg;

typedef struct {
    cxg_blob raw;
    cxg_seg  seg[64];
    int      nseg;
} cxg_image;

/* extract_tlist.Image: PT_LOAD segments only, section-aware, never flat. */
int  cxg_image_open(cxg_image *im, const char *path);
void cxg_image_close(cxg_image *im);
/* NULL when the span is not fully inside a mapped segment -- python's
 * KeyError("VA %#x not mapped"). */
const unsigned char *cxg_image_read(cxg_image *im, uint32_t va, size_t n);

/* --------------------------------------------------------------- globalus */
typedef struct {
    cxg_blob raw;
    uint32_t count;
} cxg_globalus;

int  cxg_globalus_open(cxg_globalus *g, const char *path);
void cxg_globalus_close(cxg_globalus *g);
/* UTF-16LE -> UTF-8, unpaired surrogates and a truncated tail becoming U+FFFD
 * exactly as python's 'replace' handler does.  Returns `out`. */
char *cxg_globalus_get(cxg_globalus *g, uint32_t idx, char *out, size_t cap);

/* ----------------------------------------------------------------- base-40 */
/* FUN_001AECC0 as tools/py_extract_archive/extract_tlist.py decodes it: LSB
 * char first, then REVERSED, then stripped.  "US_C3_V1".            [C] */
char *cxg_b40(uint64_t v, char *out /* >= 13 bytes */);
/* tools/gen_ai_pace.py's b40 -- the same charset with NO reversal, which is
 * why its event ids read "FCRGSFFO" where the tlist decoder reads "OFFSGRCF".
 * Both are in the tree; each generator keeps the one it shipped with. */
char *cxg_b40_raw(uint64_t v, char *out /* >= 13 bytes */);

/* ------------------------------------------------------------------ paths */
int  cxg_mkdir_p(const char *path);
void cxg_join(char *out, size_t cap, const char *a, const char *b);
/* WHERE A GENERATED HEADER GOES.  `out_root` is the pipeline's repo-root
 * stand-in -- extracted assets land under <out_root>/build/..., and generated
 * headers under <out_root>/gen/, the project-wide convention.  $B3_GEN_OUT
 * (the driver's --gen-out) overrides the directory outright, which is how a
 * caller aims one generator at a scratch path for an oracle diff without
 * moving the rest of the run. */
void cxg_gen_path(char *out, size_t cap, const char *out_root,
                  const char *filename);
/* os.path.relpath(path, start), both made absolute and normalised first.
 * The generators print it, so the report text depends on it. */
void cxg_relpath(char *out, size_t cap, const char *path, const char *start);
/* The port's repo root: $B3_REPO_DIR, else the compiled-in default.  The
 * generators read the port's own annotated headers under src, plus
 * tools/emulate_sfx.py and build/burnout3.elf -- none of which live under the
 * GAME directory the stage ABI hands them, so this is a SECOND root.  The
 * driver's --repo sets the variable; nothing here ever writes inside it. */
/* The checkout this runs out of.  No absolute path is compiled in -- the
 * drivers here are meant to be run from the repository root, and --repo /
 * $B3_REPO_DIR override it when they are not. */
#ifndef CXG_DEFAULT_REPO_DIR
#  define CXG_DEFAULT_REPO_DIR "."
#endif
const char *cxg_repo_root(void);

/* ------------------------------------------------------------- C literals */
/* gen_trackselect.py's cstr(): '"' + s with \ and " escaped + '"'. */
char *cxg_cstr(const char *s, char *out, size_t cap);

/* ---------------------------------------------------------- text scanning */
/* The parity-assertion readers.  The python generators use small regexes over
 * the port's own headers; these are the same matches, hand-rolled, so the
 * emitted tables stay derived from the assertions rather than typed in.
 *
 *   cxg_scan_assert:  _Static_assert(offsetof(<struct>, <name>) == <off>
 *                     `need_0x` mirrors the two spellings in the tree --
 *                     gen_ai_ranges.py requires the 0x prefix, while
 *                     gen_vehicle_ranges.py's `(?:0x)?` does not (the rigid
 *                     body is asserted as `== 0`, and requiring the prefix is
 *                     what once dropped the whole body from the transfer).
 * Returns the number of hits, in FILE ORDER, capped at `cap`. */
typedef struct {
    char     name[96];
    uint32_t off;
} cxg_assert_hit;

int cxg_scan_assert(const char *src, const char *struct_name, int need_0x,
                    cxg_assert_hit *out, int cap);
/* python's `("offsetof(B3VehicleFull, %s)" % base) in src` membership test. */
int cxg_has_assert_for(const char *src, const char *struct_name,
                       const char *member);

#endif /* CX_COMMON_G_H */
