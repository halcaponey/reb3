#!/usr/bin/env python3
"""Differential regression for retail's traffic SPAWN POLICY.

Every rule the harness now runs when a pool request produces cars is executed
here out of build/burnout3.elf under Unicorn and compared with the recovered
model, then the model is replayed over the shipped .bgd tables:

    FUN_00048760 @0x00048760   the manager RNG (seeded @0x001A3EA7/0x001A3EB1)
    FUN_001A6590 @0x001A6590   vehicle CLASS, weighted by the per-road rate table
    FUN_001A5E30 @0x001A5E30   MODEL inside the class, weighted by record +0x10
    FUN_001A5F90 @0x001A5F90   PAINT, the eight percentage bytes at record +0x08
    FUN_0019E5B0 @0x0019E5B0   (path,row) -> (manager record, slot)
    FUN_0019E640 @0x0019E640   the same search forward, i.e. the section end
    FUN_001A6070 @0x001A6183   the population law (checked as a model replay,
                               the function itself needs the whole manager)
    FUN_001A3470 @0x001A34AC   the STAMP/UNSTAMP dispatch: a request's
                               direction byte and the manager's +0x363BC
                               travel-sense flag decide whether the request
                               SPAWNS or RETIRES.  Executed here for all eight
                               (direction, flag) pairs, then applied to the
                               shipped table so the population figures below
                               are the ones retail's spawn arm really produces.

The population law needs no emulation to be pinned: its four constants are
read straight out of the image here (0.44704, 1/60, 0.5, 0.30, 0.15, 1/2^32).
"""
import math
import os
import pathlib
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import emulate_ai as ea                                       # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX,  # noqa: E402
                               UC_X86_REG_EDX, UC_X86_REG_ESI)

F_RNG = 0x00048760
F_PICK_CLASS = 0x001A6590
F_PICK_MODEL = 0x001A5E30
F_PICK_PAINT = 0x001A5F90
F_ROW_OWNER = 0x0019E5B0
F_ROW_NEXT = 0x0019E640

G_RNG_SCALE = 0x0054F46C          # 1/2^32, installed by the ctor @0x00264880
G_RNG_STATE = 0x00649B28          # manager+0x36348
G_RNG_CARRY = 0x00649B2C          # manager+0x3634C
G_MANAGER = 0x006137E0
G_RECORDS = 0x006137E4            # manager+4, stride 0x118

# FUN_001A3EA0 @0x001A3EA7 / @0x001A3EB1
SEED_STATE = 0xFD462907
SEED_CARRY = 0x02B9D6F8

# FUN_001A5E30's jump table @0x001A5F10, index = class - 1
CLASS_LIST_OFFSET = {1: 0x54, 2: 0x60, 3: 0x78, 4: 0x84, 5: 0x6C, 0x0B: 0x90}

SCRATCH = ea.OTHER + 0x1000


# --------------------------------------------------------------- the model
class Rng(object):
    """FUN_00048760: s' = (s<<16) + (int16)(s>>16) + c ; c' = c + s'."""

    def __init__(self, state=SEED_STATE, carry=SEED_CARRY):
        self.state = state & 0xFFFFFFFF
        self.carry = carry & 0xFFFFFFFF

    def u32(self):
        high = (self.state >> 16) & 0xFFFF
        if high >= 0x8000:
            high -= 0x10000
        nxt = ((self.state << 16) + high + self.carry) & 0xFFFFFFFF
        self.carry = (self.carry + nxt) & 0xFFFFFFFF
        self.state = nxt
        return nxt

    def f(self):
        return float(self.u32()) * (1.0 / 4294967296.0)


def pick_class(rng, rate):
    """FUN_001A6590: cumulative walk over six per-class rates, `acc > r`."""
    draw = rng.f() * math.fsum(rate[:6])
    acc = 0.0
    for cls in range(6):
        acc = float(struct.unpack('<f', struct.pack('<f', acc + rate[cls]))[0])
        if acc > draw:
            return cls
    return 1


def pick_model(rng, weights, total):
    """FUN_001A5E30: single-entry lists take no draw; otherwise `rng % total`
    against the running weight sum with a `<=` compare (jle @0x001A5EF4)."""
    if len(weights) == 1:
        return 0
    draw = rng.u32() % total
    acc = 0
    for index, weight in enumerate(weights):
        acc += weight
        if draw <= acc:
            return index
    return -1


