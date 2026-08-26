#!/usr/bin/env python3
"""SHINE-HOT: re-derive the car-gloss ceiling from the retail reference frames.

tools/validate_car_shine.py asserts that the port's car paint stays inside a
band.  This is where that band comes from, so it can be checked and re-derived
rather than taken on trust.

It reads the nine xemu captures in `REFERENCE IMAGES/` -- retail Burnout 3,
chase cam, the same `xenier` / `MASTROIANNI` / `RUGERIERO` car that appears in
the user's own build/debug_dump_080.bmp -- segments the car out of each, and
prints the four statistics the validator checks.  Pass any number of extra
image paths to measure them the same way (build/debug_dump_080.bmp is the
interesting one); their kernel is scaled by the resolution ratio so the
morphology means the same thing at 2048x1536 as at 640x480.

The segmentation is a morphological CLOSING of the red-paint mask followed by a
hole fill and a largest-component pick.  Closing a mask of the car's red panels
pulls in everything they enclose -- the windows, the badges, the highlights --
so the silhouette contains exactly the surfaces the gloss question is about,
without needing a hand-drawn outline.  It is only used on the references; the
port's own runs get an exact mask from the module's ENVDBG hook instead.
"""
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, "REFERENCE IMAGES")

# the chase-cam car box in 640x480 reference coordinates, scaled per image
BOX = (240, 410, 220, 430)     # y0, y1, x0, x1
CLOSE_K = 9                    # at 640x480
RED_MARGIN = 25                # R - max(G, B) that counts as "red paint"


def lum(a):
    """the SAME luma tools/validate_car_shine.py uses -- Rec.601."""
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def sat(px):
    mx, mn = px.max(-1), px.min(-1)
    return np.where(mx > 0, (mx - mn) / np.maximum(mx, 1.0), 0.0)


def silhouette(a):
    h, w = a.shape[:2]
    s = w / 640.0
    y0, y1, x0, x1 = (int(round(v * s)) for v in BOX)
    box = np.zeros((h, w), bool)
    box[y0:y1, x0:x1] = True
    red = ((a[..., 0] - np.maximum(a[..., 1], a[..., 2])) > RED_MARGIN) & box
    k = max(3, int(round(CLOSE_K * s)))
    m = ndimage.binary_fill_holes(
        ndimage.binary_closing(red, structure=np.ones((k, k))))
    lab, n = ndimage.label(m)
    if n == 0:
        return None
    sizes = ndimage.sum(m, lab, range(1, n + 1))
    return lab == (1 + int(np.argmax(sizes)))


