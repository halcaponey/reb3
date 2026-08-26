// RUNTIME FONT METRICS.
//
// Replaces the compiled-in src/burnout3_font.h, the three glyph tables
// tools/extract_font.py recovered from the retail XBE's .data section and
// baked into the C source.  The consumer name keeps its old spelling --
// `b3_font_globalfont` is still the thing src/burnout3_hud.c takes the address
// of -- so call sites are unchanged; only the STORAGE moves, from a const
// struct chosen at compile time to a table filled on first use from
//
//     build/frontend/font.bin                    ('B3FN', version 1)
//
// which tools/cextract's `font` stage writes alongside the atlas PNGs and the
// header it still generates (tools/cextract/cx_art_font.c, and the format
// block in tools/cextract/cx_extract.h).
//
// The ATLAS PIXELS were never compiled in -- they have always been loaded from
// build/frontend/<name>.png -- so this closes the last half of the font: the
// publisher's typeface no longer appears in the port's source in any form.
//
// ============================================================ PROVENANCE [C]
// Unchanged from the header.  The three fonts are compiled into the retail
// executable, each as a font object followed by its texture record and DXT5
// bitmap: objects 0x3C84D8 / 0x3D9CB8 / 0x3E23B8, texture records 0x3C9C38 /
// 0x3DA338 / 0x3E3B18, bound by name in FUN_0002EF90.  The glyph record
// layout is proven by FUN_001C1060's charmap walk:
//     +0x00 f32 u  +0x04 f32 v  +0x08 f32 w  +0x0C f32 h
//     +0x10 f32 xoffset  +0x14 f32 yoffset  +0x18 f32 advance
//     +0x1C u32 charcode
// The metrics here are PIXEL-space (the u/v extents multiplied by the atlas
// size), exactly as the header stored them, and the atlases are white + alpha
// with colour applied at draw time by the 2D pipeline.
// See docs/RE_FRONTEND.md section 6.
//
// ================================================================= PRECISION
// The header printed the UVs "%.6f" and every pixel metric "%.1f", so the
// compiled constants were ROUNDED decimals -- and the port's [S-ref] HUD
// offsets were measured against those rounded numbers, not against the raw
// f32.  The extractor therefore formats each value with the header's own
// format and parses it back before storing it, which makes this table
// bit-identical to the one it replaces.  cx_art_font.c is one format string
// away from full precision if a later revision wants it.
//
// ===================================================================== LOUD
// A missing font.bin does not look missing: every glyph would measure zero and
// the HUD would draw blank plates with no text.  The loader names the file and
// stops.

#ifndef BURNOUT3_FONT_RUNTIME_H
#define BURNOUT3_FONT_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format constants, not art: 0x20..0x7E is the range FUN_001C1060's charmap
 * covers, and the three fonts are the three the executable carries. */
#define B3_FONT_GLYPHS   95
#define B3_FONT_MAX       8
#define B3_FONT_GLOBAL    0     /* GlobalFont, 256x256, ASCII 32..126 */
#define B3_FONT_HEAD      1     /* HeadFont,   128x256, space/'-'/A-Z */
#define B3_FONT_SMALL     2     /* SmallFont,  128x128, ASCII 32..126 */

typedef struct B3Glyph {
    float u0, v0, u1, v1;   /* normalized atlas rect */
    float w, h;             /* glyph size, atlas px  */
    float xoff, yoff;       /* pen offset, atlas px  */
    float advance;          /* pen advance, atlas px */
    unsigned char present;  /* 0 = font has no glyph */
} B3Glyph;

typedef struct B3Font {
    const char *name;       /* atlas basename in build/frontend */
    int tex_w, tex_h;
    float line_h;           /* max glyph yoff+h, atlas px */
    B3Glyph glyph[B3_FONT_GLYPHS];   /* chars 0x20..0x7E */
} B3Font;

static struct {
    int    loaded;
    int    count;
    B3Font font[B3_FONT_MAX];
    char   name[B3_FONT_MAX][32];
} g_b3_fonts;

static inline unsigned b3_fn_u32(const unsigned char* d) {
    return (unsigned)d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16)
         | ((unsigned)d[3] << 24);
}

