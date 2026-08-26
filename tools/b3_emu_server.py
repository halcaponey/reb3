#!/usr/bin/env python3
"""Run RETAIL's own per-frame vehicle pipeline for the live game.

The harness speaks a line protocol on stdin/stdout; this process keeps one
warmed emulate_pipeline.Pipeline per car, so the retail code's own state stays
resident between frames instead of being re-seeded every call. That matters:
a cold call costs ~8.7 ms while a warm one costs ~1.3 ms, because Unicorn's
translation cache has to rebuild otherwise.

Protocol (all text, one command per line):

    hello                                  -> ok <version>
    seed <car> <x> <y> <z> <yaw>           -> ok
    step <car> <thr> <brk> <steer> <boost> <dt> <gx> <gy> <gz> <nx> <ny> <nz>
                                           -> st <32 floats>

`g*`/`n*` are the ground point and normal under the car, measured by the
HARNESS against the real extracted collision world. emulate_pipeline stubs
retail's own ground-poly collection (FUN_0011BC60) with a fixed plane at y=0
spanning +-5000 of the origin, which is useless to a live game: the track sits
at y~149 and x~5200, so the car both falls forever and runs off the edge of
the plane. Re-laying the plane under the car every frame is what marries the
port's collision world to retail's physics.
    bye                                    -> ok

`st` fields, in order:
    pos[3] vel[3] speed dir[3] right[3] up[3] at[3] omega[3]
    rpm gear torque steer_deg drift slide airborne
    wheel_cur[4]

The three basis rows are sent explicitly rather than reconstructing `right`
from up x at: the retail frame is not guaranteed orthonormal mid-collision,
and a reconstructed row would quietly differ from the one retail is using.

Errors reply `err <message>`; the caller demotes that feature to the RE port.
"""
import base64
import gc
import os
import struct
import sys
import time
import importlib.util

_STEPR_MAX = (int(os.environ["B3_STEPR_MAX"])
              if os.environ.get("B3_STEPR_MAX") else None)

VERSION = 1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ep = _load("ep", os.path.join(ROOT, "tools", "emulate_pipeline.py"))
ec = _load("ec", os.path.join(ROOT, "tools", "emulate_carcol.py"))
et = _load("et", os.path.join(ROOT, "tools", "emulate_td_rules.py"))
vs = _load("vs", os.path.join(ROOT, "tools", "validate_score_events.py"))
vc = _load("vc", os.path.join(ROOT, "tools", "emulate_tdfx_camera.py"))
ea = _load("ea", os.path.join(ROOT, "tools", "emulate_ai.py"))

# validate_port carries a Pipeline subclass that runs the WHOLE retail substep
# loop as retail code (emu_start 0x0011C0A0..0x0011C16C) rather than calling the
# stages one at a time from Python. That matters here for one reason:
# FUN_0011AEF0, the chassis-vs-world resolve, runs at its real call site. Plain
# emulate_pipeline stubs it out, which is why the car had no walls and drove
# straight off the track.
vp = _load("vp", os.path.join(ROOT, "tools", "validate_port.py"))
RelocPipeline = vp._make_reloc_pipeline(ep)

