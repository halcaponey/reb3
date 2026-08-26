#!/usr/bin/env python3
"""
Differential acceptance suite for src/burnout3_carcol.c.

Every case seeds two vehicles, runs the REAL x86 under Unicorn
(tools/emulate_carcol.py) and the compiled C port from identical state, and
asserts the two agree field for field.

  narrow phase      FUN_0010A9D0 -> FUN_0010AC20   contact point / normal /
                                                   per-body separation
  broad phase       FUN_00114270                   world AABB of the box
  impulse           FUN_0010F8D0                   two-body contact impulse
  force routing     FUN_001205E0                   +0xF0 vs +0xF0/+0x100
  racer vs racer    FUN_001121F0                   full response + slam class
  car vs wreck      FUN_00113960                   response + crash threshold

The last section ("consume") is not differential: it is the drive-through
regression guard.  It asserts the invariant the two above leave implicit --
that a resolved contact reaches a body somebody actually INTEGRATES.  On the
wreck path FUN_00113960 deliberately gives the alive car nothing (kind 2,
@0x00113B75), so the wreck is the only body holding the response; if the
harness does not integrate it, the contact is silently discarded and the car
is driven through while still being drawn.

Usage: python3 tools/validate_carcol.py [section]
"""
import math
import os

# The driver links burnout3_backend.c; pin it to the RE path so this
# differential test is unaffected by whatever build/backends.cfg says.
os.environ['B3_BACKENDS'] = '/dev/null'

import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import importlib.util
_spec = importlib.util.spec_from_file_location(
    "ec", os.path.join(HERE, "emulate_carcol.py"))
ec = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ec)

# ---------------------------------------------------------------------------
# the C driver -- compiled against src/burnout3_carcol.c
# ---------------------------------------------------------------------------
DRIVER = r'''
/* Differential driver for burnout3_carcol.c (built by validate_carcol.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "burnout3_carcol.h"

/* burnout3_vehicle_sim.c is linked in for b3_rigid_body_integrate; its wheel
 * ray never runs here, so the world probe is a stub. */
int b3_ground_probe(float x, float y, float z, float* h, float n[3]) {
    (void)x; (void)y; (void)z; (void)h; (void)n; return -1;
}

static B3RigidBody rbs[2];
/* The 4x4 is not inline in B3RigidBody any more (retail keeps it in its
 * own object behind the pointer at +0x204), so the driver must give each
 * body storage before a row is written. */
static float rbs__frame_store[2][4][4];
static B3CarBody   bodies[2];
static B3CarHull   hulls[2];

static float rf(void) { double d; if (scanf("%lf", &d) != 1) exit(2); return (float)d; }
static int   ri(void) { int i; if (scanf("%d", &i) != 1) exit(2); return i; }

static void pv(const char* k, const float v[4]) {
    printf("%s %.9g %.9g %.9g %.9g\n", k, v[0], v[1], v[2], v[3]);
}
static void pf(const char* k, float v) { printf("%s %.9g\n", k, v); }

static void read_body(int i) {
    char path[512];
    B3CarBody* b = &bodies[i];
    memset(b, 0, sizeof(*b));
    memset(&rbs[i], 0, sizeof(rbs[i]));
    b3_rigid_body_bind_frame(&rbs[i], rbs__frame_store[i]);  /* AFTER the memset */
    b->rb = &rbs[i];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) rbs[i].frame[r][c] = rf();
    for (int c = 0; c < 4; c++) b->bbmax[c] = rf();
    for (int c = 0; c < 4; c++) b->bbmin[c] = rf();
    b->mass = rf();
    for (int c = 0; c < 3; c++) rbs[i].vel[c] = rf();
    rbs[i].vel[3] = sqrtf(rbs[i].vel[0]*rbs[i].vel[0] + rbs[i].vel[1]*rbs[i].vel[1]
                        + rbs[i].vel[2]*rbs[i].vel[2]);
    for (int c = 0; c < 3; c++) rbs[i].omega[c] = rf();
    rbs[i].omega[3] = 0.0f;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        rbs[i].inv_inertia_world[r][c] = rf();
    b->type        = (unsigned char)ri();
    b->crashed     = (unsigned char)ri();
    b->grounded    = (unsigned char)ri();
    b->asleep      = (unsigned char)ri();
    b->drift_state = ri();
    b->yaw_input   = rf();
    /* the type-3 arm's inputs (FUN_00112E70); every other mode leaves them
     * at the values body_payload's defaults supply. */
    b->speed       = rf();
    b->authority   = rf();
    b->flags_1353  = (unsigned char)ri();
    b->immune      = (unsigned char)ri();
    b->crash_mode  = (unsigned char)ri();
    b->no_crash    = (unsigned char)ri();
    b->designated  = (unsigned char)ri();
    if (scanf("%511s", path) != 1) exit(2);
    if (!b3_carcol_hull_load(path, &hulls[i])) { fprintf(stderr, "hull %s\n", path); exit(3); }
    b->hull = &hulls[i];
    /* inverse frame, same construction as the integrator */
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        rbs[i].inv_frame[r][c] = rbs[i].frame[r][c];
    {
        float t, (*m)[4] = rbs[i].inv_frame, p[4];
        t = m[0][1]; m[0][1] = m[1][0]; m[1][0] = t;
        t = m[0][2]; m[0][2] = m[2][0]; m[2][0] = t;
        t = m[1][2]; m[1][2] = m[2][1]; m[2][1] = t;
        for (int c = 0; c < 4; c++)
            p[c] = m[3][0]*m[0][c] + m[3][1]*m[1][c] + m[3][2]*m[2][c];
        for (int c = 0; c < 4; c++) m[3][c] = -p[c];
    }
}

/* The contact boundary hands its result to the INTEGRATOR, and a contact
 * nobody integrates is a contact nobody feels.  `parked` is the harness's
 * parked-wreck model (carcol_pass): no drive servo, gravity cancelled, the
 * body height pinned -- a wreck may be shoved along the road, never through
 * it. */
static void integrate_one(int i, float dt, int parked) {
    float keep_y = rbs[i].frame[3][1];
    if (parked) rbs[i].force_acc[1] += 20.0f * bodies[i].mass;
    b3_rigid_body_integrate(&rbs[i], bodies[i].mass, 0.0f, 0, 0, dt);
    if (parked) rbs[i].frame[3][1] = keep_y;
}

static void dump_body(const char* tag, int i) {
    char k[64];
    snprintf(k, sizeof k, "%s.force",      tag); pv(k, rbs[i].force_acc);
    snprintf(k, sizeof k, "%s.torque",     tag); pv(k, rbs[i].torque_acc);
    snprintf(k, sizeof k, "%s.imp_force",  tag); pv(k, rbs[i].imp_force);
    snprintf(k, sizeof k, "%s.imp_torque", tag); pv(k, rbs[i].imp_torque);
    snprintf(k, sizeof k, "%s.deflection", tag); pv(k, rbs[i].deflection);
    snprintf(k, sizeof k, "%s.contact_pt", tag); pv(k, bodies[i].contact_pt);
    printf("%s.touched %d\n", tag, bodies[i].touched);
    printf("%s.hit_side %d\n", tag, bodies[i].hit_side);
}

int main(void) {
    int mode = ri();
    B3CarContact ct;
    if (mode == 3) {                       /* FUN_0010F8D0 in isolation */
        float pt3[4], pt1[4], vrel[4], n[4], out[4], e, m1, m3;
        read_body(0); read_body(1);
        for (int c = 0; c < 4; c++) pt3[c] = rf();
        for (int c = 0; c < 4; c++) pt1[c] = rf();
        for (int c = 0; c < 4; c++) vrel[c] = rf();
        for (int c = 0; c < 4; c++) n[c] = rf();
        e = rf();
        m1 = bodies[1].mass; m3 = bodies[0].mass;
        float j = b3_carcol_mutual_impulse(bodies[1].rb, m1, bodies[0].rb, m3,
                                           pt3, pt1, vrel, n, e, out);
        pv("imp", out); pf("j", j);
        return 0;
    }
    if (mode == 4) {                       /* FUN_001205E0 */
        float f[4], p[4];
        read_body(0); read_body(1);
        for (int c = 0; c < 4; c++) f[c] = rf();
        for (int c = 0; c < 4; c++) p[c] = rf();
        b3_carcol_apply_force(bodies[0].rb, bodies[0].drift_state, f, p);
        dump_body("a", 0);
        return 0;
    }
    if (mode == 8) {                       /* FUN_0010FCE0, no bodies */
        float a0[2], a1[2], b0[2], b1[2], pa[2], pb[2], ta, tb, o[4];
        for (int c = 0; c < 2; c++) a0[c] = rf();
        for (int c = 0; c < 2; c++) a1[c] = rf();
        for (int c = 0; c < 2; c++) b0[c] = rf();
        for (int c = 0; c < 2; c++) b1[c] = rf();
        float d = b3_carcol_seg_closest2d(a0, a1, b0, b1, pa, pb, &ta, &tb);
        pf("dist", d);
        o[0] = pa[0]; o[1] = pa[1]; o[2] = 0; o[3] = 0; pv("pa", o);
        o[0] = pb[0]; o[1] = pb[1];           pv("pb", o);
        pf("ta", ta); pf("tb", tb);
        return 0;
    }
    read_body(0); read_body(1);
    if (mode == 5) {                       /* FUN_00114270 world AABB */
        float lo[3], hi[3], l4[4], h4[4];
        b3_carcol_world_aabb(&bodies[0], lo, hi);
        for (int c = 0; c < 3; c++) { l4[c] = lo[c]; h4[c] = hi[c]; }
        l4[3] = h4[3] = 0.0f;
        pv("lo", l4); pv("hi", h4);
        return 0;
    }
    if (mode == 6) {                       /* resolve + ONE integrator frame */
        float dt = rf();
        float p0[2][3], v0[2][3];
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 3; k++) {
                p0[i][k] = rbs[i].frame[3][k];
                v0[i][k] = rbs[i].vel[k];
            }
        int hit = b3_carcol_resolve(&bodies[0], &bodies[1], &ct);
        printf("hit %d\n", hit);
        pf("impact", ct.impact);
        pv("a.imp_force", rbs[0].imp_force);
        pv("a.force", rbs[0].force_acc);
        pv("a.deflection", rbs[0].deflection);
        pv("b.imp_force", rbs[1].imp_force);
        pv("b.force", rbs[1].force_acc);
        pv("b.deflection", rbs[1].deflection);
        for (int i = 0; i < 2; i++)
            integrate_one(i, dt, bodies[i].crashed);
        for (int i = 0; i < 2; i++) {
            char k[32]; float d[4];
            for (int c = 0; c < 3; c++) d[c] = rbs[i].frame[3][c] - p0[i][c];
            d[3] = 0.0f;
            snprintf(k, sizeof k, "%c.dpos", 'a' + i); pv(k, d);
            for (int c = 0; c < 3; c++) d[c] = rbs[i].vel[c] - v0[i][c];
            d[3] = 0.0f;
            snprintf(k, sizeof k, "%c.dvel", 'a' + i); pv(k, d);
        }
        return 0;
    }
    if (mode == 9) {
        /* Drive the SAME pair for N frames, rebuilding the traffic car's
         * pose from its lane cursor every frame exactly as traffic_update()
         * does, and report the frame on which the contact finally clears.
         * arm 0 = retail's type-3 arm; arm 1 = the OLD routing (the
         * racer-vs-racer response, whose mass split hands the traffic body
         * a share that the lane rebuild then throws away). */
        int party = ri();
        float dt = rf();
        int nframes = ri();
        int arm = ri();
        B3CarContact c9;
        float bframe[4][4], bvel[4], p0[3];
        int cleared = -1, f;
        memcpy(bframe, rbs[1].frame, sizeof bframe);
        memcpy(bvel, rbs[1].vel, sizeof bvel);
        for (int k = 0; k < 3; k++) p0[k] = rbs[0].frame[3][k];
        if (arm) bodies[1].type = B3_COL_TYPE_TRAFFIC;
        for (f = 0; f < nframes; f++) {
            int hit;
            for (int i = 0; i < 2; i++) {
                memset(rbs[i].force_acc,  0, sizeof rbs[i].force_acc);
                memset(rbs[i].torque_acc, 0, sizeof rbs[i].torque_acc);
                memset(rbs[i].imp_force,  0, sizeof rbs[i].imp_force);
                memset(rbs[i].imp_torque, 0, sizeof rbs[i].imp_torque);
                memset(rbs[i].deflection, 0, sizeof rbs[i].deflection);
            }
            memcpy(rbs[1].frame, bframe, sizeof bframe);
            memcpy(rbs[1].vel, bvel, sizeof bvel);
            memset(&c9, 0, sizeof c9);
            hit = arm ? b3_carcol_resolve(&bodies[0], &bodies[1], &c9)
                      : b3_carcol_resolve_traffic(&bodies[0], &bodies[1],
                                                  party, &c9);
            if (f == 0) {
                printf("hit %d\n", hit);
                printf("push %d\n", c9.push);
                pf("pen", c9.pen);
                pv("a.deflection", rbs[0].deflection);
                pv("b.deflection", rbs[1].deflection);
            }
            if (!hit) { cleared = f; break; }
            integrate_one(0, dt, 0);
        }
        printf("cleared %d\n", cleared);
        {   float d[4];
            for (int k = 0; k < 3; k++) d[k] = rbs[0].frame[3][k] - p0[k];
            d[3] = 0.0f; pv("a.dpos", d); }
        return 0;
    }
    if (mode == 7) {                       /* FUN_00112E70, live traffic */
        int party = ri();
        int hit = b3_carcol_resolve_traffic(&bodies[0], &bodies[1], party, &ct);
        printf("hit %d\n", hit);
        printf("push %d\n", ct.push);
        printf("promote %d\n", ct.promote);
        printf("crash_a %d\n", ct.crash_a);
        pf("impact", ct.impact);
        pf("vn_mph", ct.vn_mph);
        pf("pen", ct.pen);
        pf("metric_mph", ct.metric_mph);
        pf("thresh_mph", ct.thresh_mph);
        pv("point", ct.point); pv("normal", ct.normal);
        dump_body("a", 0); dump_body("b", 1);
        return 0;
    }
    if (mode == 0) {
        int hit = b3_carcol_contact(&bodies[0], &bodies[1], &ct);
        printf("hit %d\n", hit);
        if (hit) { pv("point", ct.point); pv("normal", ct.normal);
                   pv("pen_a", ct.pen_a); pv("pen_b", ct.pen_b); }
        return 0;
    }
    int hit = (mode == 1) ? b3_carcol_resolve_alive(&bodies[0], &bodies[1], &ct)
                          : b3_carcol_resolve_wreck(&bodies[0], &bodies[1], &ct);
    printf("hit %d\n", hit);
    if (hit) {
        pv("point", ct.point); pv("normal", ct.normal);
        pf("impact", ct.impact);
        pf("vn_mph", ct.vn_mph);
        printf("slam %d\n", ct.slam_class);
        printf("crash_a %d\n", ct.crash_a);
        printf("crash_b %d\n", ct.crash_b);
        printf("event %d\n", ct.event);
        printf("attacker_is_b %d\n", ct.attacker_is_b);
        pf("strength", ct.strength);
        dump_body("a", 0); dump_body("b", 1);
    }
    return 0;
}
'''

