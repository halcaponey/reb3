#!/usr/bin/env python3
'''Traffic alignment + render/collision parity check, for EVERY shipped track.

The defect this exists to catch: the traffic system consuming COMPILED-IN
per-track data.  src/burnout3_traffic_data.h and src/burnout3_track_paths.h are
regenerated for whatever `--track` last ran and the repo pins them to
US_C3_V1, so on the other 34 tracks the traffic manager was driving another
world's model list, oncoming line, spawn seeds and lane cross-section.
Measured before the fix: 246 of 400 spawn-policy model entries resolved to
"no such car", and the pinned oncoming polyline sat a median 1.8-8.3 km from
the road the player was actually on.

Everything here is DATA-DRIVEN and track-agnostic: the tracks come from
build/tracks/*/, every threshold is a single global constant derived from the
measured spread across all tracks (see THRESHOLDS), and no per-track value
appears anywhere in this file.

Checks (static, no game run needed -- run these any time):
  caps      every build/tracks/<id>/traffic.bin is a B3TR v4 asset that fits
            the runtime loader's caps in src/burnout3_traffic_runtime.h
  models    every spawn-policy model entry in a track's traffic_paths.bin
            resolves to a car in the SAME track's traffic.bin  (must be 100%)
  geometry  a track's own traffic geometry (oncoming line, spawn seeds) lies
            on that track's own road ribbon
  meshes    every car the solver can be handed has a drawable mesh
            (collidable => drawable)
  source    src/burnout3_full.c consumes the RUNTIME traffic data, and keeps
            the render transform tied to the collision transform

Check (runtime, needs a free game lock):
  --run     boot each track headless and assert every ACTIVE traffic agent is
            on that track's road ribbon and wearing one of its own models
  flow      (part of --run) the traffic FLOWS BOTH WAYS: every agent travels
            FORWARD along its own descriptor, and the share of it running
            AGAINST the player matches what the track's own directed path
            network + spawn policy predict for the road the player drove

Usage:
    python3 tools/validate_traffic_align.py                 # static, all tracks
    python3 tools/validate_traffic_align.py --tracks EU_C1_V1,AS_M1_V1
    python3 tools/validate_traffic_align.py --run --seconds 20
'''

import argparse
import math
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKS_DIR = os.path.join(ROOT, "build", "tracks")
CARS_DIR = os.path.join(ROOT, "build", "cars")
# B3_FULL_C points the SOURCE checks at a shadow tree, the way B3_BIN points
# the run at a shadow build: the traffic parity work is delivered as patches,
# so a fix has to be measurable before it lands.
FULL_C = os.environ.get("B3_FULL_C") or os.path.join(ROOT, "src",
                                                     "burnout3_full.c")
RUNTIME_H = os.path.join(ROOT, "src", "burnout3_traffic_runtime.h")

# ---------------------------------------------------------------- THRESHOLDS
# Derived from the spread measured over all 36 shipped tracks with each
# track's OWN data, then given a wide margin.  They are format-level sanity
# bounds, not track knowledge.
#
#   oncoming line -> own road ribbon : worst legitimate median  53 m, p95 202 m
#   spawn seeds   -> own road ribbon : worst legitimate median   3.8 m, max 71 m
#
# The failure mode they must catch (another track's data) measured a median of
# 1842-8287 m, so anything in the hundreds separates the two cleanly.  The
# oncoming line is a separate drive line that legitimately threads roads the
# pool's path set does not cover, hence the looser bound.
MAX_ONCOMING_MEDIAN_M = 250.0
MAX_SPAWN_MEDIAN_M = 150.0

# Mirrors of the runtime loader's caps (src/burnout3_traffic_runtime.h).
CAR_MAX, SPAWN_MAX, LANE_MAX = 32, 512, 32

CAT_NAME = {0: "compact", 1: "light", 2: "bus", 3: "truck",
            4: "TRACTOR", 5: "TRAILER", 6: "special"}
CAT_SPECIAL = 6


# ------------------------------------------------------------------ readers
def _cstr(b):
    return b.split(b"\0")[0].decode("ascii", "replace")


def read_traffic_bin(path):
    '''build/tracks/<id>/traffic.bin -- 'B3TR' v4 (tools/extract_traffic.py).'''
    d = open(path, "rb").read()
    if len(d) < 0x1C or d[:4] != b"B3TR":
        raise ValueError("not a B3TR asset")
    ver, ncar, nspawn, nonc, nspec, nlane = struct.unpack_from("<IIIIII", d, 4)
    if ver != 4:
        raise ValueError("B3TR version %d, expected 4" % ver)
    o = 0x1C
    cars = []
    for _ in range(ncar):
        cid, cls, car = _cstr(d[o:o + 16]), _cstr(d[o + 16:o + 24]), _cstr(d[o + 24:o + 40])
        cat, kingpin = struct.unpack_from("<ii", d, o + 40)
        cars.append(dict(id=cid, cls=cls, car=car, cat=cat, kingpin=kingpin))
        o += 72
    spawns = [struct.unpack_from("<6f", d, o + i * 24) for i in range(nspawn)]
    o += nspawn * 24
    onc = [struct.unpack_from("<3f", d, o + i * 12) for i in range(nonc)]
    o += nonc * 12
    lanes = [struct.unpack_from("<fi", d, o + i * 8) for i in range(nlane)]
    o += nlane * 8
    if o != len(d):
        raise ValueError("trailing bytes: parsed %d of %d" % (o, len(d)))
    return dict(version=ver, cars=cars, spawns=spawns, oncoming=onc,
                lanes=lanes, specials=nspec)


def read_traffic_paths_bin(path):
    '''build/tracks/<id>/traffic_paths.bin -- 'B3TP'; road points + spawn policy.'''
    d = open(path, "rb").read()
    if len(d) < 0x18 or d[:4] != b"B3TP":
        raise ValueError("not a B3TP asset")
    ver, npt, npath, nwin, nreq = struct.unpack_from("<IIIII", d, 4)
    o = 0x18
    points = [struct.unpack_from("<3f", d, o + i * 12) for i in range(npt)]
    o += npt * 12
    for _ in range(npath):
        n = struct.unpack_from("<I", d, o)[0]
        o += 4 + n * 4 + n * 8 + n * 0x12
    o += nwin * 16 + nreq * 6
    entries = []
    if ver >= 4:
        ncl, nen, nbd, nrd = struct.unpack_from("<IIII", d, o)
        o += 16 + ncl * 16
        for _ in range(nen):
            entries.append(_cstr(d[o:o + 16]))
            o += 32
    return dict(version=ver, points=points, paths=npath, entries=entries)


def read_traffic_paths_full(path):
    '''The whole B3TP asset: the shared point pool, every path's pair rows and
    cumulative distances, the pool windows and requests, and the v4 spawn
    policy.  read_traffic_paths_bin() above skips all of it -- the flow leg
    needs the geometry AND the policy.'''
    d = open(path, "rb").read()
    if len(d) < 0x18 or d[:4] != b"B3TP":
        raise ValueError("not a B3TP asset")
    ver, npt, npath = struct.unpack_from("<III", d, 4)
    o = 0x10
    nwin = nreq = 0
    if ver >= 3:
        nwin, nreq = struct.unpack_from("<II", d, o)
        o += 8
    points = [struct.unpack_from("<3f", d, o + i * 12) for i in range(npt)]
    o += npt * 12
    paths = []
    for _ in range(npath):
        n = struct.unpack_from("<I", d, o)[0]
        o += 4
        pairs = [struct.unpack_from("<HH", d, o + i * 4) for i in range(n)]
        o += n * 4 + n * 8                       # pairs, then {distance,width}
        if ver >= 2:
            o += n * 0x12                        # FUN_001A0750's branch rows
        paths.append(pairs)
    windows, requests = [], []
    for i in range(nwin):
        fp, lp, rb, rc, rf, _pad = struct.unpack_from("<IIIBBH", d, o)
        o += 16
        windows.append(dict(first=fp, last=lp, base=rb, count=rc, refresh=rf))
    for i in range(nreq):
        fr, lr, pid, dr = struct.unpack_from("<HHBB", d, o)
        o += 6
        requests.append(dict(first_row=fr, last_row=lr, path_id=pid,
                             direction=dr))
    mix = None
    if ver >= 4:
        ncl, nen, nbd, nrd = struct.unpack_from("<IIII", d, o)
        o += 16 + ncl * 16 + nen * 32
        bindings = []
        for i in range(nbd):
            pid, rec, slot, _pad, start = struct.unpack_from("<BBBBI", d, o)
            o += 8
            bindings.append((pid, rec, slot, start))
        roads = []
        for i in range(nrd):
            rec, slot, _pad, mph = struct.unpack_from("<BBHf", d, o)
            rate = struct.unpack_from("<6f", d, o + 8)
            o += 32
            roads.append(dict(record=rec, slot=slot, mph=mph, rate=rate))
        mix = dict(bindings=bindings, roads=roads)
    return dict(version=ver, points=points, paths=paths, windows=windows,
                requests=requests, mix=mix)


