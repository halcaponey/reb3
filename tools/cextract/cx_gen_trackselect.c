/* cx_gen_trackselect.c -- the C port of tools/gen_trackselect.py.
 *
 * Emit burnout3_trackselect.h -- everything the TRACK SELECT screen needs.
 *
 * Nothing in this file is typed in by hand except the provenance comments and
 * the two *asset* maps at the bottom, which are marked [S] because the XBE
 * contains the texture NAMES but no code reference to them (see the header's
 * "ART" section and docs/RE_FRONTEND.md 7.4).  Every row of the track table is
 * read out of the shipped data or out of the correctly-mapped XBE image:
 *
 *   Tracks/tlist.bin            36 packed track ids + flagsA/flagsB
 *                               (loaded verbatim to 0x004D3000: +4 count,
 *                                +8 flagsA, +0x208 flagsB, +0x408 ids)    [C]
 *   DAT_0039EBC0  u64[36]       the XBE's own copy of the packed ids, the
 *                               array FUN_00158640 searches                [C]
 *   DAT_0039ECE0  u32[36]       Globalus idx, UPPERCASE directional name   [C]
 *   DAT_0039ED70  u32[36]       Globalus idx, Title Case directional name  [C]
 *   DAT_0039EE00  u32[36]       Globalus idx, venue name (FUN_00158680)    [C]
 *   FUN_00157630                region from id[0]: 'U'->0 'E'->1 'A'->2    [C]
 *   FUN_001574F0                id -> "tracks/<REG>/<Cn>_<Vn>/"            [C]
 *   Tracks/<..>/Gamedata.bgd    event record P+0x3B8 = lap count; the
 *                               OFFSGRCF / ONSGRCF slots are the offline /
 *                               online SINGLE RACE templates               [C]
 *   <..> event SPATIAL record   6 start-grid slots of 0x50 bytes           [C]
 *   DAT_0039F9B0  u32[28]       region-map LOCATION list (Globalus idx)    [C]
 *   DAT_0039F990  u8[28]        location type -> DAT_0039F978[]            [C]
 *   DAT_0039FA20  u8[28]        second per-location byte, meaning open     [?]
 *   DAT_0039E880  u64[10]       the ten SPECIAL EVENT ids                  [C]
 *   DAT_0039E8D0  u32[10]       their "UNLOCK THE SPECIAL EVENT IN X"      [C]
 *   DAT_0039E8F8  u32[10]       their "<X> POSTCARD" reward names          [C]
 *
 * ============================================================== THE IMAGE ==
 * The python original reads build/burnout3.elf, the correctly-mapped XBE.
 * This port looks for it at <repo>/build/burnout3.elf (override with $B3_ELF)
 * and falls back to the game's own default.xbe, whose section VAs are the
 * identity on the ELF's -- so either image answers the same VA with the same
 * bytes.  Never a flat load: HANDOFF.md section 2.
 */
#define _POSIX_C_SOURCE 200809L

#include "cx_common_g.h"
#include "cx_extract.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the four parallel 36-entry XBE tables, all indexed by tlist index [C] */
#define VA_IDS      0x0039EBC0u   /* u64[36], searched by FUN_00158640      */
#define VA_NAME_UC  0x0039ECE0u   /* u32[36], "SILVER LAKE SOUTHBOUND"      */
#define VA_NAME_TC  0x0039ED70u   /* u32[36], "Silver Lake Southbound"      */
#define VA_VENUE    0x0039EE00u   /* u32[36], "SILVER LAKE"  (FUN_00158680) */
/* --- the region-map location list [C] ------------------------------------ */
#define VA_LOC_NAME 0x0039F9B0u   /* u32[28] Globalus idx                   */
#define VA_LOC_TYPE 0x0039F990u   /* u8[28]  -> index into DAT_0039F978     */
#define VA_LOC_KIND 0x0039F978u   /* u32[6] = {0x2f,0x2f,0x32,0x30,0x30,0x33} */
#define VA_LOC_FLAG 0x0039FA20u   /* u8[28]  meaning open [?]               */
#define LOC_COUNT   28
/* --- the ten special events [C] ------------------------------------------ */
#define VA_SPECIAL_ID     0x0039E880u
#define VA_SPECIAL_UNLOCK 0x0039E8D0u
#define VA_SPECIAL_REWARD 0x0039E8F8u
#define N_SPECIAL   10

#define TS_MAX_TRACKS 128

/* FUN_000DF960's region base offsets [C] */
static const int LOC_BASE[3] = { 0, 9, 21 };

static const char *const REGION_NAME[3] = { "USA", "EUROPE", "FAR EAST" };
/* Globalus indices for the region label, in the three casings retail ships:
 *   227/378 SELECT REGION headers use 229/230/231 and 379/380/381 (identical
 *   text); the multiplayer option ROW uses the Title Case 716/717/718.  [C] */
static const int REGION_STR[3]    = { 229, 230, 231 };
static const int REGION_STR_TC[3] = { 716, 717, 718 };

/* [S] -- per-region map art.  The XBE .rdata string pool
 * 0x003998A0..0x00399DE8 holds every one of these names and Frontend.txd
 * holds a texture for each, but NO code in default.xbe references the strings
 * by address (checked by a full 4-byte immediate scan of every PT_LOAD byte,
 * and by Ghidra xrefs), so the binding below is read off the ASSETS, not off
 * code.  See the header notes. */