# ---------------------------------------------------------------------------
BUILD = None


def build_driver():
    global BUILD
    d = tempfile.mkdtemp(prefix="carcol_")
    src = os.path.join(d, "carcol_drv.c")
    open(src, "w").write(DRIVER)
    exe = os.path.join(d, "carcol_drv")
    cmd = ["gcc", "-std=c11", "-O2", "-I", os.path.join(ROOT, "src"),
           "-o", exe, src, os.path.join(ROOT, "src", "burnout3_carcol.c"),
           # burnout3_carcol.c consults build/backends.cfg (and the emulation
           # bridge when carcol=retail), so the driver needs both objects.
           # The driver never sets carcol=retail, so this only satisfies the
           # linker -- the RE path is what is under test here.
           os.path.join(ROOT, "src", "burnout3_backend.c"),
           os.path.join(ROOT, "src", "burnout3_emu.c"),
           # b3_rigid_body_integrate, for the "somebody must actually be
           # moved by this contact" section (mode 6).
           os.path.join(ROOT, "src", "burnout3_vehicle_sim.c"),
           "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr)
        raise SystemExit("driver build failed")
    BUILD = exe
    return exe


def run_driver(payload):
    r = subprocess.run([BUILD], input=payload, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("driver failed (%d): %s" % (r.returncode, r.stderr))
    out = {}
    for line in r.stdout.splitlines():
        p = line.split()
        if not p:
            continue
        out[p[0]] = [float(x) for x in p[1:]] if len(p) > 2 else float(p[1])
    return out


def body_payload(st, hullpath):
    v = []
    for row in st['frame']:
        v += list(row)
    v += list(st['bbmax']) + list(st['bbmin']) + [st['mass']]
    v += list(st.get('vel', [0, 0, 0]))
    v += list(st.get('omega', [0, 0, 0]))
    ii = st.get('inv_inertia', DEFAULT_II)
    for row in ii:
        v += list(row)
    s = " ".join("%.9g" % x for x in v)
    s += " %d %d %d %d %d %.9g" % (
        st.get('type', 0), st.get('crashed', 0), st.get('grounded', 0),
        st.get('asleep', 0), st.get('drift', 0), st.get('yaw_input', 0.0))
    s += " %.9g %.9g %d %d %d %d %d %s" % (
        st.get('speed', 0.0), st.get('authority', 1.0),
        st.get('flags_1353', 0), st.get('immune', 0), st.get('crash_mode', 0),
        st.get('no_crash', 0), st.get('designated', 0), hullpath)
    return s


DEFAULT_II = [[1.0 / 900, 0, 0, 0], [0, 1.0 / 1800, 0, 0], [0, 0, 1.0 / 1600, 0]]

PASS = 0
FAIL = 0
SECTION = None


def chk(name, got, want, tol=1e-3, rel=1e-3):
    global PASS, FAIL
    ok = True
    if isinstance(want, (list, tuple)):
        if not isinstance(got, (list, tuple)) or len(got) < len(want):
            ok = False
        else:
            for a, b in zip(got, want):
                if abs(a - b) > tol + rel * max(abs(a), abs(b)):
                    ok = False
    else:
        a, b = float(got), float(want)
        if abs(a - b) > tol + rel * max(abs(a), abs(b)):
            ok = False
    if ok:
        PASS += 1
    else:
        FAIL += 1
        print("  FAIL %-28s got=%s want=%s" % (name, got, want))
    return ok


def chk_eq(name, got, want):
    """Exact equality -- for image bytes and integer verdicts."""
    global PASS, FAIL
    if got == want:
        PASS += 1
        return True
    FAIL += 1
    print("  FAIL %-28s got=%s want=%s" % (name, got, want))
    return False


def hull_path(cls, car):
    return os.path.join(ROOT, "build", "cars", "%s_%s.hull" % (cls, car))


def car(cls, car_id, **kw):
    bmax, bmin = ec.bbox(cls, car_id)
    st = dict(hull=ec.load_hull(cls, car_id), bbmax=bmax, bbmin=bmin,
              _cls=cls, _car=car_id)
    st.update(kw)
    return st


# ---------------------------------------------------------------------------
CASES_NARROW = [
    # name, A, B
    ("side-by-side",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 20]),
     dict(pos=(1.4, 0, 0.5), yaw=0.0, mass=900.0, vel=[0, 0, 18])),
    ("nose-to-tail",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 30]),
     dict(pos=(0.05, 0, 3.6), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
    ("angled-side",
     dict(pos=(0, 0, 0), yaw=0.20, mass=800.0, vel=[3, 0, 25]),
     dict(pos=(1.7, 0, 1.1), yaw=-0.15, mass=1100.0, vel=[-2, 0, 22])),
    ("t-bone",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 26]),
     dict(pos=(1.9, 0.05, 1.2), yaw=1.4, mass=1000.0, vel=[-9, 0, 4])),
    ("deep-overlap",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 10]),
     dict(pos=(0.9, 0.02, 0.3), yaw=0.05, mass=900.0, vel=[0, 0, 12])),
    ("rear-into-nose",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 5]),
     dict(pos=(-0.02, 0, -3.7), yaw=0.0, mass=1400.0, vel=[0, 0, 30])),
    ("pitched",
     dict(pos=(0, 0.15, 0), yaw=0.1, pitch=0.12, mass=800.0, vel=[1, 0, 24]),
     dict(pos=(1.6, 0, 0.9), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
    ("no-contact",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 20]),
     dict(pos=(6.0, 0, 0), yaw=0.0, mass=900.0, vel=[0, 0, 20])),
]


