// RUNTIME TRACK SELECT table.
//
// Replaces the compiled-in src/burnout3_trackselect.h, which tools/
// gen_trackselect.py generated with the 36 shipped tracks' RETAIL DISPLAY
// STRINGS baked into the C source -- the 18 venue names, the 36 directional
// track names and the three region labels, straight out of Globalus.bin.
// Those strings are the publisher's content, not recovered program behaviour,
// and a user-supplies-assets distribution cannot ship them.  The C STRUCTURE
// and the indices stay exactly as they were; only the DATA moves to boot time.
//
// ================================================================ THE SOURCE
// Everything below is read, at startup, out of files the USER supplies:
//
//   build/burnout3.elf    the user's own default.xbe, mapped by
//                         tools/xbe2elf.py -- the same image every extractor
//                         and validator already addresses by VA.
//     DAT_0039EBC0  u64[n]  packed track ids, the array FUN_00158640
//                           searches; ITS ORDER IS TLIST ORDER, and
//                           gen_trackselect.py asserts it equals
//                           Tracks/tlist.bin's own id table row for row  [C]
//     DAT_0039ECE0  u32[n]  Globalus index, UPPERCASE directional name   [C]
//     DAT_0039EE00  u32[n]  Globalus index, venue name (FUN_00158680)    [C]
//   build/Globalus.bin    the user's own Data/Globalus.bin: u32 count at
//                         +0x08, u32 offset table at +0x10, UTF-16LE strings
//                         terminated by a 0x0000 unit                    [C]
//   build/tracks/<id>/pace.bin   the event's LAP COUNT, .bgd param+0x3B8,
//                         via b3_ai_pace_laps() (RE_BGD 3)               [C]
//
// and three things are DERIVED from the id itself, by the rules the game's own
// code applies -- no table needed:
//     region   id[0]: 'U' -> 0, 'E' -> 1, 'A' -> 2   FUN_00157630        [C]
//     kind     id[3]: 'C' circuit, 'M' mixed, 'P' point-to-point         [C]
//     dir      "US_C3_V1" -> "US/C3_V1"              FUN_001574F0        [C]
//
// ============================================================= WHAT IS LEFT
// The five ART NAME STEMS per region.  Those are FILE NAMES under
// build/frontend/, produced from the user's own Frontend.txd by
// tools/extract_txd.py; the port already spells a dozen of its siblings
// inline ("B3Logo.png", "A_Button.png", "World_Map.png").  The XBE .rdata
// string pool holds every one of them but NOTHING references them by address
// -- a byte-aligned 4-byte immediate scan over every PT_LOAD byte finds zero
// hits and Ghidra agrees -- so the region -> stem binding is the port's own
// [S] reading of the assets, not game data.  It stays.
//
// ============================================================= WHAT IS GONE
// B3_LOCATIONS / B3_LOCATION_BASE / B3_SPECIAL_EVENTS -- the 28 region-map
// locations and the ten special events.  Every column of those was Globalus
// text too, and NOTHING in the port ever read them: they were documentation
// carried as C data.  The documentation itself is unchanged and lives where
// it belongs, in tools/gen_trackselect.py's docstring (the VAs, the decode
// and the [C]/[S]/[?] marks) and docs/RE_FRONTEND.md section 7.

#ifndef BURNOUT3_TRACKSELECT_RUNTIME_H
#define BURNOUT3_TRACKSELECT_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format cap, not a track count: retail ships 36 and tlist.bin's own bound is
 * 128.  The loader stops at the first row that is not a well-formed id. */
#define B3_TRACK_MAX       64

#define B3_RACE_GRID       6    /* .bgd SPATIAL record: 6 x 0x50 slots  [C] */

/* Region indices exactly as FUN_00157630 returns them from the track id's
 * first character: 'U' -> 0, 'E' -> 1, 'A' -> 2, anything else -> 3.   [C] */
#define B3_REGION_USA      0
#define B3_REGION_EUROPE   1
#define B3_REGION_FAREAST  2

/* The XBE tables, by VA in build/burnout3.elf.  Same constants
 * tools/gen_trackselect.py uses; they are addresses in recovered code, not
 * game data. */
#define B3_VA_TRACK_IDS    0x0039EBC0u   /* u64[n], FUN_00158640         [C] */
#define B3_VA_TRACK_NAME   0x0039ECE0u   /* u32[n] Globalus, UPPERCASE   [C] */
#define B3_VA_TRACK_VENUE  0x0039EE00u   /* u32[n] Globalus, FUN_00158680[C] */

