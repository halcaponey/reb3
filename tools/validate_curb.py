#!/usr/bin/env python3
"""CURB-STRIKE PARITY -- the RE suspension vs retail's own, over a real curb.

WHY THIS EXISTS
    The user report is "on the RE backend, driving over a median curb makes
    the car rotate far too much; retail rides over curbs smoothly".  Curbs are
    the one piece of track geometry that is BOTH a wheel-ray target (a low
    horizontal top face) and a chassis-contact target (a near-vertical riser
    a few centimetres tall), so a divergence can come from either side.  This
    validator pins both.

THE TWO SIDES
    retail  tools/emulate_pipeline.py + validate_port._make_reloc_pipeline:
            the real x86 substep loop 0x0011C0A0..0x0011C16C executed under
            Unicorn with EDI = 2, so FUN_0011D460, FUN_0011AEF0, FUN_001239C0,
            FUN_00123FD0 and FUN_00109560 all run at their own addresses over
            a real polygon soup in veh+0x200.
    RE      tools/curb_traj.c -> b3_vehicle_step_full, with `soup_ground_ray`
            bound to THE SAME soup.  Retail's wheel ray walks veh+0x200
            (`mov eax,[esi+0x200]` @0x001237E5 [C]), so binding both hooks to
            one set is what makes this a SOLVER comparison rather than a
            geometry one.

THE CURB
    Height and surface types are measured out of the shipped collision data,
    not invented.  On US_C3_V1 (tools/../build/tracks/US_C3_V1/collision.bin,
    60373 tris) the 287 triangles that are true medians -- a low-to-high step
    with drivable road on both sides -- have step heights quantised to
    1/65.536 m, with 175 of them at exactly 0.152588 m and 48 at 0.198364 m.
    Their vertical faces carry surface 0x0011 (OfflinePavement, 535 of them)
    or 0x0018 (80), the low road beside them 0x000c (OfflineTarmac, 609).
    See the CURB_* constants below; regenerate with scan_curbs.py.

    0x0018 matters on its own: its low byte 0x18 > 0x14, which is exactly the
    band retail's wheel ray REFUSES TO TEST -- see the gate transcribed in
    tools/curb_traj.c and the `0x18 curb` case below.

EXPOSURE (all 36 extracted tracks, scan_wheelgate.py)
    4,069,262 collision triangles; 2,611,287 survive the loader exclusion
    rule; 1,051,235 of those (40.3%) are gated out of retail's wheel ray and
    were being ray-tested by the port.  81% of that is near-vertical wall
    geometry a downward ray rarely decides, but 121,640 are near-horizontal
    (|n.y| > 0.9) and 75,595 of those face UP -- plates the port's wheels
    could stand on and retail's cannot, worth +4.24% extra standable area.
    Surface low byte 0x18 alone is ~61% of it, on 30 of the 36 tracks.
    Median tops specifically are mostly fine: only 2.3% are gated overall,
    but that reaches 14.6% on US_C3, 10.4% on US_C2 and 7.5% on US_P1.

USAGE
    python3 tools/validate_curb.py [--verbose] [--wheelgate 0|1]

    --wheelgate 1 turns on the retail surface gate in the RE driver, which is
    the proposed fix; with it off the driver is today's port.
"""
import argparse
import importlib.util
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

# ---------------------------------------------------------------------------
# MEASURED CURB GEOMETRY (build/tracks/US_C3_V1/collision.bin)
# ---------------------------------------------------------------------------
CURB_H_COMMON = 0.152587890625   # 10 x 1/65.536; 175 of 287 medians
CURB_H_TALL = 0.198364257812     # 13 x 1/65.536; 48 of 287
CURB_SURF_PAVE = 0x0011          # OfflinePavement -- 535 curb faces
CURB_SURF_18 = 0x0018            # 80 curb faces; low byte 0x18 > 0x14
ROAD_SURF = 0x000c               # OfflineTarmac -- 609 adjoining low roads


def _ground_plane(y=0.0, surf=ROAD_SURF, s=5000.0):
    """The low road.  Winding matches validate_port._ground_plane."""
    n = (0.0, 1.0, 0.0)
    return [(((-s, y, -s), (-s, y, s), (s, y, -s)), n, surf),
            (((s, y, s), (s, y, -s), (-s, y, s)), n, surf)]


def _riser(px, pz, nx, nz, ylo, yhi, half=40.0, surf=CURB_SURF_PAVE):
    """A vertical face through (px, pz) with in-plane unit normal (nx,0,nz),
    spanning y in [ylo, yhi].  Winding taken from validate_port._tri_plane:
    (v1-v0) x (v2-v0) comes out along +n, which is what FUN_001B2230's
    one-sided `det > 1e-8` needs to see it from the front."""
    tx, tz = -nz, nx
    a = (px + tx * half, ylo, pz + tz * half)
    b = (px - tx * half, ylo, pz - tz * half)
    c = (px - tx * half, yhi, pz - tz * half)
    d = (px + tx * half, yhi, pz + tz * half)
    n = (nx, 0.0, nz)
    return [((a, b, c), n, surf), ((a, c, d), n, surf)]


def _top(px, pz, nx, nz, y, half=40.0, depth=60.0, surf=CURB_SURF_PAVE):
    """The island's horizontal top, hinged on the same line as the riser and
    extending `depth` metres AWAY from the car (along -n).  Corner order is
    chosen so (v1-v0) x (v2-v0) points +y."""
    tx, tz = -nz, nx
    A = (px + tx * half, y, pz + tz * half)
    B = (px - tx * half, y, pz - tz * half)
    C = (px - tx * half - nx * depth, y, pz - tz * half - nz * depth)
    D = (px + tx * half - nx * depth, y, pz + tz * half - nz * depth)
    n = (0.0, 1.0, 0.0)
    return [((A, B, C), n, surf), ((A, C, D), n, surf)]


