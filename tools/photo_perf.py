#!/usr/bin/env python3
"""
photo_perf -- what the six photorealism effects COST, in milliseconds.

The wave's budget gate: a per-effect frame-ms table at 1080p, and an all-on
figure that has to hold 60.0 fps with the whole existing chain live.  "It feels
fine" is not an answer to that and neither is a frame rate read off the HUD, so
this drives the engine's own B3_FRAME_PROF (src/burnout3_full.c) -- two wall
clock numbers, the whole loop iteration and render_frame() inside it -- and
reports the median of several windows per leg.

HOW TO READ IT.  `render_frame` is the number the effects live in: it is the
scene draw, the shadow pass, the whole aftereffects chain and the HUD.  `frame`
is that plus the simulation, the audio pump and the present.  An effect's cost
is its leg's `render_frame` minus the reference leg's, and the two are measured
on the SAME pinned stretch of the same race, in the same process configuration,
so the difference is the effect.

WHAT IT IS NOT.  It is a WALL CLOCK, not a GPU timer query -- so it measures
what the CPU waited for, which on a GPU-bound frame is the GPU and on a
CPU-bound one is not.  Every run sets B3_NO_VSYNC=1, because with vsync on this
measures the wait for the scanout and nothing else.  The GL line the engine
prints tells you which renderer produced the numbers; a software rasteriser and
a real GPU differ by two orders of magnitude and the table is worthless without
it.

Usage:
    python3 tools/photo_perf.py --res 1920x1080
    python3 tools/photo_perf.py --res 1920x1080 --seconds 25 --legs off,all
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get("B3_BIN") or os.path.join(ROOT, "burnout3")

FX = [
    ("tonemap", "B3_PHOTO_TONEMAP", "1 filmic tonemap + grade"),
    ("ssao",    "B3_PHOTO_SSAO",    "2 SSAO"),
    ("atmos",   "B3_PHOTO_ATMOS",   "3 depth atmospherics"),
    ("shadow",  "B3_PHOTO_SHADOW",  "4 sun shadow maps"),
    ("ssr",     "B3_PHOTO_SSR",     "5 SSR on shine spans"),
    ("godray",  "B3_PHOTO_GODRAY",  "6 god rays"),
    ("lights",  "B3_PHOTO_LIGHTS",  "7 per-source lights"),
]
ALL_ENVS = [e for _, e, _ in FX]

PROF = re.compile(r"\[frameprof\] frame ([\d.]+) ms \(([\d.]+) fps\) \| "
                  r"render_frame ([\d.]+) ms")
GL = re.compile(r"\[Burnout3\] GL: (.*)")
AFX = re.compile(r"\[afx\] chain ready (\d+)x(\d+)")


# TIER 4r's legs.  The ray REPLACES tier 4's map rather than adding a pass,
# so it is not a switch of its own in the FX table above -- it is a different
# ANSWER to the same tier, and the honest way to price it is against tier 4
# on the same stretch of the same race.  `rt` is that tier alone with the ray
# answering, `all-rt` is the shipped default with the option turned on.
#
# B3_RT=0 is pinned on every OTHER leg, for the same reason every B3_PHOTO_*
# is popped below: an operator's build/settings.cfg must not be able to make
# this table price something other than what it says it is pricing.
#
# TIER 4rc's legs are the same idea one level down.  The cars' own trees are
# not a seventh effect either: they are OCCLUDERS ADDED to a ray that has to be
# running anyway, so the honest way to price them is against the same ray
# without them, on the same stretch of the same race.  B3_RT_CARS is pinned on
# every leg here for exactly the reason B3_RT is.
RT_LEGS = {
    "rt":          {"B3_PHOTO": "1", "B3_RT": "1", "B3_RT_CARS": "off",
                    "_only": "shadow"},
    "rt-cars":     {"B3_PHOTO": "1", "B3_RT": "1", "B3_RT_CARS": "racers",
                    "_only": "shadow"},
    "all-rt":      {"B3_PHOTO": "1", "B3_RT": "1", "B3_RT_CARS": "off"},
    "all-rt-cars": {"B3_PHOTO": "1", "B3_RT": "1", "B3_RT_CARS": "racers"},
    "all-rt-traf": {"B3_PHOTO": "1", "B3_RT": "1", "B3_RT_CARS": "all"},
}


def leg_env(tag):
    if tag in RT_LEGS:
        env = {k: v for k, v in RT_LEGS[tag].items() if not k.startswith("_")}
        only = RT_LEGS[tag].get("_only")
        if only:
            for t, e, _ in FX:
                env[e] = "1" if t == only else "0"
        return env
    if tag == "off":
        return {"B3_PHOTO": "0", "B3_RT": "0"}
    if tag == "all":
        return {"B3_PHOTO": "1", "B3_RT": "0"}
    n = None
    if tag.startswith("lights:"):
        n, tag = tag.split(":", 1)[1], "lights"
    env = {"B3_PHOTO": "1", "B3_RT": "0"}
    for t, e, _ in FX:
        env[e] = "1" if t == tag else "0"
    if n is not None:
        env["B3_PHOTO_LIGHT_N"] = n
    return env


def run(tag, args):
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": args.track,
        "B3_RES": args.res,
        "B3_TESTDRIVE": "1",
        "B3_NO_VSYNC": "1",          # or this measures the scanout wait
        # B3_FIXED_DT PUTS THE GOVERNOR IN MODE 0 -- "one tick per frame, no
        # governor, no limiter" (burnout3_full.c) -- which is what this needs:
        # WITH the limiter the frame is pinned at 16.67 ms by construction and
        # every leg reports 60.0 fps whatever it costs, which measures the
        # sleep and not the effect.  It also makes the run deterministic, so
        # two legs are the same stretch of race.
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_MUSIC_SEED": "1",
        "B3_FRAME_PROF": str(args.window),
        "B3_EXIT_AT": str(args.seconds),
        "B3_AFX": "1",
        "B3_MSAA": str(args.msaa),
    })
    for e in ALL_ENVS + ["B3_PHOTO", "B3_RT", "B3_RT_CARS", "B3_RT_RAYS",
                         "B3_RT_CAR_N"]:
        env.pop(e, None)
    env.update(leg_env(tag))
    for kv in args.env:
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    p = subprocess.run(["timeout", "1200", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = p.stdout.decode("utf-8", "replace")
    frames, scenes, gl, size = [], [], None, None
    for line in log.splitlines():
        m = PROF.search(line)
        if m:
            frames.append(float(m.group(1)))
            scenes.append(float(m.group(3)))
        m = GL.search(line)
        if m and not gl:
            gl = m.group(1)
        m = AFX.search(line)
        if m and not size:
            size = m.group(1) + "x" + m.group(2)
    return frames, scenes, gl, size, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--res", default="1920x1080")
    ap.add_argument("--track", default="US_C3_V1")
    ap.add_argument("--seconds", type=float, default=22.0)
    ap.add_argument("--window", type=int, default=180,
                    help="frames per reported window; the first is discarded")
    ap.add_argument("--msaa", type=int, default=4)
    ap.add_argument("--legs", default="",
                    help="comma list; 'rt' and 'all-rt' are tier 4r's")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--lights-n", default="",
                    help="comma list of tier-7 budgets to time, alone")
    args = ap.parse_args()

    legs = ([t.strip() for t in args.legs.split(",") if t.strip()]
            if args.legs else ["off"] + [t for t, _, _ in FX] + ["all"])
    label = {t: d for t, _, d in FX}
    label["off"] = "B3_PHOTO=0 (reference)"
    label["all"] = "ALL SEVEN (the shipped default)"
    label["rt"] = "4r RAY-TRACED sun shadow"
    label["all-rt"] = "ALL SEVEN + RAY TRACING"
    # --lights-n=<a,b,c>: tier 7 ALONE at each budget, which is the cost curve
    # the wave report quotes.  It is a separate axis from --legs because the
    # question is not "what does this effect cost" but "what does one more
    # light cost", and only the second one has a shape.
    if args.lights_n:
        for n in [int(x) for x in args.lights_n.split(",") if x.strip()]:
            legs.append("lights:%d" % n)
            label["lights:%d" % n] = "7 per-source lights, N=%d" % n

    print("photo_perf -- %s, MSAA %dx, %s\n" % (args.res, args.msaa, args.track))
    rows = []
    gl = size = None
    for tag in legs:
        f, s, g, sz, log = run(tag, args)
        gl = gl or g
        size = size or sz
        if len(s) < 2:
            print("  %-10s no profile windows (%d) -- is B3_FRAME_PROF wired?"
                  % (tag, len(s)))
            print(log[-1200:])
            rows.append((tag, None, None, 0))
            continue
        rows.append((tag, statistics.median(f), statistics.median(s), len(s)))
        print("  %-10s frame %6.2f ms  render_frame %6.2f ms  (%d windows)"
              % (tag, rows[-1][1], rows[-1][2], len(s)))

    print("\n  GL: %s" % (gl or "?"))
    print("  chain: %s" % (size or "?"))

    base = next((r for r in rows if r[0] == "off" and r[2]), None)
    print("\n%-28s %10s %10s %10s %8s" % (
        "leg", "frame ms", "render ms", "delta ms", "fps"))
    print("-" * 70)
    out = []
    for tag, f, s, n in rows:
        if s is None:
            continue
        d = (s - base[2]) if base else 0.0
        line = "%-28s %10.2f %10.2f %+10.2f %8.1f" % (
            label.get(tag, tag), f, s, d, 1000.0 / f if f > 0 else 0)
        print(line)
        out.append(line)
    if base:
        allrow = next((r for r in rows if r[0] == "all" and r[2]), None)
        if allrow:
            fps = 1000.0 / allrow[1] if allrow[1] > 0 else 0.0
            hold = fps >= 60.0
            print("\n  ALL-ON HOLDS 60.0: %s (%.1f fps, %.2f ms of headroom "
                  "against a 16.67 ms budget)"
                  % ("YES" if hold else "NO", fps, 16.667 - allrow[1]))
            return 0 if hold else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