def pick_paint(rng, colours):
    """FUN_001A5F90: `rng % 100` against eight cumulative percentage bytes."""
    draw = rng.u32() % 100
    acc = 0
    for index in range(8):
        acc += colours[index]
        if draw <= acc:
            return index
    return 8


def row_owner(bindings, row):
    """FUN_0019E5B0: the binding with the LARGEST start_row <= row."""
    best = None
    for index, (start, _record, _slot) in enumerate(bindings):
        if start <= row and (best is None or start > bindings[best][0]):
            best = index
    return best


def row_next(bindings, row):
    """FUN_0019E640: the binding with the SMALLEST start_row > row."""
    best = None
    for index, (start, _record, _slot) in enumerate(bindings):
        if start > row and (best is None or start < bindings[best][0]):
            best = index
    return best


def population(span, speed_mph, rate):
    """FUN_001A6070 @0x001A6183: cars per request section."""
    rate_sum = math.fsum(rate[:6])
    speed = speed_mph * 0.44704
    if rate_sum <= 0.0 or speed <= 0.0 or span <= 0.0:
        return 0, 0.0
    count_f = (span / speed) * (1.0 / 60.0) * rate_sum
    if count_f < 0.5:
        return 0, 0.0
    return int(count_f + 0.5), span / count_f


# ------------------------------------------------------------- the emulator
class Mix(object):
    def __init__(self):
        self.session = ea.Session()
        self.session.wf(G_RNG_SCALE, 1.0 / 4294967296.0)

    def seed(self, state=SEED_STATE, carry=SEED_CARRY):
        self.session.wu(G_RNG_STATE, state)
        self.session.wu(G_RNG_CARRY, carry)

    def rng(self, state, carry):
        """Run FUN_00048760 on a {state, carry} pair in scratch memory."""
        session = self.session
        session.wu(SCRATCH, state)
        session.wu(SCRATCH + 4, carry)
        value = session.call(F_RNG, regs={UC_X86_REG_EAX: SCRATCH})
        assert session.fault is None, session.fault
        return value, session.ru(SCRATCH), session.ru(SCRATCH + 4)

    def pick_class(self, rate):
        """FUN_001A6590(EAX=?, EDX=slot, ESI=record, [esp+4]=rate_sum)."""
        session = self.session
        record, slot = SCRATCH + 0x100, 1
        table = SCRATCH + 0x200
        for index, value in enumerate(rate):
            session.wf(table + index * 4, value)
        session.wu(record + slot * 4 + 0x10, table)
        total = struct.unpack('<f', struct.pack('<f', math.fsum(rate[:6])))[0]
        session.call(F_PICK_CLASS,
                     regs={UC_X86_REG_EDX: slot, UC_X86_REG_ESI: record},
                     stack_args=(struct.unpack('<I', struct.pack('<f', total))[0],))
        assert session.fault is None, session.fault
        return session.uc.reg_read(UC_X86_REG_EAX)

    def pick_model(self, cls, weights, total):
        """FUN_001A5E30(ECX=class).

        `mov eax,[0x6137E0]` @0x001A5E31 loads the manager's FIRST dword, which
        FUN_001A3AE0 shows is the event TDESC pointer -- so the six
        `{ptr, count, weight_total}` list triples are TDESC-relative, exactly
        where tools/extract_traffic.py reads them.
        """
        session = self.session
        records = SCRATCH + 0x400
        tdesc = SCRATCH + 0x2000
        session.wu(G_MANAGER, tdesc)
        for index, weight in enumerate(weights):
            session.wu(records + index * 0x18 + 0x10, weight)
        offset = CLASS_LIST_OFFSET[cls]
        session.wu(tdesc + offset, records)
        session.wu(tdesc + offset + 4, len(weights))
        session.wu(tdesc + offset + 8, total)
        session.call(F_PICK_MODEL, regs={UC_X86_REG_ECX: cls})
        assert session.fault is None, session.fault
        result = session.uc.reg_read(UC_X86_REG_EAX)
        if result == 0:
            return -1
        assert (result - records) % 0x18 == 0, hex(result)
        return (result - records) // 0x18

    def pick_paint(self, colours):
        """FUN_001A5F90(ESI=record)."""
        session = self.session
        record = SCRATCH + 0x600
        session.uc.mem_write(record + 8, bytes(bytearray(colours)))
        session.call(F_PICK_PAINT, regs={UC_X86_REG_ESI: record})
        assert session.fault is None, session.fault
        return session.uc.reg_read(UC_X86_REG_EAX) & 0xFF

    def row_search(self, forward, bindings, row):
        """FUN_0019E5B0 / FUN_0019E640 over a synthetic path descriptor."""
        session = self.session
        desc = SCRATCH + 0x800
        session.wb(desc + 0x48, len(bindings))
        for index, (start, record, slot) in enumerate(bindings):
            session.wb(desc + 6 + index, record)
            session.wb(desc + 0x1E + index, slot)
            rows = SCRATCH + 0xA00 + record * 0x40
            session.wu(G_RECORDS + record * 0x118 + 0x40, SCRATCH + 0xC00
                       + record * 0x20)
            session.wu(SCRATCH + 0xC00 + record * 0x20 + 4, rows)
            session.wu(rows + slot * 4, start)
        out_slot, out_row = SCRATCH + 0xE00, SCRATCH + 0xE10
        session.call(F_ROW_NEXT if forward else F_ROW_OWNER,
                     stack_args=(desc, row, out_slot, out_row))
        assert session.fault is None, session.fault
        record = session.uc.reg_read(UC_X86_REG_EAX)
        if record == 0:
            return None
        return ((record - G_RECORDS) // 0x118, session.ru(out_slot),
                session.ri(out_row))