def curb(p, dist=6.0, h=CURB_H_COMMON, yaw_deg=0.0, surf=CURB_SURF_PAVE,
         road=ROAD_SURF, top_surf=None):
    """A raised median crossing the car's path `dist` metres ahead.

    The reloc scenarios drive the car toward +z, so the riser's outward
    normal must have nz < 0 to face it.  `yaw_deg` rotates the whole island
    about y: 0 puts both front wheels on the step in the same substep (a pure
    PITCH strike), a non-zero angle staggers them (a ROLL strike, which is the
    median-clip the user reported)."""
    a = math.radians(yaw_deg)
    nx, nz = math.sin(a), -math.cos(a)
    px, pz = p[0], p[2] + dist
    return (_ground_plane(0.0, surf=road)
            + _riser(px, pz, nx, nz, 0.0, h, surf=surf)
            + _top(px, pz, nx, nz, h,
                   surf=surf if top_surf is None else top_surf))


# ---------------------------------------------------------------------------
# cases: name -> (checkpoint frame, window, inputs, soup builder, tolerances)
# ---------------------------------------------------------------------------
def _tall_wall(p, dist=1.6, surf=CURB_SURF_PAVE):
    """A full-height head-on wall -- the CONTROL that must crash on both
    backends, so a green curb leg cannot be green merely because the crash
    trigger is dead."""
    return (_ground_plane(0.0)
            + _riser(p[0], p[2] + dist, 0.0, -1.0, 0.0, 6.0, surf=surf))


# ---------------------------------------------------------------------------
# REAL CURB GEOMETRY
#
# The synthetic risers above are two big triangles, which makes every one of
# them a "sliver" under FUN_0011AC30's near-vertical reject (FUN_0011ABB0):
# their short edge is purely vertical.  Real track curbs are triangulated
# differently and 5.2% of the shipped short near-vertical faces do NOT have a
# sliver edge -- those are the ones retail's contact test actually admits.
# Testing only the synthetic shape would therefore never exercise the arm the
# user is hitting.
#
# What is kept from the file is the part the filters read: the real
# TRIANGULATION (edge lengths decide the sliver test), the real step height
# (decides the box clip), the real normal and the real surface id.  The
# triangles are rigidly relocated (rotation about y + translation, both
# orientation-preserving, so winding and one-sidedness survive) so the curb
# sits `dist` ahead of the harness car at `yaw_deg`, with its base at y = 0 on
# the synthetic road plane.  Importing the surrounding road as well would drag
# in that site's camber and slope and make the approach non-deterministic
# without touching any filter this test is about.
# ---------------------------------------------------------------------------
_COL_CACHE = {}


def _load_collision(track):
    if track in _COL_CACHE:
        return _COL_CACHE[track]
    import numpy as np
    p = os.path.join(ROOT, 'build', 'tracks', track, 'collision.bin')
    b = open(p, 'rb').read()
    if b[:4] != b'B3CL':
        raise RuntimeError('%s: not B3CL' % p)
    import struct as _s
    ver, count = _s.unpack_from('<II', b, 4)
    rec = np.frombuffer(b[0x28:0x28 + 40 * count],
                        dtype=np.uint8).reshape(count, 40)
    v = rec[:, :36].copy().view(np.float32).reshape(count, 3, 3)
    typ = rec[:, 36:38].copy().view(np.uint16).reshape(count)
    e1, e2 = v[:, 1] - v[:, 0], v[:, 2] - v[:, 0]
    n = np.cross(e1, e2)
    L = np.linalg.norm(n, axis=1, keepdims=True)
    L[L < 1e-12] = 1.0
    n = n / L
    lo = typ & 0xFF
    excl = (lo == 0x22) | (lo == 0x23) | ((typ & 0x1000) != 0)
    _COL_CACHE[track] = (v, typ, n, excl)
    return _COL_CACHE[track]


def real_curb(p, track='US_C3_V1', tri=12092, dist=6.0, yaw_deg=20.0,
              patch=8.0, road=ROAD_SURF):
    """The real curb face `tri` of `track`, with the real riser and island-top
    triangles within `patch` metres of it, relocated in front of the car."""
    import numpy as np
    v, typ, n, excl = _load_collision(track)
    base_y = float(v[tri, :, 1].min())
    top_y = float(v[tri, :, 1].max())
    cen = v[tri].mean(axis=0)
    # the local patch: more of the same riser, plus the island top it carries
    d2 = ((v[:, :, 0].mean(axis=1) - cen[0]) ** 2
          + (v[:, :, 2].mean(axis=1) - cen[2]) ** 2)
    near = (~excl) & (d2 < patch * patch)
    ymin, ymax = v[:, :, 1].min(axis=1), v[:, :, 1].max(axis=1)
    riser = near & (np.abs(n[:, 1]) < 0.35) & (ymin > base_y - 0.15) \
        & (ymax < top_y + 0.15)
    top = near & (n[:, 1] > 0.9) & (np.abs(ymin - top_y) < 0.06)
    idx = np.nonzero(riser | top)[0]
    if idx.size == 0:
        raise RuntimeError('%s tri %d: empty patch' % (track, tri))
    if idx.size > 200:                       # nearest first, both soups cap
        idx = idx[np.argsort(d2[idx])[:200]]
    # rotate so the face normal points back at the car, then yaw it
    a = math.radians(yaw_deg)
    tgt = math.atan2(-math.cos(a), math.sin(a))
    th = math.atan2(n[tri][2], n[tri][0]) - tgt
    ct, st = math.cos(th), math.sin(th)

    def xf(q):
        x, y, z = float(q[0]) - cen[0], float(q[1]) - base_y, \
            float(q[2]) - cen[2]
        # land it in FRONT OF THE CAR, which after the 240-frame accelerate
        # checkpoint is a long way down +z from the world origin
        return (x * ct + z * st + p[0], y, -x * st + z * ct + p[2] + dist)

    def xfn(q):
        x, y, z = float(q[0]), float(q[1]), float(q[2])
        return (x * ct + z * st, y, -x * st + z * ct)

    out = _ground_plane(0.0, surf=road)
    for i in idx:
        out.append((tuple(xf(v[i, k]) for k in range(3)), xfn(n[i]),
                    int(typ[i])))
    return out


