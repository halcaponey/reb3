#!/usr/bin/env python3
"""
validate_photo -- the PHOTOREALISM WAVE's gate.

WHAT IT IS GATING, and what it deliberately is NOT.  Every one of the seven
effects is INSPIRED: none of them exists in the retail image and none of them
can be checked against it.  So this suite cannot ask "does it match the game",
and it must not pretend to.  What it CAN ask, and does:

  1  SOURCE + LAW    the switches exist, are documented, and the master gate
                     is a HARD gate -- executed, by compiling the GL-free half
                     of burnout3_aftereffects.c and calling b3_photo_on() and
                     b3_photo_fx() with real environments.  This is the same
                     construction validate_postfx.py section F uses.

  2  BIT IDENTITY    B3_PHOTO=0 renders the pinned frame BIT FOR BIT as the
                     pre-wave build does.  This is the load-bearing one: it is
                     what lets validate_carfx, validate_car_shine,
                     validate_draw_distance and the rest go on pinning
                     B3_PHOTO=0 and go on verifying RECOVERED behaviour.  If
                     this leg ever fails, those suites are no longer measuring
                     what they say they measure.

  3  THE SEVEN LEGS  one EXECUTED leg per effect: on versus off, on the same
                     pinned frame, asserting that the effect moves pixels in
                     the region and the DIRECTION it claims to.  An effect that
                     runs, costs its passes and changes nothing is the failure
                     mode this wave hit twice while it was being built (the
                     occlusion buffer came out white; the reflection mask came
                     out black), and neither was visible from a screenshot.

  4  THE HUD         untouched by all seven.  The layer works on the scene target
                     and the HUD is drawn into the UI target afterwards, so
                     this is a structural claim about the hook order and it is
                     checked as one.

  5  THE SKY         untouched by the four depth-gated effects.  Proves the
                     depth cut: an effect that leaked onto the sky dome would
                     be reading depth wrongly, and a flat plate where the sky
                     was is the loudest possible symptom of it.

  9  TIER 4r          the OPTIONAL ray-traced sun shadow, which is OFF by
                     default -- so what is gated first is that it is
                     INVISIBLE when off (bit-identical to the build before it
                     existed), and only then that it is worth something when
                     on.  Its traversal is EXECUTED, GL-free, against the
                     shipped bvh.bin, and asked the one question the depth
                     map structurally cannot answer: how much of this track
                     is shadowed by something further away than the whole
                     cascade box.

  6  TEMPORAL        the layer must not make the picture UNSTEADY.  There is
                     no TAA in this port, so any per-frame noise in the
                     occlusion or the shadows -- a screen-locked dither
                     pattern, a shadow map that snaps by a texel, a normal
                     reconstructed from depth that is mostly noise at distance
                     -- reads as flicker.  It is invisible on a moving road
                     and glaring next to a HUD element that is not moving,
                     which is why it reaches a player as "flashing when text
                     is on screen" rather than as noise.  Measured on a PINNED
                     camera, where the car drives out of shot in a second and
                     almost every pixel afterwards is static geometry: any
                     frame-to-frame change there is flicker by definition.

Everything is offscreen (SDL_VIDEODRIVER=offscreen, SDL_AUDIODRIVER=dummy).

Usage:
    python3 tools/validate_photo.py
    python3 tools/validate_photo.py --no-render     # sections 1 only
    B3_PHOTO_REF_BIN=/path/to/pre-wave/burnout3 python3 tools/validate_photo.py

THE REFERENCE BINARY.  Section 2 compares against a build of the tree WITHOUT
this wave.  Point B3_PHOTO_REF_BIN at one (build it from the wave's base commit
with `git archive <base> | tar -x -C <dir> && make -C <dir> burnout3`).  With
no reference binary the section still runs the half it can and says clearly
that the cross-build half was skipped rather than passed.


*** THE HARNESS FLIPS A COIN, AND THIS SUITE HAD TO LEARN IT THE HARD WAY. ***

A single pinned render is NOT reproducible on this harness, and that is a
PRE-EXISTING property of the tree, not something this wave introduced.
MEASURED here, ten runs of ONE binary with an identical environment, a warm
asset cache and an identical count of 135 materialisation lines in the log:

    run   1   2   3   4   5   6   7   8   9  10
    hash  A   B   C   D   D   D   D   D   D   E

and the frames that differ are not a few pixels apart -- they differ over
700,000 pixels by up to 250 levels, i.e. they are DIFFERENT MOMENTS ON THE
TRACK.  Something upstream of the render occasionally spends an extra
simulation step before the green light, so `B3_SHOT_FRAME=400` lands somewhere
else.  tools/afx_sweep.py's `pin_ok` was written about exactly this failure
("it silently corrupted two legs of a sweep here before this guard existed"),
and the same guard is needed here.

So every comparison in this file is PINNED, twice over:

  * a WORLD CHECK first (`pin`, the overlap of the two frames' strongest
    edges).  A grade, a curve, a haze and a shadow all rescale the luma, but
    none of them moves where a building's silhouette is -- so this is blind to
    the thing under test and sensitive to the thing that must not vary.  A leg
    that fails it is RE-RENDERED rather than reported.

  * for the bit-identity leg, a SET INTERSECTION rather than a single pair.
    Each side is rendered several times and the claim is that the two sets of
    bytes OVERLAP: the pre-wave build and B3_PHOTO=0 produce the SAME FRAME,
    byte for byte, and the coin flip decides only which of the harness's
    moments both of them are photographing.  That is a strictly stronger claim
    than one lucky pair matching, and unlike one lucky pair it does not fail a
    correct build one time in four.
"""
import argparse
import ctypes
import hashlib
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get("B3_BIN") or os.path.join(ROOT, "burnout3")
REF_BIN = os.environ.get("B3_PHOTO_REF_BIN")
OUT = os.environ.get("B3_PHOTO_OUT") or os.path.join(ROOT, "build", "photoval")

TRACK = "US_C3_V1"
# 1024x768 AND NOT SOMETHING LARGER, because section 2 compares against a
# PRE-WAVE BINARY and the wave also lifted the window clamp that SDL's
# `offscreen` driver imposes (it reports a fixed 1024x768 desktop, and
# burnout3_full.c used to fit the window inside it -- which silently gave every
# headless run a 1024x768 drawable however large a B3_RES it was handed, and
# made a 1080p performance measurement impossible).  A pre-wave binary cannot
# be asked for anything bigger offscreen, so the identity leg asks for the size
# both builds agree on.  Nothing about the seven effects is resolution-dependent;
# tools/photo_perf.py measures them at 1920x1080.
RES = "1024x768"
SHOT_FRAME = 400
# A second pinned frame, chosen because the sun is ON SCREEN there -- the god
# rays are correctly a no-op at frame 400 (the car is driving away from the
# sun), so gating them on that frame would gate nothing.  Found with
# B3_PHOTO_VERBOSE=1, which prints the sun's projected uv every 120 frames.
SUN_FRAME = 2160

# The cascade's own box, restated here rather than read out of the header:
# the tier-4r section's headline claim is a comparison against it, and a validator that
# took the number from the thing it is measuring would agree with any value.
B3_SH_EXTENT = 260.0

FX = ["tonemap", "ssao", "atmos", "shadow", "ssr", "godray", "lights"]
FX_ENV = {t: "B3_PHOTO_" + t.upper() for t in FX}

_checks = [0, 0]
_fails = []


def check(ok, what, detail=""):
    _checks[0] += 1
    if ok:
        _checks[1] += 1
    else:
        _fails.append(what + ((" -- " + detail) if detail else ""))
    print("  %s  %s%s" % ("PASS" if ok else "FAIL", what,
                          ("   [" + detail + "]") if detail else ""))
    return ok


# ======================================================================
# 1  SOURCE + LAW -- the switches, EXECUTED
# ======================================================================
def section_source():
    print("\n== 1. SOURCE + LAW ==")
    hdr = open(os.path.join(ROOT, "src", "burnout3_aftereffects.h")).read()
    src = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()

    check("NOTHING IN THIS SECTION IS A CLAIM ABOUT BURNOUT 3" in hdr,
          "the layer is marked INSPIRED in the header, unmistakably")
    for t in FX:
        check(FX_ENV[t] in hdr and FX_ENV[t] in src,
              "%s has its own switch, %s" % (t, FX_ENV[t]))
    check("B3_PHOTO_DEF" in src, "the seven defaults live in one table")
    # the constants: every tunable must be an env of the same name
    consts = [l.split()[1] for l in hdr.splitlines()
              if l.startswith("#define B3_PHOTO_")]
    tunable = [c for c in consts
               if not c.startswith("B3_PHOTO_FX_")
               and c not in ("B3_PHOTO_LUT_N",)
               and not c.endswith(("_R", "_G", "_B"))]
    # BOTH FILES, because a few of these are read at the CALL SITE rather than
    # in the knob block: the shadow box is fitted in burnout3_full.c and so are
    # the headlight beams, since both need the frame's camera and its cars.
    # The claim being gated is "every constant has an env of its own", not
    # "every constant is read in this one function".
    full = open(os.path.join(ROOT, "src", "burnout3_full.c")).read()
    missing = [c for c in tunable
               if ('"%s"' % c) not in src and ('"%s"' % c) not in full]
    check(not missing,
          "every tunable constant is overridable by an env of the same name",
          ("missing: " + ", ".join(missing)) if missing else
          "%d constants" % len(tunable))

    # ---- EXECUTE the gate -------------------------------------------------
    tmp = tempfile.mkdtemp(prefix="b3photo")
    so = os.path.join(tmp, "photo.so")
    cc = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O0", "-DB3_AFX_NO_GL",
         # burnout3_postfx.c comes along because the laws call into it, and it
         # needs its own no-GL switch or the probe pulls in b3r_gl_depth_mask
         "-DB3_POSTFX_NO_GL",
         "-I" + os.path.join(ROOT, "src"),
         os.path.join(ROOT, "src", "burnout3_aftereffects.c"),
         os.path.join(ROOT, "src", "burnout3_postfx.c"),
         "-o", so, "-lm"],
        capture_output=True)
    if cc.returncode != 0:
        check(False, "the GL-free probe builds",
              cc.stderr.decode("utf-8", "replace")[-300:])
        return

    # Each environment is a SEPARATE process, because the switches latch on
    # first read -- which is itself part of the contract and is asserted here.
    probe = os.path.join(tmp, "probe.py")
    open(probe, "w").write(
        "import ctypes,sys\n"
        "l=ctypes.CDLL(sys.argv[1])\n"
        "print(l.b3_photo_on(), *[l.b3_photo_fx(i) for i in range(7)])\n")

    def ask(env):
        e = dict(os.environ)
        for k in list(FX_ENV.values()) + ["B3_PHOTO"]:
            e.pop(k, None)
        e.update(env)
        r = subprocess.run([sys.executable, probe, so], env=e,
                           capture_output=True)
        return [int(x) for x in r.stdout.decode().split()]

    d = ask({})
    check(d == [1] * 8,
          "the shipped default: the master and all seven on", str(d))
    d = ask({"B3_PHOTO": "0"})
    check(d == [0] * 8,
          "B3_PHOTO=0 turns the master and all seven off", str(d))
    # THE HARD-GATE CLAIM, and it is the one the pinning strategy rests on.
    d = ask(dict([("B3_PHOTO", "0")] + [(FX_ENV[t], "1") for t in FX]))
    check(d == [0] * 8,
          "B3_PHOTO=0 is a HARD gate: no individual switch can defeat it",
          str(d))
    d = ask({"B3_PHOTO_SSR": "0"})
    check(d == [1, 1, 1, 1, 1, 0, 1, 1],
          "an individual switch turns its own effect OFF under B3_PHOTO=1",
          str(d))
    d = ask({"B3_PHOTO_SHADOW": "0"})
    check(d == [1, 1, 1, 1, 0, 1, 1, 1],
          "...and off, without disturbing the other six", str(d))
    d = ask({"B3_PHOTO_LIGHTS": "0"})
    check(d == [1, 1, 1, 1, 1, 1, 1, 0],
          "...and tier 7's own switch is wired the same way", str(d))