# SOUP CAPACITY.  This used to relocate ep.SOUP_TYPE to 0x30029F00 "to the top
# of the region Pipeline actually maps".  That was true when REGION_SZ was
# 0x2A000; it is 0x30000 now (emulate_pipeline.py:114), and 0x30029F00 is only
#   (0x30029F00 - SOUP_REC) / 0x40 = 0x1EC0 / 0x40 = 123
# records past SOUP_REC -- while SOUP_CAP below still let the harness upload 256.
# set_soup (validate_port.py:3018-3028) writes record t at SOUP_REC + 0x40*t and
# then its type at SOUP_TYPE + 2*t, so from t = 123 the record array wrote
# straight over the type table and the type words wrote back into the records:
# the vertices of every triangle past 122 and the surface bytes of triangles
# 0..31 both became garbage, which is the record-array-through-the-type-table
# corruption emulate_pipeline.py:105-110 warns about, one cap higher.
# It fires for real: 5.7% of on-road positions in build/collision.bin gather
# >= 123 triangles into the harness's {5.5, 34, 5.5} box (measured, 4148
# road-centroid samples).
# emulate_pipeline's derived value is both correct and in range:
#   SOUP_TYPE = SOUP_REC + SOUP_MAX*0x40 = 0x3002C040, top = +0x200 = 0x3002C240
#   REGION_LO + REGION_SZ                                        = 0x30030000
# so keep it, and assert the invariant rather than hard-coding an address again.
assert ep.SOUP_TYPE >= ep.SOUP_REC + ep.SOUP_MAX * 0x40, (
    "SOUP_TYPE at 0x%08X leaves room for only %d of %d records"
    % (ep.SOUP_TYPE, (ep.SOUP_TYPE - ep.SOUP_REC) // 0x40, ep.SOUP_MAX))
assert ep.SOUP_TYPE + ep.SOUP_MAX * 2 <= ep.REGION_LO + ep.REGION_SZ, (
    "SOUP_TYPE table runs past the mapped region")
SOUP_CAP = ep.SOUP_MAX      # the emulator's record capacity, not a guess

CARS = {}


def out(s):
    sys.stdout.write(s + "\n")
    sys.stdout.flush()


def get(car, dt):
    p = CARS.get(car)
    if p is None:
        p = RelocPipeline(dt=dt)
        p.wu(ep.SOUP_HDR + 4, ep.SOUP_REC)
        p.wu(ep.SOUP_HDR + 8, ep.SOUP_TYPE)
        p.soup_count = 0
        # Warm the translation cache so the first real frame is not a 8.7 ms
        # spike the player feels as a hitch on the start line.
        p.frame(throttle=0.0)
        CARS[car] = p
    return p


def do_seed(a):
    car = int(a[0])
    x, y, z, yaw = (float(v) for v in a[1:5])
    p = get(car, 1.0 / 60.0)
    # CTX0 holds the 4x4 frame: rows 0..2 are the basis, row 3 the position.
    import math
    c, s = math.cos(yaw), math.sin(yaw)
    rows = [[c, 0.0, -s, 0.0],
            [0.0, 1.0, 0.0, 0.0],
            [s, 0.0, c, 0.0],
            [x, y, z, 1.0]]
    for r in range(4):
        for k in range(4):
            p.wf(ep.CTX0 + 16 * r + 4 * k, rows[r][k])
    out("ok")


def do_soup(a):
    """soup <car> <n> then n * (v0 v1 v2 normal type) -- 13 numbers each.

    The harness gathers these from the REAL collision world around the car and
    re-sends only when the car crosses into a new tile, so this costs a couple
    of uploads a second rather than one a frame. Vertices arrive already in
    GAME space (the harness mirrors z and unswaps the winding the GL-space
    loader applied).
    """
    car, n = int(a[0]), int(a[1])
    p = get(car, 1.0 / 60.0)
    vals = [float(x) for x in a[2:]]
    if len(vals) < n * 13:
        out("err soup short: %d numbers for %d tris" % (len(vals), n))
        return
    tris = []
    for i in range(min(n, SOUP_CAP)):
        q = vals[i * 13:(i + 1) * 13]
        tris.append(((q[0:3], q[3:6], q[6:9]), q[9:12], int(q[12]) & 0xFFFF))
    p.set_soup(tris)
    out("ok %d" % len(tris))


# The pointer slots the EMULATOR owns inside the vehicle window. The port's
# struct is byte-identical to retail's now, so the whole window transfers as
# raw bytes -- but a host pointer means nothing in the emulator's address
# space, so these seven are preserved across the write. This is the address
# space boundary, not a data translation: no field is converted.
VEHICLE_PTRS = (0x200, 0x204, 0xCC0, 0x13F4, 0x13F8, 0x1568, 0x14D8)


def do_stepraw(a):
    """stepraw <car> <thr> <brk> <steer> <boost> <dt> <b64 window> <b64 frame>

    Takes the port's own B3VehicleFull bytes, runs retail's frame over them,
    and hands the bytes back. Nothing is marshalled field by field -- that is
    what shape parity bought. The 4x4 travels separately because retail keeps
    it in its own object (v+0x204 -> CTX0), not inside the vehicle.
    """
    car = int(a[0])
    thr, brk, steer = float(a[1]), float(a[2]), float(a[3])
    boost, dt = int(a[4]), float(a[5])
    win = base64.b64decode(a[6])
    fr = base64.b64decode(a[7]) if len(a) > 7 else None

    p = get(car, dt)
    base = ep.VEHICLE
    keep = {o: p.ru(base + o) for o in VEHICLE_PTRS}
    p.uc.mem_write(base, win)
    for o, v in keep.items():
        p.wu(base + o, v)
    if fr:
        p.uc.mem_write(ep.CTX0, fr)
        _dy = os.environ.get("B3_CTX_DY")
        if _dy:   # diagnostic: lift the body origin before retail steps
            _y = struct.unpack_from("<f", bytes(p.uc.mem_read(ep.CTX0 + 0x34, 4)))[0]
            p.uc.mem_write(ep.CTX0 + 0x34, struct.pack("<f", _y + float(_dy)))          # the frame object's 4x4

    p.dt = dt
    p.frame(throttle=thr, brake=brk, steer=steer, boost=boost)

    out("raw %s %s" % (
        base64.b64encode(bytes(p.uc.mem_read(base, len(win)))).decode(),
        base64.b64encode(bytes(p.uc.mem_read(ep.CTX0, 64))).decode()))


RANGES = {}          # car -> [(off, len), ...]


def do_ranges(a):
    """ranges <car> <n> <off> <len> ...  -- the RECOVERED byte ranges.

    The port models 82 ranges (1240 bytes) of retail's 0x1A00 vehicle object.
    Transferring the whole window would replace the 81% we have NOT recovered
    with zeros, and retail's substep loop then spins on NaN. So only these
    ranges move, at the SAME offset on both sides, and the rest stays exactly
    as this session seeded it.
    """
    car, n = int(a[0]), int(a[1])
    vals = [int(x) for x in a[2:2 + 2 * n]]
    RANGES[car] = merge_ranges([(vals[2 * i], vals[2 * i + 1]) for i in range(n)])
    out("ok %d" % len(RANGES[car]))


def do_stepr(a):
    """stepr <car> <thr> <brk> <steer> <boost> <dt> -- INPUTS ONLY.

    The vehicle is retail's between handovers (see hstate). Writing the port's
    recovered ranges here every frame is what dropped the car through the
    floor: the fields retail rebuilds and clears went stale on the port the
    moment its own step stopped running.
    """
    car = int(a[0])
    thr, brk, steer = float(a[1]), float(a[2]), float(a[3])
    boost, dt = int(a[4]), float(a[5])
    blob = base64.b64decode(a[6]) if len(a) > 6 else None
    fr = base64.b64decode(a[7]) if len(a) > 7 else None

    rs = RANGES.get(car)
    if not rs:
        out("err no ranges for car %d" % car)
        return
    p = get(car, dt)
    base = ep.VEHICLE

    # scatter the recovered ranges in, preserving the pointer slots that carry
    # emulator addresses (a host pointer means nothing over here)
    # Inputs only in the normal path: `blob` is absent because retail owns the
    # vehicle between handovers. The scatter below stays for the legacy
    # every-frame form and for the B3_STEPR_MAX bisect that located the
    # fall-through (N=0 holds the car up, N=1 drops it -- span 0 is the whole
    # rigid body, whose derived matrices retail rebuilds itself).
    if blob:
        keep = {o: p.ru(base + o) for o in VEHICLE_PTRS}
        _lim = _STEPR_MAX          # read once at import, not per call
        for _i, (off, ln, at) in enumerate(rs):
            if _lim is not None and _i >= _lim:
                break
            p.uc.mem_write(base + off, blob[at:at + ln])
        for o, v in keep.items():
            p.wu(base + o, v)
    if fr:
        p.uc.mem_write(ep.CTX0, fr)

    _cap = os.environ.get("B3_CAPTURE_AT")
    if _cap and car == 0:
        _st = globals().setdefault("_CAPN", {})
        _st[car] = _st.get(car, 0) + 1
        if _st[car] == int(_cap):
            import sys as _s
            p.write_state("build/drive_state.txt")
            with open("build/drive_soup.txt", "w") as _f:
                for _v, _n, _sf in getattr(p, "soup", []):
                    _f.write(" ".join("%.9g" % x for _q in _v for x in _q))
                    _f.write(" %.9g %.9g %.9g %d\n" % (_n[0], _n[1], _n[2], _sf))
            with open("build/drive_inputs.txt", "w") as _f:
                _f.write("%.9g %.9g %.9g %d %.9g\n" % (thr, brk, steer, boost, dt))
            _s.stderr.write("[cap] stepr #%d, %d tris\n"
                            % (_st[car], len(getattr(p, "soup", []))))
            _s.stderr.flush()
    p.dt = dt
    p.frame(throttle=thr, brake=brk, steer=steer, boost=boost)

    if os.environ.get("B3_FALL_DBG"):
        import sys as _s
        c = p.capture()
        st = globals().setdefault("_FALLST", {})
        was = st.get(car, 0.0)
        if c["vel"][1] < -4.0 and was >= -4.0:
            under = 0; near = 0
            px, py, pz = c["pos"][0], c["pos"][1], c["pos"][2]
            for verts, nrm, _surf in getattr(p, "soup", []):
                xs = [q[0] for q in verts]; zs = [q[2] for q in verts]
                ys = [q[1] for q in verts]
                if min(xs) - 1.5 <= px <= max(xs) + 1.5 and \
                   min(zs) - 1.5 <= pz <= max(zs) + 1.5:
                    near += 1
                    if max(ys) <= py + 0.2 and nrm[1] > 0.5:
                        under += 1
            _wc = [bytes(p.uc.mem_read(ep.VEHICLE + 0x820 + i*0xC0 + 0xB3, 1))[0]
                   for i in range(4)]
            _cl = [struct.unpack_from("<f", bytes(p.uc.mem_read(
                       ep.VEHICLE + 0x820 + i*0xC0 + 0x64, 4)))[0] for i in range(4)]
            if os.environ.get("B3_FALL_CAPTURE"):
                try:
                    p.write_state("build/fall_state.txt")
                    with open("build/fall_soup.txt", "w") as _f:
                        for _v, _n, _sf in getattr(p, "soup", []):
                            _f.write(" ".join("%.9g" % x for _q in _v for x in _q))
                            _f.write(" %.9g %.9g %.9g %d\n" % (_n[0], _n[1], _n[2], _sf))
                    _s.stderr.write("[fall] captured state+soup (%d tris)\n"
                                    % len(getattr(p, "soup", [])))
                except Exception as _e:
                    _s.stderr.write("[fall] capture failed: %s\n" % _e)
            _s.stderr.write("[fall] wheels contact=%s cur_len=%s\n"
                            % (_wc, ["%.3f" % x for x in _cl]))
            _s.stderr.write("[fall] BEGINS car %d pos=(%.1f %.1f %.1f) vy=%.2f "
                            "soup=%d  tris spanning the car XZ=%d, of those "
                            "upward-facing below it=%d\n"
                            % (car, px, py, pz, c["vel"][1],
                               getattr(p, "soup_count", -1), near, under))
            _s.stderr.flush()
        st[car] = c["vel"][1]
    if os.environ.get("B3_STEPR_DBG"):
        import sys as _s
        c = p.capture()
        _s.stderr.write("[stepr] in(thr=%.2f brk=%.2f str=%.2f boost=%d) "
                        % (thr, brk, steer, boost)
                        + "soup=%d pos=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f) "
                        "wheels=%s contact198=%d c212=%d\n"
                        % (getattr(p, 'soup_count', -1),
                           c["pos"][0], c["pos"][1], c["pos"][2],
                           c["vel"][0], c["vel"][1], c["vel"][2],
                           " ".join("%.2f" % w["cur"] for w in c["wheels"]),
                           struct.unpack_from("<i", bytes(p.uc.mem_read(base + 0x198, 4)))[0],
                           bytes(p.uc.mem_read(base + 0x212, 1))[0])
                        + "         wheel_count=%d class=%d flags1353=%d\n"
                        % (bytes(p.uc.mem_read(base + 0x1169, 1))[0],
                           bytes(p.uc.mem_read(base + 0x215, 1))[0],
                           bytes(p.uc.mem_read(base + 0x1353, 1))[0]))
        _s.stderr.flush()
    chunks = [bytes(p.uc.mem_read(base + off, ln)) for off, ln, _ in rs]
    out("rng %s %s" % (
        base64.b64encode(b"".join(chunks)).decode(),
        base64.b64encode(bytes(p.uc.mem_read(ep.CTX0, 64))).decode()))



CRASH = {"rs": None}

F_CHASSIS = 0x0011AEF0         # chassis-vs-world contact resolve


def do_crashranges(a):
    """crashranges <n> then n (off,len) pairs -- FUN_0011AEF0's interface."""
    n = int(a[0])
    v = [int(x) for x in a[1:1 + 2 * n]]
    CRASH["rs"] = merge_ranges([(v[2 * i], v[2 * i + 1]) for i in range(n)])
    out("ok %d" % n)


def do_crashres(a):
    """crashres <car> <b64 ranges> -> <eax> <b64 ranges>, after FUN_0011AEF0.

    Runs on the PIPELINE session for this car, because that is the one whose
    vehicle has a real veh+0x200 soup behind it -- the frozen collision set
    the resolve reads. The port uploads that soup with the same `soup` command
    physics=retail uses, so the world both sides see is the same one.

    Thiscall: ECX = the vehicle (emulate_crash_traj.py calls it this way).
    """
    rs = CRASH["rs"]
    if not rs:
        out("err no crash ranges")
        return
    car = int(a[0])
    blob = base64.b64decode(a[1])
    p = get(car, 1.0 / 60.0)
    base = ep.VEHICLE
    keep = {o: p.ru(base + o) for o in VEHICLE_PTRS}
    # B3_STEPR_MAX=N applies only the first N merged spans -- the bisect that
    # located the physics=retail fall-through (N=0 holds the car up, N=1 drops
    # it, and span 0 is the whole rigid body).  Read ONCE: this handler runs
    # ~10 times a frame (5 port-physics cars x 2 substeps, retail's own call
    # site inside FUN_0011BE50), and a getenv per call is pure overhead.
    _lim = _STEPR_MAX
    for _i, (off, ln, at) in enumerate(rs):
        if _lim is not None and _i >= _lim:
            break
        p.uc.mem_write(base + off, blob[at:at + ln])
    for o, val in keep.items():
        p.wu(base + o, val)
    try:
        eax = p.call(F_CHASSIS, regs={UC_X86_REG_ECX: base})
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    chunks = [bytes(p.uc.mem_read(base + off, ln)) for off, ln, _ in rs]
    out("cres %d %s" % (int(eax or 0),
                        base64.b64encode(b"".join(chunks)).decode()))



# --- TRAFFIC spawn-time choices -------------------------------------------
#
# The traffic feature's per-frame half is the population law FUN_001A6070,
# which reads the traffic MANAGER object this tree does not map -- so
# validate_traffic_mix.py checks it as a model replay against constants read
# from the image, and it stays on the port. What IS callable is the pair of
# choices made when a car spawns, and both have clean standalone conventions
# that validate_traffic_mix.py already exercises.
#
# Both consume the manager RNG, so the port's state/carry go over with the
# call and the advanced pair comes back: one stream, whichever side draws.
TR = {"s": None}

F_PICK_MODEL = 0x001A5E30      # ECX = class -> EAX = record pointer
F_PICK_PAINT = 0x001A5F90      # ESI = record -> EAX & 0xFF = paint index
G_MANAGER_P = 0x006137E0       # the manager's event-TDESC pointer
G_RNG_STATE = 0x00649B28       # manager+0x36348
G_RNG_CARRY = 0x00649B2C       # manager+0x3634C
TR_SCRATCH = ea.OTHER + 0x1000   # the session's own scratch, already mapped
# The six list triples inside the event TDESC. Read from
# tools/validate_traffic_mix.py, which pins them against the real function --
# NOT guessed. An invented set sends FUN_001A5E30 at a list that is not there,
# EAX comes back outside the synthetic record array, and the index derived
# from it walked the caller's model table straight off the end.
CLASS_LIST_OFFSET = {1: 0x54, 2: 0x60, 3: 0x78, 4: 0x84, 5: 0x6C, 0x0B: 0x90}


def _tr_session():
    """A Session, reusing its OTHER region for the synthetic records.

    validate_traffic_mix.py builds them at exactly this address, so the layout
    the two agree on is the suite's -- and mapping a fresh region instead just
    collides (UC_ERR_MAP), because Session already covers it.
    """
    if TR["s"] is None:
        TR["s"] = ea.Session()
    return TR["s"]


def do_tmodel(a):
    """tmodel <state> <carry> <cls> <total> <w0..wn> -> tm <idx> <state> <carry>"""
    st, cy, cls, total = int(a[0]), int(a[1]), int(a[2]), int(a[3])
    weights = [int(x) for x in a[4:]]
    s = _tr_session()
    records = TR_SCRATCH + 0x400
    tdesc = TR_SCRATCH + 0x2000
    s.wu(G_MANAGER_P, tdesc)
    for i, w in enumerate(weights):
        s.wu(records + i * 0x18 + 0x10, w)
    off = CLASS_LIST_OFFSET.get(cls)
    if off is None:
        out("err bad class %d" % cls)
        return
    s.wu(tdesc + off, records)
    s.wu(tdesc + off + 4, len(weights))
    s.wu(tdesc + off + 8, total)
    s.wu(G_RNG_STATE, st)
    s.wu(G_RNG_CARRY, cy)
    try:
        s.call(F_PICK_MODEL, regs={UC_X86_REG_ECX: cls})
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    eax = s.uc.reg_read(UC_X86_REG_EAX)
    if eax == 0:
        idx = -1
    else:
        # The result must be a record IN the array we built; anything else
        # means the call read a list we did not set up, and returning the
        # subtraction anyway hands the caller a garbage index.
        delta = eax - records
        if delta < 0 or delta % 0x18 or delta // 0x18 >= len(weights):
            out("err tmodel: EAX %#x outside the record array" % eax)
            return
        idx = delta // 0x18
    out("tm %d %d %d" % (idx, s.ru(G_RNG_STATE), s.ru(G_RNG_CARRY)))


def do_tpaint(a):
    """tpaint <state> <carry> <c0..c7> -> tp <idx> <state> <carry>"""
    st, cy = int(a[0]), int(a[1])
    cols = [int(x) & 0xFF for x in a[2:10]]
    s = _tr_session()
    record = TR_SCRATCH + 0x600
    s.uc.mem_write(record + 8, bytes(bytearray(cols)))
    s.wu(G_RNG_STATE, st)
    s.wu(G_RNG_CARRY, cy)
    try:
        s.call(F_PICK_PAINT, regs={UC_X86_REG_ESI: record})
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    out("tp %d %d %d" % (s.uc.reg_read(UC_X86_REG_EAX) & 0xFF,
                         s.ru(G_RNG_STATE), s.ru(G_RNG_CARRY)))



# --- HUD event ticker ------------------------------------------------------
#
# FUN_0004D310, the ticker's per-frame update: six category probes over the
# score object's B3CatRecords, then the row walk (lifetime, fade-out,
# stacking). tools/emulate_hud_ticker.py already runs it for real with only
# the DRAWING stubbed, and its TickerTrace exposes exactly the two halves of
# the interface -- set_record() writes a B3CatRecord, live_rows() reads the
# element's row slots back -- so this drives that rather than rebuilding it.
#
# The interface is a shared object at identical offsets on both sides:
# B3HudTickIn is the score-object category record (value +0x00, clock +0x04,
# prev +0x08, open +0x10, tier +0x11, prev_tier +0x12, count +0x13) and
# B3TickRow is the element's row slot (obj+0x570 + i*0x28: live +0x00,
# timer +0x08, y +0x10, tier +0x18, flash +0x1C, phase +0x20, pulse +0x24).
HUD = {"t": None}


def do_hudtick(a):
    """hudtick <dt> then 6 records of 7 fields -> ht <n> then 7 rows + order.

    Fields per record, in B3HudTickIn order:
        value clock prev open tier prev_tier count
    """
    import emulate_hud_ticker as eh
    t = HUD["t"]
    if t is None:
        t = HUD["t"] = eh.TickerTrace()
        t.reset()
    dt = float(a[0])
    vals = a[1:]
    if len(vals) < 6 * 7:
        out("err hudtick: want 42 fields, got %d" % len(vals))
        return
    for i, (_idx, off, _thr, _label) in enumerate(eh.ROWS):
        f = vals[i * 7:(i + 1) * 7]
        t.set_record(off, value=float(f[0]), clock=float(f[1]),
                     prev=float(f[2]), open_=int(float(f[3])),
                     tier=int(float(f[4])), prev_tier=int(float(f[5])),
                     count=int(float(f[6])))
    try:
        t.update(dt)
        rows = t.live_rows()
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    # every slot, then the live order newest-first
    body = []
    for i in range(7):
        slot = eh.OBJ + eh.OBJ_ROWS + i * eh.OBJ_ROW_STRIDE
        body += ["%d" % eh.u32(t.uc, slot + 0x00),
                 "%.9g" % eh.f32(t.uc, slot + 0x08),
                 "%.9g" % eh.f32(t.uc, slot + 0x10),
                 "%d" % eh.i8(t.uc, slot + 0x18),
                 "%.9g" % eh.f32(t.uc, slot + 0x1C),
                 "%.9g" % eh.f32(t.uc, slot + 0x20),
                 "%.9g" % eh.f32(t.uc, slot + 0x24)]
    order = [str(r["row"]) for r in rows if 0 <= r["row"] < 7]
    out("ht %d %s %s" % (len(order), " ".join(body), " ".join(order)))



# --- SFX emitters ----------------------------------------------------------
#
# tools/emulate_sfx.py runs the real emitter and captures its call into
# PlaySound3D (0x001CD8D0), which is the boundary: everything up to that call
# is the game's event law -- which wave, what gain, what playback rate -- and
# everything past it is the Xbox's 3D DirectSound voice manager, which has no
# meaning off the console. The port reproduces the law and mixes the result
# itself, so the switch hands over exactly the captured {wave, gain, pitch}.
#
# FUN_00141010, the trigger the HUD ticker fires, is NOT this: it is stubbed
# even in emulate_hud_ticker because it walks into that voice manager.
SFX = {"s": None}

SFX_KINDS = ["ecx", "eax_x0", "eax_m8", "st_obj_m8", "st4"]


def do_sfxfire(a):
    """sfxfire <addr> <kind> <mag> -> sx <n> then n * (wave gain pitch)."""
    import emulate_sfx as es
    if SFX["s"] is None:
        SFX["s"] = es.Sfx()
    addr, kind, mag = int(a[0]), int(a[1]), float(a[2])
    if not 0 <= kind < len(SFX_KINDS):
        out("err sfxfire: bad kind %d" % kind)
        return
    try:
        voices, err = SFX["s"].fire(addr, SFX_KINDS[kind], mag)
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    if err:
        out("err sfxfire: %s" % str(err).replace("\n", " ")[:100])
        return
    parts = []
    for v in voices[:4]:
        parts += ["%s" % (v.get("wave") or "?"), "%.9g" % v.get("gain", 0.0),
                  "%.9g" % v.get("pitch", 1.0)]
    out("sx %d %s" % (min(len(voices), 4), " ".join(parts)))



HANDOVER = {"rs": None}


def do_hranges(a):
    """hranges <n> then n (off,len) pairs -- the HANDOVER set.

    Distinct from `ranges`, which is the read-back set. Retail owns the
    vehicle while it drives it; the port pushes state across once and mirrors
    after that, so this list deliberately omits every field retail rebuilds
    (inv_frame, inv_inertia_world) or clears (the substep accumulators).
    """
    n = int(a[0])
    v = [int(x) for x in a[1:1 + 2 * n]]
    HANDOVER["rs"] = merge_ranges([(v[2 * i], v[2 * i + 1]) for i in range(n)])
    out("ok %d" % n)


def do_hstate(a):
    """hstate <car> <b64 state> <b64 frame> -- give retail the wheel."""
    rs = HANDOVER["rs"]
    if not rs:
        out("err no handover ranges")
        return
    car = int(a[0])
    blob = base64.b64decode(a[1])
    fr = base64.b64decode(a[2]) if len(a) > 2 else None
    p = get(car, 1.0 / 60.0)
    base = ep.VEHICLE
    keep = {o: p.ru(base + o) for o in VEHICLE_PTRS}
    _lim = os.environ.get("B3_HSTATE_MAX")
    _lim = int(_lim) if _lim else None
    for _i, (off, ln, at) in enumerate(rs):
        if _lim is not None and _i >= _lim:
            break
        p.uc.mem_write(base + off, blob[at:at + ln])
    for o, val in keep.items():
        p.wu(base + o, val)
    if fr:
        p.uc.mem_write(ep.CTX0, fr)
    if os.environ.get("B3_HSTATE_DBG"):
        import sys as _s
        def _f4(o):
            return struct.unpack_from("<4f", bytes(p.uc.mem_read(base + o, 16)))
        _s.stderr.write("[hstate] after: vel=%s dir=%s omega=%s\n"
                        % (["%.3f" % x for x in _f4(0xB0)],
                           ["%.3f" % x for x in _f4(0xC0)],
                           ["%.3f" % x for x in _f4(0xD0)]))
        _s.stderr.flush()
    out("ok %d" % len(rs))



def do_stepra(a):
    """stepra <car> <boost> <dt> <b64 racecar+ai-object> -- DRIVER + STEP
    in ONE session.

    When ai and physics are BOTH retail, running them in two Unicorn sessions
    is the port's artifact, not the game's: retail has FUN_00105340 write
    v+0x1400 and FUN_0011BE50 read it in the SAME vehicle object. Split across
    sessions, the port has to shuttle the driver's fields between them every
    frame, the AI ends up reading a mirror that is one frame old and 23% of
    the object, and its view goes incoherent -- 1026 rpm at 21 m/s in gear 2.
    Throttle then reached the physics step in 7% of frames against 41% with
    the port's own AI.

    Here the driver runs against the pipeline's own vehicle, so its output IS
    the input the step consumes. No shuttle, no contention.
    """
    car, boost, dt = int(a[0]), int(a[1]), float(a[2])
    # AI["car"] already carries the racecar ranges AND the AI-object ranges,
    # the latter pre-biased by 0x1A00 on the C side, so one list covers both.
    cr = AI["car"]
    if not cr:
        out("err no ai ranges")
        return
    rs = RANGES.get(car)
    if not rs:
        out("err no ranges for car %d" % car)
        return
    rcb = base64.b64decode(a[3])
    p = get(car, dt)
    base = ep.VEHICLE

    # The pipeline seeds racecar fields the PHYSICS depends on -- notably
    # +0x1920, which emulate_pipeline sets to 0 ("mode 0: normal") while the
    # port's B3AiCar carries race_mode = 1 at the same offset. Writing the AI
    # view straight over it changes the physics gate, so those are held back.
    _keep = {int(x, 0) for x in
             os.environ.get("B3_STEPRA_KEEP", "0x1920").split(",") if x.strip()}
    for off, ln, at in cr:
        if off in _keep:
            continue
        p.uc.mem_write(ep.RACECAR + off, rcb[at:at + ln])
    # the driver reaches the racecar through v+0x1568
    p.wu(base + 0x1568, ep.RACECAR)
    # Clear the driver's OUTPUTS before it runs, exactly as the port's own
    # b3_ai_drive does at entry. FUN_00105340 does not clear the brake on its
    # throttle path -- line 0x1404 is only written when traffic_class != 0,
    # which is false for a racer -- so in retail something upstream resets the
    # inputs each frame. Without that reset the brake STICKS: once it is 1.0
    # it is read back, fed to the step, and read again next frame, and the car
    # brakes to a standstill and never recovers (883 of 1310 frames braking).
    # v+0x1408 is deliberately NOT cleared: it is the slew limiter's memory,
    # and v+0x14C8 is the live gear, which retail leaves alone.
    # NOTE: deliberately not clearing the driver's outputs here. The port's
    # b3_ai_drive clears them at entry, but doing the same in this session
    # measured WORSE (max 23 mph vs 50), and 0x1400 is read back as the
    # dither's previous-throttle memory, so a blanket clear is wrong. The
    # brake stickiness on the racer path (0x1404 is only cleared when
    # traffic_class != 0) is real and still unexplained -- see the ledger.
    try:
        dispatch_head(p.uc, base)
        p.call(F_AI_DRIVER, regs={UC_X86_REG_EDI: base})
    except Exception as exc:                                # noqa: BLE001
        out("err driver %s" % str(exc).replace("\n", " ")[:100])
        return

    # the RAW throttle: p.frame() is emulate_pipeline's FUN_00104D30 input
    # glue and derives v+0x1400 = min(1, raw * v[0x13BC]) itself.
    _rf = lambda A: struct.unpack_from("<f", bytes(p.uc.mem_read(A, 4)))[0]
    thr = _rf(base + 0x1414)
    brk = _rf(base + 0x1404)
    steer = _rf(base + 0x1408)
    bits = bytes(p.uc.mem_read(base + 0x13FC, 1))[0]
    p.dt = dt
    p.frame(throttle=thr, brake=brk, steer=steer,
            boost=1 if (bits & 4) else boost)

    if os.environ.get("B3_STEPRA_DBG"):
        import sys as _s
        c = p.capture()
        _s.stderr.write("[stepra] car%d drv(thr=%.2f brk=%.2f str=%.2f) "
                        "pos=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f) spd=%.1f "
                        "gear=%d soup=%d\n"
                        % (car, thr, brk, steer, c["pos"][0], c["pos"][1],
                           c["pos"][2], c["vel"][0], c["vel"][1], c["vel"][2],
                           c["speed"], c["gear"], getattr(p, "soup_count", -1))
                        + "          rev=%.2f arm=%.2f hold=%.2f stop=%d tgt=%.1f\n"
                        % (_rf(base + 0x157C), _rf(base + 0x1578),
                           _rf(base + 0x1570),
                           bytes(p.uc.mem_read(base + 0x1552, 1))[0],
                           _rf(ep.RACECAR + 0x23C4)))
        _s.stderr.flush()
    veh = b"".join(bytes(p.uc.mem_read(base + off, ln)) for off, ln, _ in rs)
    aiv = b"".join(bytes(p.uc.mem_read(base + off, ln)) for off, ln, _ in AI["veh"])
    out("rnga %s %s %s" % (
        base64.b64encode(veh).decode(),
        base64.b64encode(bytes(p.uc.mem_read(ep.CTX0, 64))).decode(),
        base64.b64encode(aiv).decode()))


CARCOL = {"s": None}


def do_ccol(a):
    """ccol <crA> <tyA> <crB> <tyB> then per slot: rb, extra, hull, frame (b64)

    Runs retail's own car-vs-car resolve -- FUN_001121F0 when both bodies are
    alive, FUN_00113960 when either is wrecked -- over the PORT's bytes.

    `rb` is B3RigidBody, which is now retail-shaped, so it scatters straight in
    at veh+0x00. `extra` is bbmax/bbmin/mass at veh+0x1D0/+0x1E0/+0x1F0. `hull`
    is B3CarHull, which is now byte-identical to the retail 0x600 record, so it
    goes in as-is and the session relinks it exactly as FUN_00122830 does.
    Nothing is converted in either direction.
    """
    cr = [int(a[0]), int(a[2])]
    ty = [int(a[1]), int(a[3])]
    blobs = [base64.b64decode(x) for x in a[4:12]]

    # One session, reused. Constructing an ec.Session per contact pair costs
    # ~6 ms of ELF load and mapping and dumps Unicorn's translation cache every
    # time -- measured at ~20 ms per resolve, which ran the game ten times
    # slower than the RE path. Reused, the cache stays warm.
    s = CARCOL["s"]
    if s is None:
        s = CARCOL["s"] = ec.Session()
    for slot in (0, 1):
        rb, extra, hull, fr = blobs[slot * 4:slot * 4 + 4]
        mat = [list(struct.unpack_from("<4f", fr, 16 * r)) for r in range(4)]
        st = dict(hull=hull, frame=mat, type=ty[slot], crashed=cr[slot],
                  bbmax=list(struct.unpack_from("<4f", extra, 0)),
                  bbmin=list(struct.unpack_from("<4f", extra, 16)),
                  mass=struct.unpack_from("<f", extra, 32)[0])
        s.seed(slot, st)
        # the port's own dynamics, byte for byte, over the seeded body
        veh = ec.VEH_A if slot == 0 else ec.VEH_B
        s.uc.mem_write(veh, rb)

    g = s.resolve_wreck() if (cr[0] or cr[1]) else s.resolve_alive()

    if os.environ.get("B3_CCOL_SOLID") and g.get("hit") and g.get("impact", 0) > 2000.0:
        import sys as _s
        _rf = lambda A: struct.unpack_from("<f", bytes(s.uc.mem_read(A, 4)))[0]
        _s.stderr.write("[solid] impact=%.0f n=(%.2f %.2f %.2f) | EMU impA=(%.0f %.0f %.0f) "
                        "impB=(%.0f %.0f %.0f) | mA=%.0f mB=%.0f crA=%d crB=%d\n"
                        % (g.get("impact",0.0), g["normal"][0], g["normal"][1],
                           g["normal"][2],
                           _rf(ec.VEH_A+0x110), _rf(ec.VEH_A+0x114), _rf(ec.VEH_A+0x118),
                           _rf(ec.VEH_B+0x110), _rf(ec.VEH_B+0x114), _rf(ec.VEH_B+0x118),
                           _rf(ec.VEH_A+0x1F0), _rf(ec.VEH_B+0x1F0), cr[0], cr[1]))
        _s.stderr.flush()
    if os.environ.get("B3_CCOL_DBG") and g.get("hit"):
        import sys as _s
        _rf = lambda A: struct.unpack_from("<f", bytes(s.uc.mem_read(A, 4)))[0]
        ia = sum(abs(_rf(ec.VEH_A + 0x110 + 4*k)) for k in range(3))
        ib = sum(abs(_rf(ec.VEH_B + 0x110 + 4*k)) for k in range(3))
        if ia + ib < 1e-3:          # a contact that moved NOTHING
            def dump(base, tag):
                inv = [[_rf(base + 0x70 + 16*r + 4*c) for c in range(4)]
                       for r in range(4)]
                det = (inv[0][0]*(inv[1][1]*inv[2][2] - inv[1][2]*inv[2][1])
                     - inv[0][1]*(inv[1][0]*inv[2][2] - inv[1][2]*inv[2][0])
                     + inv[0][2]*(inv[1][0]*inv[2][1] - inv[1][1]*inv[2][0]))
                _s.stderr.write("[ccoldbg]  %s vel=(%.2f %.2f %.2f) spd=%.2f "
                                "mass=%.0f invI=(%.5f %.5f %.5f) invFrame_det=%.4f "
                                "asleep=%d crashed=%d\n"
                                % (tag, _rf(base+0xB0), _rf(base+0xB4),
                                   _rf(base+0xB8), _rf(base+0xBC),
                                   _rf(base+0x1F0), _rf(base+0x40),
                                   _rf(base+0x54), _rf(base+0x68), det,
                                   bytes(s.uc.mem_read(base+0x20E,1))[0],
                                   bytes(s.uc.mem_read(base+0x210,1))[0]))
            _s.stderr.write("[ccoldbg] HIT but ZERO impulse: impact=%.0f n=(%.2f %.2f %.2f)\n"
                            % (g.get("impact", 0.0), g["normal"][0],
                               g["normal"][1], g["normal"][2]))
            dump(ec.VEH_A, "A"); dump(ec.VEH_B, "B")
            _s.stderr.flush()

    # Retail's own slam report -- the game-context vtable+0x64 notify the
    # session hooks at STUB_SLAM: (kind, attacker_veh, victim_veh, strength).
    # The port's B3CarContact carries these as HARNESS-side fields past the
    # 0x30 retail window (event/attacker_is_b/strength), and the reply used
    # to stop at the window, so on carcol=retail those fields were stack
    # garbage in the caller -- carcol_slam_racers() gates on event >= 1, and
    # the slam was never reported, which is why the all-retail build never
    # produced a takedown.  Send retail's own values.
    slams = g.get("slams") or []
    if os.environ.get("B3_CCOL_SLAMDBG") and g.get("hit"):
        import sys as _s
        _s.stderr.write("[slamdbg] hit=1 impact=%.0f slam_class=%d slams=%r cr=%r\n"
                        % (g.get("impact",0), g.get("slam",0), slams, cr))
        _s.stderr.flush()
    if slams:
        # a hard contact reports TWICE -- the rub (kind 1) AND the slam
        # (kind 3..6).  One event goes to the harness per contact, and the
        # port classifier picks the slam over the rub, so forward the
        # HIGHEST kind; taking slams[0] delivered the rub every time and no
        # slam was ever reported.
        kind, att, _vic, strength = max(slams, key=lambda t: t[0])
        att_is_b = 1 if att == ec.VEH_B else 0
    else:
        kind, att_is_b, strength = 0, 0, 0.0
    out("cc %s %s %s %d %d %d %d %.9g" % (
        base64.b64encode(bytes(s.uc.mem_read(ec.PAIR, 0x30))).decode(),
        base64.b64encode(bytes(s.uc.mem_read(ec.VEH_A, len(blobs[0])))).decode(),
        base64.b64encode(bytes(s.uc.mem_read(ec.VEH_B, len(blobs[4])))).decode(),
        int(g.get("crash_a", 0)), int(g.get("crash_b", 0)),
        int(kind), att_is_b, strength))


TD = {"w": None, "ranges": None}


def do_tdranges(a):
    """tdranges <n> <off> <len> ... -- the recovered B3TdCar ranges."""
    n = int(a[0])
    v = [int(x) for x in a[1:1 + 2 * n]]
    TD["ranges"] = [(v[2 * i], v[2 * i + 1]) for i in range(n)]
    out("ok %d" % n)


def do_tdslam(a):
    """tdslam <ncars> <attacker> <victim> <strength> <type> <b64 car blobs>

    Retail's own slam gate, FUN_00197BE0, over the port's racecar bytes.
    td_rules is STATEFUL, so the world is built once and kept: each call
    scatters the port's cars in at retail's offsets and reads the verdict back.
    """
    clock = float(a[0])
    ncars, att, vic = int(a[1]), int(a[2]), int(a[3])
    strength, type_byte = float(a[4]), int(a[5])
    blob = base64.b64decode(a[6])
    rs = TD["ranges"]
    if not rs:
        out("err no tdcar ranges")
        return
    w = TD["w"]
    if w is None or w.ncars != ncars:
        w = TD["w"] = et.World(ncars=ncars)
        w.ncars = ncars
        for i in range(ncars):
            w.seed_car(i)
    per = sum(l for _, l in rs)
    for i in range(ncars):
        base = w.rc(i)
        pos = i * per
        for off, ln in rs:
            w.uc.mem_write(base + off, blob[pos:pos + ln])
            pos += ln
    # The AGGRESSOR field (+0x16BC) is a racecar POINTER in retail and a slot
    # INDEX in the port (a shape clash the raw scatter copies verbatim), so it
    # is translated on the way IN (index -> emulated pointer) ...
    # retail stores the RACECAR pointer there ([+0x16BC]+0x1920 is a racecar
    # offset in FUN_00105340's read of it), so translate with rc(); accept
    # pv() too on the way back in case another writer uses it.
    for i in range(ncars):
        base = w.rc(i)
        idx = struct.unpack("<i", bytes(w.uc.mem_read(base + 0x16BC, 4)))[0]
        ptr = w.rc(idx) if 0 <= idx < ncars else 0
        w.uc.mem_write(base + 0x16BC, struct.pack("<I", ptr))
    # ... and the race clock is a GLOBAL the world never advanced: retail
    # stamped aggressor_time = 0.00, and the port's crash-wait window
    # (clock <= stamp + MAX_CRASH_WAIT) could then never pass -- the whole
    # slam -> wreck -> takedown chain silently died here.
    w.set_clock(clock)
    ok = w.slam_gate(att, vic, strength, type_byte)
    # ... and back OUT (pointer -> index) after the call.
    for i in range(ncars):
        base = w.rc(i)
        ptr = struct.unpack("<I", bytes(w.uc.mem_read(base + 0x16BC, 4)))[0]
        idx = -1
        for k in range(ncars):
            if ptr in (w.rc(k), w.pv(k)):
                idx = k
                break
        w.uc.mem_write(base + 0x16BC, struct.pack("<i", idx))
    chunks = []
    for i in range(ncars):
        base = w.rc(i)
        chunks += [bytes(w.uc.mem_read(base + off, ln)) for off, ln in rs]
    out("td %d %s" % (1 if ok else 0,
                      base64.b64encode(b"".join(chunks)).decode()))


SC = {"ranges": None}

# regparm3 entries the score feature switches on
F_SCORE_CONTACT = 0x00197920   # near-miss cancel
F_SCORE_MARK    = 0x001979E0   # rubbing contact mark



def do_scranges(a):
    n = int(a[0])
    v = [int(x) for x in a[1:1 + 2 * n]]
    SC["ranges"] = [(v[2 * i], v[2 * i + 1]) for i in range(n)]
    out("ok %d" % n)


def do_score(a):
    """score <which> <arg> <b64 recovered ranges> -> the ranges, after retail.

    The port's B3ScoreEvents is retail-shaped over the score object's first
    0x600, so its bytes go in at the game's own offsets, retail's own
    regparm3 entry runs, and the same ranges come back. Nothing converted.
    """
    which, arg = a[0], int(a[1])
    blob = base64.b64decode(a[2])
    rs = SC["ranges"]
    if not rs:
        out("err no score ranges")
        return
    img = bytearray(vs.SCORE_SZ)
    pos = 0
    for off, ln in rs:
        img[off:off + ln] = blob[pos:pos + ln]
        pos += ln
    func = F_SCORE_CONTACT if which == "contact" else F_SCORE_MARK
    if which == "contact":          # regparm3: EAX=0, EDX=obj, ECX=score
        res, err = vs.run_regparm3(bytes(img), func, 0, vs.OBJ, vs.SCORE, "default")
    else:                            # regparm3: EAX=score, EDX=_, ECX=car
        res, err = vs.run_regparm3(bytes(img), func, vs.SCORE, 0, arg, "default")
    if err:
        out("err %s" % str(err).replace("\n", " ")[:120])
        return
    chunks, pos = [], 0
    for off, ln in rs:
        chunks.append(bytes(res[off:off + ln]))

    # run_regparm3 builds a fresh Unicorn every call -- correct for a test,
    # a leak here: the game issues thousands of score events and the sidecar
    # died after ~1700. Reference counting alone does not release the emulator
    # promptly, so collect on a cadence.
    SC["n"] = SC.get("n", 0) + 1
    if SC["n"] % 64 == 0:
        gc.collect()

    out("sc %s" % base64.b64encode(b"".join(chunks)).decode())


CAM = {"n": 0}


def do_cam(a):
    """cam <12 car-matrix floats> <speed> <boost_ramp> <dt> <yaw> <pitch> <look_back>

    Retail's own follow camera, FUN_0015E550, over the port's car transform.
    The camera's interface IS scalars and a matrix -- that is the function's
    real signature, so nothing is being converted here; there is no second
    struct shape in the middle.
    """
    v = [float(x) for x in a[:17]]
    look_back = int(float(a[17])) if len(a) > 17 else 0
    rows = [tuple(v[i * 3:i * 3 + 3]) for i in range(4)]
    speed, ramp, dt, yaw, pitch = v[12], v[13], v[14], v[15], v[16]
    try:
        g = vc.follow_update(rows, speed, dt, yaw, pitch,
                             boost_ramp=ramp, look_back=look_back)
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    # follow_update now reuses one warmed Unicorn (emulate_tdfx_camera's
    # _cam_session), so there is no per-frame instance to leak and no reason to
    # force a collection.  The old cadence collect was itself a cost: the pause
    # landed on whichever command happened to be running, which is where ccol's
    # 40.9 ms and hudtick's 59.7 ms maxima came from.
    CAM["n"] += 1
    # follow_update returns the orientation as a QUATERNION, not a basis
    eye = list(g["eye"])
    quat = list(g["quat"])
    out("cam " + " ".join("%.9g" % f for f in
                          eye + quat + [g["fov"], g["pitch"], g["yaw"]]))



def merge_ranges(rs):
    """Coalesce ADJACENT (off,len) pairs into spans over the same payload.

    The payload arrives as the ranges concatenated in offset order, so a run
    of ranges that touch is one contiguous slice on both sides and can move in
    a single mem_write/mem_read. That matters: each Unicorn call crosses into
    Python, and the AI path alone makes 101 of them per driver invocation, 360
    times a second -- measured at 96 calls/s against 360 before the vehicle
    joined the transfer, a slowdown that tracked the range COUNT, not the byte
    count.

    Only genuinely adjacent ranges are merged -- gap == 0, never a tolerance.
    The bytes in a gap are `_pad` here and REAL seeded state in the emulator;
    writing our zeros over them is what hangs retail's substep loop.

    Returns [(off, length, payload_offset)].
    """
    out = []
    pos = 0
    for off, ln in rs:
        if out and off == out[-1][0] + out[-1][1]:
            out[-1][1] += ln
        else:
            out.append([off, ln, pos])
        pos += ln
    return [tuple(x) for x in out]


AI = {"s": None, "car": None, "veh": None, "ms": []}
AI_OBJECT_OFFSET = 0x1A00   # retail embeds the AI object here

from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX,
                               UC_X86_REG_EDI, UC_X86_REG_ESI)

def dispatch_head(uc, veh):
    """FUN_00104D30's pre-driver work: the stop flag is cleared first."""
    uc.mem_write(veh + 0x1552, b"\x00")


F_AI_DRIVER = 0x00105340       # the AI racer driver
F_TRAFFIC_DRIVER = 0x00105150  # the reduced traffic driver       # the AI racer driver


def do_airanges(a):
    """airanges <nc> <nv>, then nc racecar (off,len) pairs, then nv vehicle."""
    nc, nv = int(a[0]), int(a[1])
    v = [int(x) for x in a[2:2 + 2 * (nc + nv)]]
    AI["car"] = merge_ranges([(v[2 * i], v[2 * i + 1]) for i in range(nc)])
    AI["veh"] = merge_ranges([(v[2 * (nc + i)], v[2 * (nc + i) + 1])
                              for i in range(nv)])
    out("ok %d %d" % (nc, nv))


def do_ai(a):
    """ai <which> <clock> <dt> <b64 racecar> <b64 vehicle> <b64 frame>.

    <which>: 0 = FUN_00105340, the AI racer driver (EDI = the vehicle)
             1 = FUN_00105150, the reduced traffic driver (thiscall,
                 ECX = the vehicle; it follows v+0x1568 to the racecar and
                 reads the target angle/speed at racecar+0x23C0/0x23C4, which
                 the AI-object ranges put there).

    FUN_00105340 over the port's own bytes, each object at its own base:
    B3AiCar scatters into the RACECAR and B3VehicleFull into the physics
    VEHICLE, both at retail's offsets. The driver reads speed/heading/gear/rpm
    out of the vehicle and writes throttle/brake/steer back into it, and sets
    the boost latch in the racecar, so both come back. Nothing is converted.
    """
    cr, vr = AI["car"], AI["veh"]
    if not cr or not vr:
        out("err no ai ranges")
        return
    which = int(a[0])
    # DAT_0060EA20 / DAT_0060EA1C are runtime globals, not struct fields, so
    # they ride the line rather than the scatter transfer.  The driver's launch
    # dither parks a deadline (clock + 2.0) in v+0x1574 and holds the throttle
    # down until the clock passes it -- with a frozen clock it never does.
    clock, dt_in = float(a[1]), float(a[2])
    a = a[2:]                      # a[1]=car a[2]=veh a[3]=frame, as before
    car = base64.b64decode(a[1])
    veh = base64.b64decode(a[2])
    frm = base64.b64decode(a[3]) if len(a) > 3 else None
    sess = AI["s"]
    if sess is None:
        sess = AI["s"] = ea.Session()
        ea.seed_car(sess)
    for off, ln, at in cr:
        sess.uc.mem_write(ea.RC + off, car[at:at + ln])
    for off, ln, at in vr:
        sess.uc.mem_write(ea.VEH + off, veh[at:at + ln])
    if frm and not os.environ.get('B3_AI_NOFRAME'):
        # the RenderWare frame behind vehicle+0x204; seed_car writes the same
        # four rows there, and the driver reads its forward/right from it
        sess.uc.mem_write(ea.FRAME, frm)
    # DAT_0060EA20 / DAT_0060EA1C: the race clock and frame dt.  These are
    # globals, so the scatter transfer cannot carry them and ea.Session()
    # leaves them frozen at their seed values.  FUN_00105340's launch dither
    # parks a DEADLINE of clock + 2.0 in v+0x1574 and holds the throttle at
    # zero until the clock passes it -- against a frozen clock, forever.  The
    # combined path never hit this because the physics session ticks its own
    # clock, which is why ai=retail only ever drove with physics=retail.
    sess.wf(ea.G_CLOCK, clock)
    if dt_in > 0.0:
        sess.wf(ea.G_DT, dt_in)
    try:
        # FUN_00105340 takes the VEHICLE in EDI. Without it the driver runs
        # against whatever EDI held and writes nothing at all -- every car
        # came back with thr/brk/str = 0 and coasted to a standstill.
        # validate_ai.py has always called it this way (regs={EDI: VEH}).
        dispatch_head(sess.uc, ea.VEH)
        if which:
            sess.call(F_TRAFFIC_DRIVER, regs={UC_X86_REG_ECX: ea.VEH})
        else:
            sess.call(F_AI_DRIVER, regs={UC_X86_REG_EDI: ea.VEH})
    except Exception as exc:                                # noqa: BLE001
        out("err %s" % str(exc).replace("\n", " ")[:110])
        return
    if os.environ.get("B3_AI_DBG"):
        import sys as _s
        _rf = lambda A: struct.unpack_from("<f", bytes(sess.uc.mem_read(A, 4)))[0]
        _s.stderr.write("[aidrv] speed=%.2f tgt_spd=%.2f tgt_ang=%.2f gear=%d "
                        "rpm=%.0f -> thr=%.2f brk=%.2f str=%.2f\n"
                        % (_rf(ea.VEH + 0xBC), _rf(ea.RC + 0x23C4),
                           _rf(ea.RC + 0x23C0), _ri(ea.VEH + 0x14C8),
                           _rf(ea.VEH + 0x149C) * 9.549296,
                           _rf(ea.VEH + 0x1400), _rf(ea.VEH + 0x1404),
                           _rf(ea.VEH + 0x1408))
                        + "        IN vBC=%.3f v13AC=%.3f v1470=%.1f v149C=%.1f "
                          "v14C8=%d v1524=%d v1534=%.2f v1550=%d | RC 23C0=%.2f "
                          "23C4=%.2f 2188=%d 2413=%d 2414=%d 23F8=%d 190C=%.2f "
                          "1920=%d 134C=%d | OUT raw=%.2f\n"
                        % (_rf(ea.VEH + 0xBC), _rf(ea.VEH + 0x13AC),
                           _rf(ea.VEH + 0x1470), _rf(ea.VEH + 0x149C),
                           _ri(ea.VEH + 0x14C8), _ri(ea.VEH + 0x1524),
                           _rf(ea.VEH + 0x1534),
                           bytes(sess.uc.mem_read(ea.VEH + 0x1550, 1))[0],
                           _rf(ea.RC + 0x23C0), _rf(ea.RC + 0x23C4),
                           bytes(sess.uc.mem_read(ea.RC + 0x2188, 1))[0],
                           bytes(sess.uc.mem_read(ea.RC + 0x2413, 1))[0],
                           bytes(sess.uc.mem_read(ea.RC + 0x2414, 1))[0],
                           _ri(ea.RC + 0x23F8), _rf(ea.RC + 0x190C),
                           _ri(ea.RC + 0x1920), _ri(ea.RC + 0x134C),
                           _rf(ea.VEH + 0x1414))
                        + "        gates: rev_timer=%.2f stuck_arm=%.2f "
                          "brake_hold=%.2f lsdm=%d stop=%d drift=%d "
                          "crash_timer=%.2f race_mode=%d\n"
                        % (_rf(ea.VEH + 0x157C), _rf(ea.VEH + 0x1578),
                           _rf(ea.VEH + 0x1570),
                           bytes(sess.uc.mem_read(ea.VEH + 0x1550, 1))[0],
                           bytes(sess.uc.mem_read(ea.VEH + 0x1552, 1))[0],
                           _ri(ea.VEH + 0x1524), _rf(ea.RC + 0x190C),
                           _ri(ea.RC + 0x1920)))
        _s.stderr.flush()
    c_out = b"".join(bytes(sess.uc.mem_read(ea.RC + off, ln)) for off, ln, _ in cr)
    v_out = b"".join(bytes(sess.uc.mem_read(ea.VEH + off, ln)) for off, ln, _ in vr)
    out("ai %s %s" % (base64.b64encode(c_out).decode(),
                      base64.b64encode(v_out).decode()))


def do_step(a):
    car = int(a[0])
    thr, brk, steer = float(a[1]), float(a[2]), float(a[3])
    boost, dt = int(a[4]), float(a[5])
    p = get(car, dt)
    # Pipeline takes dt at construction and reads self.dt inside frame(); the
    # game's dt is frame-locked (period/divisor) and moves, so track it here.
    p.dt = dt
    p.frame(throttle=thr, brake=brk, steer=steer, boost=boost)
    c = p.capture()
    v = []
    v += c["pos"]
    v += c["vel"]
    v.append(c["speed"])
    v += c["dir"]
    v += [p.rf(ep.CTX0 + 0x00 + 4 * i) for i in range(3)]     # right (row 0)
    v += c["up"]
    v += c["at"]
    v += c["omega"]
    v += [c["rpm"], float(c["gear"]), c["torque"], c["steer_deg"],
          float(c["drift"]), c["slide"], float(c["airborne"])]
    v += [w["cur"] for w in c["wheels"]]
    out("st " + " ".join("%.9g" % f for f in v))


PROF = {}
PROF_T = [0.0, 0.0]


def main():
    trace = os.environ.get("B3_EMU_TRACE")
    for line in sys.stdin:
        if trace:
            sys.stderr.write("> " + line)
            sys.stderr.flush()
        parts = line.split()
        if not parts:
            continue
        cmd, a = parts[0], parts[1:]
        _prof = os.environ.get("B3_EMU_PROF")
        _t0 = time.time() if _prof else 0.0
        try:
            if cmd == "hello":
                out("ok %d" % VERSION)
            elif cmd == "airanges":
                do_airanges(a)
            elif cmd == "ai":
                do_ai(a)
            elif cmd == "cam":
                do_cam(a)
            elif cmd == "scranges":
                do_scranges(a)
            elif cmd == "score":
                do_score(a)
            elif cmd == "tdranges":
                do_tdranges(a)
            elif cmd == "tdslam":
                do_tdslam(a)
            elif cmd == "ccol":
                do_ccol(a)
            elif cmd == "crashranges":
                do_crashranges(a)
            elif cmd == "crashres":
                do_crashres(a)
            elif cmd == "tmodel":
                do_tmodel(a)
            elif cmd == "tpaint":
                do_tpaint(a)
            elif cmd == "hudtick":
                do_hudtick(a)
            elif cmd == "sfxfire":
                do_sfxfire(a)
            elif cmd == "ranges":
                do_ranges(a)
            elif cmd == "stepr":
                do_stepr(a)
            elif cmd == "stepra":
                do_stepra(a)
            elif cmd == "hranges":
                do_hranges(a)
            elif cmd == "hstate":
                do_hstate(a)
            elif cmd == "stepraw":
                do_stepraw(a)
            elif cmd == "soup":
                do_soup(a)
            elif cmd == "seed":
                do_seed(a)
            elif cmd == "step":
                do_step(a)
            elif cmd == "bye":
                out("ok")
                return
            else:
                out("err unknown command %s" % cmd)
        except Exception as exc:                        # noqa: BLE001
            out("err %s: %s" % (type(exc).__name__, str(exc).replace("\n", " ")))
        if _prof:
            dt_ms = (time.time() - _t0) * 1000.0
            e = PROF.setdefault(cmd, [0, 0.0, 0.0])
            e[0] += 1; e[1] += dt_ms
            if dt_ms > e[2]: e[2] = dt_ms
            PROF_T[0] += dt_ms
            if PROF_T[0] - PROF_T[1] > 5000.0:
                PROF_T[1] = PROF_T[0]
                rows = sorted(PROF.items(), key=lambda kv: -kv[1][1])
                sys.stderr.write("[prof] total %.0f ms over %d calls\n"
                                 % (PROF_T[0], sum(v[0] for v in PROF.values())))
                for k, v in rows[:9]:
                    sys.stderr.write("[prof]   %-11s n=%-6d tot=%8.1f ms "
                                     "mean=%6.3f max=%8.3f\n"
                                     % (k, v[0], v[1], v[1]/v[0], v[2]))
                sys.stderr.flush()


if __name__ == "__main__":
    main()