def live_chassis(tris, ny_max=0.70):
    """The subset src/burnout3_full.c's harness_soup_freeze actually hands the
    contact resolve: b3_collision_filter_walls keeps only
    `n.y >= -0.70 && |n.y| <= 0.70`, so every ground and island-top face is
    dropped before b3_crash_response ever sees it.  Retail's own gather
    (FUN_0011BBE0 @0x0011BC43) rejects `n.y < -0.7` and nothing else, so
    retail's resolve DOES see them.  Passing this as the port's chassis soup
    while retail keeps the full set is what isolates that difference."""
    return [t for t in tris if -0.70 <= t[1][1] <= ny_max]


_TOL = dict(pos=2e-3, vel=1e-2, omega=2e-2, roll_pct=10.0, pitch_pct=10.0,
            roll_abs=0.05, pitch_abs=0.05,
            # ride smoothness: peak body rate per axis (deg/s) and the peak
            # frame-to-frame lateral / vertical velocity step (m/s)
            rate=1.0, kick=2e-2)

CASES = {
    # square-on: both front wheels step up together -> pitch only
    'median 0.153 m, square on':
        (240, 45, (1.0, 0.0, 0.0, 0), lambda p: curb(p, 6.0, CURB_H_COMMON)),
    # the reported case: an angled median clip, front wheels staggered
    'median 0.153 m, 20 deg clip':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_COMMON, yaw_deg=20.0)),
    'median 0.153 m, 40 deg clip':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_COMMON, yaw_deg=40.0)),
    # the second measured height mode
    'median 0.198 m, 20 deg clip':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_TALL, yaw_deg=20.0)),
    # surface 0x0018: low byte 0x18 > 0x14, so retail's wheel ray
    # (FUN_00123790 @0x00123822..0x0012383E [C]) refuses to test it and the
    # wheels roll straight through.  80 curb faces on US_C3_V1 carry it.
    'median 0x0018 (wheel ray must ignore)':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_COMMON, yaw_deg=20.0,
                        surf=CURB_SURF_18)),
    # OVER-GATING GUARD.  The riser is 0x0018 but the TOP is 0x0011, which is
    # the mix the data actually shows (US_C3_V1: 535 curb faces 0x0011 vs 80
    # 0x0018, 502 median tops 0x0011 vs 56 0x0018 -- correlated, not
    # identical).  A wheel stands on the TOP, so this curb must still be
    # mounted, at retail's exact excursion.  Gating the riser as well would
    # show up here as a false 0.000 deg.
    'median, gated riser + drivable top':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_COMMON, yaw_deg=20.0,
                        surf=CURB_SURF_18, top_surf=CURB_SURF_PAVE)),
    # control: flat road, no curb.  Must be bit-tight on both sides.
    'flat road (control)':
        (240, 45, (1.0, 0.0, 0.0, 0), lambda p: _ground_plane(0.0)),

    # -- CRASH-TRIGGER LEGS ------------------------------------------------
    # The user's remaining symptom: "I am still crashing a 'crash' when
    # driving into short curbs that would in retail be handled with the car's
    # suspension."  Every curb leg above and below asserts crash=0 on BOTH
    # sides; the wall control below asserts crash=1 on both, so a green run
    # cannot mean the trigger is simply dead.
    #
    # REAL geometry, not the synthetic riser: 94.8% of the shipped short
    # near-vertical faces are killed by FUN_0011AC30's sliver reject, so the
    # synthetic shape (whose short edge is purely vertical) can only ever
    # exercise the rejected path.  These indices are the SURVIVORS -- faces
    # whose real triangulation has no sliver edge, which is what reaches the
    # box clip and the crash trigger.
    'REAL curb US_C3_V1 #12092 (0.198 m, 0x0011)':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'US_C3_V1', 12092, 6.0, 20.0),
         dict(crash=0)),
    'REAL curb US_C3_V1 #12092, head-on':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'US_C3_V1', 12092, 6.0, 0.0),
         dict(crash=0)),
    'REAL curb EU_C1_V2 #16701 (0.107 m, 0x0013)':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'EU_C1_V2', 16701, 6.0, 20.0),
         dict(crash=0)),
    'REAL curb EU_C1_V2 #16701, head-on':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'EU_C1_V2', 16701, 6.0, 0.0),
         dict(crash=0)),
    # ...and the same real curbs with the LIVE port's chassis soup: the
    # |n.y| > 0.70 filter drops every island-top and road face before the
    # resolve sees it, while retail keeps them.  This is suspect (2) from the
    # wheel-gate report, isolated.
    # THIS CASE CARRIES A SECOND, SEPARATE DEFECT, pinned not hidden.
    # With the live chassis filter the port reports contact_state_198 = 0 for
    # 13 frames where retail reports 2 (a GROUND contact).  Retail's ground
    # arm is record-only -- no impulse, no crash -- so it does NOT cause the
    # crash symptom; the wall and ground arms are mutually exclusive with wall
    # winning (b3_crash_response), so admitting ground cannot suppress a wall
    # crash either.  What it does change is that retail OVERWRITES the contact
    # record (+0x160/+0x170/+0x190/+0x194) every frame the body is over
    # ground, while the port leaves the previous WALL record standing --
    # FUN_0011AEF0 rewrites only +0x198 unconditionally.  Fix is to stop
    # dropping ground from the chassis soup (retail's FUN_0011BBE0
    # @0x0011BC43 rejects only n.y < -0.7); deferred because it changes which
    # faces occupy the 96-poly budget and wants a live run to land safely.
    'REAL curb US_C3_V1 #12092, LIVE chassis filter':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'US_C3_V1', 12092, 6.0, 20.0),
         dict(crash=0, chassis='live',
              known=dict(kind='cstate', n=13,
                         why='ground arm suppressed by the |n.y|>0.70 '
                             'chassis filter; retail says 2, port says 0'))),
    # ...and the SAME strike with the filter FIXED to retail's own rule
    # (FUN_0011BBE0 @0x0011BC43 bounds n.y only from below).  The ground
    # faces come back, retail's ground arm runs, and the 13-frame cstate
    # divergence above must be GONE -- no `known` allowance here.
    'REAL curb US_C3_V1 #12092, LIVE chassis filter FIXED':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'US_C3_V1', 12092, 6.0, 20.0),
         dict(crash=0, chassis='live', ny_max=1.01)),
    'REAL curb EU_C1_V2 #16701, LIVE chassis filter':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: real_curb(p, 'EU_C1_V2', 16701, 6.0, 0.0),
         dict(crash=0, chassis='live')),
    # synthetic medians must not crash either, at low authority where the
    # bars are 1.375 m/s of dv and 0.0354 of head-on
    'median 0.198 m head-on, authority 0.05':
        (240, 45, (1.0, 0.0, 0.0, 0),
         lambda p: curb(p, 6.0, CURB_H_TALL, yaw_deg=0.0),
         dict(crash=0, authority=0.05)),
    # THE CONTROL: a full-height wall head-on at the same authority MUST fire
    # on both backends.
    'CONTROL tall wall head-on, authority 0.05 (must crash)':
        (240, 25, (1.0, 0.0, 0.0, 0), lambda p: _tall_wall(p, 0.4),
         # a sustained head-on wall stall re-solves the impulse from a
         # re-derived centroid every substep -- validate_port carries the same
         # widened band on its own deep-wall cases.  The CRASH VERDICT is the
         # claim here and it is exact.
         dict(crash=1, authority=0.05,
              tol=dict(pos=5e-3, vel=1e-1, omega=1e-1, rate=5.0, kick=1e-1))),
}


