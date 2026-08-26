#!/usr/bin/env python3
"""
GOLD STANDARD for car-vs-car collision: execute the retail chain under
Unicorn over two synthetic-but-faithfully-seeded vehicle structs.

Functions driven here (corrected ELF mapping, build/burnout3.elf):

  FUN_0010A9D0  build the hull-query context, then FUN_0010ABC0/FUN_0010AC20
                (convex hull narrow phase)  -> contact point/normal + the
                per-body separation written back into ctx+0x60 / ctx+0x150
  FUN_0010F8D0  the two-body contact impulse
  FUN_001121F0  racer-vs-racer response (both cars un-crashed)
  FUN_00113960  car-vs-crashed-car response
  FUN_001205E0  force routing (linear-only vs at-the-contact-point)

Stubs (patched over the real entry, all outside the ported logic):
  FUN_0010DCA0  crash-state entry            -> xor eax,eax; ret 0xC
  FUN_00141700  slam sound cue               -> xor eax,eax; ret 8
  game ctx vtable +0x64  slam/BP reporter    -> mov al,1; ret 0x10

Also:  --extract-hulls     copy each pveh/*.bgv's 0x600-byte collision hull
                           record (file +0x1060, the one FUN_00122C20 copies
                           into veh+0x220) to build/cars/<CLS>_<CarN>.hull

Usage:
  python3 tools/emulate_carcol.py --extract-hulls
  python3 tools/emulate_carcol.py --selftest
"""
import glob
import math
import os
import struct
import sys

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_CODE, UC_PROT_ALL, UcError)
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX,
                               UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI)

import os as _os, sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
from b3_paths import game_path, game_root  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF = os.path.join(ROOT, "build", "burnout3.elf")
GAME = game_root()

PAGE = 0x1000

# --- real function addresses ------------------------------------------------
F_A9D0   = 0x0010A9D0    # hull query context + narrow phase
F_F8D0   = 0x0010F8D0    # two-body contact impulse
F_1121F0 = 0x001121F0    # racer vs racer
F_113960 = 0x00113960    # car vs crashed car
F_1205E0 = 0x001205E0    # force routing
F_112E70 = 0x00112E70    # car vs a LIVE traffic car (a type-3 object)
F_FCE0   = 0x0010FCE0    # 2-D segment/segment closest points
F_DCA0   = 0x0010DCA0    # crash entry (stubbed)
F_E580   = 0x0010E580    # the other crash entry (stubbed)
F_114910 = 0x00114910    # traffic object -> real vehicle (hooked, see below)
F_141700 = 0x00141700    # sound cue (stubbed)
D_73BB8C = 0x0073BB8C    # the "designated" marker FUN_0010FBC0 compares

G_D5370  = 0x004D5370    # game root pointer

# --- synthetic memory -------------------------------------------------------
VEH_A     = 0x30000000
VEH_B     = 0x30010000
VEH_SZ    = 0x3000
FRAME_A   = 0x30020000
FRAME_B   = 0x30020100
OBJ_A     = 0x30021000
OBJ_B     = 0x30021100
PAIR      = 0x30022000
RACE_A    = 0x30023000
RACE_B    = 0x30024000
GAMEROOT  = 0x30025000
GAMEOBJ   = 0x30025200
VTABLE    = 0x30025400
STUB_SLAM = 0x30025600
CTXBUF    = 0x30026000        # hull query context (0x210)
SCRATCH   = 0x30028000
STUB_RUB  = 0x30029000        # game ctx vtable +0x54 (the type-3 rub report)
STUB_MODE = 0x30029100        # game ctx vtable +0x90 (game mode)
STUB_SUB  = 0x30029200        # game ctx vtable +0x94 (sub mode)
TRAFFIC_B = 0x30030000        # a TRAFFIC CAR record (frame at +0x70)
MODEL_B   = 0x30040000        # its model: box +0xE80/+0xE90, hull +0x1060
REGION_LO = 0x30000000
REGION_SZ = 0x80000