static const struct {
    const char *silhouette, *continent, *lsat, *mip, *tile;
} REGION_ART[3] = {
    { "USA",     "USA1",    "US_LSAT", "SATMAPU_L%d", "SATMAPU%d_%d" },
    { "Europe",  "EUROPE1", "EU_LSAT", "SATMAPE_L%d", "SATMAPE%d_%d" },
    { "FarEast", "ASIA1",   "AU_LSAT", "SATMAPA_L%d", "SATMAPA%d_%d" },
};

/* [S] -- venue -> crash-newspaper / signature-takedown art.  Ten venues only:
 * exactly the ten locations DAT_0039F990 marks type 2 (the crash junctions).
 * Matched by name; the file set has no other candidate and no leftovers. */
static const struct {
    const char *venue, *headline, *sigtd;
} VENUE_ART[] = {
    { "WATERFRONT",      "HDWaterfront",     "waterfront-st"     },
    { "DOWNTOWN",        "HDDowntown",       "downtown-st"       },
    { "SILVER LAKE",     "HDSilverLake",     "silverlake-ST"     },
    { "WINTER CITY",     "HDWinterCity",     "wintercity-st"     },
    { "ALPINE",          "HDAlpine",         "alpine-st"         },
    { "RIVIERA",         "HDRiviera",        "riviera-st"        },
    { "VINEYARD",        "HDVineyard",       "vineyard-st"       },
    { "GOLDEN CITY",     "HDGoldenCity",     "goldencity-st"     },
    { "DOCKSIDE",        "HDDockside",       "dockside-st"       },
    { "ISLAND PARADISE", "HDIslandParadise", "islandparadise-st" },
};

/* [S] -- the ten special-event reward POSTCARDS, in DAT_0039E880 order.  The
 * order is [C] (the event ids decode to the venue), the file names are matched
 * by name -- and PCus-p2p2a / PCeu-p2p2 are decisive, the ids are USP2/EUP2. */
static const char *const POSTCARD_ART[N_SPECIAL] = {
    "PCWinterCity", "PCAlpine", "PCVineyard", "PCDockside",
    "PCIslandParadise", "PCGoldenCity", "PCus-p2p2a",
    "PCeu-p2p2", "PCSilverLake", "PCSpecialGP1",
};

