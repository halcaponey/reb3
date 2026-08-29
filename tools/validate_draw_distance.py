#!/usr/bin/env python3
"""validate_draw_distance.py -- the race view's near/far planes, and the
geometry they are required to admit.

THE LAW BEING CHECKED  [C]
--------------------------
Retail's renderer init `FUN_0002ECC0` gives all three of its view objects the
same near/far pair, as immediate pushes into the viewport setters:

    FUN_001D92A0(view, near)   near -> view+0x80
    FUN_001D9360(view, far)    far  -> view+0x84

    0002eda6  MOVSS XMM0,[0x003b1684]   ; 0x3F000000 = 0.5
    0002edae  PUSH 0x3f000000           ; near := 0.5
    0002edb3  PUSH ECX / CALL 0x001d92a0
    0002edc4  MOVSS XMM0,[0x003a340c]   ; 0x461C4000 = 10000.0
    0002edcc  PUSH 0x461c4000           ; far  := 10000.0
    0002edd1  PUSH EDX / CALL 0x001d9360
      (repeated verbatim at 0002ede6/0002edee, 0002ee22/0002ee2a for near
       and 0002ee04/0002ee0c, 0002ee40/0002ee48 for far)

and the sky dome corroborates the far value from the other side:
`FUN_00032580 @0x000325AB` scales the dome by `DAT_004D67E0 - 1000.0`
(the literal 1000.0 lives at 0x003B16CC), i.e. the dome is authored to sit
1000 units inside the far plane -- 9000 for a far plane of 10000.

RETAIL DOES NOT CULL THE WORLD BY DISTANCE.  The only per-submesh gate in the
streamed-world draw is `FUN_001B23F0(&DAT_004D67F0, submesh)`, a plain
bounding-box-vs-plane-set test with no distance term (see the citation block
in src/burnout3_trackmesh.c).  So the far plane IS the draw distance, and the
port must not sit inside it.

WHAT THE SECTIONS DO
--------------------
1. SOURCE   -- the two constants are present in src/burnout3_full.c, and every
               race-view projection plus the sky dome is fed from them.
2. WORLD    -- shipped-asset measurement: for each track, how many of its own
               track.obj vertices fall inside the race frustum in the
               5000..10000 band, i.e. how much geometry a 5000 far plane
               throws away.  Nothing here is hardcoded per track; the camera
               stations come from that track's own route.bin.
3. RENDER   -- (needs the built binary + a GL context) render one frame from a
               long-sightline station on two tracks, once with the far plane
               forced back to 5000 and once at retail's 10000, and assert that
               the far band of the image gains lit (non-background) pixels.
               Two legs per track: with the near-field SCENERY pass off, which
               isolates the far plane, and in the shipped configuration.  See
               the note by MIN_BAND_GAIN for why the pair is taken twice.

Usage:
    python3 tools/validate_draw_distance.py              # source + world + render
    python3 tools/validate_draw_distance.py --no-render  # no GL needed
    python3 tools/validate_draw_distance.py --all        # world sweep, 36 tracks
Env:
    B3_BIN      binary to render with (default ./burnout3)
    B3_DD_OUT   where the render frames go (default build/drawdist)
"""

import math
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKS = os.path.join(ROOT, "build", "tracks")
FULL_C = os.environ.get("B3_DD_SRC", os.path.join(ROOT, "src", "burnout3_full.c"))

# The retail pair.  Provenance in the module docstring; these are the numbers
# the port is asserted to carry, not a tuning choice.
RETAIL_NEAR = 0.5
RETAIL_FAR = 10000.0
# The plane the port used to ship with -- the "before" leg of every check here.
OLD_FAR = 5000.0

# The race view as burnout3_full.c builds it: 60 deg vertical FOV (the default
# g_cam_fov_deg), 16:9.
FOV_DEG = 60.0
ASPECT = 16.0 / 9.0

fails = []
checks = [0]


def check(cond, msg):
    checks[0] += 1
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        fails.append(msg)
    return bool(cond)


