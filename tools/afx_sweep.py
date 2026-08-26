#!/usr/bin/env python3
"""
afx_sweep -- measure how VISIBLE the aftereffects speed blur actually is.

The question this answers is not "what is `s`" (that is arithmetic, and
tools/validate_postfx.py already gates it) but "can a human see it", which is
a question about pixels and has to be measured on pixels.

THE PINNED FRAME.  Driving faster to measure the blur at a higher speed also
moves the camera, the traffic and the sun, so two such frames differ for a
dozen reasons and the blur is not one that can be separated out.  Instead
every leg here renders the SAME frame -- same track, same fixed dt, same frame
number, therefore the same world -- and overrides only the three B3AfxInputs
through B3_AFX_FORCE_MPH / _BOOST / _DIVISOR.  The reference leg is the same
frame again with B3_AFX_BLUR=0.  Any pixel that differs between a leg and the
reference differs BECAUSE OF THE BLUR, and nothing else.

THE METRICS, all against that reference:

  dE      mean |delta| in 0..255 levels over the whole frame.  "How much of
          the picture did the effect touch."  Under ~1.5 a viewer will not
          notice at all; this is the number that was ~1 at 90 mph before the
          2026-08-24 retune.
  dE_out  the same, restricted to the outer annulus (normalised radius > 0.5).
          The radial mask deliberately keeps the centre sharp, so the frame-
          wide figure understates the effect; this is where it lives.
  radial  mean |dI/dr| as a FRACTION of the reference's.  A radial smear
          destroys gradients along the radius and leaves gradients across it
          alone, so this is the metric that is specifically about smear rather
          than about brightness: 1.00 is "no smear", 0.80 is "a fifth of the
          radial detail is gone".
  tang    mean |dI/dtangent|, same normalisation.  The control: an honest
          radial blur moves `radial` much further than `tang`.  If both fall
          together the pass is just blurring, and if both RISE the pass is
          only adding light.

Usage:
    python3 tools/afx_sweep.py --out build/afx_strips/before
    python3 tools/afx_sweep.py --out build/afx_strips/after  --label after

Every run is offscreen (SDL_VIDEODRIVER=offscreen, SDL_AUDIODRIVER=dummy):
the operator must see and hear nothing.
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The legs.  (tag, mph, boost, human label)
LEGS = [
    ("off",   None, None, "blur OFF (reference)"),
    ("060",   60.0, 0.0,  "60 mph"),
    ("090",   90.0, 0.0,  "90 mph"),
    ("120",  120.0, 0.0,  "120 mph"),
    ("150",  150.0, 0.0,  "150 mph"),
    ("boost", 150.0, 1.0, "150 mph + boost"),
]

TRACK = "US_C3_V1"
RES = "1280x720"
SHOT_FRAME = "400"


def run_leg(tag, mph, boost, outdir, extra_env):
    """Render one pinned frame.  Returns the .bmp path."""
    bmp = os.path.join(outdir, "afx_%s.bmp" % tag)
    env = dict(os.environ)
    # OFFSCREEN FIRST, before anything else can matter.
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": TRACK,
        "B3_RES": RES,
        "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_SHOT": bmp,
        "B3_SHOT_FRAME": SHOT_FRAME,
        # the chain's own state must not drift between legs
        "B3_AFX": "1",
    })
    env.update(extra_env)
    if mph is None:
        env["B3_AFX_BLUR"] = "0"          # the reference: blur off, rest on
    else:
        env["B3_AFX_FORCE_MPH"] = "%.3f" % mph
        env["B3_AFX_FORCE_BOOST"] = "%.3f" % boost
    if os.path.exists(bmp):
        os.remove(bmp)
    p = subprocess.run(["timeout", "600", os.path.join(_root, "burnout3")],
                       cwd=_root, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    log = p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")
    for line in log.splitlines():
        if line.startswith("[afx]") or "postfx path" in line:
            print("      | " + line.strip())
    if not os.path.exists(bmp):
        print("      ! no frame written for leg %s" % tag)
        print(log[-2000:])
        return None
    return bmp


def load(bmp):
    a = np.asarray(Image.open(bmp).convert("RGB")).astype(np.float64)
    return a


def polar_grads(g):
    """|dI/dr| and |dI/dtangent| per pixel, for a luma plane `g`."""
    h, w = g.shape
    yy, xx = np.mgrid[0:h, 0:w]
    dx = (xx - (w - 1) / 2.0) / ((w - 1) / 2.0)
    dy = (yy - (h - 1) / 2.0) / ((h - 1) / 2.0)
    r = np.hypot(dx, dy)
    r_safe = np.where(r < 1e-6, 1e-6, r)
    ux, uy = dx / r_safe, dy / r_safe          # unit radial
    gx = np.zeros_like(g)
    gy = np.zeros_like(g)
    gx[:, 1:-1] = (g[:, 2:] - g[:, :-2]) * 0.5
    gy[1:-1, :] = (g[2:, :] - g[:-2, :]) * 0.5
    rad = np.abs(gx * ux + gy * uy)            # along the radius
    tan = np.abs(-gx * uy + gy * ux)           # across it
    return rad, tan, r


def pin_ok(ref, img):
    """Did this leg render the SAME WORLD as the reference?

    B3_SHOT_FRAME pins a RENDERED FRAME NUMBER, and the frames before the
    green light are not a fixed count -- a cold asset cache spends a different
    number of them loading, so frame 400 can land at a different race time
    between two runs of the same binary.  When that happens the leg is a
    photograph of somewhere else and every number computed from it is
    meaningless.  It is not hypothetical: it silently corrupted two legs of a
    sweep here before this guard existed, and the only reason it was caught is
    that the frames were looked at.

    The check needs no instrumentation, because the effect under measurement
    supplies it: the radial mask holds the mix weight at exactly 0 inside
    B3_BLUR_MASK_R0, so the centre of the frame is bit-identical across legs
    IF AND ONLY IF the world is.  (True while the divisor is 1, which is the
    speed sweep; the crash legs desaturate globally and must not use this.)
    """
    h, w, _ = ref.shape
    rr = int(min(h, w) * 0.06)
    a = ref[h // 2 - rr:h // 2 + rr, w // 2 - rr:w // 2 + rr]
    b = img[h // 2 - rr:h // 2 + rr, w // 2 - rr:w // 2 + rr]
    return float(np.abs(a - b).max())


def measure(ref, img):
    d = np.abs(img - ref)
    dE = d.mean()
    lref = ref @ (0.299, 0.587, 0.114)
    limg = img @ (0.299, 0.587, 0.114)
    rad_r, tan_r, r = polar_grads(lref)
    rad_i, tan_i, _ = polar_grads(limg)
    out = r > 0.5
    dE_out = d[out].mean()
    # normalised over the annulus, which is where the mask lets the blur act
    radial = rad_i[out].mean() / max(rad_r[out].mean(), 1e-9)
    tang = tan_i[out].mean() / max(tan_r[out].mean(), 1e-9)
    return dE, dE_out, radial, tang


def strip(paths, labels, notes, dest, title):
    """Side-by-side contact strip, downscaled, with a caption under each."""
    ims = [Image.open(p).convert("RGB") for p in paths]
    tw = 420
    th = int(round(tw * ims[0].height / ims[0].width))
    ims = [im.resize((tw, th), Image.LANCZOS) for im in ims]
    cap = 46
    pad = 8
    cols = len(ims)
    W = cols * tw + (cols + 1) * pad
    H = th + cap + 3 * pad + 26
    sheet = Image.new("RGB", (W, H), (18, 18, 20))
    d = ImageDraw.Draw(sheet)
    d.text((pad, pad), title, fill=(235, 235, 240))
    for i, im in enumerate(ims):
        x = pad + i * (tw + pad)
        y = 26 + pad
        sheet.paste(im, (x, y))
        d.text((x + 2, y + th + 4), labels[i], fill=(240, 240, 245))
        d.text((x + 2, y + th + 18), notes[i], fill=(160, 170, 185))
    sheet.save(dest)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--label", default="")
    args = ap.parse_args()
    outdir = os.path.abspath(args.out)
    os.makedirs(outdir, exist_ok=True)

    print("afx_sweep -> %s" % outdir)
    print("  pinned frame %s of %s at %s, offscreen" % (SHOT_FRAME, TRACK, RES))

    # THE WARM-UP, and it is not optional.  The first run of a batch pays for
    # whatever the asset cache is missing, which costs it a different number of
    # pre-race frames than every run after it -- so leg 1 lands at a different
    # race time than legs 2..6 and the reference frame is a photograph of
    # somewhere else.  That is exactly the failure pin_ok() detects, and the
    # cheapest way to not have it is to spend one throwaway run first.
    print("  warm-up (discarded)")
    run_leg("warmup", None, None, outdir, {})

    shots = {}
    for tag, mph, boost, label in LEGS:
        print("  leg %-6s %s" % (tag, label))
        bmp = run_leg(tag, mph, boost, outdir, {})
        if bmp is None:
            return 1
        shots[tag] = bmp

    ref = load(shots["off"])
    rows = []
    bad = []
    print("\n  %-18s %7s %8s %8s %8s %6s" %
          ("leg", "dE", "dE_out", "radial", "tang", "pin"))
    print("  " + "-" * 60)
    for tag, mph, boost, label in LEGS:
        if tag == "off":
            rows.append((label, 0.0, 0.0, 1.0, 1.0))
            print("  %-18s %7.2f %8.2f %8.3f %8.3f %6s"
                  % (label, 0, 0, 1, 1, "ref"))
            continue
        img = load(shots[tag])
        pin = pin_ok(ref, img)
        dE, dE_out, radial, tang = measure(ref, img)
        rows.append((label, dE, dE_out, radial, tang))
        print("  %-18s %7.2f %8.2f %8.3f %8.3f %6.0f%s"
              % (label, dE, dE_out, radial, tang, pin,
                 "  <-- WORLD DIVERGED" if pin > 2.0 else ""))
        if pin > 2.0:
            bad.append(label)
    if bad:
        print("\n  PIN FAILED on %d leg(s): %s" % (len(bad), ", ".join(bad)))
        print("  frame %s landed at a different race time -- the numbers above"
              % SHOT_FRAME)
        print("  are NOT comparable. Re-run with a warm asset cache.")

    title = "Burnout 3 aftereffects -- speed blur sweep"
    if args.label:
        title += "  [%s]" % args.label
    title += "   (pinned frame %s, %s, %s)" % (SHOT_FRAME, TRACK, RES)
    notes = []
    for (label, dE, dE_out, radial, tang) in rows:
        if label.endswith("(reference)"):
            notes.append("--")
        else:
            notes.append("dE %.1f  outer %.1f  radial %.2f"
                         % (dE, dE_out, radial))
    dest = os.path.join(outdir, "strip.png")
    strip([shots[t] for t, _, _, _ in LEGS],
          [l for (_, _, _, l) in LEGS], notes, dest, title)
    print("\n  strip -> %s" % dest)

    with open(os.path.join(outdir, "metrics.txt"), "w") as f:
        f.write("leg\tdE\tdE_out\tradial\ttang\n")
        for (label, dE, dE_out, radial, tang) in rows:
            f.write("%s\t%.3f\t%.3f\t%.4f\t%.4f\n"
                    % (label, dE, dE_out, radial, tang))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