static const char CXG_TS_TOP[] =
    "// GENERATED by tools/gen_trackselect.py -- do not edit by hand.\n"
    "//\n"
    "// The retail TRACK SELECT data set: the 36 shipped tracks, the three regions,\n"
    "// the 28 region-map locations, and the ten special events.  Provenance for\n"
    "// every column is in the generator's docstring and in the per-field comments\n"
    "// below; docs/RE_FRONTEND.md section 7 carries the screen spec.\n"
    "//\n"
    "// ============================================================ WHAT IS [C] ==\n"
    "// Everything numeric here is read at run time out of Tracks/tlist.bin, out of\n"
    "// the per-track Gamedata.bgd, or out of the correctly-mapped XBE image at the\n"
    "// VA named in the generator.  The generator asserts that the XBE's own id\n"
    "// table (DAT_0039EBC0, the array FUN_00158640 searches) decodes to the same\n"
    "// 36 ids as tlist.bin, so a misaligned table cannot slip through.\n"
    "//\n"
    "// ============================================================ WHAT IS [S] ==\n"
    "// The *art* columns.  The XBE .rdata string pool 0x003998A0..0x00399DE8 holds\n"
    "// \"HDSilverLake\", \"PCSilverLake\", \"SatMapU1_1\", \"silverlake-ST1\", \"US_LSat\",\n"
    "// \"World_Map\" and the rest, and Frontend.txd holds a texture for each -- but\n"
    "// NOTHING in default.xbe references those strings by address.  A byte-aligned\n"
    "// 4-byte immediate scan over every PT_LOAD byte of the image finds zero hits,\n"
    "// and Ghidra agrees (0 xrefs).  The frontend that consumes them is\n"
    "// data-driven, so the per-track binding below is read off the ASSETS\n"
    "// themselves, not off code.  It is marked [S] and it is the best-supported\n"
    "// approximation, not a recovered fact.\n"
    "//\n"
    "// One correction the assets force, and the orchestrator must not miss:\n"
    "// RETAIL HAS NO PER-TRACK PREVIEW PHOTO.  The three obvious candidates are\n"
    "// all something else --\n"
    "//   HD*.png (256x256)      newspaper front pages for the CRASH results screen\n"
    "//                          (\"ALPINE SMASH!\", \"SILVER LAKE LUNACY!\"), one per\n"
    "//                          crash venue, 10 of them.\n"
    "//   *-st1/-st2.png (256^2) Polaroid photos of the SIGNATURE TAKEDOWNS\n"
    "//                          (\"Avalanche!\", \"Gone Fishin'\"), two per crash\n"
    "//                          venue, 20 of them.\n"
    "//   PC*.png (256x128)      the ten special-event reward POSTCARDS.\n"
    "// What the track select actually shows is the region's satellite map with a\n"
    "// route overlay -- see B3_TRACK_REGIONS and docs/RE_FRONTEND.md 7.3.\n"
    "\n"
    "#ifndef BURNOUT3_TRACKSELECT_H\n"
    "#define BURNOUT3_TRACKSELECT_H\n"
    "\n"
    "#include <stddef.h>          /* NULL -- the M/P rows carry no venue art */\n"
    "\n"
    "#define B3_TRACK_COUNT     36   /* Tracks/tlist.bin +0x004                 [C] */\n"
    "#define B3_LOCATION_COUNT  28   /* FUN_000DF960's bases 0/9/21 + 7         [C] */\n"
    "#define B3_SPECIAL_COUNT   10   /* DAT_0039E880 .. 0x0039E8C8              [C] */\n"
    "#define B3_RACE_GRID       6    /* .bgd SPATIAL record: 6 x 0x50 slots     [C] */\n"
    "\n"
    "/* Region indices exactly as FUN_00157630 returns them from the track id's\n"
    " * first character: 'U' -> 0, 'E' -> 1, 'A' -> 2, anything else -> 3.   [C] */\n"
    "#define B3_REGION_USA      0\n"
    "#define B3_REGION_EUROPE   1\n"
    "#define B3_REGION_FAREAST  2\n"
    "\n"
    "/* Location kinds, the values FUN_000DF960 reads out of DAT_0039F978 and\n"
    " * compares against 0x2F.                                               [C] */\n"
    "#define B3_LOC_RACE        0x2F\n"
    "#define B3_LOC_CRASH       0x32\n"
    "\n"
    "typedef struct {\n"
    "    unsigned char  index;        /* 0..2, == FUN_00157630's return       [C] */\n"
    "    unsigned short name_str;     /* Globalus \"USA\"/\"EUROPE\"/\"FAR EAST\"   [C] */\n"
    "    unsigned short name_str_tc;  /* Globalus \"USA\"/\"Europe\"/\"Far East\"   [C] */\n"
    "    const char    *name;         /* English literal fallback             [C] */\n"
    "    const char    *art_silhouette; /* 256x256 flat map icon              [S] */\n"
    "    const char    *art_continent;  /* 512x512 satellite continent        [S] */\n"
    "    const char    *art_lsat;       /* 256x256 regional satellite         [S] */\n"
    "    const char    *art_mip_fmt;    /* printf \"%d\" 1..4: 64x32..512x256   [S] */\n"
    "    const char    *art_tile_fmt;   /* printf \"%d,%d\": map n=1..3, half 1|2 [S] */\n"
    "} B3TrackRegion;\n"
    "\n"
    "typedef struct {\n"
    "    unsigned char  order;        /* tlist index == retail's menu order;\n"
    "                                  * every XBE table is indexed by it     [C] */\n"
    "    const char    *id;           /* \"US_C3_V1\", base-40 (FUN_001AECC0)   [C] */\n"
    "    const char    *dir;          /* \"US/C3_V1\"  (FUN_001574F0)           [C] */\n"
    "    unsigned char  region;       /* B3_REGION_*                          [C] */\n"
    "    char           kind;         /* 'C' circuit, 'M' mixed, 'P' point2pt [C] */\n"
    "    unsigned char  variant;      /* 1 = V1, 2 = V2                       [C] */\n"
    "    signed char    sibling;      /* the other variant's index, -1 none   [C] */\n"
    "    unsigned char  flag_p;       /* tlist flagsA -- 1 on the eight P     [C] */\n"
    "    unsigned char  flag_offline; /* tlist flagsB -- 0 on the eight AS    [C] */\n"
    "    unsigned short venue_str;    /* Globalus, DAT_0039EE00 (FUN_00158680)[C] */\n"
    "    const char    *venue;        /* \"SILVER LAKE\"                        [C] */\n"
    "    unsigned short name_str;     /* Globalus, DAT_0039ECE0 (UPPERCASE)   [C] */\n"
    "    const char    *name;         /* \"SILVER LAKE SOUTHBOUND\"             [C] */\n"
    "    unsigned short name_str_tc;  /* Globalus, DAT_0039ED70 (Title Case)  [C] */\n"
    "    const char    *name_tc;      /* \"Silver Lake Southbound\"             [C] */\n"
    "    unsigned char  laps;         /* OFFSGRCF, .bgd event P+0x3B8         [C] */\n"
    "    unsigned char  laps_online;  /* ONSGRCF                              [C] */\n"
    "    unsigned char  grid;         /* populated start-grid slots (all 6)   [C] */\n"
    "    unsigned char  in_custom;    /* survives the custom-race filters     [C] */\n"
    "    const char    *art_headline; /* HD*.png, crash venues only, NOT a\n"
    "                                  * track-select preview                 [S] */\n"
    "    const char    *art_sigtd;    /* \"<venue>-st\" stem, + \"1\"/\"2\"         [S] */\n"
    "} B3TrackSelect;\n"
    "\n"
    "typedef struct {\n"
    "    unsigned char  index;        /* 0..27, the region-map slot           [C] */\n"
    "    unsigned char  region;       /* B3_REGION_*                          [C] */\n"
    "    unsigned short name_str;     /* Globalus venue name, DAT_0039F9B0    [C] */\n"
    "    const char    *name;                                              /* [C] */\n"
    "    unsigned char  kind;         /* B3_LOC_RACE / B3_LOC_CRASH           [C] */\n"
    "    unsigned char  is_crash;                                          /* [C] */\n"
    "    unsigned char  flag;         /* DAT_0039FA20, meaning not recovered  [?] */\n"
    "} B3TrackLocation;\n"
    "\n"
    "typedef struct {\n"
    "    const char    *event_id;     /* base-40 decode of DAT_0039E880       [C] */\n"
    "    unsigned long long packed;                                        /* [C] */\n"
    "    unsigned short unlock_str;   /* \"UNLOCK THE SPECIAL EVENT IN X\"      [C] */\n"
    "    unsigned short reward_str;   /* \"<X> POSTCARD\"                       [C] */\n"
    "    const char    *art_postcard; /* PC*.png                              [S] */\n"
    "} B3SpecialEvent;\n";