def make_state(spec, cls="COMP", cid="Car1"):
    st = car(cls, cid)
    st['frame'] = ec.frame_from(spec.get('yaw', 0.0), spec['pos'],
                                spec.get('pitch', 0.0))
    for k in ('mass', 'vel', 'omega', 'type', 'crashed', 'grounded',
              'asleep', 'drift', 'yaw_input', 'inv_inertia'):
        if k in spec:
            st[k] = spec[k]
    st.setdefault('mass', 800.0)
    return st


def section(name):
    global SECTION
    SECTION = name
    print("\n== %s" % name)


# ---------------------------------------------------------------------------
def run_narrow():
    section("narrow phase (FUN_0010A9D0 -> FUN_0010AC20)")
    for name, sa, sb in CASES_NARROW:
        A = make_state(sa)
        B = make_state(sb, "SUPR", "Car1") if "t-bone" in name else make_state(sb)
        s = ec.Session()
        s.seed(0, A); s.seed(1, B)
        g = s.narrow(0)
        payload = "0\n%s\n%s\n" % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                                   body_payload(B, hull_path(B['_cls'], B['_car'])))
        c = run_driver(payload)
        print(" case %s  (game hit=%d valid=%d)" % (name, g['hit'], g['valid']))
        chk(name + ".hit", c['hit'], 1 if g['valid'] else 0, tol=0)
        if not g['valid']:
            continue
        chk(name + ".point", c['point'], g['point'], tol=1e-4)
        chk(name + ".normal", c['normal'], g['normal'], tol=1e-4)
        dA = [g['posA'][i] - A['frame'][3][i] for i in range(4)]
        dB = [g['posB'][i] - B['frame'][3][i] for i in range(4)]
        chk(name + ".pen_a", c['pen_a'], dA, tol=1e-4)
        chk(name + ".pen_b", c['pen_b'], dB, tol=1e-4)


def run_aabb():
    section("broad phase world AABB (FUN_00114270)")
    specs = [
        ("axis-aligned", dict(pos=(10, 1, -4), yaw=0.0)),
        ("yawed", dict(pos=(-3, 0.5, 7), yaw=0.9)),
        ("yaw+pitch", dict(pos=(2, 2, 2), yaw=-1.3, pitch=0.25)),
    ]
    for name, sp in specs:
        A = make_state(sp)
        B = make_state(dict(pos=(50, 0, 50), yaw=0.0))
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.aabb(0)
        payload = "5\n%s\n%s\n" % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                                   body_payload(B, hull_path(B['_cls'], B['_car'])))
        c = run_driver(payload)
        chk(name + ".lo", c['lo'][:3], g['lo'][:3], tol=1e-4)
        chk(name + ".hi", c['hi'][:3], g['hi'][:3], tol=1e-4)


def run_impulse():
    section("two-body impulse (FUN_0010F8D0)")
    cases = [
        ("head-on", [0.5, 0.4, 1.0, 1], [0.5, 0.4, 1.0, 1],
         [-12.0, 0.0, -3.0, 0], [1.0, 0.0, 0.0, 0], 0.1),
        ("offset-lever", [1.0, 0.2, -1.6, 1], [1.0, 0.2, -1.6, 1],
         [4.0, -1.0, 9.0, 0], [0.0, 0.0, 1.0, 0], 0.1),
        ("e0", [0.2, 0.6, 0.3, 1], [0.2, 0.6, 0.3, 1],
         [7.0, 2.0, -1.0, 0], [0.6, 0.0, 0.8, 0], 0.0),
        ("diag-normal", [-0.9, 0.1, 2.0, 1], [-0.9, 0.1, 2.0, 1],
         [-3.0, 0.5, 6.0, 0], [0.57735, 0.57735, 0.57735, 0], 0.35),
    ]
    A = make_state(dict(pos=(0, 0, 0), yaw=0.3, mass=800.0,
                        omega=[0.1, 0.8, -0.2]))
    B = make_state(dict(pos=(1.8, 0, 0.6), yaw=-0.2, mass=1300.0,
                        omega=[-0.3, 1.2, 0.05]))
    s = ec.Session(); s.seed(0, A); s.seed(1, B)
    for name, pt3, pt1, vrel, n, e in cases:
        g = s.impulse(pt3, pt1, vrel, n, e)
        payload = ("3\n%s\n%s\n%s %s %s %s %.9g\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(B, hull_path(B['_cls'], B['_car'])),
                      " ".join("%.9g" % x for x in pt3),
                      " ".join("%.9g" % x for x in pt1),
                      " ".join("%.9g" % x for x in vrel),
                      " ".join("%.9g" % x for x in n), e))
        c = run_driver(payload)
        chk("impulse." + name, c['imp'], g, tol=1e-3, rel=1e-5)