# ======================================================================
# the render harness
# ======================================================================
def shot(name, env_extra, binary=GAME, frame=SHOT_FRAME, res=RES):
    os.makedirs(OUT, exist_ok=True)
    bmp = os.path.join(OUT, name + ".bmp")
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": TRACK,
        "B3_RES": res,
        "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_SHOT": bmp,
        "B3_SHOT_FRAME": str(frame),
        "B3_AFX": "1",
        # THE TWO PINS EVERY BIT-EXACT LEG HERE NEEDS, and they are not this
        # wave's invention -- validate_carfx.py's shot() carries the same pair
        # with the same reasoning:
        #   B3_MUSIC_SEED     nothing else calls b3_music_seed(), so the
        #                     shuffle bag opens on time(NULL) and the EA TRAX
        #                     ticker differs run to run.
        #   B3_TRACK_NOSHINE  the track's class-1/7/10 additive specular pass
        #                     is GL_ONE/GL_ONE with depth writes off over
        #                     OVERLAPPING geometry, and float addition is
        #                     commutative but not associative.  MEASURED here
        #                     before it was pinned: two runs of the SAME
        #                     binary differed in 0.0001% of the frame by up to
        #                     2 levels, which is enough to fail every
        #                     bit-exact leg in this file for a reason that has
        #                     nothing to do with the wave.
        "B3_MUSIC_SEED": "1",
        "B3_TRACK_NOSHINE": "1",
        #   B3_RT             tier 4r is OFF by default, but its setting
        #                     PERSISTS to build/settings.cfg -- which lives in
        #                     the directory these runs use as their CWD.  An
        #                     operator who turned the option on in the pause
        #                     menu once would otherwise have every leg in this
        #                     file, the bit-identity one included, measuring a
        #                     renderer nobody asked for.  Pinned here; a leg
        #                     that wants the ray says so.
        "B3_RT": "0",
        #   B3_RT_CARS        tier 4rc changes what the shipped shader is
        #                     assembled from AND whether retail's blob shadow
        #                     is drawn, so a leg that did not pin it would be
        #                     measuring whatever the operator's shell happened
        #                     to hold.  `racers` is the shipped default and is
        #                     what the legs that want the cars ask for.
        "B3_RT_CARS": "racers",
    })
    for k in list(FX_ENV.values()) + ["B3_PHOTO"]:
        env.pop(k, None)
    env.update(env_extra)
    if os.path.exists(bmp):
        os.remove(bmp)
    subprocess.run(["timeout", "900", binary], cwd=ROOT, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return bmp if os.path.isfile(bmp) else None


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def arr(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float64)


def bands(a):
    """The frame's four regions, as row slices.

    SKY      the top TENTH, and only the middle of it.  MEASURED, not
             guessed: the top-RIGHT of this pinned frame is a distant mountain
             ridge and a shop roof, which the atmospherics correctly haze by
             up to 49 levels -- a "sky" band that swept them in would be
             testing the exact opposite of what it says.  Rows 0-10%, columns
             25-70% is dome and nothing else here.
    FAR      the horizon band, where the atmospherics live.
    NEAR     the bottom third: road, car, contact shadows.
    HUD      two corner boxes.  The layer runs BEFORE the HUD is drawn, so
             these must not move at all.
    """
    h, w, _ = a.shape
    return {
        "sky":  a[0:int(h * 0.10), int(w * 0.25):int(w * 0.70), :],
        "far":  a[int(h * 0.36):int(h * 0.52), :, :],
        "near": a[int(h * 0.66):, :, :],
    }


def hud_mask(ref):
    """The HUD's own opaque art, as a boolean mask over the frame.

    NOT "the corner boxes": those are mostly the SCENE, seen through a
    semi-transparent banner, and of course the scene changes.  What must not
    change is the art the HUD draws on top of them -- and the loudest of that
    is the yellow numerals, which are opaque.  The mask is selected from the
    REFERENCE frame, so every leg is judged on the same set of pixels.
    """
    h, w, _ = ref.shape
    yellow = ((ref[:, :, 0] > 190) & (ref[:, :, 1] > 140)
              & (ref[:, :, 2] < 110))
    box = np.zeros(yellow.shape, bool)
    box[0:int(h * 0.14), 0:int(w * 0.22)] = True        # POS / LAP
    box[int(h * 0.86):, int(w * 0.65):] = True          # the speedo
    m = yellow & box
    # ...AND ONLY ITS INTERIOR.  A colour threshold selects the antialiased
    # RIM of every glyph along with its body, and a rim pixel is a blend of
    # the art and the scene behind it -- so it follows the scene by a level or
    # two, by construction, and the share of the mask that does is a function
    # of how bright the scene under the speedo happens to be.  That made the
    # check a brightness meter with an HUD's name on it: it went red the day
    # the headlight beams started lighting the near-field road, at 74.8%
    # bit-exact against a threshold of 75, having measured 92.6% within two
    # levels against a threshold of 95 -- while every claim it is written to
    # make was still true.  Two erosions drop the rim and leave the body, and
    # the body is the thing the hook order actually buys.
    for _ in range(2):
        e = m.copy()
        e[1:, :] &= m[:-1, :]
        e[:-1, :] &= m[1:, :]
        e[:, 1:] &= m[:, :-1]
        e[:, :-1] &= m[:, 1:]
        m = e
    return m


def lum(a):
    return a @ np.array([0.299, 0.587, 0.114])


def world_pin(a, b):
    """Did these two frames photograph the same MOMENT?

    The overlap of the two frames' top 4% of edge pixels.  Blind to tone --
    which is what every effect in this wave changes -- and sensitive to
    geometry, which is what must not change between a leg and its reference.
    Same moment: 0.6 and up.  Different moment: well under 0.2.
    """
    def top_edges(x, frac=0.04):
        l = lum(x)
        gx = np.zeros_like(l)
        gy = np.zeros_like(l)
        gx[:, 1:-1] = l[:, 2:] - l[:, :-2]
        gy[1:-1, :] = l[2:, :] - l[:-2, :]
        g = np.hypot(gx, gy)
        return g >= np.quantile(g, 1.0 - frac)
    ma, mb = top_edges(a), top_edges(b)
    u = float((ma | mb).sum())
    return float((ma & mb).sum() / u) if u > 0 else 0.0


PIN_MIN = 0.55


def shot_pinned(name, env_extra, ref, frame=SHOT_FRAME, tries=4):
    """Render until the world agrees with `ref`, or give up and say so.

    See the coin-flip note in the module docstring.  Re-rendering is legitimate
    here precisely because the thing being retried is not the thing under
    test: the effect is deterministic, the harness's choice of moment is not.
    """
    for i in range(tries):
        p = shot(name if i == 0 else "%s_r%d" % (name, i), env_extra,
                 frame=frame)
        if not p:
            return None, 0.0
        a = arr(p)
        pin = world_pin(ref, a)
        if pin >= PIN_MIN:
            return a, pin
        print("      (world diverged at pin %.3f -- re-rendering %s)"
              % (pin, name))
    return a, pin


# ======================================================================
# 2  BIT IDENTITY
# ======================================================================
def section_identity():
    print("\n== 2. BIT IDENTITY: B3_PHOTO=0 is the pre-wave frame ==")

    def hashes(tag, env, binary=GAME, n=4):
        out = []
        for i in range(n):
            p = shot("%s_%d" % (tag, i), env, binary=binary)
            if p:
                out.append((sha(p), p))
        return out

    mine = hashes("off", {"B3_PHOTO": "0"})
    if not check(len(mine) >= 3, "B3_PHOTO=0 renders the pinned frame"):
        return None
    hs = set(h for h, _ in mine)
    print("      B3_PHOTO=0 produced %d distinct frame(s) in %d runs: %s"
          % (len(hs), len(mine), " ".join(sorted(h[:8] for h in hs))))
    # NOT a determinism gate -- see the coin-flip note.  It is reported so the
    # operator can see how the harness behaved on this machine today.

    # EVERY switch, from the table rather than from a list written out here --
    # a hand-written list is a list that goes stale the moment a tier is added,
    # and the one thing this leg must not do is quietly stop forcing one of
    # them on while still claiming the master beat them all.
    forced = hashes("off_forced",
                    dict([("B3_PHOTO", "0")]
                         + [(FX_ENV[t], "1") for t in FX]), n=3)
    fs = set(h for h, _ in forced)
    check(bool(hs & fs),
          "B3_PHOTO=0 is BIT-IDENTICAL with every effect switch forced ON -- "
          "the master really is a hard gate, in pixels as well as in the law",
          "%d shared frame(s)" % len(hs & fs))

    if not REF_BIN:
        print("  SKIP  the cross-build half: set B3_PHOTO_REF_BIN to a "
              "pre-wave binary")
        print("        (this is NOT a pass -- the wave's headline gate is "
              "unproven without it)")
    elif not os.path.isfile(REF_BIN):
        check(False, "B3_PHOTO_REF_BIN exists", REF_BIN)
    else:
        # B3_PHOTO=0 ON THE REFERENCE TOO, and leaving it off was this leg
        # reporting a failure that was its own.
        #
        # It used to hand the reference an EMPTY environment, which was right
        # exactly once: for a binary from before the wave, which has no
        # B3_PHOTO to read.  Point it at any build from after the wave -- which
        # is what you do to check a LATER change against the wave's own
        # baseline -- and the empty environment means the master defaults ON,
        # so the leg compared this build with the wave off against that one
        # with the wave on and reported 97.4% of pixels differing.  Nothing
        # was wrong with either build.
        #
        # Setting it is correct for both: a genuinely pre-wave binary ignores
        # an env it never reads.  MEASURED with it set, six runs of the two
        # binaries alternately: one hash, aa802238, all six times.
        ref = hashes("ref", {"B3_PHOTO": "0"}, binary=REF_BIN, n=4)
        if check(len(ref) >= 3, "the reference binary renders the pinned frame"):
            rs = set(h for h, _ in ref)
            print("      the pre-wave build produced %d distinct frame(s) in "
                  "%d runs" % (len(rs), len(ref)))
            shared = hs & rs
            ok = check(bool(shared),
                       "*** B3_PHOTO=0 IS BIT-IDENTICAL TO THE PRE-WAVE "
                       "BUILD *** -- this is what lets the recovered-pixel "
                       "suites pin it",
                       "%d frame(s) byte-for-byte identical: %s"
                       % (len(shared), " ".join(sorted(h[:8] for h in shared)))
                       if shared else "no shared frame in %d x %d runs"
                       % (len(mine), len(ref)))
            if not ok:
                # say HOW different, so a real regression is distinguishable
                # from a coin flip that never landed the same way twice
                a, b = arr(mine[0][1]), arr(ref[0][1])
                d = np.abs(a - b)
                print("        best-effort pair: %.4f%% of pixels differ, "
                      "max delta %d, world pin %.3f"
                      % ((d.max(axis=2) > 0).mean() * 100, int(d.max()),
                         world_pin(a, b)))
    # hand back a frame for section 3 to measure against
    return mine[0][1]


# ======================================================================
# 3b  ATMOSPHERICS ON A DARK SKY
# ======================================================================
# THE FRAME A PLAYER REPORTED.  US_M1 is a coastal storm at dusk: the sky
# measures 12..40 levels and the track's authored fog colour, 0.073/0.096/
# 0.139, is the darkest of the 36 shipped by a factor of two.  Tier 3 used to
# fade distant geometry toward a COMPILED-IN daylight blue leaned on the sun
# COLOUR -- and enviro.dat +0x60 is a tint, not a radiance, so no track has a
# dim one.  The distant city therefore came out at 199 levels against a sky at
# 104 in the reported 16:9 framing (119.5 against 71.3 in the 4:3 one this leg
# renders, which is what a run against a pre-fix binary prints): flat
# light-grey cut-outs pasted over the weather, which is exactly how it was
# reported ("far away things appear totally gray").
#
# Two laws are gated here, and neither is expressible on the day track:
#   1. the haze can never be BRIGHTER than the sky it silhouettes against
#   2. the haze only ever ADDS airlight -- the extinction half of the transport
#      equation is retail's own fog, already applied by the world pass, and
#      applying it twice took the far band BELOW the tier-off frame
#
# The two regions are found from the pass's OWN sky mask rather than from
# hand-measured boxes: the block is gated by a hard depth cut, so within the
# horizon band a pixel that moved is far geometry and a pixel that did not is
# the dome.  That is aspect-ratio independent, which hand-measured rows are
# not -- this frame is 4:3 here and the viewpoint it was found at was 16:9.
ATMOS_DARK_TRACK = os.environ.get("B3_PHOTO_ATMOS_DARK_TRACK", "US_M1_V1")
# Recovered with B3_DUMP_FRAME from the reported frame's own gamestate: the
# player at (1105.1, 97.4, -364.9) heading +Z, camera backed off along it.
# A long sightline down the coast road with the distant city on the left, the
# headland on the right, and storm sky over both.
ATMOS_DARK_CAM = "1106.5,99.9,-372.8,1098.2,97.4,-325.5"
ATMOS_DARK_FRAME = 60


def section_atmos_dark():
    def leg(tag, on):
        env = {"B3_PHOTO": "1", "B3_TRACK": ATMOS_DARK_TRACK,
               "B3_CAM": ATMOS_DARK_CAM}
        for t in FX:
            env[FX_ENV[t]] = "1" if (t == "atmos" and on) else "0"
        return shot(tag, env, frame=ATMOS_DARK_FRAME)

    off_p = leg("atmos_dark_off", False)
    on_p = leg("atmos_dark_on", True)
    if not check(off_p and on_p,
                 "atmos/dark: the %s leg rendered" % ATMOS_DARK_TRACK):
        return
    off, on = arr(off_p), arr(on_p)
    if not check(off.shape == on.shape, "atmos/dark: the pair is comparable"):
        return
    h, w, _ = off.shape
    # the horizon band, left of the headland: storm sky over distant city
    reg = (slice(int(h * 0.22), int(h * 0.46)), slice(int(w * 0.03),
                                                     int(w * 0.62)))
    d = np.abs(on[reg] - off[reg]).max(axis=2)
    hazed = d > 1.0
    dome = d <= 1.0
    frac = float(hazed.mean() * 100)
    check(frac > 2.0,
          "atmos/dark: the horizon band IS hazed (a leg that changed nothing "
          "would pass every check below)", "%.1f%% of the band moved" % frac)
    if hazed.any() and dome.any():
        far_y = float(lum(on[reg])[hazed].mean())
        sky_y = float(lum(on[reg])[dome].mean())
        check(far_y <= sky_y,
              "atmos/dark: the haze never rises ABOVE the sky it silhouettes "
              "against -- no light-grey cut-outs",
              "far %.1f vs sky %.1f (ratio %.2f)"
              % (far_y, sky_y, far_y / max(sky_y, 1e-6)))
    # LAW 2, over the WHOLE WORLD and not just the band the haze is strongest
    # in: inscatter only.  Rows 15..65% is every pixel of the world this
    # camera sees and NO HUD -- the top banners live above it and the EA TRAX
    # song ticker below, and the ticker is the one thing in the frame that is
    # not a function of the moment alone (it animates on elapsed frames, and
    # the loading path does not spend the same number of them every run).
    # Measured, it was 2.9% of the frame and up to 202 levels: a HUD delta
    # dressed as a lighting one.
    wr = (slice(int(h * 0.15), int(h * 0.65)), slice(None))
    drop = float((lum(on[wr]) < lum(off[wr]) - 1.0).mean() * 100)
    check(drop < 0.10,
          "atmos/dark: it ADDS airlight and never subtracts -- the extinction "
          "half is retail's fog, and is not applied twice",
          "%.3f%% of the world darker by >1 level" % drop)
    lo_off = float(np.quantile(lum(off[reg])[hazed], 0.25)) if hazed.any() else 0.0
    lo_on = float(np.quantile(lum(on[reg])[hazed], 0.25)) if hazed.any() else 0.0
    check(lo_on >= lo_off,
          "atmos/dark: the dark quarter of the far band does not fall",
          "p25 %.1f -> %.1f" % (lo_off, lo_on))


# ======================================================================
# 3, 4, 5  THE SIX LEGS, the HUD and the sky
# ======================================================================
def section_legs(off_bmp):
    print("\n== 3. THE SEVEN EFFECTS, one executed leg each ==")
    if not off_bmp:
        check(False, "the reference frame exists")
        return
    off = arr(off_bmp)
    ob = bands(off)
    frames = {}

    def leg(tag, frame=SHOT_FRAME, ref=off):
        env = {"B3_PHOTO": "1"}
        for t in FX:
            env[FX_ENV[t]] = "1" if t == tag else "0"
        a, pin = shot_pinned("leg_" + tag, env, ref, frame=frame)
        if a is not None and pin < PIN_MIN:
            print("      ! %s: the world would not settle (pin %.3f); its "
                  "numbers below are not trustworthy" % (tag, pin))
        return a

    def leg_env_shot(name, tag, extra, ref):
        """One effect's leg with extra environment -- see tier 7 below."""
        env = {"B3_PHOTO": "1"}
        for t in FX:
            env[FX_ENV[t]] = "1" if t == tag else "0"
        env.update(extra)
        a, pin = shot_pinned(name, env, ref)
        if a is not None and pin < PIN_MIN:
            print("      ! %s: the world would not settle (pin %.3f)"
                  % (name, pin))
        return a

    for t in FX:
        if t == "lights":
            continue          # tier 7 has its own leg below, at full dusk
        frames[t] = leg(t)

    # ---- 1 TONEMAP: recovers the top, keeps the exposure, adds the grade ----
    a = frames["tonemap"]
    if check(a is not None, "tonemap: the leg rendered"):
        clip_off = float((off >= 254.5).any(axis=2).mean() * 100)
        clip_on = float((a >= 254.5).any(axis=2).mean() * 100)
        check(clip_on < clip_off * 0.6,
              "tonemap: CLIPPING FALLS -- the shoulder recovers the top",
              "%.2f%% -> %.2f%%" % (clip_off, clip_on))
        dmean = float(lum(a).mean() - lum(off).mean())
        check(abs(dmean) < 8.0,
              "tonemap: the exposure INTENT survives (frame mean within 8 "
              "levels of the recovered x2)", "%+.2f levels" % dmean)
        sat = lambda x: float((x.max(axis=2) - x.min(axis=2)).mean())
        check(sat(a) > sat(off) + 2.0,
              "tonemap: the authored grade is present (saturation rises)",
              "%.2f -> %.2f" % (sat(off), sat(a)))

    # ---- 2 SSAO: grounds, and grounds where the ground is ------------------
    a = frames["ssao"]
    if check(a is not None, "ssao: the leg rendered"):
        ab = bands(a)
        dn = float(lum(ab["near"]).mean() - lum(ob["near"]).mean())
        check(dn < -0.6,
              "ssao: the NEAR band DARKENS -- occlusion, not brightening",
              "%+.2f levels" % dn)
        moved = float((np.abs(a - off).max(axis=2) > 2).mean() * 100)
        check(moved > 5.0,
              "ssao: it is VISIBLE (a white occlusion buffer is the failure "
              "this exists to catch)", "%.2f%% of pixels moved" % moved)

    # ---- 3 ATMOSPHERICS: distance, and only distance -----------------------
    a = frames["atmos"]
    if check(a is not None, "atmos: the leg rendered"):
        ab = bands(a)
        dfar = float(np.abs(ab["far"] - ob["far"]).mean())
        dnear = float(np.abs(ab["near"] - ob["near"]).mean())
        check(dfar > dnear * 2.0,
              "atmos: the FAR band moves far more than the NEAR one -- it is "
              "a function of DISTANCE", "far %.3f vs near %.3f" % (dfar, dnear))
        # AERIAL PERSPECTIVE LIFTS DISTANT SHADOWS.  That is the physical
        # claim and it is the one worth gating: scatter ADDS light along the
        # path, so the darkest quarter of the far band gets BRIGHTER while the
        # bright end barely moves.  (The band's standard deviation is NOT the
        # test -- it RISES here, because the band holds bright sky as well as
        # dark ridge, and lifting the ridge toward the sky's value moves it
        # away from the band's own mean.  That was the first check written
        # and it failed on a correct render.)
        lo_off = float(np.quantile(lum(ob["far"]), 0.25))
        lo_on = float(np.quantile(lum(ab["far"]), 0.25))
        check(lo_on > lo_off + 1.0,
              "atmos: distant SHADOWS LIFT -- scatter adds light along the "
              "path", "p25 %.1f -> %.1f" % (lo_off, lo_on))
        near_d = float(np.abs(ab["near"] - ob["near"]).max())
        check(near_d <= 2.0,
              "atmos: the NEAR field is left alone -- the car and the road "
              "under it do not get hazed", "max delta %d" % int(near_d))

    # ---- 3b ATMOSPHERICS ON A DARK SKY -------------------------------------
    section_atmos_dark()

    # ---- 4 SHADOWS: darken, and only where the sun is blocked --------------
    a = frames["shadow"]
    if check(a is not None, "shadow: the leg rendered"):
        # TIER 4 IS SUN LIGHTING, NOT JUST A STENCIL.  It is a shadow map AND
        # an energy-preserving directional relight, so "the frame darkens" is
        # the WRONG claim -- and it failed on a correct render once the relight
        # went in.  What must be true is that light is REDISTRIBUTED: a real
        # slice of the frame goes down (in shadow, or turned away from the
        # sun), a real slice goes up (facing it), and the mean barely moves.
        d = lum(a) - lum(off)
        down = float((d < -2).mean() * 100)
        up = float((d > 2).mean() * 100)
        net = float(d.mean())
        check(down > 1.0, "shadow: a real slice of the frame goes into shade",
              "%.2f%% of pixels darker by >2 levels" % down)
        check(up > 1.0, "shadow: ...and a real slice is lit by the sun "
              "(the directional term)",
              "%.2f%% of pixels brighter by >2 levels" % up)
        check(abs(net) < 4.0,
              "shadow: it REDISTRIBUTES light rather than dimming the frame",
              "net %+.2f levels" % net)
        moved = float((np.abs(a - off).max(axis=2) > 2).mean() * 100)
        check(2.0 < moved < 92.0,
              "shadow: it is directional, not a global multiply -- part of the "
              "frame is untouched", "%.2f%% of pixels moved" % moved)

    # ---- 5 SSR: only under the mask, and never everywhere -------------------
    a = frames["ssr"]
    if check(a is not None, "ssr: the leg rendered"):
        moved = float((np.abs(a - off).max(axis=2) > 2).mean() * 100)
        check(moved > 0.20,
              "ssr: the shine mask is NOT BLACK (a mask built from the "
              "specular value instead of from coverage was: 0.02%)",
              "%.2f%% of pixels moved" % moved)
        check(moved < 35.0,
              "ssr: it is confined to the track's own shine spans",
              "%.2f%% of pixels moved" % moved)

    # ---- 6 GOD RAYS: on the sun frame, and toward the sun -------------------
    a = frames["godray"]
    if check(a is not None, "godray: the leg rendered (frame %d)" % SHOT_FRAME):
        same = float(np.abs(a - off).max())
        # <= 2 rather than == 0: with the god rays LIVE the composite is a
        # different assembled shader (it declares uPhoto and adds a
        # multiply-by-zero term), and a different shader binary is entitled to
        # round the passes downstream of it a level differently.  The claim
        # being gated is "no shafts", not "no shader".
        check(same <= 2.0,
              "godray: NOTHING at frame %d -- the sun is behind the car there, "
              "and a shaft with no sun on screen would be the bug" % SHOT_FRAME,
              "max delta %d" % int(same))
    print("\n  -- the sun frame (%d), where the shafts can exist --" % SUN_FRAME)
    off_sun = shot("off_sun", {"B3_PHOTO": "0"}, frame=SUN_FRAME)
    # PINNED AGAINST THE SUN FRAME, not against frame 400.  Passing the
    # frame-400 reference here made the world check compare two different
    # MOMENTS and cry wolf on every run -- the guard was right that they were
    # not the same world, and wrong that it mattered.
    on_sun = leg("godray", frame=SUN_FRAME,
                 ref=arr(off_sun) if off_sun else off)
    if check(off_sun and on_sun is not None,
             "godray: the sun frame rendered"):
        o = arr(off_sun)
        dmean = float(lum(on_sun).mean() - lum(o).mean())
        check(dmean > 0.15,
              "godray: the frame BRIGHTENS -- light shafts ADD light",
              "%+.3f levels" % dmean)
        d = np.abs(on_sun - o).max(axis=2)
        h = d.shape[0]
        top = float(d[0:h // 2].mean())
        bot = float(d[h // 2:].mean())
        check(top > bot,
              "godray: the shafts are concentrated in the SUN's half of the "
              "frame", "top %.3f vs bottom %.3f" % (top, bot))

    # ---- 7 PER-SOURCE LIGHTS ----------------------------------------------
    # AT FULL DUSK, because the shipped gain on this DAYTIME track is an
    # effective 0.11 and a leg measuring that would be measuring the day
    # multiplier rather than the tier.  B3_PHOTO_LIGHT_DUSK=1 is the override
    # that exists for exactly this: it asks "does the tier work", and the day
    # behaviour is asked separately, in section 8.
    print("\n  -- tier 7 at full dusk (the day gain is section 8's question) --")
    a = leg_env_shot("leg_lights_dusk", "lights",
                     {"B3_PHOTO_LIGHT_DUSK": "1"}, off)
    if check(a is not None, "lights: the leg rendered"):
        dmean = float(lum(a).mean() - lum(off).mean())
        check(dmean > 0.30,
              "lights: the frame BRIGHTENS -- a lamp ADDS light, it does not "
              "redistribute it the way tier 4b does",
              "%+.3f levels" % dmean)
        moved = float((np.abs(a - off).max(axis=2) > 2).mean() * 100)
        check(moved > 1.0,
              "lights: it is VISIBLE (a light field that derived nothing, or "
              "picked nothing, is the failure this exists to catch)",
              "%.2f%% of pixels moved" % moved)
        # A POOL, NOT A LIFT, and this is the shape of the claim rather than
        # a band comparison -- the lamps on this frame are up the road, not
        # under the camera, so `bands()` puts them all in FAR and the obvious
        # near-versus-far test measures where the street furniture happens to
        # be.  What is true of a light and false of an ambient add is that the
        # light it adds is CONCENTRATED: a uniform lift would have the
        # brightest 5% of pixels carrying 5% of it.
        # THE STATISTIC CHANGED, AND THE CLAIM DID NOT.  This used to be the
        # share of the light carried by the brightest 5% of pixels, against a
        # threshold that had already been walked from 60% to 25% once, when
        # tier 7b's beams arrived: a scenery bulb throws a tight pool (the
        # brightest 5% carried 99.9%) and a headlight throws a footprint that
        # is legitimately BROAD.  It came due again when the beams were fixed
        # to actually light the road (18.4%), and a threshold that has to be
        # walked down every time the feature works better is not measuring the
        # thing it is named after.
        #
        # What "an ambient lift with a light's name on it" actually IS, is a
        # constant: it touches everything the tier can touch, by the same
        # amount.  So the two gates below are the two things a constant cannot
        # do, and neither of them cares how large the footprint is.
        #
        #   DARK    a real light leaves most of the frame alone.  Measured on
        #           the shipped build: 57% of the frame gets under a twentieth
        #           of the peak.  A lift gets 0% (bar the sky, which the pass
        #           excludes by construction and which is a fifth of this
        #           frame -- hence a floor of 25 rather than of 50).
        #   SPREAD  and among what it DOES touch, it has a bright core and a
        #           falloff.  Measured 0.52 of coefficient of variation.  A
        #           lift is exactly 0.00, by definition.
        d = np.clip(lum(a) - lum(off), 0.0, None)
        peak = float(np.percentile(d, 99.9))
        dark = float((d < 0.05 * peak).mean() * 100.0)
        touched = d[d > 1.0]
        cov = (float(touched.std() / max(touched.mean(), 1e-9))
               if touched.size else 0.0)
        check(peak > 0.0 and dark > 25.0,
              "lights: the added light LEAVES MOST OF THE FRAME ALONE -- an "
              "ambient lift with a light's name on it cannot",
              "%.1f%% of pixels under a twentieth of the peak (a lift: ~0%%)"
              % dark)
        check(cov > 0.25,
              "lights: ...and what it does touch has a CORE AND A FALLOFF -- "
              "these are pools and beams, not a constant",
              "coefficient of variation %.2f over the lit pixels "
              "(a lift: 0.00)" % cov)

    # ---- 4  THE HUD --------------------------------------------------------
    print("\n== 4. THE HUD is untouched by all seven ==")
    allp = shot("leg_all", {"B3_PHOTO": "1"})
    if check(allp, "the all-on frame rendered"):
        al = arr(allp)
        hm = hud_mask(off)
        d = np.abs(al - off).max(axis=2)[hm]
        exact = float((d == 0).mean() * 100)
        near = float((d <= 2).mean() * 100)
        # WHAT THE CLAIM ACTUALLY IS.  The HUD is drawn into the UI target
        # AFTER the whole layer has run, so no pass here ever processes an HUD
        # pixel -- but the HUD is COMPOSITED over the scene, and the scene has
        # changed, so its antialiased edges follow the background by a level or
        # two.  The INTERIOR of the art does not move, and that is the property
        # the hook order buys and the one gated here -- which is why hud_mask()
        # erodes the rim away before this arrives.
        check(exact > 95.0 and near == 100.0,
              "the HUD's own art is untouched with all seven on -- the layer "
              "runs before the HUD is drawn",
              "%.1f%% of %d interior numeral pixels bit-exact, %.1f%% within 2"
              % (exact, int(hm.sum()), near))
        check(float(d.max()) <= 4.0,
              "...and the few that are not are still following the background "
              "by a level or two, not being processed",
              "worst interior pixel %d levels" % int(d.max()))

    # ---- 5  THE SKY --------------------------------------------------------
    print("\n== 5. THE SKY is untouched by the depth-gated effects ==")
    for t in ("ssao", "atmos", "shadow", "ssr"):
        a = frames[t]
        if a is None:
            continue
        # A FRACTION, NOT A MAXIMUM.  The band is a fixed rectangle and the
        # harness picks a different MOMENT between runs (see the coin-flip note
        # at the top), so on some runs a lamp arm or a shop roof drifts into
        # it -- and the atmospherics correctly haze those by tens of levels.
        # A real depth-cut leak moves the WHOLE band; a bit of intruding
        # geometry moves a handful of pixels.  The fraction tells them apart
        # and the maximum cannot.
        db = np.abs(bands(a)["sky"] - ob["sky"]).max(axis=2)
        frac = float((db > 2).mean() * 100)
        check(frac < 1.5,
              "%s: the sky dome is untouched (the depth cut works)" % t,
              "%.2f%% of the sky band moved, worst pixel %d"
              % (frac, int(db.max())))


# ======================================================================
# 6  TEMPORAL STABILITY
# ======================================================================
# The camera is PINNED here, and that is the whole trick.  With the chase cam
# the frame-to-frame difference is dominated by 44 metres per second of camera
# motion -- measured, 11.7 levels a frame -- and a flicker of one level is four
# per cent of that, i.e. invisible in the number.  Park the camera and the same
# flicker is the only thing left.
#
# The pose is the player's own at frame 400 on this track, backed off and aimed
# down the road (recovered with B3_DUMP_FRAME).  It does not have to be a
# beautiful shot; it has to be a fixed one with geometry in it.
STATIC_CAM = "5115,152,-1935,5090,150,-1890"
STATIC_FIRST = 430
STATIC_N = 40


def shot_seq(name, env_extra):
    outdir = os.path.join(OUT, name)
    if os.path.isdir(outdir):
        for f in os.listdir(outdir):
            os.remove(os.path.join(outdir, f))
    os.makedirs(outdir, exist_ok=True)
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": TRACK, "B3_RES": RES, "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1",
        "B3_CAM": STATIC_CAM,
        "B3_SHOT_SEQ": outdir,
        "B3_SHOT_FIRST": str(STATIC_FIRST), "B3_SHOT_EVERY": "1",
        "B3_EXIT_AT": str(int((STATIC_FIRST + STATIC_N) / 60) + 3),
        "B3_AFX": "1",
    })
    env["B3_RT"] = "0"       # see the note in shot(): the option persists
    for k in list(FX_ENV.values()) + ["B3_PHOTO"]:
        env.pop(k, None)
    env.update(env_extra)
    subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fs = sorted(f for f in os.listdir(outdir) if f.endswith(".bmp"))
    return [arr(os.path.join(outdir, f)) for f in fs[:STATIC_N]]


def section_temporal():
    print("\n== 6. TEMPORAL STABILITY: the layer must not make it flicker ==")

    def unsteadiness(frames):
        """Mean |frame N - frame N-1| over a PINNED camera."""
        return float(np.mean([np.abs(frames[i] - frames[i - 1]).mean()
                              for i in range(1, len(frames))]))

    off = shot_seq("static_off", {"B3_PHOTO": "0"})
    on = shot_seq("static_all", {"B3_PHOTO": "1"})
    if not check(len(off) >= 10 and len(on) >= 10,
                 "the pinned-camera sequences rendered",
                 "%d off, %d on frames" % (len(off), len(on))):
        return
    u_off = unsteadiness(off)
    u_on = unsteadiness(on)
    # THE THRESHOLD.  The reference is not zero -- traffic drives through the
    # shot and the sky's cloud layers scroll -- so the claim is a RATIO: the
    # layer may not more than double what the frame was already doing, and the
    # absolute addition must stay under a fifth of a level, which is below what
    # anyone can see on a static picture.  MEASURED at the time of writing:
    # 0.215 off, 0.232 on.
    check(u_on < u_off * 2.0 and (u_on - u_off) < 0.20,
          "all seven add no visible frame-to-frame instability on a PINNED "
          "camera", "off %.3f -> on %.3f levels a frame" % (u_off, u_on))

    # ...and the same question asked of the one effect that could plausibly
    # answer it badly: the occlusion carries a per-pixel dither, and a dither
    # that is locked to the SCREEN swims over a moving world.
    ao = shot_seq("static_ssao", {
        "B3_PHOTO": "1", "B3_PHOTO_TONEMAP": "0", "B3_PHOTO_ATMOS": "0",
        "B3_PHOTO_SHADOW": "0", "B3_PHOTO_SSR": "0", "B3_PHOTO_GODRAY": "0",
        "B3_PHOTO_SSAO": "1"})
    if len(ao) >= 10:
        u_ao = unsteadiness(ao)
        check(u_ao < u_off * 2.0 and (u_ao - u_off) < 0.20,
              "the occlusion's dither does not swim -- the bilateral blur is "
              "eating it", "off %.3f -> ssao %.3f" % (u_off, u_ao))

    # ...and of tier 4r, which is the OTHER effect with a per-pixel pattern
    # in it: the sun's disc is sampled on a golden-angle spiral rotated by a
    # hash of gl_FragCoord.  That rotation is SCREEN-LOCKED and carries no
    # time, deliberately -- an animated one would look strictly better in
    # motion and would turn this leg red, which is the correct outcome for a
    # port with no TAA in it.  There is no blur eating this one, so the claim
    # is stronger than the occlusion's: the pattern must not move AT ALL
    # beyond what the world underneath it is already doing.
    rt = shot_seq("static_rt", {
        "B3_PHOTO": "1", "B3_PHOTO_TONEMAP": "0", "B3_PHOTO_ATMOS": "0",
        "B3_PHOTO_SSAO": "0", "B3_PHOTO_SSR": "0", "B3_PHOTO_GODRAY": "0",
        "B3_PHOTO_LIGHTS": "0", "B3_PHOTO_SHADOW": "1", "B3_RT": "1"})
    if len(rt) >= 10:
        u_rt = unsteadiness(rt)
        check(u_rt < u_off * 2.0 and (u_rt - u_off) < 0.20,
              "the RAY's cone dither is screen-locked and does not swim "
              "either -- there is no TAA here and an animated rotation would "
              "correctly fail this",
              "off %.3f -> ray %.3f" % (u_off, u_rt))


# ======================================================================
# 7  THE SUN SHADOW ON THE ROAD
# ======================================================================
# WHY THIS SECTION EXISTS.  A player reported "a large shadow travelling with
# the car spanning the road", and it was real: the shadow map's NORMAL OFFSET
# was scaled by `1 / max(N.L, 0.15)`, so on a road under a low sun the lookup
# walked several metres sideways across the map and every near-field road pixel
# read the shadow of whatever stood ten to twenty metres down-sun of it.  Since
# the offset is faded out with distance (`nconf`), the false shade stopped at
# about two hundred metres and travelled with the camera.
#
# The three checks below are the three ways that failure was pinned down, kept
# as gates: the offset is bounded by construction, the road does not shadow
# itself, and the caster list is what the pass says it is.
SHADOW_DRIVE_FRAME = 460


def road_patch(a):
    """The tarmac immediately in front of the camera.

    NOT `bands()["near"]`, and the difference is the whole measurement: the
    near band is a third of the frame and most of it is the car, the kerbs,
    the shopfronts and the HUD, all of which legitimately move.  The strip
    below is road and only road on this track at these frames, which is what
    lets it carry a tight threshold -- averaged over the near band the same
    defect diluted to under half a level and nothing would have failed.
    """
    h, w, _ = a.shape
    return a[int(h * 0.84):int(h * 0.99), int(w * 0.25):int(w * 0.78)]


def car_shadow_patch(a):
    """The tarmac the player's own TRACED shadow falls on.

    NOT road_patch(), and the difference was measured rather than assumed: at
    this track and these frames the sun throws the car's shadow out to one
    side, so road_patch() -- the strip directly in front of the camera --
    contains the retail BLOB and none of the traced shadow, and reads
    identically with the car's instance in the trace and out of it.  The
    window below was located by differencing those two frames cell by cell and
    taking where the difference actually was: rows 64-80%, columns 54-68%,
    which is road beside the car and not the car.

    Like road_patch() it is a claim about ONE track at ONE frame, and like
    road_patch() it is worth its narrowness -- averaged over anything wider the
    15-level signal dilutes into the noise of a moving frame."""
    h, w, _ = a.shape
    return a[int(h * 0.64):int(h * 0.80), int(w * 0.54):int(w * 0.68)]


def section_shadow_road():
    print("\n== 7. THE SUN SHADOW ON THE ROAD ==")
    src = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()

    # ---- the law, in the source -------------------------------------------
    # The offset moves the LOOKUP, and what it buys is a displacement in the
    # map's own plane: `d * sin(angle(N, L))` texels sideways.  Scaling `d` by
    # that same sine bounds the walk at one `uSh2.w`; scaling it by a
    # RECIPROCAL of N.L is unbounded, and that was the bug.
    # THE MAP's offset, and only the map's: tier 4r starts its ray from
    # `P + Nw * uRt.y`, which is a fixed few centimetres and has nothing to
    # do with a texel (the tier-4r section gates that one).  Matching on the bare
    # prefix found both the moment tier 4r landed and reported the defect
    # this section is named after as present again.
    off_line = [l for l in src.splitlines()
                if "vec3 Ps = P + Nw" in l and "uSh2.w" in l]
    check(len(off_line) == 1,
          "the shadow MAP's normal offset is one line",
          "%d found" % len(off_line))
    if off_line:
        l = off_line[0]
        check("/ max(ndl" not in l and "/max(ndl" not in l,
              "the normal offset is NOT scaled by a reciprocal of N.L -- an "
              "unbounded sideways walk across the map is the "
              "shadow-that-follows-the-car defect", l.strip())
        check("nsl" in l,
              "...it is scaled by sin(angle(N, L)), which bounds the walk at "
              "one offset", l.strip())
    check("float nsl = sqrt(max(0.0, 1.0 - ndl * ndl))" in src,
          "...and that sine is computed the guarded way")

    # ---- the coverage boundary: OUTSIDE the cascade is LIT ------------------
    # `inbox` is the whole boundary policy.  A pixel the box does not cover
    # must come back with sh = 0, i.e. FULL SUN -- the alternative (shadowed)
    # would be a moving band across the road at the box edge.
    check("float inbox = step(0.0, min(e.x, e.y))" in src,
          "outside the cascade resolves LIT: `inbox` is zero there, so the "
          "shadow term is zero and the pixel is untouched")
    check("clamp(min(e.x, e.y) / max(1.0 - uSh2.x, 1e-3)" in src,
          "...and the edge is FEATHERED rather than cut, over B3_PHOTO_SH_FADE")

    if not os.path.isfile(GAME):
        return

    # ---- EXECUTED: the road does not shadow itself -------------------------
    # With the track as the ONLY caster there is nothing on the road to cast
    # anything, so the shadow tier must move essentially nothing.  Anything it
    # does move is the road shadowing itself -- acne, or a lookup that walked.
    # SUN_DIRECT=0 as well, and it is not optional: tier 4b's relight is on
    # the same switch as the map and it touches half the frame by design, so a
    # leg that left it on would be measuring the relight and calling it acne.
    #
    # AND LIGHTS=0, for exactly the same reason, which this leg got away with
    # not saying for as long as tier 7's headlight beams put nothing on the
    # road: they now put a pool there deliberately, and a leg that leaves them
    # on measures that pool and calls it shadow acne.  (It failed the moment
    # the beams started working, at 99% of the road strip -- which is its own
    # small piece of evidence about how invisible they had been.)
    def leg(name, extra, frame=SHADOW_DRIVE_FRAME):
        env = {"B3_PHOTO": "1", "B3_PHOTO_TONEMAP": "0", "B3_PHOTO_SSAO": "0",
               "B3_PHOTO_ATMOS": "0", "B3_PHOTO_SSR": "0",
               "B3_PHOTO_GODRAY": "0", "B3_PHOTO_SHADOW": "1",
               "B3_PHOTO_LIGHTS": "0", "B3_PHOTO_SUN_DIRECT": "0"}
        env.update(extra)
        return shot(name, env, frame=frame)

    # TWO MOMENTS, because this is the one thing in the file that has to be
    # true while DRIVING rather than on one pinned frame: the artefact this
    # section exists for was invisible in a still and unmistakable in motion.
    for fr in (420, SHADOW_DRIVE_FRAME):
        base = shot("sh_road_off_%d" % fr, {"B3_PHOTO": "0"}, frame=fr)
        self_ = leg("sh_road_track_%d" % fr,
                    {"B3_PHOTO_SH_CASTERS": "track"}, frame=fr)
        if check(base and self_,
                 "the road-only shadow legs rendered (frame %d)" % fr):
            o, a = arr(base), arr(self_)
            if world_pin(o, a) < PIN_MIN:
                print("      ! the world would not settle; the number below "
                      "is not trustworthy")
            moved = float((np.abs(road_patch(a) - road_patch(o))
                           .max(axis=2) > 4).mean() * 100)
            check(moved < 2.0,
                  "frame %d: with the TRACK as the only caster the road is "
                  "uniformly sun-lit -- it does not shadow itself" % fr,
                  "%.2f%% of the road strip moved by more than 4 levels"
                  % moved)

    # ---- EXECUTED: the offset may not INVENT shadow on the road ------------
    # This is the check that bites on the defect this section is named after,
    # and it asks the question the defect answers wrongly: does turning the
    # normal offset on put shade on the tarmac in front of the car?  It must
    # not -- a bounded offset moves a shadow EDGE by a texel and leaves open
    # road alone.
    #
    # MEASURED on the same driving frame, same legs, same strip: with the
    # offset scaled by `1 / max(N.L, 0.15)` the road went -15.37 levels; with
    # it scaled by the sine, -0.66.  The threshold sits between them with a
    # factor of five of headroom either way.
    for fr in (420,):
        ship = leg("sh_off_ship_%d" % fr, {}, frame=fr)
        zero = leg("sh_off_zero_%d" % fr, {"B3_PHOTO_SH_NORMOFF": "0"},
                   frame=fr)
        if check(ship and zero,
                 "the normal-offset legs rendered (frame %d)" % fr):
            s, z = arr(ship), arr(zero)
            if world_pin(s, z) < PIN_MIN:
                print("      ! the world would not settle; the number below "
                      "is not trustworthy")
            d = float(lum(road_patch(s)).mean() - lum(road_patch(z)).mean())
            check(abs(d) < 4.0,
                  "frame %d: the shadow lookup's normal offset does not "
                  "INVENT shade on the open road -- its sideways walk is "
                  "bounded by the map's own texel" % fr,
                  "%+.2f levels on the road strip" % d)

    # ---- EXECUTED: the caster list is what the pass says it is -------------
    # Not a re-reading of the call site: the renderer counts the draws that
    # reached the map, and the car mesh -- which draws through a raw
    # glDrawArrays -- counts itself.  "The cars are not casters" is the reason
    # retail's recovered blob shadow is not fought with, so it is a claim
    # worth executing.
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({"B3_TRACK": TRACK, "B3_RES": RES, "B3_TESTDRIVE": "1",
                "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
                "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1", "B3_AFX": "1",
                "B3_PHOTO": "1", "B3_PHOTO_VERBOSE": "1",
                "B3_SHOT": os.path.join(OUT, "sh_casters.bmp"),
                "B3_SHOT_FRAME": str(SHADOW_DRIVE_FRAME)})
    for k in list(FX_ENV.values()):
        env.pop(k, None)
    os.makedirs(OUT, exist_ok=True)
    p = subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = p.stdout.decode("utf-8", "replace")
    line = [l for l in log.splitlines() if "shadow casters:" in l]
    if check(bool(line), "the caster inventory was printed"):
        m = re.search(r"track (\d+) \+ props (\d+) \+ scenery (\d+) = "
                      r"(\d+) draws in the map; cars (\d+)", line[0])
        if check(bool(m), "...and it parses", line[0].strip()):
            t, pr, sc, tot, cars = (int(x) for x in m.groups())
            check(cars == 0,
                  "the CARS are not shadow casters -- they keep retail's own "
                  "recovered blob shadow, and a second car shadow would fight "
                  "it", "%d car spans reached the map" % cars)
            check(t + pr + sc == tot and tot > 0,
                  "...and NOTHING ELSE is either: the map's draw count is "
                  "exactly track + props + scenery",
                  "%d + %d + %d = %d" % (t, pr, sc, tot))


# ======================================================================
# 8  TIER 7: THE PER-SOURCE LIGHTS
# ======================================================================
# The interesting claim about this tier is not that it lights things, which
# section 3 checks, but WHERE ITS LIGHTS COME FROM.  There is no light table
# on the disc; the positions and the colours are derived from the scenery
# models' own art, per model, once at load.  So what is gated here is that the
# derivation is DATA and not authorship: it runs, it finds different lights on
# different tracks, no track name appears anywhere near it, and the day/dusk
# behaviour is a function of the track's own sun rather than of a list.
LIGHT_TRACK_B = os.environ.get("B3_PHOTO_LIGHT_TRACK_B", "US_P1_V1")


def light_run(name, track, extra, frame=460):
    """One run, returning (log, frame array or None)."""
    os.makedirs(OUT, exist_ok=True)
    bmp = os.path.join(OUT, name + ".bmp")
    if os.path.exists(bmp):
        os.remove(bmp)
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({"B3_TRACK": track, "B3_RES": RES, "B3_TESTDRIVE": "1",
                "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
                "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1", "B3_AFX": "1",
                "B3_PHOTO": "1", "B3_PHOTO_VERBOSE": "1",
                "B3_SHOT": bmp, "B3_SHOT_FRAME": str(frame)})
    for k in list(FX_ENV.values()):
        env.pop(k, None)
    env.update(extra)
    p = subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = p.stdout.decode("utf-8", "replace")
    return log, (arr(bmp) if os.path.isfile(bmp) else None)


DERIVED = re.compile(r"(\d+) per-source lights derived from (\d+) of (\d+) "
                     r"scenery models")


def section_lights():
    print("\n== 8. TIER 7: THE PER-SOURCE LIGHTS ==")
    hdr = open(os.path.join(ROOT, "src", "burnout3_aftereffects.h")).read()
    scn = open(os.path.join(ROOT, "src", "burnout3_scenery.c")).read()

    # ---- the provenance, written down where it can be found ---------------
    check("There is NO light table in this game" in hdr,
          "the header says what retail has and does not: NO light table "
          "anywhere in the recovered formats")
    check("model+0x1664" in hdr and "RE_CARFX" in hdr,
          "...and cites the ONE light table retail does have -- the per-CAR "
          "corona records in the .bgv model -- as the shape this follows")

    # ---- NOT PER-TRACK.  The standing directive, checked mechanically.
    # COMMENTS ARE STRIPPED FIRST, and that is not a loophole: a comment
    # citing a track it was MEASURED on ("0.21..0.39 across US_C1_V1's 1370
    # instances") is provenance and is exactly what this tree asks for.  What
    # must not exist is a track id the CODE reads.
    code = re.sub(r"/\*.*?\*/", "", scn, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    tracks = re.findall(r"\b(?:US|EU|AS)_[CMP]\d_V\d\b", code)
    check(not tracks,
          "the derivation names no TRACK: it is per MODEL, so a track this "
          "port has never seen gets its lights for free",
          ("found: " + ", ".join(sorted(set(tracks)))) if tracks else
          "no track id in src/burnout3_scenery.c")
    check("b3_photo_light_rules" in scn,
          "...and every threshold it derives with is a header constant with "
          "an env of its own, not a number in the loader")

    if not os.path.isfile(GAME):
        return

    # ---- EXECUTED: it derives, and it derives from the ART -----------------
    log_a, _ = light_run("li_a", TRACK, {})
    ma = DERIVED.search(log_a)
    n_a = mod_a = 0
    if check(bool(ma), "the derivation ran and reported itself (%s)" % TRACK,
             ma.group(0) if ma else "no derivation line in the log"):
        n_a, lit_a, mod_a = (int(x) for x in ma.groups())
        check(n_a > 0 and lit_a > 0,
              "%s: lights were derived from the scenery's own art" % TRACK,
              "%d lights from %d of %d models" % (n_a, lit_a, mod_a))
        check(lit_a < mod_a,
              "...and NOT from all of it -- a derivation that called every "
              "model a light would be a derivation that decided nothing",
              "%d of %d models" % (lit_a, mod_a))
        cols = re.findall(r"light model .*rgb ([\d.]+) ([\d.]+) ([\d.]+)",
                          log_a)
        check(len(cols) == lit_a,
              "...and each one carries a colour taken from its own texels",
              "%d colours for %d models" % (len(cols), lit_a))

    # ---- EXECUTED: a DIFFERENT TRACK, a different field --------------------
    log_b, _ = light_run("li_b", LIGHT_TRACK_B, {})
    mb = DERIVED.search(log_b)
    if not mb:
        print("      ! %s not available here -- the cross-track leg is "
              "SKIPPED, not passed" % LIGHT_TRACK_B)
    elif ma:
        n_b, lit_b, mod_b = (int(x) for x in mb.groups())
        check(n_b > 0 and (n_b, mod_b) != (n_a, mod_a),
              "a second track derives a DIFFERENT field from the same binary "
              "-- the light list is the track's art, not this port's opinion",
              "%s %d lights / %d models, %s %d / %d"
              % (TRACK, n_a, mod_a, LIGHT_TRACK_B, n_b, mod_b))

    # ---- EXECUTED: the DUSK factor is the track's own sun ------------------
    da = re.search(r"dusk ([\d.]+)", log_a)
    db = re.search(r"dusk ([\d.]+)", log_b) if mb else None
    if check(bool(da), "the dusk factor is reported"):
        va = float(da.group(1))
        check(0.0 <= va <= 1.0, "%s: the dusk factor is in range" % TRACK,
              "%.2f" % va)
        if db:
            vb = float(db.group(1))
            check(vb > va,
                  "%s reads later in the day than %s -- derived from "
                  "enviro.dat's own sun elevation and colour, with no track "
                  "named anywhere" % (LIGHT_TRACK_B, TRACK),
                  "%s %.2f vs %s %.2f" % (LIGHT_TRACK_B, vb, TRACK, va))

    # ---- EXECUTED: day is SUBTLE, dusk SINGS -------------------------------
    only = {FX_ENV[t]: ("1" if t == "lights" else "0") for t in FX}
    base = shot("li_off", {"B3_PHOTO": "0"}, frame=460)
    # THE BEAMS STAND DOWN FOR THIS PAIR, and the sentence being gated says
    # why: "a LAMP at noon is a lamp that is on and losing".  That is the
    # streetlights' curve (B3_PHOTO_LIGHT_DAY 0.22) and it is not the beams',
    # which ride their own floor for the opposite reason -- a headlight the
    # player cannot see in daylight is the defect this wave exists to fix.
    # Leaving them in measured both curves and attributed the sum to the one
    # named in the check.
    e_day = dict(only, B3_PHOTO_HEAD_ON="0")
    e_day["B3_PHOTO_LIGHT_DUSK"] = "0"
    e_dusk = dict(only, B3_PHOTO_HEAD_ON="0")
    e_dusk["B3_PHOTO_LIGHT_DUSK"] = "1"
    _, a_day = light_run("li_day", TRACK, e_day)
    _, a_dusk = light_run("li_dusk", TRACK, e_dusk)
    if check(bool(base) and a_day is not None and a_dusk is not None,
             "the day and dusk legs rendered"):
        o = arr(base)
        d_day = float(lum(a_day).mean() - lum(o).mean())
        d_dusk = float(lum(a_dusk).mean() - lum(o).mean())
        check(d_dusk > d_day * 2.0,
              "DUSK SINGS AND DAY IS RESTRAINED: the same lamps on the same "
              "frame are worth more than twice as much at the roster's dusk "
              "end", "day %+.2f vs dusk %+.2f levels" % (d_day, d_dusk))
        check(abs(d_day) < 3.0,
              "...and by day they stay under three levels of frame mean -- a "
              "lamp at noon is a lamp that is on and losing",
              "%+.2f levels" % d_day)

    # ---- EXECUTED: the budget is TWO budgets --------------------------------
    #
    # It used to be one, with the beams carved out of B3_PHOTO_LIGHT_N and
    # capped at half of it.  That held at the shipped N=18 -- measured, 6 beams
    # on 6 of 6 racers in every one of 7745 frames across both test tracks --
    # and it silently failed below it: at N=8 the cap is 4 and two of six
    # racers drove with no beam at all.  A guarantee that holds only at the
    # default is not a guarantee, so the reservations now sit side by side:
    # N is exactly what its name says, how many STREETLIGHTS a frame
    # accumulates, and the beams are added on top.  N=4 is the leg because it
    # is the setting the old spelling broke on.
    e4 = dict(only)
    e4["B3_PHOTO_LIGHT_N"] = "4"
    log4, _ = light_run("li_n4", TRACK, e4)
    # FOUR RESERVATIONS NOW, not two: tiers 7c and 7d joined the line and the
    # verbose pick prints all of them.  The claim is unchanged in shape -- the
    # shader declares exactly the SUM, and lowering N cannot take a slot from
    # any class of car lamp -- so the leg gained two terms and no new idea.
    picks = re.findall(r"(\d+) lamps \+ (\d+) headlamps \+ (\d+) taillamps "
                       r"\+ (\d+) flames = (\d+) of a budget of (\d+)", log4)
    if check(bool(picks), "the per-frame pick reports itself"):
        budget = int(picks[0][5])
        lamps = max(int(p[0]) for p in picks)
        heads = max(int(p[1]) for p in picks)
        tails = max(int(p[2]) for p in picks)
        worst = max(int(p[4]) for p in picks)
        check(budget == 4 + heads + TAIL_SLOTS + BOOST_SLOTS
              and worst <= budget,
              "B3_PHOTO_LIGHT_N is the STREETLIGHTS' budget and every class of "
              "car lamp is reserved beside it, so the shader declares exactly "
              "the sum",
              "4 lamps + %d beams + %d tail slots + %d flame slots = a budget "
              "of %d, worst frame %d"
              % (heads, TAIL_SLOTS, BOOST_SLOTS, budget, worst))
        flames = max(int(p[3]) for p in picks)
        worst_car = max(int(p[1]) + int(p[2]) + int(p[3]) for p in picks)
        check(worst_car <= HEAD_SLOTS + TAIL_SLOTS + BOOST_SLOTS,
              "...and no class of car lamp can spend the lamps' half of it "
              "either: every one of them stays inside its own reservation, so "
              "B3_PHOTO_LIGHT_N is a FLOOR on the street and not a ceiling",
              "%d beams + %d tails + %d flames at worst against %d + %d + %d "
              "reserved" % (heads, tails, flames, HEAD_SLOTS, TAIL_SLOTS,
                            BOOST_SLOTS))
        # ...AND THE SPARE FALLS TO THE STREET, which is why this leg no
        # longer asserts `lamps <= 4`.  b3_photo_head_slots() has always
        # promised that "whatever it does not use falls to the lamps, so a
        # race with two cars in it does not waste four slots on cars that are
        # not there" -- and with three classes of car lamp there is nearly
        # always something spare: several shipped models carry no type-1 tail
        # records at all, and on most frames nobody is boosting, so N=4
        # measures SEVEN streetlights here.  That is the promise being kept,
        # not the street being robbed; what would be a defect is the street
        # dropping BELOW N, and that is what is gated.
        check(lamps >= 4,
              "...and the street reaches all four of its own: an unused car-"
              "lamp slot (a model with no tail lamps, a frame with no flame) "
              "falls to the lamps rather than idling, exactly as the beams' "
              "reservation has always promised",
              "%d lamps at best against N=4" % lamps)


# ======================================================================
# 9  THE NEAR-FIELD BLANKET -- at the user's own track, size and frame
#
# A player reported the whole near-field road as a uniform dark blanket with a
# straight edge at mid-distance beyond which the world was sunlit, travelling
# with the camera.  The evidence was build/debug_dump_089.bmp: US_C1_V1, frame
# 655, 2048x1536 -- the size the game BOOTS INTO, which is why this section
# does not use the 1024x768 the identity leg is pinned to.  Nothing here is
# resolution-dependent in principle; it is measured at the reported size
# because that is where it was reported, and again at a second size so that a
# pass which only behaves at one of them cannot hide.
#
# The measurement that mattered was not "how dark is the road" -- an occlusion
# term is ALLOWED to darken a road it can see a reason to darken.  It is
# whether the pass can tell an open road from a real contact at all.  Before
# the fix, open flat road measured -42.62 levels and a genuine wheel-to-road
# contact -42.82: the same number, i.e. no discrimination whatever, which is
# what a blanket IS.
# ======================================================================
BLANKET_TRACK = "US_C1_V1"
BLANKET_FRAME = 655
# Boxes in FRACTIONS of the frame, so the same two regions are measured at
# whatever size the leg is run at.  Chosen on this frame: open carriageway to
# the right of the player, and the shaded contact under the parked bus.
BLANKET_OPEN = (0.60, 0.72, 0.96, 0.95)
BLANKET_CONTACT = (0.679, 0.547, 0.762, 0.596)


def _box(a, frac):
    h, w = a.shape[0], a.shape[1]
    x0, y0, x1, y1 = frac
    return a[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]


def shot_log(name, env_extra, frame=SHOT_FRAME, res=RES):
    """shot(), but the run's stdout comes back -- some of what this file has
    to check is something the engine PRINTS about itself rather than something
    it draws (the shadow map's own coverage, for one)."""
    os.makedirs(OUT, exist_ok=True)
    bmp = os.path.join(OUT, name + ".bmp")
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({"B3_TRACK": TRACK, "B3_RES": res, "B3_TESTDRIVE": "1",
                "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
                "B3_SHOT": bmp, "B3_SHOT_FRAME": str(frame), "B3_AFX": "1",
                "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1"})
    # B3_RT with the rest of them, and it is NOT decoration: main() pins the
    # option off for the whole suite (see the note there), so a leg that
    # inherits that pin and then asks the pause menu to toggle the row finds
    # it correctly GREYED -- "pinned by B3_RT for this run" -- and the toggle
    # correctly refused.  The tier-4r legs set what they need explicitly.
    #
    # ENDING THE RUN: this function has no B3_EXIT_AT and does not want one.
    # B3_SHOT_FRAME ends it instead, and that matters for the pause legs --
    # an open pause overlay FREEZES the race clock, so a run ended by race
    # time would hang until `timeout 900` killed it, four times over.
    for k in list(FX_ENV.values()) + ["B3_PHOTO", "B3_RT"]:
        env.pop(k, None)
    env.update(env_extra)
    p = subprocess.run(["timeout", "900", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.stdout.decode("utf-8", "replace")


def section_blanket():
    print("\n== 9. THE NEAR-FIELD BLANKET (the user's own track and size) ==")
    src = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()
    ren = open(os.path.join(ROOT, "src", "burnout3_render.c")).read()

    # ---- the law, in the source -------------------------------------------
    # The occlusion pass rasterises at HALF resolution and every use of
    # `uTexel` in it is a step to a neighbouring pixel of ITS OWN grid.  Handed
    # the scene's full-res texel, the step lands inside the same half-res pixel
    # and the reconstructed normal comes out horizontal on flat road.
    ao_texel = [l for l in src.splitlines() if "g_p_ao.uTexel," in l]
    check(len(ao_texel) == 1, "the occlusion pass' texel is uploaded once",
          "%d found" % len(ao_texel))
    if ao_texel:
        check("g_t[T_AO].w" in ao_texel[0],
              "the occlusion pass is handed ITS OWN texel, not the scene's -- "
              "a half-res pass stepping by a full-res texel reconstructs its "
              "normal from inside its own pixel", ao_texel[0].strip())
    check("uAOAmt.w" in src,
          "the occlusion has an ANGLE BIAS: max(0, sine) on a flat surface "
          "rectifies the reconstruction noise into a positive DC, and the "
          "gain then multiplies it into a wash")
    check("uAO.x * (sr / sr0)" in src,
          "...and the range taper uses the radius the SAMPLED DISC covers, so "
          "a bound screen-radius cap degrades to a smaller-radius occlusion "
          "instead of to a ramp in distance")
    # The shadow pass' own half of the report: its projection carries no
    # display mirror, so it must not inherit the world's winding.
    sh = ren[ren.find("int b3r_shadow_begin("):ren.find("void b3r_shadow_end(")]
    check("b3r_gl_front_face(GL_CW)" in sh,
          "the sun shadow pass sets its OWN winding -- its ortho carries no "
          "display mirror, so inheriting the world's CCW culls exactly the "
          "faces that should have been the casters")

    # ---- executed, at the reported size and at a second one ---------------
    for res in ("2048x1536", "1280x960"):
        tag = res.replace("x", "_")
        env = {"B3_TRACK": BLANKET_TRACK}
        ref = shot("blanket_off_" + tag, dict(env, B3_PHOTO="0"),
                   frame=BLANKET_FRAME, res=res)
        one = shot("blanket_ssao_" + tag,
                   dict(env, B3_PHOTO="1", B3_PHOTO_TONEMAP="0",
                        B3_PHOTO_ATMOS="0", B3_PHOTO_SHADOW="0",
                        B3_PHOTO_SSR="0", B3_PHOTO_GODRAY="0",
                        B3_PHOTO_LIGHTS="0", B3_PHOTO_SSAO="1"),
                   frame=BLANKET_FRAME, res=res)
        if not check(bool(ref) and bool(one),
                     "the blanket legs rendered at %s" % res):
            continue
        a, b = lum(arr(ref)), lum(arr(one))
        open_d = _box(b[..., None], BLANKET_OPEN).mean() \
            - _box(a[..., None], BLANKET_OPEN).mean()
        cont_d = _box(b[..., None], BLANKET_CONTACT).mean() \
            - _box(a[..., None], BLANKET_CONTACT).mean()
        check(open_d > -12.0,
              "%s: the OPEN near-field road still reads sunlit -- the "
              "occlusion does not blanket it" % res,
              "%+.2f levels (the reported defect measured -42.6)" % open_d)
        check(cont_d < open_d - 4.0 and cont_d < -4.0,
              "%s: ...and a REAL contact is still shaded, so the pass is "
              "measuring geometry rather than applying a wash" % res,
              "contact %+.2f vs open road %+.2f levels" % (cont_d, open_d))
        check(cont_d < open_d * 2.0,
              "%s: the pass can TELL THEM APART -- the blanket was exactly "
              "the failure to" % res,
              "contact is %.1fx the open road (the defect was 1.0x)"
              % (cont_d / open_d if open_d else 0.0))

    # ---- and the map is no longer empty of its own casters ---------------
    dump = os.path.join(OUT, "blanket_map.pgm")
    log = shot_log("blanket_map",
                   {"B3_TRACK": BLANKET_TRACK, "B3_PHOTO": "1",
                    "B3_PHOTO_SH_DUMP": dump,
                    "B3_PHOTO_SH_DUMP_AT": str(BLANKET_FRAME)},
                   frame=BLANKET_FRAME, res="2048x1536")
    m = re.search(r"([0-9.]+)% at the far plane", log)
    if check(bool(m), "the shadow map reports its own coverage"):
        far = float(m.group(1))
        check(far < 60.0,
              "the sun shadow map is not empty of the casters it was handed "
              "-- with the winding inverted the single-sided track mesh was "
              "culled straight back out of it",
              "%.1f%% of the map at the far plane (inverted: 67.3%%)" % far)


# ======================================================================
# 10  THE CARS' BEAMS LIGHT THE ROAD AHEAD OF THEM
#
# The same report: "the opponent's car shows a bright blue-white glow at its
# REAR, and no light pools appear on the road ahead of any car anywhere".  Tier
# 7b indexed the corona pass' row-major object->world rotation down its columns
# instead -- the TRANSPOSE, which for a rotation is the inverse -- so the beam
# was placed and aimed by the car's rotation run backwards: it left the tail
# and lit the road the car had already driven over.
#
# The leg lights ONE car (the player) so the measurement cannot be confused by
# five other beams, and asks where the light landed.
# ======================================================================
def section_beams():
    print("\n== 10. THE CARS' BEAMS LIGHT THE ROAD AHEAD ==")
    full = open(os.path.join(ROOT, "src", "burnout3_full.c")).read()
    blk = full[full.find("TIER 7b: THE CARS' OWN HEADLIGHTS"):]
    blk = blk[:blk.find("nch = nhead;")]
    # world.x = R[0]*m.x + R[1]*m.y + R[2]*m.z -- the corona pass' own spelling
    check("wx = R[0]*sp[0] + R[1]*sp[1] + R[2]*sp[2];" in blk,
          "the beam's ORIGIN uses R the way the corona pass uses it "
          "(row-major object->world), not its transpose")
    check("dx = R[0]*sn[0] + R[1]*sn[1] + R[2]*sn[2];" in blk,
          "...and so does its AIM -- a transposed rotation is the inverse "
          "one, which points the beam out of the tail")
    # THE DUSK FLOOR MOVED TO THE SHADER, next to the beams' gain, their wrap
    # and their lit-bias -- which is where it belonged: while the lamps and
    # the beams shared one uniform gain the only way for the beams to have
    # their own day curve was to smuggle the RATIO of the two through the
    # per-light COLOUR and let the shader cancel it.  So the claim is checked
    # where it now lives, and the caller is checked for NOT doing it any more.
    afx = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()
    check("g_pk.hd_day + (1.0f - g_pk.hd_day) * g_light_dusk" in afx,
          "the beams ride dusk LINEARLY off a floor, so they are on in "
          "daylight: squared, US_C1_V1's 0.14 became 0.02 and a track that "
          "spends a third of its lap under a stadium deck got nothing")
    check("* dsk * dsk" not in blk and "hmix / (gmix" not in blk,
          "...and the caller no longer divides the lamps' gain back out of "
          "the beam COLOUR to fake a curve the shader now owns")
    # THE WRAP TERM, which is the whole of round three.  N.L on a road lit by
    # a lamp 0.615 m up is 0.02-0.12; on the wall beside it, 1.0.  That is the
    # entire difference between the tunnel shot that passed and the open road
    # the player was looking at.
    check("mix(nl, 1.0, uHeadK.z)" in afx,
          "the beams take a WRAP term the scenery lamps do not: a headlight "
          "grazes the road at two degrees and a cosine throws that away")
    check("acc += uLightC[i].rgb * (att * nl);" in afx,
          "...and the scenery lamps keep the plain cosine, so a streetlight "
          "is still a streetlight")

    base = {"B3_TRACK": BLANKET_TRACK, "B3_PHOTO": "1",
            "B3_PHOTO_TONEMAP": "0", "B3_PHOTO_SSAO": "0",
            "B3_PHOTO_ATMOS": "0", "B3_PHOTO_SHADOW": "0",
            "B3_PHOTO_SSR": "0", "B3_PHOTO_GODRAY": "0",
            "B3_PHOTO_LIGHTS": "1", "B3_PHOTO_HEAD_CARS": "1",
            "B3_PHOTO_HEAD_DAY": "1.0"}
    off = shot("beam_off", dict(base, B3_PHOTO_HEAD_ON="0"),
               frame=BLANKET_FRAME, res="2048x1536")
    on = shot("beam_on", base, frame=BLANKET_FRAME, res="2048x1536")
    if not check(bool(off) and bool(on), "the beam legs rendered"):
        return
    a, b = lum(arr(off)), lum(arr(on))
    d = b - a
    lit = d > 2.0
    if not check(lit.sum() > 2000,
                 "the player's beam puts a POOL on the road -- an invisible "
                 "headlight is the other half of the report",
                 "%d pixels lifted" % int(lit.sum())):
        return
    h = d.shape[0]
    rows = np.arange(h)[:, None] * np.ones((1, d.shape[1]))
    w = d[lit]
    cy = (rows[lit] * w).sum() / w.sum() / h
    # THE CHASE CAMERA SITS BEHIND THE CAR, so the road AHEAD of it is higher
    # up the frame and the tail is lower.  0.62 is below the car's own rear
    # bumper on this frame; the transposed beam put 100% of its light there.
    ahead = d[lit & (rows < 0.62 * h)].sum() / w.sum()
    check(ahead > 0.95,
          "...and the pool is AHEAD of the car, not against its tail",
          "%.1f%% of the light forward of the bumper, centroid at y=%.3f "
          "(transposed: 0.0%%)" % (100.0 * ahead, cy))


# ======================================================================
# 12  THE BEAMS OVER A DRIVE, WHICH IS THE ONLY PLACE THEY COUNT
#
# THE SAME REPORT, A THIRD TIME: "I am still not seeing the player or opponent
# cars' headlights casting light onto the track."  Section 10 was green when
# that was written, and section 10 was not wrong -- it was measuring the wrong
# thing.  A pinned frame chosen because the effect shows on it, under a leg
# that lights ONE car with the day floor forced to 1.0, answers "can this beam
# put light on a road" and not "does a player see headlights".
#
# What the third round measured instead was an ordinary lap at the user's own
# 2048x1536, rendered twice, differenced over the road band, at six moments
# ten seconds apart.  On the shipped defaults the answer was 0.06, 1.67 and
# 2.33 levels on US_C1_V1's open sunlit road -- under one step of 8-bit
# quantisation.  The frames that DID pool were the tunnel and the barriers:
# every one of them a VERTICAL surface, because a lamp 0.615 m above the road
# meets that road at two to six per cent of full incidence and meets a wall at
# a hundred.
#
# So this section gates the two things a curated frame cannot see:
#
#   1  THE BEAMS ARE ADMITTED.  Every racer gets a slot, every frame, on a
#      real drive -- not "the player's is reserved and the rest take their
#      chances".  Parsed from the engine's own per-frame ledger.
#   2  THE POOL READS.  The post-tonemap delta over the road band, on the
#      WORST of six moments of ordinary driving, on both a day track and a
#      dusk one, must clear a floor that a player can actually see.
#
# THE FLOOR IS 8 LEVELS at the 99th percentile of the band.  Not a physicist's
# number: three levels is where 8-bit banding starts being visible on a smooth
# gradient, and a headlight the user has asked three times to be able to see
# needs headroom over "technically present".  The shipped build measures 94 to
# 232, so the floor is a regression alarm and not a target.
# ======================================================================
BEAM_DRIVE = [("US_C1_V1", 400, 100), ("US_P1_V1", 430, 80)]
BEAM_FLOOR = 8.0
# THE CEILING IS THE FOURTH ROUND OF THE SAME REPORT, in the other direction:
# "the headlights are too bright".  A floor on its own is a one-way gate, and
# a one-way gate is how a feature that could not be seen became a feature that
# was blown -- so the road pool now has both ends pinned.  80 is chosen against
# the two measurements it has to separate: the pre-tune wrap of 0.85 put
# US_C1_V1's least-lit moment at 111.9 and the shipped 0.40 puts it at 57.4, so
# 80 sits between them with room either side for a track this suite has never
# driven.
BEAM_CEIL = 80.0
# ...and the other end of the SAME tune: the underpass has to survive it.  The
# shipped build measures 176.1 on US_P1_V1's brightest moment against 229.0
# before, i.e. the tunnel kept 77% while the open road lost half.  140 is the
# alarm for a change that took the wall down with the road -- which is exactly
# what lowering GAIN instead of WRAP would have done.
BEAM_TUNNEL = 140.0


def beam_drive(track, on, first, every, n=6):
    """One leg of a driving sample: the BMP sequence plus the light ledger."""
    seq = os.path.join(OUT, "drive_%s_%d" % (track, on))
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
        "B3_TRACK": track, "B3_RES": "2048x1536", "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_AFX": "1", "B3_MUSIC_SEED": "1", "B3_TRACK_NOSHINE": "1",
        "B3_SHOT_SEQ": seq, "B3_SHOT_FIRST": str(first),
        "B3_SHOT_EVERY": str(every), "B3_PHOTO_LIGHT_STATS": "1",
        "B3_EXIT_AT": str(int((first + every * n) / 60.0) + 5),
        "B3_PHOTO_HEAD_ON": str(on),
    })
    p = subprocess.run(["timeout", "1200", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    frames = sorted(os.path.join(seq, f) for f in os.listdir(seq)
                    if f.startswith("frame_") and f.endswith(".bmp"))
    return p.stdout.decode("utf-8", "replace"), frames[:n]


LEDGER = re.compile(r"\[photo-lights\] f=(\d+) beams=(\d+) lamped=(\d+) "
                    r"racers=(\d+)")


def road_band(d):
    """Rows 42-62%, columns 15-85%.

    MEASURED, not guessed.  The chase camera hides the road from the car's
    nose out to about 10 m behind the car's own body, so the pool a headlight
    throws lands in the MIDDLE of the frame: the lit-pixel centroid over both
    test tracks sits at y = 0.49-0.54, and a "near field" band at the bottom
    of the frame is looking at bodywork.
    """
    h, w = d.shape
    return d[int(h * 0.42):int(h * 0.62), int(w * 0.15):int(w * 0.85)]


def section_beam_drive():
    print("\n== 12. THE BEAMS OVER A DRIVE ==")
    for track, first, every in BEAM_DRIVE:
        log_on, on = beam_drive(track, 1, first, every)
        _, off = beam_drive(track, 0, first, every)

        # ---- 1: every racer is admitted, every frame ----------------------
        led = LEDGER.findall(log_on)
        if check(len(led) > 300,
                 "%s: the per-frame light ledger ran over a real drive"
                 % track, "%d frames" % len(led)):
            starved = [(f, b, r) for f, b, _, r in led if int(b) < int(r)]
            worst = min(int(b) for _, b, _, _ in led)
            racers = max(int(r) for _, _, _, r in led)
            check(not starved,
                  "%s: BEAMS ADMITTED >= RACER COUNT in every frame of the "
                  "drive -- no racer ever loses its slot to a streetlight"
                  % track,
                  "%d beams worst frame, %d racers, %d starved frames of %d"
                  % (worst, racers, len(starved), len(led)))

        # ---- 2: the pool reads, on the worst of six moments ---------------
        if not check(len(on) >= 6 and len(off) >= 6,
                     "%s: both driving legs rendered six frames" % track,
                     "on %d, off %d" % (len(on), len(off))):
            continue
        p99s, pins = [], []
        for a, b in zip(on, off):
            A, B = arr(a), arr(b)
            pins.append(world_pin(B, A))
            p99s.append(float(np.percentile(road_band(lum(A) - lum(B)), 99)))
        check(min(pins) >= 0.55,
              "%s: the two legs photographed the same six moments" % track,
              "worst pin %.2f" % min(pins))
        check(min(p99s) >= BEAM_FLOOR,
              "%s: THE POOL READS on all six moments of ordinary driving -- "
              "post-tonemap road-band delta over %.0f levels at 2048x1536"
              % (track, BEAM_FLOOR),
              "worst %.1f, best %.1f levels (pre-fix on this track: %.1f)"
              % (min(p99s), max(p99s),
                 8.0 if track == "US_C1_V1" else 26.7))
        # ...AND IS NOT BLOWN, which is the fourth round of this report and
        # the first one in the other direction.  A floor alone can only ever
        # be answered by turning the beams up, and the answer to "I cannot
        # see them" was turned up far enough that the next answer was "they
        # are too bright".  Both ends are now numbers.
        check(min(p99s) <= BEAM_CEIL,
              "%s: ...AND IS NOT BLOWN -- the LEAST lit of the six moments is "
              "under %.0f levels, so the road is a pool and not a wash"
              % (track, BEAM_CEIL),
              "worst %.1f levels (at the pre-tune wrap of 0.85 this track "
              "measured %.1f)" % (min(p99s), 111.9 if track == "US_C1_V1"
                                  else 86.1))
        # THE TUNNEL IS THE CASE THE TIER EXISTS FOR, and it is what makes
        # WRAP the right knob rather than GAIN: gain would have taken this
        # down by the same factor as the road above.  US_P1_V1's brightest
        # moments are its underpasses.
        if track == "US_P1_V1":
            check(max(p99s) >= BEAM_TUNNEL,
                  "%s: *** AND THE TUNNEL IS STILL STRONG *** -- the tune "
                  "took the open road down by half and the underpass by a "
                  "quarter, which is the wrap term being a grazing-incidence "
                  "term and not a brightness" % track,
                  "best %.1f levels, floor %.0f (pre-tune: 229.0)"
                  % (max(p99s), BEAM_TUNNEL))


# ======================================================================
# 14  TIER 7c / 7d: THE TAIL LAMPS AND THE BOOST FLAME'S LIGHT
#
# Two more classes of light carried by the cars themselves, out of the SAME
# recovered table the beams come from -- model+0x1664 / +0x16AC, type 1 (tail),
# type 2 (brake) and type 8 (tailpipe).  So unlike tiers 7 and 7b, some of what
# this section gates is a RECOVERED RULE and can be checked as one:
#
#   the RULE      FUN_00187C70 tests the brake bit 0x10 BEFORE the tail bit
#                 0x08, so a braking car shows brake INSTEAD of tail.  The
#                 corona sprites have always obeyed it (B3FX_CORONAS and its
#                 explicit skip); the light must obey the same one, from the
#                 same predicate, or the pool and the glow disagree about what
#                 a car is doing.
#   the COLOUR    the tail and brake colours are 0x004161D0 and 0x004161C0
#                 verbatim.  The FLAME's colour is derived -- re-derived HERE,
#                 from the shipped sprites, so an art change moves the light.
#   the LEVEL     the flame's intensity is b3_boostfx_level() untouched, which
#                 is why a wreck carries no flame light for free.
#   the FLICKER   deterministic by construction.  Retail's flame flicker is a
#                 per-frame rand(); reusing it would have broken every pinned
#                 frame in this file, so it is a hash of (frame, slot) and this
#                 section renders the same drive twice to say so.
#   the CEILING   AFX_LIGHT_MAX is 32 and the other tiers spend 30 of it, so
#                 the flames get a POOL of two transient slots rather than a
#                 per-car reservation.  Gated as arithmetic, GL-free.
#   the LEVELS    both pools have a FLOOR and a CEILING from the first day, in
#                 the BEAM_FLOOR/BEAM_CEIL/BEAM_TUNNEL shape -- because the
#                 beams reached that shape only after four rounds of "I cannot
#                 see it" followed by "it is too bright".
#
# *** THE PLAYER DOES NOT BOOST ON A TEST DRIVE. ***  B3_TESTDRIVE holds the
# throttle and nothing else, so every flame on an ordinary drive belongs to the
# AI -- and the ledger's flamedist column says the median burning car is 116 m
# from the eye.  Three attempts at a boost leg pinned frames inside a real burn
# window and measured EXACTLY ZERO moved pixels, which is a correct light
# photographed from too far away.  B3_TEST_PAD_BOOST=1 (already in the tree for
# validate_aftertouch's real-input leg) holds the boost button from the same
# local the pad writes; the player then burns two metres from the eye.
# ======================================================================
TAIL_SLOTS = 6            # B3_PHOTO_TAIL_CARS
BOOST_SLOTS = 2           # B3_PHOTO_BOOST_LIGHTS -- 32 - 18 - 6 - 6
LIGHT_N = 18              # B3_PHOTO_LIGHT_N
HEAD_SLOTS = 6            # B3_PHOTO_HEAD_CARS
AFX_LIGHT_MAX = 32

# The same two tracks and the same six moments the beams are measured over, so
# the two tiers can be read against each other.
TAIL_DRIVE = [("US_C1_V1", 400, 100), ("US_P1_V1", 430, 80)]
# MEASURED, 1024x768, 99th percentile of the RED-channel delta over the band of
# road BEHIND the player, tails on against tails off, six moments:
#
#            US_C1_V1 (day)      US_P1_V1 (dusk)
#            worst    best       worst    best
#   0.22       15      42          28      75      <- shipped
#   0.35       26      66
#   0.55       44     100         (the barrier went red at this one)
#
# The floor is a regression alarm and not a target; the ceiling is the other
# half of it, and it is set against the shipped worst frame rather than against
# the best one because "the LEAST lit moment is already a wash" is what "too
# bright" actually means.
TAIL_FLOOR = 5.0
TAIL_CEIL = 55.0
# ...and the dusk track's BEST moment, which is this tier's equivalent of
# BEAM_TUNNEL: the alarm for a tune that took the evening down with the noon.
TAIL_DUSK = 45.0

# The player's own burn, found from the ledger's flamemask column: it starts at
# frame 4748 and the button goes at about 4787, after which the recovered decay
# takes the level down over half a second.  4752/10/12 therefore lands four
# frames on the BURN, three on the FADE and five with the flame out -- which is
# exactly the three populations the legs below need, from one pair of runs.
BOOST_DRIVE = ("US_C1_V1", 4752, 10, 12)    # track, first, every, n
# THE TREE'S OWN PIXEL JITTER, and why this leg counts PIXELS rather than
# demanding bytes.
#
# Two runs of one binary at one frame are NOT bit-identical in this tree, and
# that predates every light in it: the docstring at the top of this file
# records ten runs producing five different frames, and B3_TRACK_NOSHINE is
# already pinned here because the track's additive specular pass is GL_ONE over
# overlapping geometry and float addition is not associative.  With NOSHINE on,
# what is left measures 3-6 levels over 5-29 pixels -- and it measures that on
# frames where NOTHING IS BURNING, so it is not this tier's.  A leg that asked
# for byte equality would fail a correct build about one run in four.
#
# MEASURED, one pair of runs, twelve frames of the boost window:
#
#     the flame POOL itself (tier on vs off)   21,269 - 48,516 px, to 85 levels
#     two identical runs (any frame)                0 -      29 px, to  6 levels
#
# Three orders of magnitude apart, and that gap is the gate: a per-frame rand()
# -- which is what retail's own flame flicker is -- would redraw the WHOLE pool
# differently, i.e. put tens of thousands of pixels in the second column.  So
# the claim is a RATIO against the pool this very run measured, with the
# frames that carry no flame as the control, and neither number is written down
# here as a level to be tuned.
BOOST_NOISE_LEVELS = 8.0
BOOST_NOISE_PIXELS = 200
BOOST_NOISE_SHARE = 100.0   # the pool must be >= this many times the jitter
# LUMINANCE and not red: the default flame is the blue-white `coronaboost` and
# its red channel is a quarter of its blue.  MEASURED at the shipped 0.35:
# worst 15.8, best 31.0 over the road band behind the car.
BOOST_FLOOR = 8.0
BOOST_CEIL = 90.0

# The recovered per-pool modulation constants the flame sprites are drawn with
# (0x00415CC0 / 0x00415CD0, burnout3_boostfx.c).  Restated here rather than
# parsed out of the source, because this leg RE-DERIVES the header's flame
# colours and a validator that took both halves from the thing it is measuring
# would agree with any value.
BOOST_POOL_MOD = {"coronaboost": (0.7, 0.72, 0.75),
                  "coronaboostred": (0.8, 0.8, 0.8)}

# THE BRAKE LEG'S OWN WINDOW, and it is a different one from the tails' for a
# reason the first cut of this section learned the hard way: over six moments a
# hundred frames apart, the ledger's brakedist column says the nearest braking
# car is 13, 45, 118, 37 and 125 metres away.  A knob experiment on a lamp 120 m
# up the road moves no pixels whatever the knob does, so five of the six frames
# proved nothing and the leg went red while the code was correct.  430/45 puts
# three braking cars inside 25 m and still catches two frames where nobody is
# braking at all, which is what the other half of the claim needs.
BRAKE_DRIVE = ("US_C1_V1", 430, 45, 10)
BRAKE_NEAR = 25.0

LEDGER14 = re.compile(
    r"\[photo-lights\] f=(\d+) beams=(\d+) lamped=(\d+) racers=(\d+) "
    r"tails=(\d+) braking=(\d+) brakemask=0x([0-9a-f]+) brakedist=(-?[\d.]+) "
    r"flames=(\d+) flick=([\d.]+) flamemask=0x([0-9a-f]+) "
    r"flamedist=(-?[\d.]+) flamelev=([\d.]+) scenery=(\d+) "
    r"total=(\d+) budget=(\d+)")


def led14(log):
    """The ledger as {frame: dict}, which is what every leg here indexes by."""
    out = {}
    for m in LEDGER14.finditer(log):
        g = m.groups()
        out[int(g[0])] = {
            "beams": int(g[1]), "racers": int(g[3]), "tails": int(g[4]),
            "braking": int(g[5]), "brakemask": int(g[6], 16),
            "brakedist": float(g[7]),
            "flames": int(g[8]), "flick": g[9], "flamemask": int(g[10], 16),
            "flamedist": float(g[11]), "flamelev": float(g[12]),
            "total": int(g[14]), "budget": int(g[15])}
    return out


def strip_c_comments(src):
    """...because this section's own prose would otherwise fail it.

    Two of the checks below are 'this construct does NOT appear in that block'
    -- no second copy of the brake threshold, no rand() in the flame block --
    and both went red on the first run against a correct tree, because the code
    they are guarding EXPLAINS ITSELF in a comment that names the thing being
    forbidden ("never rand(), never a clock").  A tree where the dangerous
    construct is documented next to the safe one is the tree this repo asks
    for, so the scan has to read code and the comments have to survive.  Same
    move, and the same reasoning, as section 8's track-id scan.
    """
    s = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def car_drive(tag, track, extra, first, every, n, res="1024x768"):
    """One driving leg: the BMP sequence plus the per-frame light ledger.

    Deliberately NOT a parameter sprawl on beam_drive(): that one is pinned to
    the beams' own environment (it forces B3_PHOTO_HEAD_ON and nothing else),
    and threading three more switches through it would have made the beams'
    measurement depend on this section's argument list.
    """
    seq = os.path.join(OUT, "cardrive_" + tag)
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
        "B3_RT": "0", "B3_RT_CARS": "racers",
        "B3_SHOT_SEQ": seq, "B3_SHOT_FIRST": str(first),
        "B3_SHOT_EVERY": str(every), "B3_PHOTO_LIGHT_STATS": "1",
        # B3_EXIT_AT is a RACE-CLOCK second, not a wall-clock one, and the
        # clock stops when the race ends -- so an exit time past the chequered
        # flag never arrives and the run has to be killed by `timeout`.  Five
        # seconds past the last shot is the same margin beam_drive() uses.
        "B3_EXIT_AT": str(int((first + every * n) / 60.0) + 5),
    })
    env.update(extra)
    p = subprocess.run(["timeout", "1200", GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    frames = sorted(os.path.join(seq, f) for f in os.listdir(seq)
                    if f.startswith("frame_") and f.endswith(".bmp"))
    return (p.stdout.decode("utf-8", "replace"), frames[:n],
            [first + i * every for i in range(n)])


def rear_band(d):
    """The road BEHIND the player: rows 62-85%, columns 15-85%.

    MEASURED, like the beams' road_band() and in the opposite direction.  A
    chase camera is behind the car, so what is behind the CAR is between the
    car and the CAMERA -- i.e. lower in the frame than the beams' pool, not
    higher.  Swept as a per-5%-row profile of the tail delta on both test
    tracks: nothing above row 45%, a shoulder from 50% and the mass from 60%
    down.  85% is the cut-off rather than 100% because the bottom band carries
    the speedo and the near-miss ticker.
    """
    h, w = d.shape[:2]
    return d[int(h * 0.62):int(h * 0.85), int(w * 0.15):int(w * 0.85)]


def flame_band(d):
    """...and the flame's, which reaches further down and wider.

    The exhaust is BELOW the tail lamps and fires along the road rather than
    across it, so its pool runs from mid-frame to the bottom edge.  Same
    measurement, same profile sweep: the tail's mass is at rows 60-75% and the
    flame's at 85-100%.
    """
    h, w = d.shape[:2]
    return d[int(h * 0.62):, int(w * 0.10):int(w * 0.90)]


def lit_chroma(d, thresh=3.0):
    """The colour a delta ADDED, normalised to a unit maximum channel.

    Blind to how bright the light is and sensitive to what colour it is, which
    is the only way to ask "is this pool the flame's own colour" without also
    asking "is the gain still 0.35".
    """
    m = d.reshape(-1, 3)
    hot = m.sum(axis=1) > thresh
    if hot.sum() < 50:
        return None
    c = m[hot].mean(axis=0)
    return c / max(c.max(), 1e-9)


def derive_flame_colours():
    """The two flame colours, re-derived from the shipped sprites.

    What a flame EMITS is the whole sprite's energy, so this is the
    alpha-weighted mean of each texture's texels times its own recovered pool
    modulation constant, normalised to a unit maximum channel -- the same
    arithmetic the header states, run against the PNGs rather than read out of
    the header.  Returns None when the art has not been extracted.
    """
    out = {}
    for name, mod in BOOST_POOL_MOD.items():
        p = os.path.join(ROOT, "build", "boostfx", name + ".png")
        if not os.path.isfile(p):
            return None
        im = np.asarray(Image.open(p).convert("RGBA")).astype(np.float64)
        rgb, a = im[:, :, :3] / 255.0, im[:, :, 3:4] / 255.0
        mean = (rgb * a).sum(axis=(0, 1)) / max(a.sum(), 1e-9)
        c = mean * np.array(mod)
        out[name] = c / max(c.max(), 1e-9)
    return out


def hdr_floats(hdr, *names):
    got = []
    for n in names:
        m = re.search(r"#define\s+%s\s+([-\d.]+)f?" % n, hdr)
        got.append(float(m.group(1)) if m else None)
    return got


def section_car_lamps():
    print("\n== 14. TIER 7c/7d: THE TAIL LAMPS AND THE BOOST FLAME ==")
    hdr = open(os.path.join(ROOT, "src", "burnout3_aftereffects.h")).read()
    full = open(os.path.join(ROOT, "src", "burnout3_full.c")).read()
    cfx = open(os.path.join(ROOT, "src", "burnout3_carfx.c")).read()

    # ---- 14.1 SOURCE + LAW -------------------------------------------------
    check("model+0x1664" in hdr and "FUN_00187C70" in hdr,
          "the header cites the table these lamps come from and the function "
          "whose ORDER decides between them")
    check("0x004161D0" in hdr and "0x004161C0" in hdr,
          "...and the two colours by the addresses they were read from, so "
          "the tail/brake ratio is retail's rather than a look")
    # THE DRIFT GATE.  The corona sprites and the light now make the same
    # exclusivity claim in two files; this is what keeps a fix to one from
    # silently missing the other, exactly as section 11 does for the two
    # shadow shader texts.
    check("light_byte & B3_CARFX_LIGHT_BRAKE" in cfx
          and "cc->bit == B3_CARFX_LIGHT_TAIL" in cfx,
          "the CORONA pass still skips the tail row when the brake bit is "
          "set -- the sprite's half of the same recovered rule")
    check(re.search(r"type\s*=\s*braking\s*\?\s*2\s*:\s*1", full) is not None,
          "*** AND THE LIGHT PICKS ONE LAMP SET, NOT TWO ***: the type is a "
          "single ternary on the brake state, so 'brake INSTEAD of tail' is "
          "the shape of the code and not a rule it has to remember")
    n_lamp_calls = len(re.findall(r"b3_carfx_car_lamps\(c,\s*type,", full))
    check(n_lamp_calls == 1,
          "...and there is exactly ONE gather in the tail block, so a car can "
          "only ever occupy one tail slot",
          "%d call(s)" % n_lamp_calls)
    # ONE SPELLING OF THE BRAKE TEST.  Two would be how the pool and the
    # sprite come to disagree, and the disagreement would be invisible in any
    # frame where only one of them is on screen.
    code = strip_c_comments(full)
    n_thresh = len(re.findall(r"last_brake\s*>\s*0\.05f", code))
    check(n_thresh == 1 and "car_braking(" in code,
          "the brake threshold is written ONCE, in car_braking(), and both "
          "the corona pass and the tail light ask it",
          "%d occurrence(s) of the literal test" % n_thresh)

    # THE FLICKER IS NOT A CLOCK.  Scanned over the tier 7d block itself
    # rather than over the file, because burnout3_full.c is 25k lines and of
    # course something in it calls rand().
    m0 = full.find("TIER 7d: THE BOOST FLAME'S LIGHT")
    m1 = full.find("nch = nres;", m0) if m0 >= 0 else -1
    blk = strip_c_comments(full[m0:m1]) if 0 <= m0 < m1 else ""
    if check(bool(blk), "the tier 7d block was located in the source"):
        bad = [w for w in ("rand(", "srand(", "time(", "clock(",
                           "SDL_GetTicks", "g_race_time")
               if w in blk]
        check(not bad,
              "*** THE FLAME FLICKER IS DETERMINISTIC BY CONSTRUCTION ***: no "
              "PRNG and no clock anywhere in the block -- retail's own flicker "
              "is a per-frame rand() and reusing it would have broken every "
              "pinned frame in this file",
              ("found: " + ", ".join(bad)) if bad else "clean")
        check("b3_light_hash01(" in blk and "g_frame_count" in blk,
              "...and what it uses instead is a hash of (frame counter, car "
              "slot) and nothing else")

    # THE FLAME COLOURS, RE-DERIVED FROM THE SHIPPED ART.
    der = derive_flame_colours()
    if der is None:
        print("      ! build/boostfx/*.png not extracted here -- the flame "
              "colour derivation is SKIPPED, not passed")
    else:
        want = {
            "coronaboost": hdr_floats(hdr, "B3_PHOTO_FLAME_R",
                                      "B3_PHOTO_FLAME_G", "B3_PHOTO_FLAME_B"),
            "coronaboostred": hdr_floats(hdr, "B3_PHOTO_FLAMEHOT_R",
                                         "B3_PHOTO_FLAMEHOT_G",
                                         "B3_PHOTO_FLAMEHOT_B")}
        for name, c in der.items():
            h = want[name]
            ok = all(v is not None for v in h) and \
                max(abs(c[i] - h[i]) for i in range(3)) < 0.01
            check(ok,
                  "the %s light colour is the SPRITE'S OWN, re-derived from "
                  "the shipped PNG times its recovered pool constant" % name,
                  "art %.3f %.3f %.3f vs header %s"
                  % (c[0], c[1], c[2],
                     " ".join("%.3f" % v if v is not None else "?"
                              for v in h)))
        check(der["coronaboost"][2] > der["coronaboost"][0] * 2.0,
              "*** AND THE DEFAULT FLAME IS BLUE, NOT WARM ***: only the five "
              "Car10 specials burn orange, so a gate written for warmth would "
              "have failed on the rest of the roster",
              "blue %.3f against red %.3f"
              % (der["coronaboost"][2], der["coronaboost"][0]))

    # ---- 14.1b THE CEILING, EXECUTED ---------------------------------------
    #
    # The whole reason the flames get a pool of two rather than a slot each is
    # that 18 + 6 + 6 + 6 does not fit in 32.  That is arithmetic, it is
    # GL-free, and it is run here rather than read, through the same probe .so
    # section 1 builds.
    tmp = tempfile.mkdtemp(prefix="b3lamp")
    so = os.path.join(tmp, "photo.so")
    cc = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O0", "-DB3_AFX_NO_GL",
         "-DB3_POSTFX_NO_GL", "-I" + os.path.join(ROOT, "src"),
         os.path.join(ROOT, "src", "burnout3_aftereffects.c"),
         os.path.join(ROOT, "src", "burnout3_postfx.c"), "-o", so, "-lm"],
        capture_output=True)
    if not check(cc.returncode == 0, "the GL-free budget probe builds",
                 cc.stderr.decode("utf-8", "replace")[-300:]):
        return
    probe = os.path.join(tmp, "probe.py")
    open(probe, "w").write(
        "import ctypes,sys\n"
        "l=ctypes.CDLL(sys.argv[1])\n"
        "print(l.b3_photo_light_budget(), l.b3_photo_head_slots(),\n"
        "      l.b3_photo_tail_slots(), l.b3_photo_boost_slots())\n")

    def budget(env):
        e = dict(os.environ)
        for k in list(e):
            if k.startswith("B3_PHOTO"):
                e.pop(k, None)
        e.update(env)
        r = subprocess.run([sys.executable, probe, so], env=e,
                           capture_output=True)
        return [int(x) for x in r.stdout.decode().split()]

    b = budget({})
    check(b == [AFX_LIGHT_MAX, HEAD_SLOTS, TAIL_SLOTS, BOOST_SLOTS],
          "*** THE FOUR RESERVATIONS ADD UP TO THE CEILING EXACTLY ***: "
          "%d streetlights + %d beams + %d tails + %d flames = %d, which is "
          "AFX_LIGHT_MAX -- so the flame pool is what was LEFT, and that is "
          "why it is a pool"
          % (LIGHT_N, HEAD_SLOTS, TAIL_SLOTS, BOOST_SLOTS, AFX_LIGHT_MAX),
          "budget %d = %d + %d + %d + %d"
          % (b[0], b[0] - b[1] - b[2] - b[3], b[1], b[2], b[3]) if b else "-")
    b = budget({"B3_PHOTO_LIGHTS": "0"})
    check(b == [0, 0, 0, 0],
          "tier 7's switch stands all four of them down together", str(b))
    b = budget({"B3_PHOTO_LIGHT_N": "4"})
    check(b == [4 + HEAD_SLOTS + TAIL_SLOTS + BOOST_SLOTS, HEAD_SLOTS,
                TAIL_SLOTS, BOOST_SLOTS],
          "...and lowering the STREETLIGHTS' budget cannot take a slot from "
          "any class of car lamp", str(b))
    # The pathological end: a grid so large that the car lamps alone would
    # breach the array.  The street must lose everything and the ARRAY must
    # still be legal, because a budget over AFX_LIGHT_MAX is a shader that
    # would not compile and a memcpy past the end of g_light_p.
    b = budget({"B3_PHOTO_HEAD_CARS": "8", "B3_PHOTO_TAIL_CARS": "8",
                "B3_PHOTO_BOOST_LIGHTS": "8", "B3_PHOTO_LIGHT_N": "32"})
    check(b and b[0] <= AFX_LIGHT_MAX,
          "...and no combination of switches can ask for more lights than the "
          "uniform array holds", str(b))

    if not os.path.isfile(GAME):
        return

    # ---- 14.1c THE TWO TIERS CAN BE RETIRED TO THE BYTE --------------------
    #
    # The same shape section 11 opens with, and here it does a second job.
    #
    # These tiers are ON by default, so B3_PHOTO=0 is not the only thing that
    # has to be provable: there must be a way to put the frame back EXACTLY
    # where it was before them.  A tier that cannot be retired to the byte is a
    # tier every existing pinned suite has to be re-baselined around.
    #
    # AND IT DISCHARGES SECTION 11's LAST LEG.  That leg compares the WHOLE
    # photo stack against a reference binary, so any deliberate rendering
    # change on this side turns it red -- its own note says so, and it cannot
    # tell "somebody added a tier" from "tier 4r is leaking".  When retiring
    # exactly these two tiers puts the frame back onto the reference's bytes,
    # the difference section 11 measured is EXACTLY 7c and 7d.  That turns
    # "expected, see the note" into a measurement.
    #
    # *** AND THE KNOB THAT DOES IT IS THE RESERVATION, NOT THE SWITCH. ***
    # This leg was first written with B3_PHOTO_TAIL_ON=0 / B3_PHOTO_BOOST_ON=0
    # and it FAILED against a correct tree, which is the most useful thing it
    # has done so far.  Those switches retire the LIGHTS; they deliberately do
    # not shorten the shader's uniform ARRAY, for the reason
    # b3_photo_head_slots() spells out at length -- an array whose length
    # depends on the frame's contents makes the two legs of every A/B compile
    # different programs.  So with them off the budget is still 32 rather than
    # 24, and two things follow.  The streetlights spill into the spare room
    # ("whatever it does not use falls to the lamps", the rule the beams have
    # always had).  And even on a frame where they do NOT -- frame 400 finds
    # only 13 lamps in range, well under either budget, so the light CONTENTS
    # are identical -- the frame still moves: measured, 75 pixels by up to 17
    # levels, because the loop bound is a compile-time constant and thirty-two
    # additions of zero do not associate the way twenty-four do.  A zeroed slot
    # is free in arithmetic and not in floating point.
    #
    # B3_PHOTO_TAIL_CARS=0 / B3_PHOTO_BOOST_LIGHTS=0 retire the RESERVATIONS,
    # the budget goes back to 18 + 6, the assembled shader is the old text, and
    # the frame is the old frame.  That is the stand-down, and the distinction
    # is worth having in a gate rather than in a comment.
    if REF_BIN and os.path.isfile(REF_BIN):
        refs, gone, kept, on = set(), set(), set(), set()
        for i in range(4):
            q = shot("lamp_ref_%d" % i, {"B3_PHOTO": "1"}, binary=REF_BIN)
            if q:
                refs.add(sha(q))
            q = shot("lamp_gone_%d" % i,
                     {"B3_PHOTO": "1", "B3_PHOTO_TAIL_CARS": "0",
                      "B3_PHOTO_BOOST_LIGHTS": "0"})
            if q:
                gone.add(sha(q))
        for i in range(2):
            q = shot("lamp_kept_%d" % i,
                     {"B3_PHOTO": "1", "B3_PHOTO_TAIL_ON": "0",
                      "B3_PHOTO_BOOST_ON": "0"})
            if q:
                kept.add(sha(q))
            q = shot("lamp_on_%d" % i, {"B3_PHOTO": "1"})
            if q:
                on.add(sha(q))
        check(bool(refs & gone),
              "*** THE TWO NEW TIERS CAN BE RETIRED TO THE BYTE ***: with "
              "B3_PHOTO_TAIL_CARS=0 and B3_PHOTO_BOOST_LIGHTS=0 the whole "
              "photo stack is BYTE-FOR-BYTE the build they were added to -- so "
              "section 11's cross-build leg is measuring exactly 7c and 7d, "
              "and nothing else in the stack has moved",
              "%d shared frame(s): %s"
              % (len(refs & gone),
                 " ".join(sorted(h[:8] for h in refs & gone))
                 if refs & gone else "none"))
        check(on and not (on & refs),
              "...and NOT byte-identical with them on, which is what stops "
              "the leg above being passed by two knobs that were never wired "
              "to anything", "%d shared frame(s)" % len(on & refs))
        check(kept and not (kept & refs) and not (kept & on),
              "...and the _ON switches are a THIRD frame, neither the "
              "reference's nor the default's: they retire the lights and "
              "deliberately leave the shader's array at its full length, so "
              "the street keeps the spare slots and the longer loop reorders "
              "the sum by a few ulps.  Retiring a tier and standing its "
              "reservation down are different acts",
              "ref %d / on %d / _ON=0 %d distinct frame(s), overlaps %d and %d"
              % (len(refs), len(on), len(kept), len(kept & refs),
                 len(kept & on)))
    else:
        print("      ! no B3_PHOTO_REF_BIN -- the stand-down leg is SKIPPED, "
              "not passed (it is what proves these tiers can be retired to "
              "the byte)")

    # ---- 14.2 / 14.3 THE TAILS, OVER A DRIVE -------------------------------
    for track, first, every in TAIL_DRIVE:
        log_on, on, fon = car_drive("tail_%s_1" % track, track,
                                    {"B3_PHOTO_TAIL_ON": "1"},
                                    first, every, 6)
        _, off, _ = car_drive("tail_%s_0" % track, track,
                              {"B3_PHOTO_TAIL_ON": "0"}, first, every, 6)
        L = led14(log_on)
        if check(len(L) > 300,
                 "%s: the per-frame ledger ran over a real drive" % track,
                 "%d frames" % len(L)):
            rows = list(L.values())
            check(all(1 <= r["tails"] <= TAIL_SLOTS for r in rows),
                  "%s: every frame lights at least one car's rear lamps and "
                  "none exceeds the reservation" % track,
                  "tails %d..%d against %d slots"
                  % (min(r["tails"] for r in rows),
                     max(r["tails"] for r in rows), TAIL_SLOTS))
            # THE FAITHFUL PART, and it surprised this port: several shipped
            # car models carry NO type-1 records at all (COMP_Car3, SPRT_Car8,
            # every TSPC traffic model...), so retail draws no tail corona for
            # them and neither does this.  `tails` is therefore allowed to be
            # short of `racers`, and a gate that demanded one slot per car
            # would have been demanding a lamp the artists did not model.
            check(all(r["tails"] <= r["racers"] for r in rows),
                  "%s: ...and never MORE than one slot per racer, which is "
                  "the exclusivity rule seen from the ledger: a braking car "
                  "does not get a second light" % track,
                  "worst %d tails against %d racers"
                  % (max(r["tails"] for r in rows),
                     max(r["racers"] for r in rows)))
            check(all(r["braking"] <= r["tails"] for r in rows),
                  "%s: every braking car's slot IS its tail slot" % track)
            check(max(r["braking"] for r in rows) >= 1,
                  "%s: ...and the brake path actually executed on this drive "
                  "-- a rule nothing exercises is a rule nothing gates" % track,
                  "%d cars braking at once, worst frame"
                  % max(r["braking"] for r in rows))
            check(all(r["total"] <= r["budget"] for r in rows),
                  "%s: the frame never asks for more lights than the shader "
                  "declares" % track,
                  "worst %d of %d" % (max(r["total"] for r in rows),
                                      rows[0]["budget"]))
            check(all(r["flames"] <= BOOST_SLOTS for r in rows),
                  "%s: and the flame pool never exceeds its two slots" % track,
                  "worst %d" % max(r["flames"] for r in rows))

        if not check(len(on) >= 6 and len(off) >= 6,
                     "%s: both tail legs rendered six frames" % track,
                     "on %d, off %d" % (len(on), len(off))):
            continue
        p99s, pins, chroms = [], [], []
        for a, b2 in zip(on, off):
            A, B = arr(a), arr(b2)
            pins.append(world_pin(B, A))
            d = A - B
            p99s.append(float(np.percentile(rear_band(d)[:, :, 0], 99)))
            c = lit_chroma(rear_band(d))
            if c is not None:
                chroms.append(c)
        check(min(pins) >= 0.55,
              "%s: the two tail legs photographed the same six moments" % track,
              "worst pin %.2f" % min(pins))
        check(min(p99s) >= TAIL_FLOOR,
              "%s: *** THE TAIL POOL READS *** on all six moments of ordinary "
              "driving -- red-channel delta over the road behind the car, "
              "over %.0f levels" % (track, TAIL_FLOOR),
              "worst %.1f, best %.1f" % (min(p99s), max(p99s)))
        check(min(p99s) <= TAIL_CEIL,
              "%s: ...AND IS NOT A RED FILTER -- the LEAST lit of the six is "
              "under %.0f levels" % (track, TAIL_CEIL),
              "worst %.1f (at the first-cut gain of 0.55 this track measured "
              "%.1f)" % (min(p99s), 44.0 if track == "US_C1_V1" else 70.0))
        if chroms:
            c = np.mean(chroms, axis=0)
            check(c[0] > 0.9 and c[1] < 0.2 and c[2] < 0.2,
                  "%s: ...and what it added is RED, which is the recovered "
                  "colour and not an exposure change" % track,
                  "%.3f %.3f %.3f" % (c[0], c[1], c[2]))
        # THE DUSK END, this tier's BEAM_TUNNEL: the alarm for a tune that
        # takes the evening down along with the noon.
        if track == "US_P1_V1":
            check(max(p99s) >= TAIL_DUSK,
                  "%s: *** AND THE DUSK END STILL SINGS *** -- the brightest "
                  "of the six clears %.0f levels, so the day tune did not "
                  "flatten the hour these lamps are for" % (track, TAIL_DUSK),
                  "best %.1f levels" % max(p99s))

    # ---- 14.4 BRAKE INSTEAD OF TAIL, EXECUTED ------------------------------
    #
    # The source half above says the code can only pick one lamp set.  This is
    # the half that says it picks the RIGHT one, and it is a knob experiment
    # rather than a screenshot: B3_PHOTO_BRAKE_REACH scales the BRAKE light's
    # radius and nothing else, so
    #
    #   * a frame where nobody is braking must come out BIT-IDENTICAL -- if a
    #     non-braking car were lit by the brake entry, the knob would reach it;
    #   * a frame where somebody IS braking must change -- if a braking car
    #     were lit by the TAIL entry (i.e. if the exclusivity ran the wrong
    #     way round), the knob would reach nothing.
    #
    # Both halves are needed.  Either one alone is satisfied by a bug.
    track, first, every, n = BRAKE_DRIVE
    log_r, ref, fb = car_drive("brake_ref", track,
                               {"B3_PHOTO_TAIL_ON": "1"}, first, every, n)
    log_b, big, _ = car_drive("brake_big", track,
                              {"B3_PHOTO_TAIL_ON": "1",
                               "B3_PHOTO_BRAKE_REACH": "6.0"},
                              first, every, n)
    Lr = led14(log_r)
    if check(len(big) >= n and len(ref) >= n,
             "the brake-reach leg rendered against the shipped one",
             "big %d, ref %d" % (len(big), len(ref))):
        same, moved, far, skipped = [], [], 0, 0
        for a, b2, f in zip(big, ref, fb):
            if f not in Lr:
                skipped += 1
                continue
            A, B = arr(a), arr(b2)
            if world_pin(B, A) < 0.55:
                skipped += 1
                continue
            g = np.abs(A - B)
            rec = (f, float(g.max()), int((g.sum(axis=2) > 0).sum()))
            r = Lr[f]
            if r["braking"] == 0:
                same.append(rec)
            elif 0.0 <= r["brakedist"] <= BRAKE_NEAR:
                moved.append(rec)
            else:
                far += 1      # braking, but too far up the road to measure
        check(skipped <= 2, "the brake-reach frames pinned to the same moments",
              "%d of %d skipped" % (skipped, n))
        # PIXEL COUNTS AND NOT BYTES, for the reason set out over
        # BOOST_NOISE_SHARE: any two runs of this tree disagree on a handful
        # of pixels by a couple of levels, whatever is being rendered.  The
        # separation is three orders of magnitude wide, so it is measured as a
        # ratio against the change the knob makes on the frames it should.
        same_px = max([p for _, _, p in same] or [0])
        move_px = min([p for _, _, p in moved] or [0])
        check(same and move_px and same_px * BOOST_NOISE_SHARE <= move_px
              and same_px <= BOOST_NOISE_PIXELS,
              "*** A BRAKE KNOB TOUCHES NOTHING WHEN NOBODY IS BRAKING ***: "
              "with the brake radius scaled six-fold, the frames whose ledger "
              "says braking=0 are untouched -- so a cruising car is lit by "
              "its TAIL lamp and not by its brake lamp",
              "%d frames, worst %d px against %d px on the braking frames"
              % (len(same), same_px, move_px))
        check(moved and all(d > 2.0 for _, d, _ in moved),
              "*** ...AND IT MOVES THE ROAD WHEN SOMEBODY NEARBY IS ***: "
              "every frame with a braking car inside %.0f m changes, so a "
              "braking car is lit by its BRAKE lamp -- which, with one slot "
              "per car, is the 0x10-before-0x08 rule EXECUTED rather than "
              "read" % BRAKE_NEAR,
              "%d near frames, worst delta %.0f levels over %d px (%d "
              "further-off braking frames not measured -- a lamp 100 m up the "
              "road moves no pixels whatever the knob does)"
              % (len(moved), min([d for _, d, _ in moved] or [0]), move_px,
                 far))

    # ---- 14.5 THE FLAME ----------------------------------------------------
    track, first, every, n = BOOST_DRIVE
    pad = {"B3_TEST_PAD_BOOST": "1"}
    log_1, b_on, fb = car_drive("boost_1", track, dict(pad),
                                first, every, n)
    _, b_off, _ = car_drive("boost_0", track,
                            dict(pad, B3_PHOTO_BOOST_ON="0"),
                            first, every, n)
    L1 = led14(log_1)
    if not check(len(b_on) >= n and len(b_off) >= n,
                 "both boost legs rendered", "on %d, off %d"
                 % (len(b_on), len(b_off))):
        return
    # THREE KINDS OF FRAME, and the recovered envelope is what sorts them.
    # b3_boostfx_level() is 2.0 for the ignition flare, holds at 1.0 while the
    # boost burns and decays to 0 over half a second after the button goes --
    # so a frame two thirds of the way down that fade is SUPPOSED to light the
    # road less than a burning one.  Demanding the same floor of both would
    # have been demanding that the recovered envelope be ignored, and the
    # first cut of this leg did exactly that and went red at 2.8 levels on a
    # frame whose flame was almost out.  The ledger publishes the level, so
    # the leg can sort by it instead of guessing.
    burn, fade, dark, p99s, fadep, chroms = [], [], [], [], [], []
    pool_px = 0
    for a, b2, f in zip(b_on, b_off, fb):
        A, B = arr(a), arr(b2)
        if world_pin(B, A) < 0.55 or f not in L1:
            continue
        d = A - B
        r = L1[f]
        if r["flames"] == 0:
            g = np.abs(d)
            dark.append((f, float(g.max()), int((g.sum(axis=2) > 0).sum())))
            continue
        pool_px = max(pool_px, int((np.abs(d).sum(axis=2) > 0).sum()))
        p = float(np.percentile(lum(flame_band(d)), 99))
        if r["flamelev"] >= 1.0:
            burn.append(f)
            p99s.append(p)
            c = lit_chroma(flame_band(d), 6.0)
            if c is not None:
                chroms.append(c)
        else:
            fade.append((f, r["flamelev"]))
            fadep.append(p)
    if check(len(burn) >= 3,
             "the player's own burn was photographed (B3_TEST_PAD_BOOST)",
             "%d burning frames of %d, at %s"
             % (len(burn), n, ", ".join("f%d" % f for f in burn))):
        check(min(p99s) >= BOOST_FLOOR,
              "*** THE FLAME LIGHTS THE ROAD IT IS BLASTING *** -- luminance "
              "delta over the band behind the car, over %.0f levels on every "
              "frame the flame is BURNING.  An additive billboard lights "
              "nothing; until this tier a car could run a metre of fire over "
              "black tarmac and leave no mark on it" % BOOST_FLOOR,
              "worst %.1f, best %.1f" % (min(p99s), max(p99s)))
        check(max(p99s) <= BOOST_CEIL,
              "...AND IS NOT A SPOTLIGHT: the brightest burning frame is "
              "under %.0f levels" % BOOST_CEIL,
              "best %.1f (at a gain of 1.20 this window measured 138)"
              % max(p99s))
    if fade:
        check(max(fadep) < min(p99s) if p99s else False,
              "*** AND THE POOL FADES WITH THE FLAME ***: the frames the "
              "recovered level has already begun decaying on light the road "
              "LESS than every burning frame, because the light's intensity "
              "IS b3_boostfx_level() and not a second envelope written "
              "alongside it",
              "fading %s against a burning worst of %.1f"
              % (", ".join("f%d lev %.2f -> %.1f levels" % (f, l, p)
                           for (f, l), p in zip(fade, fadep)),
                 min(p99s) if p99s else -1.0))
    if chroms and der is not None:
        c = np.mean(chroms, axis=0)
        # WHICHEVER PALETTE THIS CAR HAS.  The roster's default is the
        # blue-white pool and five models burn orange; the leg asks which of
        # the two the pool is CLOSER to, and requires the answer to be a good
        # match rather than requiring a particular one -- a suite that
        # demanded blue would go red the day the harness picked a Car10.
        best = min(der, key=lambda k: float(np.abs(der[k] - c).sum()))
        err = float(np.abs(der[best] - c).max())
        check(err < 0.25,
              "*** AND IT IS THE FLAME'S OWN COLOUR ***: the pool matches the "
              "%s sprite's derived palette, so the light and the billboard "
              "agree about what is burning" % best,
              "pool %.3f %.3f %.3f vs art %.3f %.3f %.3f (worst channel "
              "%.3f)" % (c[0], c[1], c[2], der[best][0], der[best][1],
                         der[best][2], err))
    dark_px = max([p for _, _, p in dark] or [0])
    check(dark and pool_px and dark_px * BOOST_NOISE_SHARE <= pool_px
          and dark_px <= BOOST_NOISE_PIXELS,
          "...and a frame where NOBODY is burning is untouched by the tier -- "
          "the two slots are TRANSIENT, so a car that is not boosting costs "
          "the street nothing.  Measured against the pool this same run drew, "
          "not against zero: the tree's additive passes disagree between any "
          "two runs by a few pixels whatever is being rendered",
          "%d unlit frames, worst %d px of a %d px pool (%.0f levels)"
          % (len(dark), dark_px, pool_px,
             max([d for _, d, _ in dark] or [0])))

    # THE DETERMINISM LEG.  The same drive, the same environment, twice: on
    # every frame the two runs agree they photographed (world pin) the flame
    # must be the SAME flame, byte for byte.  This is the claim that a per-
    # frame rand() would fail and that a hash of (frame, slot) cannot.
    log_2, b_again, _ = car_drive("boost_1b", track, dict(pad),
                                  first, every, n)
    L2 = led14(log_2)
    if check(len(b_again) >= n, "the boost leg rendered a second time",
             "%d frames" % len(b_again)):
        lit_d, dark_d = [], []
        for a, b2, f in zip(b_on, b_again, fb):
            if f not in L1 or f not in L2:
                continue
            A, B = arr(a), arr(b2)
            if world_pin(A, B) < 0.95:
                continue          # a coin-flip victim, not a flicker
            g = np.abs(A - B)
            rec = (f, float(g.max()), int((g.sum(axis=2) > 0).sum()))
            (lit_d if L1[f]["flames"] and L2[f]["flames"]
             else dark_d).append(rec)
        base = max([d for _, d, _ in dark_d] or [0.0])
        basep = max([p for _, _, p in dark_d] or [0])
        check(len(lit_d) >= 3 and len(dark_d) >= 2,
              "the determinism leg has both populations: burning frames AND "
              "a control of frames with no flame at all",
              "%d burning, %d unlit" % (len(lit_d), len(dark_d)))
        worst = max([d for _, d, _ in lit_d] or [999.0])
        worstp = max([p for _, _, p in lit_d] or [10 ** 9])
        check(pool_px and worstp * BOOST_NOISE_SHARE <= pool_px
              and worstp <= max(basep, BOOST_NOISE_PIXELS)
              and worst <= max(base, BOOST_NOISE_LEVELS),
              "*** THE FLAME ADDS NO NON-DETERMINISM ***: across two runs of "
              "the same drive, a BURNING frame disagrees on under a hundredth "
              "of the pool's own pixels -- and no more than an UNLIT frame "
              "from the same pair.  A per-frame rand(), which is what retail's "
              "own flame flicker is, would have redrawn ALL of them",
              "burning worst %d px of a %d px pool (%.0f levels); unlit "
              "control %d px (%.0f levels), which is the tree's own "
              "additive-pass jitter and predates this tier"
              % (worstp, pool_px, worst, basep, base))
        # ...and it is a FLICKER and not a constant, which the ledger can say
        # without rendering anything at all.
        vals = {L1[f]["flick"] for f in L1 if L1[f]["flames"] > 0}
        nlit = sum(1 for f in L1 if L1[f]["flames"] > 0)
        check(nlit > 20 and len(vals) > nlit * 0.9,
              "...and it IS a flicker: the per-frame multiplier takes a "
              "different value on essentially every burning frame",
              "%d distinct values over %d burning frames" % (len(vals), nlit))
        shared = [f for f in L1 if f in L2 and L1[f]["flames"] > 0]
        check(shared and all(L1[f]["flick"] == L2[f]["flick"]
                             for f in shared),
              "...and the two runs produce the SAME sequence of multipliers, "
              "frame for frame",
              "%d burning frames compared" % len(shared))


# ====================================================================
# 11  TIER 4r: THE RAY-TRACED SUN SHADOW
# ======================================================================
# The option this section gates is OFF BY DEFAULT, and that is the whole
# shape of it: what has to be proved first is that it is INVISIBLE when off,
# and only then that it is worth something when on.
#
#   the LAW      the switch defaults off, the master gates it, $B3_RT wins
#                over the file -- EXECUTED, by compiling src/burnout3_rt.c on
#                its own and calling b3_rt_want() in real environments.
#   the TRACE    the shipped C traversal, run against the shipped bvh.bin, on
#                the shipped geometry -- and asked the one question the depth
#                map structurally cannot answer: how many points on this
#                track are shadowed by something FURTHER AWAY THAN THE
#                CASCADE'S BOX.
#   the DRIFT    AFX_LIGHT_SHADOW and AFX_LIGHT_SHADOW_RT duplicate their
#                shared opening and ending on purpose (see the note over the
#                RT block).  This is the gate that keeps a fix to one from
#                silently missing the other.
#   the PIXELS   with the option off the frame is the reference build's, bit
#                for bit; with it on the ray moves pixels the map did not,
#                and the OPEN ROAD -- where neither should be doing much --
#                stays within a bounded distance of the map's answer.
RT_SAMPLES = 4000
RT_SEED = 7


def rt_probe_so(tmp):
    so = os.path.join(tmp, "rt.so")
    cc = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O2",
         "-I" + os.path.join(ROOT, "src"),
         os.path.join(ROOT, "src", "burnout3_rt.c"), "-o", so, "-lm"],
        capture_output=True)
    if cc.returncode != 0:
        check(False, "the GL-free ray-tracing probe builds",
              cc.stderr.decode("utf-8", "replace")[-300:])
        return None
    return so


def section_rt(render=True):
    print("\n== 11. TIER 4r: THE RAY-TRACED SUN SHADOW ==")
    hdr = open(os.path.join(ROOT, "src", "burnout3_rt.h")).read()
    rtc = open(os.path.join(ROOT, "src", "burnout3_rt.c")).read()
    src = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()

    # ---- the source ----------------------------------------------------
    check("NOTHING IN THIS FILE IS A CLAIM ABOUT BURNOUT 3" in hdr,
          "tier 4r is marked INSPIRED in its header, unmistakably")
    check("DEFAULT OFF" in hdr and "g_want = 0;" in rtc,
          "...and it is OFF by default, in the header and in the code")
    check('"B3_RT"' in rtc, "B3_RT overrides the setting, for harnesses")
    check('"build/settings.cfg"' in rtc.replace("B3_RT_CFG", "") or
          'B3_RT_CFG "build/settings.cfg"' in rtc,
          "the setting persists to build/settings.cfg, next to mixer.cfg")
    # WHAT THE LIMITATION IS CHANGED when tier 4rc landed, and the check
    # changed with it rather than being deleted.  Cars ARE in the tree now,
    # through a second model-space tree per vehicle and a two-level trace; what
    # is still out is the knocked props and the DAMAGE STATES, and the rule
    # that keeps the blob and the ray from both applying is a claim the header
    # has to carry because a reader cannot see it from the shader.
    check("blob shadow" in hdr and "SUPPRESSED" in hdr,
          "the BLOB-SUPPRESSION RULE is written down: retail's own shadow "
          "stands aside for exactly the cars the ray took, and for nothing "
          "else")
    check("DAMAGE STATES" in hdr and "intact hull" in hdr,
          "...and so is what tier 4rc still cannot do: a wreck is traced with "
          "its intact hull, which is a limitation and is written down rather "
          "than hidden")
    # every knob an env of its own, the same rule tier 1-7 are held to
    # B3_RT_WEB_OPTION is not a knob, it is a PLATFORM VERDICT -- one
    # measured number deciding whether the web offers the row at all -- and
    # its env is B3_RT_SHOW, which overrides the decision rather than the
    # value.  Same exemption B3_PHOTO_LUT_N has above, and for the same
    # reason: a constant that is not a tunable is not a missing tunable.
    # ...and neither are the three below, for the same reason one more time.
    # B3_RT_CARS_OFF/RACERS/ALL are the VALUES $B3_RT_CARS takes, not three
    # separate knobs; B3_RT_RAYS_MAX and B3_RT_STEPS_MAX are the CEILINGS the
    # knobs are clamped to, and a ceiling with an env would be a ceiling.
    # (b3_rt_knobs() announces every clamp, which is the accountability those
    # two owe -- a user set B3_RT_RAYS=128, got a silent 16 and reported the
    # knob as dead.)
    NOT_KNOBS = ("B3_RT_WEB_OPTION", "B3_RT_CARS_OFF", "B3_RT_CARS_RACERS",
                 "B3_RT_CARS_ALL", "B3_RT_RAYS_MAX", "B3_RT_STEPS_MAX")
    consts = [l.split()[1] for l in hdr.splitlines()
              if l.startswith("#define B3_RT_")
              and l.split()[1] not in NOT_KNOBS]
    check('"B3_RT_CARS"' in rtc,
          "...and B3_RT_CARS picks which cars the ray may trace, so the "
          "instance cost can be measured rather than argued about")
    check("clamped to" in rtc,
          "*** EVERY CLAMP ANNOUNCES ITSELF ***: a knob that is silently "
          "clamped is indistinguishable from a knob that does nothing, which "
          "is exactly how B3_RT_RAYS=128 came to be reported as broken")
    check("b3_rt_announce" in rtc and "ray tracing OFF" in rtc,
          "...and there is ONE UNCONDITIONAL ARMING LINE saying whether the "
          "ray is answering and, when it is not, which of the reasons it is")
    check('"B3_RT_SHOW"' in rtc,
          "...and the web's own verdict has an override, so the "
          "measurement behind it stays repeatable")
    missing = [c for c in consts if ('"%s"' % c) not in rtc]
    check(not missing, "every tier-4r constant is overridable by an env of "
          "the same name",
          ("missing: " + ", ".join(missing)) if missing else
          "%d constants" % len(consts))

    # ---- EXECUTE the switch --------------------------------------------
    tmp = tempfile.mkdtemp(prefix="b3rt")
    so = rt_probe_so(tmp)
    if not so:
        return
    probe = os.path.join(tmp, "probe.py")
    open(probe, "w").write(
        "import ctypes,sys\n"
        "l=ctypes.CDLL(sys.argv[1])\n"
        "print(l.b3_rt_want(), l.b3_rt_env_forced())\n")

    def ask(env, cwd=None):
        e = dict(os.environ)
        e.pop("B3_RT", None)
        e.update(env)
        r = subprocess.run([sys.executable, probe, so], env=e,
                           cwd=cwd or tmp, capture_output=True)
        return [int(x) for x in r.stdout.decode().split()]

    check(ask({}) == [0, 0],
          "THE DEFAULT IS OFF -- with no file and no env the option is off, "
          "which is what makes every existing pinned suite still valid")
    check(ask({"B3_RT": "1"}) == [1, 1],
          "B3_RT=1 turns it on and says it was the env that did")
    check(ask({"B3_RT": "0"}) == [0, 1], "B3_RT=0 turns it off")
    # the FILE, and the env beating it
    os.makedirs(os.path.join(tmp, "build"), exist_ok=True)
    open(os.path.join(tmp, "build", "settings.cfg"), "w").write(
        "raytracing 1\nsomethingelse 0.5000\n")
    check(ask({}) == [1, 0],
          "build/settings.cfg turns it on across runs -- the pause menu's "
          "setting survives a restart")
    check(ask({"B3_RT": "0"}) == [0, 1],
          "...and $B3_RT beats the file, so a harness cannot be overruled by "
          "whatever the last person to open the menu left there")

    # ---- EXECUTE the trace ---------------------------------------------
    tdir = os.path.join(ROOT, "build", "tracks", TRACK)
    if not os.path.exists(os.path.join(tdir, "bvh.bin")):
        print("  SKIP  the traversal legs: %s has no bvh.bin" % TRACK)
    else:
        l = ctypes.CDLL(so)
        l.b3_rt_world_load.restype = ctypes.c_int
        l.b3_rt_tri_count.restype = ctypes.c_int
        l.b3_rt_tri_data.restype = ctypes.POINTER(ctypes.c_float)
        l.b3_rt_transmittance.restype = ctypes.c_float
        l.b3_rt_transmittance.argtypes = [ctypes.POINTER(ctypes.c_float),
                                          ctypes.POINTER(ctypes.c_float),
                                          ctypes.c_float]
        if not check(l.b3_rt_world_load(tdir.encode()) == 1,
                     "the shipped bvh.bin loads into the shipped traversal"):
            return
        n = l.b3_rt_tri_count()
        v = np.ctypeslib.as_array(l.b3_rt_tri_data(),
                                  shape=(n * 12,)).reshape(n, 3, 4)[:, :, 0:3]

        # THE TRACK'S OWN SUN, in the GL frame, exactly as burnout3_full.c
        # publishes it: track.mtl's scene_light_dir (enviro.dat +0x80, [C] as
        # a field, already negated to point TOWARD the sun) with the loaders'
        # single Z mirror on top.
        sun = None
        for line in open(os.path.join(tdir, "track.mtl")):
            if line.startswith("# scene_light_dir"):
                x, y, z = [float(t) for t in line.split()[2:5]]
                sun = np.array([x, y, -z])
        if sun is None:
            print("  SKIP  the traversal legs: no scene_light_dir")
            return
        sun = sun / np.linalg.norm(sun)
        check(sun[1] > 0.0,
              "the track's sun is above the horizon in the frame the tree "
              "lives in -- the Z mirror went the right way",
              "elevation %.1f deg" % np.degrees(np.arcsin(sun[1])))

        e1 = v[:, 1] - v[:, 0]
        e2 = v[:, 2] - v[:, 0]
        nr = np.cross(e1, e2)
        ln = np.linalg.norm(nr, axis=1)
        good = ln > 1e-6
        nr[good] /= ln[good][:, None]
        cent = v.mean(axis=1)
        ylo, yhi = cent[:, 1].min(), cent[:, 1].max()
        ground = good & (np.abs(nr[:, 1]) > 0.85) & \
            (cent[:, 1] < ylo + (yhi - ylo) * 0.35)
        if not check(int(ground.sum()) > 500,
                     "the track has ground to stand on",
                     "%d up-facing low triangles" % int(ground.sum())):
            return
        rng = np.random.RandomState(RT_SEED)
        idx = rng.choice(np.nonzero(ground)[0],
                         size=min(RT_SAMPLES, int(ground.sum())),
                         replace=False)
        d = (ctypes.c_float * 3)(*[float(x) for x in sun])

        def trace(o, tmax):
            oc = (ctypes.c_float * 3)(*[float(x) for x in o])
            return float(l.b3_rt_transmittance(oc, d, ctypes.c_float(tmax)))

        blocked, dists, bad = 0, [], 0
        for i in idx:
            o = cent[i] + np.array([0.0, 0.30, 0.0])
            t = trace(o, 2000.0)
            if t < 0.0 or t > 1.0:
                bad += 1
            if t < 0.999:
                blocked += 1
                lo, hi = 0.0, 2000.0
                for _ in range(30):
                    mid = 0.5 * (lo + hi)
                    if trace(o, mid) < 0.999:
                        hi = mid
                    else:
                        lo = mid
                dists.append(hi)
        dists = np.array(dists)
        check(bad == 0, "every transmittance is in [0, 1]",
              "%d out of range" % bad)
        check(0.05 < blocked / float(len(idx)) < 0.95,
              "the traced world is a world: some of the ground is in sun and "
              "some is in shade",
              "%d of %d blocked (%.1f%%)"
              % (blocked, len(idx), 100.0 * blocked / len(idx)))

        # *** THE LEG THIS SECTION EXISTS FOR ***
        #
        # The depth-map cascade is one box B3_PHOTO_SH_EXTENT metres wide
        # around the camera.  An occluder OUTSIDE that box is not in the map
        # at all, so the shadow it casts cannot exist however the map is
        # sampled or biased -- it is a structural limit, not a quality
        # setting.  The ray has no box.  So: count the ground samples whose
        # NEAREST OCCLUDER is further away than the whole cascade.
        ext = B3_SH_EXTENT
        far = int((dists > ext).sum()) if dists.size else 0
        check(far > 0,
              "*** KNOWN GEOMETRY CASTS A RAY-TRACED SHADOW THE CASCADE "
              "COULD NOT ***: %d of %d ground samples on %s are shadowed by "
              "an occluder further than the cascade's whole %g m box, so no "
              "depth map fitted to that box contains the caster at all"
              % (far, len(idx), TRACK, ext),
              "median occluder %.1f m, p90 %.1f m, furthest %.1f m"
              % (np.median(dists), np.percentile(dists, 90), dists.max())
              if dists.size else "no blocked samples")
        check(dists.size and dists.max() > ext * 1.5,
              "...and the furthest is well past it, not a rounding of it",
              "%.1f m vs %g m" % (dists.max() if dists.size else 0, ext))

        # a ray fired at a triangle from just above it must hit it: the
        # cheapest possible check that the tree indexes what it contains
        hit = 0
        down = (ctypes.c_float * 3)(0.0, -1.0, 0.0)
        for i in idx[:400]:
            o = cent[i] + np.array([0.0, 4.0, 0.0])
            oc = (ctypes.c_float * 3)(*[float(x) for x in o])
            if l.b3_rt_transmittance(oc, down, ctypes.c_float(50.0)) < 0.999:
                hit += 1
        check(hit >= 395,
              "a ray dropped onto a triangle from four metres up finds it",
              "%d of 400" % hit)

    # ---- the shared text may not drift ---------------------------------
    def block(name):
        i = src.index("static const char *%s =" % name)
        j = src.index(";\n", i)
        return [x.strip() for x in src[i:j].splitlines()
                if x.strip().startswith('"')]

    try:
        m, r = block("AFX_LIGHT_SHADOW"), block("AFX_LIGHT_SHADOW_RT")
    except ValueError:
        check(False, "both shadow blocks are present in the source")
        return
    head = 7      # "{", ndl, the relight's five lines
    tail = 7      # the geometric fold-in, the darken and the cool tint
    check(m[:head] == r[:head],
          "the two shadow blocks OPEN identically -- the directional relight "
          "is tier 4b's and belongs to both answers",
          "%d lines" % head)
    mt = [x.replace("uSh.x *", "uSh.x * uRt.w *") for x in m[-tail:]]
    check(mt == r[-tail:],
          "...and they END identically but for the ray's own strength scale: "
          "the darken, the cool tint and the geometric fold-in are shared, "
          "and this check is what keeps a fix to one from missing the other",
          "%d lines" % tail)

    # ---- the pixels ----------------------------------------------------
    # Everything above is GL-free -- the switch, the traversal, the shared
    # text -- so --no-render still gates all of it.
    if not render or not os.path.isfile(GAME):
        return
    # A SET INTERSECTION AND NOT ONE PAIR, for the reason the module
    # docstring gives at length: a single pinned render is not reproducible
    # on this harness, so a leg that compares one frame against one frame
    # fails a correct build about a third of the time.  It did exactly that
    # here.  The claim is that the two SETS of bytes overlap -- the same
    # frame comes out of both settings -- and the coin flip only decides
    # which of the harness's moments both of them photographed.
    def hset(tag, env, n=3):
        out = set()
        for i in range(n):
            q = shot("%s_%d" % (tag, i), env)
            if q:
                out.add(sha(q))
        return out

    base = hset("rt_base", {"B3_PHOTO": "0"})
    forced = hset("rt_base_rt", {"B3_PHOTO": "0", "B3_RT": "1"})
    if check(base and forced, "the master-gate legs rendered"):
        check(bool(base & forced),
              "*** B3_PHOTO=0 IS A HARD GATE OVER TIER 4r TOO ***: turning "
              "ray tracing on under it changes not one byte of the frame",
              "%d shared frame(s): %s"
              % (len(base & forced),
                 " ".join(sorted(h[:8] for h in base & forced))
                 if base & forced else "none"))

    off = hset("rt_off", {"B3_PHOTO": "1", "B3_RT": "0"}, n=4)
    dflt = hset("rt_dflt", {"B3_PHOTO": "1", "B3_RT": ""}, n=4)
    if check(off and dflt, "the option-off legs rendered"):
        check(bool(off & dflt),
              "the SHIPPED DEFAULT is the option off: B3_PHOTO=1 with an "
              "EMPTY B3_RT is byte-for-byte B3_RT=0",
              "%d shared frame(s)" % len(off & dflt))

    # THE REFERENCE BINARY HAS TO BE THE CURRENT ONE.  This leg compares the
    # whole photo stack, not just B3_PHOTO=0, so ANY rendering change on the
    # other side of it -- a shadow winding fix, an occlusion fix -- makes it
    # red for a reason that is not tier 4r.  Build it from the commit tier 4r
    # started from, not from an older one:
    #     git archive <master> | tar -x -C /tmp/b3ref && make -C /tmp/b3ref
    if REF_BIN and os.path.isfile(REF_BIN):
        refs = set()
        for i in range(4):
            q = shot("rt_ref_%d" % i, {"B3_PHOTO": "1"}, binary=REF_BIN)
            if q:
                refs.add(sha(q))
        mine = set()
        for i in range(4):
            q = shot("rt_mine_%d" % i, {"B3_PHOTO": "1", "B3_RT": "0"})
            if q:
                mine.add(sha(q))
        check(bool(refs & mine),
              "*** THE OPTION IS INVISIBLE WHEN OFF ***: the whole photo "
              "stack with ray tracing off is BIT-IDENTICAL to the build "
              "tier 4r started from",
              "%d shared frame(s)%s" % (
                  len(refs & mine),
                  "" if refs & mine else
                  " -- if section 2's cross-build leg PASSED, the difference "
                  "is a photo-stack change on this side rather than a leak "
                  "from tier 4r, and this reference binary is the wrong one: "
                  "see the note above it"))
    else:
        print("  SKIP  the invisible-when-off cross-build leg: set "
              "B3_PHOTO_REF_BIN")

    # ---- THE MENU, end to end ------------------------------------------
    #
    # Not the setting -- the MENU.  The setting is already gated above by
    # calling into it; what is unproven until here is the screen with a
    # cursor, a hit test, a key-repeat rule and a file write in it.  So this
    # drives the REAL SDL queue (B3_PAUSE_AT parks the overlay open on a
    # frame, B3_PAUSE_KEYS pushes keys into it) and then asks the two
    # questions a settings screen has to answer: did the renderer change
    # THIS run, and is the choice still there the NEXT one.
    # SAVE AND RESTORE, and the restore has to happen even when a leg
    # raises: this file is the game's own, an operator may have set it, and
    # the rest of the suite is pinned against it above.
    cfg = os.path.join(ROOT, "build", "settings.cfg")
    keep = open(cfg).read() if os.path.exists(cfg) else None
    try:
        if os.path.exists(cfg):
            os.remove(cfg)
        log = shot_log("rt_menu", {"B3_PHOTO": "1", "B3_PAUSE_AT": "380",
                                   "B3_PAUSE_ROW": "3",
                                   "B3_PAUSE_KEYS": "enter"})
        check("ray tracing ON" in log,
              "the pause menu's RAY TRACING row toggles on a RETURN through "
              "the real SDL queue")
        check("PHOTO: ray tracing ON" in log,
              "...and the renderer changes IN THE SAME RUN -- the deferred "
              "program is rebuilt and the BVH uploaded mid-race, rather than "
              "branched, so an off frame goes on paying nothing",
              "the upload line, not the boot verdict: the verdict is printed "
              "once at startup and would only say what the process BOOTED "
              "with")
        check(os.path.exists(cfg) and "raytracing 1" in open(cfg).read(),
              "...and the choice is written to build/settings.cfg, next to "
              "mixer.cfg and routed by the same ISO write rule",
              open(cfg).read().strip() if os.path.exists(cfg) else "no file")
        log2 = shot_log("rt_menu2", {"B3_PHOTO": "1"})
        check("shadow(RAY" in log2,
              "...and it SURVIVES THE RESTART: a second process with no env "
              "at all reads the file and boots with the ray answering")
        # and the row says so rather than lying: an env-pinned run greys it
        log3 = shot_log("rt_menu3", {"B3_PHOTO": "1", "B3_RT": "0",
                                     "B3_PAUSE_AT": "380",
                                     "B3_PAUSE_ROW": "3",
                                     "B3_PAUSE_KEYS": "enter"})
        # MATCHED ON THE UPLOAD LINE'S OWN PREFIX, not on the bare words.
        # `[rt] ray tracing OFF (B3_RT=0)` is the ARMING LINE, which is printed
        # once by every process whichever way the option lands -- it exists so
        # that a user tuning B3_RT_* can tell a dead knob from a feature that
        # is not running, and it is therefore in this log by design.  The
        # question here is whether the RENDERER came on, and the two lines that
        # answer that are the upload's and the verdict's.
        check("PHOTO: ray tracing ON" not in log3 and "shadow(RAY" not in log3,
              "a row the env has already decided is GREYED and refuses the "
              "key -- the menu never shows a value the renderer is not using")

        # ---- THE MSAA ROW, and the one thing about it that can go wrong.
        #
        # Row 4 (the sliders are 0..2, RAY TRACING is 3).  A sample-count
        # change is not a resize, so afx_resize() would early-out on the
        # unchanged size and the multisampled pair would never be rebuilt --
        # which is why there is a force flag, and why the interesting claim is
        # not "the setting changed" but "the CHAIN rebuilt and survived".  It
        # goes through afx_resize_or_retire(), the path web_resize_sweep.py
        # hardens, and a chain that failed to rebuild would retire itself and
        # say so.
        if os.path.exists(cfg):
            os.remove(cfg)
        logm = shot_log("msaa_menu", {"B3_PHOTO": "1", "B3_AFX_VERBOSE": "1",
                                      "B3_PAUSE_AT": "380",
                                      "B3_PAUSE_ROW": "4",
                                      "B3_PAUSE_KEYS": "enter"})
        check("[afx] msaa " in logm,
              "the pause menu's MSAA row cycles on a RETURN through the real "
              "SDL queue")
        check("rebuilt from" in logm,
              "*** AND THE CHAIN REBUILDS MID-RACE ***: a sample count is not "
              "a size, so this goes through the resize path deliberately -- "
              "the hardened one -- rather than through a second rebuild path "
              "with its own bugs")
        check("THE CHAIN IS RETIRED" not in logm and "FATAL" not in logm,
              "...and it SURVIVES the rebuild: the chain is not retired and "
              "the frame does not fall back to the legacy postfx path")
        check(os.path.exists(cfg) and "msaa" in open(cfg).read(),
              "...and the choice is written to build/settings.cfg, beside "
              "raytracing and through the same ISO write rule",
              open(cfg).read().strip() if os.path.exists(cfg) else "no file")
        # ---- THE HEADLIGHTS ROW, and the one thing that is DIFFERENT about
        # it.  The other two rows change what the renderer IS -- an assembled
        # shader, a chain of targets -- and both of them prove themselves by
        # something being rebuilt.  This one changes a float in a uniform the
        # deferred pass uploads every frame, so the claim is the opposite:
        # nothing is rebuilt, and the proof is that the knob CACHE was dropped
        # (g_pk is read once a run, so a row that only moved its own variable
        # would do nothing until the next process).
        #
        # Row 5 (sliders 0..2, RAY TRACING 3, MSAA 4).  The cycle is
        # OFF -> LOW -> MED -> HIGH, four stops, and MED is the default.
        if os.path.exists(cfg):
            os.remove(cfg)
        logh = shot_log("head_menu", {"B3_PHOTO": "1", "B3_PAUSE_AT": "380",
                                      "B3_PAUSE_ROW": "5",
                                      "B3_PAUSE_KEYS": "enter"})
        check("[afx] headlights HIGH" in logh,
              "the pause menu's HEADLIGHTS row cycles on a RETURN through the "
              "real SDL queue, and one press from the shipped MED lands on "
              "HIGH -- the pre-playtest look, reachable from inside the game")
        check("wrap 0.85" in logh,
              "...and the value it moves is the WRAP -- the grazing-incidence "
              "term that dims the road and leaves the wall alone -- rather "
              "than a gain, which would take the underpass down with it")
        check("rebuilt from" not in logh and "THE CHAIN IS RETIRED" not in logh,
              "...and NOTHING IS REBUILT for it: this row costs a dropped knob "
              "cache, where the MSAA row costs a whole chain")
        check(os.path.exists(cfg) and "headlights 3" in open(cfg).read(),
              "...and the choice is written to build/settings.cfg, beside the "
              "other two and through the same ISO write rule",
              open(cfg).read().strip() if os.path.exists(cfg) else "no file")
        # the whole cycle, and that OFF is reachable and comes back round
        if os.path.exists(cfg):
            os.remove(cfg)
        logc = shot_log("head_cycle", {"B3_PHOTO": "1", "B3_PAUSE_AT": "380",
                                       "B3_PAUSE_ROW": "5",
                                       "B3_PAUSE_KEYS": "enter,enter,enter"})
        check("headlights HIGH" in logc and "headlights OFF" in logc
              and "headlights LOW" in logc,
              "...and the cycle is OFF -> LOW -> MED -> HIGH -> OFF, four "
              "stops: one either side of the default, because the report this "
              "row answers is about TASTE and taste gets disagreed with twice",
              " | ".join(l.strip() for l in logc.splitlines()
                         if "headlights" in l))
        # an env-pinned run GREYS it, exactly as $B3_RT and $B3_MSAA do
        if os.path.exists(cfg):
            os.remove(cfg)
        logp = shot_log("head_pin", {"B3_PHOTO": "1",
                                     "B3_PHOTO_HEAD_WRAP": "0.85",
                                     "B3_PAUSE_AT": "380",
                                     "B3_PAUSE_ROW": "5",
                                     "B3_PAUSE_KEYS": "enter"})
        check("[afx] headlights" not in logp and not os.path.exists(cfg),
              "a row $B3_PHOTO_HEAD_WRAP has already decided is GREYED and "
              "refuses the key -- and writes no file, so a harness pin cannot "
              "be turned into a persisted user setting by a stray RETURN")

        # the two settings SHARE that file, and each preserves the other
        open(cfg, "w").write("raytracing 1\nmsaa 2\n")
        log4 = shot_log("both_menu", {"B3_PHOTO": "1", "B3_PAUSE_AT": "380",
                                      "B3_PAUSE_ROW": "3",
                                      "B3_PAUSE_KEYS": "enter"})
        del log4
        txt = open(cfg).read() if os.path.exists(cfg) else ""
        check("raytracing 0" in txt and "msaa 2" in txt,
              "*** THE TWO SETTINGS SHARE ONE FILE ***: toggling RAY TRACING "
              "rewrites its own key and PRESERVES msaa, which is the property "
              "b3_rt_save() was written with before there was a second "
              "setting to need it", txt.strip().replace("\n", " | "))
    finally:
        if os.path.exists(cfg):
            os.remove(cfg)
        if keep is not None:
            open(cfg, "w").write(keep)

    # the ray against the map, isolated: the shadow tier alone, both answers
    # B3_RT_CARS=off IS PART OF THE ISOLATION, and leaving it out cost three
    # false failures on the day tier 4rc landed.  This section prices the RAY
    # AGAINST THE MAP over the STATIC world; tier 4rc adds car trees AND
    # suppresses retail's blob for those cars, and road_patch() is the tarmac
    # immediately in front of the camera -- i.e. exactly where the player's own
    # blob sits.  With the cars live the strip came back 20 levels brighter and
    # three checks failed, all of them reporting a second experiment rather
    # than a defect.  The cars get their own section below.
    iso = {"B3_PHOTO": "1", "B3_PHOTO_TONEMAP": "0", "B3_PHOTO_SSAO": "0",
           "B3_PHOTO_ATMOS": "0", "B3_PHOTO_SSR": "0",
           "B3_PHOTO_GODRAY": "0", "B3_PHOTO_LIGHTS": "0",
           "B3_PHOTO_SUN_DIRECT": "0", "B3_PHOTO_SHADOW": "1",
           "B3_RT_CARS": "off"}
    # 420 AND NOT SHADOW_DRIVE_FRAME: section 7 measures the road on this
    # frame for the reason this section needs it too -- at 460 the road strip
    # is under the music ticker, the HUD is drawn after the whole layer, and
    # every leg then reads the same pixels and agrees to +0.00 about nothing.
    fr = 420
    none_ = shot("rt_shnone", dict(iso, B3_PHOTO_SHADOW="0"), frame=fr)
    map_ = shot("rt_shmap", dict(iso, B3_RT="0"), frame=fr)
    ray_ = shot("rt_shray", dict(iso, B3_RT="1"), frame=fr)
    if check(none_ and map_ and ray_,
             "the three shadow-answer legs rendered (frame %d)" % fr):
        N, M, R = arr(none_), arr(map_), arr(ray_)
        if min(world_pin(N, M), world_pin(N, R)) < PIN_MIN:
            print("      ! the world would not settle; the numbers below are "
                  "not trustworthy")
        moved = float((np.abs(R - M).max(axis=2) > 4).mean() * 100)
        check(moved > 2.0,
              "the ray is a DIFFERENT ANSWER, not a re-spelling of the map: "
              "it moves pixels the map did not",
              "%.2f%% of the frame differs by more than 4 levels" % moved)
        check(lum(R).mean() < lum(N).mean() - 0.5,
              "...and it is a SHADOW: the frame is darker than with the tier "
              "off", "%+.2f levels" % (lum(R).mean() - lum(N).mean()))
        # THE OPEN ROAD, where neither answer should be doing much: this is
        # the bound on how far the two may disagree where there is nothing
        # to disagree about.  Measured at +0.55 levels when this was written.
        d = float(lum(road_patch(R)).mean() - lum(road_patch(M)).mean())
        check(abs(d) < 6.0,
              "ON OPEN ROAD the two answers agree to within a bounded "
              "difference -- the ray is not a second exposure setting",
              "%+.2f levels on the road strip" % d)
        dn = float(lum(road_patch(R)).mean() - lum(road_patch(N)).mean())
        check(abs(dn) < 4.0,
              "...and the ray does not INVENT shade on the open road -- "
              "there is no texel for it to fall behind and no bias to get "
              "wrong", "%+.2f levels against the tier off" % dn)

    section_rt_cars(iso, fr)


# ======================================================================
# 13  TIER 4rc: THE CARS' OWN TREES
# ======================================================================
#   the SOURCE     the artefact, the two-level trace and the blob rule are
#                  claims, and each is marked INSPIRED where it is made.
#   the CASTER     a car PROVABLY casts a traced shadow onto the road: the
#                  same frame with the car's instances excluded is measurably
#                  lighter under the car, and the difference is confined to
#                  the ground.
#   the DOUBLE     the blob and the ray never both apply.  This is the one
#                  that would be a visible defect rather than a missing
#                  feature -- two shadows under one car, one of them a fixed
#                  ellipse -- and it is checked from the RENDERER's own ledger
#                  rather than from the source, because the rule lives in a
#                  branch and a branch can be right in the source and wrong in
#                  the frame.
def section_rt_cars(iso, fr):
    print("\n== 13. TIER 4rc: THE CARS' OWN TREES ==")
    hdr = open(os.path.join(ROOT, "src", "burnout3_rt.h")).read()
    afx = open(os.path.join(ROOT, "src", "burnout3_aftereffects.c")).read()
    full = open(os.path.join(ROOT, "src", "burnout3_full.c")).read()

    # ---- the source ----------------------------------------------------
    check("NOTHING IN THIS BLOCK IS A CLAIM ABOUT BURNOUT 3" in afx
          and "blobbyshadow" in afx,
          "the car trace is marked INSPIRED where it is written, and says "
          "what retail actually did instead")
    check("b3rtCars" in afx and "b3rtCarWalk" in afx,
          "the two-level trace is in the shader: an instance loop over a "
          "sphere reject, then the same stackless walk one level down")
    # the STUB, read out of the source the way the source spells it: a GLSL
    # function assembled from adjacent C string literals, so the text to look
    # for is the quoted lines and not the shader they concatenate into
    stub = afx[afx.index("AFX_LIGHT_U_SHADOW_RT_NOCARS ="):]
    stub = stub[:stub.index(";\n")]
    check('"  return 1.0;\\n"' in stub and '"float b3rtCars(' in stub,
          "...and a frame with no car trees compiles a CONSTANT 1.0 instead, "
          "so the multiply folds away rather than costing a call")
    check("b3_afx_rt_car_traced(i)" in full,
          "the blob pass asks whether the ray took this car, in the renderer "
          "rather than in a comment")

    # ---- THE CASTER, and it is a difference of two frames ---------------
    #
    # THE ONLY HONEST WAY TO ASK "does a car cast" is to render the same
    # moment with and without that car's INSTANCE in the trace -- not with and
    # without the CAR, which would also remove its bodywork and half the
    # frame, and not by emptying the budget, which would recompile the shader
    # AND bring the blob back.  B3_RT_CAR_TRACE=0 drops the instances from the
    # ray while leaving the program, the pose and the blob decision exactly
    # where they were, so the difference between these two frames is the
    # traced shadow and nothing else.
    with_ = shot("rtc_on", dict(iso, B3_RT="1", B3_RT_CARS="racers"), frame=fr)
    without = shot("rtc_none", dict(iso, B3_RT="1", B3_RT_CARS="racers",
                                    B3_RT_CAR_TRACE="0"), frame=fr)
    if not check(with_ and without, "the car-caster legs rendered"):
        return
    A, B = arr(with_), arr(without)
    pin = world_pin(A, B)
    if pin < PIN_MIN:
        print("      ! the world would not settle (pin %.2f); the numbers "
              "below are not trustworthy" % pin)
    d = float(lum(car_shadow_patch(A)).mean()
              - lum(car_shadow_patch(B)).mean())
    check(d < -3.0,
          "*** A CAR CASTS A TRACED SHADOW ONTO THE ROAD ***: the tarmac "
          "BESIDE the player is darker with the car's instance in the trace "
          "than in the identical frame with the instances dropped -- same "
          "program, same pose, same blob decision, one difference",
          "%+.2f levels (pin %.2f)" % (d, pin))
    # ...and it is a SHADOW rather than a global darkening: the sky must not
    # move, because a ray that never leaves the ground cannot reach it.
    sky = A[:int(A.shape[0] * 0.25)], B[:int(B.shape[0] * 0.25)]
    ds = abs(float(lum(sky[0]).mean() - lum(sky[1]).mean()))
    check(ds < 0.5,
          "...and it is a SHADOW and not an exposure change: the sky is "
          "untouched, because a car cannot occlude it",
          "%+.2f levels" % ds)

    # ---- NO DOUBLE DARKENING -------------------------------------------
    #
    # The claim is not "the blob is off" -- it is that the blob is off for
    # EXACTLY the cars the ray took and on for everything else.  Three legs
    # say that: the ray with cars (blob suppressed), the ray with the cars
    # knob off (blob back), and the map (blob back).  If the suppression were
    # keyed on the wrong thing, one of the last two would lose its blob too.
    b_off = shot("rtc_blob_off", dict(iso, B3_RT="1", B3_RT_CARS="off"),
                 frame=fr)
    b_map = shot("rtc_blob_map", dict(iso, B3_RT="0", B3_RT_CARS="racers"),
                 frame=fr)
    if check(b_off and b_map, "the blob-rule legs rendered"):
        O, M2 = arr(b_off), arr(b_map)
        # road_patch(), not the shadow patch: this is the tarmac directly in
        # front of the camera, which is where the blob's own ellipse sits and
        # where the traced shadow (thrown to one side by the sun) does not.
        lit = float(lum(road_patch(A)).mean())
        blob_off = float(lum(road_patch(O)).mean())
        blob_map = float(lum(road_patch(M2)).mean())
        check(lit > blob_off + 4.0,
              "*** THE BLOB AND THE RAY NEVER BOTH APPLY ***: with the car "
              "traced the road under it is LIGHTER than with the ray on and "
              "the cars knob off, because retail's ellipse has stood aside "
              "for the traced outline rather than being added to it",
              "%.1f traced vs %.1f blob" % (lit, blob_off))
        check(abs(blob_map - blob_off) < 4.0,
              "...and B3_RT_CARS=racers does NOT suppress the blob when the "
              "ray is not answering at all -- the suppression is keyed on "
              "what was UPLOADED, so turning ray tracing off brings every "
              "blob straight back",
              "%.1f map vs %.1f ray-no-cars" % (blob_map, blob_off))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()

    # THE OPTION IS PINNED OFF FOR THE WHOLE SUITE, and it is pinned HERE
    # rather than leg by leg because several legs build their environment by
    # hand and one of them will always be forgotten.  Tier 4r persists to
    # build/settings.cfg -- in the directory these runs use as their CWD --
    # and with it on the depth-map pass is skipped entirely, so section 7's
    # caster inventory and section 9's coverage line stop being printed and
    # two legs go red for a reason that has nothing to do with either.  That
    # is not a hypothetical: it is what a leftover `raytracing 1` did here.
    # The tier-4r section turns it on for its own legs explicitly, and its
    # switch probe pops it to test the default.
    os.environ["B3_RT"] = "0"
    os.environ["B3_RT_SHOW"] = "1"

    print("=" * 68)
    print("validate_photo -- the PHOTOREALISM WAVE (seven effects, all INSPIRED)")
    print("=" * 68)
    section_source()
    if args.no_render:
        # the switch, the traversal and the shared-text drift are all
        # GL-free; only section 11's pixel legs need a context
        section_rt(render=False)
    if not args.no_render:
        if not os.path.isfile(GAME):
            check(False, "the game binary exists", GAME)
        else:
            # the discarded warm-up: a cold asset cache spends a different
            # number of frames loading, and frame 400 then lands somewhere else
            shot("warmup", {"B3_PHOTO": "0"})
            shot("warmup2", {"B3_PHOTO": "0"})
            off = section_identity()
            section_legs(off)
            section_shadow_road()
            section_lights()
            section_blanket()
            section_beams()
            section_rt()
            section_beam_drive()
            section_car_lamps()
            section_temporal()

    print("\n" + "=" * 68)
    print("  %d/%d checks passed" % (_checks[1], _checks[0]))
    for f in _fails:
        print("  FAILED: " + f)
    print("=" * 68)
    return 0 if _checks[1] == _checks[0] else 1


if __name__ == "__main__":
    sys.exit(main())
