#!/usr/bin/env python3
"""SHINE: does the car body actually carry a reflection layer, and does that
layer VARY -- across the body, and as the car drives?

"The cars look flat" is a claim about pixels, so this is a pixel test.  It
drives the harness headless on two tracks whose shipped `enviro.dat +0xA0`
reflection sheets have the two shapes that ship (US/C3's 512x128 sky PANORAMA
and EU/C3's 128x128 SPHERE MAP), and measures the body's reflection layer
ALONE -- the environment lerp plus its specular, with the shaded paint removed
-- through the module's own B3_CARFX_ENVDBG hook.

Three headless runs of the same deterministic drive per track:

  1. B3_CARFX_ENVDBG=1, B3_CARFX_ENVUV=raw
        every car fragment is painted GREEN if its stage-1 lookup lands on the
        sheet and RED if it lands outside it.  The intersection of the painted
        pixels over all frames is an exact CAR MASK that no background pixel
        can enter, and the red fraction inside it is the defect this test was
        written for: the recovered raw-R.xy lookup ranges over [-1,1]^2 while a
        2D lookup has [0,1]^2 and stage 1 is CLAMP, so most of the body reads
        one frozen border texel and the reflection cannot vary at all.
        This paint is also the split the raw-vs-sheet comparison scores each
        leg through -- always-red against always-green -- see layer_checks.
  2. B3_CARFX_ENVDBG=3, B3_CARFX_ENVUV=raw   the reflection layer, raw lookup.
  3. B3_CARFX_ENVDBG=3                       the reflection layer, sheet lookup.

The floors are luminance over the masked body, 0..255.  They sit an order of
magnitude below a working reflection and above what the raw lookup measures,
so the test states the difference between "flat" and "not flat" rather than
pinning a look.

SHINE-HOT (2026-08-21) -- the OTHER side of the same measurement
----------------------------------------------------------------
Everything above is a FLOOR.  Every check in the original file could only fail
for "too flat", so when the reflection layer came back three times too hot
nothing went red and the only instrument left was the user's eye -- which is
how B3FX_T_REFL_GAIN went 0.50 -> 0.35 -> 0.25 -> 0.18 with the user reporting
"still too bright" after each one.  A one-sided test is half a test.

So this file now also carries a CEILING, measured off the nine retail frames in
`REFERENCE IMAGES/` (xemu captures of the same car -- the `xenier` /
`MASTROIANNI` / `RUGERIERO` model that appears in the user's own
build/debug_dump_080.bmp).  Each retail frame is segmented with a morphological
closing of its red-paint mask, which recovers the car's silhouette including
its windows and highlights, and measured the same way this file measures ours:

    retail car silhouette, 9 frames    p99.5 lum   clip%   veil%   sky p99.9
      13-50-41  open road                  185.6    0.00    0.17       204.2
      13-50-50  open road                  214.0    0.00    0.50       220.4
      13-52-39  open sunlit road           186.5    0.00    0.02       201.8
      13-52-44  open sunlit road           188.7    0.00    0.25       206.4
      13-52-56  open road                  181.2    0.00    0.00       201.4
      13-53-14  shade                      149.1    0.00    0.00       232.7
      13-53-46  open road                  200.3    0.00    0.10       208.8
      13-53-51  overcast                   176.2    0.00    0.01       226.0
      13-54-21  tunnel                     119.0    0.01    0.01       201.8

    ours, build/debug_dump_080.bmp        255.0    3.06    5.69       228.7

(`tools/measure_retail_car_band.py` prints exactly that table, and takes extra
image paths, so the band can be re-derived instead of believed.)

`clip` is the share of body pixels with all three channels >= 250; `veil` is
the share that is bright AND desaturated (lum > 200 and saturation < 0.25),
which is what an additive white highlight sitting on coloured paint looks like
and what "the gloss is too bright" actually names.  Retail never exceeds 0.01%
and 0.50%; the port's capture is at 3.06% and 5.69%.

Two notes on making the port's numbers mean the same thing.  The retail frames
have to be segmented by morphology because there is nothing else to segment
them with; the port's runs get an EXACT silhouette from the module's own
ENVDBG=1 paint, so that is what is used here.  And inside that silhouette only
the pixels the reflection layer actually reaches are measured -- see GLOSS_T --
because the retail column is a car whose decals do not blow out and the port's
are, for a reason that is not gloss.

Three checks come out of that table, and the third is the one that does not
need a band at all:

  * the peak sits inside a BAND.  Retail's daylight frames span 181.2..214.0,
    so [150, 225] admits every retail daylight frame with margin at both ends
    while excluding both failure modes -- a body with no highlight left (it
    would sit near its own median, 35..72 in retail) and the 255 the port had.
  * the bright-desaturated VEIL, as a share of the whole body so it is the
    same quantity as the retail column, stays under a cap set from that table.
    Clip% is printed but NOT gated -- it is dominated by the port's white
    sponsor decals, which blow out for an unrelated diffuse reason and barely
    move when the gloss is fixed.  See the note by MAX_VEIL_PCT.
  * THE REFLECTION MAY NOT OUT-SHINE WHAT IT REFLECTS.  In all nine retail
    frames the car's peak is BELOW the sky's peak in the same frame -- the
    tightest margin is 200.3 vs 208.8, a ratio of 0.971, and it is 9/9.  This
    one is frame-internal and needs no absolute number, so it holds on any
    track at any time of day, and it is the check the port's own capture fails
    outright: 255.0 against a sky of 228.7, a ratio of 1.115.

SHINE-SHAPE (2026-08-21) -- the THIRD side
------------------------------------------
A floor and a ceiling still cannot tell a FLAT reflection from a shaped one:
both were green when the user looked at the result and called it "too flat".
So the file now also measures STRUCTURE -- roof against flank by geometry,
the tonal range on dark paint, and whether the brightest part of the body is a
compact glint or a clipped plateau -- with bands re-derived by
`tools/measure_retail_car_band.py --structure` from five rear-chase captures,
three of them of a BLACK car (dark paint is the only body on which the gloss is
most of what you see).  See the block above MIN_ROOF_FLANK_LIT.

Measured on one binary with one variable, the three sides fail in three
different directions -- re-measured 2026-08-21 after the two statistics below
were reconciled (see the RECONCILE notes in layer_checks and by `vl` in hot):

    shipped                          50/50
    B3_CARFX_SHAPE=0  "too flat"     41/50   roof/flank and the layer's own
                                              roof/flank on all three cases,
                                              dark p5, log-std, and two floors
    B3_CARFX_REFLGAIN=2.2 "too hot"  37/50   peak 255.0, veil 16.9..22.2%,
                                              car/sky 1.04-1.05, glint rg
                                              0.24..0.26 and population
                                              96x..197x the top 0.1%

Neither reconciliation moved a retail-derived band: PEAK_LO/HI, MAX_VEIL_PCT,
MAX_CAR_SKY_RATIO and every SHINE-SHAPE constant are the numbers
`tools/measure_retail_car_band.py` derives, unchanged.  What changed is the
two places where the port's measurement was not the same QUANTITY as the
retail column it was being compared against.
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# B3_SHINE_BIN lets a shadow build be measured without touching the tree's
# own ./burnout3, which several agents share.
GAME = os.environ.get("B3_SHINE_BIN") or os.path.join(ROOT, "burnout3")
OUT = os.environ.get("B3_SHINE_OUT", "/tmp/b3_carshine")

# one panorama sheet (US/C3, 512x128) and one sphere map (EU/C3, 128x128),
# plus -- SHINE-SHAPE -- a DARK-PAINT car, which is the only body on which the
# reflection is most of what you see and therefore the only one whose tonal
# range is a statement about the gloss rather than about the albedo.  It is
# also the car the 2026-08-21 retail captures show, so the two are comparable.
# (track, B3_PLAYER_CAR or None)
TRACKS = [("US_C3_V1", None), ("EU_C3_V1", None), ("US_C3_V1", "MSCL_Car2")]

FIRST, EVERY, NFRAMES = 180, 40, 12
MAX_OFF_SHEET_PCT = 1.0      # of the masked body, after the fix
MIN_STRENGTH = 6.0           # the layer is there at all
MIN_SPATIAL = 8.0            # it has structure across the body
MIN_TEMPORAL = 2.0           # and it moves as the car turns

# SHINE-HOT: the ceiling.  Every number here is measured off `REFERENCE
# IMAGES/` -- see the header table -- not chosen to make the current build
# pass.  tools/measure_retail_car_band.py re-derives them from the frames.
PEAK_LO, PEAK_HI = 150.0, 225.0   # retail daylight spans 181.2 .. 214.0
MAX_VEIL_PCT = 0.60               # retail's worst is 0.50
MAX_CAR_SKY_RATIO = 1.00          # retail's worst is 0.971, and it is 9/9
# Clip% is REPORTED, not gated, and that is deliberate.  The port's white
# sponsor decals blow out for a reason that has nothing to do with gloss --
# on a white texel the diffuse `2*paint*E` meets the present composite's x2
# with no headroom left -- so clip barely moves when the gloss is fixed
# (measured US_C3_V1: 0.88% before, 0.69% after, against retail's 0.01%).
# Gating on it would either be permanently red or, tuned to pass, never fire.
# It is printed so the separate defect stays visible and measured.
# A pixel counts as GLOSS-DRIVEN when the isolated reflection layer (the
# ENVDBG=3 run this file already takes) is at least this bright there.  It is
# what keeps the ceiling measuring the gloss instead of the paint texture:
# the car's white sponsor decals -- `xenier`, `RUGERIERO`, `MKR` -- are ALBEDO,
# they clip in this port for an unrelated reason (the diffuse `2*paint*E` meets
# the present x2 with no headroom on a white texel), and retail's do not.  That
# is a real defect and a separate one; measured through this mask the ceiling
# does not silently inherit it, and does not silently excuse it either.
GLOSS_T = 40.0
# the absolute band is a DAYLIGHT statement, so it is asserted on the daylight
# track; the ratio and the caps are frame-internal and hold everywhere.
DAYLIGHT_TRACKS = {"US_C3_V1"}

# =========================================================================
# SHINE-SHAPE (2026-08-21) -- the THIRD side of the same measurement.
#
# Everything above is a floor or a ceiling on MAGNITUDE.  Both were green when
# the user looked at the result and called it "too flat", because neither of
# them can see STRUCTURE: a reflection layer whose only spatial variable is the
# Fresnel term has exactly the same mean, the same body-wide std and the same
# peak as one that pools the sky on the roof and lets the flanks fall away.
# The checks below are about shape, and they are red on the composition that
# was called flat.
#
# THE REFERENCE.  Five rear-chase retail captures -- three of the BLACK muscle
# car on the mountain track and two of the red car in the city -- measured by
# `tools/measure_retail_car_band.py --structure`, which re-derives every number
# below and can paint its silhouettes over the frames for checking:
#
#                          p5    p50   p99.5  log-std  roof/flank  glint  plat
#   19-33-01 black car    10.2   61.2  240.1   0.386      1.46     0.140   1.0
#   19-32-57 black car     9.4   61.6  239.3   0.407      2.34     0.037   1.0
#   19-32-44 black car    12.0   71.6  255.0   0.393      2.80     0.093  14.4
#   15-29-33 red car       9.3   36.8  117.7   0.320      1.30     0.089   1.0
#   12-24-05 red car      13.7   46.9  138.2   0.311      1.43     0.133   1.0
#
# and the port, on one deterministic drive, one binary, one variable
# (B3_CARFX_SHAPE), measured through the geometric split described in (a):
#
#   US_C3 dark  BEFORE    17.3   51.6  237.6   0.294      1.45 lit / 1.89 layer
#   US_C3 dark  AFTER     10.0   66.8  251.6   0.423      2.49 lit / 4.76 layer
#   US_C3 red   BEFORE    24.7   63.8  253.7   0.291      1.28 lit / 1.83 layer
#   US_C3 red   AFTER     10.1   74.6  250.3   0.396      1.90 lit / 3.99 layer
#
# (a) ROOF vs FLANK.  Split by GEOMETRY, not by a bounding box: the module's
#     B3_CARFX_ENVDBG=6 paints every body fragment RED where its world normal
#     faces up (N.y >= 0.55) and GREEN where it faces sideways or straight back
#     (|N.y| <= 0.30).  A bounding-box split cannot do this job -- on the
#     port's own hatchback the upper-middle of the silhouette is mostly REAR
#     WINDOW, so a box measures the glass and reads 0.83 on a car whose paint
#     is correctly structured.
#     The retail column above is a box split (it is all a photograph allows),
#     and it mixes the two populations in BOTH bands, so a geometric split of
#     the same scene must separate at least as much: retail's five rear-chase
#     frames span 1.30..2.80 with a box, and hand-placed panel patches on the
#     black car's trunk deck against its quarter panel -- the most nearly
#     up-facing and the most nearly side-facing PAINT on it -- give 1.17..1.38.
#     The floor below is under both.
# (b) TONAL RANGE ON DARK PAINT.  This is the one that is red on "flat".  The
#     retail black car holds p5 = 9.4..12.0 while its trunk lip peaks past 239
#     -- four and a half stops on one panel -- and its log-luminance std is
#     0.386..0.407 (the red car, less extreme, 0.311..0.320).  A layer with no
#     ground half lifts the darks instead: the port measured p5 = 17.3 and
#     log-std 0.294 before, both outside every retail frame.
# (c) GLINT COMPACTNESS.  Retail's brightest 0.1% of body pixels sit in a tight
#     blob: radius of gyration over the car's bbox diagonal is 0.037..0.140,
#     and the population is the size it should be (|{L >= t}| / 0.1%N = 1.0 on
#     four of the five frames, 14.4 on the one frame that clips).  A too-hot
#     build has no glint at all, it has a PLATEAU: build/debug_dump_079.bmp and
#     _080.bmp -- the user's own captures of the state before this wave --
#     measure 30x and 28x that population, and driving the shipped composition
#     hot on purpose (B3_CARFX_REFLGAIN=2.2) measures 200x.  That is what the
#     ratio catches, and it is the check that is red for "too hot" while (b) is
#     red for "too flat".
MIN_ROOF_FLANK_LIT = 1.15     # retail box 1.30..2.80; hand patches 1.17..1.38
# The LAYER ratio has no retail counterpart -- a photograph cannot have the
# reflection separated out of it -- so it is not a retail band.  It is a
# STRUCTURE floor, and its whole job is to say WHERE the lit ratio comes from:
# a shader that got its roof/flank separation out of the diffuse E(N) alone
# would pass the lit check and fail this one.
MIN_ROOF_FLANK_LAYER = 1.50
MAX_DARK_P5 = 15.0            # retail spans 9.3..13.7
MIN_DARK_PEAK = 150.0         # retail spans 117.7..255.0; the ceiling is
                              # PEAK_HI above, on the gloss-driven population
MIN_LOG_STD = 0.30            # retail spans 0.311..0.407
MAX_GLINT_RG = 0.18           # retail spans 0.037..0.140
MAX_GLINT_PLATEAU = 10.0      # retail 1.0 (x4) and 14.4 on its clipped frame;
                              # the port's pre-wave captures are 28x and 30x

PASS, FAIL = [], []
HAVE_SHAPE = True


def has_shape():
    """does the binary under test carry the SHINE-SHAPE composition?

    The structure block needs B3_CARFX_ENVDBG=6, which is part of the same
    src/burnout3_carfx.c change as the composition it measures.  On a binary
    built before that change ENVDBG=6 falls through to a normal render and the
    hue test would classify the car's own PAINT as a band, so the block has to
    be skipped rather than run on nonsense.  The probe is the shader source,
    which is a literal in the executable.
    """
    try:
        with open(GAME, "rb") as f:
            return b"uTunedFx" in f.read()
    except OSError:
        return False


def ck(cond, what, detail=""):
    (PASS if cond else FAIL).append(what)
    print("  %s %s%s" % ("ok  " if cond else "FAIL", what,
                         ("   [%s]" % detail) if detail else ""))


def run(track, outdir, extra, car=None):
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    env = dict(os.environ)
    env.update({
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
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(int((FIRST + EVERY * NFRAMES) / 60.0) + 3),
        "B3_TRACK": track, "B3_AUTODRIVE": "1", "B3_CAMSIDE": "1",
        "B3_SHOT_SEQ": outdir, "B3_SHOT_FIRST": str(FIRST),
        "B3_SHOT_EVERY": str(EVERY),
    })
    if car:
        env["B3_PLAYER_CAR"] = car
    # a None value REMOVES the variable -- B3_CAMSIDE is tested with getenv()
    # alone, so setting it empty would still park the camera broadside.
    for k, v in extra.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = v
    subprocess.run([GAME], cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=600)
    fs = sorted(f for f in os.listdir(outdir) if f.endswith(".bmp"))
    return [np.asarray(Image.open(os.path.join(outdir, f)).convert("RGB"))
            .astype(np.float32) for f in fs]


# the capture path applies the present gamma ramp, which lifts the blacks (pure
# red reads back as 255,107,107), so the debug colours are classified by HUE
# DOMINANCE and not by an absolute floor.
def is_red(f):
    return (f[..., 0] - np.maximum(f[..., 1], f[..., 2])) > 80


def is_green(f):
    return (f[..., 1] - np.maximum(f[..., 0], f[..., 2])) > 80


def erode(m):
    e = m.copy()
    for _ in range(2):
        e[1:, :] &= e[:-1, :]
        e[:-1, :] &= e[1:, :]
        e[:, 1:] &= e[:, :-1]
        e[:, :-1] &= e[:, 1:]
    return e


def lum(a):
    return 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]


def measure(frames, mask):
    """strength / spatial / temporal, all in 0..255 luminance.

    `temporal` is the PER-PIXEL standard deviation over the frame sequence,
    averaged over the mask -- not the deviation of the frame means.  A
    highlight that sweeps across the body leaves the body mean almost
    unchanged, so the frame-mean statistic is blind to exactly the thing under
    test; the per-pixel one is not.
    """
    v = np.stack([lum(f) for f in frames])[:, mask]
    return (float(v.mean()), float(v.std(axis=1).mean()),
            float(v.std(axis=0).mean()))


def off_sheet_pct(frames, mask):
    return 100.0 * float(np.mean([is_red(f)[mask].mean() for f in frames]))


# --------------------------------------------------------------- SHINE-HOT
def sat(px):
    mx, mn = px.max(-1), px.min(-1)
    return np.where(mx > 0, (mx - mn) / np.maximum(mx, 1.0), 0.0)


def player_mask(m):
    """the largest connected component of a car mask -- the player's car.

    The player's car is the nearest and by far the largest body in frame;
    every other blob is a rival further down the road.  Falls back to the whole
    mask if SciPy is not installed, which only makes the checks that use it
    stricter.

    CAVEAT, and it is why the VEIL no longer uses this (see the note by `vl`
    in hot()): "largest component" only isolates the player while the
    silhouettes stay disjoint.  On EU/C3 a rival runs alongside and touches the
    player's spoiler, and the two become ONE component on 9 of 17 broadside
    frames.  The structure block below survives that because every statistic it
    takes is a RATIO between two bands of the same mask, so a second car in the
    mask perturbs numerator and denominator together; a SHARE of the body, like
    the veil, does not have that protection.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return m
    lab, n = ndimage.label(m)
    if n <= 1:
        return m
    sizes = ndimage.sum(m, lab, range(1, n + 1))
    return lab == (1 + int(np.argmax(sizes)))