def run_force():
    section("force routing (FUN_001205E0)")
    cases = [
        ("drift-linear", dict(drift=1, omega=[0, 0.5, 0])),
        ("slow-yaw-at-point", dict(drift=0, omega=[0, 0.5, 0])),
        ("fast-yaw-same-sign", dict(drift=0, omega=[0, 6.0, 0])),
        ("fast-yaw-opposite", dict(drift=0, omega=[0, -6.0, 0])),
    ]
    force = [1500.0, 0.0, -400.0, 0.0]
    point = [1.1, 0.3, 1.9, 1.0]
    for name, extra in cases:
        A = make_state(dict(pos=(0, 0, 0), yaw=0.25, mass=800.0, **extra))
        B = make_state(dict(pos=(40, 0, 0), yaw=0.0, mass=900.0))
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.apply_force(0, force, point)
        payload = ("4\n%s\n%s\n%s\n%s\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(B, hull_path(B['_cls'], B['_car'])),
                      " ".join("%.9g" % x for x in force),
                      " ".join("%.9g" % x for x in point)))
        c = run_driver(payload)
        chk(name + ".force", c['a.force'], g['force'], tol=1e-3, rel=1e-6)
        chk(name + ".torque", c['a.torque'], g['torque'], tol=1e-2, rel=1e-6)


RESPONSE_CASES = [
    # name, A spec, B spec, B car
    # --- head-on into ONCOMING traffic: the crash-parity boundary -------
    # Retail gates an alive-vs-alive car crash on
    #   |dot(vrel, n)| * 2.236936 ([0x0038994c]) > 150.0 ([0x003ebe4c])
    # (COMISS/JBE @0x0011281e/@0x0011283e in FUN_001121F0).  Nose-to-nose,
    # vn IS the closing speed, so these three pin the threshold from both
    # sides -- and the type-2 case proves retail gives a TRAFFIC body no
    # lower bar than a racer, which is why hitting oncoming traffic below
    # ~160 mph closing legitimately does not crash.
    # --- the TRAFFIC wreck threshold, 2500 vs 5000 ---------------------
    # FUN_00113960 wrecks the racer on `impact > 5000` ([0x003EBE50]),
    # dropping to 2500 ([0x003EBE54]) when an UN-crashed non-traffic car
    # hits a CRASHED traffic car.  These two sit in the discriminating
    # band (impact ~3223): identical geometry, and only the B.type=2 case
    # may crash the racer.  This is retail's cheap cascade -- once a
    # traffic car is wrecked, 15 mph of closing is enough to wreck you.
    ("wreck-vs-crashed-traffic",   # B = TRAFFIC, crashed -> thresh 2500
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  6.71]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -3.13],
          type=2, crashed=1), None),
    ("wreck-vs-crashed-racer",     # B = RACER, crashed   -> thresh 5000
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  6.71]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -3.13],
          type=0, crashed=1), None),
    ("headon-under-150",           # 120 + 30 = 150 mph closing -> NO crash
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  53.64]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41]), None),
    ("headon-over-150",            # 130 + 30 = 160 mph closing -> crash
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  58.11]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41]), None),
    ("headon-traffic-type2",       # same, B typed as TRAFFIC -> same bar
     dict(pos=(0, 0, 0),   yaw=0.0,       mass=900.0,  vel=[0, 0,  58.11]),
     dict(pos=(0, 0, 3.9), yaw=3.14159265, mass=1200.0, vel=[0, 0, -13.41],
          type=2), None),
    ("lateral-rub",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0.6, 0, 24.0]),
     dict(pos=(1.45, 0, 0.4), yaw=0.0, mass=900.0, vel=[-0.6, 0, 23.0]), None),
    ("hard-side-swipe",
     dict(pos=(0, 0, 0), yaw=0.12, mass=800.0, vel=[8.0, 0, 30.0],
          omega=[0, 0.4, 0], yaw_input=0.3),
     dict(pos=(1.6, 0, 0.6), yaw=-0.05, mass=1200.0, vel=[-4.0, 0, 26.0],
          omega=[0, -0.2, 0], yaw_input=-0.1), None),
    ("nose-into-tail-hi",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 42.0]),
     dict(pos=(0.0, 0, 4.02), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]), None),
    ("nose-into-tail-lo",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 24.0]),
     dict(pos=(0.0, 0, 4.02), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]), None),
    # The rear-end branch clamps BOTH longitudinal parameters: the attacker's
    # contact must sit at/past its own bbmax.z and the victim's at/behind its
    # bbmin.z.  COMP/Car4's hull nose overhangs its box by 0.197 and
    # SUPR/Car10's tail by 0.373, so that pair reaches it.
    ("rear-end-slam",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 46.0],
          _car=("COMP", "Car4")),
     dict(pos=(0.0, 0, 4.35), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]),
     ("SUPR", "Car10")),
    ("rear-end-light",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 33.0],
          _car=("COMP", "Car4")),
     dict(pos=(0.0, 0, 4.35), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0]),
     ("SUPR", "Car10")),
    ("rear-ended-by-b",
     dict(pos=(0, 0, 0), yaw=0.0, mass=1100.0, vel=[0, 0, 20.0],
          _car=("SUPR", "Car10")),
     dict(pos=(0.0, 0, -4.35), yaw=0.0, mass=800.0, vel=[0, 0, 46.0],
          _car=("COMP", "Car4")), None),
    ("t-bone",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[0, 0, 34.0]),
     dict(pos=(1.85, 0.02, 1.4), yaw=1.35, mass=1300.0, vel=[-14.0, 0, 3.0]),
     ("SUPR", "Car1")),
    ("grounded-a",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[2.0, 0, 20.0], grounded=1),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-1.0, 0, 20.0]), None),
    ("grounded-b",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[2.0, 0, 20.0]),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-1.0, 0, 20.0],
          grounded=1), None),
    ("drifting-a",
     dict(pos=(0, 0, 0), yaw=0.1, mass=800.0, vel=[5.0, 0, 28.0], drift=1,
          omega=[0, 1.5, 0]),
     dict(pos=(1.7, 0, 0.5), yaw=0.0, mass=1000.0, vel=[0, 0, 25.0]), None),
    ("heavy-vs-light",
     dict(pos=(0, 0, 0), yaw=0.0, mass=600.0, vel=[6.0, 0, 30.0]),
     dict(pos=(1.55, 0, 0.3), yaw=0.0, mass=2200.0, vel=[-2.0, 0, 28.0]), None),
    ("crash-speed",
     dict(pos=(0, 0, 0), yaw=0.0, mass=800.0, vel=[45.0, 0, 20.0]),
     dict(pos=(1.5, 0, 0.2), yaw=0.0, mass=900.0, vel=[-45.0, 0, 20.0]), None),
]


def run_response(mode, label, mkB=None):
    section(label)
    for name, sa, sb, other in RESPONSE_CASES:
        A = make_state(sa, *(sa.get('_car') or ("COMP", "Car1")))
        B = make_state(sb, *(sb.get('_car') or other or ("COMP", "Car1")))
        if mkB:
            mkB(A, B)
        s = ec.Session(); s.seed(0, A); s.seed(1, B)
        g = s.resolve_alive() if mode == 1 else s.resolve_wreck()
        payload = ("%d\n%s\n%s\n" % (mode,
                   body_payload(A, hull_path(A['_cls'], A['_car'])),
                   body_payload(B, hull_path(B['_cls'], B['_car']))))
        c = run_driver(payload)
        chk(name + ".hit", c['hit'], g['hit'], tol=0)
        if not g['hit'] or not c['hit']:
            print("  %-18s no contact" % name)
            continue
        print("  %-18s vn=%6.1f mph impact=%9.1f slam=%d crash=%d%d events=%s"
              % (name, c['vn_mph'], c['impact'], int(c['slam']),
                 int(g['crash_a']), int(g['crash_b']),
                 [(t, 'B' if a == ec.VEH_B else 'A', round(st, 3))
                  for t, a, v, st in g['slams']]))
        chk(name + ".point", c['point'], g['point'], tol=1e-4)
        chk(name + ".normal", c['normal'], g['normal'], tol=1e-4)
        chk(name + ".impact", c['impact'], g['impact'], tol=1e-2, rel=1e-5)
        chk(name + ".slam", c['slam'], g['slam'], tol=0)
        chk(name + ".crash_a", c['crash_a'], g['crash_a'], tol=0)
        chk(name + ".crash_b", c['crash_b'], g['crash_b'], tol=0)
        sl = g['slams'][-1] if g['slams'] else (0, 0, 0, 0.0)
        chk(name + ".event", c['event'], sl[0], tol=0)
        chk(name + ".attacker_is_b", c['attacker_is_b'],
            1 if sl[1] == ec.VEH_B else 0, tol=0)
        chk(name + ".strength", c['strength'], sl[3], tol=1e-5, rel=1e-5)
        for tag, gd in (("a", g['a']), ("b", g['b'])):
            chk("%s.%s.deflection" % (name, tag), c[tag + '.deflection'],
                gd['deflection'], tol=1e-4)
            chk("%s.%s.imp_force" % (name, tag), c[tag + '.imp_force'],
                gd['imp_force'], tol=1e-2, rel=1e-5)
            chk("%s.%s.imp_torque" % (name, tag), c[tag + '.imp_torque'],
                gd['imp_torque'], tol=1e-2, rel=1e-5)
            chk("%s.%s.force" % (name, tag), c[tag + '.force'],
                gd['force'], tol=1e-2, rel=1e-5)
            chk("%s.%s.torque" % (name, tag), c[tag + '.torque'],
                gd['torque'], tol=1e-1, rel=1e-5)
            chk("%s.%s.contact_pt" % (name, tag), c[tag + '.contact_pt'],
                gd['contact_pt'], tol=1e-4)
            chk("%s.%s.touched" % (name, tag), c[tag + '.touched'],
                gd['touched'], tol=0)
            chk("%s.%s.hit_side" % (name, tag), c[tag + '.hit_side'],
                gd['hit_side'], tol=0)


