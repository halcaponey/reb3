#!/usr/bin/env python3
"""
Differential acceptance test for the JOINED takedown -> SCORE path.

tools/validate_td_rules.py proves the takedown TRIGGER rules and
tools/validate_score_events.py proves the four boost/BP EARN events, but
neither owns the join: a contact happens, and the BP, the boost transfer and
the counters it moves on BOTH cars have to be retail's.  That join is this
file, and it is a new axis rather than a new section of either suite because
it needs both worlds at once -- the td_rules Unicorn world for the trigger and
the slam scorer's own parameter block for the consequences.

Every expected number here is EXECUTED retail: the real FUN_001989A0 runs
under Unicorn with FUN_00197F90 (the type selector) and FUN_0019A050 (the
callout/BP poster) LIVE, and the compiled C module is driven from identical
seeded state.  Nothing is compared against a transcription of the decompiler
and nothing is compared against the port's own previous behaviour.

Functions executed for real
  FUN_001989A0  the slam handler -- BP, boost, counters, both cars
  FUN_00197F90  the slam TYPE selector (pure geometry on pv+0x204)
  FUN_0019A050  the combo/callout poster that actually pays the slam BP
  FUN_00029F30  the game-context +0x64 dispatcher (the rub case)
  FUN_00197BE0  the full-slam gate
  FUN_00197920  the contact/pass notification (the near-miss case)
  FUN_00197430 / FUN_00197040 / FUN_00198E60 / FUN_001994D0
                the crash -> attribution -> claim -> commit -> award chain

Sections
   1  image constants        every parameter, read out of build/burnout3.elf
   2  the type taxonomy      FUN_00197F90 executed over seeded frames
   3  the Super-Slam gate    the three disjuncts and both strict boundaries
   4  slam BP                4 types x cheap x burning, both sides
   5  attacker boost gain    tier / mult / bonus / scale / clamp / crash party
   6  victim boost drain     tier / mult / floor / peg flags / forced stop
   7  aggressor vs victim    the full field split, repeated slams
   8  the AI grudge          racecar+0x23E0, the only user of slam strength
   9  an already-wrecked victim
  10  slam -> crash -> commit -> award, end to end
  11  a NEAR MISS must not score as a takedown
  12  a RUB must not score as a takedown
  13  the retail bridge's field widths

Usage:  python3 tools/validate_takedown_score.py [section] [-v]
"""
import os

# burnout3_td_rules.c consults build/backends.cfg; pin it to the RE path so
# this differential is unaffected by the live backend selection.
os.environ['B3_BACKENDS'] = '/dev/null'

import struct
import subprocess
import sys
import tempfile

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
sys.path.insert(0, _here)

import emulate_td_rules as E                                   # noqa: E402

VERBOSE = '-v' in sys.argv
PASS = 0
FAIL = 0
FAILURES = []


def check(name, got, want, tol=None):
    global PASS, FAIL
    if tol is not None:
        try:
            ok = abs(float(got) - float(want)) <= tol
        except (TypeError, ValueError):
            ok = False
    else:
        ok = got == want
    if ok:
        PASS += 1
        if VERBOSE:
            print("  ok   %-62s %s" % (name, got))
    else:
        FAIL += 1
        FAILURES.append("%-62s got %r want %r" % (name, got, want))
        print("  FAIL %-62s got %r want %r" % (name, got, want))