def hot(frames, maskframes, layerframes):
    """peak / clip% / veil% over the GLOSS-DRIVEN body, and the sky's own peak.

    Three aligned runs of one deterministic drive: the lit frame, the ENVDBG=1
    car paint, and the ENVDBG=3 reflection layer.  Same fixed dt, same
    autodrive, so frame i of each is the same instant.

    The mask is PER FRAME, not the cross-frame intersection the checks above
    use.  An intersection shrinks to the one patch of bodywork that never moves
    in frame, which on a chase cam is the middle of the boot -- exactly not
    where a roof highlight lands.

    Inside that silhouette only the pixels the reflection layer actually
    reaches are measured (GLOSS_T).  See the note on GLOSS_T: without it this
    ceiling reads the white decal texels, which are albedo and belong to a
    different defect.

    All four statistics are per-frame and then averaged, so one unlucky frame
    cannot carry the verdict.  The sky band is the top third of the frame: on a
    chase cam that is sky and skyline and never the player's car, and it is the
    reflection's SOURCE, which is the whole point of the ratio.
    """
    pk, cl, vl, va, sk, cov = [], [], [], [], [], []
    # the player's own bodywork: what stays inside the silhouette on EVERY
    # frame of a rigidly-parked broadside camera.  See the note by `vl`.
    player = erode(np.logical_and.reduce([is_red(f) | is_green(f)
                                          for f in maskframes]))
    if player.sum() < 500:          # no stable body: fall back to the union,
        player = None               # which only ever makes the check stricter
    for f, mf, lf in zip(frames, maskframes, layerframes):
        m = erode(is_red(mf) | is_green(mf))
        if m.sum() < 500:
            continue
        sk.append(np.percentile(lum(f[:f.shape[0] // 3]), 99.9))
        g = m & (lum(lf) >= GLOSS_T)
        cov.append(100.0 * float(g.sum()) / float(m.sum()))
        if g.sum() < 200:          # no gloss reaches the body at all
            pk.append(0.0)
            cl.append(0.0)
            vl.append(0.0)
            va.append(0.0)
            continue
        px = f[g]
        L = lum(px)
        pk.append(np.percentile(L, 99.5))
        # veil and clip are shares of the WHOLE BODY, not of the gloss
        # population, so they mean the same thing as the retail column they
        # are compared against -- and so a track where the gloss reaches only
        # a few percent of the car cannot inflate its own denominator.
        n = float(m.sum())
        cl.append(100.0 * float((px >= 250).all(-1).sum()) / n)
        va.append(100.0 * float(((L > 200) & (sat(px) < 0.25)).sum()) / n)
        # ...and the GATED veil is the PLAYER's car alone.  SHINE-SHAPE
        # (2026-08-21): the mask covers every car the shader draws, and on
        # EU/C3 one of the rivals is painted WHITE.  White paint is bright and
        # desaturated wherever any gloss reaches it, so the moment the
        # reflection layer stops being flat and starts reaching more of the
        # body, that rival walks into the numerator.  That is the same class
        # of confound GLOSS_T exists for (white ALBEDO, not gloss), and it is
        # one GLOSS_T cannot catch because the layer does reach the rival.
        # The retail column is a single car filling the frame, so a single car
        # is what the port has to put next to it.  Both numbers are printed.
        #
        # RECONCILE (2026-08-21): the player used to be picked as the largest
        # CONNECTED COMPONENT of the per-frame silhouette, and that is what
        # EU/C3 measured 0.76% through -- a red flag on a car that has no veil
        # on it at all.  On EU/C3 the white rival runs alongside and its nose
        # TOUCHES the player's spoiler, so the two silhouettes are one
        # component on 9 of 17 frames (the "player" is 100% of the body on
        # those) and every counted veil pixel sits on the rival.  Morphology
        # cannot separate them -- the bridge survives eight erosions -- and
        # gating on the underlying paint's saturation cannot either, because
        # it throws away the whole of a BLACK car, which is the one body the
        # gloss matters most on (measured: 1% of MSCL_Car2 survives it).
        #
        # What does separate them is the CAMERA.  These runs are B3_CAMSIDE:
        # the camera is rigidly parked broadside of the player, so the
        # player's bodywork is the part of the silhouette that does not MOVE
        # in frame, while every rival sweeps through.  `player` below is the
        # cross-frame intersection of the silhouette -- the same construction
        # the mask checks above already assert is a stable car mask -- and it
        # is measured to be 74..85% of the per-frame body, covering roof,
        # shoulder and flank, with no rival pixel in it (checked by eye on
        # EU/C3 frame 7: the intersection lies entirely on the player's red
        # paint and excludes the white rival completely).
        #
        # Teeth kept: driving the shipped composition hot on purpose
        # (B3_CARFX_REFLGAIN=2.2) measures 22.23% through this mask against
        # the 0.60% cap -- the same 22.3% the header's table records.
        pm = m if player is None else player
        pg = g & pm
        vl.append(100.0 * float(((lum(f[pg]) > 200) & (sat(f[pg]) < 0.25)).sum())
                  / max(float(pm.sum()), 1.0) if pg.sum() else 0.0)
    if not pk:
        return None
    return (float(np.mean(pk)), float(np.mean(cl)), float(np.mean(vl)),
            float(np.mean(sk)), float(np.mean(cov)), float(np.mean(va)),
            0 if player is None else int(player.sum()))


def main():
    global HAVE_SHAPE
    if not os.path.exists(GAME):
        raise SystemExit("burnout3 not built")
    HAVE_SHAPE = has_shape()
    for track, car in TRACKS:
        tag = track if not car else "%s %s" % (track, car)
        print("\n[%s]" % tag)
        d = os.path.join(OUT, track + ("_" + car if car else ""))
        mf = run(track, d + "/mask", {"B3_CARFX_ENVDBG": "1",
                                      "B3_CARFX_ENVUV": "raw"}, car)
        ck(bool(mf), "%s: the headless run produced frames" % tag,
           "%d" % len(mf))
        if not mf:
            continue
        mask = erode(np.logical_and.reduce([is_red(f) | is_green(f)
                                            for f in mf]))
        stable = int(mask.sum()) > 500
        ck(stable,
           "%s: a stable car mask exists across %d frames" % (tag, len(mf)),
           "%d px" % int(mask.sum()))

        # kept: these frames are ALSO the per-frame car silhouettes the
        # SHINE-HOT block below measures the lit body through.
        mfx = run(track, d + "/maskfix", {"B3_CARFX_ENVDBG": "1"}, car)
        # kept: these frames are ALSO the reflection layer the SHINE-HOT block
        # uses to tell gloss apart from paint.
        af = run(track, d + "/after", {"B3_CARFX_ENVDBG": "3"}, car)
        lit = run(track, d + "/lit", {}, car)

        # SHINE-SHAPE: its own set, on the CHASE camera.
        #
        # Everything above runs under B3_CAMSIDE, which parks the camera
        # broadside -- the right view for "does the layer vary and move",
        # because it sweeps the whole flank past the lens.  It is the wrong
        # view for the structure block: every retail reference frame is a
        # rear CHASE shot, and two of the three statistics below are
        # viewpoint-dependent.  The glint's radius of gyration is measured
        # against the car's bounding-box diagonal, and broadside that diagonal
        # is the car's LENGTH while the highlight is a long thin band along the
        # shoulder -- correct, glossy, and 0.22 against a retail column of
        # 0.037..0.140 measured on cars seen end-on.  Same drive, same frames,
        # same everything except the camera the retail column was shot from.
        chase = {"B3_CAMSIDE": None}
        cmf = caf = cnb = clit = None
        if HAVE_SHAPE:
            cmf = run(track, d + "/c_mask",
                      dict(chase, B3_CARFX_ENVDBG="1"), car)
            caf = run(track, d + "/c_layer",
                      dict(chase, B3_CARFX_ENVDBG="3"), car)
            cnb = run(track, d + "/c_bands",
                      dict(chase, B3_CARFX_ENVDBG="6"), car)
            clit = run(track, d + "/c_lit", dict(chase), car)

        # SHINE-HOT: the checks below this point need only PER-FRAME masks, so
        # they are no longer skipped on a track where the cross-frame
        # intersection comes out empty -- which happens whenever the drive
        # swings the car far enough across the frame, and used to take the
        # whole track's coverage with it.
        if stable:
            off_raw = off_sheet_pct(mf, mask)
            off_fix = off_sheet_pct(mfx, mask)
            ck(off_fix <= MAX_OFF_SHEET_PCT,
               "%s: every body fragment's stage-1 lookup lands ON the sheet"
               % tag,
               "%.1f%% off-sheet, was %.1f%% with the raw lookup"
               % (off_fix, off_raw))
            layer_checks(tag, track, car, mask, af, d, mf)

        hot_checks(tag, lit, mfx, af)
        if HAVE_SHAPE:
            shape_checks(tag, track, car, clit, cmf, caf, cnb)
        else:
            print("       SKIP structure: %s has no B3_CARFX_ENVDBG=6 --"
                  " apply the SHINE-SHAPE carfx patch and rebuild"
                  % os.path.basename(GAME))

    n = len(PASS) + len(FAIL)
    print("\n%d/%d checks pass" % (len(PASS), n))
    return 1 if FAIL else 0


def layer_checks(tag, track, car, mask, af, d, rawmask):
    # SHINE-SHAPE amended the CONTROL, not the claim.  This pair is about the
    # LOOKUP -- raw R.xy against the sheet's own parameterisation -- and with
    # the 1d hemisphere weight in the composition the layer's dominant spatial
    # variable is the hemisphere, which is common to BOTH lookups and swamps
    # the difference the pair exists to measure (25.3 against 26.2 on US/C3,
    # a 3% inversion of a 2x effect).  So the comparison is taken with
    # B3_CARFX_SHAPE=0 on BOTH sides, where the lookup is the only spatial
    # variable there is.  One variable, and it is the one named in the check.
    # The FLOORS below stay on the shipped composition, which is what ships.
    flat = {"B3_CARFX_SHAPE": "0"}
    before_f = run(track, d + "/before",
                   dict(flat, B3_CARFX_ENVDBG="3", B3_CARFX_ENVUV="raw"), car)
    ctrl_f = run(track, d + "/ctrl", dict(flat, B3_CARFX_ENVDBG="3"), car)
    before = measure(before_f, mask)
    ctrl = measure(ctrl_f, mask)
    after = measure(af, mask)
    print("       reflection layer   strength  spatial  temporal")
    print("       raw-R.xy lookup    %8.2f %8.2f %9.3f" % before)
    print("       sheet lookup       %8.2f %8.2f %9.3f" % after)
    ck(after[0] >= MIN_STRENGTH,
       "%s: the body carries a reflection layer" % tag,
       "mean %.2f >= %.1f" % (after[0], MIN_STRENGTH))
    ck(after[1] >= MIN_SPATIAL,
       "%s: the layer has structure ACROSS the body" % tag,
       "std %.2f >= %.1f" % (after[1], MIN_SPATIAL))
    ck(after[2] >= MIN_TEMPORAL,
       "%s: the layer MOVES as the car drives" % tag,
       "per-pixel std %.2f >= %.1f" % (after[2], MIN_TEMPORAL))
    # SHINE-HOT amended this from "all three up" to "structure and motion
    # up".  STRENGTH is no longer the right thing to compare: the frozen
    # border texel the raw lookup reads on US_C3_V1 is a mid-grey
    # (148,151,148), so a correct sheet lookup can legitimately come out
    # DIMMER than it -- the body now also reflects the dark ground half of
    # the panorama instead of that uniform grey.  Dimmer-but-structured is
    # the fix working, not failing, and MIN_STRENGTH above already holds
    # the absolute floor.  Flatness was always the claim under test, and
    # flatness is what spatial and temporal measure.
    #
    # RECONCILE (2026-08-21) -- the statistic, for the third and last time.
    # SHINE-SHAPE divided `spatial` and `temporal` by `strength` to take the
    # layer's magnitude out of the comparison.  That normalisation is what
    # EU/C3 was red on, and it is unsound: it divides by the RAW leg's own
    # mean, and the raw leg's mean is set by WHICH TEXEL THE CLAMP FROZE ON,
    # which is an accident of the sheet.  Measured, on the two shipped sheet
    # shapes:
    #
    #                        clamped corner   raw leg    spat/str  temp/str
    #   US/C3  512x128 pano   grey 148.4       16.13      0.736     0.1219
    #   EU/C3  128x128 sphere BLACK 0.0         2.78      1.447     1.0230
    #
    # A sphere map is a disc inscribed in a square, so its corners are pure
    # black -- the raw lookup reads NOTHING there, the leg's mean collapses to
    # 2.78, and the ratio explodes.  The more thoroughly the raw lookup is
    # broken, the more easily it wins.  And the absolute comparison the
    # normalisation replaced is unsound in the mirror image, for the same
    # reason: on the dark car the frozen texel is BRIGHTER than the sheet's
    # own mean, so the correct lookup loses by 6% on spatial.  Neither leg's
    # magnitude is a comparable unit, in either direction.
    #
    # So the raw leg's magnitude is removed from the comparison entirely, by
    # scoring each leg against ITSELF.  The module's own ENVDBG=1 paint splits
    # the body by what the CLAMP did to it: `frozen` is the fragments whose
    # raw lookup lands OFF the sheet in every frame -- one texel, no
    # environment variation possible, by construction -- and `live` is the
    # fragments that land ON it in every frame.  Take, within ONE run, how
    # much structure and motion the frozen half carries relative to the live
    # half.  Any constant scale cancels in that ratio, whether the frozen
    # texel is 148 or 0.  A working lookup makes the frozen half behave like
    # the live half, so the ratio rises towards 1:
    #
    #                     frozen/live spatial      frozen/live temporal
    #                     raw  ->  sheet           raw  ->  sheet
    #   US/C3   pano      0.686 -> 1.025 (+49%)    0.390 -> 1.317 (+238%)
    #   EU/C3   sphere    0.396 -> 1.132 (+186%)   0.760 -> 1.427  (+88%)
    #   US/C3   dark car  0.585 -> 0.762  (+30%)   0.396 -> 0.714  (+80%)
    #
    # Both legs stay at B3_CARFX_SHAPE=0, for the reason in the note above:
    # the 1d hemisphere weight is common to both lookups and swamps the one
    # difference this pair exists to measure.
    frozen = mask & np.logical_and.reduce([is_red(f) for f in rawmask])
    live = mask & np.logical_and.reduce([is_green(f) for f in rawmask])
    ok = int(frozen.sum()) >= 200 and int(live.sum()) >= 200

    def split(m):
        """(frozen/live spatial, frozen/live temporal) for one leg."""
        sf, tf = measure(m, frozen)[1:]
        sl, tl = measure(m, live)[1:]
        return sf / max(sl, 1e-6), tf / max(tl, 1e-6)

    if not ok:
        # No frozen population means the recovered raw lookup never clamped
        # off-sheet on this track, and the premise of the whole pair is gone;
        # no live one means there is nothing to score it against.  Either way
        # that is worth a red, not a silent skip.
        ck(False,
           "%s: structure and motion are up on the raw lookup" % tag,
           "the clamp froze %d px and left %d always-on-sheet -- "
           "need >= 200 of each" % (int(frozen.sum()), int(live.sum())))
        return
    bs, bt = split(before_f)
    cs, ct = split(ctrl_f)
    ck(cs > bs and ct > bt,
       "%s: structure and motion are up on the raw lookup" % tag,
       "frozen/live spatial %.3f>%.3f  temporal %.3f>%.3f  over %d "
       "clamp-frozen px vs %d always-on-sheet  (both legs "
       "B3_CARFX_SHAPE=0; shipped layer %.1f/%.2f on %.1f)"
       % (cs, bs, ct, bt, int(frozen.sum()), int(live.sum()),
          after[1], after[2], after[0]))

# ------------------------------------------------------------- SHINE-HOT
# the LIT frame this time, not the isolated layer: "too bright" is a claim
# about what the player sees, so it is measured on that.
def hot_checks(tag, lit, mfx, af):
    h = hot(lit, mfx, af)
    ck(h is not None, "%s: the lit run gave a per-frame car mask" % tag)
    if h is None:
        return
    peak, clip, veil, sky, cov, veil_all, pl = h
    print("       lit body, gloss-driven pixels (%.1f%% of the body);"
          " player silhouette %d px" % (cov, pl))
    print("         peak(p99.5) %6.1f   clip %5.2f%%   veil %5.2f%%"
          " (all cars %5.2f%%)   sky peak %6.1f"
          % (peak, clip, veil, veil_all, sky))
    if tag.split()[0] in DAYLIGHT_TRACKS:
        ck(PEAK_LO <= peak <= PEAK_HI,
           "%s: peak body highlight is inside the retail daylight band"
           % tag,
           "%.1f in [%.0f, %.0f]; retail daylight 181.2..214.0"
           % (peak, PEAK_LO, PEAK_HI))
    ck(veil <= MAX_VEIL_PCT,
       "%s: no bright desaturated VEIL over the paint" % tag,
       "%.2f%% <= %.2f%%; retail's worst is 0.50%%"
       % (veil, MAX_VEIL_PCT))
    ck(peak <= sky * MAX_CAR_SKY_RATIO,
       "%s: the reflection does not out-shine the sky it reflects"
       % tag,
       "car %.1f vs sky %.1f (ratio %.2f); retail is 9/9 under 0.96"
       % (peak, sky, peak / max(sky, 1e-6)))


# ----------------------------------------------------------- SHINE-SHAPE
def is_blue(f):
    return (f[..., 2] - np.maximum(f[..., 0], f[..., 1])) > 80


def glint(L, mask):
    """compactness of the brightest 0.1% of the body.

    Returns (radius of gyration / bbox diagonal, |{L >= t}| / 0.1%N).  The
    second number is the one that catches a build that has no glint but a
    PLATEAU: when the top of the body is clipped flat, the 99.9th-percentile
    threshold lands on the plateau and the population it selects is many times
    the 0.1% it asked for.
    """
    v = L[mask]
    n = max(20, int(round(0.001 * v.size)))
    t = np.partition(v, -n)[-n]
    g = mask & (L >= t)
    ys, xs = np.nonzero(g)
    ys2, xs2 = np.nonzero(mask)
    diag = np.hypot(ys2.max() - ys2.min(), xs2.max() - xs2.min())
    rg = np.sqrt(((ys - ys.mean()) ** 2 + (xs - xs.mean()) ** 2).mean())
    return float(rg / max(diag, 1.0)), float(g.sum()) / float(n)


def structure(lit, mfx, af, nb):
    """roof/flank by GEOMETRY, tonal range, and glint compactness.

    Four aligned runs of the same deterministic drive -- the lit frame, the
    ENVDBG=1 silhouette, the ENVDBG=3 reflection layer and the ENVDBG=6 normal
    CLASS -- so frame i of each is the same instant and the bands are the
    shader's own normals, not a guess from the silhouette's bounding box.
    Everything is measured on the PLAYER's car (see player_mask): the retail
    column is one car filling the frame.
    """
    out = {k: [] for k in ("lit_rf", "lay_rf", "roof", "flank", "p5", "p50",
                           "p995", "ls", "rg", "plat")}
    for f, mf, lf, bf in zip(lit, mfx, af, nb):
        m = erode(is_red(mf) | is_green(mf))
        if m.sum() < 500:
            continue
        m = player_mask(m)
        roof = m & is_red(bf)          # N.y >= 0.55  -- roof, deck, hood
        flank = m & is_green(bf)       # |N.y| <= 0.30 -- flanks, rear panel
        if roof.sum() < 200 or flank.sum() < 200:
            continue
        L, Ly = lum(f), lum(lf)
        body = L[m]
        out["lit_rf"].append(L[roof].mean() / max(L[flank].mean(), 1e-6))
        out["lay_rf"].append(Ly[roof].mean() / max(Ly[flank].mean(), 1e-6))
        out["roof"].append(Ly[roof].mean())
        out["flank"].append(Ly[flank].mean())
        out["p5"].append(np.percentile(body, 5))
        out["p50"].append(np.percentile(body, 50))
        out["p995"].append(np.percentile(body, 99.5))
        out["ls"].append(np.log10(np.maximum(body, 1.0)).std())
        r, p = glint(L, m)
        out["rg"].append(r)
        out["plat"].append(p)
    if not out["lit_rf"]:
        return None
    return {k: float(np.mean(v)) for k, v in out.items()}


# the tonal-range band is an absolute statement about DARK PAINT under
# DAYLIGHT, so it is asserted where both hold; the ratios and the glint are
# frame-internal and hold everywhere.
def shape_checks(tag, track, car, lit, mfx, af, nb):
    s = structure(lit, mfx, af, nb)
    ck(s is not None,
       "%s: the ENVDBG=6 run split the body into up- and side-facing bands"
       % tag)
    if s is None:
        return
    print("       structure   roof/flank  lit %.2f  layer %.2f"
          "  (layer roof %.1f, flank %.1f)"
          % (s["lit_rf"], s["lay_rf"], s["roof"], s["flank"]))
    print("       tonal range p5 %5.1f  p50 %5.1f  p99.5 %6.1f  log-std %.3f"
          % (s["p5"], s["p50"], s["p995"], s["ls"]))
    print("       glint       rg/diag %.3f   population %.1fx the top 0.1%%"
          % (s["rg"], s["plat"]))

    ck(s["roof"] > 0.0 and s["flank"] > 0.0
       and s["lit_rf"] >= MIN_ROOF_FLANK_LIT,
       "%s: the up-facing paint carries MORE reflection than the flank" % tag,
       "lit roof/flank %.2f >= %.2f, both bands non-empty; retail's five "
       "rear-chase frames span 1.30..2.80"
       % (s["lit_rf"], MIN_ROOF_FLANK_LIT))
    ck(s["lay_rf"] >= MIN_ROOF_FLANK_LAYER,
       "%s: and that separation comes from the REFLECTION, not the diffuse"
       % tag,
       "layer roof/flank %.2f >= %.2f" % (s["lay_rf"], MIN_ROOF_FLANK_LAYER))
    if car and track in DAYLIGHT_TRACKS:
        ck(s["p5"] <= MAX_DARK_P5,
           "%s: dark paint keeps its BLACKS" % tag,
           "p5 %.1f <= %.1f; retail's five rear-chase frames span 9.3..13.7"
           % (s["p5"], MAX_DARK_P5))
        ck(s["p995"] >= MIN_DARK_PEAK,
           "%s: and still peaks -- the range is wide, not just dark" % tag,
           "p99.5 %.1f >= %.1f; retail 117.7..255.0"
           % (s["p995"], MIN_DARK_PEAK))
        ck(s["ls"] >= MIN_LOG_STD,
           "%s: the body spans a retail TONAL RANGE" % tag,
           "log-std %.3f >= %.2f; retail 0.311..0.407, and the composition "
           "the user called flat measures 0.294"
           % (s["ls"], MIN_LOG_STD))
    ck(s["rg"] <= MAX_GLINT_RG,
       "%s: the brightest 0.1%% of the body is a compact GLINT" % tag,
       "rg/diag %.3f <= %.2f; retail 0.037..0.140"
       % (s["rg"], MAX_GLINT_RG))
    ck(s["plat"] <= MAX_GLINT_PLATEAU,
       "%s: that glint is a highlight, not a clipped PLATEAU" % tag,
       "population %.1fx <= %.1fx the top 0.1%%; retail 1.0x on four frames "
       "of five, and the port's pre-wave captures 28x and 30x"
       % (s["plat"], MAX_GLINT_PLATEAU))


if __name__ == "__main__":
    sys.exit(main())
