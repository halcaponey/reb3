#!/usr/bin/env python3
"""NO BAKED GAME DATA -- the gate for a user-supplies-assets distribution.

WHY THIS EXISTS
    The standing data-driven-tracks directive is "all track data is loaded
    from disk and NOTHING is cooked into the C source; any parameters are
    loaded from data files".  EIGHT generated headers used to violate it.

    Phase 1 took the five per-track ones:

        src/burnout3_track_paths.h    US_C3_V1's race line + wall strands
        src/burnout3_start_grid.h     US_C3_V1's six start-grid slots
        src/burnout3_ai_pace.h        all 36 tracks' .bgd AI pace records
        src/burnout3_trackselect.h    the retail Globalus DISPLAY STRINGS
        src/burnout3_traffic_data.h   US_C3_V1's traffic set (already dead)

    Phase 2 took the three that are not per-track, and so had no per-track
    asset to move into:

        src/burnout3_car_physics.h    every car's Data/vdb.xml tuning, 332 KB
        src/burnout3_vehicle_data.h   the 107-entry pveh/ roster
        src/burnout3_font.h           the three XBE fonts' glyph metrics

    -- plus the in-race HUD's own Globalus LABELS, which were typed into
    src/burnout3_hud.c as English literals with their recovered index in a
    trailing comment, and the two GLOBAL track fallbacks (build/track.obj and
    build/collision.bin, i.e. whichever track was extracted LAST).

    Each is now loaded at run time out of the user's own files.  A compiled-in
    copy is worse than a missing file, because it does not look missing: the
    port used to substitute Silver Lake's road, grid and traffic on every
    other track and the symptom read as a physics bug.  So this validator
    asserts BOTH halves -- the headers are gone AND the replacements really
    come off disk, proven by the loaders' own log lines on a real boot.

WHAT IT CHECKS
    1  source     none of the eight headers exists; nothing includes them;
                  their data symbols appear nowhere in src/; no retail
                  Globalus display string is left in the source -- track
                  names, venues, screen chrome AND the HUD labels; and no
                  global track/collision fallback path is left either
    2  assets     every track offered by the selector has the replacement
                  artefacts, pace.bin included, and the three dump-global
                  runtime assets exist and parse
    3  parity     pace.bin decodes to the same bytes tools/gen_ai_pace.py
                  reads out of Gamedata.bgd  (skipped without a game dump);
                  roster.bin re-derives from the user's own pveh/ headers;
                  font.bin re-derives from the user's own XBE; and
                  car_physics.bin keys 1:1 onto the roster
    4  boot       two tracks boot and every replacement announces the FILE it
                  loaded from -- one track with a race-line pool and one
                  without, so both arms of the aim recovery are exercised --
                  including the three phase-2 loaders
    5  loud       with an asset hidden, the port names the missing file and
                  stops, instead of silently driving another track's road or
                  another car's tuning

Run: python3 tools/validate_no_baked_data.py [--tracks A,B] [--seconds N]
                                             [--skip-boot]
"""
import argparse
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

# The purged headers, and the data symbols each of them defined.  A symbol
# reappearing in src/ means the data came back.
PURGED = {
    # ---- phase 1: the per-track headers -------------------------------
    "burnout3_track_paths.h":  ["B3_CENTERLINE", "B3_WALL_A", "B3_WALL_B",
                                "B3_ROUTE_START", "B3_LAP_LENGTH"],
    "burnout3_start_grid.h":   ["B3_START_GRID"],
    "burnout3_ai_pace.h":      ["B3_AI_PACE["],
    "burnout3_trackselect.h":  ["B3_LOCATIONS", "B3_SPECIAL_EVENTS",
                                "B3_LOCATION_BASE"],
    "burnout3_traffic_data.h": ["B3_TRAFFIC_CARS_TABLE"],
    # ---- phase 2: the dump-global ones --------------------------------
    # The needles are the TABLE DEFINITIONS, not the consumer spellings:
    # B3_CAR_PHYSICS / VEHICLES / b3_font_globalfont all still appear in the
    # source, and are supposed to -- they resolve to the runtime loaders now.
    # What must never come back is a definition.
    "burnout3_car_physics.h":  ["B3_CARPARAMS_",
                                "B3CarPhysics B3_CAR_PHYSICS["],
    "burnout3_vehicle_data.h": ["VehicleInfo VEHICLES[",
                                "#define VEHICLE_COUNT 1"],
    "burnout3_font.h":         ["B3Font b3_font_globalfont",
                                "B3Font b3_font_headfont",
                                "B3Font b3_font_smallfont"],
}

# The GLOBAL track fallbacks: build/track.obj and build/collision.bin are
# whatever track was extracted LAST, so loading either instead of the
# per-track file silently raced a different circuit -- exactly the hazard the
# purged headers had.  Both arms are gone; these are the needles that prove it
# (a comment naming the file is fine, a path literal in code is not).
GLOBAL_FALLBACKS = ['"build/track.obj"', '"build/collision.bin"']

# The retail display text is NOT listed here -- it is READ OUT OF THE USER'S
# OWN FILES and used as the grep needle, so this validator carries none of the
# publisher's strings either, and it checks all 36 names and 18 venues rather
# than a hand-picked handful.  Same tables tools/gen_trackselect.py documents:
#   build/burnout3.elf   DAT_0039ECE0 u32[n] Globalus idx, UPPERCASE name  [C]
#                        DAT_0039ED70 u32[n] Globalus idx, Title Case name [C]
#                        DAT_0039EE00 u32[n] Globalus idx, venue name      [C]
#                        DAT_0039EBC0 u64[n] packed track ids              [C]
#   build/Globalus.bin   u32 count @+0x08, u32 offsets @+0x10, UTF-16LE    [C]
ELF_PATH = os.path.join(ROOT, "build", "burnout3.elf")
GLOBALUS_PATH = os.path.join(ROOT, "build", "Globalus.bin")
VA_TRACK_IDS = 0x0039EBC0
VA_TRACK_NAME_UC = 0x0039ECE0
VA_TRACK_NAME_TC = 0x0039ED70
VA_TRACK_VENUE = 0x0039EE00
GSTR_REGION = (229, 230, 231)
# The TRACK SELECT screen's own chrome, which was also typed into
# src/burnout3_full.c as English literals and is retail text too.  Indices
# fixed by exact match against the user's Globalus.bin (see
# src/burnout3_trackselect_runtime.h for the block structure that corroborates
# them): the three SELECT headers, the four status marquees and the four
# game-mode names the selector lists as locked.
GSTR_CHROME = (227, 232, 383, 228, 234, 235, 236, 2559, 250, 252, 249, 254)
# The in-race HUD's own labels, which were typed into src/burnout3_hud.c as
# English literals with the index in a trailing comment.  Every index here was
# ALREADY in the source and is recovered code (the imm32 the element's
# constructor loads), so it stays; only the TEXT moved to boot time, into
# src/burnout3_hudstr_runtime.h.  Same rule as GSTR_CHROME: the strings are
# read from the USER'S OWN Globalus.bin and used as the grep needle, so this
# validator carries none of them either.
#   2002 POS  2003 LAP  1987 mph  2191 IMPACT TIME     (B3HUD_STR_* / 4)
#   2107..2113 the seven ticker row labels             (B3HUD_TICK_STR_* / 4)
#   1993..1998 "1st".."6th", the six-entry in-race run (B3HUD_TAG_STR_1ST / 4)
GSTR_HUD = ((2002, 2003, 1987, 2191)
            + tuple(range(2107, 2114)) + tuple(range(1993, 1999)))
