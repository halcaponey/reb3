#!/usr/bin/env python3
"""Ground truth for the Burnout 3 AI AVOIDANCE stage: runs the REAL x86 of
the profile-frame / strip-binning / risk-query functions under Unicorn.

The avoidance sub-object is `AI+0x2B0` == `racecar+0x1CB0` (docs/RE_AI.md
section 15.1).  This module lays out the *route* data the stage actually
reads -- which is the thing the port was missing -- so the frame functions
can be executed rather than guessed:

    DAT_0060EA2C[]   section-list table, stride 8, entry+4 -> list header
    header+0x00      -> u16 pairs (leftVertIdx, rightVertIdx) per node
    header+0x04      -> per-node cumulative arc length (stride 8, float)
    header+0x08      -> nodes, stride 10
    header+0x0C      u16 node count
    header+0x0E      u8  wrap flag
    node+3           flags; (&7)==4 hard no-go, (&7)==5 soft no-go / branch
    node+4/+6        u8 link-A list id / u16 link-A node index (0xFF = none)
    node+5/+8        u8 link-B list id / u16 link-B node index
    DAT_0073A174     base of the vec4 road-edge vertex pool (stride 0x10)

    racecar+0x18C4   -> &DAT_0060EA2C[id]   (current section list)
    racecar+0x18C8   u16 current node index
    racecar+0x18CC   float fraction within the node span  [0,1]
    racecar+0x1AF0   road forward unit vec4
    racecar+0x1B00   road right   unit vec4
    racecar+0x2444 / +0x2448   the car's own extents used for the footprint

Functions executed here (analyzed burnout3.elf, .text = flat + 0x10000):

  0x00170260  build the profile FRAME  -> avoid+0x00 left edge, +0x10 right
              edge, +0x20 car position, +0x30 right->left unit axis,
              +0x444 road width, +0x448/+0x44C the car's lateral coords
  0x00170100  the car's own FOOTPRINT band -> avoid+0x440 count,
              +0x441 high strip, +0x442 low strip
  0x0016F400  paint one cross-section into the strips (the world->strip map)
  0x0016FCD0  the risk query at a strip -> mean clearance, side total, max
  0x0016F000  the clear-path aim strip (smoothed clearance scan)

Run standalone for a smoke test:  python3 tools/emulate_ai_avoid.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UcError, UC_PROT_ALL)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP,
                               UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_XMM0)

import emulate_vehicle as ev                                  # noqa: E402

ELF = ev.ELF
PAGE = 0x1000

# --- memory map (well clear of the 0x10000..0x778200 image) -----------------
STACK_BASE = 0x20000000
STACK_SIZE = 0x00100000
VEH = 0x30000000            # physics vehicle  (racecar+0x2440 -> here)
VEH_SZ = 0x4000
RC = 0x40000000             # racecar; AI = RC+0x1A00, AVOID = AI+0x2B0
RC_SZ = 0x10000
FRAME = 0x60000000          # the car's transform behind vehicle+0x204
FRAME_SZ = 0x1000
ROUTE = 0x80000000          # section table + headers + nodes + arc lengths
ROUTE_SZ = 0x00040000
VERTS = 0x88000000          # the vec4 road-edge vertex pool
VERTS_SZ = 0x00040000
MAGIC_RET = 0x50000000

AI = RC + 0x1A00
AV = AI + 0x2B0             # == RC + 0x1CB0

# --- function addresses ----------------------------------------------------
F_FRAME = 0x00170260        # profile frame  (avoid+0x00..0x44C)
F_BAND = 0x00170100         # own footprint band (avoid+0x440..0x442)
F_PAINT = 0x0016F400        # paint a cross-section into the strips
F_QUERY = 0x0016FCD0        # risk query at a strip
F_AIMSTRIP = 0x0016F000     # clear-path aim strip

# --- globals ---------------------------------------------------------------
G_SECTBL = 0x0060EA2C       # the section-list table base
G_VERTPOOL = 0x0073A174     # pointer to the road-edge vertex pool
G_STRIPS_PER_M = 0x005A96EC  # strips per metre (5.0 from DAT_003B1694)
G_ONEVEC = 0x0040A8C0       # the (1,1,1,1)-ish constant block 00170260 falls
                            # back on when the two edge points coincide

# --- the registered AI/Avoidance tune block (docs/RE_AI.md section 1) ------
P_LOOKAHEAD_RC = 0x0047A17C   # AVOID: LookAhead dist racecars
P_SOFT_NOGO_T = 0x0047A180    # AVOID: Soft No Go offset time
P_SOFT_NOGO_D = 0x0047A184    # AVOID: Soft No Go offset distance
P_HARD_NOGO_T = 0x0047A188    # Hard No Go offset time
P_HARD_NOGO_D = 0x0047A18C    # Hard No Go offset distance
P_DISCARD_D = 0x0047A190      # Distance to discard fatally colliding racecar
P_DISCARD_V = 0x0047A194      # Vert dist to discard traffic and racecars
P_SWEEP_DT = 0x0047A198       # dt between start and end vehicle
P_SPD_10 = 0x0047A19C         # Speed when car is <10m away
P_SPD_20 = 0x0047A1A0         # Speed when car is <20m away
P_SPD_30 = 0x0047A1A4         # Speed when car is <30m away
P_SPD_R95 = 0x0047A1A8        # Speed when risk is >0.95
P_SPD_R90 = 0x0047A1AC        # Speed when risk is >0.9
P_STEER_F = 0x0047A1B0        # Steering factor big=>extreme
P_EXTRA_D = 0x0047A1B4        # Extra softNoGo offset dist for future
P_EXTRA_T = 0x0047A1B8        # Extra softNoGo offset time for future
P_AGG_TF = 0x0047A1BC         # Aggression time variation factor
P_AGG_DF = 0x0047A1C0         # Aggression dist variation factor
P_AGG_TO = 0x0047A1C4         # Aggression time variation offset
P_AGG_DO = 0x0047A1C8         # Aggression dist variation offset

# retail Data/vdb.xml column, docs/RE_AI.md section 1
VDB = {
    P_LOOKAHEAD_RC: 20.0, P_SOFT_NOGO_T: 2.5, P_SOFT_NOGO_D: 101.0,
    P_HARD_NOGO_T: 0.0, P_HARD_NOGO_D: 200.0, P_DISCARD_D: 100.0,
    P_DISCARD_V: 5.0, P_SWEEP_DT: 0.2, P_SPD_10: 26.2, P_SPD_20: 40.0,
    P_SPD_30: 60.0, P_SPD_R95: 16.0, P_SPD_R90: 30.0, P_STEER_F: 5.1,
    P_EXTRA_D: 5.0, P_EXTRA_T: 0.08, P_AGG_TF: 3.08, P_AGG_DF: 10.0,
    P_AGG_TO: 0.0, P_AGG_DO: 10.0,
}

STRIPS_PER_M = 5.0
N_STRIPS = 256
TMAX = 8.0                  # u8 0xFF * 8/255
DMAX = 500.0                # s16 32767 * 1000/65536
T_SCALE = 8.0 / 255.0
D_SCALE = 1000.0 / 65536.0

# avoid-object field offsets
AV_LEFT = 0x000             # left road-edge point  (vec4)
AV_RIGHT = 0x010            # right road-edge point (vec4)
AV_CAR = 0x020              # the car's world position (vec4)
AV_AXIS = 0x030             # normalized right->left axis, y forced to 0
AV_TYPE = 0x040             # u8 [256]
AV_TIME = 0x140             # u8 [256]
AV_DIST = 0x240             # s16[256]
AV_WIN = 0x440              # u8  footprint strip count
AV_HI = 0x441               # u8  footprint high strip
AV_LO = 0x442               # u8  footprint low strip
AV_WIDTH = 0x444            # f32 road width at the node
AV_LATL = 0x448             # f32 the car's distance from the LEFT edge
AV_LATR = 0x44C             # f32 the car's distance from the RIGHT edge
AV_AIM = 0x460
AV_DIR = 0x470
AV_TT = 0x484
AV_SPEED = 0x488
AV_SPD_GATE = 0x48C         # the 50.0 gate that drops type-6 out of dmin
AV_RISK_MEAN_R = 0x490
AV_RISK_MEAN_L = 0x494
AV_RISK_TOT_R = 0x498
AV_RISK_TOT_L = 0x49C
AV_AI = 0x4A0
AV_RC2 = 0x4A4
AV_STATE = 0x4AC
AV_PHASE = 0x4AF
AV_NAV = 0x454


def f2u(f):
    return struct.unpack('<I', struct.pack('<f', float(f)))[0]


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xFFFFFFFF))[0]


class Session(object):
    """One Unicorn instance with racecar / avoid object / route laid out."""

    def __init__(self, params=None):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        ev.load_elf(uc, ELF)
        for base, size in ((STACK_BASE, STACK_SIZE), (VEH, VEH_SZ),
                           (RC, RC_SZ), (FRAME, FRAME_SZ),
                           (ROUTE, ROUTE_SZ), (VERTS, VERTS_SZ),
                           (MAGIC_RET & ~(PAGE - 1), PAGE)):
            uc.mem_map(base, size, UC_PROT_ALL)
        self.uc = uc
        self.fault = None
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)

        for i, v in enumerate([1, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 1, 0, 0, 0, 0, 1]):
            self.wf(FRAME + i * 4, float(v))

        self.wu(VEH + 0x204, FRAME)
        self.wu(RC + 0x2440, VEH)
        self.wu(RC + 0x1A00, AI)
        self.wu(RC + 0x1A04, RC)
        self.wu(AV + AV_NAV, RC)
        self.wu(AV + AV_AI, AI)
        self.wu(AV + AV_RC2, RC)
        self.wu(AI + 0x760, RC)
        self.wu(AI + 0x7A0, RC)

        self.wf(G_STRIPS_PER_M, STRIPS_PER_M)
        self.wu(G_VERTPOOL, VERTS)
        for i, v in enumerate((1.0, 1.0, 1.0, 1.0)):
            self.wf(G_ONEVEC + i * 4, v)

        for a, v in (params if params is not None else VDB).items():
            self.wf(a, v)

        self._vn = 0        # next free vertex slot
        self._scratch = None
        self._scratch_node = 50
        self._ra = ROUTE + 0x100   # next free route allocation

    # --- memory helpers ---
    def _unmapped(self, uc, access, address, size, value, user):
        if self.fault is None:
            self.fault = "unmapped 0x%08X @ EIP 0x%08X" % (
                address, uc.reg_read(UC_X86_REG_EIP))
        return False

    def wf(self, a, v):
        self.uc.mem_write(a, struct.pack('<f', float(v)))

    def wu(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', int(v) & 0xFFFFFFFF))

    def wh(self, a, v):
        self.uc.mem_write(a, struct.pack('<H', int(v) & 0xFFFF))

    def ws(self, a, v):
        self.uc.mem_write(a, struct.pack('<h', int(v)))

    def wb(self, a, v):
        self.uc.mem_write(a, bytes([int(v) & 0xFF]))

    def wv(self, a, vec):
        for i, c in enumerate(vec):
            self.wf(a + i * 4, c)

    def rf(self, a):
        return struct.unpack('<f', self.uc.mem_read(a, 4))[0]

    def ru(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def rh(self, a):
        return struct.unpack('<H', self.uc.mem_read(a, 2))[0]

    def rs(self, a):
        return struct.unpack('<h', self.uc.mem_read(a, 2))[0]

    def rb(self, a):
        return self.uc.mem_read(a, 1)[0]

    def rv(self, a, n=4):
        return [self.rf(a + i * 4) for i in range(n)]

    def alloc(self, n):
        a = self._ra
        self._ra = (self._ra + n + 15) & ~15
        return a

    # --- route construction -------------------------------------------
    def add_vert(self, p):
        """Append a vec4 to the road-edge vertex pool; return its index."""
        i = self._vn
        self.wv(VERTS + i * 0x10, (p[0], p[1], p[2], 0.0))
        self._vn += 1
        return i

    def build_section(self, list_id, edges, flags=None, wrap=0, arc=None):
        """Lay out one section list.

        edges: [(leftPoint, rightPoint), ...] one pair per node.
        flags: per-node flag byte (bit 0..2: 4 = hard no-go, 5 = soft no-go).
        Returns the entry address (what racecar+0x18C4 points at).
        """
        n = len(edges)
        recs = self.alloc(4 * (n + 1))
        nodes = self.alloc(10 * (n + 1))
        arcs = self.alloc(8 * (n + 1))
        hdr = self.alloc(0x10)
        for i, (lp, rp) in enumerate(edges):
            self.wh(recs + i * 4 + 0, self.add_vert(lp))
            self.wh(recs + i * 4 + 2, self.add_vert(rp))
        # one extra record so node n-1 can read its "+4/+6" successor
        self.wh(recs + n * 4 + 0, self.rh(recs + (n - 1) * 4 + 0))
        self.wh(recs + n * 4 + 2, self.rh(recs + (n - 1) * 4 + 2))
        for i in range(n + 1):
            base = nodes + i * 10
            self.wb(base + 3, (flags or [0] * n)[i] if i < n else 0)
            self.wb(base + 4, 0xFF)
            self.wb(base + 5, 0xFF)
            self.wh(base + 6, 0)
            self.wh(base + 8, 0)
            self.wf(arcs + i * 8, (arc or [float(i)] * (n + 1))[i]
                    if arc else float(i))
        self.wu(hdr + 0x00, recs)
        self.wu(hdr + 0x04, arcs)
        self.wu(hdr + 0x08, nodes)
        self.wh(hdr + 0x0C, n)
        self.wb(hdr + 0x0E, wrap)
        entry = G_SECTBL + list_id * 8
        self.wu(entry + 4, hdr)
        return entry

    def straight_road(self, list_id=0, n=64, width=16.0, spacing=10.0,
                      y=0.0, flags=None):
        """A straight road along +Z, centre on x=0, right edge at -width/2.

        Returns the section entry address.  Retail's axis runs right->left,
        so with +Z forward and +X right the axis comes out as -X.
        """
        edges = []
        for i in range(n):
            z = i * spacing
            edges.append(((-width * 0.5, y, z), (width * 0.5, y, z)))
        return self.build_section(list_id, edges, flags=flags)

    def seed_car(self, pos=(0.0, 0.0, 0.0), fwd=(0.0, 0.0, 1.0),
                 right=(1.0, 0.0, 0.0), node=0, t=0.0, entry=None,
                 car_len=4.5, car_wid=2.0, speed=40.0):
        up = (0.0, 1.0, 0.0)
        self.wv(FRAME + 0x00, right + (0.0,))
        self.wv(FRAME + 0x10, up + (0.0,))
        self.wv(FRAME + 0x20, fwd + (0.0,))
        self.wv(FRAME + 0x30, pos + (0.0,))
        self.wv(RC + 0x1AF0, fwd + (0.0,))
        self.wv(RC + 0x1B00, right + (0.0,))
        # +0x2444 is the car WIDTH and +0x2448 the car LENGTH, both FULL
        # extents.  docs/RE_AI.md 14.4 pins them from unrelated retail
        # functions (+0x2448 x 0.5 = "half the car length" in FUN_00169D70;
        # +0x2444 is compared against the LATERAL offset +0x38 in
        # FUN_0016A620), and FUN_00170100 executed with asymmetric values
        # agrees: aligned to the road only +0x2444 moves the band (x0.5), at
        # 90 degrees only +0x2448 does (x0.3).  These two writes used to be
        # the other way round, which hid the same swap in the port's caller
        # from the differential suite.
        self.wf(RC + 0x2444, car_wid)
        self.wf(RC + 0x2448, car_len)
        self.wh(RC + 0x18C8, node)
        self.wf(RC + 0x18CC, t)
        if entry is not None:
            self.wu(RC + 0x18C4, entry)
        self.wf(VEH + 0xBC, speed)

    def clear_profile(self):
        for i in range(N_STRIPS):
            self.wb(AV + AV_TYPE + i, 0)
            self.wb(AV + AV_TIME + i, 0xFF)
            self.ws(AV + AV_DIST + i * 2, 32767)

    def profile(self):
        return ([self.rb(AV + AV_TYPE + i) for i in range(N_STRIPS)],
                [self.rb(AV + AV_TIME + i) * T_SCALE for i in range(N_STRIPS)],
                [self.rs(AV + AV_DIST + i * 2) * D_SCALE
                 for i in range(N_STRIPS)])

    # --- call ---
    def call(self, addr, regs=None, stack_args=(), max_steps=2000000):
        uc = self.uc
        self.fault = None
        sp = STACK_BASE + STACK_SIZE - 0x8000
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r in (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                  UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI):
            uc.reg_write(r, AV)
        for r, v in (regs or {}).items():
            uc.reg_write(r, v & 0xFFFFFFFF)
        try:
            uc.emu_start(addr, MAGIC_RET, count=max_steps)
        except UcError as e:
            if self.fault is None:
                self.fault = "%s @ 0x%08X" % (e, uc.reg_read(UC_X86_REG_EIP))
        return u2f(uc.reg_read(UC_X86_REG_XMM0) & 0xFFFFFFFF)

    # --- the five stage entry points ----------------------------------
    def build_frame(self):
        """FUN_00170260 -> avoid+0x00/0x10/0x20/0x30/0x444/0x448/0x44C."""
        self.call(F_FRAME, {UC_X86_REG_EBX: AV})
        return dict(left=self.rv(AV + AV_LEFT), right=self.rv(AV + AV_RIGHT),
                    car=self.rv(AV + AV_CAR), axis=self.rv(AV + AV_AXIS),
                    width=self.rf(AV + AV_WIDTH),
                    lat_left=self.rf(AV + AV_LATL),
                    lat_right=self.rf(AV + AV_LATR))

    def build_band(self):
        """FUN_00170100 -> avoid+0x440 count, +0x441 hi, +0x442 lo."""
        self.call(F_BAND, {UC_X86_REG_ESI: AV})
        return dict(win=self.rb(AV + AV_WIN), hi=self.rb(AV + AV_HI),
                    lo=self.rb(AV + AV_LO))

    def paint(self, entry, node, kind, time, dist):
        """FUN_0016F400: stamp the cross-section of `node` into the strips."""
        self.call(F_PAINT, {UC_X86_REG_EAX: entry, UC_X86_REG_EDX: node,
                            UC_X86_REG_EBX: AV},
                  stack_args=(kind & 0xFF, f2u(time), f2u(dist)))

    def paint_strips(self, lo, hi, kind, time, dist):
        """Stamp an exact strip span by executing FUN_0016F400.

        The strips come out of geometry, so this solves the world->strip map
        backwards and installs the two solved edge points on a scratch node
        (both that node and its successor, so the section fraction drops
        out of the lerp).  The stamp itself is still retail's.
        """
        if self._scratch is None:
            self._scratch = (self.add_vert((0.0, 0.0, 0.0)),
                             self.add_vert((0.0, 0.0, 0.0)))
        vl, vr = self._scratch
        node = self._scratch_node
        recs = self.ru(self.ru(self.ru(RC + 0x18C4) + 4) + 0x00)
        for n in (node, node + 1):
            self.wh(recs + n * 4 + 0, vl)
            self.wh(recs + n * 4 + 2, vr)
        # strip(p) = (int)((dot(p - right, axis) - lat_right) * 5) + 128
        right = self.rv(AV + AV_RIGHT, 3)
        axis = self.rv(AV + AV_AXIS, 3)
        latr = self.rf(AV + AV_LATR)

        def solve(strip):
            # retail converts with plain TRUNCATION toward zero (measured
            # over u = -24.9 .. +24.6), so nudge away from zero to land
            # unambiguously inside the requested strip.
            k = strip - N_STRIPS // 2
            u = (k + (0.5 if k >= 0 else -0.5)) / STRIPS_PER_M + latr
            return tuple(right[i] + axis[i] * u for i in range(3))

        self.wv(VERTS + vl * 0x10, solve(hi) + (0.0,))
        self.wv(VERTS + vr * 0x10, solve(lo) + (0.0,))
        self.paint(self.ru(RC + 0x18C4), node, kind, time, dist)

    def query(self, strip):
        """FUN_0016FCD0(strip) -> (mean clearance, side total, max)."""
        out = self.alloc(0x30)
        for i in range(3):
            self.wf(out + i * 4, 0.0)
        self.call(F_QUERY, {UC_X86_REG_EAX: 0, UC_X86_REG_EDX: strip,
                            UC_X86_REG_ECX: AV},
                  stack_args=(out, out + 4, out + 8))
        return self.rf(out), self.rf(out + 4), self.rf(out + 8)

    def aim_strip(self):
        """FUN_0016F000 -> the clear-path aim strip index."""
        self.call(F_AIMSTRIP, {UC_X86_REG_EAX: AV}, stack_args=(AV,))
        from unicorn.x86_const import UC_X86_REG_EAX as _E
        return self.uc.reg_read(_E) & 0xFFFFFFFF


def main():
    s = Session()
    entry = s.straight_road(width=16.0, n=64, spacing=10.0)
    s.seed_car(pos=(3.0, 0.0, 5.0), node=0, t=0.5, entry=entry)
    fr = s.build_frame()
    print("fault:", s.fault)
    print("frame:", {k: [round(x, 4) for x in v] if isinstance(v, list)
                     else round(v, 4) for k, v in fr.items()})
    bd = s.build_band()
    print("band :", bd, "fault:", s.fault)
    s.clear_profile()
    s.paint(entry, 10, 6, 1.25, 42.0)
    ty, ti, di = s.profile()
    hit = [i for i in range(N_STRIPS) if ty[i]]
    print("painted strips:", (hit[0], hit[-1]) if hit else None,
          "t=%.3f d=%.2f" % (ti[hit[0]], di[hit[0]]) if hit else "")
    print("query(128):", [round(x, 4) for x in s.query(128)])


if __name__ == '__main__':
    main()