# ---------------------------------------------------------------------------
def _roll_pitch(rec):
    """Body attitude from the frame rows, identical formula on both sides.
    roll  = rotation about the travel axis  (right.y vs up.y)
    pitch = nose elevation                  (at.y)"""
    right, up, at = rec['right'], rec['up'], rec['at']
    roll = math.degrees(math.atan2(right[1], up[1]))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, at[1]))))
    return roll, pitch


def _lat_kick(seq):
    """The RIDE-SMOOTHNESS metric the user is describing: the largest
    frame-to-frame step in the body-LATERAL and body-VERTICAL velocity
    components.  A curb that is ridden out by the suspension moves these
    smoothly; a curb that is being resolved as a body contact steps them.
    Resolved onto the body axes so a yawing car does not read as a kick."""
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    lat = [dot(r['vel'], r['right']) for r in seq]
    ver = [r['vel'][1] for r in seq]
    dl = max((abs(lat[i] - lat[i - 1]) for i in range(1, len(lat))),
             default=0.0)
    dv = max((abs(ver[i] - ver[i - 1]) for i in range(1, len(ver))),
             default=0.0)
    return dl, dv


def _body_rates(rec):
    """omega resolved onto the body axes: (roll rate, pitch rate, yaw rate)."""
    w = rec['omega']
    right, up, at = rec['right'], rec['up'], rec['at']
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    return (math.degrees(dot(w, at)), math.degrees(dot(w, right)),
            math.degrees(dot(w, up)))


RE_UNITS = ('burnout3_vehicle_sim.c', 'burnout3_crash.c', 'burnout3_panels.c',
            'burnout3_backend.c', 'burnout3_emu.c')


def build_re_driver(patched=None):
    """Build tools/curb_traj.c against the repo tree, or against a directory
    holding patched copies of any src/ file (`patched` wins on the include
    path and per translation unit)."""
    exe = os.path.join(ROOT, 'build',
                       'curb_traj_patched' if patched else 'curb_traj')
    inc = ['-I' + os.path.join(ROOT, 'src')]
    extra = []
    if patched:
        inc = ['-I' + patched] + inc
    # The gate landed in the live tree after this suite was written: detect it
    # from whichever header the build will actually include.
    hdr_dir = patched if patched else os.path.join(ROOT, 'src')
    hdr = os.path.join(hdr_dir, 'burnout3_vehicle_sim.h')
    if not os.path.exists(hdr):
        hdr = os.path.join(ROOT, 'src', 'burnout3_vehicle_sim.h')
    with open(hdr) as fh:
        if 'wheel_gate' in fh.read():
            extra = ['-DB3_SOUP_GROUND_RAY_HAS_GATE']
    src = [os.path.join(ROOT, 'tools', 'curb_traj.c')]
    for f in RE_UNITS:
        p = os.path.join(patched, f) if patched else None
        src.append(p if p and os.path.exists(p) else os.path.join(ROOT, 'src', f))
    r = subprocess.run(['cc', '-O2'] + inc + extra + ['-o', exe] + src
                       + ['-lm'], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-2000:])
        return None
    return exe


