/* cx_art_common.h -- agent E's private helpers for the ART/FRONTEND family
 * (tools/extract_txd.py, extract_font.py, extract_carfx_art.py,
 * extract_postfx_art.py, extract_boostfx_art.py, extract_particlefx_art.py).
 *
 * Everything format-related is REUSED from cx_common_b.[ch]: the texture
 * record walk, the DXT/paletted decoders, the Morton unswizzler and the flat
 * .txd container header are already there and already proven against these
 * exact banks.  What lives here is only what the art tools add on top:
 *
 *   * read_name() with the PRINTABLE test (extract_txd.py / carfx's
 *     `all(32 <= ord(c) < 127 ...)`), which is a different predicate from
 *     cx_common_b's parse_record isalnum() test,
 *   * extract_carfx_art.py's loose decode_record(),
 *   * the "pull these names out of a .txd bank" driver shared verbatim by
 *     extract_carfx_art / extract_boostfx_art / extract_particlefx_art,
 *   * python's os.listdir()+sorted(), os.path.relpath() and round(),
 *   * extract_font.py's Image32 (an ELF32 PT_LOAD VA mapper).
 *
 * ===================================================== THE OUTPUT ROOT
 * The dump-global entry points take (game_dir, out_root).  `out_root` stands
 * in for the REPO ROOT, and every tool writes exactly the repo-relative path
 * its python original writes:
 *
 *     <out_root>/build/frontend/<name>.png  extract_txd.py, extract_font.py
 *     <out_root>/src/burnout3_font.h       extract_font.py
 *     <out_root>/build/carfx/           extract_carfx_art.py
 *     <out_root>/build/cars/<car>.lights   extract_carfx_art.py
 *     <out_root>/build/postfx/          extract_postfx_art.py
 *     <out_root>/build/boostfx/<n>.png     extract_boostfx_art.py
 *     <out_root>/build/particlefx/<n>.png  extract_particlefx_art.py
 *
 * so an oracle diff is a plain directory diff.  extract_postfx_art.py's
 * manifest records os.path.relpath(<abs out>) -- i.e. paths relative to the
 * PROCESS CWD -- so cxe_relpath() reproduces posixpath.relpath() exactly and
 * the manifest matches when the C runs with cwd == out_root, precisely as the
 * python matches when run with cwd == the repo root.
 */
#ifndef CX_ART_COMMON_H
#define CX_ART_COMMON_H

#include "cx_common_b.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- game root */
/* `game_dir` when non-NULL/non-empty, else $B3_GAME_DIR, else the shipped
 * default the python tools hard-code. */
const char *cxe_game_dir(const char *game_dir);

/* ------------------------------------------------------------ path utils */
/* snprintf a "<a>/<b>" join; returns 0 on success, -1 if it would truncate. */
int cxe_join(char *out, size_t cap, const char *a, const char *b);
/* mkdir -p "<root>/<rel>" and hand back the joined path. */
int cxe_out_dir(char *out, size_t cap, const char *root, const char *rel);

/* posixpath.relpath(path) -- relative to the process CWD, purely lexical
 * (normpath, no symlink resolution), '.' when they are the same path. */
int cxe_relpath(const char *path, char *out, size_t cap);

/* CPython float.__round__(None): C round() with the halfway case pulled to
 * even, which is what "%d" % round(x) prints. */
double cxe_py_round(double x);

/* ----------------------------------------------------- sorted directories */
typedef struct {
    char **v;
    int    n;
} cxe_names;

/* sorted(os.listdir(dir)) -- codepoint order (strcmp; ASCII names), '.' and
 * '..' excluded exactly as os.listdir() excludes them.  0 on success. */
int  cxe_listdir_sorted(const char *dir, cxe_names *out);
void cxe_names_free(cxe_names *ns);
int  cxe_is_dir(const char *path);
int  cxe_is_file(const char *path);

/* --------------------------------------------------------------- records */
/* extract_txd.py / extract_carfx_art.py read_name(): a NUL-terminated ASCII
 * string of at most 64 characters, non-empty, EVERY character in 32..126.
 * Returns 0 on success (a "name"), -1 for python's None. */
int cxe_read_name(const cxb_blob *b, int64_t off, char *out, size_t cap);

/* extract_carfx_art.py decode_record(): one texture record -> name + RGBA.
 * Deliberately as loose as the python -- no dimension or power-of-two test,
 * palette 0 only -- because the callers filter by NAME afterwards. */
typedef struct {
    char           name[80];
    int            w, h;
    unsigned       fmt;
    int            depth;
    unsigned char *rgba;      /* w*h*4, caller frees via cxe_rec_free */
} cxe_rec;

int  cxe_decode_record(const cxb_blob *b, int64_t rec, cxe_rec *out);
void cxe_rec_free(cxe_rec *r);

/* --------------------------------------------------------- the txd banks */
/* Open a bank and validate its header (magic + entrySize 16). */
int cxe_txd_open(const char *path, cxb_blob *b, uint32_t *count);
/* The TOC test extract_carfx_art / boostfx / particlefx use: the OFFSET RANGE
 * ONLY.  extract_txd.py additionally requires id == i+1 and both filler words
 * zero -- that stricter test is cxb_txd_entry_offset(). */
int cxe_txd_offset_loose(const cxb_blob *b, uint32_t i, int64_t *off);

/* The shared body of extract_carfx_art.extract_textures(),
 * extract_boostfx_art.extract_textures() and
 * extract_particlefx_art.extract_textures(): walk a bank, decode every
 * record, write <outdir>/<name>.png for every record whose name is WANTED,
 * then fail if a REQUIRED name never showed up. */
typedef enum {
    CXE_REPORT_CARFX = 0,     /* "  %-14s %dx%d  alpha range (a, b) -> ..." */
    CXE_REPORT_BOOSTFX,       /* "  %-16s %dx%d  alpha (..)  rgb (..) -> " */
    CXE_REPORT_PARTICLEFX     /* "  %-18s ...", optional names are noted   */
} cxe_report_style;

int cxe_pull_named(const char *txd_path, const char *outdir,
                   const char *const *wanted, int nwanted,
                   const char *const *required, int nrequired,
                   cxe_report_style style);

/* ------------------------------------------------- extract_font's Image32 */
typedef struct {
    uint32_t va, off, fsz, msz;
} cxe_elf_seg;

typedef struct {
    cxb_blob    f;
    cxe_elf_seg seg[64];
    int         nseg;
} cxe_elf;

int  cxe_elf_open(const char *path, cxe_elf *e);
void cxe_elf_close(cxe_elf *e);
/* Image32.read(): the PT_LOAD segment containing `va`; bytes past p_filesz
 * come back as zeroes (python slices short and pads).  -1 = VA unmapped. */
int  cxe_elf_read(const cxe_elf *e, uint32_t va, size_t n, unsigned char *out);
uint32_t cxe_elf_u32(const cxe_elf *e, uint32_t va, int *ok);

#ifdef __cplusplus
}
#endif

#endif /* CX_ART_COMMON_H */