STACK_BASE = 0x20000000
STACK_SIZE = 0x100000
MAGIC_RET  = 0x50000000

HULL_OFF = 0x220              # veh+0x220 (FUN_00122830)
HULL_SZ  = 0x600
BGV_HULL_OFF = 0x1060


def f2b(v):
    return struct.pack('<f', v)


def load_elf(uc, path):
    data = open(path, 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        t, off, va, _, fsz, msz, _, _ = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if t == 1:
            segs.append((va, off, fsz, msz))
    lo = min(s[0] for s in segs) & ~(PAGE - 1)
    hi = max(s[0] + s[3] for s in segs)
    hi = (hi + PAGE - 1) & ~(PAGE - 1)
    uc.mem_map(lo, hi - lo, UC_PROT_ALL)
    for va, off, fsz, _ in segs:
        uc.mem_write(va, data[off:off + fsz])
    return lo, hi


def _fault(uc, access, address, size, value, user):
    try:
        uc.mem_map(address & ~(PAGE - 1), PAGE, UC_PROT_ALL)
    except UcError:
        return False
    return True


# ---------------------------------------------------------------------------
# matrix helpers (mirroring src/burnout3_vehicle_sim.c's conventions)
# ---------------------------------------------------------------------------
def frame_from(yaw, pos, pitch=0.0):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    right = [cy, 0.0, -sy, 0.0]
    up = [sy * sp, cp, cy * sp, 0.0]
    at = [sy * cp, -sp, cy * cp, 0.0]
    return [right, up, at, [pos[0], pos[1], pos[2], 1.0]]


def invert_rigid(m):
    r = [row[:] for row in m]
    r[0][1], r[1][0] = r[1][0], r[0][1]
    r[0][2], r[2][0] = r[2][0], r[0][2]
    r[1][2], r[2][1] = r[2][1], r[1][2]
    p = [m[3][0] * r[0][j] + m[3][1] * r[1][j] + m[3][2] * r[2][j]
         for j in range(4)]
    r[3] = [-x for x in p]
    return r


def write_mat(uc, addr, m):
    for i, row in enumerate(m):
        uc.mem_write(addr + i * 16, b''.join(f2b(x) for x in row))


def read_vec(uc, addr, n=4):
    return list(struct.unpack('<%df' % n, uc.mem_read(addr, 4 * n)))


# ---------------------------------------------------------------------------
class Session:
    """One Unicorn instance with two seeded vehicles."""

    def __init__(self):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        load_elf(self.uc, ELF)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, _fault)
        self.uc.mem_map(REGION_LO, REGION_SZ, UC_PROT_ALL)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        self.uc.mem_map(MAGIC_RET & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        self.uc.mem_write(REGION_LO, b'\0' * REGION_SZ)

        # stubs
        self.uc.mem_write(F_DCA0, b'\x31\xC0\xC2\x0C\x00')       # xor eax,eax; ret 0xC
        self.uc.mem_write(F_141700, b'\x31\xC0\xC2\x08\x00')     # xor eax,eax; ret 8
        self.uc.mem_write(STUB_SLAM, b'\xB0\x01\xC2\x10\x00')    # mov al,1; ret 0x10
        self.uc.mem_write(VTABLE + 0x64, struct.pack('<I', STUB_SLAM))
        self.uc.mem_write(GAMEOBJ + 0x00, struct.pack('<I', VTABLE))
        self.uc.mem_write(GAMEROOT + 0x1B8, struct.pack('<I', GAMEOBJ))
        self.uc.mem_write(G_D5370, struct.pack('<I', GAMEROOT))
        # DAT_004A52B3 selects the OBB path in FUN_00113960; retail value 0.
        self.uc.mem_write(0x004A52B3, b'\x00')

        # FUN_0010E580 -- the non-car crash entry, no stack args.
        self.uc.mem_write(F_E580, b'\xC3')
        # game ctx vtable +0x54: FUN_00112E70's rub report (2 args).
        self.uc.mem_write(STUB_RUB, b'\xB0\x01\xC2\x08\x00')
        self.uc.mem_write(VTABLE + 0x54, struct.pack('<I', STUB_RUB))
        # FUN_00017310's mode test: vtable +0x90 == 6, or +0x94 in {3,4,5}.
        self.set_game_mode(0, 0)
        self.uc.mem_write(VTABLE + 0x90, struct.pack('<I', STUB_MODE))
        self.uc.mem_write(VTABLE + 0x94, struct.pack('<I', STUB_SUB))

        # FUN_00114910 -- the traffic-object PROMOTION.  Retail takes a real
        # 0x2430 vehicle record out of the collision world's own pool and
        # FUN_00120BA0 seeds it from the traffic record; here the harness owns
        # the record (VEH_B), so the hook performs exactly the seeding the
        # retail path does -- frame from rec+0x70, veh+0xBC = rec+0xC4,
        # veh+0xB0 = at * speed (0x00120DE3..0x00120E3C) -- and relinks the
        # object handle: type 3 -> 4, +0x04 -> the new frame, +0x08 -> the new
        # +0x1D0 box, +0x0C -> the new vehicle.  FUN_00113960 then runs for
        # real over it, which is where the promoted car gets its impulse.
        self.uc.mem_write(F_114910, b'\xC2\x18\x00')          # ret 0x18
        self.promotions = []
        self.traffic_seed = None
        def _promote(uc, addr, size, user):
            sp = uc.reg_read(UC_X86_REG_ESP)
            obj, marker = struct.unpack('<II', uc.mem_read(sp + 4, 8))
            self.promotions.append(obj)
            self._promote_object(obj, marker & 0xFF)
        self.uc.hook_add(UC_HOOK_CODE, _promote, begin=F_114910, end=F_114910)

        # record which vehicle FUN_0010DCA0 (crash entry) is fired for
        self.crashed_vehs = []
        def _dca0(uc, addr, size, user):
            sp = uc.reg_read(UC_X86_REG_ESP)
            self.crashed_vehs.append(
                struct.unpack('<I', uc.mem_read(sp + 8, 4))[0])
        self.uc.hook_add(UC_HOOK_CODE, _dca0, begin=F_DCA0, end=F_DCA0)

        # record the game-context virtual +0x64 slam report
        # (type, attacker_veh, victim_veh, strength) -- FUN_001989A0's caller
        self.slams = []
        def _slam(uc, addr, size, user):
            sp = uc.reg_read(UC_X86_REG_ESP)
            a = struct.unpack('<4I', uc.mem_read(sp + 4, 16))
            self.slams.append((a[0], a[1], a[2],
                               struct.unpack('<f', struct.pack('<I', a[3]))[0]))
        self.uc.hook_add(UC_HOOK_CODE, _slam, begin=STUB_SLAM, end=STUB_SLAM)

    # -- seeding ------------------------------------------------------------
    def seed(self, slot, st):
        """slot 0 -> A, 1 -> B.  st is a dict (see CASES)."""
        uc = self.uc
        veh = VEH_A if slot == 0 else VEH_B
        frame = FRAME_A if slot == 0 else FRAME_B
        obj = OBJ_A if slot == 0 else OBJ_B
        race = RACE_A if slot == 0 else RACE_B

        uc.mem_write(veh, b'\0' * VEH_SZ)
        m = st['frame']
        write_mat(uc, frame, m)
        inv = invert_rigid(m)
        write_mat(uc, veh + 0x70, inv)

        # world inverse inertia rows (+0x40/+0x50/+0x60)
        ii = st.get('inv_inertia', [[1.0 / 900, 0, 0, 0],
                                    [0, 1.0 / 1800, 0, 0],
                                    [0, 0, 1.0 / 1600, 0]])
        for i in range(3):
            uc.mem_write(veh + 0x40 + i * 16,
                         b''.join(f2b(x) for x in ii[i]))

        vel = st.get('vel', [0, 0, 0])
        spd = math.sqrt(sum(v * v for v in vel))
        uc.mem_write(veh + 0xB0, b''.join(f2b(x) for x in vel) + f2b(spd))
        om = st.get('omega', [0, 0, 0])
        uc.mem_write(veh + 0xD0, b''.join(f2b(x) for x in om) + f2b(0.0))

        uc.mem_write(veh + 0x1D0, b''.join(f2b(x) for x in st['bbmax']))
        uc.mem_write(veh + 0x1E0, b''.join(f2b(x) for x in st['bbmin']))
        uc.mem_write(veh + 0x1F0, f2b(st['mass']))
        uc.mem_write(veh + 0x204, struct.pack('<I', frame))
        uc.mem_write(veh + 0x208, struct.pack('<I', veh + HULL_OFF))
        uc.mem_write(veh + 0x20C, bytes([2]))
        uc.mem_write(veh + 0x20E, bytes([st.get('asleep', 0)]))
        uc.mem_write(veh + 0x210, bytes([st.get('crashed', 0)]))
        uc.mem_write(veh + 0x211, b'\0')
        uc.mem_write(veh + 0x212, bytes([st.get('grounded', 0)]))
        uc.mem_write(veh + 0x13F4, struct.pack('<I', race))
        uc.mem_write(veh + 0x1408, f2b(st.get('yaw_input', 0.0)))
        uc.mem_write(veh + 0x1524, struct.pack('<i', st.get('drift', 0)))
        uc.mem_write(veh + 0x153C, b'\0')
        uc.mem_write(race + 0x19BC, bytes([slot]))

        # hull record, relinked exactly as FUN_00122830 does
        rec = bytearray(st['hull'])
        base = veh + HULL_OFF
        struct.pack_into('<I', rec, 0x00, base + 0x1C)
        struct.pack_into('<I', rec, 0x04, base + 0xA0)
        struct.pack_into('<I', rec, 0x08, base + 0x320)
        struct.pack_into('<I', rec, 0x0C, base + 0x480)
        struct.pack_into('<I', rec, 0x10, base + 0x4F8)
        uc.mem_write(base, bytes(rec))

        # collision object record (stride 0x30)
        uc.mem_write(obj, b'\0' * 0x30)
        uc.mem_write(obj + 0x00, bytes([st.get('type', 0)]))
        uc.mem_write(obj + 0x04, struct.pack('<I', frame))
        uc.mem_write(obj + 0x08, struct.pack('<I', veh + 0x1D0))
        uc.mem_write(obj + 0x0C, struct.pack('<I', veh))

    # -- the type-3 (live traffic car) side ---------------------------------
    def set_game_mode(self, mode, sub):
        """FUN_00017310 -- 'crash party' is game mode 6 or sub-mode 3/4/5."""
        self.uc.mem_write(STUB_MODE, b'\xB8' + struct.pack('<I', mode) + b'\xC3')
        self.uc.mem_write(STUB_SUB,  b'\xB8' + struct.pack('<I', sub) + b'\xC3')

    def seed_traffic(self, st):
        """Seed slot 1 as a LIVE TRAFFIC CAR: a type-3 collision handle whose
        entity is a traffic record, NOT a vehicle.  FUN_00111620 (called by
        the traffic spawn FUN_001A2B20) writes exactly this handle:
            +0x00 type 3   +0x04 rec+0x70 (its own 4x4)
            +0x08 model+0xE80 (the box)   +0x0C the traffic record
        and FUN_0010FB70 hands the narrow phase model+0x1060 as the hull."""
        uc = self.uc
        uc.mem_write(TRAFFIC_B, b'\0' * 0x200)
        uc.mem_write(MODEL_B, b'\0' * 0x1700)

        m = st['frame']
        write_mat(uc, TRAFFIC_B + 0x70, m)
        uc.mem_write(TRAFFIC_B + 0xB0, struct.pack('<I', MODEL_B))
        uc.mem_write(TRAFFIC_B + 0xC4, f2b(st.get('speed', 0.0)))
        uc.mem_write(TRAFFIC_B + 0x174, bytes([st.get('flags_174', 0)]))

        uc.mem_write(MODEL_B + 0xE80, b''.join(f2b(x) for x in st['bbmax']))
        uc.mem_write(MODEL_B + 0xE90, b''.join(f2b(x) for x in st['bbmin']))
        # The type-3 path takes the hull straight from the loaded model
        # (FUN_0010FB70 returns model+0x1060) and FUN_0010AC20 dereferences
        # the pointer table at +0x00..+0x10, so the record must be relinked
        # in place -- the same relink FUN_00122C20 performs on the copy it
        # makes for a vehicle.  [S: the in-place relink site is not located.]
        rec = bytearray(st['hull'])
        base = MODEL_B + BGV_HULL_OFF
        for i, off in enumerate((0x1C, 0xA0, 0x320, 0x480, 0x4F8)):
            struct.pack_into('<I', rec, i * 4, base + off)
        uc.mem_write(base, bytes(rec))

        uc.mem_write(OBJ_B, b'\0' * 0x30)
        uc.mem_write(OBJ_B + 0x00, bytes([3]))
        uc.mem_write(OBJ_B + 0x04, struct.pack('<I', TRAFFIC_B + 0x70))
        uc.mem_write(OBJ_B + 0x08, struct.pack('<I', MODEL_B + 0xE80))
        uc.mem_write(OBJ_B + 0x0C, struct.pack('<I', TRAFFIC_B))
        uc.mem_write(D_73BB8C, b'\0')
        self.traffic_seed = dict(st)

    def _promote_object(self, obj, marker):
        """FUN_00114910 + FUN_00120BA0 over the harness's own record."""
        uc = self.uc
        st = self.traffic_seed
        rec = struct.unpack('<I', uc.mem_read(obj + 0x0C, 4))[0]
        m = [list(struct.unpack('<4f', uc.mem_read(rec + 0x70 + r * 16, 16)))
             for r in range(4)]
        spd = struct.unpack('<f', uc.mem_read(rec + 0xC4, 4))[0]

        uc.mem_write(VEH_B, b'\0' * VEH_SZ)
        write_mat(uc, FRAME_B, m)                     # veh+0x204 frame object
        write_mat(uc, VEH_B + 0x70, invert_rigid(m))  # @0x00120D81
        ii = st.get('inv_inertia', [[1.0 / 900, 0, 0, 0], [0, 1.0 / 1800, 0, 0],
                                    [0, 0, 1.0 / 1600, 0]])
        for i in range(3):
            uc.mem_write(VEH_B + 0x40 + i * 16, b''.join(f2b(x) for x in ii[i]))
        # veh+0xBC = rec+0xC4 and veh+0xB0.. = frame.at * speed  @0x00120DDD
        uc.mem_write(VEH_B + 0xBC, f2b(spd))
        uc.mem_write(VEH_B + 0xC0, b''.join(f2b(x) for x in m[2]))
        # +0xB0/+0xB4/+0xB8 only -- retail writes the three lanes one at a
        # time (@0x00120E20/0x00120E2E/0x00120E3C) and leaves +0xBC holding
        # the speed it stored at 0x00120DDD.
        for k in range(3):
            uc.mem_write(VEH_B + 0xB0 + k * 4, f2b(m[2][k] * spd))
        uc.mem_write(VEH_B + 0x1D0, b''.join(f2b(x) for x in st['bbmax']))
        uc.mem_write(VEH_B + 0x1E0, b''.join(f2b(x) for x in st['bbmin']))
        uc.mem_write(VEH_B + 0x1F0, f2b(st['mass']))
        uc.mem_write(VEH_B + 0x204, struct.pack('<I', FRAME_B))
        uc.mem_write(VEH_B + 0x208, struct.pack('<I', VEH_B + HULL_OFF))
        uc.mem_write(VEH_B + 0x13F4, struct.pack('<I', RACE_B))
        uc.mem_write(VEH_B + 0x242B, bytes([marker]))
        hull = bytearray(st['hull'])
        base = VEH_B + HULL_OFF
        for i, off in enumerate((0x1C, 0xA0, 0x320, 0x480, 0x4F8)):
            struct.pack_into('<I', hull, i * 4, base + off)
        uc.mem_write(base, bytes(hull))

        uc.mem_write(obj + 0x00, bytes([4]))          # @0x00114913 *param_1 = 4
        uc.mem_write(obj + 0x04, struct.pack('<I', FRAME_B))
        uc.mem_write(obj + 0x08, struct.pack('<I', VEH_B + 0x1D0))
        uc.mem_write(obj + 0x0C, struct.pack('<I', VEH_B))

    def seg_closest2d(self, a0, a1, b0, b1):
        """FUN_0010FCE0(EDX=b1, ECX=b0, [+8]=a0, [+0xC]=a1, ESI=pB, EDI=pA,
        [+0x10]=&tA, [+0x14]=&tB) -> distance in XMM0."""
        uc = self.uc
        base = SCRATCH + 0x100
        addr = {}
        for i, (nm, v) in enumerate((('a0', a0), ('a1', a1),
                                     ('b0', b0), ('b1', b1))):
            addr[nm] = base + i * 0x10
            uc.mem_write(addr[nm], b''.join(f2b(x) for x in v))
        pa, pb, ta, tb = base + 0x40, base + 0x50, base + 0x60, base + 0x70
        for a in (pa, pb, ta, tb):
            uc.mem_write(a, b'\0' * 16)
        sp = STACK_BASE + STACK_SIZE - 0x2000
        for i, a in enumerate([addr['a0'], addr['a1'], ta, tb]):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a))
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        uc.reg_write(UC_X86_REG_ESP, sp)
        uc.reg_write(UC_X86_REG_EDX, addr['b1'])
        uc.reg_write(UC_X86_REG_ECX, addr['b0'])
        uc.reg_write(UC_X86_REG_ESI, pb)
        uc.reg_write(UC_X86_REG_EDI, pa)
        uc.emu_start(F_FCE0, MAGIC_RET, count=200000)
        try:
            import unicorn.x86_const as xc
            dist = struct.unpack('<f', bytes(uc.reg_read(xc.UC_X86_REG_XMM0)
                                             .to_bytes(16, 'little')[:4]))[0]
        except Exception:
            dist = None
        return dict(dist=dist,
                    pa=read_vec(uc, pa, 2), pb=read_vec(uc, pb, 2),
                    ta=read_vec(uc, ta, 1)[0], tb=read_vec(uc, tb, 1)[0])

    def resolve_traffic(self):
        """FUN_00112E70(param_1 = the collision world, param_2 = the pair)."""
        self._pair()
        self.crashed_vehs = []
        self.slams = []
        self.promotions = []
        self._run(F_112E70, {}, [0, PAIR])
        out = self._pair_out()
        out['promote'] = int(OBJ_B in self.promotions)
        return out

    def dump(self, slot):
        uc = self.uc
        veh = VEH_A if slot == 0 else VEH_B
        return dict(
            force=read_vec(uc, veh + 0xF0),
            torque=read_vec(uc, veh + 0x100),
            imp_force=read_vec(uc, veh + 0x110),
            imp_torque=read_vec(uc, veh + 0x120),
            deflection=read_vec(uc, veh + 0x130),
            contact_pt=read_vec(uc, veh + 0x150),
            touched=uc.mem_read(veh + 0x211, 1)[0],
            hit_side=uc.mem_read(veh + 0x153C, 1)[0],
        )

    # -- calling ------------------------------------------------------------
    def _run(self, eip, regs, stack_args, timeout_insn=8000000):
        uc = self.uc
        sp = STACK_BASE + STACK_SIZE - 0x2000
        for i, a in enumerate(stack_args):
            uc.mem_write(sp + 4 + i * 4, struct.pack('<I', a & 0xFFFFFFFF))
        uc.mem_write(sp, struct.pack('<I', MAGIC_RET))
        uc.reg_write(UC_X86_REG_ESP, sp)
        for r, v in regs.items():
            uc.reg_write(r, v & 0xFFFFFFFF)
        uc.emu_start(eip, MAGIC_RET, count=timeout_insn)
        return uc.reg_read(UC_X86_REG_EAX)

    def narrow(self, mode=0):
        """FUN_0010A9D0(EDX=vehA, ECX=vehB, [+4]=ctx, [+8]=mode) -> AL"""
        uc = self.uc
        uc.mem_write(CTXBUF, b'\0' * 0x210)
        eax = self._run(F_A9D0,
                        {UC_X86_REG_EAX: 0, UC_X86_REG_EDX: VEH_A,
                         UC_X86_REG_ECX: VEH_B},
                        [CTXBUF, mode])
        hit = eax & 0xFF
        return dict(hit=hit,
                    valid=struct.unpack('<I', uc.mem_read(CTXBUF + 0x1E4, 4))[0],
                    normal=read_vec(uc, CTXBUF + 0x1F0),
                    point=read_vec(uc, CTXBUF + 0x200),
                    posA=read_vec(uc, CTXBUF + 0x60),
                    posB=read_vec(uc, CTXBUF + 0x150))

    def impulse(self, pt3, pt1, vrel, n, e):
        """FUN_0010F8D0(EAX=vehB, ECX=vehA, pt3, pt1, vrel, n, e, out)"""
        uc = self.uc
        p3, p1, vr, nn, out = (SCRATCH, SCRATCH + 0x10, SCRATCH + 0x20,
                               SCRATCH + 0x30, SCRATCH + 0x40)
        for a, v in ((p3, pt3), (p1, pt1), (vr, vrel), (nn, n)):
            uc.mem_write(a, b''.join(f2b(x) for x in v))
        uc.mem_write(out, b'\0' * 16)
        self._run(F_F8D0,
                  {UC_X86_REG_EAX: VEH_B, UC_X86_REG_ECX: VEH_A,
                   UC_X86_REG_EDX: 0},
                  [p3, p1, vr, nn, struct.unpack('<I', f2b(e))[0], out])
        return read_vec(uc, out)

    def _pair(self):
        uc = self.uc
        uc.mem_write(PAIR, b'\0' * 0x30)
        uc.mem_write(PAIR + 0x24, struct.pack('<I', OBJ_A))
        uc.mem_write(PAIR + 0x28, struct.pack('<I', OBJ_B))

    def resolve_alive(self):
        self._pair()
        self.crashed_vehs = []
        self.slams = []
        self._run(F_1121F0, {}, [PAIR])
        return self._pair_out()

    def resolve_wreck(self):
        self._pair()
        self.crashed_vehs = []
        self.slams = []
        self._run(F_113960, {}, [0, PAIR])
        return self._pair_out()

    def _pair_out(self):
        uc = self.uc
        return dict(point=read_vec(uc, PAIR + 0x00),
                    normal=read_vec(uc, PAIR + 0x10),
                    impact=struct.unpack('<f', uc.mem_read(PAIR + 0x20, 4))[0],
                    hit=uc.mem_read(PAIR + 0x2C, 1)[0],
                    slam=uc.mem_read(PAIR + 0x2D, 1)[0],
                    crash_a=int(VEH_A in self.crashed_vehs),
                    crash_b=int(VEH_B in self.crashed_vehs),
                    slams=list(self.slams),
                    a=self.dump(0), b=self.dump(1))

    def aabb(self, slot):
        """FUN_00114270(EAX=object record) -> record +0x10 lo / +0x20 hi"""
        obj = OBJ_A if slot == 0 else OBJ_B
        self._run(0x00114270, {UC_X86_REG_EAX: obj}, [])
        return dict(lo=read_vec(self.uc, obj + 0x10),
                    hi=read_vec(self.uc, obj + 0x20))

    def apply_force(self, slot, force, point):
        """FUN_001205E0(EAX=veh, EDI=&force, [+4]=&point)"""
        uc = self.uc
        f, p = SCRATCH + 0x80, SCRATCH + 0x90
        uc.mem_write(f, b''.join(f2b(x) for x in force))
        uc.mem_write(p, b''.join(f2b(x) for x in point))
        veh = VEH_A if slot == 0 else VEH_B
        self._run(F_1205E0, {UC_X86_REG_EAX: veh, UC_X86_REG_EDI: f}, [p])
        return self.dump(slot)