# ----------------------------------------------------------------- the runs
def test_rng(mix):
    model = Rng()
    state, carry = SEED_STATE, SEED_CARRY
    for step in range(64):
        want = model.u32()
        _st0, state, carry = mix.rng(state, carry)
        assert state == want, 'rng step %d: %08x != %08x' % (step, state, want)
        assert carry == model.carry, 'rng carry step %d' % step
    # the uniform itself is returned on the x87 stack, so it is pinned instead
    # by the scale constant (test_constants) plus the class/paint draws below,
    # which run the whole float path inside the retail functions.
    return 64


def test_class(mix):
    tables = [
        [0.0, 0.989, 1.6985, 0.0, 0.3225, 0.0],     # US_C3_V1 record 0 slot 0
        [0.0, 2.44, 4.532, 0.0, 1.043, 0.0],        # record 4 slot 0
        [0.0, 1.36, 0.918, 0.0, 0.0893, 0.0],       # record 21 slot 0
        [0.0, 0.0, 0.0, 0.0, 0.0375, 0.0],          # a tractor-only road
        [0.0, 1.0, 0.0, 0.0, 0.0, 0.0],             # single-class road
    ]
    checks = 0
    for rate in tables:
        model = Rng()
        mix.seed()
        for _ in range(48):
            want = pick_class(model, rate)
            got = mix.pick_class(rate)
            assert got == want, 'class %s: %d != %d' % (rate, got, want)
            checks += 1
    return checks


def test_model(mix):
    cases = [
        (1, [1, 1], 2),                 # US_C3_V1 compacts
        (2, [1, 1, 1, 1, 1, 2], 7),     # US_C3_V1 light heavies
        (4, [1], 1),                    # the single tractor
        (0x0B, [1, 3], 4),              # the two trailers
        (2, [5, 2, 3], 10),
    ]
    checks = 0
    for cls, weights, total in cases:
        model = Rng()
        mix.seed()
        for _ in range(40):
            want = pick_model(model, weights, total)
            got = mix.pick_model(cls, weights, total)
            assert got == want, ('model cls %d %s: %d != %d'
                                 % (cls, weights, got, want))
            checks += 1
    return checks


def test_paint(mix):
    cases = [
        [10, 18, 18, 18, 18, 18, 0, 0],
        [11, 19, 18, 18, 17, 17, 0, 0],
        [9, 13, 13, 13, 13, 13, 13, 13],
        [100, 0, 0, 0, 0, 0, 0, 0],
    ]
    checks = 0
    for colours in cases:
        model = Rng()
        mix.seed()
        for _ in range(40):
            want = pick_paint(model, colours)
            got = mix.pick_paint(colours)
            assert got == want, 'paint %s: %d != %d' % (colours, got, want)
            checks += 1
    return checks


