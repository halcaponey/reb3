#!/usr/bin/env python3
"""
photo_strip -- the PHOTOREALISM WAVE's contact sheet and its measurements.

WHAT THIS ANSWERS.  tools/validate_photo.py gates the six effects (does
B3_PHOTO=0 come back bit-identical, does each effect move the pixels it claims
to move, in the direction it claims).  This is the other half: what do they
actually LOOK like, side by side, on one frame, so a human can settle a look by
looking at it.  It is the same division of labour tools/afx_sweep.py has with
tools/validate_postfx.py, and the same pinned-frame discipline.

THE PINNED FRAME.  Every leg renders the SAME frame -- same track, same fixed
dt, same frame number, therefore the same world -- and changes only the
B3_PHOTO_* switches.  Any pixel that differs between a leg and the reference
differs BECAUSE OF THAT EFFECT and nothing else.  There is a warm-up run first
and a pin check afterwards, both for the reason afx_sweep.py documents at
length: B3_SHOT_FRAME pins a RENDERED FRAME NUMBER, and a cold asset cache
spends a different number of frames loading, so frame 400 can land at a
different race time between two runs of the same binary.

Everything is offscreen (SDL_VIDEODRIVER=offscreen, SDL_AUDIODRIVER=dummy):
the operator must see and hear nothing.

Usage:
    python3 tools/photo_strip.py --out build/photo/strip
    python3 tools/photo_strip.py --out build/photo/beauty --res 1920x1080 \
                                 --legs off,all
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The six switches, in tier order.  The tag is what --legs takes.
FX = [
    ("tonemap", "B3_PHOTO_TONEMAP", "1 filmic tonemap + grade"),
    ("ssao",    "B3_PHOTO_SSAO",    "2 SSAO"),
    ("atmos",   "B3_PHOTO_ATMOS",   "3 depth atmospherics"),
    ("shadow",  "B3_PHOTO_SHADOW",  "4 sun shadow maps"),
    ("ssr",     "B3_PHOTO_SSR",     "5 SSR on shine spans"),
    ("godray",  "B3_PHOTO_GODRAY",  "6 god rays"),
]
ALL_ENVS = [e for _, e, _ in FX]

TRACK = "US_C3_V1"
RES = "1280x720"
SHOT_FRAME = "400"


def leg_env(tag):
    """The B3_PHOTO_* environment for one leg.

    `off`  -- the master gate down.  This is the BIT-IDENTITY reference and
              the one every measurement is taken against.
    `all`  -- every effect at its shipped default.
    `<fx>` -- that effect ALONE, so the strip shows what each one contributes
              rather than what the sum looks like.
    `no-<fx>` -- everything except that one, which is the other useful view:
              what does the frame lose when this is taken away.
    `rt` / `all-rt` -- tier 4r, the OPTIONAL ray-traced sun shadow.  It is not
              a switch in the FX table above because it is not a seventh
              effect: it is a different ANSWER to tier 4, replacing the depth
              map's term outright, so the panel worth looking at is `shadow`
              next to `rt` on the same frame.  Every other leg pins B3_RT=0,
              because the option persists to build/settings.cfg in this very
              directory and a strip that quietly measured somebody's saved
              setting would be a strip about nothing.
    """
    if tag == "rt":
        env = {"B3_PHOTO": "1", "B3_RT": "1"}
        for t, e, _ in FX:
            env[e] = "1" if t == "shadow" else "0"
        return env
    if tag == "all-rt":
        return {"B3_PHOTO": "1", "B3_RT": "1"}
    if tag == "off":
        return {"B3_PHOTO": "0", "B3_RT": "0"}
    if tag == "all":
        return {"B3_PHOTO": "1", "B3_RT": "0"}
    env = {"B3_PHOTO": "1", "B3_RT": "0"}
    if tag.startswith("no-"):
        want = tag[3:]
        for t, e, _ in FX:
            if t == want:
                env[e] = "0"
        return env
    for t, e, _ in FX:
        env[e] = "1" if t == tag else "0"
    return env


def run_leg(tag, outdir, res, extra_env, shot_frame, track):
    bmp = os.path.join(outdir, "photo_%s.bmp" % tag)
    env = dict(os.environ)
    # OFFSCREEN FIRST, before anything else can matter.
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": track,
        "B3_RES": res,
        "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_SHOT": bmp,
        "B3_SHOT_FRAME": shot_frame,
        "B3_AFX": "1",
        # Nothing else calls b3_music_seed(), so the shuffle bag opens on
        # time(NULL) and the EA TRAX ticker names a DIFFERENT SONG in every
        # leg.  It is only the HUD, and no measurement here reads it -- but a
        # contact sheet whose eight panels disagree about what is playing
        # invites the reader to wonder what else differs between them, and the
        # whole point of the sheet is that nothing does except the effect.
        "B3_MUSIC_SEED": "1",
    })
    # a stray B3_PHOTO_* in the operator's shell would silently make every leg
    # a different experiment -- clear the lot, then set this leg's
    for e in ALL_ENVS + ["B3_PHOTO", "B3_RT"]:
        env.pop(e, None)
    env.update(leg_env(tag))
    env.update(extra_env)
    if os.path.exists(bmp):
        os.remove(bmp)
    p = subprocess.run(["timeout", "900", os.path.join(_root, "burnout3")],
                       cwd=_root, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    log = (p.stdout.decode("utf-8", "replace")
           + p.stderr.decode("utf-8", "replace"))
    for line in log.splitlines():
        if line.startswith("[afx]") or "postfx path" in line \
                or "shadow map" in line or "PHOTO" in line:
            print("      | " + line.strip())
    if not os.path.exists(bmp):
        print("      ! no frame written for leg %s" % tag)
        print(log[-2500:])
        return None
    return bmp


def load(bmp):
    return np.asarray(Image.open(bmp).convert("RGB")).astype(np.float64)


def measure(ref, img):
    """The numbers that say whether an effect did anything, and what kind.

    dE       mean |delta| in 0..255 levels.  "How much of the picture moved."
    moved%   fraction of pixels that moved by more than 2 levels.
    dMean    the change in frame mean -- a SIGNED number, because "darker" and
             "brighter" are different claims and an effect that says it grounds
             the scene had better not be brightening it.
    clip%    fraction of pixels at 255 on any channel.  The tonemap's whole
             argument is that this goes DOWN.
    sat      mean chroma (max - min over the channels).  The grade's claim.
    """
    d = np.abs(img - ref)
    lum = lambda a: a @ np.array([0.299, 0.587, 0.114])
    return {
        "dE": float(d.mean()),
        "moved%": float((d.max(axis=2) > 2.0).mean() * 100.0),
        "dMean": float(lum(img).mean() - lum(ref).mean()),
        "clip%": float((img >= 254.5).any(axis=2).mean() * 100.0),
        "sat": float((img.max(axis=2) - img.min(axis=2)).mean()),
    }


def pin_ok(a, b):
    """Did these two legs render the SAME WORLD?

    THIS CANNOT BE A PIXEL COMPARISON, and that is the whole difficulty.
    afx_sweep.py gets to check bit-identity in the middle of the frame because
    its effect has a region where it is provably zero; this wave has no such
    region -- the tonemap touches every pixel by construction, so an honest
    "same world" test has to be blind to tone and sensitive to GEOMETRY.

    So it looks at EDGES, not levels.  A grade, a curve, a haze and a shadow
    all rescale the luma; none of them moves where a building's silhouette is.

    IT IS AN OVERLAP, NOT A CORRELATION, and the difference matters.  A shadow
    ADDS edges and a haze REMOVES them, so a correlation of edge MAGNITUDES
    scores a working effect down for working -- the first cut of this check
    flagged four of the six legs as a diverged world when every one of them was
    a correct render of the same moment.  What does not change is WHERE the
    strongest edges are: the silhouette of a building is in the same place
    whatever the light is doing.  So both frames are reduced to their top 4% of
    edge pixels and the answer is the Jaccard overlap of those two sets.

    Same world, different lighting: 0.6 and up.  Different moment on the track:
    under 0.2, because every silhouette has moved.
    """
    def top_edges(x, frac=0.04):
        l = x @ np.array([0.299, 0.587, 0.114])
        gx = np.zeros_like(l)
        gy = np.zeros_like(l)
        gx[:, 1:-1] = l[:, 2:] - l[:, :-2]
        gy[1:-1, :] = l[2:, :] - l[:-2, :]
        g = np.hypot(gx, gy)
        return g >= np.quantile(g, 1.0 - frac)
    ma, mb = top_edges(a), top_edges(b)
    union = float((ma | mb).sum())
    return float((ma & mb).sum() / union) if union > 0 else 0.0


def contact_sheet(paths, labels, out_png, cols=3, width=640):
    ims = []
    for p, lab in zip(paths, labels):
        if not p:
            continue
        im = Image.open(p).convert("RGB")
        h = int(im.height * width / im.width)
        im = im.resize((width, h), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, width, 20], fill=(0, 0, 0))
        d.text((6, 5), lab, fill=(255, 230, 90))
        ims.append(im)
    if not ims:
        return None
    w, h = ims[0].size
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (w * cols, h * rows), (16, 16, 16))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(out_png)
    return out_png


# ======================================================================
# THE DRIVING STRIP -- six frames of an ordinary lap, not one curated shot
#
# WHY THIS EXISTS.  A pinned single frame is the right instrument for "does
# this effect move the pixels it claims to move", and it is the WRONG one for
# "would a player notice".  The headlight beams proved that the hard way: a
# curated shot inside US_C1_V1's stadium tunnel measured +66 levels and was
# used to sign the feature off, while the same build on the same track in
# ordinary sunlight put 0.06 to 2.3 levels on the road -- under one step of
# 8-bit quantisation -- and the player reported, three times, that he could
# not see his headlights.  A frame that agrees with you is not evidence.
#
# So: SIX frames spread over ten seconds of chase driving, the same drive
# rendered twice (the effect on, the effect off), pinned pair by pair, and the
# measurement taken over the ROAD BAND rather than the whole frame.
#
# THE ROAD BAND is rows 42%-62% of the frame, columns 15%-85%.  Measured, not
# guessed: the chase camera sits about 8 m behind the car and 2.5 m up, so the
# car's own body hides the road from its nose out to roughly 10 m and the pool
# a headlight throws lands in the MIDDLE of the frame, not the bottom.  The
# lit-pixel centroid over both test tracks sits at y = 0.49-0.54.
# ======================================================================
DRIVE_FIRST = 400          # first shot, ~6.7 s in at the fixed dt
DRIVE_EVERY = 100          # ...and one every 1.67 s after it, so 6 span 10 s


def run_drive(tag, outdir, res, extra, track, first, every, n):
    """One leg of the driving strip: a numbered BMP sequence from one run."""
    seq = os.path.join(outdir, "seq_" + tag)
    os.makedirs(seq, exist_ok=True)
    for f in os.listdir(seq):
        if f.endswith(".bmp"):
            os.remove(os.path.join(seq, f))
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    for k in list(env):
        if k.startswith("B3_PHOTO"):
            env.pop(k, None)
    env.update({
        "B3_TRACK": track, "B3_RES": res, "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_AFX": "1", "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1",
        # tier 4r pinned OFF, for the reason leg_env() gives: the option
        # persists to build/settings.cfg in the very directory these runs use
        # as their CWD, and a strip that quietly measured somebody's saved
        # setting would be a strip about nothing.
        "B3_RT": "0",
        "B3_SHOT_SEQ": seq, "B3_SHOT_FIRST": str(first),
        "B3_SHOT_EVERY": str(every),
        "B3_EXIT_AT": str(int((first + every * n) / 60.0) + 5),
    })
    env.update(extra)
    subprocess.run(["timeout", "1200", os.path.join(_root, "burnout3")],
                   cwd=_root, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    return sorted(os.path.join(seq, f) for f in os.listdir(seq)
                  if f.startswith("frame_") and f.endswith(".bmp"))[:n]


def road_band(d):
    h, w = d.shape
    return d[int(h * 0.42):int(h * 0.62), int(w * 0.15):int(w * 0.85)]


def drive_strip(out, res, track, on_env, off_env, n, first, every, cols):
    print("  warm-up (discarded)")
    run_drive("warm", out, res, off_env, track, first, every, 1)
    print("  leg ON")
    on = run_drive("on", out, res, on_env, track, first, every, n)
    print("  leg OFF")
    off = run_drive("off", out, res, off_env, track, first, every, n)
    if not on or not off:
        print("  ! a leg wrote no frames")
        return None, []
    rows, paths, labels = [], [], []
    for i, (a, b) in enumerate(zip(on, off)):
        A, B = load(a), load(b)
        d = (A - B) @ np.array([0.299, 0.587, 0.114])
        bx = road_band(d)
        rows.append(dict(i=i, pin=pin_ok(B, A),
                         p99=float(np.percentile(bx, 99)),
                         mean=float(bx.mean()), mx=float(bx.max()),
                         lit=float((bx >= 8).mean())))
        paths += [b, a]
        labels += ["%d/%d  OFF" % (i + 1, len(on)),
                   "%d/%d  ON   road p99 %+.0f  lit %.0f%%"
                   % (i + 1, len(on), rows[-1]["p99"], 100 * rows[-1]["lit"])]
    sheet = contact_sheet(paths, labels,
                          os.path.join(out, "drive_%s.png" % track),
                          cols=cols, width=520)
    hdr = "%-6s %6s %9s %9s %9s %8s" % ("shot", "pin", "road p99",
                                        "road mean", "road max", "lit>=8")
    print("\n" + hdr)
    print("-" * len(hdr))
    for r in rows:
        print("%-6d %6.2f %9.2f %9.2f %9.2f %7.1f%%%s" %
              (r["i"], r["pin"], r["p99"], r["mean"], r["mx"],
               100 * r["lit"], "" if r["pin"] >= 0.55 else "  <-- DIVERGED"))
    return sheet, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--res", default=RES)
    ap.add_argument("--track", default=TRACK)
    ap.add_argument("--frame", default=SHOT_FRAME)
    ap.add_argument("--legs", default="",
                    help="comma list: off,all,<fx>,no-<fx>,rt,all-rt; "
                         "default is off + each of the six alone + all")
    ap.add_argument("--env", action="append", default=[],
                    help="K=V, repeatable, applied to every leg")
    ap.add_argument("--cols", type=int, default=3)
    ap.add_argument("--no-warmup", action="store_true")
    ap.add_argument("--drive", metavar="ENV",
                    help="DRIVING STRIP instead of the pinned-frame sheet: "
                         "six frames over ten seconds of ordinary chase "
                         "driving, rendered twice with ENV=1 and ENV=0, "
                         "before/after, measured over the road band.  "
                         "e.g. --drive B3_PHOTO_HEAD_ON")
    ap.add_argument("--shots", type=int, default=6)
    ap.add_argument("--first", type=int, default=DRIVE_FIRST)
    ap.add_argument("--every", type=int, default=DRIVE_EVERY)
    args = ap.parse_args()

    extra = {}
    for kv in args.env:
        if "=" in kv:
            k, v = kv.split("=", 1)
            extra[k] = v

    if args.drive:
        os.makedirs(args.out, exist_ok=True)
        sheet, rows = drive_strip(args.out, args.res, args.track,
                                  dict(extra, **{args.drive: "1"}),
                                  dict(extra, **{args.drive: "0"}),
                                  args.shots, args.first, args.every,
                                  args.cols)
        if sheet:
            print("\n  strip -> %s" % sheet)
        bad = [r for r in rows if r["pin"] < 0.55]
        return 1 if (bad or not rows) else 0

    legs = ([t.strip() for t in args.legs.split(",") if t.strip()]
            if args.legs else ["off"] + [t for t, _, _ in FX] + ["all"])
    labels = {t: d for t, _, d in FX}
    labels["off"] = "B3_PHOTO=0 (reference)"
    labels["all"] = "ALL SIX (the default)"
    labels["rt"] = "4r RAY-TRACED sun shadow"
    labels["all-rt"] = "ALL + RAY TRACING"
    for t, _, d in FX:
        labels["no-" + t] = "all except " + d

    os.makedirs(args.out, exist_ok=True)

    # THE DISCARDED WARM-UP.  The first run of a cold cache spends a different
    # number of frames materialising assets, so it is not comparable with
    # anything -- including itself.  afx_sweep.py learned this the hard way.
    if not args.no_warmup:
        print("  warm-up (discarded)")
        run_leg("off", args.out, args.res, extra, args.frame, args.track)

    paths, rows = [], []
    ref = None
    for tag in legs:
        print("  leg %s" % tag)
        p = run_leg(tag, args.out, args.res, extra, args.frame, args.track)
        paths.append(p)
        if not p:
            rows.append((tag, None, 0.0))
            continue
        img = load(p)
        if ref is None and tag == "off":
            ref = img
        if ref is None:
            rows.append((tag, None, 1.0))
            continue
        rows.append((tag, measure(ref, img), pin_ok(ref, img)))

    sheet = contact_sheet(paths, [labels.get(t, t) for t in legs],
                          os.path.join(args.out, "strip.png"), cols=args.cols)

    hdr = "%-10s %8s %8s %8s %8s %8s %7s" % (
        "leg", "dE", "moved%", "dMean", "clip%", "sat", "pin")
    print("\n" + hdr)
    print("-" * len(hdr))
    lines = [hdr, "-" * len(hdr)]
    bad = 0
    for tag, m, pin in rows:
        if m is None:
            line = "%-10s   (no frame)" % tag
        else:
            line = "%-10s %8.3f %8.2f %+8.2f %8.2f %8.2f %7.4f" % (
                tag, m["dE"], m["moved%"], m["dMean"], m["clip%"], m["sat"],
                pin)
            if pin < 0.45:
                line += "   <-- WORLD DIVERGED"
                bad += 1
        print(line)
        lines.append(line)
    with open(os.path.join(args.out, "metrics.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    if sheet:
        print("\n  strip -> %s" % sheet)
    print("  metrics -> %s" % os.path.join(args.out, "metrics.txt"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