# ---------------------------------------------------------------- section 1
def section_source():
    print("\n[1] SOURCE -- the near/far law in src/burnout3_full.c")
    src = open(FULL_C).read()

    m = re.search(r"#define\s+B3_VIEW_NEAR_RETAIL\s+([0-9.]+)f", src)
    check(m is not None and abs(float(m.group(1)) - RETAIL_NEAR) < 1e-6,
          "B3_VIEW_NEAR_RETAIL == %g  [FUN_0002ECC0 @0x0002EDAE]" % RETAIL_NEAR)
    m = re.search(r"#define\s+B3_VIEW_FAR_RETAIL\s+([0-9.]+)f", src)
    check(m is not None and abs(float(m.group(1)) - RETAIL_FAR) < 1e-6,
          "B3_VIEW_FAR_RETAIL  == %g  [FUN_0002ECC0 @0x0002EDCC]" % RETAIL_FAR)

    # Every mat4_perspective in the race view must take the accessors, and no
    # literal near/far pair may survive there.
    persp = [p for p in re.findall(r"mat4_perspective\s*\(([^;]*?)\)\s*;",
                                   src, re.S)
             if "float fov" not in p]          # drop the definition itself
    check(len(persp) >= 3,
          "found %d race-view mat4_perspective call sites" % len(persp))
    bad = [p for p in persp
           if "b3_view_near()" not in p or "b3_view_far()" not in p]
    check(not bad,
          "every mat4_perspective uses b3_view_near()/b3_view_far() "
          "(%d offenders)" % len(bad))

    # The sky dome scale is farClip - 1000; feeding it a stale constant would
    # leave the dome 5000 units inside the world it is meant to close off.
    m = re.search(r"b3_postfx_sky_draw\s*\(\s*eye\s*,\s*([^,]+),", src)
    check(m is not None and "b3_view_far()" in m.group(1),
          "sky dome takes the same far plane [FUN_00032580 @0x000325AB]")

    check("0.1f, 5000.0f" not in src,
          "no 0.1/5000 near/far pair left in the file")


# ---------------------------------------------------------------- section 2
def load_route(track):
    """route.bin's two wall strands -> the route midline (the same midline
    burnout3_full.c builds at load: g_cl[i] = (g_wa[i] + g_wb[i]) * 0.5)."""
    p = os.path.join(TRACKS, track, "route.bin")
    with open(p, "rb") as f:
        hdr = f.read(40)
        magic, ver, wall = struct.unpack_from("<4sII", hdr, 0)
        if magic != b"B3RT" or ver != 3:
            raise ValueError("bad route.bin header %r v%d" % (magic, ver))
        raw = f.read(wall * 12 * 2)
    if len(raw) < wall * 24:
        raise ValueError("route.bin truncated")
    vals = struct.unpack("<%df" % (wall * 6), raw)
    a = [vals[i * 3:i * 3 + 3] for i in range(wall)]
    b = [vals[wall * 3 + i * 3: wall * 3 + i * 3 + 3] for i in range(wall)]
    return [((a[i][0] + b[i][0]) * 0.5,
             (a[i][1] + b[i][1]) * 0.5,
             (a[i][2] + b[i][2]) * 0.5) for i in range(wall)]


def load_obj_verts(track):
    p = os.path.join(TRACKS, track, "track.obj")
    xs, ys, zs = [], [], []
    with open(p) as f:
        for line in f:
            if line[:2] == "v ":
                q = line.split()
                xs.append(float(q[1]))
                ys.append(float(q[2]))
                zs.append(float(q[3]))
    return xs, ys, zs