def test_row_search(mix):
    cases = [
        [(0, 0, 1), (31, 22, 1), (108, 23, 1), (301, 1, 1)],   # US_C3_V1 path 0
        [(0, 5, 0)],
        [(0, 19, 0), (99, 25, 0)],
    ]
    checks = 0
    for bindings in cases:
        for row in (0, 1, 30, 31, 32, 99, 107, 108, 300, 301, 500):
            want = row_owner(bindings, row)
            got = mix.row_search(False, bindings, row)
            if want is None:
                assert got is None, 'owner row %d' % row
            else:
                start, record, slot = bindings[want]
                assert got == (record, slot, start), (
                    'owner row %d: %s != %s' % (row, got,
                                                (record, slot, start)))
            want = row_next(bindings, row)
            got = mix.row_search(True, bindings, row)
            if want is None:
                assert got is None, 'next row %d' % row
            else:
                start, record, slot = bindings[want]
                assert got == (record, slot, start), (
                    'next row %d: %s != %s' % (row, got,
                                               (record, slot, start)))
            checks += 2
    return checks


F_STAMP_DISPATCH = 0x001A34AC      # FUN_001A3470, just past the row min/max
A_STAMP_ARM = 0x001A34EC           # the request populates
A_UNSTAMP_ARM = 0x001A3611         # the request retires
MGR_FORWARD = 0x363BC              # manager+0x363BC, FUN_001A3EA0 @0x001A3F1F


def test_dispatch(mix):
    """Run FUN_001A3470's dispatch for every (direction, +0x363BC) pair.

    The block is straight-line from 0x001A34AC to whichever arm it picks, so a
    code hook stopping at 0x001A34EC (STAMP) or 0x001A3611 (UNSTAMP) reads the
    verdict straight out of retail without needing the manager's tables.
    """
    from unicorn import UcError, UC_PROT_ALL, UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX,
                                   UC_X86_REG_ESP)
    session = mix.session
    uc = session.uc
    BASE = 0x80000000
    try:
        uc.mem_map(BASE, 0x80000, UC_PROT_ALL)
    except UcError:
        pass                                   # already mapped by a rerun
    request, descriptor, manager, stack = (BASE + 0x100, BASE + 0x200,
                                           BASE + 0x1000, BASE + 0x70000)
    exits = []

    def on_code(u, address, size, user):
        if address in (A_STAMP_ARM, A_UNSTAMP_ARM):
            exits.append(address)
            u.emu_stop()

    handle = uc.hook_add(UC_HOOK_CODE, on_code)
    uc.mem_write(descriptor + 4, struct.pack('<H', 0))
    checks = 0
    # retail's own truth table: forwards (the constructor's value) direction 0
    # spawns and 1/2 retire; backwards it is the other way round; anything
    # outside {0,1,2} always retires (@0x001A34D4).
    for forward in (0, 1):
        for direction in (0, 1, 2, 3, 7):
            uc.mem_write(manager + MGR_FORWARD, bytes([forward]))
            uc.mem_write(request, struct.pack('<HHBB', 10, 40, 0, direction))
            uc.mem_write(stack + 0x28, struct.pack('<I', manager))
            uc.mem_write(stack + 0x34, struct.pack('<I', 0))
            uc.reg_write(UC_X86_REG_EAX, request)
            uc.reg_write(UC_X86_REG_EBX, descriptor)
            uc.reg_write(UC_X86_REG_ESP, stack)
            del exits[:]
            try:
                uc.emu_start(F_STAMP_DISPATCH, 0, count=4000)
            except UcError as exc:
                raise AssertionError('dispatch dir=%d fwd=%d: %s'
                                     % (direction, forward, exc))
            assert len(exits) == 1, (
                'dispatch dir=%d fwd=%d reached %r'
                % (direction, forward, [hex(e) for e in exits]))
            got = exits[0] == A_STAMP_ARM
            if direction == 0:
                wantv = forward != 0
            elif direction in (1, 2):
                wantv = forward == 0
            else:
                wantv = False
            assert got == wantv, (
                'dispatch dir=%d fwd=%d: retail %s, model %s'
                % (direction, forward, 'STAMP' if got else 'UNSTAMP',
                   'STAMP' if wantv else 'UNSTAMP'))
            checks += 1
    uc.hook_del(handle)
    return checks


def request_stamps(direction, forward):
    """The recovered model of the above."""
    if direction == 0:
        return forward != 0
    if direction in (1, 2):
        return forward == 0
    return False