def wreck_setup(A, B):
    B['crashed'] = 1


# ---------------------------------------------------------------------------
# THE DRIVE-THROUGH REGRESSION.
#
# "resolved" is not "felt".  FUN_00113960 (b3_carcol_resolve_wreck) forces the
# UN-crashed car to kind 2 -- IMMOVABLE -- at 0x00113B75, so a racer that hits
# a WRECK gets nothing at all: 100% of the contact impulse (+0x110) and 100%
# of the separation (+0x130) go to the wreck's body.  That is retail, and the
# `wreck` section above already diffs it field for field.
#
# What it leaves implicit is the CONSUMER.  In retail a crashed car keeps
# running its own solver, so its integrator drains +0x110/+0x130 and the wreck
# is shoved aside.  carcol_pass() used to skip the integrate for any traffic
# car with `crashed_until > g_race_time`, and traffic_update() skips a parked
# wreck entirely -- so on the wreck path NOTHING in the pair was ever
# integrated.  The racer got zero, the wreck never moved, and the player drove
# clean through a car that was still being drawn.
#
# These cases pin the whole chain: what each body RECEIVES from the solve, and
# what one frame of the real integrator then DOES with it.  Any arrangement in
# which a contact leaves both bodies unmoved fails here.
# ---------------------------------------------------------------------------
CONSUME_CASES = [
    # name, racer spec, traffic spec, traffic model, who must move
    ("racer-into-alive-traffic",
     dict(pos=(0, 0, 0),    yaw=0.0, mass=1200.0, vel=[0, 0, 45.0]),
     dict(pos=(0, 0, 4.35), yaw=0.0, mass=2000.0, vel=[0, 0, 20.0], type=2),
     ("HEVY", "Car11"), "both"),
    ("racer-into-wrecked-traffic",
     dict(pos=(0, 0, 0),    yaw=0.0, mass=1200.0, vel=[0, 0, 45.0]),
     dict(pos=(0, 0, 4.35), yaw=0.0, mass=2000.0, vel=[0, 0, 0.0],
          type=2, crashed=1),
     ("HEVY", "Car11"), "wreck-only"),
    ("racer-into-wrecked-traffic-slow",   # below the 2500 crash threshold
     dict(pos=(0, 0, 0),    yaw=0.0, mass=1200.0, vel=[0, 0, 12.0]),
     dict(pos=(0, 0, 4.35), yaw=0.0, mass=2000.0, vel=[0, 0, 0.0],
          type=2, crashed=1),
     ("HEVY", "Car11"), "wreck-only"),
]