static const char CXG_TS_BOTTOM[] =
    "/* =========================================================== THE UNLOCK MODEL\n"
    " *\n"
    " * Retail keeps per-track availability in a 36-BYTE ARRAY at DAT_0044D0CC,\n"
    " * indexed by the tlist index -- one byte per track, 0 locked / 1 unlocked.\n"
    " *\n"
    " *   FUN_0001BCC0(id_lo, id_hi)             the \"is this track selectable?\"\n"
    " *                                          predicate every menu calls  [C]\n"
    " *       if (FUN_001575F0(id) < 0) return 0;        // not in tlist.bin\n"
    " *       i = FUN_00158640(id);                      // tlist index\n"
    " *       if (i == -1) return 1;\n"
    " *       return ((unsigned char *)0x0044D0CC)[i];\n"
    " *\n"
    " *   FUN_0001C9D0(profile)                  rebuilds the whole array    [C]\n"
    " *       memset(0x0044D0CC, 0, 36);\n"
    " *       for (e = 0; e <= 0x48; e++)                // 73 World Tour events\n"
    " *           if (DAT_0044D01F[e] && DAT_0039E2A8[e]\n"
    " *               && *(char *)(profile + 0x386 + e) > 0) {\n"
    " *               // the event's track id(s): DAT_0039DF38[e*2] / DAT_0039DF3C\n"
    " *               // a GRAND PRIX (hi == 0, lo < 7) instead expands through\n"
    " *               // PTR_DAT_003ED0F8[lo], DAT_0039E778[lo] rounds\n"
    " *               //   DAT_0039E778 = { 3, 3, 3, 3, 4, 4, 4 }\n"
    " *               // -> every round's track unlocks\n"
    " *               ((unsigned char *)0x0044D0CC)[tlist_index_of(id)] = 1;\n"
    " *           }\n"
    " *       DAT_0044D164 = popcount(0x0044D0CC[0..35]);   // \"tracks unlocked\"\n"
    " *\n"
    " * So a track is unlocked IFF at least one World Tour event that runs on it\n"
    " * has been completed with a medal, and the state lives in the PROFILE, at\n"
    " * profile+0x386+event_index (one byte per event, 73 events).  There is no\n"
    " * per-track bit in the save -- the array is derived every time the profile\n"
    " * is loaded.\n"
    " *\n"
    " * ---- DELIBERATE DEVIATION -------------------------------------------------\n"
    " * The port ships with ALL 36 TRACKS UNLOCKED.  That is a decision, not an\n"
    " * oversight: there is no World Tour progression in the harness, so there is\n"
    " * no profile to derive DAT_0044D0CC from.  Modelled as\n"
    " *\n"
    " *     b3_track_unlocked(i)  ==  1   for every i\n"
    " *\n"
    " * i.e. FUN_0001BCC0 with the array pre-filled.  Nothing else about the\n"
    " * screen changes -- the filters below are retail's and still apply.\n"
    " * Do NOT implement locking; if it is ever wanted, the hook is exactly the\n"
    " * one predicate above.\n"
    " *\n"
    " * ================================================ THE CUSTOM-RACE FILTERS ==\n"
    " * FUN_00079590 (offline two-player setup) and FUN_0008D020 (its online\n"
    " * sibling) build the SELECT TRACK list by walking the loaded tlist at\n"
    " * 0x004D3000 from index 0 to count-1 and keeping a track only if ALL of:\n"
    " *\n"
    " *   [C 0x00079784]  id[3] != 'M'                     -- drops the 8 M tracks\n"
    " *   [C 0x00079797]  FUN_001575A0(id) == 0            -- tlist flagsA, drops\n"
    " *                                                       the 8 P tracks\n"
    " *   [C 0x000797A6]  FUN_0001BCC0(id) != 0            -- unlocked\n"
    " *   [C 0x000797B1]  FUN_00157630(id) == chosen_region\n"
    " *\n"
    " * leaving the 20 CIRCUIT tracks: 6 USA, 8 Europe, 6 Far East.  The name it\n"
    " * shows is DAT_0039ED70[i] -- the Title Case directional name -- with\n"
    " * Globalus 1257 (\"Unknown Track\") as the fallback for a missing index\n"
    " * (`mov eax, 0x4E9` at 0x000797E8).                                    [C]\n"
    " *\n"
    " * B3_TRACKS[i].in_custom carries the first two filters precomputed.\n"
    " *\n"
    " * ============================================================ RACE SETTINGS\n"
    " *   laps          the OFFSGRCF event's P+0x3B8: 3 on every circuit except\n"
    " *                 Alpine (EU_C2_*, which is 2 offline / 3 online), 2 on the\n"
    " *                 M tracks, 1 on the P tracks.                          [C]\n"
    " *   laps_online   the ONSGRCF event's P+0x3B8, for reference.           [C]\n"
    " *   grid          6 on all 36 tracks -- the event SPATIAL record has six\n"
    " *                 0x50-byte start-grid slots and all six are populated on\n"
    " *                 every shipped track.  Corroborated by Globalus 2559,\n"
    " *                 \"PICK A TRACK AND TAKE ON UP TO FIVE OTHER RACERS\", and\n"
    " *                 720, \"This game mode requires a maximum of 6 players\". [C]\n"
    " *\n"
    " * The custom-race screen exposes laps as an editable row whose min/max live\n"
    " * at screen+0xC8 / screen+0xCA (FUN_00079590's row kind 3); the values in\n"
    " * those two fields were not recovered.                                  [?]\n"
    " */\n"
    "\n"
    "/* Convenience: the port unlocks everything (see above). */\n"
    "#define b3_track_unlocked(i)  (1)\n"
    "\n"
    "#endif /* BURNOUT3_TRACKSELECT_H */";