B40_CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"


def b40(v):
    """FUN_001AECC0: base-40, LSB char first, then reversed. [C]"""
    out = []
    for _ in range(12):
        out.append(B40_CS[v % 40])
        v //= 40
    return "".join(reversed(out)).strip()


def elf_segments(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"\x7fELF":
        return None, None
    phoff = struct.unpack_from("<I", raw, 0x1C)[0]
    entsz, num = struct.unpack_from("<HH", raw, 0x2A)
    segs = []
    for i in range(num):
        typ, off, va, _pa, fsz, _msz = struct.unpack_from(
            "<6I", raw, phoff + i * entsz)
        if typ == 1:
            segs.append((va, off, fsz))
    return raw, segs


def retail_track_strings():
    """-> ({id: (venue, name_uc, name_tc)}, [regions], [chrome], [hud]).

    The exact table src/burnout3_trackselect_runtime.h builds at boot, built
    here independently so a divergence in either shows up.  The last two lists
    are the TRACK SELECT chrome and the in-race HUD labels: both are retail
    display text the port resolves by index at run time, and both are needles.
    They are kept apart because the HUD's are SHORT ("POS", "1st") and so need
    a lower length floor than the region names, one of which ("USA") is also a
    legitimate Frontend.txd art stem.  All four are None when the user's files
    are not present."""
    if not (os.path.exists(ELF_PATH) and os.path.exists(GLOBALUS_PATH)):
        return None, None, None, None
    raw, segs = elf_segments(ELF_PATH)
    if not segs:
        return None, None, None, None

    def rd(va, n):
        for base, off, fsz in segs:
            if base <= va < base + fsz and va - base + n <= fsz:
                return raw[off + va - base:off + va - base + n]
        return None

    with open(GLOBALUS_PATH, "rb") as f:
        gl = f.read()
    gcount = struct.unpack_from("<I", gl, 8)[0]

    def gstr(idx):
        if idx >= gcount:
            return None
        off = struct.unpack_from("<I", gl, 0x10 + idx * 4)[0]
        end = off
        while end + 1 < len(gl) and gl[end:end + 2] != b"\0\0":
            end += 2
        return gl[off:end].decode("utf-16-le", "replace")

    tracks = {}
    for i in range(128):
        p = rd(VA_TRACK_IDS + i * 8, 8)
        if p is None:
            break
        tid = b40(struct.unpack_from("<Q", p, 0)[0])
        if not (len(tid) == 8 and tid[2] == "_" and tid[5] == "_"):
            break
        cols = []
        for va in (VA_TRACK_VENUE, VA_TRACK_NAME_UC, VA_TRACK_NAME_TC):
            q = rd(va + i * 4, 4)
            cols.append(gstr(struct.unpack_from("<I", q, 0)[0]) if q else None)
        tracks[tid] = tuple(cols)
    return (tracks, [gstr(s) for s in GSTR_REGION],
            [gstr(s) for s in GSTR_CHROME], [gstr(s) for s in GSTR_HUD])

# The per-track artefacts that replaced the headers.  route.bin carries the
# route, the wall strands, the nav graph AND the race line.
NEED = ["route.bin", "collision.bin", "traffic.bin", "grid.bin", "pace.bin",
        "track.obj"]

DEFAULT_TRACKS = "US_C3_V1,EU_C1_V1"   # with a race-line pool, and without


class Report(object):
    def __init__(self):
        self.ok = 0
        self.bad = 0

    def check(self, name, cond, detail=""):
        if cond:
            self.ok += 1
        else:
            self.bad += 1
        print("  %-58s %s   %s" % (name, "OK  " if cond else "FAIL", detail))
        return cond


def src_files():
    return (sorted(glob.glob(os.path.join(SRC, "*.c")))
            + sorted(glob.glob(os.path.join(SRC, "*.h"))))


def strip_comments(text):
    """Blank out /* */ and // runs, keeping offsets and line structure.

    The retail-string check runs on CODE only: provenance prose is the whole
    point of this port and naming a venue in a comment is not shipping a data
    table.  Keeping the length identical means a later line lookup still
    lands where it did."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":            # skip over string literals
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " "
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


# --------------------------------------------------------------- section 1
def section_source(r):
    print("\n1. SOURCE -- the baked headers are gone")
    for h, syms in sorted(PURGED.items()):
        r.check("src/%s absent" % h,
                not os.path.exists(os.path.join(SRC, h)))

    tracks, regions, chrome, hud = retail_track_strings()
    needles = set()
    if tracks:
        for cols in tracks.values():
            needles.update(c for c in cols if c and len(c) >= 5)
        needles.update(c for c in (regions or []) + (chrome or [])
                       if c and len(c) >= 5)
        # the HUD labels down to three characters: every one of them is an
        # exact-match whole literal, and none collides with a legitimate
        # spelling anywhere in src/ (the region names do -- see above)
        needles.update(c for c in (hud or []) if c and len(c) >= 3)

    inc = {}
    sym = {}
    strings = {}
    fallbacks = {}
    for path in src_files():
        text = open(path, encoding="utf-8", errors="replace").read()
        code = strip_comments(text)
        base = os.path.basename(path)
        for g in GLOBAL_FALLBACKS:
            if g in code:
                fallbacks.setdefault(g, []).append(base)
        for h, syms in PURGED.items():
            if re.search(r'#\s*include\s*"%s"' % re.escape(h), text):
                inc.setdefault(h, []).append(base)
            for s in syms:
                for m in re.finditer(re.escape(s), code):
                    line = text[text.rfind("\n", 0, m.start()) + 1:
                                text.find("\n", m.start())]
                    sym.setdefault(s, []).append("%s:%s"
                                                 % (base, line.strip()))
        for s in needles:
            # A WHOLE STRING LITERAL IN CODE -- not the words in a comment,
            # and not a prefix: "EUROPE1" is a Frontend.txd file stem, not
            # the Globalus label "EUROPE", and only the closing quote tells
            # them apart.
            if ('"%s"' % s) in code:
                strings.setdefault(s, []).append(base)

    r.check("nothing #includes a purged header", not inc,
            "; ".join("%s <- %s" % (k, v) for k, v in inc.items()))
    r.check("no purged data symbol referenced in code", not sym,
            "; ".join("%s @ %s" % (k, v[0]) for k, v in sym.items()))
    if needles:
        r.check("no retail Globalus display string literal in src/",
                not strings,
                "%d needles read from the user's own files%s"
                % (len(needles),
                   "" if not strings else "; hit in "
                   + str(sorted(set(sum(strings.values(), []))))))
    else:
        print("  -- build/burnout3.elf or build/Globalus.bin absent, "
              "string check skipped")

    # the GLOBAL track fallbacks -- "whatever was extracted last" -- are gone
    r.check("no global track/collision fallback path in code", not fallbacks,
            "; ".join("%s <- %s" % (k, v) for k, v in fallbacks.items()))

    inventory_remaining()

    # the replacements must be present and must be loaders, not tables
    for h in ("burnout3_ai_pace_runtime.h", "burnout3_trackselect_runtime.h",
              "burnout3_traffic_runtime.h",
              "burnout3_car_physics_runtime.h",
              "burnout3_vehicle_data_runtime.h",
              "burnout3_font_runtime.h",
              "burnout3_hudstr_runtime.h"):
        p = os.path.join(SRC, h)
        ok = os.path.exists(p)
        detail = ""
        if ok:
            t = open(p, encoding="utf-8", errors="replace").read()
            ok = "fopen(" in t
            detail = "%d lines" % t.count("\n")
        r.check("src/%s is a runtime loader" % h, ok, detail)


def inventory_remaining():
    """REPORT, not a gate: every OTHER Globalus string still spelled out in
    src/ code.

    The checks above are exact -- they assert that the strings the port is
    KNOWN to resolve by index (track names, venues, regions, the track-select
    chrome, the HUD labels) appear nowhere in the source.  That is a closed
    set, deliberately: a needle list of all 3985 shipped strings would flag
    every three-letter coincidence.

    But "the checks pass" is not "src/ carries no retail text", and letting
    the two be confused is exactly how a purge stops halfway.  So this prints
    what is LEFT, per file, as the worklist for the next phase.  It never
    fails: some hits genuinely are coincidences (an env-var value "OFF", the
    Frontend.txd art stem "USA"), and deciding which is a reading job, not a
    regex's."""
    if not (os.path.exists(ELF_PATH) and os.path.exists(GLOBALUS_PATH)):
        return
    with open(GLOBALUS_PATH, "rb") as f:
        gl = f.read()
    gcount = struct.unpack_from("<I", gl, 8)[0]
    table = {}
    for i in range(gcount):
        off = struct.unpack_from("<I", gl, 0x10 + i * 4)[0]
        end = off
        while end + 1 < len(gl) and gl[end:end + 2] != b"\0\0":
            end += 2
        t = gl[off:end].decode("utf-16-le", "replace")
        if len(t) >= 3:
            table.setdefault(t, i)
    per = {}
    for path in src_files():
        code = strip_comments(open(path, encoding="utf-8",
                                   errors="replace").read())
        hit = sorted(set(t for t in re.findall(r'"((?:[^"\\]|\\.)*)"', code)
                         if t in table))
        if hit:
            per[os.path.basename(path)] = hit
    total = sum(len(v) for v in per.values())
    print("  -- inventory (not a gate): %d further Globalus string(s) still "
          "spelled out in src/ code" % total)
    for k in sorted(per, key=lambda n: -len(per[n])):
        ex = ", ".join("%r@%d" % (t, table[t]) for t in per[k][:3])
        print("       %-32s %4d   e.g. %s" % (k, len(per[k]), ex))


# --------------------------------------------------------------- section 2
def complete_tracks():
    out = []
    for d in sorted(glob.glob(os.path.join(ROOT, "build", "tracks", "*"))):
        if not os.path.isdir(d):
            continue
        if all(os.path.exists(os.path.join(d, n)) for n in NEED):
            out.append(os.path.basename(d))
    return out


def section_assets(r):
    print("\n2. ASSETS -- the replacements exist on disk")
    tracks = complete_tracks()
    r.check("tracks with a complete asset set", len(tracks) >= 2,
            "%d: %s%s" % (len(tracks), ", ".join(tracks[:4]),
                          " ..." if len(tracks) > 4 else ""))
    # pace.bin is the newest of them and the one the lap count comes from
    paced = sorted(glob.glob(os.path.join(ROOT, "build", "tracks",
                                          "*", "pace.bin")))
    missing = [t for t in tracks
               if not os.path.exists(os.path.join(ROOT, "build", "tracks", t,
                                                  "pace.bin"))]
    r.check("every complete track has pace.bin", not missing,
            "%d pace.bin, missing %s" % (len(paced), missing or "none"))

    bad = []
    laps = {}
    for p in paced:
        tid = os.path.basename(os.path.dirname(p))
        ev = read_pace(p)
        if ev is None:
            bad.append(tid)
            continue
        std = [e for e in ev if e["id"] == "OFFSGRCF"]
        if not std or not (1 <= std[0]["laps"] <= 9):
            bad.append(tid)
        else:
            laps[tid] = std[0]["laps"]
    r.check("every pace.bin is a valid B3PC v1 with an OFFSGRCF lap count",
            not bad, "%d files, laps %d..%d" %
            (len(paced), min(laps.values()) if laps else 0,
             max(laps.values()) if laps else 0) if not bad else str(bad))

    # route.bin's race-line pool is what replaced B3_CENTERLINE
    withrl = 0
    for t in tracks:
        h = read_route_header(os.path.join(ROOT, "build", "tracks", t,
                                           "route.bin"))
        if h and h["center_count"] > 0:
            withrl += 1
    r.check("route.bin carries a race-line pool where the .bgd had one",
            withrl > 0, "%d/%d tracks" % (withrl, len(tracks)))
    return tracks


def read_route_header(path):
    try:
        with open(path, "rb") as f:
            d = f.read(0x28)
    except IOError:
        return None
    if len(d) < 0x28 or d[:4] != b"B3RT":
        return None
    (ver, wall, center, onc, route, start, lap, flags,
     strip) = struct.unpack_from("<IIIIIIfII", d, 4)
    return dict(version=ver, wall_count=wall, center_count=center,
                oncoming_count=onc, route_count=route, route_start=start,
                lap_length=lap, flags=flags, strip_pairs=strip)


def read_pace(path):
    """-> [{id, laps, n_opp, slots:[bytes8 x6]}] or None."""
    try:
        with open(path, "rb") as f:
            d = f.read()
    except IOError:
        return None
    if len(d) < 0x10 or d[:4] != b"B3PC":
        return None
    ver, n, slots = struct.unpack_from("<III", d, 4)
    if ver != 1 or slots != 6 or 0x10 + n * 72 > len(d):
        return None
    out = []
    for i in range(n):
        o = 0x10 + i * 72
        eid = d[o:o + 16].split(b"\0")[0].decode("ascii", "replace")
        laps, n_opp = struct.unpack_from("<II", d, o + 16)
        out.append(dict(id=eid, laps=laps, n_opp=n_opp,
                        slots=[d[o + 24 + k * 8:o + 32 + k * 8]
                               for k in range(6)]))
    return out


# ------------------------------------------- the three dump-global assets
# The phase-2 replacements.  Paths are repo-relative, exactly as the loaders
# spell them (src/burnout3_*_runtime.h).
CAR_PHYSICS_BIN = os.path.join(ROOT, "build", "cars", "car_physics.bin")
ROSTER_BIN = os.path.join(ROOT, "build", "cars", "roster.bin")
FONT_BIN = os.path.join(ROOT, "build", "frontend", "font.bin")

# the .bgv/.btv classes, in the order tools/extract_vehicles.py walks them
# (sorted(CLASS_NAMES)); the roster's ROW ORDER is part of the contract,
# because the port indexes it by position (roster_player_car's nth).
VEH_CLASSES = [("COMP", "Compact"), ("CUPE", "Coupe"), ("HEVY", "Heavy"),
               ("HSPC", "HSpec"), ("MSCL", "Muscle"), ("SPRT", "Sports"),
               ("SUPR", "Super"), ("TSPC", "TSpec")]
BGV_MAGIC = 0x17


def f32(x):
    """The f32 nearest x, as the bits -- the only honest float comparison."""
    return struct.unpack("<I", struct.pack("<f", x))[0]


def read_car_physics(path=None):
    """-> [{id, cls, file, params:[(off, bits)]}] or None.  'B3CP' v1."""
    try:
        with open(path or CAR_PHYSICS_BIN, "rb") as f:
            d = f.read()
    except IOError:
        return None
    if len(d) < 16 or d[:4] != b"B3CP":
        return None
    ver, ncar, nparam = struct.unpack_from("<III", d, 4)
    if ver != 1 or 16 + ncar * 40 + nparam * 8 > len(d):
        return None
    out, base, seen = [], 16 + ncar * 40, 0
    for i in range(ncar):
        o = 16 + i * 40
        n = struct.unpack_from("<I", d, o + 36)[0]
        if seen + n > nparam:
            return None
        out.append(dict(
            id=d[o:o + 16].split(b"\0")[0].decode("ascii", "replace"),
            cls=d[o + 16:o + 24].split(b"\0")[0].decode("ascii", "replace"),
            file=d[o + 24:o + 36].split(b"\0")[0].decode("ascii", "replace"),
            params=[(struct.unpack_from("<H", d, base + (seen + k) * 8)[0],
                     struct.unpack_from("<I", d, base + (seen + k) * 8 + 4)[0])
                    for k in range(n)]))
        seen += n
    return out


def read_roster(path=None):
    """-> (vlist_version, vlist_count, [row]) or None.  'B3VR' v1."""
    try:
        with open(path or ROSTER_BIN, "rb") as f:
            d = f.read()
    except IOError:
        return None
    if len(d) < 24 or d[:4] != b"B3VR":
        return None
    ver, n, stride, vlv, vlc = struct.unpack_from("<IIIII", d, 4)
    if ver != 1 or stride != 64 or 24 + n * 64 > len(d):
        return None
    rows = []
    for i in range(n):
        o = 24 + i * 64
        kind, size, var, sec = struct.unpack_from("<IIII", d, o + 40)
        rows.append(dict(
            file=d[o:o + 16].split(b"\0")[0].decode("ascii", "replace"),
            cls=d[o + 16:o + 24].split(b"\0")[0].decode("ascii", "replace"),
            cname=d[o + 24:o + 40].split(b"\0")[0].decode("ascii", "replace"),
            kind=kind, size=size, variant=var, sections=sec,
            dim_a=struct.unpack_from("<I", d, o + 56)[0],
            dim_b=struct.unpack_from("<I", d, o + 60)[0]))
    return vlv, vlc, rows


def read_font(path=None):
    """-> [{name, w, h, line, glyph:[(9 bits, present)]}] or None.  'B3FN'."""
    try:
        with open(path or FONT_BIN, "rb") as f:
            d = f.read()
    except IOError:
        return None
    if len(d) < 16 or d[:4] != b"B3FN":
        return None
    ver, nfont, nglyph = struct.unpack_from("<III", d, 4)
    if ver != 1 or nglyph != 95 or 16 + nfont * (44 + 95 * 40) > len(d):
        return None
    out = []
    for i in range(nfont):
        o = 16 + i * (44 + 95 * 40)
        w, h = struct.unpack_from("<II", d, o + 32)
        g = []
        for c in range(95):
            q = o + 44 + c * 40
            g.append((struct.unpack_from("<9I", d, q), d[q + 36]))
        out.append(dict(name=d[o:o + 32].split(b"\0")[0].decode("ascii",
                                                                "replace"),
                        w=w, h=h,
                        line=struct.unpack_from("<I", d, o + 40)[0], glyph=g))
    return out


def section_global_assets(r):
    """The three phase-2 assets exist, parse, and carry a plausible fleet."""
    print("\n2b. GLOBAL ASSETS -- the three phase-2 replacements")
    cp = read_car_physics()
    r.check("build/cars/car_physics.bin is a valid B3CP v1", cp is not None,
            "%d cars, %d params" % (len(cp), sum(len(c["params"]) for c in cp))
            if cp else "missing or unparseable")
    if cp:
        drivable = [c for c in cp if c["file"].endswith(".bgv")]
        traffic = [c for c in cp if c["file"].endswith(".btv")]
        r.check("drivable cars carry the 64-param set",
                bool(drivable) and all(len(c["params"]) == 64
                                       for c in drivable),
                "%d cars" % len(drivable))
        r.check("traffic cars carry the 9-param reduced set (FUN_00134AC0)",
                bool(traffic) and all(len(c["params"]) == 9 for c in traffic),
                "%d cars" % len(traffic))

    ro = read_roster()
    r.check("build/cars/roster.bin is a valid B3VR v1", ro is not None,
            "%d vehicles, vlist v%d declares %d" % (len(ro[2]), ro[0], ro[1])
            if ro else "missing or unparseable")
    if ro:
        r.check("the roster matches vlist.bin's declared count",
                ro[1] == 0 or ro[1] == len(ro[2]),
                "%d rows vs %d declared" % (len(ro[2]), ro[1]))

    fo = read_font()
    r.check("build/frontend/font.bin is a valid B3FN v1", fo is not None,
            ", ".join("%s %dx%d" % (f["name"], f["w"], f["h"]) for f in fo)
            if fo else "missing or unparseable")
    if fo:
        r.check("every font has a space glyph and a non-zero line height",
                all(f["glyph"][0][1] and f["line"] for f in fo),
                "%d fonts" % len(fo))
        # the atlas PNG the metrics index must be there too, and agree
        bad = []
        for f in fo:
            png = os.path.join(ROOT, "build", "frontend", f["name"] + ".png")
            if not os.path.exists(png):
                bad.append(f["name"] + " (no png)")
                continue
            with open(png, "rb") as fh:
                hdr = fh.read(24)
            w, h = struct.unpack_from(">II", hdr, 16)
            if (w, h) != (f["w"], f["h"]):
                bad.append("%s %dx%d vs png %dx%d" % (f["name"], f["w"],
                                                      f["h"], w, h))
        r.check("font.bin's atlas dimensions match the atlas PNGs", not bad,
                str(bad) if bad else "%d atlases" % len(fo))


# --------------------------------------------------------------- section 3
def section_parity(r):
    """pace.bin must decode to the bytes gen_ai_pace.py reads."""
    print("\n3. PARITY -- pace.bin vs tools/gen_ai_pace.py's own decode")
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import gen_ai_pace                                     # noqa: E402
    except Exception as e:                       # pragma: no cover
        print("  -- gen_ai_pace import failed (%s), skipped" % e)
        return
    root = os.environ.get("B3_GAME_DIR", gen_ai_pace.DEFAULT_ROOT)
    files = sorted(glob.glob(os.path.join(root, "Tracks", "*", "*",
                                          "Gamedata.bgd")))
    if not files:
        print("  -- no game dump under %s, skipped" % root)
        return
    checked = mism = 0
    for f in files:
        got = gen_ai_pace.read_track(f)
        if not got:
            continue
        tid, events = got
        p = os.path.join(ROOT, "build", "tracks", tid, "pace.bin")
        ours = read_pace(p)
        if ours is None:
            continue
        checked += 1
        if len(ours) != len(events):
            mism += 1
            continue
        for a, b in zip(ours, events):
            # gen_ai_pace.py's own base-40 decoder omits FUN_001AECC0's final
            # reversal, so its ids are spelled backwards; the ASSET carries
            # the forward spelling.  Compare the bytes and the reversed id.
            if a["id"] != b["id"][::-1] or a["n_opp"] != b["n_opp"] \
                    or a["slots"] != b["slots"]:
                mism += 1
                break
    r.check("pace.bin is byte-identical to the python decode", mism == 0,
            "%d tracks compared, %d mismatched" % (checked, mism))


def game_root():
    """The mounted game dump, or None."""
    root = os.environ.get("B3_GAME_DIR")
    if not root:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        sys.path.insert(0, os.path.join(ROOT, "tools", "py_extract_archive"))
        try:
            import gen_ai_pace
            root = gen_ai_pace.DEFAULT_ROOT
        except Exception:
            return None
    return root if os.path.isdir(root) else None


def car_sort_key(name):
    """tools/extract_vehicles.py's own key: EVERY digit of the stem read as
    one integer, then the stem as tie-break."""
    stem = os.path.splitext(name)[0]
    digits = "".join(c for c in stem if c.isdigit())
    return (int(digits) if digits else 0, stem)


def section_parity_roster(r):
    """roster.bin must re-derive from the USER'S OWN pveh/ headers -- every
    field, in the right row order.  This is the check that the compiled-in
    table never had: it compares the shipped asset against the game files it
    claims to describe, not against another copy of itself."""
    print("\n3b. PARITY -- roster.bin vs the user's own pveh/ headers")
    ro = read_roster()
    if ro is None:
        print("  -- build/cars/roster.bin absent, skipped")
        return
    root = game_root()
    if not root or not os.path.isdir(os.path.join(root, "pveh")):
        print("  -- no game dump with a pveh/ directory, skipped")
        return
    pveh = os.path.join(root, "pveh")

    want = []
    for code, name in VEH_CLASSES:
        cdir = os.path.join(pveh, code)
        if not os.path.isdir(cdir):
            continue
        for ext, kind in ((".bgv", 0), (".btv", 1)):
            files = sorted((os.path.basename(q) for q in
                            glob.glob(os.path.join(cdir, "*" + ext))),
                           key=car_sort_key)
            for fn in files:
                path = os.path.join(cdir, fn)
                with open(path, "rb") as fh:
                    d = fh.read(0x80)
                if len(d) < 0x80:
                    continue
                magic, _z, size, var, cnt = struct.unpack_from("<5I", d, 0)
                actual = os.path.getsize(path)
                if magic != BGV_MAGIC or size != actual:
                    continue      # read_header()'s own reject
                a, b = struct.unpack_from("<2f", d, 0x14)
                want.append(dict(file=fn, cls=code, cname=name, kind=kind,
                                 size=actual, variant=var, sections=cnt,
                                 dim_a=f32(float("%.5f" % a)),
                                 dim_b=f32(float("%.5f" % b))))

    got = ro[2]
    r.check("roster.bin has one row per accepted pveh/ file",
            len(got) == len(want), "%d rows vs %d files" % (len(got),
                                                            len(want)))
    n = min(len(got), len(want))
    bad = [i for i in range(n) if got[i] != want[i]]
    r.check("every roster.bin row re-derives from the file's own header",
            not bad and len(got) == len(want),
            "%d rows compared, %d differ%s" % (n, len(bad),
             "" if not bad else " (first: %r vs %r)" % (got[bad[0]],
                                                        want[bad[0]])))

    cp = read_car_physics()
    if cp is not None:
        key = set((v["cls"], v["file"]) for v in got)
        orphan = [c["id"] for c in cp if (c["cls"], c["file"]) not in key]
        r.check("every car_physics.bin row keys onto a roster vehicle",
                not orphan, "%d cars, %d orphaned%s"
                % (len(cp), len(orphan),
                   "" if not orphan else ": " + str(orphan[:5])))


# The three fonts, by VA in build/burnout3.elf -- the same constants
# tools/extract_font.py and tools/cextract/cx_art_font.c carry.  These are
# ADDRESSES IN RECOVERED CODE, not game data.                          [C]
FONT_VA = [("GlobalFont", 0x3C84D8, 0x3C9C38, 0x3E7B98),
           ("HeadFont", 0x3D9CB8, 0x3DA338, 0x3E7BB0),
           ("SmallFont", 0x3E23B8, 0x3E3B18, 0x3E7BA4)]


def section_parity_font(r):
    """font.bin must re-derive from the USER'S OWN XBE image -- FUN_001C1060's
    charmap walk, done here independently of the extractor."""
    print("\n3c. PARITY -- font.bin vs the user's own XBE image")
    fo = read_font()
    if fo is None:
        print("  -- build/frontend/font.bin absent, skipped")
        return
    if not os.path.exists(ELF_PATH):
        print("  -- build/burnout3.elf absent, skipped")
        return
    raw, segs = elf_segments(ELF_PATH)
    if not segs:
        print("  -- build/burnout3.elf is not an ELF, skipped")
        return

    def rd(va, n):
        for base, off, fsz in segs:
            if base <= va < base + fsz and va - base + n <= fsz:
                return raw[off + va - base:off + va - base + n]
        return None

    bad, checked = [], 0
    for i, (name, obj, rec, slot) in enumerate(FONT_VA):
        if i >= len(fo):
            bad.append("%s missing from the asset" % name)
            continue
        f = fo[i]
        if f["name"] != name:
            bad.append("font %d is %r, expected %r" % (i, f["name"], name))
            continue
        if struct.unpack_from("<I", rd(slot, 4), 0)[0] != obj:
            bad.append("%s: pointer slot does not reach the object" % name)
            continue
        tw = struct.unpack_from("<I", rd(rec + 0x38, 4), 0)[0]
        th = struct.unpack_from("<I", rd(rec + 0x3C, 4), 0)[0]
        if (tw, th) != (f["w"], f["h"]):
            bad.append("%s: %dx%d vs asset %dx%d" % (name, tw, th, f["w"],
                                                     f["h"]))
            continue
        default_off = struct.unpack_from("<I", rd(obj + 0x1C, 4), 0)[0]
        cmap = struct.unpack_from("<128I", rd(obj + 0x20, 512), 0)
        line = None
        for c in range(0x20, 0x7F):
            o, guard = cmap[c], 0
            while o != default_off:
                cc = rd(obj + o + 28, 4)
                if cc is None or struct.unpack_from("<I", cc, 0)[0] == c:
                    break
                o += 0x20
                guard += 1
                if guard > 4096:
                    break
            g = f["glyph"][c - 0x20]
            if o == default_off or guard > 4096:
                if g[1] or any(g[0]):
                    bad.append("%s: '%c' present in the asset, absent in the "
                               "image" % (name, c))
                continue
            if not g[1]:
                bad.append("%s: '%c' absent in the asset" % (name, c))
                continue
            u, v, gw, gh, xo, yo, adv = struct.unpack_from(
                "<7f", rd(obj + o, 28), 0)
            if u < 0.0 or gh == 0.0:
                u = v = gw = gh = 0.0
            # the extractor's own quantisation: the header printed the UVs
            # "%.6f" and the pixel metrics "%.1f", so the asset carries the
            # rounded decimals and this must round the same way
            wnt = (f32(float("%.6f" % u)), f32(float("%.6f" % v)),
                   f32(float("%.6f" % (u + gw))), f32(float("%.6f" % (v + gh))),
                   f32(float("%.1f" % (gw * tw))), f32(float("%.1f" % (gh * th))),
                   f32(float("%.1f" % (xo * tw))), f32(float("%.1f" % (yo * th))),
                   f32(float("%.1f" % (adv * tw))))
            checked += 1
            if tuple(g[0]) != wnt:
                bad.append("%s: '%c' metrics differ" % (name, c))
            h = yo * th + gh * th
            line = h if line is None or h > line else line
        if line is not None and f["line"] != f32(float("%.3f" % line)):
            bad.append("%s: line height differs" % name)
    r.check("font.bin re-derives from the XBE's own glyph records", not bad,
            "%d glyphs compared%s" % (checked,
             "" if not bad else "; " + "; ".join(bad[:3])))


# --------------------------------------------------------------- section 4
def boot(track, seconds, extra_env=None):
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        # THE PHOTOREALISM WAVE IS PINNED OFF HERE.
        # src/burnout3_aftereffects.h ships six INSPIRED screen-space
        # effects on by default.  This suite verifies RECOVERED pixel
        # behaviour, so it owns its conditions and turns them off -- the
        # same move, and the same reasoning, as the B3_MUSIC_SEED /
        # B3_TRACK_NOSHINE pins.  It is sound rather than a dodge because
        # tools/validate_photo.py section 2 PROVES B3_PHOTO=0 renders
        # bit-identically to the pre-wave build; if that leg ever fails,
        # this pin stops being valid and this suite stops measuring what
        # it says it measures.
        "B3_PHOTO": "0",
        "SDL_AUDIODRIVER": "dummy",
        # This suite hides assets under build/ to test the loud-FATAL path;
        # the game's iso-default would silently re-materialise them instead.
        "B3_DATA_MODE": "build",
        "B3_AUTODRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(seconds),
        "B3_TRACK": track,
        "B3_TRACK_TEST": "1",
    })
    if extra_env:
        env.update(extra_env)
    p = subprocess.run(["timeout", str(max(180, seconds * 10)), "./burnout3"],
                       cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def section_boot(r, tracks, seconds):
    print("\n4. BOOT -- every replacement names the FILE it loaded from")
    for track in tracks:
        print("  -- %s" % track)
        rc, log = boot(track, seconds)
        rh = read_route_header(os.path.join(ROOT, "build", "tracks", track,
                                            "route.bin")) or {}

        # start grid: build/tracks/<t>/grid.bin  (was B3_START_GRID)
        m = re.search(r'\[track\] start grid: (\d+) slots from '
                      r'build/tracks/%s/grid\.bin' % re.escape(track), log)
        r.check("%s  start grid from grid.bin" % track, bool(m),
                m.group(0) if m else "no [track] start grid line")

        # route: the retail nav ribbon inside route.bin, or its boundary strip
        m = re.search(r'\[track\] route: retail nav section \d+, (\d+) nodes',
                      log)
        m2 = re.search(r'\[track\] route/wall strands: (\d+) stations from '
                       r'build/tracks/%s/route\.bin' % re.escape(track), log)
        r.check("%s  route from route.bin" % track, bool(m or m2),
                (m or m2).group(0) if (m or m2) else "no route line")
        r.check("%s  no compiled-in route fallback" % track,
                "compiled-in fallback" not in log,
                "" if "compiled-in fallback" not in log
                else "the purged header's fallback fired")

        # race line: route.bin's centerline pool (was B3_CENTERLINE)
        m = re.search(r'\[track\] race line: (\d+) points from '
                      r'build/tracks/%s/route\.bin' % re.escape(track), log)
        want = rh.get("center_count", 0) > 0
        r.check("%s  race line %s" % (track,
                                      "from route.bin" if want
                                      else "absent, as the file says"),
                bool(m) == want,
                (m.group(0) if m else "route.bin center_count=%d"
                 % rh.get("center_count", -1)))
        m = re.search(r'\[tracktest\] raceline points=(\d+) '
                      r'aim_recoveries=(\d+)', log)
        r.check("%s  aim recovery measured" % track, bool(m),
                m.group(0) if m else "no [tracktest] raceline line")
        if m:
            r.check("%s  race-line point count matches the file" % track,
                    int(m.group(1)) == rh.get("center_count", -1),
                    "%s vs route.bin %d" % (m.group(1),
                                            rh.get("center_count", -1)))

        # AI pace: build/tracks/<t>/pace.bin  (was B3_AI_PACE)
        m = re.search(r'\[Burnout3\] ai pace: build/tracks/%s/pace\.bin -- '
                      r'(\d+) events, using (\w+) \((\d+) opponents, '
                      r'(\d+) laps\)' % re.escape(track), log)
        r.check("%s  AI pace records from pace.bin" % track, bool(m),
                m.group(0)[11:] if m else "no [Burnout3] ai pace line")

        # traffic: build/tracks/<t>/traffic.bin (was burnout3_traffic_data.h)
        m = re.search(r'\[Burnout3\] traffic data: %s -- (\d+) cars'
                      % re.escape(track), log)
        r.check("%s  traffic set from traffic.bin" % track, bool(m),
                m.group(0)[11:] if m else "no [Burnout3] traffic data line")

        # ---- phase 2 -------------------------------------------------
        # the render mesh and the collision world, PER TRACK.  The old
        # global fallbacks were whichever track was extracted last, so the
        # point of these two is that the path names THIS track.
        m = re.search(r'REAL track geometry: \d+ verts, \d+ tris from '
                      r'build/tracks/%s/track\.obj' % re.escape(track), log)
        r.check("%s  render mesh from the track's own track.obj" % track,
                bool(m), m.group(0) if m else "no per-track REAL track line")
        m = re.search(r'GAME collision world: (\d+) triangles '
                      r'\(build/tracks/%s/collision\.bin\)'
                      % re.escape(track), log)
        r.check("%s  collision from the track's own collision.bin" % track,
                bool(m), m.group(0) if m else "no per-track collision line")

        # the roster (was burnout3_vehicle_data.h)
        m = re.search(r'\[Burnout3\] vehicle roster: (\d+) vehicles from '
                      r'(\S*build/cars/roster\.bin)', log)
        r.check("%s  vehicle roster from roster.bin" % track, bool(m),
                m.group(0)[11:] if m else "no [Burnout3] vehicle roster line")

        # the per-car VDB tuning (was burnout3_car_physics.h)
        m = re.search(r'\[Burnout3\] car physics: (\d+) cars, (\d+) params '
                      r'from (\S*build/cars/car_physics\.bin)', log)
        r.check("%s  per-car tuning from car_physics.bin" % track, bool(m),
                m.group(0)[11:] if m else "no [Burnout3] car physics line")

        # the glyph metrics (was burnout3_font.h)
        m = re.search(r'\[Burnout3\] font metrics: (\d+) fonts from '
                      r'(\S*build/frontend/font\.bin)', log)
        r.check("%s  glyph metrics from font.bin" % track, bool(m),
                m.group(0)[11:] if m else "no [Burnout3] font metrics line")

        # the HUD's own labels (were English literals in burnout3_hud.c)
        m = re.search(r'\[Burnout3\] hud labels: (\d+) strings from '
                      r'(\S*Globalus\.bin)', log)
        r.check("%s  HUD labels from the user's Globalus.bin" % track,
                bool(m), m.group(0)[11:] if m else "no [Burnout3] hud labels "
                                                   "line")

        r.check("%s  clean exit" % track, rc == 0, "rc=%d" % rc)


# --------------------------------------------------------------- section 5
def section_loud(r, track, seconds):
    """A missing asset must NAME THE FILE and stop -- not fall back to
    another track's data.  Point B3_TRACK at a track id that has no build
    directory at all: every loader below must refuse rather than substitute."""
    print("\n5. LOUD -- a missing asset names the file instead of substituting")
    ghost = "ZZ_C9_V9"
    if os.path.isdir(os.path.join(ROOT, "build", "tracks", ghost)):
        print("  -- %s exists, skipped" % ghost)
        return
    rc, log = boot(ghost, seconds)
    r.check("refuses to run without the track's assets", rc != 0,
            "rc=%d" % rc)
    fatal = [ln for ln in log.splitlines() if "FATAL" in ln]
    r.check("says FATAL rather than carrying on", bool(fatal),
            fatal[0][:70] if fatal else "no FATAL line")
    # WHICH per-track file it stops on depends on load order -- since the two
    # global fallbacks went, track.obj is the first thing an unextracted track
    # is missing, and before that it was grid.bin or route.bin.  Any of the
    # four is a pass; naming NOTHING is the failure.
    named = [n for n in ("track.obj", "collision.bin", "grid.bin", "route.bin")
             if ("build/tracks/%s/%s" % (ghost, n)) in log]
    r.check("names the missing file, by path", bool(named),
            "build/tracks/%s/%s named" % (ghost, ", ".join(named))
            if named else "no path named")
    r.check("tells the user how to extract it",
            "cxtract" in log and "--track %s" % ghost in log,
            "extraction command present" if "cxtract" in log
            else "no extraction hint")

    # ---- phase 2: the three dump-global assets -----------------------
    # Each loader is pointed at a path that does not exist, one at a time, on
    # a track that IS fully extracted.  A silent fallback would boot; the
    # requirement is that it names the file and stops.
    for label, var, want in (
            ("car_physics.bin", "B3_CAR_PHYSICS_BIN",
             "build/cars/car_physics.bin"),
            ("roster.bin", "B3_ROSTER_BIN", "build/cars/roster.bin"),
            ("font.bin", "B3_FONT_BIN", "build/frontend/font.bin")):
        ghost_path = os.path.join(ROOT, "build", "__no_such_%s" % label)
        if os.path.exists(ghost_path):
            continue
        rc, log = boot(track, seconds, {var: ghost_path})
        r.check("a missing %s stops the port" % label, rc != 0, "rc=%d" % rc)
        r.check("...naming the file and the cxtract command",
                ghost_path in log and "FATAL" in log and "cxtract" in log,
                "named" if ghost_path in log else "path not named")
        r.check("...and saying outright there is no compiled-in copy",
                "no compiled-in" in log,
                "%s: the message names the absence" % want)


# --------------------------------------------------------------- section 6
def section_menu(r, track, seconds):
    """The TRACK SELECT screen, end to end: it must print the retail venue and
    name for the chosen track, and those must be the strings sitting in the
    USER'S OWN Globalus.bin at the index the USER'S OWN XBE names -- which is
    what this validator reads independently, above."""
    print("\n6. TRACK SELECT -- the display strings come off the user's files")
    tracks, regions, chrome, _hud = retail_track_strings()
    if not tracks:
        print("  -- build/burnout3.elf or build/Globalus.bin absent, skipped")
        return
    r.check("XBE id table decodes to a plausible track list",
            len(tracks) >= 20 and track in tracks,
            "%d tracks, %s %s" % (len(tracks), track,
                                  "present" if track in tracks else "MISSING"))
    r.check("region labels resolve out of Globalus.bin",
            all(regions) and len(set(regions)) == 3,
            "/".join(x or "?" for x in regions))

    env = {"B3_MENU": "1", "B3_MENU_AUTOSELECT": track,
           "B3_MENU_SHOT_LEVEL": "2"}
    e = dict(os.environ)
    e.update({
        "SDL_VIDEODRIVER": "offscreen", "SDL_AUDIODRIVER": "dummy",
        # THE PHOTOREALISM WAVE IS PINNED OFF HERE.
        # src/burnout3_aftereffects.h ships six INSPIRED screen-space
        # effects on by default.  This suite verifies RECOVERED pixel
        # behaviour, so it owns its conditions and turns them off -- the
        # same move, and the same reasoning, as the B3_MUSIC_SEED /
        # B3_TRACK_NOSHINE pins.  It is sound rather than a dodge because
        # tools/validate_photo.py section 2 PROVES B3_PHOTO=0 renders
        # bit-identically to the pre-wave build; if that leg ever fails,
        # this pin stops being valid and this suite stops measuring what
        # it says it measures.
        "B3_PHOTO": "0",
        "B3_DATA_MODE": "build",   # see boot(): hidden-asset legs need build mode
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(seconds), "B3_TRACK_TEST": "1",
    })
    e.update(env)
    e.pop("B3_TRACK", None)          # the MENU must choose it
    e.pop("B3_AUTODRIVE", None)
    p = subprocess.run(["timeout", str(max(180, seconds * 10)), "./burnout3"],
                       cwd=ROOT, env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = p.stdout.decode("utf-8", "replace")

    m = re.search(r'\[Burnout3\] track select data: (\d+) tracks from '
                  r'(\S+) \+ (\S+) \((\d+) strings\)', log)
    r.check("table loaded from the ELF + Globalus.bin", bool(m),
            m.group(0)[11:] if m else "no [Burnout3] track select data line")
    if m:
        r.check("track count matches the XBE id table",
                int(m.group(1)) == len(tracks),
                "%s vs %d" % (m.group(1), len(tracks)))
        r.check("it named the two user files", m.group(2).endswith(".elf")
                and m.group(3).endswith("Globalus.bin"),
                "%s + %s" % (m.group(2), m.group(3)))

    venue, name_uc, _tc = tracks[track]
    laps = 0
    ev = read_pace(os.path.join(ROOT, "build", "tracks", track, "pace.bin"))
    for x in (ev or []):
        if x["id"] == "OFFSGRCF":
            laps = x["laps"]
    m = re.search(r'\[Burnout3\] track select: ([^/]+) / ([^/]+) / (.+?) '
                  r'-> B3_TRACK=(\w+), (\d+) laps', log)
    r.check("selector announced its choice", bool(m),
            m.group(0)[11:] if m else "no [Burnout3] track select line")
    if m:
        got = (m.group(1).strip(), m.group(2).strip(), m.group(3).strip(),
               m.group(4), int(m.group(5)))
        r.check("venue string == the user's Globalus entry",
                got[1] == venue, "%r vs %r" % (got[1], venue))
        r.check("track name string == the user's Globalus entry",
                got[2] == name_uc, "%r vs %r" % (got[2], name_uc))
        r.check("region label == the user's Globalus entry",
                got[0] in (regions or []), "%r in %s" % (got[0], regions))
        r.check("lap count == the .bgd OFFSGRCF param+0x3B8 in pace.bin",
                got[4] == laps and laps > 0, "%d vs %d" % (got[4], laps))
        r.check("B3_TRACK set to the chosen id", got[3] == track,
                "%s vs %s" % (got[3], track))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tracks", default=DEFAULT_TRACKS)
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--skip-boot", action="store_true")
    a = ap.parse_args()

    print("=" * 72)
    print("NO BAKED GAME DATA -- src/ carries code, build/ carries the game")
    print("=" * 72)

    r = Report()
    section_source(r)
    have = section_assets(r)
    section_global_assets(r)
    section_parity(r)
    section_parity_roster(r)
    section_parity_font(r)

    if a.skip_boot:
        print("\n4-5. BOOT -- skipped (--skip-boot)")
    elif not os.path.exists(os.path.join(ROOT, "burnout3")):
        print("\n4-5. BOOT -- ./burnout3 not built, skipped")
    else:
        want = [t for t in a.tracks.split(",") if t]
        miss = [t for t in want if t not in have]
        if miss:
            print("\n4-5. BOOT -- %s not extracted, skipped" % miss)
        else:
            section_boot(r, want, a.seconds)
            section_loud(r, want[0], a.seconds)
            section_menu(r, want[0], a.seconds)

    print("\n" + "=" * 72)
    print("%d passed, %d failed" % (r.ok, r.bad))
    print("=" * 72)
    return 1 if r.bad else 0


if __name__ == "__main__":
    sys.exit(main())