# ==========================================================================
# the image: every parameter this suite drives both sides with
# ==========================================================================
def _load_image():
    data = open(os.path.join(_root, 'build', 'burnout3.elf'), 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        t, off, va, _pa, fsz, _msz, _f, _a = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if t == 1:
            segs.append((va, off, fsz))

    def rd(va, n):
        for v, o, f in segs:
            if v <= va < v + f:
                return data[o + (va - v):o + (va - v) + n]
        raise KeyError("VA 0x%08X not mapped" % va)
    return rd


RD = _load_image()
I32 = lambda va: struct.unpack('<i', RD(va, 4))[0]       # noqa: E731
F32 = lambda va: struct.unpack('<f', RD(va, 4))[0]       # noqa: E731

# The COMPILED-IN defaults, straight out of .data.  Both sides are seeded
# from these, so no number in this file is invented and no number is the
# port's own.
IMG = dict(
    slam_bp=[I32(0x003F7448 + 4 * k) for k in range(4)],   # "Slam Type BP"
    super_bp=[I32(0x003F7458 + 4 * k) for k in range(4)],  # "Super Slam Type BP"
    burning_bp=I32(0x003F7444),                            # "Burning Slam Extra BP"
    slam_energy=F32(0x003F73EC),
    boost_quantum=F32(0x003F72E4),
)
IMG_MPH = F32(0x0038994C)          # 2.2369363
IMG_CHEAP_MPH = F32(0x003A2938)    # 70
IMG_CHEAP_S = F32(0x003B1698)      # 3
IMG_HEAD_LO = F32(0x003B1770)      # 45
IMG_HEAD_HI = F32(0x003B1DA4)      # 135
IMG_REAR_CONE = F32(0x003A7964)    # 30
IMG_SIDE_CONE = F32(0x0041A798)    # 25
IMG_MSG_SLAM = list(RD(0x003A4B18, 16))
IMG_MSG_SLAM2 = list(RD(0x003A4B28, 16))
IMG_BP_TAKEDOWN = I32(0x003F746C)   # the COMPILED default (1000);
# the oracle's world seeds the retail vdb tune (150) over it, so the
# end-to-end section reads the live value back instead of using this.


# ==========================================================================
# the C driver
# ==========================================================================
DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "burnout3_td_rules.h"
#include "burnout3_crash.h"

void b3_rigid_body_integrate(struct B3RigidBody* rb, float mass_kg,
                             float com_height, int in_race, int state6,
                             float dt) {
    (void)rb; (void)mass_kg; (void)com_height;
    (void)in_race; (void)state6; (void)dt;
}

static B3TdRules R;
static float POS[B3_TDR_MAX_CARS][3];
static int use_pos = 0;

static void dump(int s) {
    B3TdCar* c = &R.car[s];
    printf("bp=%d bp_td=%d bp_aggr=%d slams_made=%d times_slammed=%d "
           "td_made=%d aftertouch_td=%d boost_tier=%d boost_size=%.6f "
           "boost_meter=%.6f boost_earned=%.6f boost_forcestop=%d "
           "slam_kind=%d slam_cheap=%d slam_burning=%d slam_energy=%.6f "
           "slammed_energy=%.6f last_slam_time=%.6f slam_time=%.6f "
           "slam_type=%d last_slam_kind=%d aggressor=%d aggressor_time=%.6f "
           "human_slam=%d ai_aggression=%.6f td_credited=%d td_count=%d\n",
           c->bp, c->bp_takedown, c->bp_aggressive, c->slams_made,
           c->times_slammed, c->td_made, c->aftertouch_td, c->boost_tier,
           c->boost_size, c->boost_meter, c->boost_earned, c->boost_forcestop,
           c->slam_kind, c->slam_cheap, c->slam_burning, c->slam_energy,
           c->slammed_energy, c->last_slam_time, c->slam_time, c->slam_type,
           c->last_slam_kind, c->aggressor, c->aggressor_time, c->human_slam,
           c->ai_aggression, c->td_credited, c->td_count);
}

int main(void) {
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "reset")) {
            int n; sscanf(line, "%*s %d", &n);
            b3_td_reset(&R, n); use_pos = 0;
            memset(POS, 0, sizeof(POS));
        } else if (!strcmp(cmd, "car")) {
            int s, cls, g; sscanf(line, "%*s %d %d %d", &s, &cls, &g);
            b3_td_set_car(&R, s, cls, g);
        } else if (!strcmp(cmd, "params")) {
            B3TdSlamParams* p = &b3_td_slam_params;
            double se, bq, bs; int bbp, cp;
            sscanf(line, "%*s %d %d %d %d %d %d %d %d %d %d %d %lf %lf %lf %d",
                   &p->slam_bp[0], &p->slam_bp[1], &p->slam_bp[2],
                   &p->slam_bp[3], &p->super_bp[0], &p->super_bp[1],
                   &p->super_bp[2], &p->super_bp[3], &bbp, &cp, &cp,
                   &se, &bq, &bs, &cp);
            p->burning_bp = bbp;
            p->slam_energy = (float)se; p->boost_quantum = (float)bq;
            p->boost_scale = (float)bs; p->crash_party = cp;
        } else if (!strcmp(cmd, "set")) {
            char f[32]; int s; double v;
            sscanf(line, "%*s %d %31s %lf", &s, f, &v);
            B3TdCar* c = &R.car[s];
            if (!strcmp(f, "crashed")) c->crashed = (unsigned char)v;
            else if (!strcmp(f, "respawning")) c->respawning = (unsigned char)v;
            else if (!strcmp(f, "race_state")) c->race_state = (int)v;
            else if (!strcmp(f, "speed_ms")) c->speed_ms = (float)v;
            else if (!strcmp(f, "crash_stamp")) c->crash_stamp = (float)v;
            else if (!strcmp(f, "crash_time")) c->crash_time = (float)v;
            else if (!strcmp(f, "boost_tier")) c->boost_tier = (int)v;
            else if (!strcmp(f, "boost_size")) c->boost_size = (float)v;
            else if (!strcmp(f, "boost_meter")) c->boost_meter = (float)v;
            else if (!strcmp(f, "boost_earned")) c->boost_earned = (float)v;
            else if (!strcmp(f, "boost_mult")) c->boost_mult = (float)v;
            else if (!strcmp(f, "boost_bonus")) c->boost_bonus = (float)v;
            else if (!strcmp(f, "boost_peg_a")) c->boost_peg_a = (unsigned char)v;
            else if (!strcmp(f, "boost_peg_b")) c->boost_peg_b = (unsigned char)v;
            else if (!strcmp(f, "boosting")) c->boosting = (unsigned char)v;
            else if (!strcmp(f, "ramp_done")) c->boost_ramp_done = (unsigned char)v;
            else if (!strcmp(f, "ai_aggression")) c->ai_aggression = (float)v;
            else if (!strcmp(f, "ai_aggr_step")) c->ai_aggr_step = (float)v;
            else if (!strcmp(f, "ai_aggr_cap")) c->ai_aggr_cap = (float)v;
            else if (!strcmp(f, "aggressor")) c->aggressor = (int)v;
            else if (!strcmp(f, "aggressor_time")) c->aggressor_time = (float)v;
            else if (!strcmp(f, "td_credited")) c->td_credited = (unsigned char)v;
            else { fprintf(stderr, "bad field %s\n", f); exit(2); }
        } else if (!strcmp(cmd, "setidx")) {
            char f[32]; int s, k; double v;
            sscanf(line, "%*s %d %31s %d %lf", &s, f, &k, &v);
            B3TdCar* c = &R.car[s];
            if (!strcmp(f, "claim")) c->claim[k] = (float)v;
            else if (!strcmp(f, "contact")) c->contact_time[k] = (float)v;
            else if (!strcmp(f, "tdby")) c->taken_down_by[k] = (unsigned char)v;
            else { fprintf(stderr, "bad idx field %s\n", f); exit(2); }
        } else if (!strcmp(cmd, "frame")) {
            int s, k; float m[4][4]; double v[16];
            sscanf(line, "%*s %d %lf %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf %lf %lf %lf", &s,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
            for (k = 0; k < 16; k++) m[k / 4][k % 4] = (float)v[k];
            b3_td_set_frame(&R, s, (const float (*)[4])m);
        } else if (!strcmp(cmd, "slamtype")) {
            int k; float am[4][4], vm[4][4]; double v[32];
            sscanf(line, "%*s %lf %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf %lf %lf %lf "
                   "%lf %lf %lf %lf %lf %lf %lf %lf",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15],
                   &v[16], &v[17], &v[18], &v[19], &v[20], &v[21], &v[22],
                   &v[23], &v[24], &v[25], &v[26], &v[27], &v[28], &v[29],
                   &v[30], &v[31]);
            for (k = 0; k < 16; k++) am[k / 4][k % 4] = (float)v[k];
            for (k = 0; k < 16; k++) vm[k / 4][k % 4] = (float)v[16 + k];
            printf("slamtype %d\n",
                   b3_td_slam_type((const float (*)[4])am,
                                   (const float (*)[4])vm));
        } else if (!strcmp(cmd, "cheap")) {
            int s; double t; sscanf(line, "%*s %d %lf", &s, &t);
            printf("cheap %d\n", b3_td_slam_cheap(&R, (float)t, s));
        } else if (!strcmp(cmd, "slambp")) {
            int ty, ch, bu; sscanf(line, "%*s %d %d %d", &ty, &ch, &bu);
            printf("slambp %d\n", b3_td_slam_bp(ty, ch, bu));
        } else if (!strcmp(cmd, "slam")) {
            double t, st; int kind, a, v;
            sscanf(line, "%*s %lf %d %d %d %lf", &t, &kind, &a, &v, &st);
            printf("ret %d\n", b3_td_slam_report(&R, (float)t, kind, a, v,
                                                 (float)st));
        } else if (!strcmp(cmd, "contact")) {
            double t, dt; int a, b, tch;
            sscanf(line, "%*s %lf %lf %d %d %d", &t, &dt, &a, &b, &tch);
            b3_td_contact(&R, (float)t, (float)dt, a, b, tch);
        } else if (!strcmp(cmd, "notify")) {
            double t; int s; sscanf(line, "%*s %lf %d", &t, &s);
            b3_td_contact_notify(&R, (float)t, s);
        } else if (!strcmp(cmd, "pos")) {
            int s; double x, y, z;
            sscanf(line, "%*s %d %lf %lf %lf", &s, &x, &y, &z);
            POS[s][0] = (float)x; POS[s][1] = (float)y; POS[s][2] = (float)z;
            use_pos = 1;
        } else if (!strcmp(cmd, "crash")) {
            double t; int s, wall, hasobj, vclass, wreck;
            B3TdCause c;
            sscanf(line, "%*s %lf %d %d %d %d %d", &t, &s, &wall, &hasobj,
                   &vclass, &wreck);
            if (wall) b3_td_cause_wall(&c, 0);
            else if (hasobj) b3_td_cause_object(&c, vclass);
            else if (wreck >= 0) b3_td_cause_wreck(&c, wreck);
            else b3_td_cause_none(&c);
            b3_td_on_crash(&R, (float)t, s,
                           (wall || hasobj || wreck >= 0) ? &c : NULL,
                           use_pos ? POS : NULL);
        } else if (!strcmp(cmd, "tick")) {
            double t; int s, i, n; B3TdEvent ev[8];
            sscanf(line, "%*s %lf %d", &t, &s);
            n = b3_td_frame(&R, (float)t, s, ev, 8);
            printf("events %d\n", n);
            for (i = 0; i < n; i++)
                printf("ev %d %d %d %d %d %d %d %d\n", ev[i].kind,
                       ev[i].attacker, ev[i].victim, ev[i].message, ev[i].bp,
                       ev[i].owner, ev[i].aftertouch, ev[i].revenge);
        } else if (!strcmp(cmd, "layout")) {
            /* offset and WIDTH of every B3TdCar field that sits on a retail
             * BYTE.  burnout3_tdcar_ranges.h sends sizeof(field) bytes to the
             * racecar, so a field wider than retail's squats on its
             * neighbour. */
#define L(f) printf(" %s:%u:%u", #f, (unsigned)offsetof(B3TdCar, f), \
                    (unsigned)sizeof(((B3TdCar*)0)->f))
            printf("layout");
            L(crashed); L(respawning); L(td_credited); L(td_credited_fx);
            L(revenge_flag); L(psyche_armed); L(boost_peg_a); L(boost_peg_b);
            L(boosting); L(boost_forcestop); L(boost_ramp_done);
            L(slam_cheap); L(slam_burning); L(human_slam);
            printf("\n");