static inline float b3_fn_f32(const unsigned char* d) {
    unsigned bits = b3_fn_u32(d);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static inline void b3_font_fatal(const char* path, const char* why) {
    fprintf(stderr,
        "[Burnout3] FATAL: %s -- %s.\n"
        "  These are the retail XBE's own glyph metrics and there is\n"
        "  no compiled-in copy to fall back to; without them every string\n"
        "  the HUD draws would measure zero and render blank.\n"
        "  Extract it from your own dump:  tools/cextract/build.sh && "
        "cxtract --all-global --out .\n"
        "  (or just this stage:  cxtract --only font --out .)\n",
        path, why);
    exit(2);
}

/* build/frontend/font.bin, 'B3FN' version 1 (tools/cextract/cx_art_font.c):
 *   +0x00 char[4] 'B3FN'   +0x04 u32 version = 1
 *   +0x08 u32 font_count   +0x0C u32 glyphs_per_font = 95
 *   +0x10 font_count x {char[32] name, u32 tex_w, u32 tex_h, f32 line_h,
 *                       95 x {f32 u0,v0,u1,v1,w,h,xoff,yoff,advance,
 *                             u8 present, u8 pad[3]}}
 * Loads once; FATAL rather than drawing with a zeroed table. */
static inline int b3_font_load(void) {
    const char* path = getenv("B3_FONT_BIN");
    unsigned char hdr[16];
    unsigned n, ng, i;
    int c;
    FILE* f;

    if (g_b3_fonts.loaded) return g_b3_fonts.count;
    if (!path || !*path) path = "build/frontend/font.bin";
    g_b3_fonts.loaded = 1;

    f = fopen(path, "rb");
    if (!f) b3_font_fatal(path, "no such file");
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "B3FN", 4) != 0
        || b3_fn_u32(hdr + 4) != 1u) {
        fclose(f);
        b3_font_fatal(path, "not a B3FN version 1 asset");
    }
    n  = b3_fn_u32(hdr + 8);
    ng = b3_fn_u32(hdr + 12);
    if (n == 0 || n > B3_FONT_MAX || ng != B3_FONT_GLYPHS) {
        fclose(f);
        b3_font_fatal(path, "font count or glyph range outside the format cap");
    }
    for (i = 0; i < n; i++) {
        unsigned char nm[32], meta[12];
        if (fread(nm, 1, 32, f) != 32 || fread(meta, 1, 12, f) != 12) {
            fclose(f);
            b3_font_fatal(path, "truncated in a font header");
        }
        memcpy(g_b3_fonts.name[i], nm, 32);
        g_b3_fonts.name[i][31] = '\0';   /* never trust a file to terminate */
        g_b3_fonts.font[i].name   = g_b3_fonts.name[i];
        g_b3_fonts.font[i].tex_w  = (int)b3_fn_u32(meta);
        g_b3_fonts.font[i].tex_h  = (int)b3_fn_u32(meta + 4);
        g_b3_fonts.font[i].line_h = b3_fn_f32(meta + 8);
        for (c = 0; c < B3_FONT_GLYPHS; c++) {
            unsigned char g[40];
            B3Glyph* q = &g_b3_fonts.font[i].glyph[c];
            if (fread(g, 1, 40, f) != 40) {
                fclose(f);
                b3_font_fatal(path, "truncated in a glyph table");
            }
            q->u0      = b3_fn_f32(g);
            q->v0      = b3_fn_f32(g + 4);
            q->u1      = b3_fn_f32(g + 8);
            q->v1      = b3_fn_f32(g + 12);
            q->w       = b3_fn_f32(g + 16);
            q->h       = b3_fn_f32(g + 20);
            q->xoff    = b3_fn_f32(g + 24);
            q->yoff    = b3_fn_f32(g + 28);
            q->advance = b3_fn_f32(g + 32);
            q->present = g[36];
        }
    }
    fclose(f);
    g_b3_fonts.count = (int)n;
    /* stderr: see the note in burnout3_car_physics_runtime.h */
    fprintf(stderr,
            "[Burnout3] font metrics: %d fonts from %s (%s %dx%d, line %.3f)\n",
            g_b3_fonts.count, path, g_b3_fonts.font[0].name,
            g_b3_fonts.font[0].tex_w, g_b3_fonts.font[0].tex_h,
            (double)g_b3_fonts.font[0].line_h);
    return g_b3_fonts.count;
}

/* The font by slot.  Out of range is the same class of mistake as a missing
 * file -- the caller would draw with a zeroed table -- so it is equally loud. */
static inline const B3Font* b3_font_get(int which) {
    b3_font_load();
    if (which < 0 || which >= g_b3_fonts.count)
        b3_font_fatal("build/frontend/font.bin", "asset carries no such font");
    return &g_b3_fonts.font[which];
}

/* The old spellings, now resolving to runtime storage.  Both `&b3_font_x` and
 * `b3_font_x.tex_w` still compile, which is why no call site changes. */
#define b3_font_globalfont  (*b3_font_get(B3_FONT_GLOBAL))
#define b3_font_headfont    (*b3_font_get(B3_FONT_HEAD))
#define b3_font_smallfont   (*b3_font_get(B3_FONT_SMALL))

#endif /* BURNOUT3_FONT_RUNTIME_H */