# ------------------------------------------------------------------ geometry
def _nearest_xz(points, q):
    return math.sqrt(min((q[0] - p[0]) ** 2 + (q[2] - p[2]) ** 2
                         for p in points))


def _median(values):
    if not values:
        return 0.0
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def distances_to_ribbon(points, ribbon, stride=1):
    '''Distance from each sampled point to the nearest road-ribbon point.'''
    if not points or not ribbon:
        return []
    return [_nearest_xz(ribbon, q) for q in points[::max(1, stride)]]


# -------------------------------------------------------------------- checks
class Result:
    def __init__(self):
        self.failures = []
        self.notes = []

    def fail(self, track, check, msg):
        self.failures.append((track, check, msg))

    def note(self, msg):
        self.notes.append(msg)


def is_extracted(track):
    '''extract_all_tracks.sh's own completeness test: a playable track has a
    route.  Stub directories (an envmap and nothing else) are not tracks.'''
    return os.path.exists(os.path.join(TRACKS_DIR, track, "route.bin"))


def check_track(track, res, verbose):
    tdir = os.path.join(TRACKS_DIR, track)
    tb_path = os.path.join(tdir, "traffic.bin")
    tp_path = os.path.join(tdir, "traffic_paths.bin")
    if not os.path.exists(tb_path):
        res.fail(track, "caps", "missing traffic.bin -- extract_all_tracks.sh "
                                "has not run for this track")
        return
    if not os.path.exists(tp_path):
        res.fail(track, "caps", "missing traffic_paths.bin")
        return

    try:
        tb = read_traffic_bin(tb_path)
    except Exception as exc:
        res.fail(track, "caps", "traffic.bin unreadable: %s" % exc)
        return
    try:
        tp = read_traffic_paths_bin(tp_path)
    except Exception as exc:
        res.fail(track, "caps", "traffic_paths.bin unreadable: %s" % exc)
        return

    # --- caps: the runtime loader must be able to hold this asset ----------
    if not tb["cars"]:
        res.fail(track, "caps", "traffic.bin declares zero cars")
    if len(tb["cars"]) > CAR_MAX:
        res.fail(track, "caps", "%d cars exceeds B3_TRAFFIC_CAR_MAX %d"
                 % (len(tb["cars"]), CAR_MAX))
    if len(tb["spawns"]) > SPAWN_MAX:
        res.fail(track, "caps", "%d spawn seeds exceeds B3_TRAFFIC_SPAWN_MAX %d"
                 % (len(tb["spawns"]), SPAWN_MAX))
    if len(tb["lanes"]) > LANE_MAX:
        res.fail(track, "caps", "%d lanes exceeds B3_TRAFFIC_LANE_MAX %d"
                 % (len(tb["lanes"]), LANE_MAX))

    # --- models: the spawn policy must name cars THIS track ships ----------
    # This is the direct regression test for the compiled-in car table: with
    # US_C3_V1's 11 models pinned in, 246 of 400 entries across the 36 tracks
    # resolved to nothing and the pool agent silently kept the previous
    # occupant's model -- which is how a semi-trailer appeared on tracks whose
    # own traffic set contains no tractor at all.
    own = {c["id"] for c in tb["cars"]}
    unresolved = sorted({e for e in tp["entries"] if e not in own})
    if unresolved:
        res.fail(track, "models",
                 "%d/%d spawn-policy entries name a car this track does not "
                 "ship: %s" % (len(unresolved), len(tp["entries"]),
                               ", ".join(unresolved[:8])))
    elif verbose:
        res.note("%-11s models   %d/%d entries resolve"
                 % (track, len(tp["entries"]), len(tp["entries"])))

    # --- geometry: this track's traffic geometry is on this track's roads --
    ribbon = tp["points"]
    if not ribbon:
        res.fail(track, "geometry", "traffic_paths.bin has no road points")
        return

    onc = tb["oncoming"]
    if onc:
        d = distances_to_ribbon(onc, ribbon, stride=max(1, len(onc) // 400))
        med = _median(d)
        if med > MAX_ONCOMING_MEDIAN_M:
            res.fail(track, "geometry",
                     "oncoming line sits a median %.0f m from this track's own "
                     "roads (limit %.0f) -- this is another track's data"
                     % (med, MAX_ONCOMING_MEDIAN_M))
        elif verbose:
            res.note("%-11s geometry oncoming median %.1f m (%d pts)"
                     % (track, med, len(onc)))
    elif verbose:
        res.note("%-11s geometry no oncoming line shipped (legal)" % track)

    spawns = tb["spawns"]
    if spawns:
        d = distances_to_ribbon(spawns, ribbon)
        med = _median(d)
        if med > MAX_SPAWN_MEDIAN_M:
            res.fail(track, "geometry",
                     "spawn seeds sit a median %.0f m from this track's own "
                     "roads (limit %.0f) -- this is another track's data"
                     % (med, MAX_SPAWN_MEDIAN_M))
        elif verbose:
            res.note("%-11s geometry spawn median %.1f m (%d seeds)"
                     % (track, med, len(spawns)))

    # --- meshes: anything the solver can be handed must be drawable --------
    # carcol_pass() admits a traffic body on its hull alone; traffic_render()
    # and traffic_draw_at() need a display list built from the same .obj.  A
    # car with no mesh is a body the player can hit and never see.
    for c in tb["cars"]:
        if c["cat"] == CAT_SPECIAL:
            continue        # specials are excluded from the driving pool
        obj = os.path.join(CARS_DIR, "%s_%s.obj" % (c["cls"], c["car"]))
        if not os.path.exists(obj):
            res.fail(track, "meshes",
                     "%s (%s) has no mesh at build/cars/%s_%s.obj -- a "
                     "collidable body with nothing to draw"
                     % (c["id"], CAT_NAME.get(c["cat"], c["cat"]),
                        c["cls"], c["car"]))


PINNED_HEADER = os.path.join(ROOT, "src", "burnout3_traffic_data.h")


def compiled_in_car_ids():
    """The car ids baked into src/burnout3_traffic_data.h, if it exists."""
    if not os.path.exists(PINNED_HEADER):
        return None
    text = open(PINNED_HEADER, encoding="utf-8", errors="replace").read()
    body = re.search(r"B3_TRAFFIC_CARS\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                     text, re.S)
    if not body:
        return None
    return [m.group(1) for m in re.finditer(r'\{\s*"([A-Z0-9]+)"\s*,',
                                            body.group(1))]


def check_pinned(tracks, res, verbose):
    """Reproduce, statically, what the RUNTIME resolves today.

    While src/burnout3_full.c consumes the generated header, every track's
    per-track spawn policy is string-matched against ONE track's car list.
    This measures the damage directly, so the fix is a number, not a claim.
    """
    src = open(FULL_C, encoding="utf-8", errors="replace").read()
    if not re.search(r'^\s*#include\s+"burnout3_traffic_data\.h"', src, re.M):
        if verbose:
            res.note("pinned   runtime reads per-track traffic.bin (not the "
                     "compiled-in table)")
        return
    compiled = compiled_in_car_ids()
    if compiled is None:
        res.fail("-", "pinned", "cannot parse B3_TRAFFIC_CARS from %s"
                 % PINNED_HEADER)
        return
    total = unresolved = 0
    worst = []
    for track in tracks:
        tp = os.path.join(TRACKS_DIR, track, "traffic_paths.bin")
        if not os.path.exists(tp):
            continue
        try:
            entries = read_traffic_paths_bin(tp)["entries"]
        except Exception:
            continue
        bad = [e for e in entries if e not in compiled]
        total += len(entries)
        unresolved += len(bad)
        if bad:
            worst.append((len(bad), len(entries), track))
    if unresolved:
        worst.sort(reverse=True)
        res.fail("-", "pinned",
                 "%d/%d spawn-policy model entries across %d track(s) do not "
                 "exist in the compiled-in car table -- those pool agents keep "
                 "the previous occupant's model (worst: %s)"
                 % (unresolved, total, len(worst),
                    ", ".join("%s %d/%d" % (t, b, n) for b, n, t in worst[:4])))


def check_source(res, verbose):
    '''Guards that keep the fix from silently regressing.'''
    if not os.path.exists(FULL_C):
        res.fail("-", "source", "src/burnout3_full.c not found")
        return
    src = open(FULL_C, encoding="utf-8", errors="replace").read()

    # 1. the traffic tables must come from the runtime asset, not the pinned
    #    generated header
    if not os.path.exists(RUNTIME_H):
        res.fail("-", "source", "src/burnout3_traffic_runtime.h missing")
    if re.search(r'^\s*#include\s+"burnout3_traffic_data\.h"', src, re.M):
        res.fail("-", "source",
                 'src/burnout3_full.c still includes "burnout3_traffic_data.h" '
                 '-- the traffic tables are pinned to whichever track the '
                 'extractor last ran for')
    if not re.search(r'^\s*#include\s+"burnout3_traffic_runtime\.h"', src, re.M):
        res.fail("-", "source",
                 'src/burnout3_full.c does not include '
                 '"burnout3_traffic_runtime.h"')
    if "b3_traffic_data_load()" not in src:
        res.fail("-", "source",
                 "traffic_init() never calls b3_traffic_data_load(); the "
                 "per-track traffic.bin is not being read")

    # 1b. FLOW.  The pool request's row ORDER is not a travel sense.
    #     FUN_001A6070 @0x001A6098 min/maxes first_row/last_row before it does
    #     anything else, so the order reaches nothing in retail, and
    #     FUN_0019F1C0 @0x0019F21D -- the whole traffic mover -- only ADDS to
    #     the agent row cursor and only INCREMENTS its row.  There is no
    #     reverse agent.  A port that reads `first_row > last_row` as "drive
    #     this one backwards" turns the against-race carriageway around,
    #     because a request whose road runs against the race is authored
    #     descending: on US_C3_V1 that is 15 of the 18 stamping requests on
    #     each of the two oncoming lanes, against 3 of 18 on the racers' own
    #     two -- traffic running the player's way on both sides of the road.
    seed = re.search(r"static int traffic_pool_seed_at\(.*?\n\}", src, re.S)
    if not seed:
        res.fail("-", "source", "traffic_pool_seed_at() not found")
    else:
        if re.search(r"path_dir\s*=\s*\(?\s*signed char\s*\)?\s*\(?\s*"
                     r"\w*reverse\w*\s*\?", seed.group(0)):
            res.fail("-", "source",
                     "traffic_pool_seed_at() still turns a pool request into "
                     "a REVERSE agent -- FUN_0019F1C0 has no reverse walk and "
                     "the row order it reads is the order the range is met "
                     "along the race, not a travel sense")
        if not re.search(r"t->path_dir\s*=\s*1\s*;", seed.group(0)):
            res.fail("-", "source",
                     "traffic_pool_seed_at() does not pin the agent travel "
                     "sense to +1 (FUN_0019F1C0 @0x0019F21D)")
    adv = re.search(r"static int traffic_path_advance\(.*?\n\}", src, re.S)
    if adv and re.search(r"path_dir\s*<\s*0", adv.group(0)):
        res.fail("-", "source",
                 "traffic_path_advance() still walks an agent DOWN its "
                 "descriptor rows; retail's mover has one direction")

    # 2. RENDER/COLLISION PARITY.  traffic_pose() is the render transform; the
    #    collision transform is the rigid body synthesised at
    #    pos.y - 0.5 - ymin with no ground probe.  The probe in traffic_pose
    #    must stay clamped to that plane or the mesh is drawn off its body.
    if "B3_TRAFFIC_POSE_SNAP_M" not in src:
        res.fail("-", "source",
                 "traffic_pose() has no B3_TRAFFIC_POSE_SNAP_M clamp -- the "
                 "render ground-probe can move the mesh off the collision "
                 "body (a truck you hit and never see)")
    else:
        pose = re.search(r"static void traffic_pose\(.*?\n\}", src, re.S)
        if pose and "B3_TRAFFIC_POSE_SNAP_M" not in pose.group(0):
            res.fail("-", "source",
                     "B3_TRAFFIC_POSE_SNAP_M is defined but traffic_pose() "
                     "does not apply it")

    # 2b. RESIDENCY.  The `streamed` flag is the second term of
    #     carcol_pass()'s admission and the renderer does not read it, so a
    #     predicate that is false near the player is a car drawn and not
    #     collidable.  Retail's own predicate is FUN_0019D7F0's four XZ
    #     half-planes (via FUN_001AD4A0, written to body+0x216 by
    #     FUN_0011BC60 @0x0011BD55): XZ only, no Y, NO RAY.  A downward
    #     ground probe is a different question and answers "no unit" over
    #     every hole in the soup -- 5%..50% of traffic-path samples per
    #     track (`--deck-audit`).
    stream = re.search(r"static int traffic_stream_refresh\(.*?\n\}", src, re.S)
    if not stream:
        res.fail("-", "source", "traffic_stream_refresh() not found")
    else:
        if "b3_collision_unit_at_xz" not in stream.group(0):
            res.fail("-", "source",
                     "traffic_stream_refresh() does not use "
                     "b3_collision_unit_at_xz() -- residency must be "
                     "FUN_0019D7F0's XZ footprint test, not a ground probe")
        if "b3_ground_probe" in stream.group(0):
            res.fail("-", "source",
                     "traffic_stream_refresh() still casts a ground ray: a "
                     "hole in the collision soup will drop the body while "
                     "traffic_render() keeps drawing the car")
    if "B3_TRAFFIC_POSE_DECK_M" not in src:
        res.fail("-", "source",
                 "traffic_pose() has no B3_TRAFFIC_POSE_DECK_M -- a "
                 "multi-level rejection (the correct outcome) cannot be told "
                 "apart from a real mis-pose")

    # 3. every body carcol_pass() can admit must have a draw path
    draw = re.search(r"static void traffic_draw_at\(.*?\n\}", src, re.S)
    if draw and re.search(r"if\s*\(!g_traffic_lists\[car\]\)\s*return\s*;",
                          draw.group(0)):
        res.fail("-", "source",
                 "traffic_draw_at() returns without drawing when the display "
                 "list is missing, but carcol_pass() still admits the trailer "
                 "body on its hull -- collidable and invisible")

    if verbose:
        res.note("source   render/collision parity guards present")


# ------------------------------------------------------------------ runtime
# The census line, with the render/collision parity fields the source patch
# adds (`str` = TrafficCar.streamed, `hull` = g_traffic_hull_ok[]).  Both are
# optional so this validator still parses a binary built before them; the
# drawn => collidable check reports that it cannot run rather than passing
# vacuously.
TFC_RE = re.compile(
    r"\[tfc\] t=\s*(?P<t>[\d.]+)\s+c(?P<slot>\d+)\s+act(?P<act>\d)\s+"
    r"(?:col(?P<col>\d)\s+str(?P<str>\d)\s+hull(?P<hull>\d)\s+)?"
    r"(?P<id>\S+)\s+cat(?P<cat>\d+).*?"
    r"dplayer\s*(?P<dplayer>-?[\d.]+)\s+"
    r"pos\s+(?P<x>-?[\d.]+)\s+(?P<y>-?[\d.]+)\s+(?P<z>-?[\d.]+)"
    r"(?P<crashed>\s+CRASHED)?")

# ------------------------------------------------------------------- FLOW
# THE DEFECT THIS LEG EXISTS TO CATCH: traffic running the SAME WAY on both
# sides of the road.  Burnout's oncoming lane is the game -- the boost and the
# near-miss risk both come from meeting traffic head on -- so a port that
# turns the far carriageway around has lost a mechanic, not a detail.
#
# There is no per-agent direction flag to check against.  Retail has none:
# FUN_0019F1C0 @0x0019F21D is the whole traffic mover and it only ever ADDS to
# the agent's row cursor (+0x30) and INCREMENTS its row, so an agent walks its
# descriptor one way and retires past the last row.  The opposition lives in
# the DATA -- the extracted path network is a directed road graph whose
# parallel carriageway paths are authored running opposite ways -- so the
# check is:
#
#   flow-align   every ACTIVE, un-wrecked agent's heading agrees with its
#                OWN path's forward tangent at its own cursor.  Zero
#                tolerance: a single agent driving its descriptor backwards
#                is a car going the wrong way up a one-way lane.         [C]
#   flow-balance the measured against-the-player share sits inside a band
#                around the share the track's own data predicts for the road
#                the player actually drove (see expected_against_share).
#   flow-lanes   where the track ships a lane cross-section, every agent in a
#                lane runs the way that lane's `dir` column declares.
#
# FLOW_ALIGN_MIN is a heading agreement, not a threshold with a spread behind
# it: an agent placed on its path by traffic_path_sample() and yawed from the
# same tangent scores +1.00 up to the sampler's own curvature error, and the
# failure mode is -1.00.  0.5 is the midpoint of a bimodal measurement.
FLOW_ALIGN_MIN = 0.5

# The balance band.  The prediction is a linear-density model of a spawn
# policy with a granular RNG behind it, sampled over a 20-40 s window, so it
# cannot be exact -- but the failure it must catch is a SIGN failure, where
# the oncoming carriageway is turned around and its share collapses toward
# zero.  Measured on US_C3_V1 with the flag in place: predicted 52%, measured
# 12% (a ratio of 0.23); with it removed: predicted 52%, measured 45% (0.86).
# The floor sits between those two populations, and it is deliberately not
# tight: the per-carriageway POPULATION can be uneven for reasons that are not
# direction at all -- EU_C3_V1 measures 0.58 before the flag was removed and
# 0.69 after, its two against-race lanes carrying a third of the cars its two
# with-race lanes do either way -- and what this check is about is the SIGN.
FLOW_MIN_RATIO = 0.45
FLOW_MAX_RATIO = 1.80

# A road that the data says is two-way at all: below this the prediction has
# nothing to assert and the balance check stands down (a genuinely one-way
# stretch is not a defect).
FLOW_TWOWAY_MIN = 0.15

# How far from the player counts as "the flow the player is driving in", and
# how near the lane cross-section's own reference line an agent has to sit
# before its lane can be identified at all.
FLOW_NEAR_M = 200.0
FLOW_RIBBON_M = 30.0

# Enough samples that a share means something.  The NEAR count is smaller
# because it is a subset by construction and some tracks legitimately keep
# their populated roads away from the player -- AS_C1_V1's spawn policy is
# dominated by the two elevated-expressway paths, and a 30 s run puts only
# ~37 agents inside 200 m of him.
FLOW_MIN_SAMPLES = 40
FLOW_MIN_NEAR = 25

# How near the middle of a declared lane an agent has to sit before that
# lane's `dir` column is allowed to speak for it.  Lane spacings across the
# shipped set run from 2.8 m up, so half of the tightest spacing is the widest
# catchment that cannot seat a car in its neighbour.
FLOW_LANE_SEAT_M = 1.4

# ...and how near the line's own HEIGHT.  The lane table is a 2-D
# cross-section of one road; AS_C1_V1's spawn policy is dominated by two
# elevated-expressway paths that pass over the surface streets, and a car up
# there projects onto the same lateral as a lane 14 m below it.
FLOW_LANE_SEAT_Y = 3.0

# A lane needs a quorum before its column is judged: with one or two samples
# a single agent mid-lane-change decides the verdict.
FLOW_LANE_MIN_PER_LANE = 5


# HOW CLOSE COUNTS AS "the player can see it".
#
# Retail's own answer, and the only distance in the image that is about a
# traffic car being in shot: FUN_001A6070 @0x001A64E5..0x001A6566 destroys a
# car it has just built when any local view is closer than DAT_003A49FC =
# 160.0 m ("retail pays for the car and throws it away rather than let it
# appear in shot", src/burnout3_full.c B3_TRAFFIC_VIEW_GATE_M).  So 160 m is
# retail's own definition of "on screen", and every DRAWN agent inside it
# must be one the solver can hit.
DRAWN_COLLIDABLE_M = 160.0

# The render-pose clamp's two constants, mirrored from src/burnout3_full.c.
# POSE_SNAP_M is the margin inside which the presentation conform may move the
# drawn mesh off the collision body; POSE_DECK_M is where "the probe missed
# the agent's own road and found another deck" begins.  `--deck-audit`
# regenerates the measurement both come from.
POSE_SNAP_M = 2.0
POSE_DECK_M = 5.0


def check_runtime(track, seconds, res, verbose):
    '''Boot the track headless and read the existing B3_TRAFFIC_TELEM census.'''
    tp = os.path.join(TRACKS_DIR, track, "traffic_paths.bin")
    tb = os.path.join(TRACKS_DIR, track, "traffic.bin")
    if not (os.path.exists(tp) and os.path.exists(tb)):
        res.fail(track, "run", "track assets missing")
        return
    ribbon = read_traffic_paths_bin(tp)["points"]
    own = {c["id"] for c in read_traffic_bin(tb)["cars"]}

    # B3_AUTODRIVE: the player has to MOVE, or nothing ever comes inside
    # retail's 160 m view gate and the `drawn => collidable` assertion has
    # no samples to work with -- traffic is created OUTSIDE that radius by
    # construction (FUN_001A6070 @0x001A6566 deletes anything closer).
    # Measured on a parked player: 159 drawn samples, none within 160 m.
    env = dict(os.environ,
               SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy",
               # THE PHOTOREALISM WAVE IS PINNED OFF HERE -- see the same note
               # in tools/validate_carfx.py's shot().  This suite reads log
               # lines rather than pixels, so it is belt and braces; what it
               # buys is that a run of it costs the shadow pass' geometry
               # neither time nor a chance to change the frame count.
               B3_PHOTO="0",
               B3_TRACK=track, B3_FIXED_DT="0.0166667",
               B3_PACE_MAX_TICKS="1", B3_EXIT_AT=str(seconds),
               B3_AUTODRIVE="1",
               # The FLOW leg needs the player pose sampled often enough
               # that "the traffic around him" is a trajectory and not four
               # snapshots; every other check only gains from the extra rows.
               B3_TRAFFIC_TELEM=str(max(1, seconds // 15)),
               B3_TRAFFIC_POSE_LOG="1")
    # B3_BIN points the run at a shadow build, so a fix can be measured
    # before it lands in the tree (the parity work is delivered as patches).
    binary = os.environ.get("B3_BIN") or os.path.join(ROOT, "burnout3")
    try:
        p = subprocess.run([binary], env=env, cwd=ROOT,
                           capture_output=True, text=True,
                           timeout=seconds * 18 + 240)
    except subprocess.TimeoutExpired:
        res.fail(track, "run", "game did not exit")
        return

    out = p.stdout + p.stderr
    worst, worst_id, n, offroad, alien = 0.0, "", 0, 0, set()
    # drawn => collidable
    have_parity_fields = False
    near = 0
    intangible = 0
    intangible_why = {"not-streamed": 0, "no-hull": 0}
    intangible_worst = None
    promoted_intangible = 0
    for m in TFC_RE.finditer(out):
        if m.group("act") != "1":
            continue                      # inactive slot: not drawn
        n += 1
        cid = m.group("id")
        if cid not in own:
            alien.add(cid)
        q = (float(m.group("x")), float(m.group("y")), float(m.group("z")))
        d = _nearest_xz(ribbon, q)
        if d > worst:
            worst, worst_id = d, cid
        if d > MAX_SPAWN_MEDIAN_M:
            offroad += 1

        # --- RENDER/COLLISION PARITY -----------------------------------
        # traffic_render() draws on `active` alone; carcol_pass() admits on
        # `active && streamed && hull_ok`.  Every frame a DRAWN agent spends
        # without the other two terms is a frame the player can see a car
        # and drive straight through it.
        #
        # `col` is not a guess: the runtime prints the very
        # traffic_carcol_admits() the contact pass itself calls, so the two
        # cannot drift.  `str`/`hull` are its inputs, used only to say WHY.
        #
        # A CRASHED agent is retail's PROMOTED type-4 vehicle, and
        # FUN_00114610 @0x001146F2 / @0x00114719 really does veto its pair on
        # `+0x216 == -1` -- so an out-of-unit wreck is intangible in retail
        # too and is counted separately rather than failed.  The assertion is
        # about the LIVE type-3 handle, which retail never vetoes.
        if m.group("col") is None:
            continue
        have_parity_fields = True
        dp = float(m.group("dplayer"))
        if dp > DRAWN_COLLIDABLE_M:
            continue
        near += 1
        if m.group("col") == "1":
            continue
        if m.group("crashed") and m.group("hull") == "1":
            promoted_intangible += 1
            continue
        why = "no-hull" if m.group("hull") != "1" else "not-streamed"
        intangible += 1
        intangible_why[why] += 1
        if intangible_worst is None or dp < intangible_worst[0]:
            intangible_worst = (dp, cid, why, q)

    if n == 0:
        res.fail(track, "run", "no active traffic agents were reported")
        return
    if alien:
        res.fail(track, "run",
                 "agents wearing models this track does not ship: %s"
                 % ", ".join(sorted(alien)[:8]))
    if offroad:
        res.fail(track, "run",
                 "%d/%d active agents further than %.0f m from this track's "
                 "roads (worst %.0f m, %s)"
                 % (offroad, n, MAX_SPAWN_MEDIAN_M, worst, worst_id))

    if not have_parity_fields:
        res.fail(track, "parity",
                 "the [tfc] census carries no col/str/hull fields, so "
                 "'drawn => collidable' cannot be checked -- rebuild with "
                 "the render/collision parity patch")
    elif near == 0:
        # never let the assertion pass vacuously: no agent inside the view
        # gate means the run told us nothing about what the player can see.
        res.fail(track, "parity",
                 "no drawn agent came within %.0f m of the player in %d s, so "
                 "'drawn => collidable' was never exercised"
                 % (DRAWN_COLLIDABLE_M, seconds))
    elif intangible:
        w = intangible_worst
        res.fail(track, "parity",
                 "%d/%d DRAWN agents inside retail's %.0f m view gate are not "
                 "collidable (%s) -- the player can see them and drive "
                 "through them; nearest %s at %.0f m (%s) at (%.0f %.0f %.0f)"
                 "%s"
                 % (intangible, near, DRAWN_COLLIDABLE_M,
                    ", ".join("%s=%d" % kv for kv in intangible_why.items()
                              if kv[1]),
                    w[1], w[0], w[2], w[3][0], w[3][1], w[3][2],
                    ("  [not-streamed here means the agent's XZ is outside "
                     "every unit in this track's collision.bin while its "
                     "traffic path and track.obj both cover it -- an "
                     "EXTRACTION gap, not a runtime law: retail's units tile "
                     "the visible world, so FUN_00114610's +0x216 == -1 pair "
                     "veto never fires in shot]"
                     if intangible_why["not-streamed"] else "")))
    elif verbose:
        res.note("%-11s parity   %d/%d drawn agents inside %.0f m, all "
                 "collidable" % (track, near, n, DRAWN_COLLIDABLE_M))
    if promoted_intangible and verbose:
        res.note("%-11s parity   %d drawn-but-intangible samples were "
                 "CRASHED (retail's promoted type-4 vehicle, which "
                 "FUN_00114610 does veto on +0x216 == -1) -- not counted"
                 % (track, promoted_intangible))

    # --- render pose: a real mis-pose fails, a second deck does not -------
    # [tfc-pose] is the defect class: the ground probe found a surface
    # 2..5 m from the body plane, which is a mis-pose on the agent's OWN
    # road.  [tfc-deck] is the multi-level case (>5 m -- a road one level
    # away), where refusing the snap and drawing the mesh on its body is the
    # correct outcome; see B3_TRAFFIC_POSE_DECK_M in src/burnout3_full.c and
    # `--deck-audit` for the measurement that separates them.
    rejects = out.count("[tfc-pose]")
    decks = out.count("[tfc-deck]")
    if rejects:
        res.fail(track, "run",
                 "%d render-pose snaps rejected within %.0f m of the body "
                 "plane: the ground probe tried to draw a body off its own "
                 "road" % (rejects, POSE_DECK_M))
    if decks and verbose:
        res.note("%-11s run      %d multi-level pose rejections (paths whose "
                 "road is another deck; mesh kept on its body)"
                 % (track, decks))
    if verbose:
        res.note("%-11s run      %d agent samples, worst %.1f m off ribbon"
                 % (track, n, worst))

    # --- FLOW: does the traffic run both ways, and the right way? --------
    check_flow(track, out, res, verbose)


# --------------------------------------------------------------- flow model
FLOW_RE = re.compile(
    r"\[tfc\] t=\s*(?P<t>[\d.]+)\s+c(?P<slot>\d+)\s+act(?P<act>\d)\s+"
    r"(?:col\d\s+str\d\s+hull\d\s+)?(?P<id>\S+)\s+cat\d+.*?"
    r"lane(?P<lane>-?\d+)\(\s*(?P<lanelat>[-+][\d.]+)(?P<lanedir>[-+])\)\s+"
    r"dir(?P<dir>[-+]\d+)\s+path(?P<path>\d+)@\s*(?P<cursor>[-\d.]+)\s+"
    r".*?yaw\s*(?P<yaw>[-+][\d.]+)\s+dplayer\s*(?P<dp>[-\d.]+)\s+"
    r"pos\s+(?P<x>-?[\d.]+)\s+(?P<y>-?[\d.]+)\s+(?P<z>-?[\d.]+)"
    r"(?P<crashed>\s+CRASHED)?")
FLOW_SUM_RE = re.compile(
    r"\[tfc\] t=\s*(?P<t>[\d.]+)\s+SUMMARY.*?player\s+(?P<px>-?[\d.]+)\s+"
    r"(?P<pz>-?[\d.]+)\s+pyaw\s*(?P<pyaw>[-+][\d.]+)")


def _path_tangent(paths, points, pid, row):
    pairs = paths[pid]
    r = max(0, min(len(pairs) - 2, int(row)))
    a1, a2 = pairs[r]
    b1, b2 = pairs[r + 1]
    ax = (points[a1][0] + points[a2][0]) * 0.5
    az = (points[a1][2] + points[a2][2]) * 0.5
    bx = (points[b1][0] + points[b2][0]) * 0.5
    bz = (points[b1][2] + points[b2][2]) * 0.5
    dx, dz = bx - ax, bz - az
    l = math.hypot(dx, dz) or 1.0
    return dx / l, dz / l, ax, az


def _mix_density(mix, pid, row, cache):
    '''Cars per metre on this row, from FUN_001A6070's own population law.

    n = (span / (mph * 0.44704)) * (1/60) * sum(rate[0..5]) is LINEAR in the
    span, so the per-metre coefficient is rate_sum / (speed_ms * 60).  The
    owning road record is FUN_0019E5B0's rule: the binding with the largest
    start_row <= row.'''
    key = (pid, row)
    if key in cache:
        return cache[key]
    best = None
    for (bp, rec, slot, start) in mix["bindings"]:
        if bp != pid or start > row:
            continue
        if best is None or start > best[0]:
            best = (start, rec, slot)
    d = 0.0
    if best is not None:
        for r in mix["roads"]:
            if r["record"] == best[1] and r["slot"] == best[2]:
                ms = r["mph"] * 0.44704
                if ms > 0.0:
                    d = sum(r["rate"]) / (ms * 60.0)
                break
    cache[key] = d
    return d


def flow_model(track):
    '''Every path row that can hold a car, as (x, z, tangent_x, tangent_z,
    cars per metre).

    Two filters, both from the track's own assets:
      * only a row a STAMPING (direction 0) request covers is ever populated --
        FUN_001A3470 @0x001A34AC sends directions 1 and 2 to the tear-down arm
        while the manager travel-sense at +0x363BC is 1, which is what a car
        driving a race forwards holds it at;
      * only a row the v4 spawn policy binds to a road record has a rate at
        all, and its cars-per-metre is that record's.

    NO RACE REFERENCE IS NEEDED.  The comparison that follows is against the
    PLAYER'S OWN HEADING at each sampled pose, so the model answers exactly
    the question the census does -- "of the traffic around him, how much runs
    the other way" -- on every track, including the ones whose route.bin has
    no main-circuit rows (US_P1_V1) and the ones whose race line sits 40 m off
    its own route polyline (EU_C3_V1).'''
    tp = read_traffic_paths_full(os.path.join(TRACKS_DIR, track,
                                              "traffic_paths.bin"))
    if tp["mix"] is None:
        return None
    covered = {}
    for r in tp["requests"]:
        if r["direction"] != 0:
            continue
        lo = min(r["first_row"], r["last_row"])
        hi = max(r["first_row"], r["last_row"])
        covered.setdefault(r["path_id"], set()).update(range(lo, hi + 1))
    rows, cache = [], {}
    for pid, pairs in enumerate(tp["paths"]):
        cov = covered.get(pid)
        if not cov:
            continue
        step = max(1, len(pairs) // 400)
        for row in range(0, len(pairs) - 1, step):
            if row not in cov:
                continue
            tx, tz, ax, az = _path_tangent(tp["paths"], tp["points"], pid, row)
            d = _mix_density(tp["mix"], pid, row, cache)
            if d > 0.0:
                rows.append((ax, az, tx, tz, d * step))
    return rows


def expected_against_share(rows, poses, radius=FLOW_NEAR_M):
    '''What share of the live traffic within `radius` of the player the DATA
    predicts should be running against him, over the poses the run actually
    visited.  Weighted by the spawn policy's linear density, so it is a
    prediction about CARS, not about tarmac.'''
    w = a = 0.0
    for (px, pz, pyaw) in poses:
        pfx, pfz = math.sin(pyaw), -math.cos(pyaw)
        for (x, z, tx, tz, d) in rows:
            if (x - px) ** 2 + (z - pz) ** 2 > radius * radius:
                continue
            if tx * pfx + tz * pfz >= 0.0:
                w += d
            else:
                a += d
    return (a / (w + a)) if (w + a) > 0.0 else None


def check_flow(track, out, res, verbose):
    '''The FLOW leg: does the traffic run BOTH WAYS, and the right way?'''
    if not FLOW_SUM_RE.search(out):
        res.fail(track, "flow",
                 "the [tfc] census carries no `yaw` / `player ... pyaw` "
                 "fields, so no travel direction can be read -- rebuild with "
                 "the traffic-flow telemetry patch")
        return
    tp = read_traffic_paths_full(os.path.join(TRACKS_DIR, track,
                                              "traffic_paths.bin"))
    lanes = read_traffic_bin(os.path.join(TRACKS_DIR, track,
                                          "traffic.bin"))["lanes"]
    onc = read_traffic_bin(os.path.join(TRACKS_DIR, track,
                                        "traffic.bin"))["oncoming"]
    rows = flow_model(track)

    poses = {}
    for m in FLOW_SUM_RE.finditer(out):
        poses[m.group("t")] = (float(m.group("px")), float(m.group("pz")),
                               float(m.group("pyaw")))

    n = near = near_against = 0
    wrecked = 0
    misaligned = []
    worst_align = 1.0
    lane_rows = []            # (lane index, agreed with the lane's dir)
    for m in FLOW_RE.finditer(out):
        if m.group("act") != "1":
            continue
        t = m.group("t")
        if t not in poses:
            continue
        # A WRECK IS NOT A ROAD AGENT.  Retail promotes a crashed traffic
        # car to a type-4 vehicle and it is carried by the crash physics from
        # then on -- its heading is whatever the impact left it at, and the
        # parity leg above already treats it as a separate population.  All
        # three "backwards" samples of the first EU_C1_V1 run were the same
        # COMPCAR14 the player had just hit, stationary at spd 0.0 with the
        # CRASHED marker on the line.
        if m.group("crashed"):
            wrecked += 1
            continue
        px, pz, pyaw = poses[t]
        yaw = float(m.group("yaw"))
        fx, fz = math.sin(yaw), -math.cos(yaw)
        n += 1

        # 1. FLOW-ALIGN: does it drive its own descriptor forwards?  [C]
        pid = int(m.group("path"))
        if pid < len(tp["paths"]):
            tx, tz, _ax, _az = _path_tangent(tp["paths"], tp["points"], pid,
                                             float(m.group("cursor")))
            dot = fx * tx + fz * tz
            if dot < worst_align:
                worst_align = dot
            if dot < FLOW_ALIGN_MIN:
                misaligned.append((m.group("id"), pid,
                                   float(m.group("cursor")), dot))

        # 2. FLOW-BALANCE: the split the player is driving in
        x, z = float(m.group("x")), float(m.group("z"))
        if (x - px) ** 2 + (z - pz) ** 2 <= FLOW_NEAR_M ** 2:
            near += 1
            if fx * math.sin(pyaw) + fz * -math.cos(pyaw) < 0.0:
                near_against += 1

        # 3. FLOW-LANES: the lane cross-section's own `dir` column.  It is
        #    written against the ONCOMING polyline with +1 = ASCENDING index,
        #    so the agent's heading is compared to that polyline's tangent,
        #    not to the race ribbon -- the two are independent recoveries and
        #    this is the check that they agree.
        if lanes and len(onc) >= 2:
            got = _oncoming_frame(onc, x, z, float(m.group("y")))
            if got is not None:
                otx, otz, olat, odist = got
                if odist <= FLOW_RIBBON_M:
                    li = min(range(len(lanes)),
                             key=lambda i: abs(lanes[i][0] - olat))
                    # Seated in the MIDDLE of a declared lane, not merely
                    # nearest to one: AS_C1_V1 declares eight lanes with two
                    # of them 2.8 m apart, so a 3 m catchment seats a car in
                    # whichever of a pair it is not in and reads its direction
                    # off the wrong row.
                    if abs(lanes[li][0] - olat) <= FLOW_LANE_SEAT_M:
                        want = 1 if lanes[li][1] >= 0 else -1
                        got_dir = 1 if (fx * otx + fz * otz) >= 0.0 else -1
                        lane_rows.append((li, got_dir == want))

    if n < FLOW_MIN_SAMPLES:
        res.fail(track, "flow",
                 "only %d active-agent samples: the flow was never measured"
                 % n)
        return

    if misaligned:
        w = min(misaligned, key=lambda e: e[3])
        res.fail(track, "flow-align",
                 "%d/%d active agents are driving their OWN descriptor "
                 "BACKWARDS (worst %s on path %d row %.0f, heading . tangent "
                 "%+.2f) -- retail's mover FUN_0019F1C0 only ever advances a "
                 "row cursor, so there is no reverse agent to model"
                 % (len(misaligned), n, w[0], w[1], w[2], w[3]))
    elif verbose:
        res.note("%-11s flow     %d agents, all forward on their own path "
                 "(worst heading . tangent %+.2f)" % (track, n, worst_align))

    if rows is None:
        res.note("%-11s flow     no v4 spawn policy: balance not predicted"
                 % track)
    elif near < FLOW_MIN_NEAR:
        res.fail(track, "flow",
                 "only %d agents came within %.0f m of the player, so the "
                 "flow he drives in was never sampled" % (near, FLOW_NEAR_M))
    else:
        want = expected_against_share(rows, list(poses.values()))
        got = near_against / float(near)
        if want is None:
            res.note("%-11s flow     no stamped road near the run's path"
                     % track)
        elif want < FLOW_TWOWAY_MIN:
            res.note("%-11s flow     the data says this stretch is one-way "
                     "(%.0f%% against): balance not asserted, measured %.0f%%"
                     % (track, 100 * want, 100 * got))
        elif got < want * FLOW_MIN_RATIO or got > want * FLOW_MAX_RATIO:
            res.fail(track, "flow-balance",
                     "%.0f%% of the traffic within %.0f m of the player runs "
                     "AGAINST him; this track's own directed path network and "
                     "spawn policy predict %.0f%% for the road he drove "
                     "(allowed %.0f%%-%.0f%%, %d samples).  A collapse toward "
                     "zero is the far carriageway running the player's way -- "
                     "traffic on both sides of the road going where he is"
                     % (100 * got, FLOW_NEAR_M, 100 * want,
                        100 * want * FLOW_MIN_RATIO,
                        100 * want * FLOW_MAX_RATIO, near))
        elif verbose:
            res.note("%-11s flow     %.0f%% against the player within %.0f m, "
                     "data predicts %.0f%% (ratio %.2f, %d samples)"
                     % (track, 100 * got, FLOW_NEAR_M, 100 * want,
                        got / want, near))

    if wrecked and verbose:
        res.note("%-11s flow     %d samples were CRASHED agents (retail's "
                 "promoted type-4 vehicle, carried by the crash physics) -- "
                 "not counted" % (track, wrecked))

    if lane_rows:
        per = {}
        for li, ok in lane_rows:
            e = per.setdefault(li, [0, 0])
            e[0 if ok else 1] += 1
        quorum = {li for li, v in per.items()
                  if v[0] + v[1] >= FLOW_LANE_MIN_PER_LANE}
        lane_rows = [(li, ok) for li, ok in lane_rows if li in quorum]
        per = {li: v for li, v in per.items() if li in quorum}
    if lane_rows:
        bad = [li for li, ok in lane_rows if not ok]
        if len(bad) > len(lane_rows) * 0.10:
            res.fail(track, "flow-lanes",
                     "%d/%d agents sitting in a declared lane run the way "
                     "that lane's `dir` column does NOT declare "
                     "(per lane ok/bad: %s; lane table %s)"
                     % (len(bad), len(lane_rows),
                        ", ".join("%d:%d/%d" % (li, v[0], v[1])
                                  for li, v in sorted(per.items())),
                        ", ".join("%+.1fm/%s" % (l[0], "asc" if l[1] >= 0
                                                 else "desc")
                                  for l in lanes)))
        elif verbose:
            res.note("%-11s flow     %d/%d lane-seated agents run the way "
                     "their lane declares" % (track, len(lane_rows) - len(bad),
                                              len(lane_rows)))


def _oncoming_frame(onc, x, z, y=None):
    '''Nearest point on the oncoming polyline: (tangent, lateral, distance),
    with the extractor's own lateral sign (s = tx*(z-cz) - tz*(x-cx)).

    `y` gates on the line's own height, so a car on an elevated deck is not
    seated in a lane of the street underneath it.'''
    best = bi = None
    for i, p in enumerate(onc):
        d2 = (p[0] - x) ** 2 + (p[2] - z) ** 2
        if best is None or d2 < best:
            best, bi = d2, i
    if bi is None:
        return None
    if y is not None and abs(onc[bi][1] - y) > FLOW_LANE_SEAT_Y:
        return None
    a = onc[bi]
    b = onc[(bi + 1) % len(onc)]
    tx, tz = b[0] - a[0], b[2] - a[2]
    l = math.hypot(tx, tz)
    if l < 1e-6:
        return None
    tx, tz = tx / l, tz / l
    return tx, tz, tx * (z - a[2]) - tz * (x - a[0]), math.sqrt(best)


# ---------------------------------------------------------------- deck audit
# Where the two pose constants come from, replayed offline so the claim is a
# number and not a memory.  This runs the EXACT queries the runtime makes --
# b3c_down_ray()'s one-sided Moller-Trumbore with the image's compiled-in
# epsilons, over one 8 m XZ cell -- against every cross-section of every
# traffic path on a track, and reports:
#
#   streamMISS  the ray finds no polygon: the port's OLD residency predicate,
#               and therefore an agent drawn but refused by carcol_pass()
#   delta       |ground - (path_y - 0.5)|, the render-pose conform's error
#
# Measured over all 36 shipped tracks, 1,635,310 samples: streamMISS runs
# 5.24% (US_C1_V1) to 50.15% (AS_C3_V2), median 21.56%, 22.20% overall, and
# 85% of the misses are XZ cells holding no triangle at ANY height.  The
# delta distribution over the 1,271,318 samples that DO find ground is
# 98.370% inside 0.6 m, 0.971% out to 2 m, 0.059% in 2-4 m, a TROUGH of 16
# samples (0.001%) in 4-5 m, then 0.599% spread over 5-23 m -- the
# multi-level population.  Hence POSE_SNAP_M = 2.0 and POSE_DECK_M = 5.0.
DET_EPS = 9.99999993922529e-09
LO_K = -9.999999747378752e-06
HI_K = 1.0000100135803223
COL_CELL = 8.0


def _load_collision_bin(path):
    import numpy as np
    d = open(path, "rb").read()
    if d[:4] != b"B3CL":
        raise ValueError("not a B3CL asset")
    ver, count = struct.unpack_from("<II", d, 4)
    if ver != 1:
        raise ValueError("B3CL version %d" % ver)
    rec = np.frombuffer(d, dtype=np.uint8, count=count * 40,
                        offset=0x28).reshape(count, 40)
    v = rec[:, :36].copy().view("<f4").reshape(count, 9).astype(float)
    typ = rec[:, 36:38].copy().view("<u2").reshape(count)
    # game -> GL, exactly as b3_collision_load does
    v0 = np.stack([v[:, 0], v[:, 1], -v[:, 2]], 1)
    v1 = np.stack([v[:, 6], v[:, 7], -v[:, 8]], 1)
    v2 = np.stack([v[:, 3], v[:, 4], -v[:, 5]], 1)
    keep = ~((((typ & 0xFF) == 0x22) | ((typ & 0xFF) == 0x23)
              | ((typ & 0x1000) != 0)))
    return v0[keep], v1[keep], v2[keep]


def _grid_collision(v0, v1, v2):
    import numpy as np
    allv = np.concatenate([v0, v1, v2], 0)
    mn = allv.min(0)
    gw = int((allv[:, 0].max() - mn[0]) / COL_CELL) + 2
    gh = int((allv[:, 2].max() - mn[2]) / COL_CELL) + 2
    lo = np.floor((np.minimum(np.minimum(v0, v1), v2) - mn) / COL_CELL)
    hi = np.floor((np.maximum(np.maximum(v0, v1), v2) - mn) / COL_CELL)
    buckets = {}
    for i in range(len(v0)):
        for cz in range(int(lo[i, 2]), int(hi[i, 2]) + 1):
            for cx in range(int(lo[i, 0]), int(hi[i, 0]) + 1):
                if 0 <= cx < gw and 0 <= cz < gh:
                    buckets.setdefault(cz * gw + cx, []).append(i)
    return dict(mn=mn, gw=gw, gh=gh,
                buckets={k: np.array(t) for k, t in buckets.items()},
                v0=v0, v1=v1, v2=v2)


def _down_ray(g, x, ay, z, by):
    '''b3c_down_ray(): min parametric t over one cell, or None.'''
    import numpy as np
    cx = int(math.floor((x - g["mn"][0]) / COL_CELL))
    cz = int(math.floor((z - g["mn"][2]) / COL_CELL))
    if cx < 0 or cz < 0 or cx >= g["gw"] or cz >= g["gh"]:
        return None
    idx = g["buckets"].get(cz * g["gw"] + cx)
    if idx is None:
        return None
    v0 = g["v0"][idx]
    e1 = g["v1"][idx] - v0
    e2 = g["v2"][idx] - v0
    d = np.array([0.0, by - ay, 0.0])
    P = np.cross(np.broadcast_to(d, e2.shape), e2)
    det = np.einsum("ij,ij->i", e1, P)
    ok = det > DET_EPS
    if not ok.any():
        return None
    T = np.array([x, ay, z]) - v0
    u = np.einsum("ij,ij->i", T, P)
    lo, hi = det * LO_K, det * HI_K
    ok &= (u > lo) & (u <= hi)
    Q = np.cross(T, e1)
    vv = Q[:, 1] * d[1]
    ok &= (vv > lo) & ((u + vv) <= hi)
    t = np.einsum("ij,ij->i", e2, Q)
    ok &= (t > lo) & (t <= hi)
    if not ok.any():
        return None
    with np.errstate(divide="ignore", invalid="ignore"):
        tt = np.where(ok, t / det, np.inf)
    k = int(np.argmin(tt))
    return None if not np.isfinite(tt[k]) else ay + (by - ay) * tt[k]


def _path_cross_sections(path):
    '''Every (point_a, point_b) pair of every path in traffic_paths.bin.'''
    import numpy as np
    d = open(path, "rb").read()
    if d[:4] != b"B3TP":
        raise ValueError("not a B3TP asset")
    ver, npt, npath, nwin, nreq = struct.unpack_from("<IIIII", d, 4)
    o = 0x18
    pts = np.frombuffer(d, dtype="<f4", count=npt * 3,
                        offset=o).reshape(npt, 3).astype(float)
    o += npt * 12
    out = []
    for _ in range(npath):
        n = struct.unpack_from("<I", d, o)[0]
        o += 4
        pr = np.frombuffer(d, dtype="<u2", count=n * 2, offset=o).reshape(n, 2)
        o += n * 4 + n * 8 + n * 0x12
        out.append((pts[pr[:, 0].astype(int)], pts[pr[:, 1].astype(int)]))
    return out


def deck_audit(tracks, res, verbose):
    try:
        import numpy  # noqa: F401
    except ImportError:
        res.fail("-", "deck-audit", "numpy is required for --deck-audit")
        return
    import numpy as np
    laterals = (0.15, 0.35, 0.5, 0.65, 0.85)
    bands = [0.0, 0.6, 1.0, 1.5, POSE_SNAP_M, 3.0, 4.0, POSE_DECK_M,
             7.0, 10.0, 15.0, 20.0, 1e9]
    grand = np.zeros(len(bands) - 1, dtype=np.int64)
    for track in tracks:
        tdir = os.path.join(TRACKS_DIR, track)
        cpath = os.path.join(tdir, "collision.bin")
        ppath = os.path.join(tdir, "traffic_paths.bin")
        if not (os.path.exists(cpath) and os.path.exists(ppath)):
            continue
        g = _grid_collision(*_load_collision_bin(cpath))
        nq = miss = 0
        deltas = []
        for a, b in _path_cross_sections(ppath):
            for lat in laterals:
                q = a + (b - a) * lat
                for r in range(len(q)):
                    x, y, z = float(q[r, 0]), float(q[r, 1]), float(q[r, 2])
                    nq += 1
                    # traffic_stream_refresh()'s OLD predicate
                    if _down_ray(g, x, y + 5.0, z, y - 25.0) is None:
                        miss += 1
                    # traffic_pose()'s conform
                    gy = _down_ray(g, x, y + 3.0, z, y - 27.0)
                    if gy is not None:
                        deltas.append(abs(gy - (y - 0.5)))
        if not nq:
            continue
        deltas = np.array(deltas) if deltas else np.zeros(1)
        hist = np.histogram(deltas, bins=bands)[0]
        grand += hist
        res.note("%-11s deck     %6d samples  ray-miss %5.1f%%  "
                 "delta p50 %.2f p95 %.2f max %5.2f  own-road(<%.0f) %5.2f%%  "
                 "deck(>%.0f) %5.2f%%"
                 % (track, nq, 100.0 * miss / nq,
                    float(np.median(deltas)),
                    float(np.percentile(deltas, 95)), float(deltas.max()),
                    POSE_DECK_M,
                    100.0 * float((deltas < POSE_DECK_M).sum()) / len(deltas),
                    POSE_DECK_M,
                    100.0 * float((deltas >= POSE_DECK_M).sum())
                    / len(deltas)))
    total = int(grand.sum())
    if not total:
        return
    res.note("ALL TRACKS  deck     delta bands: %s"
             % "  ".join("%.1f-%.1f:%.3f%%"
                         % (bands[i], bands[i + 1], 100.0 * grand[i] / total)
                         for i in range(len(grand)) if grand[i]))
    trough = int(grand[bands.index(4.0)])
    if trough > total // 1000:
        res.fail("-", "deck-audit",
                 "the 4-5 m band holds %.3f%% of samples: POSE_DECK_M = %.1f "
                 "no longer separates an own-road mis-pose from another deck"
                 % (100.0 * trough / total, POSE_DECK_M))


# --------------------------------------------------------------------- main
def main():
    global MAX_ONCOMING_MEDIAN_M, MAX_SPAWN_MEDIAN_M
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tracks", default=None,
                    help="comma-separated track ids (default: all)")
    ap.add_argument("--run", action="store_true",
                    help="also boot each track and check live agents")
    ap.add_argument("--seconds", type=int, default=20,
                    help="race seconds per track for --run")
    ap.add_argument("--max-oncoming-m", type=float, default=MAX_ONCOMING_MEDIAN_M)
    ap.add_argument("--max-spawn-m", type=float, default=MAX_SPAWN_MEDIAN_M)
    ap.add_argument("--jobs", type=int, default=1,
                    help="concurrent --run game boots (each its own process)")
    ap.add_argument("--deck-audit", action="store_true",
                    help="replay the runtime's ground queries over every "
                         "traffic path offline (no game boot): the "
                         "measurement POSE_SNAP_M/POSE_DECK_M come from")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    MAX_ONCOMING_MEDIAN_M = args.max_oncoming_m
    MAX_SPAWN_MEDIAN_M = args.max_spawn_m

    if args.tracks:
        tracks = [t for t in args.tracks.split(",") if t]
    else:
        tracks = sorted(d for d in os.listdir(TRACKS_DIR)
                        if os.path.isdir(os.path.join(TRACKS_DIR, d)))
    skipped = [t for t in tracks if not is_extracted(t)]
    tracks = [t for t in tracks if is_extracted(t)]
    for t in skipped:
        print("   skip %-11s not an extracted track (no route.bin)" % t)
    if not tracks:
        print("no extracted tracks under %s" % TRACKS_DIR)
        return 2

    res = Result()
    check_source(res, args.verbose)
    check_pinned(tracks, res, args.verbose)
    for t in tracks:
        check_track(t, res, args.verbose)
    if args.deck_audit:
        deck_audit(tracks, res, True)
    if args.run:
        if args.jobs > 1:
            import concurrent.futures
            with concurrent.futures.ThreadPoolExecutor(
                    max_workers=args.jobs) as ex:
                futs = [ex.submit(check_runtime, t, args.seconds, res,
                                  args.verbose) for t in tracks]
                for f in futs:
                    f.result()
        else:
            for t in tracks:
                check_runtime(t, args.seconds, res, args.verbose)

    for note in res.notes:
        print("   %s" % note)

    print("\ntraffic alignment: %d track(s), %d failure(s)"
          % (len(tracks), len(res.failures)))
    if res.failures:
        by_check = {}
        for track, check, msg in res.failures:
            by_check.setdefault(check, []).append((track, msg))
        for check in sorted(by_check):
            print("\n  [%s]" % check)
            for track, msg in by_check[check]:
                print("    %-11s %s" % (track, msg))
        return 1
    print("PASS -- every track's traffic data is its own, every collidable "
          "body is drawable at its collision transform, and the traffic runs "
          "both ways down its own roads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