#undef L
        } else if (!strcmp(cmd, "dump")) {
            int s; sscanf(line, "%*s %d", &s); dump(s);
        } else if (!strcmp(cmd, "quit")) {
            break;
        }
        fflush(stdout);
    }
    return 0;
}
'''

SCRATCH = os.environ.get('B3_TDS_SCRATCH',
                         os.path.join(tempfile.gettempdir(), 'b3_tdscore'))


class CDriver:
    def __init__(self):
        os.makedirs(SCRATCH, exist_ok=True)
        src = os.path.join(SCRATCH, 'tds_driver.c')
        exe = os.path.join(SCRATCH, 'tds_driver')
        with open(src, 'w') as f:
            f.write(DRIVER)
        cmd = ['gcc', '-O1', '-std=c99', '-Wall',
               '-I', os.path.join(_root, 'src'), src,
               os.path.join(_root, 'src', 'burnout3_td_rules.c'),
               os.path.join(_root, 'src', 'burnout3_backend.c'),
               os.path.join(_root, 'src', 'burnout3_emu.c'),
               os.path.join(_root, 'src', 'burnout3_crash.c'),
               '-o', exe, '-lm']
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise SystemExit("driver build failed")
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def ask(self, s):
        self.cmd(s)
        return self.p.stdout.readline().strip()

    def dump(self, slot):
        line = self.ask("dump %d" % slot)
        d = {}
        for tok in line.split():
            k, v = tok.split('=', 1)
            d[k] = float(v) if '.' in v else int(v)
        return d

    def events(self, clock, slot):
        self.cmd("tick %.6f %d" % (clock, slot))
        n = int(self.p.stdout.readline().split()[1])
        out = []
        for _ in range(n):
            q = self.p.stdout.readline().split()
            out.append(dict(kind=int(q[1]), attacker=int(q[2]),
                            victim=int(q[3]), message=int(q[4]), bp=int(q[5]),
                            owner=int(q[6]), aftertouch=int(q[7]),
                            revenge=int(q[8])))
        return out

    def seed(self, ncars=6, classes=None, boost_scale=1.0, crash_party=0):
        self.cmd("reset %d" % ncars)
        for i in range(ncars):
            cls = (0 if i == 0 else 1) if classes is None else classes[i]
            self.cmd("car %d %d %d" % (i, cls, i))
        self.set_params(boost_scale=boost_scale, crash_party=crash_party)

    def set_params(self, boost_scale=1.0, crash_party=0, **over):
        p = dict(IMG)
        p.update(over)
        self.cmd("params %d %d %d %d %d %d %d %d %d 0 0 %.9g %.9g %.9g %d" % (
            p['slam_bp'][0], p['slam_bp'][1], p['slam_bp'][2], p['slam_bp'][3],
            p['super_bp'][0], p['super_bp'][1], p['super_bp'][2],
            p['super_bp'][3], p['burning_bp'],
            p['slam_energy'], p['boost_quantum'], boost_scale, crash_party))

    def set_boost(self, i, tier=0, size=720.0, meter=0.0, earned=0.0,
                  mult=1.0, bonus=0.0, peg_a=0, peg_b=0, boosting=0,
                  ramp_done=0):
        for f, v in (('boost_tier', tier), ('boost_size', size),
                     ('boost_meter', meter), ('boost_earned', earned),
                     ('boost_mult', mult), ('boost_bonus', bonus),
                     ('boost_peg_a', peg_a), ('boost_peg_b', peg_b),
                     ('boosting', boosting), ('ramp_done', ramp_done)):
            self.cmd("set %d %s %.9g" % (i, f, v))

    def set_frame(self, i, m):
        self.cmd("frame %d %s" % (i, " ".join("%.9g" % x
                                              for r in m for x in r)))


# ==========================================================================
# shared world construction
# ==========================================================================
def world(ncars=6, classes=None, clock=40.0, slam_type=0, boost_scale=1.0,
          crash_party=0, victim=1, speed_ms=40.0):
    """A td_rules world with the SLAM SCORER live and the image parameters in
    place.  Both sides of every case start from exactly this state."""
    w = E.World(ncars)
    if classes:
        for i, c in enumerate(classes):
            w.wi(w.rc(i) + 0x1920, c)
    w.seed_slam_params(IMG)
    w.live_scorer(slam_type=slam_type, boost_scale=boost_scale,
                  crash_party=crash_party)
    w.set_clock(clock)
    for i in range(ncars):
        w.set_boost(i)
        w.wf(w.rc(i) + 0x1410, -1.0e30)      # never crashed
        w.wf(w.pv(i) + 0xBC, speed_ms)
    w.posts = []
    return w


def cworld(C, ncars=6, classes=None, boost_scale=1.0, crash_party=0,
           speed_ms=40.0):
    C.seed(ncars, classes, boost_scale=boost_scale, crash_party=crash_party)
    for i in range(ncars):
        C.set_boost(i)
        C.cmd("set %d crash_stamp -1e30" % i)
        C.cmd("set %d speed_ms %.9g" % (i, speed_ms))


def rcslot(w, ptr):
    return -1 if ptr == 0 else (ptr - E.RC_BASE) // E.RC_STRIDE


def diff_state(tag, real, c, w, keys):
    """Compare the C car against the executed retail car field by field."""
    for k in keys:
        want = real[k]
        if k == 'aggressor':
            want = rcslot(w, want)
        got = c[k]
        if isinstance(want, float):
            check("%s %s" % (tag, k), got, want, 1e-4)
        else:
            check("%s %s" % (tag, k), got, want)


SLAM_KEYS = ('bp', 'bp_td', 'bp_aggr', 'slams_made', 'times_slammed',
             'boost_meter', 'boost_earned', 'boost_forcestop', 'slam_kind',
             'slam_cheap', 'slam_burning', 'slam_energy', 'slammed_energy',
             'last_slam_time', 'slam_time', 'slam_type', 'last_slam_kind',
             'aggressor', 'aggressor_time', 'human_slam', 'ai_aggression')


# ==========================================================================
# 1. image constants
# ==========================================================================
def sec1_constants(C):
    print("\n-- 1. the slam scorer's parameters, straight out of the image")
    check("0x003F7448 Slam Type BP[4] is 4 ints", len(IMG['slam_bp']), 4)
    for k in range(4):
        check("Slam Type BP[%d] @0x%08X" % (k, 0x003F7448 + 4 * k),
              I32(0x003F7448 + 4 * k), IMG['slam_bp'][k])
        check("Super Slam Type BP[%d] @0x%08X" % (k, 0x003F7458 + 4 * k),
              I32(0x003F7458 + 4 * k), IMG['super_bp'][k])
    check("0x003F7444 Burning Slam Extra BP", IMG['burning_bp'], 15)
    check("0x003F73EC slam energy", IMG['slam_energy'], 360.0, 1e-6)
    check("0x003F72E4 boost quantum", IMG['boost_quantum'], 240.0, 1e-6)
    check("0x0038994C mph", IMG_MPH, 2.2369363, 1e-6)
    check("0x003A2938 cheap-slam mph", IMG_CHEAP_MPH, 70.0, 1e-6)
    check("0x003B1698 cheap-slam crash window", IMG_CHEAP_S, 3.0, 1e-6)
    check("0x003B1770 heading gate lo", IMG_HEAD_LO, 45.0, 1e-6)
    check("0x003B1DA4 heading gate hi", IMG_HEAD_HI, 135.0, 1e-6)
    check("0x003A7964 rear cone", IMG_REAR_CONE, 30.0, 1e-6)
    check("0x0041A798 side cone", IMG_SIDE_CONE, 25.0, 1e-6)

    # the port's compiled defaults must BE the image's
    C.seed()
    for ty in range(4):
        for ch in (0, 1):
            for bu in (0, 1):
                want = (IMG['super_bp'][ty] if ch else IMG['slam_bp'][ty]) \
                       + (IMG['burning_bp'] if bu else 0)
                got = int(C.ask("slambp %d %d %d" % (ty, ch, bu)).split()[1])
                check("C default table t%d c%d b%d" % (ty, ch, bu), got, want)

    # the slam message ladder the poster indexes: type*4 + cheap + 2*burning
    check("DAT_003A4B18 is 16 message ids", len(IMG_MSG_SLAM), 16)
    check("DAT_003A4B18[0] type 0, plain", IMG_MSG_SLAM[0], 0x45)
    check("DAT_003A4B28[0] its second post", IMG_MSG_SLAM2[0], 0x49)


# ==========================================================================
# 2. the slam TYPE selector, executed
# ==========================================================================
def _frame(right, up, at, pos):
    return [list(right) + [0.0], list(up) + [0.0], list(at) + [0.0],
            list(pos) + [1.0]]


TYPE_CASES = [
    # (name, attacker frame, victim frame)
    ("dead astern, victim 8 m ahead -> REAR",
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 8))),
    ("same heading, victim 8 m to the SIDE -> not rear",
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (8, 0, 0))),
    ("same heading, victim ahead and 2 m over -> still rear",
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (2, 0, 8))),
    ("perpendicular, attacker on the victim's LEFT",
     _frame((0, 0, -1), (0, 1, 0), (1, 0, 0), (-8, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("perpendicular, attacker on the victim's RIGHT",
     _frame((0, 0, 1), (0, 1, 0), (-1, 0, 0), (8, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("perpendicular the other way, attacker LEFT",
     _frame((0, 0, 1), (0, 1, 0), (-1, 0, 0), (-8, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("head-on (180 deg apart) -> no type",
     _frame((-1, 0, 0), (0, 1, 0), (0, 0, -1), (0, 0, 8)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("60 deg oblique, attacker right axis not near the victim's at",
     _frame((0.5, 0, -0.8660254), (0, 1, 0), (0.8660254, 0, 0.5), (-8, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("100 deg, attacker right nearly along the victim's at",
     _frame((-0.1736482, 0, 0.9848078), (0, 1, 0),
            (-0.9848078, 0, -0.1736482), (8, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0))),
    ("rear cone edge: victim ahead, 20 deg off the nose",
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (3.6397023, 0, 10.0))),
    ("rear cone edge: victim ahead, 40 deg off the nose -> glance",
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 0)),
     _frame((1, 0, 0), (0, 1, 0), (0, 0, 1), (8.3909963, 0, 10.0))),
]


def sec2_type(C):
    print("\n-- 2. the slam TYPE taxonomy (FUN_00197F90 executed)")
    C.seed()
    for (name, am, vm) in TYPE_CASES:
        w = world(slam_type=None)   # the REAL selector, not the forced stub
        w.set_frame(0, am)          # attacker
        w.set_frame(1, vm)          # victim
        real = w.slam_type_call(0, 1) & 0xFF
        got = int(C.ask("slamtype %s" % " ".join(
            "%.9g" % x for r in am for x in r) + " " + " ".join(
            "%.9g" % x for r in vm for x in r)).split()[1])
        check("type: %s" % name, got, real)
        if VERBOSE:
            print("       (retail returned %d)" % real)

    # the type reaches the callout record through the whole slam
    for (name, am, vm) in TYPE_CASES[:6]:
        w = world(slam_type=None)
        w.set_frame(0, am)
        w.set_frame(1, vm)
        w.slam(0, 1, 1.0, 0)
        real = w.score_state(0)
        cworld(C)
        C.set_frame(0, am)
        C.set_frame(1, vm)
        C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
        c = C.dump(0)
        check("type through the slam: %s" % name, c['slam_kind'],
              real['slam_kind'])
        check("type -> +0x15A0: %s" % name, c['last_slam_kind'],
              real['last_slam_kind'])


# ==========================================================================
# 3. the SUPER SLAM ("cheap") gate
# ==========================================================================
def sec3_cheap(C):
    print("\n-- 3. the Super-Slam gate (FUN_001989A0 @0x00198AC6 executed)")
    eps = 0.01
    # retail's own idle value for score+0x340 (racecar+0x1410): the score reset
    # FUN_00192EA0 @0x00193020 stores [0x003B16C0].
    idle = F32(0x003B16C0)
    check("0x003B16C0 (the score reset's idle stamp)", idle, -1.0, 1e-6)
    check("FUN_00192EA0 @0x00193020 stores it at score+0x340",
          RD(0x00193020, 8).hex(), 'f30f118740030000')   # MOVSS [EDI+0x340],XMM0
    cases = [
        # (name, speed_ms, crash_stamp, respawning)
        ("fast, clean",                     40.0,             -1.0e30, 0),
        ("just under 70 mph",   (70.0 / IMG_MPH) - eps,       -1.0e30, 0),
        ("exactly 70 mph",       70.0 / IMG_MPH,              -1.0e30, 0),
        ("just over 70 mph",    (70.0 / IMG_MPH) + eps,       -1.0e30, 0),
        ("stationary",                       0.0,             -1.0e30, 0),
        ("crashed 2.99 s ago",              40.0,               37.01, 0),
        ("crashed exactly 3.00 s ago",      40.0,               37.00, 0),
        ("crashed 3.01 s ago",              40.0,               36.99, 0),
        ("respawning",                      40.0,             -1.0e30, 1),
        ("respawning AND fast",            100.0,             -1.0e30, 1),
    ]
    for (name, spd, cst, resp) in cases:
        w = world()
        w.wf(w.pv(1) + 0xBC, spd)
        w.wf(w.rc(1) + 0x1410, cst)
        w.wb(w.rc(1) + 0x18FB, resp)
        w.slam(0, 1, 1.0, 0)
        real = w.score_state(0)

        cworld(C)
        C.cmd("set 1 speed_ms %.9g" % spd)
        C.cmd("set 1 crash_stamp %.9g" % cst)
        C.cmd("set 1 respawning %d" % resp)
        pred = int(C.ask("cheap 1 40.0").split()[1])
        C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
        c = C.dump(0)
        check("cheap %-30s flag" % name, c['slam_cheap'], real['slam_cheap'])
        check("cheap %-30s predicate" % name, pred, real['slam_cheap'])
        check("cheap %-30s BP" % name, c['bp'], real['bp'])

    # THE RACE-START WINDOW: with +0x1410 at its reset value the gate reads
    # `clock < -1.0 + 3.0`, so every slam in the first 2.0 s is a Super Slam.
    for clock in (0.0, 1.0, 1.99, 2.0, 2.5, 10.0):
        w = world(clock=clock)
        w.wf(w.rc(1) + 0x1410, idle)
        w.wf(w.pv(1) + 0xBC, 40.0)
        w.slam(0, 1, 1.0, 0)
        real = w.score_state(0)

        C.seed()
        for i in range(6):
            C.set_boost(i)
            C.cmd("set %d speed_ms 40.0" % i)
        # NOT re-seeded: b3_td_reset must already leave +0x1410 at retail's
        # own -1.0, which is the point of the case.
        pred = int(C.ask("cheap 1 %.6f" % clock).split()[1])
        C.ask("slam %.6f %d 0 1 1.0" % (clock, E.B3K_SIDE))
        c = C.dump(0)
        check("race start t=%-5.2f cheap flag" % clock, c['slam_cheap'],
              real['slam_cheap'])
        check("race start t=%-5.2f predicate" % clock, pred,
              real['slam_cheap'])
        check("race start t=%-5.2f BP" % clock, c['bp'], real['bp'])


# ==========================================================================
# 4. slam BP: 4 types x cheap x burning
# ==========================================================================
def sec4_slam_bp(C):
    print("\n-- 4. slam BP (FUN_0019A050 executed, the real poster)")
    for ty in range(4):
        for cheap in (0, 1):
            for burn in (0, 1):
                w = world(slam_type=ty)
                if cheap:
                    w.wf(w.pv(1) + 0xBC, 5.0)      # under 70 mph
                w.wb(w.rc(0) + 0x11EE, burn)       # attacker boosting
                w.posts = []
                w.slam(0, 1, 1.0, 0)
                real, realv = w.score_state(0), w.score_state(1)
                msg = w.posts[0]['msg'] if w.posts else -1
                bp_posted = w.posts[0]['bp'] if w.posts else -1

                cworld(C)
                if cheap:
                    C.cmd("set 1 speed_ms 5.0")
                C.cmd("set 0 boosting %d" % burn)
                # the port takes the type from the frames; drive it with the
                # geometry that retail's own selector maps to this type.
                am, vm = TYPE_FOR[ty]
                C.set_frame(0, am)
                C.set_frame(1, vm)
                C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
                c, cv = C.dump(0), C.dump(1)
                tag = "t%d cheap%d burn%d" % (ty, cheap, burn)

                want = (IMG['super_bp'][ty] if cheap else IMG['slam_bp'][ty]) \
                       + (IMG['burning_bp'] if burn else 0)
                check("%s retail BP is the table lookup" % tag, real['bp'], want)
                check("%s retail posts the same BP" % tag, bp_posted, want)
                check("%s retail message id" % tag, msg,
                      IMG_MSG_SLAM[ty * 4 + cheap + 2 * burn])
                check("%s C bp" % tag, c['bp'], real['bp'])
                check("%s C bp_aggr (+0x1180)" % tag, c['bp_aggr'],
                      real['bp_aggr'])
                check("%s C bp_td stays 0 (+0x117C)" % tag, c['bp_td'],
                      real['bp_td'])
                check("%s C slam_kind" % tag, c['slam_kind'], real['slam_kind'])
                check("%s C slam_cheap" % tag, c['slam_cheap'],
                      real['slam_cheap'])
                check("%s C slam_burning" % tag, c['slam_burning'],
                      real['slam_burning'])
                check("%s the VICTIM is paid nothing" % tag, cv['bp'],
                      realv['bp'])


# geometry that makes retail's own FUN_00197F90 return each type; filled in
# by sec2's executed run so nothing here is assumed.
TYPE_FOR = {}


def _resolve_type_geometry():
    for (name, am, vm) in TYPE_CASES:
        w = world(slam_type=None)
        w.set_frame(0, am)
        w.set_frame(1, vm)
        t = w.slam_type_call(0, 1) & 0xFF
        TYPE_FOR.setdefault(t, (am, vm))
    missing = [t for t in range(4) if t not in TYPE_FOR]
    if missing:
        raise SystemExit("no seeded geometry produces slam type(s) %s -- the "
                         "taxonomy table needs another case" % missing)


# ==========================================================================
# 5. the attacker's boost gain
# ==========================================================================
def sec5_attacker_boost(C):
    print("\n-- 5. the attacker's boost gain (FUN_001989A0 executed)")
    cases = [
        # (name, tier, mult, bonus, meter, size, scale, party)
        ("tier 0, x1",              0, 1.0, 0.0, 100.0, 720.0, 1.0, 0),
        ("tier 1",                  1, 1.0, 0.0, 100.0, 720.0, 1.0, 0),
        ("tier 2",                  2, 1.0, 0.0, 100.0, 720.0, 1.0, 0),
        ("tier 3",                  3, 1.0, 0.0, 100.0, 720.0, 1.0, 0),
        ("multiplier 1.5",          0, 1.5, 0.0, 100.0, 720.0, 1.0, 0),
        ("bonus 0.5",               0, 1.0, 0.5, 100.0, 720.0, 1.0, 0),
        ("mult 1.2 + bonus 0.3",    0, 1.2, 0.3, 100.0, 720.0, 1.0, 0),
        ("bonus only, mult 0",      0, 0.0, 2.0, 100.0, 720.0, 1.0, 0),
        ("mult 2 + bonus 2",        1, 2.0, 2.0,   0.0, 5000.0, 1.0, 0),
        ("bonus alone at tier 3",   3, 0.0, 1.0,   0.0, 5000.0, 1.0, 0),
        ("context scale 0.5",       0, 1.0, 0.0, 100.0, 720.0, 0.5, 0),
        ("context scale 2.0",       0, 1.0, 0.0, 100.0, 720.0, 2.0, 0),
        ("clamped at the bar top",  0, 1.0, 0.0, 700.0, 720.0, 1.0, 0),
        ("already full",            0, 1.0, 0.0, 720.0, 720.0, 1.0, 0),
        ("small bar, big gain",     0, 1.0, 0.0,   0.0, 100.0, 1.0, 0),
        ("CRASH PARTY: no gain",    0, 1.0, 0.0, 100.0, 720.0, 1.0, 1),
    ]
    for (name, tier, mult, bonus, meter, size, scale, party) in cases:
        w = world(boost_scale=scale, crash_party=party)
        w.set_boost(0, tier=tier, size=size, meter=meter, mult=mult,
                    bonus=bonus)
        w.slam(0, 1, 1.0, 0)
        real = w.score_state(0)

        cworld(C, boost_scale=scale, crash_party=party)
        C.set_boost(0, tier=tier, size=size, meter=meter, mult=mult,
                    bonus=bonus)
        C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
        c = C.dump(0)
        check("gain %-24s meter" % name, c['boost_meter'],
              real['boost_meter'], 1e-3)
        check("gain %-24s earned" % name, c['boost_earned'],
              real['boost_earned'], 1e-3)
        check("gain %-24s size untouched" % name, c['boost_size'],
              real['boost_size'], 1e-6)


# ==========================================================================
# 6. the victim's boost drain
# ==========================================================================
def sec6_victim_boost(C):
    print("\n-- 6. the victim's boost drain (FUN_001989A0 executed)")
    cases = [
        # (name, tier, mult, meter, peg_a, peg_b, boosting, ramp_done, scale,
        #  bonus)
        ("tier 0, x1",            0, 1.0, 500.0, 0, 0, 0, 0, 1.0),
        ("tier 1",                1, 1.0, 500.0, 0, 0, 0, 0, 1.0),
        ("tier 3",                3, 1.0, 500.0, 0, 0, 0, 0, 1.0),
        ("multiplier 2.0",        0, 2.0, 500.0, 0, 0, 0, 0, 1.0),
        ("multiplier 0.25",       0, 0.25, 500.0, 0, 0, 0, 0, 1.0),
        ("drains to the floor",   0, 1.0, 100.0, 0, 0, 0, 0, 1.0),
        ("empty already",         0, 1.0,   0.0, 0, 0, 0, 0, 1.0),
        ("peg A pins the meter",  0, 1.0, 100.0, 1, 0, 0, 0, 1.0),
        ("peg B pins the meter",  0, 1.0, 100.0, 0, 1, 0, 0, 1.0),
        ("both pegs",             0, 1.0, 100.0, 1, 1, 0, 0, 1.0),
        ("emptied WHILE burning", 0, 1.0, 100.0, 0, 0, 1, 0, 1.0),
        ("burning but ramp done", 0, 1.0, 100.0, 0, 0, 1, 1, 1.0),
        ("burning, meter survives", 0, 1.0, 900.0, 0, 0, 1, 0, 1.0),
        ("context scale ignored", 0, 1.0, 500.0, 0, 0, 0, 0, 0.25),
        # lands EXACTLY on zero: separates retail's `<= 0` clamp from a `< 0`
        ("exactly emptied",       0, 1.0, 240.0, 0, 0, 1, 0, 1.0),
        ("exactly emptied, tier 1", 1, 1.0, 120.0, 0, 0, 1, 0, 1.0),
        ("exactly emptied, x2",   0, 2.0, 480.0, 0, 0, 1, 0, 1.0),
        ("one unit above zero",   0, 1.0, 241.0, 0, 0, 1, 0, 1.0),
    ]
    # the victim's own event-bonus multiplier: retail's DRAIN must ignore it
    bonus_cases = [
        ("bonus ignored by the drain", 0, 1.0, 500.0, 0, 0, 0, 0, 1.0, 0.5),
        ("bonus ignored, big",         0, 1.0, 500.0, 0, 0, 0, 0, 1.0, 3.0),
        ("bonus ignored, tier 2",      2, 1.0, 500.0, 0, 0, 0, 0, 1.0, 2.0),
    ]
    for row in cases + bonus_cases:
        name, tier, mult, meter, pa, pb, bo, rd, scale = row[:9]
        bonus = row[9] if len(row) > 9 else 0.0
        w = world(boost_scale=scale)
        w.set_boost(1, tier=tier, size=1000.0, meter=meter, mult=mult,
                    bonus=bonus, peg_a=pa, peg_b=pb, boosting=bo, ramp_done=rd)
        w.slam(0, 1, 1.0, 0)
        real = w.score_state(1)

        cworld(C, boost_scale=scale)
        C.set_boost(1, tier=tier, size=1000.0, meter=meter, mult=mult,
                    bonus=bonus, peg_a=pa, peg_b=pb, boosting=bo, ramp_done=rd)
        C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
        c = C.dump(1)
        check("drain %-26s meter" % name, c['boost_meter'],
              real['boost_meter'], 1e-3)
        check("drain %-26s earned untouched" % name, c['boost_earned'],
              real['boost_earned'], 1e-6)
        check("drain %-26s forced stop" % name, c['boost_forcestop'],
              real['boost_forcestop'])


# ==========================================================================
# 7. the aggressor / victim split
# ==========================================================================
def sec7_split(C):
    print("\n-- 7. aggressor vs victim: every field, both cars")
    cases = [
        # (name, attacker, victim, classes, kind, strength, clock)
        ("human slams AI",   0, 1, [0, 1, 1, 1, 1, 1], E.B3K_SIDE, 1.0, 40.0),
        ("AI slams human",   1, 0, [0, 1, 1, 1, 1, 1], E.B3K_REAR, 0.5, 12.5),
        ("AI slams AI",      2, 3, [0, 1, 1, 1, 1, 1], E.B3K_SIDE, 0.25, 7.0),
        ("human slams human", 0, 1, [0, 0, 1, 1, 1, 1], E.B3K_REAR, 1.0, 55.0),
    ]
    for (name, a, v, classes, kind, st, clock) in cases:
        w = world(classes=classes, clock=clock)
        w.set_boost(a, meter=200.0)
        w.set_boost(v, meter=600.0)
        w.slam(a, v, st, 0 if kind == E.B3K_SIDE else 1)
        ra, rv = w.score_state(a), w.score_state(v)

        cworld(C, classes=classes)
        C.set_boost(a, meter=200.0)
        C.set_boost(v, meter=600.0)
        C.ask("slam %.6f %d %d %d %.6f" % (clock, kind, a, v, st))
        ca, cv = C.dump(a), C.dump(v)
        diff_state("%s attacker" % name, ra, ca, w, SLAM_KEYS)
        diff_state("%s victim" % name, rv, cv, w, SLAM_KEYS)
        # the split itself, asserted against retail's own post-state
        check("%s only the attacker's slams_made moved" % name,
              (ra['slams_made'], rv['slams_made']), (1, 0))
        check("%s only the victim's times_slammed moved" % name,
              (ra['times_slammed'], rv['times_slammed']), (0, 1))
        check("%s only the victim carries the OOC stamp" % name,
              (ra['slam_time'] < 0.0, rv['slam_time']), (True, clock))

    # two slams: the accumulators must add, not latch
    w = world()
    w.set_boost(0, meter=0.0)
    w.set_boost(1, meter=900.0, size=1000.0)
    w.slam(0, 1, 1.0, 0)
    w.wf(w.rc(0) + 0x16C0, -1.0)          # clear the re-slam memory
    w.wf(w.rc(1) + 0x16C0, -1.0)
    w.slam(0, 1, 1.0, 0)
    ra, rv = w.score_state(0), w.score_state(1)

    cworld(C)
    C.set_boost(0, meter=0.0)
    C.set_boost(1, meter=900.0, size=1000.0)
    C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
    C.cmd("set 0 aggressor_time -1.0")
    C.cmd("set 1 aggressor_time -1.0")
    C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
    ca, cv = C.dump(0), C.dump(1)
    diff_state("two slams attacker", ra, ca, w, SLAM_KEYS)
    diff_state("two slams victim", rv, cv, w, SLAM_KEYS)
    check("two slams: retail counted both", ra['slams_made'], 2)
    check("two slams: retail added the energy",
          ra['slam_energy'], 2.0 * IMG['slam_energy'], 1e-3)


# ==========================================================================
# 8. the AI grudge racecar+0x23E0
# ==========================================================================
def sec8_ai_grudge(C):
    print("\n-- 8. the AI grudge +0x23E0 (the only user of slam strength)")
    cases = [
        # (name, classes, a, v, step, cap, pre, strength)
        ("human -> AI, half step", [0, 1, 1, 1, 1, 1], 0, 1, 0.5, 10.0, 0.0, 1.0),
        ("scaled by strength",     [0, 1, 1, 1, 1, 1], 0, 1, 0.5, 10.0, 0.0, 0.25),
        ("accumulates",            [0, 1, 1, 1, 1, 1], 0, 1, 0.5, 10.0, 2.0, 1.0),
        ("clamped at the cap",     [0, 1, 1, 1, 1, 1], 0, 1, 5.0,  3.0, 0.0, 1.0),
        ("exactly at the cap",     [0, 1, 1, 1, 1, 1], 0, 1, 3.0,  3.0, 0.0, 1.0),
        ("AI attacker: no grudge", [0, 1, 1, 1, 1, 1], 2, 1, 0.5, 10.0, 0.0, 1.0),
        ("human victim: no grudge", [0, 0, 1, 1, 1, 1], 0, 1, 0.5, 10.0, 0.0, 1.0),
    ]
    for (name, classes, a, v, step, cap, pre, st) in cases:
        w = world(classes=classes)
        w.wf(w.rc(v) + 0x23F0, step)
        w.wf(w.rc(v) + 0x23F4, cap)
        w.wf(w.rc(v) + 0x23E0, pre)
        w.slam(a, v, st, 0)
        real = w.score_state(v)

        cworld(C, classes=classes)
        C.cmd("set %d ai_aggr_step %.9g" % (v, step))
        C.cmd("set %d ai_aggr_cap %.9g" % (v, cap))
        C.cmd("set %d ai_aggression %.9g" % (v, pre))
        C.ask("slam 40.0 %d %d %d %.6f" % (E.B3K_SIDE, a, v, st))
        c = C.dump(v)
        check("grudge %-26s" % name, c['ai_aggression'],
              real['ai_aggression'], 1e-5)


# ==========================================================================
# 9. slamming a car that is ALREADY WRECKED
# ==========================================================================
def sec9_wrecked_victim(C):
    print("\n-- 9. slamming an already-wrecked car")
    for crashed in (0, 1):
        w = world()
        w.wb(w.rc(1) + 0x18FA, crashed)
        w.set_boost(1, meter=500.0)
        w.slam(0, 1, 1.0, 0)
        ra, rv = w.score_state(0), w.score_state(1)

        cworld(C)
        C.cmd("set 1 crashed %d" % crashed)
        C.set_boost(1, meter=500.0)
        C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
        ca, cv = C.dump(0), C.dump(1)
        diff_state("wrecked=%d attacker" % crashed, ra, ca, w, SLAM_KEYS)
        diff_state("wrecked=%d victim" % crashed, rv, cv, w, SLAM_KEYS)

    # the score is IDENTICAL either way -- a wreck is deduped at the COMMIT
    w0 = world(); w0.set_boost(1, meter=500.0); w0.slam(0, 1, 1.0, 0)
    w1 = world(); w1.wb(w1.rc(1) + 0x18FA, 1)
    w1.set_boost(1, meter=500.0); w1.slam(0, 1, 1.0, 0)
    for k in ('bp', 'boost_meter', 'slams_made', 'slam_cheap'):
        check("retail: a wrecked victim changes nothing (%s)" % k,
              w0.score_state(0)[k], w1.score_state(0)[k])

    # and the commit's dedup, both sides
    for pre in (0, 1):
        w = world()
        w.wb(w.rc(1) + 0x15D6, pre)
        w.stub(E.F_AWARD, 'award', argbytes=16)
        ret = w.commit(0, 1) & 0xFF
        rv = w.score_state(1)
        ra = w.score_state(0)

        cworld(C)
        C.cmd("set 1 td_credited %d" % pre)
        C.cmd("setidx 0 claim 1 10.0")
        evs = C.events(40.0, 0)
        cv, ca = C.dump(1), C.dump(0)
        check("commit dedup pre=%d fires" % pre, 1 if evs else 0, ret)
        check("commit dedup pre=%d victim td_credited" % pre,
              cv['td_credited'], rv['td_credited'])
        check("commit dedup pre=%d attacker td_count" % pre,
              ca['td_count'], ra['td_count'])


# ==========================================================================
# 10. end to end
# ==========================================================================
def sec10_end_to_end(C):
    print("\n-- 10. slam -> out of control -> wreck -> commit -> BP, both sides")
    cases = [
        # (name, cause) : cause is passed to both the record and the C side
        ("wall takedown", dict(wall=1)),
        ("plain takedown", dict()),
    ]
    for (name, cause) in cases:
        # ---- retail ----
        w = world(clock=10.0)
        w.set_boost(0, meter=0.0)
        w.set_boost(1, meter=600.0)
        w.slam(0, 1, 1.0, 0)                          # FUN_001989A0
        after_slam_a = w.score_state(0)
        after_slam_v = w.score_state(1)

        w.set_clock(10.2)
        w.wb(w.rc(1) + 0x18FA, 1)                     # the victim wrecks
        w.attribute(1, w.cause_record(**cause) if cause else 0)
        w.set_clock(11.0)                             # past Race Car Clear Wait
        w.claim_scan(0)
        ra, rv = w.score_state(0), w.score_state(1)
        posts = [(p['msg'], p['bp']) for p in w.posts]

        # ---- the port ----
        cworld(C)
        C.set_boost(0, meter=0.0)
        C.set_boost(1, meter=600.0)
        C.ask("slam 10.0 %d 0 1 1.0" % E.B3K_SIDE)
        ca_slam = C.dump(0)
        cv_slam = C.dump(1)
        C.cmd("set 1 crashed 1")
        C.cmd("crash 10.2 1 %d 0 0 -1" % (1 if cause.get('wall') else 0))
        C.events(11.0, 0)
        ca, cv = C.dump(0), C.dump(1)

        check("%s: slam BP matches" % name, ca_slam['bp'], after_slam_a['bp'])
        check("%s: slam boost matches" % name, ca_slam['boost_meter'],
              after_slam_a['boost_meter'], 1e-3)
        check("%s: victim drain matches" % name, cv_slam['boost_meter'],
              after_slam_v['boost_meter'], 1e-3)
        check("%s: takedown credited" % name, cv['td_credited'],
              rv['td_credited'])
        check("%s: attacker td_count" % name, ca['td_count'], ra['td_count'])
        check("%s: the victim is paid nothing" % name, cv['bp'], rv['bp'])
        check("%s: the takedown BP landed" % name, ca['bp'] > ca_slam['bp'],
              ra['bp'] > after_slam_a['bp'])
        check("%s: total attacker BP" % name, ca['bp'], ra['bp'])
        # the base Takedown BP the WORLD is seeded with (the retail vdb tune
        # the td_rules oracle writes at 0x003F746C), read back out of the
        # emulated image rather than assumed.
        check("%s: retail paid slam + takedown" % name,
              ra['bp'], after_slam_a['bp'] + w.ri(E.P_BP_TAKEDOWN))
        if VERBOSE:
            print("       retail posts: %s" % posts)


# ==========================================================================
# 11. a NEAR MISS must not score as a takedown
# ==========================================================================
def sec11_near_miss(C):
    print("\n-- 11. a NEAR MISS is not a takedown (FUN_00197920 executed)")
    cases = [
        # (name, slammed_at, aggressor_set) -- the arming window is 1.0 s
        ("clean pass, never slammed",         None, 0),
        ("pass 0.5 s after being slammed",    39.5, 1),
        ("pass 2.0 s after being slammed",    38.0, 1),
        ("pass with no aggressor recorded",   39.5, 0),
    ]
    for (name, slam_at, agg) in cases:
        w = world()
        base_a = w.score_state(0)
        base_v = w.score_state(1)
        if slam_at is not None:
            w.wf(w.score(1) + 0x5F0, slam_at)
            w.wi(w.score(1) + 0x5EC, w.rc(0) if agg else 0)
        obj = w.traffic_object(1)
        w.deny_arm(1, obj)
        ra, rv = w.score_state(0), w.score_state(1)

        for k in ('bp', 'bp_aggr', 'bp_td', 'slams_made', 'times_slammed',
                  'boost_meter', 'boost_earned', 'slam_energy',
                  'slammed_energy', 'td_made', 'td_count'):
            check("near miss %-32s retail %s unmoved" % (name, k), rv[k],
                  base_v[k])
            check("near miss %-32s retail attacker %s unmoved" % (name, k),
                  ra[k], base_a[k])

        cworld(C)
        if slam_at is not None:
            C.cmd("set 1 slam_time %.6f" % slam_at
                  if False else "set 1 aggressor_time %.6f" % slam_at)
            C.cmd("set 1 aggressor %d" % (0 if agg else -1))
        before = C.dump(1)
        C.cmd("notify 40.0 1")
        after = C.dump(1)
        for k in ('bp', 'bp_aggr', 'bp_td', 'slams_made', 'times_slammed',
                  'boost_meter', 'boost_earned', 'slam_energy',
                  'slammed_energy', 'td_made', 'td_count'):
            check("near miss %-32s C %s unmoved" % (name, k), after[k],
                  before[k])

    # and a near miss never opens the takedown-claim path
    w = world()
    w.deny_arm(1, w.traffic_object(1))
    for k in range(6):
        check("near miss leaves claim slot %d idle" % k,
              w.rf(w.score(1) + 0x4D8 + 4 * k), -1.0, 1e-6)


# ==========================================================================
# 12. a RUB must not score as a takedown
# ==========================================================================
def sec12_rub(C):
    print("\n-- 12. a RUB is not a takedown (FUN_00029F30 kind 1 executed)")
    for kind in (E.B3K_RUB, E.B3K_WALL_SHUNT):
        w = world()
        w.set_boost(0, meter=100.0)
        w.set_boost(1, meter=500.0)
        base_a, base_v = w.score_state(0), w.score_state(1)
        w.posts = []
        w.slam_entry(kind, 0, 1, 1.0)
        ra, rv = w.score_state(0), w.score_state(1)

        cworld(C)
        C.set_boost(0, meter=100.0)
        C.set_boost(1, meter=500.0)
        C.ask("slam 40.0 %d 0 1 1.0" % kind)
        ca, cv = C.dump(0), C.dump(1)

        tag = "kind %d" % kind
        for k in ('bp', 'bp_aggr', 'bp_td', 'slams_made', 'times_slammed',
                  'boost_meter', 'boost_earned', 'slam_energy',
                  'slammed_energy', 'slam_kind', 'slam_cheap', 'human_slam'):
            check("%s retail attacker %s unmoved" % (tag, k), ra[k], base_a[k])
            check("%s retail victim %s unmoved" % (tag, k), rv[k], base_v[k])
            check("%s C attacker %s" % (tag, k), ca[k], ra[k])
            check("%s C victim %s" % (tag, k), cv[k], rv[k])
        check("%s retail leaves the OOC stamp idle" % tag, rv['slam_time'],
              -1.0, 1e-6)
        check("%s C leaves the OOC stamp idle" % tag, cv['slam_time'],
              rv['slam_time'], 1e-6)
        check("%s retail posts no callout" % tag, len(w.posts), 0)

    # a rub followed by a real slam still pays exactly one slam
    w = world()
    w.slam_entry(E.B3K_RUB, 0, 1, 1.0)
    w.slam_entry(E.B3K_SIDE, 0, 1, 1.0)
    ra = w.score_state(0)
    check("rub then slam: retail counts one slam", ra['slams_made'], 1)

    cworld(C)
    C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_RUB)
    C.ask("slam 40.0 %d 0 1 1.0" % E.B3K_SIDE)
    ca = C.dump(0)
    check("rub then slam: C slams_made", ca['slams_made'], ra['slams_made'])
    check("rub then slam: C bp", ca['bp'], ra['bp'])




# ==========================================================================
# 13. the retail bridge sends the right WIDTH
#
# burnout3_tdcar_ranges.h carries each B3TdCar field into the emulated racecar
# as sizeof(field) bytes at offsetof(field).  Retail touches the flags below
# with BYTE instructions and keeps live, unrelated state in the bytes that
# follow, so a field declared `int` over a retail byte silently overwrites its
# neighbour every time the td_rules=retail backend is selected.
# ==========================================================================
BYTE_FIELDS = [
    # (field, offset, a VA where retail touches it, the instruction bytes)
    ('crashed',        0x18FA, 0x00024BD9, '8a81fa180000'),  # MOV AL,[ECX+..]
    ('respawning',     0x18FB, 0x001709CB, 'c686fb180000'),  # MOV byte [ESI+..],imm8
    ('td_credited',    0x15D6, 0x0002585C, '8a86d6150000'),  # MOV AL,[ESI+..]
    ('td_credited_fx', 0x15D7, 0x00198E94, 'c687d7150000'),  # in FUN_00198E60
    ('revenge_flag',   0x168F, 0x00198F3E, 'c6878f160000'),  # in FUN_00198E60
]
# The neighbours those byte flags sit next to, with the width retail uses:
# each one is a LIVE field an over-wide port declaration would have wiped.
NEIGHBOURS = [
    ('racecar+0x18FC (the ONCOMING flag, FUN_0018D790) is a byte',
     0x0018D89F, '8893fc180000'),                 # MOV byte [EBX+0x18FC],DL
    ('racecar+0x18FB (respawning) is a byte',
     0x00183C40, '8a88fb180000'),                 # MOV CL,[EAX+0x18FB]
    ('racecar+0x1690 (2nd out-of-control clock) is a FLOAT',
     0x0011EDA3, '0f2fa290160000'),               # COMISS XMM4,[EDX+0x1690]
]


def sec13_widths(C):
    print("\n-- 13. the retail bridge's field widths (burnout3_tdcar_ranges.h)")
    C.seed()
    got = {}
    for tok in C.ask("layout").split()[1:]:
        f, off, sz = tok.split(':')
        got[f] = (int(off), int(sz))

    for (f, off, va, enc) in BYTE_FIELDS:
        img = RD(va, len(enc) // 2).hex()
        check("image: retail touches +0x%04X with %s" % (off, enc), img, enc)
        check("port: %s is at +0x%04X" % (f, off), got[f][0], off)
        check("port: %s is ONE byte, like retail" % f, got[f][1], 1)

    for (name, va, enc) in NEIGHBOURS:
        check("image: %s" % name, RD(va, len(enc) // 2).hex(), enc)

    # every remaining flag this module carries must be a byte too
    for f in ('psyche_armed', 'boost_peg_a', 'boost_peg_b', 'boosting',
              'boost_forcestop', 'boost_ramp_done', 'slam_cheap',
              'slam_burning', 'human_slam'):
        check("port: %s is one byte" % f, got[f][1], 1)

    # and no two of them may overlap
    spans = sorted((o, o + w, f) for f, (o, w) in got.items())
    for i in range(1, len(spans)):
        check("port: %s does not squat on %s" % (spans[i - 1][2], spans[i][2]),
              spans[i - 1][1] <= spans[i][0], True)

# ==========================================================================
SECTIONS = [
    ('1', sec1_constants), ('2', sec2_type), ('3', sec3_cheap),
    ('4', sec4_slam_bp), ('5', sec5_attacker_boost),
    ('6', sec6_victim_boost), ('7', sec7_split), ('8', sec8_ai_grudge),
    ('9', sec9_wrecked_victim), ('10', sec10_end_to_end),
    ('11', sec11_near_miss), ('12', sec12_rub),
    ('13', sec13_widths),
]


def main():
    want = [a for a in sys.argv[1:] if not a.startswith('-')]
    C = CDriver()
    _resolve_type_geometry()
    for (name, fn) in SECTIONS:
        if want and name not in want:
            continue
        fn(C)
    print("\n================ %d/%d ================" % (PASS, PASS + FAIL))
    if FAIL:
        print("\nFAILURES:")
        for f in FAILURES:
            print("  " + f)
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
