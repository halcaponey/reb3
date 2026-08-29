#!/usr/bin/env python3
"""
rt_car_strip -- what tier 4rc actually LOOKS like, before and after.

WHAT THIS ANSWERS.  tools/validate_photo.py section 13 gates the car shadows
(does a car provably cast, does the blob ever double up).  This is the other
half, and the same division of labour tools/photo_strip.py has with the rest of
the suite: what does it look like, side by side, on frames a human picked for
being worth looking at.

THREE SHEETS, and each answers a different question:

    road      the player's own shadow on the tarmac.  Retail's blobbyshadow is
              a fixed ellipse on the ground plane; the traced shadow has the
              car's outline, its wheels, and the sun's actual direction.
    pack      CAR ON CAR.  A frame with the field bunched, so one car's shadow
              can fall across another's bodywork -- which the blob could not do
              at all, because it is drawn on the ground.
    drive     a CHASE-CAM SEQUENCE, four consecutive moments of ordinary
              driving, because a shadow that reads well in a still and badly in
              motion is a shadow that has not been looked at properly.

THE THREE LEGS on every frame, and the middle one is the point:

    blob      ray tracing on, B3_RT_CARS=off -- the ray answers for the world
              and the cars keep retail's own quad.  This is what shipped.
    traced    the shipped default: the cars are in the trace and their blobs
              have stood aside.
    neither   B3_RT_CAR_TRACE=0 -- the instances dropped from the ray with the
              blob still suppressed, i.e. the car casting NOTHING.  It is not a
              shipping configuration; it is the control that makes the middle
              panel's difference attributable, and it is the same frame
              validate_photo measures against.

Everything is offscreen and mute.  Usage:

    python3 tools/rt_car_strip.py --out build/photo/carstrip
    python3 tools/rt_car_strip.py --res 1920x1080 --sheets road,pack
"""
import argparse
import os
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get("B3_BIN") or os.path.join(ROOT, "burnout3")

LEGS = [
    ("blob",    "retail blob (B3_RT_CARS=off)",
     {"B3_RT": "1", "B3_RT_CARS": "off"}),
    ("traced",  "TRACED (the shipped default)",
     {"B3_RT": "1", "B3_RT_CARS": "racers"}),
    ("neither", "no shadow at all (the control)",
     {"B3_RT": "1", "B3_RT_CARS": "racers", "B3_RT_CAR_TRACE": "0"}),
]

# The frames, and each was picked by looking rather than by arithmetic.
SHEETS = {
    "road":  ("US_C3_V1", [420]),
    # A LOW SUN, and it is the whole reason this sheet is on a different
    # track.  US_C3_V1's sun is 30 degrees up, so a car's shadow lies under
    # the car and can reach nothing else; US_P1_V1's is the roster's sunset
    # (enviro.dat +0x60 = 1.00/0.65/0.55, the warmest and lowest in the game),
    # which throws a car's shadow several lengths sideways -- across the road,
    # up a kerb and onto whatever is beside it.  That is the case retail's
    # blob cannot represent AT ALL, because the blob is drawn on the ground
    # plane and a shadow that climbs another car is not on the ground.
    "pack":  ("US_P1_V1", [300, 420]),
    "drive": ("US_C3_V1", [380, 420, 460, 500]),
}

OWNED = ("B3_RT", "B3_RT_CARS", "B3_RT_CAR_TRACE", "B3_RT_CAR_N",
         "B3_RT_RAYS", "B3_PHOTO")


def run(tag, frame, env_extra, outdir, res, track):
    bmp = os.path.join(outdir, "cs_%s_%d.bmp" % (tag, frame))
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": track, "B3_RES": res, "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_SHOT": bmp, "B3_SHOT_FRAME": str(frame), "B3_AFX": "1",
        "B3_MUSIC_SEED": "1", "B3_PHOTO": "1",
    })
    for k in OWNED:
        env.pop(k, None)
    env.update(env_extra)
    if os.path.exists(bmp):
        os.remove(bmp)
    subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return bmp if os.path.exists(bmp) else None


def sheet(name, paths, labels, out, crop=None):
    """One contact sheet: a row per frame, a column per leg."""
    rows = len(paths)
    cols = len(paths[0])
    im0 = Image.open(paths[0][0])
    if crop:
        w0, h0 = im0.size
        box = (int(w0 * crop[0]), int(h0 * crop[1]),
               int(w0 * crop[2]), int(h0 * crop[3]))
        im0 = im0.crop(box)
    cw, ch = im0.size
    scale = min(1.0, 640.0 / cw)
    cw, ch = int(cw * scale), int(ch * scale)
    pad, top = 6, 22
    canvas = Image.new("RGB", (cols * (cw + pad) + pad,
                               top + rows * (ch + pad)), (16, 16, 18))
    d = ImageDraw.Draw(canvas)
    for c in range(cols):
        d.text((pad + c * (cw + pad) + 3, 5), labels[c], fill=(230, 230, 235))
    for r in range(rows):
        for c in range(cols):
            im = Image.open(paths[r][c]).convert("RGB")
            if crop:
                w0, h0 = im.size
                im = im.crop((int(w0 * crop[0]), int(h0 * crop[1]),
                              int(w0 * crop[2]), int(h0 * crop[3])))
            im = im.resize((cw, ch), Image.LANCZOS)
            canvas.paste(im, (pad + c * (cw + pad), top + r * (ch + pad)))
    canvas.save(out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "photo",
                                                  "carstrip"))
    ap.add_argument("--res", default="1280x720")
    ap.add_argument("--sheets", default="road,pack,drive")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    want = [s.strip() for s in a.sheets.split(",") if s.strip()]

    print("rt_car_strip -- tier 4rc, before and after")
    print("res %s   out %s" % (a.res, a.out))
    made = []
    for name in want:
        if name not in SHEETS:
            print("  ! no sheet called %s" % name)
            continue
        track, frames = SHEETS[name]
        print("\n== %s == %s frames %s" % (name, track, frames))
        rows = []
        for f in frames:
            row = []
            for tag, _, env in LEGS:
                p = run("%s_%s" % (name, tag), f, env, a.out, a.res, track)
                if not p:
                    print("  ! %s leg %s frame %d produced nothing"
                          % (name, tag, f))
                    return 1
                row.append(p)
            rows.append(row)
            print("  frame %d: %d legs" % (f, len(row)))
        # the road and pack sheets crop in, because the shadow is a few hundred
        # pixels of a 1280x720 frame and a contact sheet that shows the whole
        # frame shows the HUD
        crop = (0.28, 0.42, 0.86, 1.0) if name in ("road", "pack") else None
        out = os.path.join(a.out, "carstrip_%s.png" % name)
        sheet(name, rows, [l for _, l, _ in LEGS], out, crop)
        made.append(out)
        print("  -> %s" % out)

    print("\n%d sheet(s):" % len(made))
    for m in made:
        print("  %s" % m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