def _len3(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def chk_gt(name, got, floor):
    """got must be strictly greater than floor."""
    global PASS, FAIL
    if got > floor:
        PASS += 1
        return True
    FAIL += 1
    print("  FAIL %-40s got=%.6g want > %.6g" % (name, got, floor))
    return False


def run_consume():
    section("a resolved contact must MOVE somebody "
            "(carcol_pass drive-through regression)")
    dt = 1.0 / 60.0
    for name, sa, sb, tcar, who in CONSUME_CASES:
        A = make_state(sa, "COMP", "Car1")
        B = make_state(sb, *tcar)
        payload = ("6\n%s\n%s\n%.9g\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(B, hull_path(B['_cls'], B['_car'])), dt))
        c = run_driver(payload)
        chk_eq(name + ".hit", int(c['hit']), 1)
        if not c['hit']:
            continue
        # what each body RECEIVED (a velocity-equivalent, m/s)
        a_got = _len3(c['a.imp_force']) / A['mass'] + _len3(c['a.deflection'])
        b_got = _len3(c['b.imp_force']) / B['mass'] + _len3(c['b.deflection'])
        # what the integrator then DID with it, on top of free travel:
        #   response displacement = ((imp + force*dt)/m)*dt + deflection
        # The integrator is imp += force*dt; vel += imp/m; pos += vel*dt;
        # pos += deflection -- so this is exact, not an approximation.
        # (Only the horizontal axes: gravity lives in the y lane.)
        def response_dpos(tag, v0):
            return [c[tag + '.dpos'][k] - v0[k] * dt for k in (0, 2)]

        def predicted(tag, mass):
            return [((c[tag + '.imp_force'][k] + c[tag + '.force'][k] * dt)
                     / mass) * dt + c[tag + '.deflection'][k] for k in (0, 2)]

        a_v0 = list(sa.get('vel', [0, 0, 0]))
        b_v0 = list(sb.get('vel', [0, 0, 0]))
        a_resp = response_dpos('a', a_v0)
        b_resp = response_dpos('b', b_v0)
        a_moved = math.hypot(*a_resp)
        b_moved = math.hypot(*b_resp)
        print("  %-32s impact=%9.1f | racer got %.4f pushed %.4f m | "
              "traffic got %.4f pushed %.4f m"
              % (name, c['impact'], a_got, a_moved, b_got, b_moved))

        # (1) SOMEBODY has to receive the contact.  A resolve that writes
        #     nothing to either body is a phantom collision.
        chk_gt(name + ".pair receives a response", a_got + b_got, 1e-4)

        if who == "both":
            # An ALIVE traffic car is a two-way contact: FUN_001121F0 splits
            # the separation by mass and gives both bodies the impulse.
            chk_gt(name + ".racer feels the alive traffic car", a_got, 1e-3)
            chk_gt(name + ".traffic feels the racer", b_got, 1e-3)
        else:
            # FUN_00113960 @0x00113B75: the alive car is kind 2, IMMOVABLE.
            # The racer is meant to get exactly nothing here -- which is
            # precisely why the WRECK must be integrated by the harness.
            chk(name + ".racer gets nothing from a wreck (kind 2)",
                a_got, 0.0, tol=1e-6, rel=0.0)
            chk_gt(name + ".the wreck receives the whole contact", b_got, 1e-3)

        # (2) ...and one frame of the REAL integrator must turn that into
        #     motion.  This is the assertion carcol_pass() was failing: it
        #     skipped b3_rigid_body_integrate for `crashed_until > g_race_time`
        #     traffic, so the only body holding the response never consumed
        #     it, the wreck stood still, and the racer -- which FUN_00113960
        #     deliberately gives nothing -- drove straight through it.
        chk_gt(name + ".the contact actually pushes a body",
               a_moved + b_moved, 1e-3)
        # the push must be EXACTLY what the solve wrote: no accumulator may
        # be left unconsumed, and none may be applied twice.
        chk(name + ".racer push == what the solve gave it",
            a_resp, predicted('a', A['mass']), tol=1e-5, rel=1e-4)
        chk(name + ".traffic push == what the solve gave it",
            b_resp, predicted('b', B['mass']), tol=1e-5, rel=1e-4)
        if who == "wreck-only":
            chk_gt(name + ".the wreck is shoved out of the way", b_moved, 1e-3)
            # a parked wreck may be pushed ALONG the road, never through it:
            # FUN_001121F0 flattens the separation's Y for an alive pair,
            # FUN_00113960 does not, so the harness pins the wreck's height.
            chk(name + ".the wreck stays at road height",
                c['b.dpos'][1], 0.0, tol=1e-6, rel=0.0)


# ---------------------------------------------------------------------------
# THE RACING GATHER'S TWO RUNTIME FILTERS -- FUN_0011BBE0, over the REAL
# build/collision.bin.  Not car-vs-car, but the same collision boundary:
# b3_sweep_sphere_ex is what feeds FUN_0011AEF0's wall trigger and
# FUN_00112E70's object trigger with a contact.
# ---------------------------------------------------------------------------
GATHER_DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include "burnout3_collision.h"

/* stdin: "sweep x y z r nymax usev vx vy vz" -> "res hit type nx ny nz" */
int main(int argc, char** argv) {
    if (b3_collision_load(argc > 1 ? argv[1] : "build/collision.bin") <= 0) {
        fprintf(stderr, "no collision.bin\n");
        return 1;
    }
    char cmd[32];
    while (scanf("%31s", cmd) == 1) {
        if (!strcmp(cmd, "sweep")) {
            float x, y, z, r, ny, vx, vy, vz, q[3], n[3];
            unsigned short ty = 0;
            int usev;
            if (scanf("%f %f %f %f %f %d %f %f %f",
                      &x, &y, &z, &r, &ny, &usev, &vx, &vy, &vz) != 9) break;
            float p[3] = {x, y, z};
            float v[3] = {vx, vy, vz};
            int hit = b3_sweep_sphere_ex(p, p, r, ny, usev ? v : NULL,
                                         q, n, &ty);
            printf("res %d %u %.5f %.5f %.5f\n", hit, (unsigned)ty,
                   hit ? n[0] : 0.0f, hit ? n[1] : 0.0f, hit ? n[2] : 0.0f);
        } else if (!strcmp(cmd, "quit")) {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
'''


def _elf_reader():
    elf = open(os.path.join(ROOT, "build", "burnout3.elf"), "rb").read()
    ph_off = struct.unpack_from('<I', elf, 0x1C)[0]
    ph_num = struct.unpack_from('<H', elf, 0x2C)[0]
    segs = [s for s in (struct.unpack_from('<IIIIIIII', elf, ph_off + i * 32)
                        for i in range(ph_num)) if s[0] == 1]

    def rd(va, n):
        for t, o, v, _p, f, _m, _fl, _a in segs:
            if v <= va < v + f:
                return elf[o + (va - v):o + (va - v) + n]
        return b'\0' * n
    return rd


def run_gather():
    """FUN_0011BBE0's two runtime filters against the real collision world."""
    section("racing gather runtime filters (FUN_0011BBE0)")
    rd = _elf_reader()
    # ---- the predicate, straight out of the instruction bytes -----------
    chk("0x003B1684 gather velocity limit = 0.5",
        struct.unpack('<f', rd(0x003B1684, 4))[0], 0.5, 1e-9)
    chk("0x0039B264 gather normal.y floor = -0.7",
        struct.unpack('<f', rd(0x0039B264, 4))[0], -0.7, 1e-6)
    chk_eq("0x0011BBFE CMP low, 0x23", rd(0x0011BBFE, 3), b'\x83\xf8\x23')
    chk_eq("0x0011BC03 CMP low, 0x22", rd(0x0011BC03, 3), b'\x83\xf8\x22')
    chk_eq("0x0011BC08 TEST ch, 0x10 (type & 0x1000)",
           rd(0x0011BC08, 3), b'\xf6\xc5\x10')
    chk_eq("0x0011BC0D CMP low, 0x15", rd(0x0011BC0D, 3), b'\x83\xf8\x15')
    chk_eq("0x0011BC15 CMP low, 0x20", rd(0x0011BC15, 3), b'\x83\xf8\x20')
    chk_eq("0x0011BC1A reads veh+0xB0 (the car's OWN velocity)",
           rd(0x0011BC1A, 7), b'\x0f\x28\x86\xb0\x00\x00\x00')
    chk_eq("0x0011BC21 takes the record's normal at +0x10",
           rd(0x0011BC21, 3), b'\x8d\x47\x10')
    chk_eq("0x0011BC2D CALL FUN_00013C60 (the dot)", rd(0x0011BC2D, 1),
           b'\xe8')
    chk_eq("0x0011BC32 COMISS the dot with [0x003B1684]",
           rd(0x0011BC32, 7), b'\x0f\x2f\x05\x84\x16\x3b\x00')
    chk_eq("0x0011BC3B loads [0x0039B264]", rd(0x0011BC3B, 8),
           b'\xf3\x0f\x10\x05\x64\xb2\x39\x00')
    chk_eq("0x0011BC43 COMISS it against normal.y (record+0x14)",
           rd(0x0011BC43, 4), b'\x0f\x2f\x47\x14')
    chk_eq("0x0011BC4B CALL FUN_0010A8E0 (append to the soup)",
           rd(0x0011BC4B, 1), b'\xe8')

    binpath = os.path.join(ROOT, "build", "collision.bin")
    if not os.path.exists(binpath):
        print("  (build/collision.bin missing -- behaviour cases skipped)")
        return
    d = tempfile.mkdtemp(prefix="gather_")
    src = os.path.join(d, "gather_drv.c")
    open(src, "w").write(GATHER_DRIVER)
    exe = os.path.join(d, "gather_drv")
    r = subprocess.run(
        ["gcc", "-std=c11", "-O2", "-I", os.path.join(ROOT, "src"),
         "-o", exe, src, os.path.join(ROOT, "src", "burnout3_collision.c"),
         "-lm"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr)
        raise SystemExit("gather driver build failed")

    data = open(binpath, "rb").read()
    ntri = struct.unpack_from('<I', data, 8)[0]

    def tri(i):
        o = 0x28 + i * 40
        v = struct.unpack_from('<9f', data, o)
        ty = struct.unpack_from('<H', data, o + 36)[0]
        p0 = (v[0], v[1], -v[2])          # loader: negate z, swap v1/v2
        p1 = (v[6], v[7], -v[8])
        p2 = (v[3], v[4], -v[5])
        e1 = [p1[k] - p0[k] for k in range(3)]
        e2 = [p2[k] - p0[k] for k in range(3)]
        n = [e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2],
             e1[0]*e2[1] - e1[1]*e2[0]]
        ln = math.sqrt(sum(c * c for c in n)) or 1.0
        n = [c / ln for c in n]
        c = [(p0[k] + p1[k] + p2[k]) / 3.0 for k in range(3)]
        return ty, n, c

    def sweeps(rows):
        payload = "".join(
            "sweep %.5f %.5f %.5f %.3f 1.1 %d %.5f %.5f %.5f\n"
            % (p[0], p[1], p[2], rad, usev, v[0], v[1], v[2])
            for p, rad, usev, v in rows) + "quit\n"
        o = subprocess.run([exe, binpath], input=payload,
                           capture_output=True, text=True)
        return [l.split() for l in o.stdout.splitlines()
                if l.startswith("res ")]

    # Pick ISOLATED representatives: a candidate only qualifies if a
    # velocity-free probe 0.35 in front of its face wins with ITS OWN type
    # (otherwise a neighbouring poly answers and the filter is untestable).
    RAD = 0.5
    OFF = 0.35

    def pick(pred, limit=400):
        cands = []
        for i in range(ntri):
            ty, n, c = tri(i)
            if abs(n[1]) < 0.2 and pred(ty & 0xFF):
                cands.append((ty, n, c))
                if len(cands) >= limit:
                    break
        if not cands:
            return None
        rows = [([c[k] + n[k] * OFF for k in range(3)], RAD, 0, (0, 0, 0))
                for ty, n, c in cands]
        got = sweeps(rows)
        for k, (ty, n, c) in enumerate(cands):
            if k < len(got) and int(got[k][1]) == 1 and int(got[k][2]) == ty:
                return (ty, n, c)
        return None

    struct_i = pick(lambda lo: 0x15 <= lo < 0x20)
    chevron_i = pick(lambda lo: lo == 0x20)
    plain_i = pick(lambda lo: lo < 0x15)

    lines, cases = [], []
    rows = []

    def q(c, n, usev, v):
        # sit the sphere just in FRONT of the face so the one-sided test passes
        rows.append(([c[k] + n[k] * OFF for k in range(3)], RAD, usev, v))

    # A dense soup means "the face was dropped" shows up as "it no longer
    # WINS the sweep", not necessarily as "nothing was hit" -- a neighbour
    # may answer instead.  `is_ty` asserts the face won, `not_ty` that it did
    # not.
    if chevron_i:
        ty, n, c = chevron_i
        q(c, n, 0, (0, 0, 0))
        cases.append(("chevron board (0x20) still collides", "is_ty", ty))
        q(c, n, 1, [-n[k] * 20.0 for k in range(3)])
        cases.append(("chevron board, driving INTO it: collides",
                      "is_ty", ty))
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("chevron board, separating at 20 m/s: dropped",
                      "not_ty", ty))
    if struct_i:
        ty, n, c = struct_i
        lo = ty & 0xFF
        q(c, n, 1, [-n[k] * 20.0 for k in range(3)])
        cases.append(("armco 0x%02X, driving INTO it: collides" % lo,
                      "is_ty", ty))
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("armco 0x%02X, separating at 20 m/s: dropped (no snag)"
                      % lo, "not_ty", ty))
        q(c, n, 1, [n[k] * 0.4 for k in range(3)])
        cases.append(("armco, separating at only 0.4 m/s (< 0.5): kept",
                      "is_ty", ty))
    if plain_i:
        ty, n, c = plain_i
        q(c, n, 1, [n[k] * 20.0 for k in range(3)])
        cases.append(("a non-structure surface 0x%02X ignores filter (a)"
                      % (ty & 0xFF), "is_ty", ty))
    res = sweeps(rows)
    for k, (name, kind, ty) in enumerate(cases):
        if k >= len(res):
            chk_eq(name, "missing", "a res line")
            continue
        hit, got_ty = int(res[k][1]), int(res[k][2])
        if kind == "is_ty":
            chk_eq(name, (hit, got_ty), (1, ty))
        else:
            chk_eq(name, hit and got_ty == ty, False)

    # filter (b): a downward-facing face may never block, whatever
    # wall_ny_max the caller passes.
    down = [i for i in range(ntri) if tri(i)[1][1] < -0.7]
    if down:
        rows = []
        for i in down[:40]:
            ty, n, c = tri(i)
            q(c, n, 0, (0, 0, 0))
        res = sweeps(rows)
        hits = [x for x in res if int(x[1]) == 1 and int(x[2]) == 0]
        chk_eq("normal.y < -0.7 faces never win a sweep (%d sampled of %d)"
               % (len(res), len(down)), len(hits), 0)


# ---------------------------------------------------------------------------
# FUN_0010FCE0 -- the 2-D segment/segment closest points the type-3 response
# uses instead of the convex hull.
# ---------------------------------------------------------------------------
SEG_CASES = [
    ("parallel-offset",  (0, 0), (0, 4),   (2, 1),  (2, 5)),
    ("crossing",         (-2, 0), (2, 0),  (0, -2), (0.3, 2)),
    ("skew-near",        (0, 0), (0, 4),   (1.2, 1.0), (2.4, 3.4)),
    ("endpoint-nearest", (0, 0), (0, 4),   (0.5, 6), (3.0, 8)),
    ("nose-to-tail",     (0, -2), (0, 2),  (0.1, 2.6), (0.1, 6.6)),
    ("angled-45",        (0, 0), (3, 3),   (2, 0),  (5, 3.2)),
    ("orthogonal",       (0, 0), (0, 4),   (-2, 2), (2, 2)),   # the sentinel
    ("far-apart",        (0, 0), (0, 4),   (50, 50), (52, 54)),
]


def run_seg2d():
    section("2-D capsule axis closest points (FUN_0010FCE0)")
    s = ec.Session()
    for name, a0, a1, b0, b1 in SEG_CASES:
        g = s.seg_closest2d(a0, a1, b0, b1)
        payload = "8\n%s\n" % " ".join(
            "%.9g" % x for x in list(a0) + list(a1) + list(b0) + list(b1))
        c = run_driver(payload)
        chk(name + ".dist", c['dist'], g['dist'], tol=1e-5, rel=1e-6)
        if abs(g['dist'] - 1000.0) < 1e-6:
            print("  %-18s ORTHOGONAL -> sentinel 1000.0 [0x003B16CC]" % name)
            continue
        chk(name + ".pa", c['pa'][:2], g['pa'], tol=1e-5)
        chk(name + ".pb", c['pb'][:2], g['pb'], tol=1e-5)
        chk(name + ".ta", c['ta'], g['ta'], tol=1e-6)
        chk(name + ".tb", c['tb'], g['tb'], tol=1e-6)
        print("  %-18s dist=%8.4f ta=%.3f tb=%.3f" % (name, g['dist'],
                                                      g['ta'], g['tb']))


# ---------------------------------------------------------------------------
# FUN_00112E70 -- a car against a LIVE traffic car (a type-3 object).
#
# The retail contract these cases pin down:
#   * the RESPONSE is a 2-D capsule test, not the convex hull;
#   * 100 % of the penetration and the whole 100.0 * min(mass, 2000) shove go
#     to the CAR (veh+0x130 / FUN_001205E0) -- the traffic car has no rigid
#     body and receives NOTHING;
#   * above `authority * 75` mph of normal closing (or `* 20` in a crash
#     party) the car crashes and FUN_00114910 PROMOTES the traffic object
#     into a real vehicle, which FUN_00113960 then launches.
# ---------------------------------------------------------------------------
TRAFFIC_CASES = [
    #  name                car(pos,yaw,speed,vel)         traffic(pos,yaw,spd)
    ("side-rub",      (0, 0, 0), 0.0, 20.0, (1.9, 0, 0.4), 0.0, 18.0, {}),
    ("side-rub-deep", (0, 0, 0), 0.0, 20.0, (1.4, 0, 0.2), 0.0, 18.0, {}),
    ("closing-slow",  (0, 0, 0), 0.0, 12.0, (0.6, 0, 4.0), 0.0, 10.0, {}),
    ("closing-hard",  (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0, {}),
    ("head-on",       (0, 0, 0), 0.0, 30.0, (0.4, 0, 3.0), 3.14159265, 12.0, {}),
    ("oblique",       (0, 0, 0), 0.25, 34.0, (1.7, 0, 2.0), -0.2, 16.0, {}),
    ("stationary-traffic",
                      (0, 0, 0), 0.0, 25.0, (0.3, 0, 4.1), 0.0, 0.0, {}),
    ("miss",          (0, 0, 0), 0.0, 25.0, (6.0, 0, 4.0), 0.0, 18.0, {}),
    ("y-gate-above",  (0, 3.5, 0), 0.0, 25.0, (0.4, 0, 4.0), 0.0, 10.0, {}),
    ("y-gate-inside", (0, 1.5, 0), 0.0, 25.0, (0.4, 0, 4.0), 0.0, 10.0, {}),
    ("crash-party",   (0, 0, 0), 0.0, 30.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(party=(6, 0))),
    ("slammed-authority",
                      (0, 0, 0), 0.0, 20.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(authority=0.1)),
    ("no-crash-flag", (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(flags_174=8)),
    ("crash-veto-1353",
                      (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(flags_1353=0x10)),
    ("muted-1353",    (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(flags_1353=0x02)),
    ("spawn-immune",  (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(immune=1)),
    ("car-already-crashed",
                      (0, 0, 0), 0.0, 60.0, (0.6, 0, 4.0), 0.0, 10.0,
                      dict(crashed=1)),
    ("truck-side",    (0, 0, 0), 0.0, 40.0, (2.3, 0, 1.0), 0.05, 14.0,
                      dict(tcar=("HEVY", "Car23"))),
    ("heavy-head-on", (0, 0, 0), 0.0, 55.0, (0.2, 0, 3.4), 3.14159265, 14.0,
                      dict(tcar=("HEVY", "Car11"))),
]

TRAFFIC_MASS = 1500.0
CAR_MASS = 1200.0


def _traffic_states(spec):
    (name, apos, ayaw, aspd, bpos, byaw, bspd, opt) = spec
    acar = opt.get('car', ("COMP", "Car1"))
    tcar = opt.get('tcar', ("HEVY", "Car11"))
    A = make_state(dict(pos=apos, yaw=ayaw, mass=CAR_MASS,
                        vel=[math.sin(ayaw) * aspd, 0.0,
                             math.cos(ayaw) * aspd],
                        type=0, crashed=opt.get('crashed', 0)), *acar)
    A['speed'] = aspd
    A['authority'] = opt.get('authority', 1.0)
    A['flags_1353'] = opt.get('flags_1353', 0)
    A['immune'] = opt.get('immune', 0)
    B = make_state(dict(pos=bpos, yaw=byaw, mass=TRAFFIC_MASS,
                        vel=[math.sin(byaw) * bspd, 0.0,
                             math.cos(byaw) * bspd],
                        type=3), *tcar)
    B['speed'] = bspd
    B['no_crash'] = 1 if opt.get('flags_174', 0) & 8 else 0
    B['designated'] = 1          # FUN_00120BA0 always stamps DAT_0073BB8C
    return name, A, B, opt


def run_traffic():
    section("car vs a LIVE traffic car -- the type-3 arm (FUN_00112E70)")
    for spec in TRAFFIC_CASES:
        name, A, B, opt = _traffic_states(spec)
        party = opt.get('party', (0, 0))
        s = ec.Session()
        s.set_game_mode(*party)
        s.seed(0, A)
        s.uc.mem_write(ec.VEH_A + 0xBC, ec.f2b(A['speed']))
        s.uc.mem_write(ec.VEH_A + 0x1534, ec.f2b(A['authority']))
        s.uc.mem_write(ec.VEH_A + 0x152C,
                       ec.f2b(1.0 if A['immune'] else -1.0))
        s.uc.mem_write(ec.VEH_A + 0x1353, bytes([A['flags_1353']]))
        tst = dict(frame=B['frame'], bbmax=B['bbmax'], bbmin=B['bbmin'],
                   mass=TRAFFIC_MASS, speed=B['speed'],
                   flags_174=opt.get('flags_174', 0),
                   hull=ec.load_hull(B['_cls'], B['_car']))
        s.seed_traffic(tst)
        g = s.resolve_traffic()

        # The port's traffic body only becomes a rigid body when it is
        # promoted, and FUN_00120BA0 seeds that body's velocity as
        # frame.at * speed (@0x00120DE3) -- which is exactly what the
        # harness's traffic_update already leaves in t->rb.
        Bp = dict(B)
        Bp['vel'] = [B['frame'][2][k] * B['speed'] for k in range(3)]
        Bp['omega'] = [0.0, 0.0, 0.0]
        payload = ("7\n%s\n%s\n%d\n"
                   % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                      body_payload(Bp, hull_path(B['_cls'], B['_car'])),
                      1 if (party[0] == 6 or party[1] in (3, 4, 5)) else 0))
        c = run_driver(payload)

        chk_eq(name + ".hit", int(c['hit']), int(g['hit']))
        chk_eq(name + ".promote", int(c['promote']), int(g['promote']))
        chk_eq(name + ".crash_a", int(c['crash_a']), int(g['crash_a']))
        print("  %-22s hit=%d push=%d promote=%d impact=%9.1f "
              "car_defl=%.4f m  traffic_imp=%.0f"
              % (name, int(c['hit']), int(c['push']), int(c['promote']),
                 c['impact'], _len3(c['a.deflection']),
                 _len3(c['b.imp_force'])))
        if not g['hit']:
            continue
        chk(name + ".impact", c['impact'], g['impact'], tol=1e-2, rel=1e-5)
        chk(name + ".normal", c['normal'], g['normal'], tol=1e-4)
        # the car's half of the response
        chk(name + ".car.deflection", c['a.deflection'],
            g['a']['deflection'], tol=1e-4)
        chk(name + ".car.force", c['a.force'], g['a']['force'],
            tol=1e-2, rel=1e-5)
        chk(name + ".car.torque", c['a.torque'], g['a']['torque'],
            tol=1e-2, rel=1e-5)
        if int(c['push']):
            chk(name + ".point", c['point'], g['point'], tol=1e-4)
            # THE CONTRACT: a sub-threshold traffic contact writes NOTHING
            # to the traffic car -- it has no rigid body to write to.
            chk_eq(name + ".traffic-untouched",
                   (_len3(g['b']['deflection']) + _len3(g['b']['imp_force'])
                    + _len3(g['b']['force'])) == 0.0, True)
            chk_eq(name + ".port-traffic-untouched",
                   (_len3(c['b.deflection']) + _len3(c['b.imp_force'])
                    + _len3(c['b.force'])) == 0.0, True)
        if int(g['promote']):
            # the promoted car is the ONLY body that moves: FUN_00113960
            # forces the still-un-crashed racer to kind 2 (immovable).
            chk(name + ".promoted.imp_force", c['b.imp_force'],
                g['b']['imp_force'], tol=1e-1, rel=1e-4)
            chk(name + ".promoted.imp_torque", c['b.imp_torque'],
                g['b']['imp_torque'], tol=1e-1, rel=1e-4)
            chk(name + ".promoted.deflection", c['b.deflection'],
                g['b']['deflection'], tol=1e-4)
            chk_gt(name + ".promoted actually moves",
                   _len3(c['b.imp_force']) / TRAFFIC_MASS
                   + _len3(c['b.deflection']), 0.05)


# ---------------------------------------------------------------------------
# THE RE-CONTACT REGRESSION.
#
# The measured cost of the old routing (racer-vs-traffic through
# FUN_001121F0) was that 78 % of broadphase pairs resolved every frame:
# the mass split gave the racer only ~55 % of the penetration and handed the
# rest to a traffic body whose pose traffic_update() rebuilt from the lane
# cursor, so the same pair re-contacted forever.  Retail's type-3 arm gives
# the CAR the whole penetration, so one frame clears the overlap even though
# the traffic car never moves.
# ---------------------------------------------------------------------------
RECONTACT_CASES = [
    ("side-rub",       (0, 0, 0), 0.0, 20.0, (1.9, 0, 0.4), 0.0, 18.0, {}),
    ("side-rub-deep",  (0, 0, 0), 0.0, 20.0, (1.4, 0, 0.2), 0.0, 18.0, {}),
    ("nose-in",        (0, 0, 0), 0.0, 12.0, (0.6, 0, 4.0), 0.0, 11.0, {}),
    ("oblique",        (0, 0, 0), 0.25, 20.0, (1.7, 0, 2.0), -0.2, 18.0, {}),
]


def run_recontact():
    section("the same pair must not re-contact forever "
            "(the 78 %-of-pairs regression)")
    dt = 1.0 / 60.0
    N = 40
    for spec in RECONTACT_CASES:
        name, A, B, opt = _traffic_states(spec)
        Bp = dict(B)
        Bp['vel'] = [B['frame'][2][k] * B['speed'] for k in range(3)]
        Bp['omega'] = [0.0, 0.0, 0.0]
        got = {}
        for arm in (0, 1):
            payload = ("9\n%s\n%s\n0\n%.9g\n%d\n%d\n"
                       % (body_payload(A, hull_path(A['_cls'], A['_car'])),
                          body_payload(Bp, hull_path(B['_cls'], B['_car'])),
                          dt, N, arm))
            got[arm] = run_driver(payload)
        t3, old = got[0], got[1]
        chk_eq(name + ".frame1.hit", int(t3['hit']), 1)
        chk_eq(name + ".frame1.push", int(t3['push']), 1)
        # THE CONTRACT: 100 % of the penetration to the CAR, nothing to the
        # traffic car (FUN_00112E70 @0x00113431 -- no mass split at all).
        chk(name + ".car takes the WHOLE penetration",
            _len3(t3['a.deflection']), t3['pen'], tol=1e-5, rel=1e-5)
        chk_eq(name + ".traffic takes none",
               _len3(t3['b.deflection']) == 0.0, True)
        # and the old routing did NOT: the racer got a mass-split share and
        # the rest went to a body the lane rebuild overwrites.
        chk_gt(name + ".old routing under-separated the car",
               t3['pen'] - _len3(old['a.deflection']), 1e-3)
        chk_gt(name + ".the car is actually pushed out", t3['pen'], 1e-3)
        # the pair must actually come apart, and sooner than the old arm
        chk_eq(name + ".type-3 arm clears within %d frames" % N,
               int(t3['cleared']) >= 0, True)
        # `old cleared == 0` means the racer-vs-racer arm never saw the
        # contact AT ALL -- the convex hull misses what the capsule catches.
        # That is a drive-through, not an improvement, so score it as such.
        drive_through = int(old['cleared']) == 0
        chk_eq(name + ".sooner than the old routing",
               drive_through
               or (int(t3['cleared']) >= 0
                   and (int(old['cleared']) < 0
                        or int(t3['cleared']) <= int(old['cleared']))), True)
        print("  %-18s pen=%.4f m | type-3: car takes %.4f, clears on frame "
              "%s | old routing: car takes %.4f, %s"
              % (name, t3['pen'], _len3(t3['a.deflection']),
                 int(t3['cleared']), _len3(old['a.deflection']),
                 "NEVER SAW THE CONTACT (drive-through)" if drive_through
                 else "clears on frame %d" % int(old['cleared'])))


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    build_driver()
    if which in ("all", "gather"):   run_gather()
    if which in ("all", "narrow"):   run_narrow()
    if which in ("all", "aabb"):     run_aabb()
    if which in ("all", "impulse"):  run_impulse()
    if which in ("all", "force"):    run_force()
    if which in ("all", "alive"):    run_response(1, "racer vs racer (FUN_001121F0)")
    if which in ("all", "wreck"):    run_response(2, "car vs wreck (FUN_00113960)",
                                                  wreck_setup)
    if which in ("all", "consume"):  run_consume()
    if which in ("all", "seg2d"):    run_seg2d()
    if which in ("all", "traffic"):  run_traffic()
    if which in ("all", "recontact"): run_recontact()
    print("\n%d/%d passed" % (PASS, PASS + FAIL))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