def measure(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(np.float32)
    m = silhouette(a)
    if m is None or m.sum() < 3000:
        return None
    px, L = a[m], lum(a[m])
    return dict(
        n=int(m.sum()),
        median=float(np.percentile(L, 50)),
        peak=float(np.percentile(L, 99.5)),
        clip=100.0 * float((px >= 250).all(-1).mean()),
        veil=100.0 * float(((L > 200) & (sat(px) < 0.25)).mean()),
        sky=float(np.percentile(lum(a[:a.shape[0] // 3]), 99.9)),
    )


# =========================================================================
# SHINE-SHAPE (2026-08-21): the STRUCTURE bands.
#
# The nine frames above are all the same red car; the bands in
# validate_car_shine.py's SHINE-SHAPE block also need DARK paint, which is
# where a reflection is most of what you see.  `--structure` measures the
# 2026-08-21 captures -- three rear-chase frames of the black muscle car on the
# mountain track and two of the red car in the city -- for the three statistics
# that block asserts.
#
# The black car cannot be segmented the way the red one is: there is no hue to
# threshold on, and a darkness threshold runs straight into the road's own
# shadow.  Its silhouette is therefore HAND-TRACED, once per frame, in the
# capture's own pixel coordinates, and `--structure --overlay <dir>` writes the
# mask painted over the frame so the tracing can be checked rather than
# believed.  The red frames keep the red-paint closing above.
#
# The roof/flank split here is a BOUNDING-BOX one -- upper-middle against the
# outer edges -- because a photograph carries no normals.  It is a LOWER BOUND
# on the geometric split the port is measured with: both of its bands mix
# up-facing and side-facing surfaces (the box's roof band on this car is partly
# rear window; its flank band is partly shoulder crown), so a split that knows
# the normals must separate at least as much.
STRUCT_REFS = [
    # (path, kind, hand-traced polygon in the capture's own pixels)
    ("xemu-2026-08-21-19-33-01.png", "poly",
     [(1430, 1258), (1540, 1245), (1730, 1245), (1850, 1262), (1940, 1320),
      (2025, 1405), (2064, 1475), (2072, 1620), (2040, 1668), (1940, 1715),
      (1800, 1735), (1540, 1735), (1390, 1715), (1300, 1668), (1258, 1600),
      (1248, 1480), (1282, 1390), (1350, 1310)]),
    ("xemu-2026-08-21-19-32-57.png", "poly",
     [(1450, 1278), (1650, 1266), (1830, 1282), (1950, 1334), (2050, 1430),
      (2106, 1530), (2114, 1650), (2090, 1730), (2010, 1810), (1810, 1850),
      (1510, 1854), (1330, 1820), (1240, 1750), (1194, 1650), (1198, 1540),
      (1270, 1430), (1360, 1340)]),
    ("xemu-2026-08-21-19-32-44.png", "poly",
     [(1480, 1270), (1660, 1254), (1840, 1274), (1960, 1330), (2054, 1434),
      (2094, 1542), (2098, 1666), (2070, 1750), (1990, 1830), (1790, 1870),
      (1500, 1870), (1330, 1834), (1240, 1760), (1190, 1654), (1198, 1534),
      (1274, 1422), (1374, 1330)]),
    ("xemu-2026-08-21-15-29-33.png", "red", (1220, 1900, 1180, 2080)),
    ("xemu-2026-08-21-12-24-05.png", "red", (1150, 1950, 1100, 2100)),
]
# The reference captures are the game's own artwork and are NOT shipped here:
# point $B3_SHINE_REFDIR at your own folder of xemu frames.
STRUCT_DIR = os.environ.get("B3_SHINE_REFDIR", "REFERENCE IMAGES")


def bbox_bands(mask):
    """upper-middle vs outer-edge bands of the silhouette's bounding box."""
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    h, w = y1 - y0 + 1, x1 - x0 + 1
    roof = np.zeros_like(mask)
    roof[y0:y0 + int(0.45 * h), x0 + int(0.28 * w):x0 + int(0.72 * w)] = True
    flank = np.zeros_like(mask)
    flank[y0 + int(0.20 * h):y0 + int(0.80 * h), x0:x0 + int(0.15 * w)] = True
    flank[y0 + int(0.20 * h):y0 + int(0.80 * h),
          x1 - int(0.15 * w):x1] = True
    return roof & mask, flank & mask


def glint(L, mask):
    """validate_car_shine.py's glint(), verbatim in behaviour."""
    v = L[mask]
    n = max(20, int(round(0.001 * v.size)))
    t = np.partition(v, -n)[-n]
    g = mask & (L >= t)
    ys, xs = np.nonzero(g)
    ys2, xs2 = np.nonzero(mask)
    diag = np.hypot(ys2.max() - ys2.min(), xs2.max() - xs2.min())
    rg = np.sqrt(((ys - ys.mean()) ** 2 + (xs - xs.mean()) ** 2).mean())
    return float(rg / max(diag, 1.0)), float(g.sum()) / float(n)


def structure_main(overlay=None):
    from PIL import ImageDraw
    print("%-26s %6s %6s %7s %8s %8s %7s %7s"
          % ("frame", "p5", "p50", "p99.5", "log-std", "roof/fl", "glint",
             "plat"))
    rows = []
    for name, kind, spec in STRUCT_REFS:
        path = os.path.join(STRUCT_DIR, name)
        if not os.path.exists(path):
            print("%-26s  (not found under %s)" % (name[-12:], STRUCT_DIR))
            continue
        a = np.asarray(Image.open(path).convert("RGB")).astype(np.float32)
        if kind == "poly":
            im = Image.new("L", (a.shape[1], a.shape[0]), 0)
            ImageDraw.Draw(im).polygon(spec, fill=255)
            m = np.asarray(im) > 0
        else:
            y0, y1, x0, x1 = spec
            box = np.zeros(a.shape[:2], bool)
            box[y0:y1, x0:x1] = True
            red = ((a[..., 0] - np.maximum(a[..., 1], a[..., 2]))
                   > RED_MARGIN) & box
            k = max(3, int(round(CLOSE_K * a.shape[1] / 640.0)))
            mm = ndimage.binary_fill_holes(
                ndimage.binary_closing(red, structure=np.ones((k, k))))
            lab, n = ndimage.label(mm)
            if n == 0:
                continue
            m = lab == 1 + int(np.argmax(ndimage.sum(mm, lab,
                                                     range(1, n + 1))))
        if overlay:
            os.makedirs(overlay, exist_ok=True)
            ov = a.copy()
            ov[..., 1] = np.where(m, np.minimum(255, ov[..., 1] + 90),
                                  ov[..., 1])
            ys, xs = np.nonzero(m)
            Image.fromarray(ov.astype("uint8")).crop(
                (max(0, xs.min() - 60), max(0, ys.min() - 60),
                 min(a.shape[1], xs.max() + 60),
                 min(a.shape[0], ys.max() + 60))).save(
                os.path.join(overlay, "mask_" + name))
        L = lum(a)
        body = L[m]
        roof, flank = bbox_bands(m)
        rg, plat = glint(L, m)
        r = dict(p5=np.percentile(body, 5), p50=np.percentile(body, 50),
                 p995=np.percentile(body, 99.5),
                 ls=float(np.log10(np.maximum(body, 1.0)).std()),
                 rf=float(L[roof].mean() / max(L[flank].mean(), 1e-6)),
                 rg=rg, plat=plat)
        rows.append(r)
        print("%-26s %6.1f %6.1f %7.1f %8.3f %8.2f %7.3f %7.1f"
              % (name[-12:], r["p5"], r["p50"], r["p995"], r["ls"], r["rf"],
                 r["rg"], r["plat"]))
    if not rows:
        return 1
    def rng(k):
        v = [r[k] for r in rows]
        return min(v), max(v)
    print("\nRETAIL STRUCTURE, %d rear-chase frames:" % len(rows))
    for k, lab in (("p5", "p5 (blacks)"), ("p995", "p99.5 (peak)"),
                   ("ls", "log-std"), ("rf", "roof/flank (bbox)"),
                   ("rg", "glint rg/diag"), ("plat", "glint population")):
        lo, hi = rng(k)
        print("  %-20s %8.3f .. %8.3f" % (lab, lo, hi))
    print("\n=> validate_car_shine.py: MIN_ROOF_FLANK_LIT = retail's minimum,"
          "\n   MAX_DARK_P5 / MIN_LOG_STD bracket the dark-paint frames,"
          "\n   MAX_GLINT_RG / MAX_GLINT_PLATEAU sit above retail's worst.")
    return 0


def main():
    if "--structure" in sys.argv:
        ov = None
        if "--overlay" in sys.argv:
            ov = sys.argv[sys.argv.index("--overlay") + 1]
        return structure_main(ov)
    refs = sorted(os.path.join(REF, f) for f in os.listdir(REF)
                  if f.lower().endswith((".png", ".bmp", ".jpg")))
    rows = []
    print("%-34s %7s %8s %7s %7s %9s %7s"
          % ("frame", "px", "med", "p99.5", "clip%", "veil%", "sky"))
    for p in refs + sys.argv[1:]:
        r = measure(p)
        tag = os.path.basename(p)
        if r is None:
            print("%-34s  (no car silhouette found)" % tag[:34])
            continue
        print("%-34s %7d %8.1f %7.1f %7.2f %9.2f %7.1f"
              % (tag[:34], r["n"], r["median"], r["peak"], r["clip"],
                 r["veil"], r["sky"]))
        if p in refs:
            rows.append(r)
    if not rows:
        return 1
    pk = np.array([r["peak"] for r in rows])
    ratio = np.array([r["peak"] / r["sky"] for r in rows])
    print("\nRETAIL, %d frames (all lighting):" % len(rows))
    print("  peak p99.5      %.1f .. %.1f" % (pk.min(), pk.max()))
    print("  clip%%           max %.2f" % max(r["clip"] for r in rows))
    print("  veil%%           max %.2f" % max(r["veil"] for r in rows))
    print("  car/sky peak    max %.3f   (%d/%d frames below 1.0)"
          % (ratio.max(), int((ratio < 1.0).sum()), len(rows)))
    print("\n=> validate_car_shine.py: PEAK_LO/PEAK_HI bracket the daylight"
          " frames,\n   MAX_CLIP_PCT / MAX_VEIL_PCT sit ~3x retail's worst,"
          " MAX_CAR_SKY_RATIO = 1.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