/* ---------------------------------------------------------------- the rows */
typedef struct {
    int      index;
    char     id[16];
    char     dir[32];
    int      region;
    char     kind;
    int      variant;
    int      sibling;
    uint32_t flag_p, flag_offline;
    uint32_t venue_str, uc_str, tc_str;
    char     venue[256], uc[256], tc[256];
    uint32_t laps, laps_online;
    int      grid;
    int      in_custom;
} ts_track;

typedef struct {
    int      index, region;
    uint32_t name_str;
    char     name[256];
    uint32_t type, kind;
    int      is_crash;
    uint32_t flag;
} ts_loc;

/* read_tlist(): [(index, id, flagA, flagB)] in file order. */
static int ts_read_tlist(const char *game, ts_track *out, int cap, int *n_out)
{
    cxg_blob b;
    uint32_t version, count, i;
    char     path[4096];
    int      rc = 1;

    cxg_join(path, sizeof(path), game, "Tracks/tlist.bin");
    if (cxg_blob_load(&b, path) != 0)
        return 1;
    version = cxg_u32(&b, 0);
    count   = cxg_u32(&b, 4);
    if (version != 4) {
        fprintf(stderr, "tlist.bin version %u (expected 4)\n", version);
        goto done;
    }
    if (count == 0 || count > 128 || (int)count > cap) {
        fprintf(stderr, "implausible track count %u\n", count);
        goto done;
    }
    for (i = 0; i < count; i++) {
        uint64_t pid = cxg_u64(&b, 0x408 + (int64_t)i * 8);
        char     name[16];

        cxg_b40(pid, name);
        if (strlen(name) != 8 || name[2] != '_' || name[5] != '_') {
            fprintf(stderr, "track id %u decodes to '%s'\n", i, name);
            goto done;
        }
        memset(&out[i], 0, sizeof(out[i]));
        out[i].index = (int)i;
        snprintf(out[i].id, sizeof(out[i].id), "%s", name);
        out[i].flag_p       = cxg_u32(&b, 0x008 + (int64_t)i * 4);
        out[i].flag_offline = cxg_u32(&b, 0x208 + (int64_t)i * 4);
    }
    if (cxg_oob(&b)) {
        fprintf(stderr, "tlist.bin is truncated\n");
        goto done;
    }
    *n_out = (int)count;
    rc = 0;
done:
    cxg_blob_free(&b);
    return rc;
}

/* read_bgd(path) -> (offline_laps, online_laps, valid_grid_slots).  [C] */
static int ts_read_bgd(const char *path, uint32_t *off_l, uint32_t *on_l,
                       int *grid)
{
    cxg_blob b;
    uint32_t cnt, i;
    int      have_off = 0, have_on = 0, have_sp = 0;
    uint32_t sp_size = 0, sp_off = 0;
    int      rc = 1;

    if (cxg_blob_load(&b, path) != 0)
        return 1;
    if (b.n < 0x1000) {
        fprintf(stderr, "%s: short .bgd header\n", path);
        goto done;
    }
    cnt = cxg_u32(&b, 0x260);
    for (i = 0; i < cnt; i++) {
        uint64_t idv;
        uint32_t rec_off, laps;
        char     eid[16];

        if (8 + (int64_t)i * 8 + 8 > 0x1000 ||
            0x198 + (int64_t)i * 4 + 4 > 0x1000) {
            fprintf(stderr, "%s: event %u is past the 0x1000 header\n",
                    path, i);
            goto done;
        }
        idv     = cxg_u64(&b, 8 + (int64_t)i * 8);
        rec_off = cxg_u32(&b, 0x198 + (int64_t)i * 4);
        cxg_b40(idv, eid);
        if ((uint64_t)rec_off + 0x3C4 > b.n)
            continue;                    /* the record is not in the file */
        laps = cxg_u32(&b, (int64_t)rec_off + 0x3B8);
        if (!strcmp(eid, "OFFSGRCF") && !have_off) {
            *off_l = laps;
            have_off = 1;
            sp_size = cxg_u32(&b, (int64_t)rec_off + 0x3BC);
            sp_off  = cxg_u32(&b, (int64_t)rec_off + 0x3C0);
            have_sp = 1;
        } else if (!strcmp(eid, "ONSGRCF") && !have_on) {
            *on_l = laps;
            have_on = 1;
        }
    }
    *grid = 0;
    if (have_sp) {
        int k;

        for (k = 0; k < 6; k++) {
            int64_t o = (int64_t)sp_off + (int64_t)k * 0x50 + 0x30;
            int     j, any = 0;

            if ((uint64_t)(k * 0x50 + 0x30 + 12) > sp_size)
                break;
            for (j = 0; j < 3; j++) {
                double v = (double)cxg_f32(&b, o + j * 4);

                if (fabs(v) > 1e-6)
                    any = 1;
            }
            if (any)
                (*grid)++;
        }
    }
    if (!have_off || !have_on) {
        fprintf(stderr, "%s: no %s single-race template\n", path,
                have_off ? "ONSGRCF" : "OFFSGRCF");
        goto done;
    }
    rc = cxg_oob(&b) ? 1 : 0;
done:
    cxg_blob_free(&b);
    return rc;
}