# ---------------------------------------------------------------------------
def extract_hulls(outdir=None):
    """Copy the .bgv/.btv collision-hull record to build/cars/*.hull."""
    outdir = outdir or os.path.join(ROOT, "build", "cars")
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for ext, sub in (("bgv", "pveh"), ("btv", "pveh")):
        for path in sorted(glob.glob(os.path.join(GAME, sub, "*", "*." + ext))):
            data = open(path, 'rb').read()
            if len(data) < BGV_HULL_OFF + HULL_SZ:
                continue
            rec = data[BGV_HULL_OFF:BGV_HULL_OFF + HULL_SZ]
            nv, np_, ne = rec[0x18], rec[0x19], rec[0x1A]
            if not (0 < nv <= 22 and 0 < np_ <= 40 and 0 < ne <= 60):
                print("  skip (bad counts %d/%d/%d): %s" % (nv, np_, ne, path))
                continue
            cls = os.path.basename(os.path.dirname(path))
            base = os.path.splitext(os.path.basename(path))[0]
            dst = os.path.join(outdir, "%s_%s.hull" % (cls, base))
            open(dst, 'wb').write(rec)
            n += 1
    print("[carcol] wrote %d hull records to %s" % (n, outdir))
    return n


def load_hull(cls, car):
    p = os.path.join(GAME, "pveh", cls, car + ".bgv")
    if not os.path.exists(p):
        p = os.path.join(GAME, "pveh", cls, car + ".btv")
    d = open(p, 'rb').read()
    return d[BGV_HULL_OFF:BGV_HULL_OFF + HULL_SZ]


def bbox(cls, car):
    p = os.path.join(GAME, "pveh", cls, car + ".bgv")
    if not os.path.exists(p):
        p = os.path.join(GAME, "pveh", cls, car + ".btv")
    d = open(p, 'rb').read()
    return (list(struct.unpack_from('<4f', d, 0xE80)),
            list(struct.unpack_from('<4f', d, 0xE90)))


if __name__ == '__main__':
    if '--extract-hulls' in sys.argv:
        extract_hulls()
    elif '--selftest' in sys.argv:
        s = Session()
        hull = load_hull("COMP", "Car1")
        bmax, bmin = bbox("COMP", "Car1")
        s.seed(0, dict(frame=frame_from(0.0, (0, 0, 0)), hull=hull,
                       bbmax=bmax, bbmin=bmin, mass=800.0,
                       vel=[0, 0, 20.0]))
        s.seed(1, dict(frame=frame_from(0.0, (1.4, 0, 2.0)), hull=hull,
                       bbmax=bmax, bbmin=bmin, mass=900.0,
                       vel=[0, 0, 10.0]))
        print("narrow:", s.narrow(0))
        print("resolve:", s.resolve_alive())
    else:
        print(__doc__)
