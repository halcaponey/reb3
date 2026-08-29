#!/usr/bin/env python3
"""
rt_shimmer -- how much does the ray-traced shadow FLICKER, and where.

WHAT THIS ANSWERS.  A player reported "with ray tracing on, things far away
have a shimmer / flash".  That is a claim about TIME, and every other tool in
this tree photographs one frame: tools/photo_strip.py pins a frame precisely so
that nothing moves, and tools/validate_photo.py section 6 measures a PINNED
camera and calls any change flicker.  Neither can see this defect, because this
defect only exists while the camera is moving.

So this renders CONSECUTIVE frames of the same deterministic race -- frame N and
frame N+1, same track, same fixed dt, same seed -- and measures how much each
horizontal BAND of the screen changed between them.  On a car doing 100 mph one
frame is about 0.7 m of travel, which moves a surface 600 m away by roughly a
tenth of a pixel: the far bands should be almost identical.  Any large change
out there is not the world moving, it is the shadow term disagreeing with
itself, which is exactly what a shimmer is.

THE MASK, and it is the whole method.  A first attempt measured horizontal
BANDS of the screen and found nothing: on a car doing 100 mph the near road
changes by 20 grey levels a frame and a shimmer of two is invisible underneath
it, and "the far band" guessed by screen row is half near road anyway.  So the
far field is not guessed, it is DISCOVERED -- from the control leg.

Render the same two frames with ray tracing OFF, and take the pixels that
barely changed.  Those are, by construction, the ones the world is not moving:
distant geometry, and the parts of the frame a tenth of a pixel of travel does
not touch.  Then measure the RT legs on EXACTLY those pixels.  The question
becomes the one the player actually asked -- on the pixels the world says are
standing still, does the shadow term disagree with itself? -- and it is
answered without anyone having to nominate where "far away" is.

HOW TO READ IT.  Three legs on the same two frames:

    map     ray tracing OFF, the depth-map cascade answering.  THE CONTROL.
            The cascade fades to lit past 260 m, so the far bands under it are
            as static as the geometry is; whatever it reports is the floor
            that the band selection and the harness contribute, and the
            other legs only mean something ABOVE it.
    raw     the ray, with the far-field corrections turned OFF
            (B3_RT_DIST_BIAS=0, B3_RT_SLOPE=0, B3_RT_GRAZE=0) -- i.e. tier 4r
            exactly as it shipped, which is what the player was looking at.
    fixed   the ray at its defaults.

The headline number is HARD FLIPS: masked pixels that changed by more than 8
grey levels between two consecutive frames.  The mean is a poor statistic here
because a shimmer is a few hundred pixels flipping hard, not every pixel
drifting slightly -- and the mean is exactly what the first version of this
tool reported, which is why it took a sweep to find the cause.

WHAT THE SWEEP FOUND, kept here because a disproved hypothesis is worth as
much as a proved one and costs a rerun to rediscover (US_C3_V1, frames
400-402, 960x540, hard flips on a 124k-pixel mask):

    the map control                      0     <- the floor
    tier 4r as it shipped              475
    + B3_RT_SUN_DEG=0 (no cone at all)  454     the spiral is NOT the cause
    + B3_RT_RAYS=32                     472     nor is the sample count
    + B3_RT_STEPS=512                   475     nor is the traversal budget
    + B3_RT_RANGE=3000                  475     nor is the range cutoff
    + B3_RT_ORIGIN=0.5 (flat, bigger)   567     a bigger FLAT push is WORSE
    + B3_RT_DIST_BIAS=1e-6              179     <- the cause
    + all three corrections             126

Usage with --gate it becomes a suite leg: the fixed leg must stay under a
fraction of the raw leg's flips, so a regression that reintroduces the acne
fails rather than being noticed by a player.

Usage:
    python3 tools/rt_shimmer.py
    python3 tools/rt_shimmer.py --frames 400,401,402 --res 1280x720
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get("B3_BIN") or os.path.join(ROOT, "burnout3")

LEGS = [
    ("map",   {"B3_RT": "0"}),
    # tier 4r EXACTLY as it shipped: all three far-field corrections off.
    # This is what the player was looking at when they reported the shimmer.
    ("raw",   {"B3_RT": "1", "B3_RT_DIST_BIAS": "0", "B3_RT_SLOPE": "0",
               "B3_RT_GRAZE": "0"}),
    ("fixed", {"B3_RT": "1"}),
]

# THE KNOBS THIS TOOL OWNS.  Cleared out of the environment before every leg:
# an operator's shell must not be able to make one leg a different experiment
# from the next, which is the same rule photo_strip.py and photo_perf.py apply
# to B3_PHOTO_*.
OWNED = ("B3_RT", "B3_RT_DIST_BIAS", "B3_RT_SLOPE", "B3_RT_GRAZE",
         "B3_RT_RAYS", "B3_RT_STEPS", "B3_RT_CARS", "B3_RT_SUN_DEG",
         "B3_RT_ORIGIN", "B3_RT_RANGE")


def shot(tag, frame, env_extra, outdir, res, track):
    bmp = os.path.join(outdir, "shim_%s_%d.bmp" % (tag, frame))
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": track, "B3_RES": res, "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_SHOT": bmp, "B3_SHOT_FRAME": str(frame), "B3_AFX": "1",
        "B3_MUSIC_SEED": "1", "B3_PHOTO": "1",
    })
    # every knob this tool touches, cleared first: an operator's shell must not
    # be able to make one leg a different experiment from the next
    for k in OWNED:
        env.pop(k, None)
    env.update(env_extra)
    if os.path.exists(bmp):
        os.remove(bmp)
    subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(bmp):
        return None
    a = np.asarray(Image.open(bmp).convert("RGB")).astype(np.float64)
    return a @ np.array([0.299, 0.587, 0.114])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", default="400,401,402")
    ap.add_argument("--res", default="1280x720")
    ap.add_argument("--track", default="US_C3_V1")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "photo",
                                                  "shimmer"))
    ap.add_argument("--gate", action="store_true",
                    help="exit non-zero unless the corrections still remove "
                         "most of the raw leg's hard flips")
    ap.add_argument("--gate-frac", type=float, default=0.55,
                    help="the fixed leg's hard flips, as a fraction of the "
                         "raw leg's, that still passes")
    ap.add_argument("--quiet", type=float, default=1.0,
                    help="grey levels a frame under which a pixel counts "
                         "as one the world is not moving")
    a = ap.parse_args()
    frames = [int(x) for x in a.frames.split(",")]
    os.makedirs(a.out, exist_ok=True)

    print("rt_shimmer -- consecutive-frame change per screen band")
    print("track %s  res %s  frames %s" % (a.track, a.res, frames))
    print()

    legs = {}
    for tag, env in LEGS:
        imgs = []
        for f in frames:
            im = shot(tag, f, env, a.out, a.res, a.track)
            if im is None:
                print("  ! leg %s frame %d produced nothing" % (tag, f))
                return 1
            imgs.append(im)
        legs[tag] = [np.abs(imgs[i + 1] - imgs[i])
                     for i in range(len(imgs) - 1)]

    # THE MASK, from the control leg: the pixels the WORLD is not moving.
    quiet = np.ones_like(legs["map"][0], dtype=bool)
    for d in legs["map"]:
        quiet &= d < a.quiet
    n = int(quiet.sum())
    tot = quiet.size
    print("the STATIC MASK: %d of %d pixels (%.1f%%) changed by less than "
          "%.1f grey\nlevels a frame with ray tracing OFF -- i.e. the world "
          "is not moving them." % (n, tot, 100.0 * n / tot, a.quiet))
    if n < tot // 50:
        print("  ! the mask is too small to mean anything -- pick a frame "
              "with a longer sightline")
        return 1
    print()

    print("  leg      mean |change|   99th pct   pixels over 8")
    out = {}
    for tag, _ in LEGS:
        vals = np.concatenate([d[quiet] for d in legs[tag]])
        out[tag] = (float(vals.mean()), float(np.percentile(vals, 99)),
                    int((vals > 8.0).sum()))
        print("  %-7s %11.4f  %9.2f  %13d" % ((tag,) + out[tag]))
    print()
    print("  `map` is the FLOOR: the harness's own noise on those pixels.")
    for tag in ("raw", "fixed"):
        print("  %-6s excess over it: mean %+.4f, 99th %+.2f"
              % (tag, out[tag][0] - out["map"][0],
                 out[tag][1] - out["map"][1]))
    f_raw, f_fix = out["raw"][2], out["fixed"][2]
    if f_raw > 0:
        print("\n  THE CORRECTIONS REMOVE %.0f%% OF THE HARD FLIPS "
              "(%d -> %d pixels)"
              % (100.0 * (1.0 - float(f_fix) / f_raw), f_raw, f_fix))
    else:
        print("\n  no hard flips on this frame even without the corrections "
              "-- pick one with a longer sightline")

    if not a.gate:
        return 0
    print("\n== GATE ==")
    if f_raw < 50:
        print("  SKIP  this frame has too few hard flips (%d) to gate on -- "
              "the defect\n        is not reproduced here, so a pass would "
              "mean nothing" % f_raw)
        return 0
    lim = a.gate_frac * f_raw
    ok = f_fix <= lim
    print("  %s  the far-field corrections hold: %d hard flips against %d "
          "without them\n        (limit %.0f, i.e. %.0f%% of the raw leg)"
          % ("ok  " if ok else "FAIL", f_fix, f_raw, lim,
             100.0 * a.gate_frac))
    if not ok:
        print("        A REGRESSION HERE IS SHADOW ACNE AT RANGE, which a "
              "player sees as\n        a shimmer and no still frame can "
              "show.  See THE FAR FIELD in\n        src/burnout3_rt.h.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