def test_constants():
    """The population/placement constants, read out of the image."""
    import extract_bgd_paths                                   # noqa: F401
    data = open('build/burnout3.elf', 'rb').read()
    ph_off = struct.unpack_from('<I', data, 0x1C)[0]
    ph_num = struct.unpack_from('<H', data, 0x2C)[0]
    segs = []
    for i in range(ph_num):
        p_type, off, va, _, fsz, _msz, _, _ = struct.unpack_from(
            '<IIIIIIII', data, ph_off + i * 32)
        if p_type == 1:
            segs.append((va, off, fsz))

    def f32(va):
        for base, off, size in segs:
            if base <= va < base + size:
                return struct.unpack_from('<f', data, off + va - base)[0]
        raise AssertionError('VA %#x not in file' % va)

    expected = {
        0x003A5958: 0.44704,          # mph -> m/s   (FUN_001A6070 @0x001A61B0)
        0x003B1838: 1.0 / 60.0,       # per-minute   (@0x001A61C8)
        0x003B1684: 0.5,              # the n_f floor(@0x001A61DC) / round
        0x003B1750: 0.30,             # row jitter   (@0x001A6301)
        0x00384A80: 0.15,             # speed jitter (@0x001A64DB)
        0x003B191C: 1.0 / 2 ** 32,    # RNG scale ctor @0x00264880
        0x003B16A8: 2.0 ** 32,        # RNG negative fixup
    }
    for va, want in expected.items():
        got = f32(va)
        assert abs(got - want) <= abs(want) * 1e-6, (
            'constant %#x: %r != %r' % (va, got, want))
    return len(expected)