def frustum_bands(track, near=RETAIL_NEAR, mid=OLD_FAR, far=RETAIL_FAR,
                  eye_up=6.0, nstations=48):
    """For nstations camera poses taken from the track's own route line
    (eye on the midline + eye_up, looking along the tangent), count the
    track's vertices inside the race frustum in three depth bands."""
    try:
        import numpy as np
    except ImportError:
        np = None
    cl = load_route(track)
    xs, ys, zs = load_obj_verts(track)
    n = len(cl)
    ty = math.tan(math.radians(FOV_DEG) * 0.5)
    tx = ty * ASPECT
    step = max(1, n // nstations)

    if np is not None:
        V = np.stack([np.asarray(xs), np.asarray(ys), np.asarray(zs)], 1)
    tot = [0, 0, 0]
    best = (0.0, None)
    for i in range(0, n, step):
        eye = [cl[i][0], cl[i][1] + eye_up, cl[i][2]]
        j = (i + 8) % n
        fx, fz = cl[j][0] - cl[i][0], cl[j][2] - cl[i][2]
        fl = math.hypot(fx, fz)
        if fl < 1e-3:
            continue
        fx, fz = fx / fl, fz / fl
        rx, rz = -fz, fx
        if np is None:
            continue
        d = V - np.asarray(eye)
        z = d[:, 0] * fx + d[:, 2] * fz
        x = d[:, 0] * rx + d[:, 2] * rz
        y = d[:, 1]
        inside = (z > near) & (np.abs(x) <= tx * z) & (np.abs(y) <= ty * z)
        n0 = int((inside & (z <= mid)).sum())
        n1 = int((inside & (z > mid) & (z <= far)).sum())
        n2 = int((inside & (z > far)).sum())
        tot[0] += n0
        tot[1] += n1
        tot[2] += n2
        frac = n1 / max(1, n0 + n1)
        if frac > best[0]:
            best = (frac, (i, tuple(eye), (fx, fz)))
    return tot, best


def cam_spec(eye, fwd, reach=200.0):
    """B3_CAM="ex,ey,ez,tx,ty,tz" for a pinned verification shot."""
    return "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f" % (
        eye[0], eye[1], eye[2],
        eye[0] + fwd[0] * reach, eye[1], eye[2] + fwd[1] * reach)


def section_world(all_tracks):
    print("\n[2] WORLD -- geometry a %g far plane throws away" % OLD_FAR)
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("  SKIP numpy unavailable")
        return {}
    if not os.path.isdir(TRACKS):
        print("  SKIP build/tracks missing (run tools/extract_all_tracks.sh)")
        return {}

    names = sorted(d for d in os.listdir(TRACKS)
                   if os.path.isfile(os.path.join(TRACKS, d, "route.bin"))
                   and os.path.isfile(os.path.join(TRACKS, d, "track.obj")))
    if not all_tracks:
        names = [t for t in names if t in PROBE_TRACKS]
    rows = {}
    for t in names:
        try:
            tot, best = frustum_bands(t)
        except Exception as exc:            # a track's assets, not this check
            print("  skip %-10s %s" % (t, exc))
            continue
        vis = tot[0] + tot[1]
        frac = tot[1] / max(1, vis)
        rows[t] = dict(near=tot[0], band=tot[1], beyond=tot[2],
                       frac=frac, best=best)
        print("  %-10s in-frustum<=%gk %8d | clipped %g..%g %8d (%5.2f%%) "
              "| beyond %g %7d" % (t, OLD_FAR / 1000, tot[0], OLD_FAR,
                                   RETAIL_FAR, tot[1], 100 * frac,
                                   RETAIL_FAR, tot[2]))

    for t in PROBE_TRACKS:
        if t in rows:
            check(rows[t]["band"] > 0,
                  "%s has geometry in the %g..%g band (the defect)"
                  % (t, OLD_FAR, RETAIL_FAR))
    return rows


# ---------------------------------------------------------------- section 3
def read_bmp(path):
    """Minimal 24/32-bit uncompressed BMP reader -> (w, h, rows of RGB)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP: %s" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError("unsupported bpp %d" % bpp)
    bypp = bpp // 8
    stride = ((w * bypp + 3) // 4) * 4
    flip = h > 0
    h = abs(h)
    rows = []
    for r in range(h):
        base = off + r * stride
        row = data[base:base + w * bypp]
        rows.append(bytes(row))
    if flip:
        rows.reverse()          # BMP stores bottom-up
    return w, h, bypp, rows


def frame_diff(a_path, b_path, band=(0.10, 0.62), thresh=18):
    """Fraction of pixels in the horizon band that differ by more than
    `thresh` on any channel.  band is (top, bottom) as a fraction of image
    height counted from the top of the image -- the sky/skyline strip where
    everything past 5000 units has to land."""
    wa, ha, ba, ra = read_bmp(a_path)
    wb, hb, bb, rb = read_bmp(b_path)
    if (wa, ha) != (wb, hb):
        raise ValueError("frame size mismatch")
    y0 = int(ha * band[0])
    y1 = int(ha * band[1])
    try:
        import numpy as np
        A = np.frombuffer(b"".join(ra[y0:y1]), dtype=np.uint8)
        B = np.frombuffer(b"".join(rb[y0:y1]), dtype=np.uint8)
        A = A.reshape(y1 - y0, wa, ba)[:, :, :3].astype(np.int16)
        B = B.reshape(y1 - y0, wa, bb)[:, :, :3].astype(np.int16)
        d = (np.abs(A - B) > thresh).any(axis=2)
        return int(d.sum()), int(d.size)
    except ImportError:
        pass
    diff = 0
    total = 0
    for y in range(y0, y1):
        A, B = ra[y], rb[y]
        for x in range(wa):
            ia = x * ba
            ib = x * bb
            total += 1
            if (abs(A[ia] - B[ib]) > thresh or abs(A[ia + 1] - B[ib + 1]) > thresh
                    or abs(A[ia + 2] - B[ib + 2]) > thresh):
                diff += 1
    return diff, total


def run_shot(binary, track, cam, out_bmp, far=None, extra=None):
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
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_TRACK": track,
        "B3_CAM": cam,
        "B3_SHOT": out_bmp,
        "B3_SHOT_FRAME": "45",
        "B3_EXIT_AT": "5",
    })
    if far is not None:
        env["B3_VIEW_FAR"] = "%g" % far
    else:
        env.pop("B3_VIEW_FAR", None)
    if extra:
        env.update(extra)
    p = subprocess.run([binary], cwd=ROOT, env=env, timeout=300,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


# THE PROBE VIEWPOINTS.  Two long-sightline stations on the two courses that
# cross the big US coastal-mountain world (track.obj spans 30 km x 21 km on
# both).  Each is the harness camera pose recorded by B3_DUMP_FRAME at the
# frame where an autodriven lap showed the largest 5000-vs-10000 difference,
# so it is a MEASURED test viewpoint, not an authored constant: the car is on
# the cliff road and the city on the far shore sits 5-7 km away.  With a 5000
# far plane that whole skyline is missing.
#
#   US_M1_V1  frame 3270  car (3359.8, 160.9, -1395.5) yaw -1.37
#   US_P2_V1  frame 4500  car (3341.9, 160.7, -1398.0) yaw -1.44
#
# (eye = car pose backed off along the car's forward by the chase offset)
PROBE_CAMS = {
    "US_M1_V1": "3367.6,164.4,-1393.9,3171.6,164.4,-1433.8",
    "US_P2_V1": "3349.8,164.2,-1397.0,3151.5,164.2,-1423.1",
}
PROBE_TRACKS = list(PROBE_CAMS)
# Measured: 1.62% (US_M1_V1) and 1.78% (US_P2_V1) of the far band lights up
# when the plane moves 5000 -> 10000.  The band excludes the bottom HUD, and
# a control pair rendered from a viewpoint with NO far geometry measures
# 0.00% there, so the noise floor is nil.  Gate set at half the smaller
# measurement: it fails on a regression of the plane, not on dither.
MIN_BAND_GAIN = 0.008
#
# RECONCILE (2026-08-21) -- what the SCENERY pass did to this leg, and why
# the number above did not have to move.
# --------------------------------------------------------------------------
# Re-measured on the current renderer, both legs fresh, the same two pinned
# viewpoints, this pair now reads 0.08% (US_M1_V1) and 0.48% (US_P2_V1) -- and
# the reason is visible in the frames: src/burnout3_scenery.c draws near-field
# trees that stand directly across both pinned sightlines.  The distant
# geometry is still there and still admitted by the far plane; it is simply
# behind a fir.  The scenery pass is an independent feature with its own
# distance cull (the record's +0x68 far distance, FUN_0003A840 @0x0003A87E),
# and it is not the thing this section is about.
#
# So the pair is taken with that pass OFF on BOTH sides -- one variable, and
# it is the one named in the check -- which restores the sightline the
# viewpoints were chosen for (the city on the far shore is back in frame) and
# reproduces the original measurement to three digits:
#
#                       shipped   scenery off   originally recorded
#     US_M1_V1           0.079%      1.617%           1.62%
#     US_P2_V1           0.478%      1.781%           1.78%
#
# Half the smaller of the fresh pair is 0.808%, i.e. the same 0.008 the
# constant above already held.  Re-derived, not adjusted.
#
# The SHIPPED configuration is then gated too, separately and lower, because
# "the far plane still reaches the player's eye through the scenery" is worth
# its own red.  Its floor is half the smaller fresh measurement, 0.0395% ->
# 0.03%, and the noise floor under it is not "nil" but exactly zero: a pair
# rendered twice at the same far plane differs in 0 pixels of the band, on
# both tracks and at both planes (these frames are bit-reproducible).
MIN_BAND_GAIN_SHIPPED = 0.0003


def section_render(rows):
    print("\n[3] RENDER -- the far band of a pinned long-sightline frame")
    binary = os.environ.get("B3_BIN", os.path.join(ROOT, "burnout3"))
    if not os.path.isfile(binary):
        print("  SKIP no binary at %s (make burnout3, or set B3_BIN)" % binary)
        return
    outdir = os.environ.get("B3_DD_OUT", os.path.join(ROOT, "build", "drawdist"))
    os.makedirs(outdir, exist_ok=True)

    for t, cam in PROBE_CAMS.items():
        if not os.path.isdir(os.path.join(TRACKS, t)):
            print("  SKIP %s: build/tracks/%s missing" % (t, t))
            continue
        print("    %s cam=%s" % (t, cam))
        # (suffix, extra env, gate, what the leg says)
        legs = (
            # THE CONTROL LEG.  The scenery pass is a separate feature with a
            # separate distance cull and it stands across both pinned
            # sightlines; switching it off on BOTH sides leaves the view
            # frustum's far plane as the only variable.  See the note by
            # MIN_BAND_GAIN.
            ("noscenery", {"B3_SCENERY": "0"}, MIN_BAND_GAIN,
             "with the scenery pass off (one variable: the far plane)"),
            # ...and the shipped configuration, which is what the player gets.
            ("shipped", {}, MIN_BAND_GAIN_SHIPPED,
             "in the SHIPPED configuration, through the scenery"),
        )
        for suffix, extra, gate, what in legs:
            before = os.path.join(outdir, "%s_%s_far%d.bmp"
                                  % (t, suffix, int(OLD_FAR)))
            after = os.path.join(outdir, "%s_%s_far%d.bmp"
                                 % (t, suffix, int(RETAIL_FAR)))
            # a leftover frame from an earlier run would let a render that
            # never happened read as a pass, so clear the targets first.
            for p in (before, after):
                if os.path.exists(p):
                    os.remove(p)
            rc, log = run_shot(binary, t, cam, before, far=OLD_FAR,
                               extra=extra)
            if not os.path.isfile(before):
                check(False,
                      "%s: render at far=%g produced no frame (rc=%d)\n%s"
                      % (t, OLD_FAR, rc, log[-800:]))
                continue
            rc, log = run_shot(binary, t, cam, after, far=None, extra=extra)
            if not os.path.isfile(after):
                check(False,
                      "%s: render at retail far produced no frame (rc=%d)\n%s"
                      % (t, rc, log[-800:]))
                continue
            diff, total = frame_diff(before, after)
            frac = diff / max(1, total)
            print("    %s far band, %-9s: %6d/%d pixels light up (%.3f%%)"
                  % (t, suffix, diff, total, 100 * frac))
            check(frac >= gate,
                  "%s: retail far plane lights >= %.2f%% of the far band %s "
                  "(measured %.3f%%)" % (t, 100 * gate, what, 100 * frac))


def main():
    all_tracks = "--all" in sys.argv
    no_render = "--no-render" in sys.argv
    print("validate_draw_distance: race-view near/far law "
          "(near %g, far %g) [C]" % (RETAIL_NEAR, RETAIL_FAR))
    section_source()
    rows = section_world(all_tracks)
    if not no_render:
        section_render(rows)
    print("\n%d checks, %d failed" % (checks[0], len(fails)))
    for f in fails:
        print("  FAILED: %s" % f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
