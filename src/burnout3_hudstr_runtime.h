// RUNTIME HUD LABEL STRINGS.
//
// The last of the retail DISPLAY TEXT to leave the C source.  Phase 1 of the
// purge moved the TRACK SELECT screen's strings out of src/burnout3_full.c
// (src/burnout3_trackselect_runtime.h); the same argument applies to the
// in-race HUD, whose plate and ticker labels were typed in as English
// literals with their Data/Globalus.bin index in a trailing comment:
//
//     draw_text(f, g_font_global, "POS",      /* Globalus 2002 [C] */ ...)
//
// The index was already recovered and already cited.  Only the TEXT was
// compiled in, and that text is the publisher's content, not recovered
// program behaviour, so a user-supplies-assets distribution cannot ship it.
// This header resolves each label out of the USER'S OWN Globalus.bin at the
// index the port already knew, and the literal is gone -- the same choice
// src/burnout3_trackselect_runtime.h made for the screen chrome, down to the
// "#<index>" placeholder that stands in when the file is absent.
//
// ================================================================ THE SOURCE
//   build/Globalus.bin    the user's own Data/Globalus.bin -- u32 count at
//                         +0x08, u32 offset table at +0x10, UTF-16LE strings
//                         terminated by a 0x0000 unit                    [C]
//                         ($B3_GLOBALUS overrides the path, as elsewhere)
//
// This is deliberately a SEPARATE, self-contained reader rather than a call
// into src/burnout3_trackselect_runtime.h: that header is included only by
// src/burnout3_full.c, it pulls in the ELF mapper and the pace loader it needs
// for the track table, and the HUD is a different translation unit that needs
// none of that.  Every symbol here carries the `b3_hs_` prefix so the two can
// coexist in one TU without colliding.
//
// ============================================================== THE INDICES
// Every index below was already in the source, in a comment, with its
// provenance.  They are ADDRESSES IN RECOVERED CODE (the imm32 the element's
// constructor loads), not game data, so they stay:
//
//   2002  "POS"        imm32@0x00053FF6  (B3HUD_STR_POS / 4)            [C]
//   2003  "LAP"        imm32@0x00051783  (B3HUD_STR_LAP / 4)            [C]
//   1987  "mph"        imm32@0x00059922  (B3HUD_STR_MPH / 4)            [C]
//   2191  "IMPACT TIME" imm32@0x00051372 (B3HUD_IMPACT_STR / 4)         [C]
//   2107..2113  the seven ticker row labels, imm32@0x0004C15C..0x0004C2D9,
//               loaded into row+0x04 by the constructor FUN_0004BFC0     [C]
//   1993..1998  "1st".."6th", the six-entry run FUN_0018F060 indexes with
//               word[car+0x10D0]-1 @0x0018EDBB (B3HUD_TAG_STR_1ST / 4)   [C]

#ifndef BURNOUT3_HUDSTR_RUNTIME_H
#define BURNOUT3_HUDSTR_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Interning caps.  The HUD asks for a couple of dozen labels and asks for the
 * same ones every frame, so the cache is a small fixed arena and the strings
 * it hands out live for the process -- callers store them as `const char *`
 * and the elements re-resolve them per draw. */
#define B3_HS_CACHE   64
#define B3_HS_LEN     64

static struct {
    int            tried;        /* a load has been attempted */
    unsigned char* raw;
    size_t         len;
    unsigned       count;
    int            n;
    unsigned       idx[B3_HS_CACHE];
    char           str[B3_HS_CACHE][B3_HS_LEN];
} g_hs;

static inline unsigned b3_hs_u32(const unsigned char* d, size_t o) {
    return (unsigned)d[o] | ((unsigned)d[o + 1] << 8)
         | ((unsigned)d[o + 2] << 16) | ((unsigned)d[o + 3] << 24);
}

static inline unsigned short b3_hs_u16(const unsigned char* d, size_t o) {
    return (unsigned short)(d[o] | (d[o + 1] << 8));
}

/* One attempt, at first use.  A missing Globalus.bin is reported ONCE and
 * every label then renders as "#<index>": loud and visibly wrong, never a
 * silent English substitution.  This matches what
 * src/burnout3_trackselect_runtime.h does with the screen chrome -- the HUD
 * is display text, not a simulation input, so it does not stop the race the
 * way a missing route.bin or car_physics.bin does. */
static inline int b3_hs_open(void) {
    const char* path = getenv("B3_GLOBALUS");
    FILE* f;
    long sz;

    if (g_hs.tried) return g_hs.raw != NULL;
    g_hs.tried = 1;
    if (!path || !*path) path = "build/Globalus.bin";

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Burnout3] hud labels: cannot read %s -- every HUD "
                "label will render as its Globalus index.  Copy your own "
                "Data/Globalus.bin there.\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x10
        || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    g_hs.len = (size_t)sz;
    g_hs.raw = (unsigned char*)malloc(g_hs.len);
    if (!g_hs.raw || fread(g_hs.raw, 1, g_hs.len, f) != g_hs.len) {
        free(g_hs.raw); g_hs.raw = NULL; fclose(f); return 0;
    }
    fclose(f);
    g_hs.count = b3_hs_u32(g_hs.raw, 8);
    if (!g_hs.count || (size_t)0x10 + (size_t)g_hs.count * 4 > g_hs.len) {
        fprintf(stderr, "[Burnout3] hud labels: %s is not a usable string "
                "table\n", path);
        free(g_hs.raw); g_hs.raw = NULL; return 0;
    }
    /* stderr: see the note in burnout3_car_physics_runtime.h */
    fprintf(stderr, "[Burnout3] hud labels: %u strings from %s\n",
            g_hs.count, path);
    return 1;
}

/* Entry `idx` as ASCII, interned.  The HUD's labels are pure ASCII; anything
 * outside it renders '?' rather than mangling the byte stream -- the same
 * rule b3_globalus_get() applies to the track names. */
static inline const char* b3_hudstr(unsigned idx) {
    size_t off, k = 0;
    char* out;
    int i;

    for (i = 0; i < g_hs.n; i++)
        if (g_hs.idx[i] == idx) return g_hs.str[i];
    if (g_hs.n >= B3_HS_CACHE) {
        /* the cap is a format limit, not a label count; say so rather than
         * silently handing back the wrong string */
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[Burnout3] hud labels: more than %d distinct "
                    "labels requested; raise B3_HS_CACHE\n", B3_HS_CACHE);
        }
        return "";
    }
    out = g_hs.str[g_hs.n];
    if (!b3_hs_open() || idx >= g_hs.count) {
        snprintf(out, B3_HS_LEN, "#%u", idx);
    } else {
        off = b3_hs_u32(g_hs.raw, 0x10 + (size_t)idx * 4);
        if (off + 2 > g_hs.len) {
            snprintf(out, B3_HS_LEN, "#%u", idx);
        } else {
            while (off + 1 < g_hs.len && k + 1 < (size_t)B3_HS_LEN) {
                unsigned short u = b3_hs_u16(g_hs.raw, off);
                if (u == 0) break;
                out[k++] = (u < 0x80) ? (char)u : '?';
                off += 2;
            }
            out[k] = '\0';
            if (k == 0) snprintf(out, B3_HS_LEN, "#%u", idx);
        }
    }
    g_hs.idx[g_hs.n] = idx;
    return g_hs.str[g_hs.n++];
}

#endif /* BURNOUT3_HUDSTR_RUNTIME_H */