/* Globalus indices for the region label.  227/378's SELECT REGION headers use
 * 229/230/231; the multiplayer option row uses the Title Case 716/717/718 --
 * identical text either way.                                            [C] */
#define B3_GSTR_REGION_USA      229
#define B3_GSTR_REGION_EUROPE   230
#define B3_GSTR_REGION_FAREAST  231

/* THE SCREEN'S OWN CHROME, which was typed into src/burnout3_full.c as English
 * literals and is retail text too.  Every index below is fixed by an EXACT
 * match against the user's own Globalus.bin plus the block structure around it
 * -- 227..257 is one contiguous run of front-end strings, with the three
 * SELECT headers, the per-region welcome lines and the game-mode name list all
 * inside it.  Three of the port's literals turn out to have been PARAPHRASES,
 * and resolving them is the more faithful screen as well as the legal one:
 *     level 0 marquee   was "RACE IN LOCATIONS ACROSS THE WORLD"
 *                       is  Globalus 228
 *     level 1 marquee   was "WELCOME TO THE <region>"
 *                       is  Globalus 234/235/236 (which continue "...
 *                           PLEASE SELECT A LOCATION")
 *     level 2 marquee   was the same sentence with the full stop dropped
 *                       is  Globalus 2559                                [C] */
#define B3_GSTR_HDR_REGION      227    /* "SELECT REGION"                 [C] */
#define B3_GSTR_HDR_LOCATION    232    /* "SELECT LOCATION"               [C] */
#define B3_GSTR_HDR_TRACK       383    /* "SELECT TRACK"                  [C] */
#define B3_GSTR_MARQUEE_REGION  228    /* level 0 status line             [C] */
#define B3_GSTR_MARQUEE_USA     234    /* level 1, region 0               [C] */
#define B3_GSTR_MARQUEE_EUROPE  235    /* level 1, region 1               [C] */
#define B3_GSTR_MARQUEE_FAREAST 236    /* level 1, region 2               [C] */
#define B3_GSTR_MARQUEE_TRACK   2559   /* level 2 status line             [C] */
/* the game-mode name list, 247..254; the four the selector lists as LOCKED */
#define B3_GSTR_MODE_ROADRAGE   250                                   /* [C] */
#define B3_GSTR_MODE_ELIMINATOR 252                                   /* [C] */
#define B3_GSTR_MODE_BURNINGLAP 249                                   /* [C] */
#define B3_GSTR_MODE_CRASH      254                                   /* [C] */

typedef struct {
    unsigned char  index;          /* 0..2, == FUN_00157630's return     [C] */
    unsigned short name_str;       /* the Globalus index it came from    [C] */
    char           name[32];       /* "USA" / "EUROPE" / "FAR EAST"      [C] */
    const char    *art_silhouette; /* 256x256 flat map icon              [S] */
    const char    *art_continent;  /* 512x512 satellite continent        [S] */
    const char    *art_lsat;       /* 256x256 regional satellite         [S] */
    const char    *art_mip_fmt;    /* printf "%d" 1..4: 64x32..512x256   [S] */
    const char    *art_tile_fmt;   /* printf "%d,%d": map n=1..3, half 1|2[S] */
} B3TrackRegion;

typedef struct {
    unsigned char  order;        /* tlist index == retail's menu order;
                                  * every XBE table is indexed by it     [C] */
    char           id[16];       /* "US_C3_V1", base-40 (FUN_001AECC0)   [C] */
    char           dir[16];      /* "US/C3_V1"  (FUN_001574F0)           [C] */
    unsigned char  region;       /* B3_REGION_*  (FUN_00157630)          [C] */
    char           kind;         /* 'C' circuit, 'M' mixed, 'P' point2pt [C] */
    unsigned char  variant;      /* 1 = V1, 2 = V2                       [C] */
    unsigned short venue_str;    /* Globalus index, DAT_0039EE00         [C] */
    char           venue[64];    /* the venue label      (Globalus text) [C] */
    unsigned short name_str;     /* Globalus index, DAT_0039ECE0         [C] */
    char           name[64];     /* the directional name (Globalus text) [C] */
    unsigned char  laps;         /* OFFSGRCF .bgd event P+0x3B8          [C] */
} B3TrackSelect;

/* The screen's chrome, in the order b3_trackselect_load() resolves it. */
enum {
    B3_UI_HDR_REGION = 0, B3_UI_HDR_LOCATION, B3_UI_HDR_TRACK,
    B3_UI_MARQUEE_REGION, B3_UI_MARQUEE_LOC_USA, B3_UI_MARQUEE_LOC_EUROPE,
    B3_UI_MARQUEE_LOC_FAREAST, B3_UI_MARQUEE_TRACK,
    B3_UI_MODE_ROADRAGE, B3_UI_MODE_ELIMINATOR, B3_UI_MODE_BURNINGLAP,
    B3_UI_MODE_CRASH,
    B3_UI_COUNT
};