/* Where the correctly-mapped image lives.  $B3_ELF, then the port's own
 * build/burnout3.elf (what the python spec reads), then the game's raw XBE. */
static int ts_open_image(cxg_image *im, const char *game, const char *repo)
{
    const char *e = getenv("B3_ELF");
    char        path[4096];

    if (e && *e)
        return cxg_image_open(im, e);
    cxg_join(path, sizeof(path), repo, "build/burnout3.elf");
    if (cxg_image_open(im, path) == 0)
        return 0;
    cxg_join(path, sizeof(path), game, "default.xbe");
    return cxg_image_open(im, path);
}

static uint32_t ts_u32(const unsigned char *p, int i)
{
    p += i * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t ts_u64(const unsigned char *p, int i)
{
    uint64_t v = 0;
    int      k;

    p += i * 8;
    for (k = 7; k >= 0; k--)
        v = (v << 8) | p[k];
    return v;
}

static int ts_build(cxg_str *L, const char *game, const char *repo,
                    int *n_tracks_out, int *n_locs_out)
{
    cxg_image            im;
    cxg_globalus         gl;
    ts_track            *tr = NULL;
    ts_loc               locs[LOC_COUNT];
    char                 path[4096];
    const unsigned char *ids, *venue, *nm_uc, *nm_tc;
    const unsigned char *ln, *lt, *lf, *lk;
    const unsigned char *sid, *sun, *srw;
    int                  n = 0, i, r, rc = 1;
    int                  img_open = 0, gl_open = 0;

    tr = (ts_track *)calloc(TS_MAX_TRACKS, sizeof(*tr));
    if (!tr)
        return 1;
    if (ts_open_image(&im, game, repo) != 0)
        goto done;
    img_open = 1;
    cxg_join(path, sizeof(path), game, "Data/Globalus.bin");
    if (cxg_globalus_open(&gl, path) != 0)
        goto done;
    gl_open = 1;
    if (ts_read_tlist(game, tr, TS_MAX_TRACKS, &n) != 0)
        goto done;

    ids   = cxg_image_read(&im, VA_IDS, (size_t)n * 8);
    venue = cxg_image_read(&im, VA_VENUE, (size_t)n * 4);
    nm_uc = cxg_image_read(&im, VA_NAME_UC, (size_t)n * 4);
    nm_tc = cxg_image_read(&im, VA_NAME_TC, (size_t)n * 4);
    if (!ids || !venue || !nm_uc || !nm_tc) {
        fprintf(stderr, "cxg: the 36-entry track tables are not mapped\n");
        goto done;
    }

    for (i = 0; i < n; i++) {
        char     xid[16];
        char     bgd[4096];
        ts_track *t = &tr[i];

        cxg_b40(ts_u64(ids, i), xid);
        if (strcmp(xid, t->id) != 0) {
            fprintf(stderr, "XBE id table row %d = '%s', tlist.bin = '%s'\n",
                    i, xid, t->id);
            goto done;
        }
        /* FUN_00157630: the region is the id's first character. */
        t->region = (t->id[0] == 'U') ? 0 : (t->id[0] == 'E') ? 1 :
                    (t->id[0] == 'A') ? 2 : -1;
        if (t->region < 0) {
            fprintf(stderr, "track %s has no region\n", t->id);
            goto done;
        }
        t->kind    = t->id[3];
        t->variant = t->id[7] - '0';
        snprintf(t->dir, sizeof(t->dir), "%c%c/%c%c_%c%c",
                 t->id[0], t->id[1], t->id[3], t->id[4], t->id[6], t->id[7]);
        snprintf(bgd, sizeof(bgd), "%s/Tracks/%c%c/%c%c_%c%c/Gamedata.bgd",
                 game, t->id[0], t->id[1], t->id[3], t->id[4],
                 t->id[6], t->id[7]);
        if (ts_read_bgd(bgd, &t->laps, &t->laps_online, &t->grid) != 0)
            goto done;
        t->venue_str = ts_u32(venue, i);
        t->uc_str    = ts_u32(nm_uc, i);
        t->tc_str    = ts_u32(nm_tc, i);
        cxg_globalus_get(&gl, t->venue_str, t->venue, sizeof(t->venue));
        cxg_globalus_get(&gl, t->uc_str, t->uc, sizeof(t->uc));
        cxg_globalus_get(&gl, t->tc_str, t->tc, sizeof(t->tc));
    }

    /* sibling variant: the other row with the same region+code  [C by id] */
    for (i = 0; i < n; i++) {
        int k;

        tr[i].sibling = -1;
        for (k = 0; k < n; k++)
            if (k != i && !strncmp(tr[k].id, tr[i].id, 5))
                tr[i].sibling = tr[k].index;
        /* FUN_00079590 / FUN_0008D020: skip kind 'M', skip flagsA != 0  [C] */
        tr[i].in_custom = (tr[i].kind != 'M' && tr[i].flag_p == 0);
    }

    /* ---- the 28 region-map locations ------------------------------------ */
    ln = cxg_image_read(&im, VA_LOC_NAME, LOC_COUNT * 4);
    lt = cxg_image_read(&im, VA_LOC_TYPE, LOC_COUNT);
    lf = cxg_image_read(&im, VA_LOC_FLAG, LOC_COUNT);
    lk = cxg_image_read(&im, VA_LOC_KIND, 24);
    if (!ln || !lt || !lf || !lk) {
        fprintf(stderr, "cxg: the region-map tables are not mapped\n");
        goto done;
    }
    for (i = 0; i < LOC_COUNT; i++) {
        locs[i].index  = i;
        locs[i].region = (i < LOC_BASE[1]) ? 0 : (i < LOC_BASE[2]) ? 1 : 2;
        locs[i].name_str = ts_u32(ln, i);
        cxg_globalus_get(&gl, locs[i].name_str, locs[i].name,
                         sizeof(locs[i].name));
        locs[i].type = lt[i];
        if (lt[i] >= 6) {
            fprintf(stderr, "location %d has type %u, past DAT_0039F978\n",
                    i, (unsigned)lt[i]);
            goto done;
        }
        locs[i].kind     = ts_u32(lk, lt[i]);
        locs[i].is_crash = (locs[i].kind == 0x32);
        locs[i].flag     = lf[i];
    }

    /* ---- the ten special events ----------------------------------------- */
    sid = cxg_image_read(&im, VA_SPECIAL_ID, N_SPECIAL * 8);
    sun = cxg_image_read(&im, VA_SPECIAL_UNLOCK, N_SPECIAL * 4);
    srw = cxg_image_read(&im, VA_SPECIAL_REWARD, N_SPECIAL * 4);
    if (!sid || !sun || !srw) {
        fprintf(stderr, "cxg: the special-event tables are not mapped\n");
        goto done;
    }

    /* ======================================================== the emit === */
    cxg_sputs(L, CXG_TS_TOP);
    cxg_nl(L);

    /* ---------------- regions ---------------- */
    cxg_sline(L, "/* The three regions, in retail's order -- FUN_00157630 returns this");
    cxg_sline(L, " * index straight from the track id's first character. [C] */");
    cxg_sline(L, "static const B3TrackRegion B3_TRACK_REGIONS[3] = {");
    for (r = 0; r < 3; r++) {
        char a[64], b[64], c[64], d[64], e[64], f[64];

        cxg_sline(L, "    { %-2d, %-4d, %-4d, %-11s, %-10s, %-10s, %-10s, "
                     "%-14s, %-14s },",
                  r, REGION_STR[r], REGION_STR_TC[r],
                  cxg_cstr(REGION_NAME[r], a, sizeof(a)),
                  cxg_cstr(REGION_ART[r].silhouette, b, sizeof(b)),
                  cxg_cstr(REGION_ART[r].continent, c, sizeof(c)),
                  cxg_cstr(REGION_ART[r].lsat, d, sizeof(d)),
                  cxg_cstr(REGION_ART[r].mip, e, sizeof(e)),
                  cxg_cstr(REGION_ART[r].tile, f, sizeof(f)));
    }
    cxg_sline(L, "};");
    cxg_nl(L);

    /* ---------------- tracks ---------------- */
    cxg_sline(L, "/* All 36 shipped tracks, in Tracks/tlist.bin order == retail's menu");
    cxg_sline(L, " * order.  Row .order is that index; every XBE table above is");
    cxg_sline(L, " * indexed by it, and FUN_00079590 walks 0..count-1 in this order. */");
    cxg_sline(L, "static const B3TrackSelect B3_TRACKS[B3_TRACK_COUNT] = {");
    for (i = 0; i < n; i++) {
        const ts_track *t = &tr[i];
        const char     *hd = NULL, *st = NULL;
        char            q1[64], q2[64], q3[300], q4[300], q5[300];
        char            qa[64], qb[64];
        size_t          k;

        for (k = 0; k < sizeof(VENUE_ART) / sizeof(VENUE_ART[0]); k++)
            if (!strcmp(VENUE_ART[k].venue, t->venue)) {
                hd = VENUE_ART[k].headline;
                st = VENUE_ART[k].sigtd;
                break;
            }
        cxg_cstr(t->id, q1, sizeof(q1));
        strcat(q1, ",");
        cxg_cstr(t->dir, q2, sizeof(q2));
        strcat(q2, ",");
        cxg_sline(L, "    { %2d, %-11s %-11s %d, '%c', %d, %2d, %d, %d,",
                  t->index, q1, q2, t->region, t->kind, t->variant,
                  t->sibling, (int)t->flag_p, (int)t->flag_offline);
        cxg_cstr(t->venue, q3, sizeof(q3) - 2);
        strcat(q3, ",");
        cxg_sline(L, "      %4d, %-26s", (int)t->venue_str, q3);
        cxg_cstr(t->uc, q4, sizeof(q4) - 2);
        strcat(q4, ",");
        cxg_sline(L, "      %4d, %-31s", (int)t->uc_str, q4);
        cxg_cstr(t->tc, q5, sizeof(q5) - 2);
        strcat(q5, ",");
        cxg_sline(L, "      %4d, %-31s", (int)t->tc_str, q5);
        if (hd) {
            cxg_cstr(hd, qa, sizeof(qa) - 2);
            strcat(qa, ",");
            cxg_cstr(st, qb, sizeof(qb) - 2);
            strcat(qb, ",");
        } else {
            snprintf(qa, sizeof(qa), "NULL,");
            snprintf(qb, sizeof(qb), "NULL,");
        }
        cxg_sline(L, "      %d, %d, %d, %d, %-20s %-22s },",
                  (int)t->laps, (int)t->laps_online, t->grid, t->in_custom,
                  qa, qb);
    }
    cxg_sline(L, "};");
    cxg_nl(L);

    /* ---------------- locations ---------------- */
    cxg_sline(L, "/* The region-map LOCATION list -- the list FUN_000DF960 indexes with");
    cxg_sline(L, " * `region_base[region] + cursor`.  Region bases 0 / 9 / 21 and the");
    cxg_sline(L, " * 28 rows are both [C]; `kind` 0x2F = race location (18 of them, one");
    cxg_sline(L, " * per venue), 0x32 = crash junction location (10, one per circuit). */");
    cxg_sline(L, "static const unsigned char B3_LOCATION_BASE[3] = { %d, %d, %d };",
              LOC_BASE[0], LOC_BASE[1], LOC_BASE[2]);
    cxg_sline(L, "static const B3TrackLocation B3_LOCATIONS[B3_LOCATION_COUNT] = {");
    for (i = 0; i < LOC_COUNT; i++) {
        char q[300];

        cxg_cstr(locs[i].name, q, sizeof(q) - 2);
        strcat(q, ",");
        cxg_sline(L, "    { %2d, %d, %4d, %-22s 0x%02X, %d, %d },",
                  locs[i].index, locs[i].region, (int)locs[i].name_str, q,
                  (unsigned)locs[i].kind, locs[i].is_crash,
                  (int)locs[i].flag);
    }
    cxg_sline(L, "};");
    cxg_nl(L);

    /* ---------------- special events ---------------- */
    cxg_sline(L, "/* The ten SPECIAL EVENTS and the postcard each awards.  Ids and both");
    cxg_sline(L, " * string columns are [C] (DAT_0039E880 / 0x39E8D0 / 0x39E8F8); the");
    cxg_sline(L, " * .png column is [S], matched by name -- but PCus-p2p2a / PCeu-p2p2");
    cxg_sline(L, " * pin themselves to the USP2 / EUP2 ids. */");
    cxg_sline(L, "static const B3SpecialEvent B3_SPECIAL_EVENTS[B3_SPECIAL_COUNT] = {");
    for (i = 0; i < N_SPECIAL; i++) {
        uint64_t v = ts_u64(sid, i);
        char     name[16], q[64], p[64];

        cxg_b40(v, name);
        cxg_cstr(name, q, sizeof(q) - 2);
        strcat(q, ",");
        cxg_cstr(POSTCARD_ART[i], p, sizeof(p));
        cxg_sline(L, "    { %-16s 0x%016" PRIX64 "ULL, %4d, %4d, %-20s },",
                  q, v, (int)ts_u32(sun, i), (int)ts_u32(srw, i), p);
    }
    cxg_sline(L, "};");
    cxg_nl(L);
    cxg_sputs(L, CXG_TS_BOTTOM);
    cxg_sputc(L, '\n');

    *n_tracks_out = n;
    *n_locs_out = LOC_COUNT;
    rc = 0;

done:
    if (gl_open)
        cxg_globalus_close(&gl);
    if (img_open)
        cxg_image_close(&im);
    free(tr);
    return rc;
}

int cx_extract_gen_trackselect(const char *game_dir, const char *out_root)
{
    const char *repo = cxg_repo_root();
    cxg_str     L = { 0 };
    char        out[4096];
    int         nt = 0, nl = 0, rc;

    cxg_gen_path(out, sizeof(out), out_root, "burnout3_trackselect.h");
    rc = ts_build(&L, game_dir, repo, &nt, &nl);
    if (rc == 0)
        rc = cxg_str_write(&L, out);
    cxg_str_free(&L);
    if (rc == 0)
        printf("wrote %s (%d tracks, %d locations, %d special events)\n",
               out, nt, nl, N_SPECIAL);
    return rc;
}

int cx_gen_trackselect_check(const char *game_dir, const char *path)
{
    const char *repo = cxg_repo_root();
    cxg_str     L = { 0 };
    char        def[4096];
    FILE       *cur;
    int         nt = 0, nl = 0, rc;

    if (!path) {
        cxg_join(def, sizeof(def), repo, "src/burnout3_trackselect.h");
        path = def;
    }
    rc = ts_build(&L, game_dir, repo, &nt, &nl);
    if (rc == 0) {
        /* An ABSENT header is not stale -- it was PURGED on purpose: it
         * carried the retail display strings, which a user-supplies-assets
         * build cannot ship, and the port now reads the same tables at boot
         * (src/burnout3_trackselect_runtime.h).  gen_trackselect.py --check
         * was adjusted the same way; this mirrors it, message included.
         * The build above still runs, so --check keeps proving the table
         * can be regenerated from the user's own files. */
        cur = fopen(path, "rb");
        if (!cur) {
            printf("burnout3_trackselect.h is PURGED (the port loads this "
                   "table at boot -- src/burnout3_trackselect_runtime.h)\n");
        } else {
            fclose(cur);
            if (!cxg_str_matches_file(&L, path)) {
                fprintf(stderr, "burnout3_trackselect.h is STALE\n");
                rc = 1;
            } else {
                printf("burnout3_trackselect.h up to date\n");
            }
        }
    }
    cxg_str_free(&L);
    return rc;
}