# ------------------------------------------------- the extracted asset itself
def test_asset():
    track = os.environ.get('B3_TRACK', os.environ.get('B3_POSTFX_TRACK',
                                                      'US_C3_V1'))
    path = pathlib.Path('build/tracks') / track / 'traffic_paths.bin'
    data = path.read_bytes()
    magic, version, point_count, path_count = struct.unpack_from('<4sIII', data)
    assert magic == b'B3TP'
    if version < 4:
        print('traffic_paths.bin is v%d: no spawn-policy section to check'
              % version)
        return 0
    window_count, request_count = struct.unpack_from('<II', data, 16)
    offset = 24 + point_count * 12
    path_rows = []
    for _ in range(path_count):
        (count,) = struct.unpack_from('<I', data, offset)
        offset += 4 + count * 4 + count * 8 + count * 0x12
        path_rows.append(count)
    windows = []
    for _ in range(window_count):
        windows.append(struct.unpack_from('<IIIBBH', data, offset))
        offset += 16
    requests = []
    for _ in range(request_count):
        requests.append(struct.unpack_from('<HHBB', data, offset))
        offset += 6
    classes_n, entries_n, bindings_n, roads_n = struct.unpack_from(
        '<IIII', data, offset)
    offset += 16
    classes = []
    for _ in range(classes_n):
        classes.append(struct.unpack_from('<IIII', data, offset))
        offset += 16
    entries = []
    for _ in range(entries_n):
        raw = data[offset:offset + 32]
        entries.append((raw[:16].split(b'\0')[0].decode('ascii'),
                        struct.unpack_from('<I', raw, 16)[0],
                        list(raw[20:28])))
        offset += 32
    bindings = []
    for _ in range(bindings_n):
        bindings.append(struct.unpack_from('<BBBBI', data, offset))
        offset += 8
    roads = {}
    for _ in range(roads_n):
        row = struct.unpack_from('<BBHf6f', data, offset)
        roads[(row[0], row[1])] = (row[3], list(row[4:]))
        offset += 32
    assert offset == len(data), 'trailing bytes in traffic_paths.bin'

    # every class code the runtime can draw must be one FUN_001A5E30 knows
    for cls, base, count, total in classes:
        assert cls in CLASS_LIST_OFFSET, 'unknown class code %d' % cls
        assert base + count <= entries_n
        assert total == sum(entries[base + k][1] for k in range(count)), (
            'class %d total %d != weight sum' % (cls, total))
    # FUN_001A5F90 draws `rng % 100`, so the eight bytes must reach 100
    for name, _weight, colours in entries:
        assert sum(colours) == 100, '%s paint weights sum to %d' % (
            name, sum(colours))
    # rate[0] is the class-0 slot FUN_001A5E30 cannot serve (it would divide by
    # zero); every shipped road must leave it at zero
    for (record, slot), (mph, rate) in roads.items():
        assert rate[0] == 0.0, 'record %d slot %d has a class-0 rate' % (
            record, slot)
        assert 0.0 <= mph <= 200.0
        for cls in range(1, 6):
            if rate[cls] <= 0.0:
                continue
            assert any(group[0] == cls and group[2] for group in classes), (
                'record %d slot %d wants class %d but the list is empty'
                % (record, slot, cls))
    for path_id, record, slot, _pad, start in bindings:
        assert path_id < path_count
        assert start < path_rows[path_id], (
            'binding path %d start_row %d >= %d'
            % (path_id, start, path_rows[path_id]))

    # replay the population law over every shipped request
    by_path = {}
    for path_id, record, slot, _pad, start in bindings:
        by_path.setdefault(path_id, []).append((start, record, slot))
    offset = 24 + point_count * 12
    distances = []
    for path_id in range(path_count):
        (count,) = struct.unpack_from('<I', data, offset)
        offset += 4 + count * 4
        rows = struct.unpack_from('<%df' % (count * 2), data, offset)
        distances.append(rows[0::2])
        offset += count * 8 + count * 0x12
    # THE DIRECTION SPLIT.  FUN_001A3EA0 @0x001A3F1F starts the manager's
    # +0x363BC at 1 and FUN_001A3110 only ever writes `old_window < new_window`
    # into it, so a racer driving an event forwards holds it at 1 throughout.
    # Under that flag FUN_001A3470 sends direction 0 to the STAMP arm and
    # directions 1 and 2 to the UNSTAMP arm -- so only direction-0 requests
    # populate, and the population below is re-derived on that basis rather
    # than over every request as it used to be.
    FORWARD = 1
    per_window = []
    per_window_all = []
    for first, last, base, count, _refresh, _pad in windows:
        total = 0
        total_all = 0
        for index in range(count):
            first_row, last_row, path_id, _direction = requests[base + index]
            spawns = request_stamps(_direction, FORWARD)
            low, high = min(first_row, last_row), max(first_row, last_row)
            row = low
            while True:
                owner = row_owner(by_path.get(path_id, []), row)
                nxt = row_next(by_path.get(path_id, []), row)
                end = high
                if nxt is not None and by_path[path_id][nxt][0] - 1 < high:
                    end = by_path[path_id][nxt][0] - 1
                if owner is not None:
                    _start, record, slot = by_path[path_id][owner]
                    road = roads.get((record, slot))
                    if road:
                        span = abs(distances[path_id][end]
                                   - distances[path_id][row])
                        cars = population(span, road[0], road[1])[0]
                        total_all += cars
                        if spawns:
                            total += cars
                if end >= high:
                    break
                row = end + 1
        per_window.append(total)
        per_window_all.append(total_all)
    live = [per_window[i] + per_window[i - 1] + per_window[i - 2]
            for i in range(len(per_window))]
    live_all = [per_window_all[i] + per_window_all[i - 1] + per_window_all[i - 2]
                for i in range(len(per_window_all))]
    assert min(live) > 0, 'some window group spawns no traffic at all'
    assert max(live) < 254, 'a window group exceeds the 254-body pool'
    # the split must actually bite -- if it did not, the two figures would be
    # identical and the STAMP/UNSTAMP arms would be doing nothing
    assert min(live) < min(live_all) or max(live) < max(live_all), (
        'the direction split changed nothing: %r vs %r'
        % ((min(live), max(live)), (min(live_all), max(live_all))))
    # and it must not halve the road to nothing: retail's own retire arm is
    # what removes these cars again, a median of four windows later
    assert min(live) >= 3, 'a window group spawns almost nothing (%d)' % min(live)
    spawn_n = sum(1 for r in requests if request_stamps(r[3], FORWARD))
    retire_n = len(requests) - spawn_n
    assert spawn_n > 0 and retire_n > 0, (
        'the split leaves one arm empty: %d spawn, %d retire'
        % (spawn_n, retire_n))
    print('  spawn policy: %d classes / %d models, %d bindings, %d roads; '
          '%d of %d requests STAMP (the rest UNSTAMP); population per '
          '3-window group min %d max %d mean %.1f '
          '(pre-split, every request spawning: min %d max %d mean %.1f)'
          % (classes_n, entries_n, bindings_n, roads_n, spawn_n, len(requests),
             min(live), max(live), sum(live) / float(len(live)),
             min(live_all), max(live_all),
             sum(live_all) / float(len(live_all))))
    return classes_n + entries_n + bindings_n + roads_n


def main():
    checks = test_constants()
    mix = Mix()
    checks += test_rng(mix)
    checks += test_class(mix)
    checks += test_model(mix)
    checks += test_paint(mix)
    checks += test_row_search(mix)
    checks += test_dispatch(mix)
    checks += test_asset()
    print('traffic spawn policy (RNG, class, model, paint, row owner, '
          'stamp/unstamp dispatch, population): OK (%d checks)' % checks)


if __name__ == '__main__':
    main()