static struct {
    int           loaded;
    int           count;
    B3TrackSelect track[B3_TRACK_MAX];
    B3TrackRegion region[3];
    char          ui[B3_UI_COUNT][80];
} g_trackselect;

/* The old spellings, now resolving to runtime storage. */
#define B3_TRACKS          (g_trackselect.track)
#define B3_TRACK_COUNT     (g_trackselect.count)
#define B3_TRACK_REGIONS   (g_trackselect.region)
#define B3_UI(which)       (g_trackselect.ui[which])

/* Convenience: the port unlocks everything.  Retail's profile-gated unlock is
 * FUN_0001BCC0; the deviation is documented in docs/RE_FRONTEND.md and this is
 * exactly the hook to restore it at. */
#define b3_track_unlocked(i)  (1)

/* ------------------------------------------------------------- the ELF map */
typedef struct { unsigned int va, off, fsz; } B3ElfSeg;

typedef struct {
    unsigned char *raw;
    size_t         len;
    B3ElfSeg       seg[32];
    int            nseg;
} B3ElfImage;

static unsigned int b3_rd_u32(const unsigned char* d, size_t o) {
    return (unsigned int)d[o] | ((unsigned int)d[o + 1] << 8)
         | ((unsigned int)d[o + 2] << 16) | ((unsigned int)d[o + 3] << 24);
}
static unsigned short b3_rd_u16(const unsigned char* d, size_t o) {
    return (unsigned short)(d[o] | (d[o + 1] << 8));
}

/* tools/extract_tlist.py's Image class, in C: the ELF32 program headers, the
 * PT_LOAD rows only. */
static int b3_elf_open(B3ElfImage* im, const char* path) {
    FILE* f;
    long sz;
    unsigned int phoff, i;
    unsigned short entsz, num;

    memset(im, 0, sizeof *im);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x40
        || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    im->len = (size_t)sz;
    im->raw = (unsigned char*)malloc(im->len);
    if (!im->raw || fread(im->raw, 1, im->len, f) != im->len) {
        free(im->raw); im->raw = NULL; fclose(f); return 0;
    }
    fclose(f);
    if (memcmp(im->raw, "\177ELF", 4) != 0) { free(im->raw); im->raw = NULL; return 0; }
    phoff = b3_rd_u32(im->raw, 0x1C);
    entsz = b3_rd_u16(im->raw, 0x2A);
    num   = b3_rd_u16(im->raw, 0x2C);
    if (entsz < 32) { free(im->raw); im->raw = NULL; return 0; }
    for (i = 0; i < num && im->nseg < 32; i++) {
        size_t p = (size_t)phoff + (size_t)i * entsz;
        if (p + 32 > im->len) break;
        if (b3_rd_u32(im->raw, p) != 1) continue;         /* PT_LOAD only */
        im->seg[im->nseg].off = b3_rd_u32(im->raw, p + 4);
        im->seg[im->nseg].va  = b3_rd_u32(im->raw, p + 8);
        im->seg[im->nseg].fsz = b3_rd_u32(im->raw, p + 16);
        if ((size_t)im->seg[im->nseg].off + im->seg[im->nseg].fsz <= im->len)
            im->nseg++;
    }
    return im->nseg > 0;
}

static void b3_elf_close(B3ElfImage* im) {
    free(im->raw);
    memset(im, 0, sizeof *im);
}

/* NULL when the VA is unmapped or the span runs off the segment. */
static const unsigned char* b3_elf_read(const B3ElfImage* im,
                                        unsigned int va, size_t n) {
    int i;
    for (i = 0; i < im->nseg; i++) {
        const B3ElfSeg* s = &im->seg[i];
        if (va >= s->va && va - s->va < s->fsz && va - s->va + n <= s->fsz)
            return im->raw + s->off + (va - s->va);
    }
    return NULL;
}

/* ------------------------------------------------------------- Globalus.bin */
typedef struct { unsigned char* raw; size_t len; unsigned int count; }
    B3Globalus;

static int b3_globalus_open(B3Globalus* g, const char* path) {
    FILE* f;
    long sz;
    memset(g, 0, sizeof *g);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0x10
        || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    g->len = (size_t)sz;
    g->raw = (unsigned char*)malloc(g->len);
    if (!g->raw || fread(g->raw, 1, g->len, f) != g->len) {
        free(g->raw); g->raw = NULL; fclose(f); return 0;
    }
    fclose(f);
    g->count = b3_rd_u32(g->raw, 8);
    if (!g->count || (size_t)0x10 + (size_t)g->count * 4 > g->len) {
        free(g->raw); g->raw = NULL; return 0;
    }
    return 1;
}