# The retail truth table for the gate, straight off FUN_00123790
# @0x00123799..0x0012383E: for veh+0x215 in {1,2,3} with veh+0x210 == 0 a
# polygon is ray-tested iff its surface low byte is <= 0x14 or == 0x26.
def _retail_testable(type_u16, class_215):
    if class_215 not in (1, 2, 3):
        return 1
    lo = type_u16 & 0xFF
    return 1 if (lo == 0x26 or lo <= 0x14) else 0


def check_gate_predicate(patched):
    """Link the PATCHED b3_collision_wheel_surface_testable and assert it
    against that table over every low byte and every class byte that occurs."""
    csrc = os.path.join(patched, 'burnout3_collision.c')
    if not os.path.exists(csrc):
        return None
    probe = os.path.join(ROOT, 'build', 'curb_gate_probe.c')
    with open(probe, 'w') as f:
        f.write('#include <stdio.h>\n#include "burnout3_collision.h"\n'
                'int main(void){for(unsigned c=0;c<6;c++)'
                'for(unsigned t=0;t<0x100;t++)'
                'printf("%u %u %d\\n",c,t,'
                'b3_collision_wheel_surface_testable((unsigned short)t,'
                '(unsigned char)c));return 0;}\n')
    exe = os.path.join(ROOT, 'build', 'curb_gate_probe')
    r = subprocess.run(['cc', '-O2', '-I' + patched,
                        '-I' + os.path.join(ROOT, 'src'), '-o', exe, probe,
                        csrc, '-lm'], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-1500:])
        return False
    out = subprocess.run([exe], capture_output=True, text=True).stdout
    bad = []
    for line in out.splitlines():
        c, t, got = (int(x) for x in line.split())
        want = _retail_testable(t, c)
        if got != want:
            bad.append((c, t, got, want))
    # the flag is a u16: the high byte must not change the verdict
    print("  gate predicate: %d/%d (class, low byte) pairs match "
          "FUN_00123790's table%s"
          % (1536 - len(bad), 1536,
             "" if not bad else "  MISMATCHES: %s" % bad[:6]))
    return not bad



# ---------------------------------------------------------------------------
# THE CONTACT-ADMISSION LEG
#
# The harness has a SECOND wall-crash producer besides FUN_0011AEF0's own
# verdict: mesh_collide's sphere sweep, whose hit goes to b3_td_wall_contact ->
# b3_crash_wall_eval and is OR-ed in at `wall_fire = cfire || fire`.  Its maths
# is retail's, but its CONTACT ADMISSION was the sphere's, and the sphere
# (centre pos.y + 0.3, radius 1.0) bottoms out 0.200 m BELOW the road while
# retail's chassis box floor sits 0.1612 m ABOVE it -- so every median between
# those two was reported to the crash trigger as a wall retail never touches.
#
# b3_crash_poly_admits is FUN_0011AC30's own admission, factored out.  This
# leg asserts (a) it agrees with the verified accumulator on every shape, and
# (b) its height floor really is the chassis box floor, and (c) a sliver-shaped
# riser is refused at every curb height.
# ---------------------------------------------------------------------------
_ADMIT_PROBE = r"""
#include <stdio.h>
#include <string.h>
#include "burnout3_crash.h"
static const float BBMAX[4] = {1.0157f, 1.1222f, 2.0636f, 0.0f};
static const float BBMIN[4] = {-1.0157f, -0.1505f, -2.0866f, 2.0636f};
static const float HUB = 0.3117f;
static void mkinv(float inv[4][4]){memset(inv,0,64);
  inv[0][0]=inv[1][1]=inv[2][2]=1.0f; inv[3][1]=-HUB;}
/* a riser STRIP with both end edges slanted by SL, so no edge is a sliver --
   the shape the shipped survivors have (US_C3_V1 tri 12092: the short-dy edge
   runs 0.464 m horizontally, and 0.464^2 > 0.2). */
static void riser(float h, B3CrashPoly* t0, B3CrashPoly* t1){
  const float L=6.0f, Z=2.0f, SL=0.6f;
  float a[3]={-L,0,Z}, b[3]={L,0,Z}, c[3]={L+SL,h,Z}, d[3]={-L+SL,h,Z};
  float* p0[3]={t0->p0,t0->p1,t0->p2}; float* p1[3]={t1->p0,t1->p1,t1->p2};
  float* s0[3]={a,b,c}; float* s1[3]={a,c,d};
  for(int i=0;i<3;i++){for(int k=0;k<3;k++){p0[i][k]=s0[i][k];p1[i][k]=s1[i][k];}
    p0[i][3]=p1[i][3]=0.0f;}
  for(int k=0;k<3;k++){t0->n[k]=(k==2)?-1.0f:0.0f;t1->n[k]=t0->n[k];}
  t0->n[3]=t1->n[3]=0.0f;}
/* the SLIVER shape: the split edge is purely vertical */
static void riser_sliver(float h, B3CrashPoly* t){
  const float L=6.0f, Z=2.0f;
  float a[3]={-L,0,Z}, b[3]={L,0,Z}, c[3]={L,h,Z};
  float* p[3]={t->p0,t->p1,t->p2}; float* s[3]={a,b,c};
  for(int i=0;i<3;i++){for(int k=0;k<3;k++)p[i][k]=s[i][k];p[i][3]=0.0f;}
  t->n[0]=0;t->n[1]=0;t->n[2]=-1.0f;t->n[3]=0;}
static int accum(const B3CrashPoly* p,const float inv[4][4]){
  B3CrashContactAcc a; b3_crash_acc_init(&a,inv,BBMAX,BBMIN);
  b3_crash_poly_contact(p,0x0011,&a); return (a.wall_count+a.gnd_count)>0;}
int main(void){
  float inv[4][4]; mkinv(inv); int bad=0,n=0; float first=-1.0f; int sl_bad=0;
  for(int mm=2;mm<=80;mm++){ float h=mm*0.01f; B3CrashPoly t0,t1;
    riser(h,&t0,&t1);
    for(int w=0;w<2;w++){ const B3CrashPoly* q=w?&t1:&t0;
      int A=b3_crash_poly_admits(q,(const float(*)[4])inv,BBMAX,BBMIN);
      int B=accum(q,(const float(*)[4])inv); n++; if(A!=B)bad++;
      if(A&&first<0.0f)first=h; }
    riser_sliver(h,&t0);
    int A=b3_crash_poly_admits(&t0,(const float(*)[4])inv,BBMAX,BBMIN);
    int B=accum(&t0,(const float(*)[4])inv); n++; if(A!=B)bad++;
    if(h<0.5f&&A)sl_bad++; }
  /* CLIPPING SAFETY: a real wall must still be admitted, or gating the
     push-out would let the car drive through barriers. */
  B3CrashPoly w0,w1; riser(6.0f,&w0,&w1);
  int wall_ok = b3_crash_poly_admits(&w0,(const float(*)[4])inv,BBMAX,BBMIN)
             || b3_crash_poly_admits(&w1,(const float(*)[4])inv,BBMAX,BBMIN);
  printf("%d %d %.3f %d %.4f %d\n",n-bad,n,first,sl_bad,HUB+BBMIN[1],wall_ok);
  return 0;}
int b3_ground_probe(float x,float y,float z,float* h,float nn[3]){
  (void)x;(void)y;(void)z;*h=0;nn[0]=0;nn[1]=1;nn[2]=0;return 0;}
"""


