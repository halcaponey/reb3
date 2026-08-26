#!/usr/bin/env python3
"""The TRACK SELECT data set, and where every column of it comes from.

SUPERSEDED AS A GENERATOR, KEPT AS THE SPEC
-------------------------------------------
This tool used to emit src/burnout3_trackselect.h.  It does not any more: that
header carried the retail DISPLAY STRINGS ("SILVER LAKE SOUTHBOUND" and 100
more, straight out of Data/Globalus.bin), which are the publisher's content
and cannot ship in the source of a user-supplies-assets build.  The port now
reads the very same tables at boot, out of the user's own files --
src/burnout3_trackselect_runtime.h, build/burnout3.elf + build/Globalus.bin,
with the lap count coming from build/tracks/<id>/pace.bin.  Writing into src/
is refused below; --check still works and reports the header as PURGED.

What stays is the map below, which is the provenance record for the whole
screen -- including the 28 region-map LOCATIONS and the ten SPECIAL EVENTS,
which the port never read at all (they were documentation carried as C data,
and the runtime table drops them).

Nothing in this file is typed in by hand except the provenance comments and
the two *asset* maps at the bottom, which are marked [S] because the XBE
contains the texture NAMES but no code reference to them (see the header's
"ART" section and docs/RE_FRONTEND.md 7.4).  Every row of the track table is
read out of the shipped data or out of the correctly-mapped XBE image:

  Tracks/tlist.bin            36 packed track ids + flagsA/flagsB
                              (loaded verbatim to 0x004D3000: +4 count,
                               +8 flagsA, +0x208 flagsB, +0x408 ids)    [C]
  DAT_0039EBC0  u64[36]       the XBE's own copy of the packed ids, the
                              array FUN_00158640 searches                [C]
  DAT_0039ECE0  u32[36]       Globalus idx, UPPERCASE directional name   [C]
  DAT_0039ED70  u32[36]       Globalus idx, Title Case directional name  [C]
  DAT_0039EE00  u32[36]       Globalus idx, venue name (FUN_00158680)    [C]
  FUN_00157630                region from id[0]: 'U'->0 'E'->1 'A'->2    [C]
  FUN_001574F0                id -> "tracks/<REG>/<Cn>_<Vn>/"            [C]
  Tracks/<..>/Gamedata.bgd    event record P+0x3B8 = lap count; the
                              OFFSGRCF / ONSGRCF slots are the offline /
                              online SINGLE RACE templates               [C]
  <..> event SPATIAL record   6 start-grid slots of 0x50 bytes           [C]
  DAT_0039F9B0  u32[28]       region-map LOCATION list (Globalus idx)    [C]
  DAT_0039F990  u8[28]        location type -> DAT_0039F978[]            [C]
  DAT_0039FA20  u8[28]        second per-location byte, meaning open     [?]
  DAT_0039E880  u64[10]       the ten SPECIAL EVENT ids                  [C]
  DAT_0039E8D0  u32[10]       their "UNLOCK THE SPECIAL EVENT IN X"      [C]
  DAT_0039E8F8  u32[10]       their "<X> POSTCARD" reward names          [C]

Usage:  python3 tools/gen_trackselect.py [--check]
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from extract_tlist import (Image, b40, globalus_strings, read_tlist,   # noqa
                           GAME_DIR, TRACKS_DIR)

OUT = os.path.join(ROOT, "src", "burnout3_trackselect.h")

# --- the four parallel 36-entry XBE tables, all indexed by tlist index [C] ---
VA_IDS      = 0x0039EBC0   # u64[36], searched by FUN_00158640
VA_NAME_UC  = 0x0039ECE0   # u32[36], "SILVER LAKE SOUTHBOUND"
VA_NAME_TC  = 0x0039ED70   # u32[36], "Silver Lake Southbound"
VA_VENUE    = 0x0039EE00   # u32[36], "SILVER LAKE"        (FUN_00158680)
# --- the region-map location list [C] -------------------------------------
VA_LOC_NAME = 0x0039F9B0   # u32[28] Globalus idx
VA_LOC_TYPE = 0x0039F990   # u8[28]  -> index into DAT_0039F978
VA_LOC_KIND = 0x0039F978   # u32[6]  = {0x2f,0x2f,0x32,0x30,0x30,0x33}
VA_LOC_FLAG = 0x0039FA20   # u8[28]  meaning open [?]
LOC_BASE    = (0, 9, 21)   # FUN_000DF960's region base offsets [C]
LOC_COUNT   = 28
# --- the ten special events [C] -------------------------------------------
VA_SPECIAL_ID     = 0x0039E880
VA_SPECIAL_UNLOCK = 0x0039E8D0
VA_SPECIAL_REWARD = 0x0039E8F8
N_SPECIAL = 10

REGION_NAME = ("USA", "EUROPE", "FAR EAST")
# Globalus indices for the region label, in the three casings retail ships:
#   227/378 SELECT REGION headers use 229/230/231 and 379/380/381 (identical
#   text); the multiplayer option ROW uses the Title Case 716/717/718. [C]
REGION_STR    = (229, 230, 231)
REGION_STR_TC = (716, 717, 718)

# [S] -- per-region map art.  The XBE .rdata string pool 0x003998A0..0x00399DE8
# holds every one of these names and Frontend.txd holds a texture for each,
# but NO code in default.xbe references the strings by address (checked by a
# full 4-byte immediate scan of every PT_LOAD byte, and by Ghidra xrefs), so
# the binding below is read off the ASSETS, not off code.  See header notes.
REGION_ART = (
    dict(silhouette="USA",     continent="USA1",    lsat="US_LSAT",
         mip="SATMAPU_L%d",    tile="SATMAPU%d_%d"),
    dict(silhouette="Europe",  continent="EUROPE1", lsat="EU_LSAT",
         mip="SATMAPE_L%d",    tile="SATMAPE%d_%d"),
    dict(silhouette="FarEast", continent="ASIA1",   lsat="AU_LSAT",
         mip="SATMAPA_L%d",    tile="SATMAPA%d_%d"),
)

# [S] -- venue -> crash-newspaper / signature-takedown art.  Ten venues only:
# exactly the ten locations DAT_0039F990 marks type 2 (the crash junctions).
# Matched by name; the file set has no other candidate and no leftovers.
VENUE_ART = {
    "WATERFRONT":      ("HDWaterfront",     "waterfront-st"),
    "DOWNTOWN":        ("HDDowntown",       "downtown-st"),
    "SILVER LAKE":     ("HDSilverLake",     "silverlake-ST"),
    "WINTER CITY":     ("HDWinterCity",     "wintercity-st"),
    "ALPINE":          ("HDAlpine",         "alpine-st"),
    "RIVIERA":         ("HDRiviera",        "riviera-st"),
    "VINEYARD":        ("HDVineyard",       "vineyard-st"),
    "GOLDEN CITY":     ("HDGoldenCity",     "goldencity-st"),
    "DOCKSIDE":        ("HDDockside",       "dockside-st"),
    "ISLAND PARADISE": ("HDIslandParadise", "islandparadise-st"),
}

# [S] -- the ten special-event reward POSTCARDS, in DAT_0039E880 order.  The
# order is [C] (the event ids decode to the venue), the file names are matched
# by name -- and PCus-p2p2a / PCeu-p2p2 are decisive, the ids are USP2 / EUP2.
POSTCARD_ART = ("PCWinterCity", "PCAlpine", "PCVineyard", "PCDockside",
                "PCIslandParadise", "PCGoldenCity", "PCus-p2p2a",
                "PCeu-p2p2", "PCSilverLake", "PCSpecialGP1")


def cstr(s):
    return '"%s"' % s.replace('\\', '\\\\').replace('"', '\\"')


def read_bgd(path):
    """-> (offline_laps, online_laps, valid_grid_slots) for one track. [C]"""
    f = open(path, 'rb')
    head = f.read(0x1000)
    cnt = struct.unpack_from('<I', head, 0x260)[0]
    off_l = on_l = None
    spatial = None
    for i in range(cnt):
        eid = b40(struct.unpack_from('<Q', head, 8 + i * 8)[0])
        rec_off = struct.unpack_from('<I', head, 0x198 + i * 4)[0]
        f.seek(rec_off)
        rec = f.read(0x800)
        laps = struct.unpack_from('<I', rec, 0x3B8)[0]
        if eid == 'OFFSGRCF' and off_l is None:
            off_l = laps
            spatial = struct.unpack_from('<II', rec, 0x3BC)   # size, offset
        elif eid == 'ONSGRCF' and on_l is None:
            on_l = laps
    grid = 0
    if spatial:
        f.seek(spatial[1])
        S = f.read(spatial[0])
        for k in range(6):
            p = struct.unpack_from('<3f', S, k * 0x50 + 0x30)
            if any(abs(v) > 1e-6 for v in p):
                grid += 1
    f.close()
    return off_l, on_l, grid


def main():
    img = Image()
    _, gstr = globalus_strings()
    rows = read_tlist()
    n = len(rows)

    ids = struct.unpack_from('<%dQ' % n, img.read(VA_IDS, n * 8), 0)
    venue = struct.unpack_from('<%dI' % n, img.read(VA_VENUE, n * 4), 0)
    nm_uc = struct.unpack_from('<%dI' % n, img.read(VA_NAME_UC, n * 4), 0)
    nm_tc = struct.unpack_from('<%dI' % n, img.read(VA_NAME_TC, n * 4), 0)

    tracks = []
    for i, tid, fa, fb in rows:
        reg_s, code, var = tid.split('_')
        assert b40(ids[i]) == tid, \
            "XBE id table row %d = %r, tlist.bin = %r" % (i, b40(ids[i]), tid)
        region = {'U': 0, 'E': 1, 'A': 2}[reg_s[0]]
        off_l, on_l, grid = read_bgd(
            os.path.join(TRACKS_DIR, reg_s, "%s_%s" % (code, var),
                         "Gamedata.bgd"))
        tracks.append(dict(
            index=i, id=tid, dir="%s/%s_%s" % (reg_s, code, var),
            region=region, kind=code[0], variant=int(var[1]),
            flag_p=fa, flag_offline=fb,
            venue_str=venue[i], venue=gstr(venue[i]),
            uc_str=nm_uc[i], uc=gstr(nm_uc[i]),
            tc_str=nm_tc[i], tc=gstr(nm_tc[i]),
            laps=off_l, laps_online=on_l, grid=grid))

    # sibling variant: the other row with the same region+code   [C by id]
    for t in tracks:
        t['sibling'] = -1
        for u in tracks:
            if u is not t and u['id'][:5] == t['id'][:5]:
                t['sibling'] = u['index']
        # FUN_00079590 / FUN_0008D020: skip code[0]=='M', skip flagsA!=0 [C]
        t['in_custom'] = int(t['kind'] != 'M' and t['flag_p'] == 0)

    # ---- the 28 region-map locations -------------------------------------
    ln = struct.unpack_from('<%dI' % LOC_COUNT,
                            img.read(VA_LOC_NAME, LOC_COUNT * 4), 0)
    lt = img.read(VA_LOC_TYPE, LOC_COUNT)
    lf = img.read(VA_LOC_FLAG, LOC_COUNT)
    lk = struct.unpack_from('<6I', img.read(VA_LOC_KIND, 24), 0)
    locs = []
    for i in range(LOC_COUNT):
        region = 0 if i < LOC_BASE[1] else (1 if i < LOC_BASE[2] else 2)
        kind = lk[lt[i]]
        locs.append(dict(index=i, region=region, name_str=ln[i],
                         name=gstr(ln[i]), type=lt[i], kind=kind,
                         is_crash=int(kind == 0x32), flag=lf[i]))

    # ---- the ten special events ------------------------------------------
    sid = struct.unpack_from('<%dQ' % N_SPECIAL,
                             img.read(VA_SPECIAL_ID, N_SPECIAL * 8), 0)
    sun = struct.unpack_from('<%dI' % N_SPECIAL,
                             img.read(VA_SPECIAL_UNLOCK, N_SPECIAL * 4), 0)
    srw = struct.unpack_from('<%dI' % N_SPECIAL,
                             img.read(VA_SPECIAL_REWARD, N_SPECIAL * 4), 0)

    out = []
    w = out.append
    w(HEADER_TOP)

    # ---------------- regions ----------------
    w("/* The three regions, in retail's order -- FUN_00157630 returns this")
    w(" * index straight from the track id's first character. [C] */")
    w("static const B3TrackRegion B3_TRACK_REGIONS[3] = {")
    for r in range(3):
        a = REGION_ART[r]
        w("    { %-2d, %-4d, %-4d, %-11s, %-10s, %-10s, %-10s, %-14s, %-14s },"
          % (r, REGION_STR[r], REGION_STR_TC[r], cstr(REGION_NAME[r]),
             cstr(a['silhouette']), cstr(a['continent']), cstr(a['lsat']),
             cstr(a['mip']), cstr(a['tile'])))
    w("};")
    w("")

    # ---------------- tracks ----------------
    w("/* All 36 shipped tracks, in Tracks/tlist.bin order == retail's menu")
    w(" * order.  Row .order is that index; every XBE table above is")
    w(" * indexed by it, and FUN_00079590 walks 0..count-1 in this order. */")
    w("static const B3TrackSelect B3_TRACKS[B3_TRACK_COUNT] = {")
    for t in tracks:
        art = VENUE_ART.get(t['venue'], (None, None))
        w("    { %2d, %-11s %-11s %d, '%s', %d, %2d, %d, %d,"
          % (t['index'], cstr(t['id']) + ",", cstr(t['dir']) + ",",
             t['region'], t['kind'], t['variant'], t['sibling'],
             t['flag_p'], t['flag_offline']))
        w("      %4d, %-26s" % (t['venue_str'], cstr(t['venue']) + ","))
        w("      %4d, %-31s" % (t['uc_str'], cstr(t['uc']) + ","))
        w("      %4d, %-31s" % (t['tc_str'], cstr(t['tc']) + ","))
        w("      %d, %d, %d, %d, %-20s %-22s },"
          % (t['laps'], t['laps_online'], t['grid'], t['in_custom'],
             (cstr(art[0]) + ",") if art[0] else "NULL,",
             (cstr(art[1]) + ",") if art[1] else "NULL,"))
    w("};")
    w("")

    # ---------------- locations ----------------
    w("/* The region-map LOCATION list -- the list FUN_000DF960 indexes with")
    w(" * `region_base[region] + cursor`.  Region bases 0 / 9 / 21 and the")
    w(" * 28 rows are both [C]; `kind` 0x2F = race location (18 of them, one")
    w(" * per venue), 0x32 = crash junction location (10, one per circuit). */")
    w("static const unsigned char B3_LOCATION_BASE[3] = { %d, %d, %d };"
      % LOC_BASE)
    w("static const B3TrackLocation B3_LOCATIONS[B3_LOCATION_COUNT] = {")
    for L in locs:
        w("    { %2d, %d, %4d, %-22s 0x%02X, %d, %d },"
          % (L['index'], L['region'], L['name_str'], cstr(L['name']) + ",",
             L['kind'], L['is_crash'], L['flag']))
    w("};")
    w("")

    # ---------------- special events ----------------
    w("/* The ten SPECIAL EVENTS and the postcard each awards.  Ids and both")
    w(" * string columns are [C] (DAT_0039E880 / 0x39E8D0 / 0x39E8F8); the")
    w(" * .png column is [S], matched by name -- but PCus-p2p2a / PCeu-p2p2")
    w(" * pin themselves to the USP2 / EUP2 ids. */")
    w("static const B3SpecialEvent B3_SPECIAL_EVENTS[B3_SPECIAL_COUNT] = {")
    for i in range(N_SPECIAL):
        w("    { %-16s 0x%016XULL, %4d, %4d, %-20s }," %
          (cstr(b40(sid[i])) + ",", sid[i], sun[i], srw[i],
           cstr(POSTCARD_ART[i])))
    w("};")
    w("")
    w(HEADER_BOTTOM)

    text = "\n".join(out) + "\n"
    if '--check' in sys.argv:
        if not os.path.exists(OUT):
            print("burnout3_trackselect.h is PURGED (the port loads this "
                  "table at boot -- src/burnout3_trackselect_runtime.h)")
            return 0
        cur = open(OUT).read()
        if cur != text:
            print("burnout3_trackselect.h is STALE", file=sys.stderr)
            return 1
        print("burnout3_trackselect.h up to date")
        return 0
    dest = None
    for i, arg in enumerate(sys.argv):
        if arg == '--out' and i + 1 < len(sys.argv):
            dest = sys.argv[i + 1]
    if dest is None:
        print("tools/gen_trackselect.py no longer writes a src/ header: the "
              "retail DISPLAY STRINGS in it are the publisher's content, and "
              "the port now reads the same tables at boot out of the user's "
              "own build/burnout3.elf + build/Globalus.bin "
              "(src/burnout3_trackselect_runtime.h).\n"
              "  This module stays as the SPEC -- its docstring carries the "
              "VA and Globalus provenance for every column, and --check keeps "
              "working.  Pass --out <path> OUTSIDE src/ to dump the old "
              "header shape for comparison.", file=sys.stderr)
        return 1
    if os.path.abspath(dest).startswith(os.path.join(ROOT, 'src') + os.sep):
        print("refusing to write %s: baked game data does not belong in src/ "
              "(tools/validate_no_baked_data.py enforces this)" % dest,
              file=sys.stderr)
        return 1
    open(dest, 'w').write(text)
    print("wrote %s (%d tracks, %d locations, %d special events)"
          % (dest, len(tracks), len(locs), N_SPECIAL))
    return 0


HEADER_TOP = r'''// GENERATED by tools/gen_trackselect.py -- do not edit by hand.
//
// The retail TRACK SELECT data set: the 36 shipped tracks, the three regions,
// the 28 region-map locations, and the ten special events.  Provenance for
// every column is in the generator's docstring and in the per-field comments
// below; docs/RE_FRONTEND.md section 7 carries the screen spec.
//
// ============================================================ WHAT IS [C] ==
// Everything numeric here is read at run time out of Tracks/tlist.bin, out of
// the per-track Gamedata.bgd, or out of the correctly-mapped XBE image at the
// VA named in the generator.  The generator asserts that the XBE's own id
// table (DAT_0039EBC0, the array FUN_00158640 searches) decodes to the same
// 36 ids as tlist.bin, so a misaligned table cannot slip through.
//
// ============================================================ WHAT IS [S] ==
// The *art* columns.  The XBE .rdata string pool 0x003998A0..0x00399DE8 holds
// "HDSilverLake", "PCSilverLake", "SatMapU1_1", "silverlake-ST1", "US_LSat",
// "World_Map" and the rest, and Frontend.txd holds a texture for each -- but
// NOTHING in default.xbe references those strings by address.  A byte-aligned
// 4-byte immediate scan over every PT_LOAD byte of the image finds zero hits,
// and Ghidra agrees (0 xrefs).  The frontend that consumes them is
// data-driven, so the per-track binding below is read off the ASSETS
// themselves, not off code.  It is marked [S] and it is the best-supported
// approximation, not a recovered fact.
//
// One correction the assets force, and the orchestrator must not miss:
// RETAIL HAS NO PER-TRACK PREVIEW PHOTO.  The three obvious candidates are
// all something else --
//   HD*.png (256x256)      newspaper front pages for the CRASH results screen
//                          ("ALPINE SMASH!", "SILVER LAKE LUNACY!"), one per
//                          crash venue, 10 of them.
//   *-st1/-st2.png (256^2) Polaroid photos of the SIGNATURE TAKEDOWNS
//                          ("Avalanche!", "Gone Fishin'"), two per crash
//                          venue, 20 of them.
//   PC*.png (256x128)      the ten special-event reward POSTCARDS.
// What the track select actually shows is the region's satellite map with a
// route overlay -- see B3_TRACK_REGIONS and docs/RE_FRONTEND.md 7.3.

#ifndef BURNOUT3_TRACKSELECT_H
#define BURNOUT3_TRACKSELECT_H

#include <stddef.h>          /* NULL -- the M/P rows carry no venue art */

#define B3_TRACK_COUNT     36   /* Tracks/tlist.bin +0x004                 [C] */
#define B3_LOCATION_COUNT  28   /* FUN_000DF960's bases 0/9/21 + 7         [C] */
#define B3_SPECIAL_COUNT   10   /* DAT_0039E880 .. 0x0039E8C8              [C] */
#define B3_RACE_GRID       6    /* .bgd SPATIAL record: 6 x 0x50 slots     [C] */

/* Region indices exactly as FUN_00157630 returns them from the track id's
 * first character: 'U' -> 0, 'E' -> 1, 'A' -> 2, anything else -> 3.   [C] */
#define B3_REGION_USA      0
#define B3_REGION_EUROPE   1
#define B3_REGION_FAREAST  2

/* Location kinds, the values FUN_000DF960 reads out of DAT_0039F978 and
 * compares against 0x2F.                                               [C] */
#define B3_LOC_RACE        0x2F
#define B3_LOC_CRASH       0x32

typedef struct {
    unsigned char  index;        /* 0..2, == FUN_00157630's return       [C] */
    unsigned short name_str;     /* Globalus "USA"/"EUROPE"/"FAR EAST"   [C] */
    unsigned short name_str_tc;  /* Globalus "USA"/"Europe"/"Far East"   [C] */
    const char    *name;         /* English literal fallback             [C] */
    const char    *art_silhouette; /* 256x256 flat map icon              [S] */
    const char    *art_continent;  /* 512x512 satellite continent        [S] */
    const char    *art_lsat;       /* 256x256 regional satellite         [S] */
    const char    *art_mip_fmt;    /* printf "%d" 1..4: 64x32..512x256   [S] */
    const char    *art_tile_fmt;   /* printf "%d,%d": map n=1..3, half 1|2 [S] */
} B3TrackRegion;

typedef struct {
    unsigned char  order;        /* tlist index == retail's menu order;
                                  * every XBE table is indexed by it     [C] */
    const char    *id;           /* "US_C3_V1", base-40 (FUN_001AECC0)   [C] */
    const char    *dir;          /* "US/C3_V1"  (FUN_001574F0)           [C] */
    unsigned char  region;       /* B3_REGION_*                          [C] */
    char           kind;         /* 'C' circuit, 'M' mixed, 'P' point2pt [C] */
    unsigned char  variant;      /* 1 = V1, 2 = V2                       [C] */
    signed char    sibling;      /* the other variant's index, -1 none   [C] */
    unsigned char  flag_p;       /* tlist flagsA -- 1 on the eight P     [C] */
    unsigned char  flag_offline; /* tlist flagsB -- 0 on the eight AS    [C] */
    unsigned short venue_str;    /* Globalus, DAT_0039EE00 (FUN_00158680)[C] */
    const char    *venue;        /* "SILVER LAKE"                        [C] */
    unsigned short name_str;     /* Globalus, DAT_0039ECE0 (UPPERCASE)   [C] */
    const char    *name;         /* "SILVER LAKE SOUTHBOUND"             [C] */
    unsigned short name_str_tc;  /* Globalus, DAT_0039ED70 (Title Case)  [C] */
    const char    *name_tc;      /* "Silver Lake Southbound"             [C] */
    unsigned char  laps;         /* OFFSGRCF, .bgd event P+0x3B8         [C] */
    unsigned char  laps_online;  /* ONSGRCF                              [C] */
    unsigned char  grid;         /* populated start-grid slots (all 6)   [C] */
    unsigned char  in_custom;    /* survives the custom-race filters     [C] */
    const char    *art_headline; /* HD*.png, crash venues only, NOT a
                                  * track-select preview                 [S] */
    const char    *art_sigtd;    /* "<venue>-st" stem, + "1"/"2"         [S] */
} B3TrackSelect;

typedef struct {
    unsigned char  index;        /* 0..27, the region-map slot           [C] */
    unsigned char  region;       /* B3_REGION_*                          [C] */
    unsigned short name_str;     /* Globalus venue name, DAT_0039F9B0    [C] */
    const char    *name;                                              /* [C] */
    unsigned char  kind;         /* B3_LOC_RACE / B3_LOC_CRASH           [C] */
    unsigned char  is_crash;                                          /* [C] */
    unsigned char  flag;         /* DAT_0039FA20, meaning not recovered  [?] */
} B3TrackLocation;

typedef struct {
    const char    *event_id;     /* base-40 decode of DAT_0039E880       [C] */
    unsigned long long packed;                                        /* [C] */
    unsigned short unlock_str;   /* "UNLOCK THE SPECIAL EVENT IN X"      [C] */
    unsigned short reward_str;   /* "<X> POSTCARD"                       [C] */
    const char    *art_postcard; /* PC*.png                              [S] */
} B3SpecialEvent;
'''

HEADER_BOTTOM = r'''/* =========================================================== THE UNLOCK MODEL
 *
 * Retail keeps per-track availability in a 36-BYTE ARRAY at DAT_0044D0CC,
 * indexed by the tlist index -- one byte per track, 0 locked / 1 unlocked.
 *
 *   FUN_0001BCC0(id_lo, id_hi)             the "is this track selectable?"
 *                                          predicate every menu calls  [C]
 *       if (FUN_001575F0(id) < 0) return 0;        // not in tlist.bin
 *       i = FUN_00158640(id);                      // tlist index
 *       if (i == -1) return 1;
 *       return ((unsigned char *)0x0044D0CC)[i];
 *
 *   FUN_0001C9D0(profile)                  rebuilds the whole array    [C]
 *       memset(0x0044D0CC, 0, 36);
 *       for (e = 0; e <= 0x48; e++)                // 73 World Tour events
 *           if (DAT_0044D01F[e] && DAT_0039E2A8[e]
 *               && *(char *)(profile + 0x386 + e) > 0) {
 *               // the event's track id(s): DAT_0039DF38[e*2] / DAT_0039DF3C
 *               // a GRAND PRIX (hi == 0, lo < 7) instead expands through
 *               // PTR_DAT_003ED0F8[lo], DAT_0039E778[lo] rounds
 *               //   DAT_0039E778 = { 3, 3, 3, 3, 4, 4, 4 }
 *               // -> every round's track unlocks
 *               ((unsigned char *)0x0044D0CC)[tlist_index_of(id)] = 1;
 *           }
 *       DAT_0044D164 = popcount(0x0044D0CC[0..35]);   // "tracks unlocked"
 *
 * So a track is unlocked IFF at least one World Tour event that runs on it
 * has been completed with a medal, and the state lives in the PROFILE, at
 * profile+0x386+event_index (one byte per event, 73 events).  There is no
 * per-track bit in the save -- the array is derived every time the profile
 * is loaded.
 *
 * ---- DELIBERATE DEVIATION -------------------------------------------------
 * The port ships with ALL 36 TRACKS UNLOCKED.  That is a decision, not an
 * oversight: there is no World Tour progression in the harness, so there is
 * no profile to derive DAT_0044D0CC from.  Modelled as
 *
 *     b3_track_unlocked(i)  ==  1   for every i
 *
 * i.e. FUN_0001BCC0 with the array pre-filled.  Nothing else about the
 * screen changes -- the filters below are retail's and still apply.
 * Do NOT implement locking; if it is ever wanted, the hook is exactly the
 * one predicate above.
 *
 * ================================================ THE CUSTOM-RACE FILTERS ==
 * FUN_00079590 (offline two-player setup) and FUN_0008D020 (its online
 * sibling) build the SELECT TRACK list by walking the loaded tlist at
 * 0x004D3000 from index 0 to count-1 and keeping a track only if ALL of:
 *
 *   [C 0x00079784]  id[3] != 'M'                     -- drops the 8 M tracks
 *   [C 0x00079797]  FUN_001575A0(id) == 0            -- tlist flagsA, drops
 *                                                       the 8 P tracks
 *   [C 0x000797A6]  FUN_0001BCC0(id) != 0            -- unlocked
 *   [C 0x000797B1]  FUN_00157630(id) == chosen_region
 *
 * leaving the 20 CIRCUIT tracks: 6 USA, 8 Europe, 6 Far East.  The name it
 * shows is DAT_0039ED70[i] -- the Title Case directional name -- with
 * Globalus 1257 ("Unknown Track") as the fallback for a missing index
 * (`mov eax, 0x4E9` at 0x000797E8).                                    [C]
 *
 * B3_TRACKS[i].in_custom carries the first two filters precomputed.
 *
 * ============================================================ RACE SETTINGS
 *   laps          the OFFSGRCF event's P+0x3B8: 3 on every circuit except
 *                 Alpine (EU_C2_*, which is 2 offline / 3 online), 2 on the
 *                 M tracks, 1 on the P tracks.                          [C]
 *   laps_online   the ONSGRCF event's P+0x3B8, for reference.           [C]
 *   grid          6 on all 36 tracks -- the event SPATIAL record has six
 *                 0x50-byte start-grid slots and all six are populated on
 *                 every shipped track.  Corroborated by Globalus 2559,
 *                 "PICK A TRACK AND TAKE ON UP TO FIVE OTHER RACERS", and
 *                 720, "This game mode requires a maximum of 6 players". [C]
 *
 * The custom-race screen exposes laps as an editable row whose min/max live
 * at screen+0xC8 / screen+0xCA (FUN_00079590's row kind 3); the values in
 * those two fields were not recovered.                                  [?]
 */

/* Convenience: the port unlocks everything (see above). */
#define b3_track_unlocked(i)  (1)

#endif /* BURNOUT3_TRACKSELECT_H */'''


if __name__ == "__main__":
    sys.exit(main())