static void b3_globalus_close(B3Globalus* g) {
    free(g->raw);
    memset(g, 0, sizeof *g);
}

/* Entry `idx` as ASCII.  The strings are UTF-16LE terminated by a 0x0000
 * unit; the track/venue names are pure ASCII, and anything outside it is
 * rendered '?' rather than mangling the byte stream. */
static int b3_globalus_get(const B3Globalus* g, unsigned int idx,
                           char* out, size_t cap) {
    size_t off, k = 0;
    if (!g->raw || idx >= g->count || cap == 0) return 0;
    off = b3_rd_u32(g->raw, 0x10 + (size_t)idx * 4);
    if (off + 2 > g->len) return 0;
    while (off + 1 < g->len && k + 1 < cap) {
        unsigned short u = b3_rd_u16(g->raw, off);
        if (u == 0) break;
        out[k++] = (u < 0x80) ? (char)u : '?';
        off += 2;
    }
    out[k] = '\0';
    return k > 0;
}

/* ---------------------------------------------------------- the id decoder */
/* FUN_001AECC0: base-40, LSB char first, then reversed. [C] */
static void b3_b40(unsigned long long v, char out[13]) {
    static const char CS[] = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
    char tmp[13];
    int i, a, b;
    for (i = 0; i < 12; i++) { tmp[i] = CS[v % 40]; v /= 40; }
    for (i = 0; i < 12; i++) out[i] = tmp[11 - i];
    out[12] = '\0';
    for (a = 0; out[a] == ' '; a++) { }
    for (b = 11; b >= a && out[b] == ' '; b--) { }
    memmove(out, out + a, (size_t)(b - a + 1));
    out[b - a + 1] = '\0';
}

/* The id table has no count of its own -- retail takes it from the tlist
 * loaded at 0x004D3000.  The rows past the last track are unrelated .data, so
 * the terminator is the SHAPE the ids are asserted to have everywhere else in
 * this port: 8 characters, '_' at 2 and 5 (tools/extract_tlist.read_tlist's
 * own assertion).  Row 36 of the shipped image fails it. */
static int b3_track_id_ok(const char* s) {
    return strlen(s) == 8 && s[2] == '_' && s[5] == '_';
}

/* Boot-time load.  Returns the number of tracks; 0 means the user's files are
 * not where the port expects them, which the caller reports LOUDLY -- there is
 * no compiled-in table to fall back to any more. */