def check_admit_predicate(srcdir):
    """Compile b3_crash_poly_admits from `srcdir` and assert it IS
    FUN_0011AC30's admission.  Returns True/False, or None if the patch that
    introduces the predicate is not present in that tree."""
    csrc = os.path.join(srcdir, 'burnout3_crash.c')
    if not os.path.exists(csrc) or 'b3_crash_poly_admits' not in open(csrc).read():
        return None
    probe = os.path.join(ROOT, 'build', 'curb_admit_probe.c')
    with open(probe, 'w') as f:
        f.write(_ADMIT_PROBE)
    exe = os.path.join(ROOT, 'build', 'curb_admit_probe')
    units = [csrc] + [os.path.join(ROOT, 'src', u) for u in
                      ('burnout3_vehicle_sim.c', 'burnout3_panels.c',
                       'burnout3_backend.c', 'burnout3_emu.c')]
    r = subprocess.run(['cc', '-O2', '-I' + srcdir,
                        '-I' + os.path.join(ROOT, 'src'), '-o', exe, probe]
                       + units + ['-lm'], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-1500:])
        return False
    out = subprocess.run([exe], capture_output=True, text=True).stdout.split()
    agree, total, floor, sliver_bad, box, wall_ok = (
        int(out[0]), int(out[1]), float(out[2]), int(out[3]), float(out[4]),
        int(out[5]))
    ok = (agree == total and sliver_bad == 0 and abs(floor - box) <= 0.011
          and wall_ok == 1)
    print("  contact admission: %d/%d agree with FUN_0011AC30's accumulator; "
          "height floor %.3f m vs chassis box floor %.4f m; "
          "sliver risers admitted %d (must be 0)   %s"
          % (agree, total, floor, box, sliver_bad, "OK" if ok else "FAIL"))
    print("    (the sphere sweep this gates bottoms out 0.200 m BELOW the "
          "road, so it saw every curb; retail's floor is %.4f m ABOVE it)"
          % box)
    print("    CLIPPING SAFETY: a 6 m wall is still admitted -> %s, so the "
          "body push-out still acts on real barriers"
          % ("YES" if wall_ok else "NO -- WALLS WOULD BE DRIVEN THROUGH"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--wheelgate', type=int, default=0,
                    help="RE driver: enable retail's FUN_00123790 surface "
                         "gate (the proposed fix)")
    ap.add_argument('--only', default=None)
    ap.add_argument('--patched', default=None,
                    help='directory of patched src/ copies; builds the RE '
                         'driver against them with the gated ray hook and '
                         'checks the gate predicate directly')
    args = ap.parse_args()

    spec = importlib.util.spec_from_file_location(
        "ep", os.path.join(ROOT, "tools", "emulate_pipeline.py"))
    ep = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ep)
    vspec = importlib.util.spec_from_file_location(
        "vp", os.path.join(ROOT, "tools", "validate_port.py"))
    vp = importlib.util.module_from_spec(vspec)
    vspec.loader.exec_module(vp)
    Reloc = vp._make_reloc_pipeline(ep)

    class CurbPipeline(Reloc):
        """Reloc plus the attitude rows and the wheel fields this test reads."""

        def capture(self):
            cap = super().capture()
            cap['right'] = [self.rf(ep.CTX0 + 0x00 + 4 * i) for i in range(3)]
            cap['wheels'] = [
                dict(cur=self.vf(0x820 + 0xC0 * i + 0x64),
                     prev=self.vf(0x820 + 0xC0 * i + 0x60),
                     contact=self.vb(0x820 + 0xC0 * i + 0xB3),
                     bump=self.vb(0x820 + 0xC0 * i + 0xB2),
                     surf=self.vb(0x820 + 0xC0 * i + 0xB0),
                     n=[self.vf(0x820 + 0xC0 * i + 0x20 + 4 * k)
                        for k in range(3)])
                for i in range(4)]
            return cap

    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    exe = build_re_driver(args.patched)
    if not exe:
        print("cannot build the RE driver")
        return 1
    fails = 0
    if args.patched:
        ok = check_gate_predicate(args.patched)
        if ok is False:
            fails += 1
    admit_src = args.patched or os.path.join(ROOT, 'src')
    ok = check_admit_predicate(admit_src)
    if ok is False:
        fails += 1
    elif ok is None:
        print("  contact admission: b3_crash_poly_admits not in this tree "
              "(the mesh_collide crash-report gate is UNPATCHED)")

    print("CURB STRIKE -- retail 0x0011C0A0..0x0011C16C vs "
          "b3_vehicle_step_full   (%s)"
          % ("PATCHED tree: the port owns the gate"
             if args.patched else "RE wheelgate=%d" % args.wheelgate))
    print("  curb heights are measured: 0.152588 m (175/287 medians) and "
          "0.198364 m (48/287)")
    for name, spec in CASES.items():
        ckpt, wlen, inp, mk = spec[:4]
        opts = dict(crash=None, chassis='same', authority=1.0, tol=None,
                    known=None, ny_max=0.70)
        if len(spec) > 4:
            opts.update(spec[4])
        if args.only and args.only not in name:
            continue
        try:
            p = CurbPipeline()
            inputs = ep.scenario_inputs('accelerate')
            for i in range(ckpt):
                p.frame(*inputs[i])
            p.wf(ep.VEHICLE + 0x1534, opts['authority'])
            pos = [p.rf(ep.CTX0 + 0x30 + 4 * i) for i in range(3)]
            tris = mk(pos)
            p.set_soup(tris)
            tag = ''.join(c if c.isalnum() else '_' for c in name)[:28]
            sf = os.path.join(ROOT, 'build', 'curb_%s.txt' % tag)
            p.write_state(sf)
            base_fired = p.crash_fired
            emu = [p.frame(*inp) for _ in range(wlen)]
        except Exception as e:
            print("  %-38s emulation: %s" % (name, e))
            fails += 1
            continue

        def _write(path, tt):
            with open(path, 'w') as f:
                for verts, n, surf in tt:
                    f.write(' '.join('%.9g' % x for q in verts for x in q))
                    f.write(' %.9g %.9g %.9g %d\n' % (n[0], n[1], n[2], surf))
        soupf = sf.replace('.txt', '_soup.txt')
        _write(soupf, tris)
        argv = [exe, '--state', sf, str(wlen), str(inp[0]), str(inp[1]),
                str(inp[2]), str(inp[3]), '--soup', soupf,
                '--authority', str(opts['authority']),
                '--wheelgate', str(args.wheelgate)]
        if opts['chassis'] == 'live':
            cf = sf.replace('.txt', '_chassis.txt')
            _write(cf, live_chassis(tris, opts['ny_max']))
            argv += ['--chassis-soup', cf]
        cw = subprocess.run(argv, capture_output=True, text=True)
        try:
            port = [json.loads(l) for l in cw.stdout.splitlines()]
        except Exception as e:
            print("  %-38s port json: %s" % (name, e))
            fails += 1
            continue
        if len(port) != wlen:
            print("  %-38s port produced %d/%d frames: %s"
                  % (name, len(port), wlen, cw.stderr[:160]))
            fails += 1
            continue

        # excursions are measured RELATIVE to the attitude at the checkpoint
        e0r, e0p = _roll_pitch(emu[0])
        p0r, p0p = _roll_pitch(port[0])
        e_roll = max(abs(_roll_pitch(e)[0] - e0r) for e in emu)
        e_pitch = max(abs(_roll_pitch(e)[1] - e0p) for e in emu)
        m_roll = max(abs(_roll_pitch(m)[0] - p0r) for m in port)
        m_pitch = max(abs(_roll_pitch(m)[1] - p0p) for m in port)
        e_rate = [max(abs(_body_rates(e)[k]) for e in emu) for k in range(3)]
        m_rate = [max(abs(_body_rates(m)[k]) for m in port) for k in range(3)]
        e_lat, e_ver = _lat_kick(emu)
        m_lat, m_ver = _lat_kick(port)

        worst = dict(pos=0.0, vel=0.0, omega=0.0)
        bad = []
        for i, (e, m) in enumerate(zip(emu, port)):
            for j in range(3):
                worst['pos'] = max(worst['pos'], abs(e['pos'][j] - m['pos'][j]))
                worst['vel'] = max(worst['vel'], abs(e['vel'][j] - m['vel'][j]))
                worst['omega'] = max(worst['omega'],
                                     abs(e['omega'][j] - m['omega'][j]))
            for k in range(4):
                if e['wheels'][k]['contact'] != m['wheels'][k]['contact']:
                    bad.append((i, 'w%d contact' % k,
                                e['wheels'][k]['contact'],
                                m['wheels'][k]['contact']))
                if e['wheels'][k]['bump'] != m['wheels'][k]['bump']:
                    bad.append((i, 'w%d bump' % k, e['wheels'][k]['bump'],
                                m['wheels'][k]['bump']))
            if e['cstate'] != m['cstate']:
                bad.append((i, 'cstate', e['cstate'], m['cstate']))
            ef = 1 if (e['cfire'] - base_fired) > 0 else 0
            if ef != (1 if m['cfire'] else 0):
                bad.append((i, 'crashfire', ef, m['cfire']))

        def pct(a, b, floor):
            if max(abs(a), abs(b)) < floor:
                return 0.0
            return abs(a - b) / max(abs(a), abs(b), 1e-9) * 100.0

        roll_pct = pct(e_roll, m_roll, _TOL['roll_abs'])
        pitch_pct = pct(e_pitch, m_pitch, _TOL['pitch_abs'])
        # THE CRASH LEG.  `crash` in the case options is the expected verdict
        # for BOTH backends: 0 = the strike must be ridden out by the
        # suspension, 1 = the control that must actually fire.  Reported
        # always, asserted when the case names an expectation.
        e_fire = 1 if (emu[-1]['cfire'] - base_fired) > 0 else 0
        m_fire = 1 if port[-1]['cfire'] else 0
        e_wallf = sum(1 for e in emu if e['cstate'] == 1)
        m_wallf = sum(1 for m in port if m['cstate'] == 1)
        crash_ok = True
        if opts['crash'] is not None:
            crash_ok = (e_fire == opts['crash'] and m_fire == opts['crash'])
        tol = dict(_TOL)
        if opts['tol']:
            tol.update(opts['tol'])
        # A KNOWN, SEPARATELY-TRACKED divergence.  Not a blanket xfail: the
        # kind and the exact count are pinned, so the case still fails if the
        # divergence grows, shrinks or changes character.
        known_note = ''
        if opts['known'] and bad:
            kind, want_n = opts['known']['kind'], opts['known']['n']
            if all(b[1] == kind for b in bad) and len(bad) == want_n:
                known_note = ('   [KNOWN: %d x %s -- %s]'
                              % (len(bad), kind, opts['known']['why']))
                bad = []
        # RIDE SMOOTHNESS.  Peak body rate per axis and the peak
        # lateral/vertical velocity step, each asserted against executed
        # retail.  These are the numbers behind "it jerks into different
        # rotations depending on the curb": if the port resolves a curb the
        # retail body never touches, the rate peaks and the lateral step
        # separate even when the mean trajectory still looks close.
        rate_err = max(abs(m_rate[k] - e_rate[k]) for k in range(3))
        kick_err = max(abs(m_lat - e_lat), abs(m_ver - e_ver))
        smooth_ok = (rate_err < tol['rate'] and kick_err < tol['kick'])
        ok = (smooth_ok and worst['pos'] < tol['pos'] and worst['vel'] < tol['vel']
              and worst['omega'] < tol['omega']
              and roll_pct < tol['roll_pct']
              and pitch_pct < tol['pitch_pct'] and not bad and crash_ok)
        fails += not ok
        print("  %-38s %s  roll %6.3f/%6.3f deg (%5.1f%%)  pitch "
              "%6.3f/%6.3f deg (%5.1f%%)  pos %.1e vel %.1e omega %.1e"
              % (name, "OK  " if ok else "FAIL", m_roll, e_roll, roll_pct,
                 m_pitch, e_pitch, pitch_pct, worst['pos'], worst['vel'],
                 worst['omega']))
        print("      peak body rates deg/s   RE roll %7.1f pitch %7.1f "
              "yaw %7.1f | retail roll %7.1f pitch %7.1f yaw %7.1f"
              "   (worst axis %.3f, bar %.1f)"
              % (m_rate[0], m_rate[1], m_rate[2],
                 e_rate[0], e_rate[1], e_rate[2], rate_err, tol['rate']))
        print("      peak velocity STEP m/s   RE lat %.4f vert %.4f | "
              "retail lat %.4f vert %.4f   (worst %.2e, bar %.0e)  %s"
              % (m_lat, m_ver, e_lat, e_ver, kick_err, tol['kick'],
                 "smooth-OK" if smooth_ok else "SMOOTHNESS FAIL"))
        print("      polys %3d  wall-contact frames RE %2d retail %2d  "
              "CRASH  RE %d retail %d%s%s"
              % (len(tris), m_wallf, e_wallf, m_fire, e_fire,
                 ("" if opts['crash'] is None
                  else "   (expected %d)" % opts['crash']) + known_note,
                 "   [LIVE chassis filter ny_max %.2f: %d/%d polys]"
                 % (opts['ny_max'], len(live_chassis(tris, opts['ny_max'])),
                    len(tris))
                 if opts['chassis'] == 'live' else ""))
        if bad:
            print("      discrete divergences: %d, first: %s"
                  % (len(bad), bad[:4]))
        if args.verbose:
            print("      frame  RE cur[0..3]                     "
                  "retail cur[0..3]                  RE roll/pitch  "
                  "retail roll/pitch")
            for i, (e, m) in enumerate(zip(emu, port)):
                mr, mp = _roll_pitch(m)
                er, epi = _roll_pitch(e)
                print("      %3d  %s  %s  %7.3f %7.3f  %7.3f %7.3f"
                      % (i,
                         ' '.join('%6.3f' % m['wheels'][k]['cur']
                                  for k in range(4)),
                         ' '.join('%6.3f' % e['wheels'][k]['cur']
                                  for k in range(4)),
                         mr - p0r, mp - p0p, er - e0r, epi - e0p))
    print("\n%s (%d failing case%s)"
          % ("PASS" if not fails else "FAIL", fails, "" if fails == 1 else "s"))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