static int b3_trackselect_load(void) {
    const char* elf_path = getenv("B3_XBE_ELF");
    const char* gl_path  = getenv("B3_GLOBALUS");
    B3ElfImage im;
    B3Globalus gl;
    unsigned int gcount = 0;
    int i, n = 0;
    static const struct {
        unsigned short str;
        const char *sil, *cont, *lsat, *mip, *tile;
    } ART[3] = {   /* [S] -- asset name stems, see the header note */
        { B3_GSTR_REGION_USA,     "USA",     "USA1",    "US_LSAT",
          "SATMAPU_L%d", "SATMAPU%d_%d" },
        { B3_GSTR_REGION_EUROPE,  "Europe",  "EUROPE1", "EU_LSAT",
          "SATMAPE_L%d", "SATMAPE%d_%d" },
        { B3_GSTR_REGION_FAREAST, "FarEast", "ASIA1",   "AU_LSAT",
          "SATMAPA_L%d", "SATMAPA%d_%d" },
    };

    if (g_trackselect.loaded) return g_trackselect.count;
    if (!elf_path || !*elf_path) elf_path = "build/burnout3.elf";
    if (!gl_path  || !*gl_path)  gl_path  = "build/Globalus.bin";
    memset(&g_trackselect, 0, sizeof g_trackselect);

    if (!b3_elf_open(&im, elf_path)) {
        fprintf(stderr, "[Burnout3] track select: cannot map %s -- run "
                "tools/xbe2elf.py on your own default.xbe first\n", elf_path);
        return 0;
    }
    if (!b3_globalus_open(&gl, gl_path)) {
        fprintf(stderr, "[Burnout3] track select: cannot read %s -- copy your "
                "own Data/Globalus.bin there\n", gl_path);
        b3_elf_close(&im);
        return 0;
    }

    {   /* the screen's own chrome, same file, same decode */
        static const unsigned short UI_STR[B3_UI_COUNT] = {
            B3_GSTR_HDR_REGION, B3_GSTR_HDR_LOCATION, B3_GSTR_HDR_TRACK,
            B3_GSTR_MARQUEE_REGION, B3_GSTR_MARQUEE_USA,
            B3_GSTR_MARQUEE_EUROPE, B3_GSTR_MARQUEE_FAREAST,
            B3_GSTR_MARQUEE_TRACK,
            B3_GSTR_MODE_ROADRAGE, B3_GSTR_MODE_ELIMINATOR,
            B3_GSTR_MODE_BURNINGLAP, B3_GSTR_MODE_CRASH,
        };
        int k;
        for (k = 0; k < B3_UI_COUNT; k++)
            if (!b3_globalus_get(&gl, UI_STR[k], g_trackselect.ui[k],
                                 sizeof g_trackselect.ui[k]))
                snprintf(g_trackselect.ui[k], sizeof g_trackselect.ui[k],
                         "#%u", (unsigned)UI_STR[k]);
    }

    for (i = 0; i < 3; i++) {
        g_trackselect.region[i].index    = (unsigned char)i;
        g_trackselect.region[i].name_str = ART[i].str;
        if (!b3_globalus_get(&gl, ART[i].str, g_trackselect.region[i].name,
                             sizeof g_trackselect.region[i].name))
            snprintf(g_trackselect.region[i].name,
                     sizeof g_trackselect.region[i].name, "REGION %d", i);
        g_trackselect.region[i].art_silhouette = ART[i].sil;
        g_trackselect.region[i].art_continent  = ART[i].cont;
        g_trackselect.region[i].art_lsat       = ART[i].lsat;
        g_trackselect.region[i].art_mip_fmt    = ART[i].mip;
        g_trackselect.region[i].art_tile_fmt   = ART[i].tile;
    }

    for (i = 0; i < B3_TRACK_MAX; i++) {
        const unsigned char* p = b3_elf_read(&im,
            B3_VA_TRACK_IDS + (unsigned int)i * 8, 8);
        const unsigned char* q;
        B3TrackSelect* t;
        char id[13];
        unsigned long long v;
        if (!p) break;
        v = (unsigned long long)b3_rd_u32(p, 0)
          | ((unsigned long long)b3_rd_u32(p, 4) << 32);
        b3_b40(v, id);
        if (!b3_track_id_ok(id)) break;

        t = &g_trackselect.track[n];
        t->order   = (unsigned char)i;
        snprintf(t->id, sizeof t->id, "%s", id);
        snprintf(t->dir, sizeof t->dir, "%c%c/%c%c_%c%c",
                 id[0], id[1], id[3], id[4], id[6], id[7]);
        t->region  = (id[0] == 'U') ? 0 : (id[0] == 'E') ? 1
                   : (id[0] == 'A') ? 2 : 3;
        t->kind    = id[3];
        t->variant = (unsigned char)(id[7] >= '0' && id[7] <= '9'
                                     ? id[7] - '0' : 0);

        q = b3_elf_read(&im, B3_VA_TRACK_VENUE + (unsigned int)i * 4, 4);
        t->venue_str = q ? (unsigned short)b3_rd_u32(q, 0) : 0;
        if (!q || !b3_globalus_get(&gl, t->venue_str, t->venue,
                                   sizeof t->venue))
            snprintf(t->venue, sizeof t->venue, "%s", t->id);
        q = b3_elf_read(&im, B3_VA_TRACK_NAME + (unsigned int)i * 4, 4);
        t->name_str = q ? (unsigned short)b3_rd_u32(q, 0) : 0;
        if (!q || !b3_globalus_get(&gl, t->name_str, t->name, sizeof t->name))
            snprintf(t->name, sizeof t->name, "%s", t->id);

        /* the event's own lap count, .bgd param+0x3B8 via pace.bin */
        t->laps = (unsigned char)b3_ai_pace_laps(t->id, NULL);
        n++;
    }

    gcount = gl.count;
    b3_globalus_close(&gl);
    b3_elf_close(&im);

    g_trackselect.count  = n;
    g_trackselect.loaded = 1;
    if (n == 0) {
        fprintf(stderr, "[Burnout3] track select: %s has no track id table at "
                "%#x\n", elf_path, B3_VA_TRACK_IDS);
        return 0;
    }
    printf("[Burnout3] track select data: %d tracks from %s + %s "
           "(%u strings)\n", n, elf_path, gl_path, gcount);
    return n;
}

#endif /* BURNOUT3_TRACKSELECT_RUNTIME_H */
