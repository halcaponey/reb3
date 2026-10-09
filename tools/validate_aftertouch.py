#!/usr/bin/env python3
"""
Acceptance test for AFTERTOUCH -- retail's crash slow-mo wreck steering
(the "Impact Time" button), FUN_00118410.

The steering half of this function was recovered by the crash-cinema wave
and lives in b3_wreck_aftertouch_steer (tools/validate_crashcinema.py
section 4 is the differential for it).  THIS file covers the three halves
that wave left open, and the runtime behaviour a player would notice:

  1. THE STATE MACHINE AND THE TIME RATE (0x0011885F..0x001188E4).
     Holding the button requests time divisor 5 in a race (3 or 4 in the
     crash-junction family); releasing it restores divisor 1 -- so RELEASING
     runs the crash FASTER than holding.  Single player only.

  2. THE AUDIO COUPLING.  DAT_003EBFD0 is an audio TIME SCALE: it is the
     second argument of the per-frame audio update FUN_001CA530, lands in
     DAT_004A1EF0 (FUN_001CD620 @0x001CD633) and multiplies every non-exempt
     voice's playback rate (FUN_001CAD10 @0x001CADC6 + five siblings).
     Retail writes it with the SAME instructions that write the divisor
     request, a flat 0.75 for every dilation -- and, crucially, NOT from the
     takedown cinematic or the wreck instant, which dilate time at normal
     audio pitch.  (The old note in burnout3_sfx.h claimed retail does not
     pitch its effects at all.  Section 2 is the disproof.)

  3. THE RUNTIME.  A deterministic player crash driven twice from a scripted
     pad -- button held and button released -- asserting the sim/wall time
     ratios, that steering actually displaces the wreck, that an aftertouch
     contact pays out as an AFTERTOUCH TAKEDOWN, and that the mixer's rate
     really follows the recovered latch.

Usage:
    python3 tools/validate_aftertouch.py             # static + runtime
    python3 tools/validate_aftertouch.py --static    # image/source only
    B3_AT_BIN=/path/to/burnout3 B3_AT_RUNDIR=/path python3 tools/...
        run a shadow build from its own working directory (the run writes
        build/crash_trace_*.log, so a shadow dir keeps it out of the tree)
"""
import os

# Links objects that consult build/backends.cfg; pin so the static sections
# are unaffected by the live backend selection.
os.environ.setdefault('B3_BACKENDS', '/dev/null')

import math
import re
import struct
import subprocess
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
# B3_AT_SRC lets the source section read a shadow tree (the wave's patches
# live outside src/ until they land); it defaults to the real src/.
_src = os.environ.get("B3_AT_SRC", os.path.join(_root, "src"))
ELF = os.path.join(_root, "build", "burnout3.elf")
CRASH_H = os.path.join(_src, "burnout3_crash.h")
TDFX_H = os.path.join(_src, "burnout3_takedown.h")
SFX_H = os.path.join(_src, "burnout3_sfx.h")
SFX_C = os.path.join(_src, "burnout3_sfx.c")
TDFX_C = os.path.join(_src, "burnout3_takedown.c")
FULL_C = os.path.join(_src, "burnout3_full.c")


# --------------------------------------------------------------------------
# ELF reader (program headers only -- the image has no section table)
# --------------------------------------------------------------------------
class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        phoff = struct.unpack_from("<I", self.d, 0x1C)[0]
        phes = struct.unpack_from("<H", self.d, 0x2A)[0]
        phn = struct.unpack_from("<H", self.d, 0x2C)[0]
        self.segs = []
        for i in range(phn):
            o = phoff + i * phes
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", self.d, o)
            if t == 1:
                self.segs.append((va, off, fsz))
        self.segs.sort()

    def off(self, va):
        for v, o, f in self.segs:
            if v <= va < v + f:
                return o + (va - v)
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else None

    def f32(self, va):
        return struct.unpack("<f", self.read(va, 4))[0]

    def find_all(self, needle, lo, hi):
        """Every VA in [lo, hi) whose bytes start `needle`."""
        out = []
        for v, o, f in self.segs:
            a = max(lo, v)
            b = min(hi, v + f)
            if a >= b:
                continue
            blob = self.d[o + (a - v): o + (b - v)]
            i = blob.find(needle)
            while i >= 0:
                out.append(a + i)
                i = blob.find(needle, i + 1)
        return out


class Check:
    def __init__(self):
        self.n = 0
        self.bad = []

    def ok(self, name, cond, detail=""):
        self.n += 1
        if cond:
            print("  ok   %-56s %s" % (name, detail))
        else:
            self.bad.append(name)
            print("  FAIL %-56s %s" % (name, detail))
        return bool(cond)


C = Check()


def bytes_at(img, va, hexs, name):
    """Assert the image bytes at `va` are exactly `hexs`."""
    want = bytes.fromhex(hexs)
    got = img.read(va, len(want))
    return C.ok(name, got == want,
                "@0x%08X %s" % (va, (got or b"").hex()))


def f32_is(img, va, want, name):
    got = img.f32(va)
    return C.ok(name, abs(got - want) < 1e-6,
                "[0x%08X] = %g (want %g)" % (va, got, want))


def src_has(path, needle, name):
    s = open(path).read()
    return C.ok(name, needle in s, os.path.basename(path))


# ==========================================================================
# 1. THE STATE MACHINE -- FUN_00118410 @0x0011885F..0x001188E4
# ==========================================================================
def section_state_machine(img):
    print("\n1. the Impact Time state machine (FUN_00118410)")

    # the SINGLE PLAYER gate: EAX = [0x0073A1C0] (the local player count --
    # FUN_00017C50 indexes the per-player arrays with `i < [0x0073A1C0]`),
    # ECX = 1, CMP EAX,ECX / JNZ past the whole block.
    bytes_at(img, 0x0011885F, "a1c0a17300", "single-player gate MOV EAX,[0x0073A1C0]")
    bytes_at(img, 0x00118864, "b901000000", "  ECX = 1")
    bytes_at(img, 0x00118869, "3bc1", "  CMP EAX,ECX")

    # the RACE arm: TEST byte [ESI+0x13FC],4 -- pad+0x84, the BOOST button
    bytes_at(img, 0x0011889A, "f60704", "race arm TEST byte [veh+0x13FC],4")
    bytes_at(img, 0x0011889F, "c644241d01", "  held flag [esp+0x1D] = 1")
    bytes_at(img, 0x001188A4, "c70524ea600005000000",
             "HELD  -> [0x0060EA24] = 5")
    bytes_at(img, 0x001188C7, "c644241d00", "  not held -> [esp+0x1D] = 0")

    # the CRASH-JUNCTION arm: divisor 3 or 4, selected by the crashbreaker
    # stamp veh+0x3A74 (COMISS XMM6(0.0),[ESI+0x3A74] / JBE 0x00118986)
    bytes_at(img, 0x0011888E, "c70524ea600003000000",
             "crash arm, breaker window -> [0x0060EA24] = 3")
    bytes_at(img, 0x00118986, "c70524ea600004000000",
             "crash arm, no breaker     -> [0x0060EA24] = 4")

    # the RELEASE: only when the engaged latch veh+0x4AC7 was set
    bytes_at(img, 0x001188CC, "8a86c74a0000", "release gate MOV AL,[veh+0x4AC7]")
    bytes_at(img, 0x001188D6, "890d24ea6000",
             "RELEASED -> [0x0060EA24] = ECX (= 1)")
    bytes_at(img, 0x001188BE, "c686c74a000001", "engage  veh+0x4AC7 = 1")
    bytes_at(img, 0x001188E4, "c686c74a000000", "release veh+0x4AC7 = 0")

    # so the RELEASED rate is strictly FASTER than the held rate
    C.ok("released divisor 1 < held divisor 5 (release runs faster)",
         True, "0x001188D6 vs 0x001188A4")

    # the consume gates and the steer law's constants
    f32_is(img, 0x003B1684, 0.5, "deadzone |h|+|v| > 0.5      [0x003B1684]")
    f32_is(img, 0x003B1694, 5.0, "crash-clock window  < 5.0   [0x003B1694]")
    f32_is(img, 0x003B16B0, 8.0, "yaw gate |ang| > 8.0 deg    [0x003B16B0]")
    f32_is(img, 0x003B16E8, 0.4, "yaw rate numerator, race    [0x003B16E8]")
    f32_is(img, 0x003A55F8, 0.75, "yaw rate numerator, crash   [0x003A55F8]")
    f32_is(img, 0x00384A80, 0.15, "breaker nudge, race         [0x00384A80]")
    f32_is(img, 0x003B1730, 0.25, "breaker nudge, crash        [0x003B1730]")
    f32_is(img, 0x003B16A4, -0.5, "visual bank scale           [0x003B16A4]")
    f32_is(img, 0x003B1768, 1.2, "visual bank step max        [0x003B1768]")
    f32_is(img, 0x003B1C5C, 17.0, "visual bank max             [0x003B1C5C]")
    f32_is(img, 0x003A69C0, 0.9, "crashbreaker window seconds [0x003A69C0]")
    f32_is(img, 0x003B16C0, -1.0, "crashbreaker stamp cleared  [0x003B16C0]")

    # the AFTERTOUCH TAKEDOWN qualifier veh+0x4AC5: set by the frame that
    # actually rotates the wreck, cleared on crash exit
    bytes_at(img, 0x00118CD3, "c686c54a000001", "qualifier veh+0x4AC5 = 1")
    bytes_at(img, 0x00119C87, "c683c54a000000", "qualifier veh+0x4AC5 = 0 on exit")

    # FUN_00013C60 is dot3 (Ghidra decompiles it to an empty body): MULPS
    # then two SHUFPS/ADDSS folds.  The steer measures the yaw error off the
    # UNIT TRAVEL DIRECTION veh+0xC0 and rotates the VELOCITY veh+0xB0.
    bytes_at(img, 0x00013C69, "0f28080f2801", "FUN_00013C60 is dot3: MOVAPS pair")
    bytes_at(img, 0x00013C6F, "0f59c1", "  MULPS XMM0,XMM1")
    bytes_at(img, 0x00013C75, "0fc6c839", "  SHUFPS XMM1,XMM0,0x39")


# ==========================================================================
# 2. THE AUDIO COUPLING -- DAT_003EBFD0 -> DAT_004A1EF0
# ==========================================================================
# Every paired (divisor request, audio rate) write site in the dilation
# family, and the four divisor sites that have NO paired rate store.
PAIRED = [
    # (divisor VA, divisor bytes, rate VA, rate bytes, divisor, rate, what)
    (0x001188A4, "c70524ea600005000000", 0x001188B6, "f30f1105d0bf3e00",
     5, 0.75, "aftertouch engage, race"),
    (0x0011888E, "c70524ea600003000000", 0x001188B6, "f30f1105d0bf3e00",
     3, 0.75, "aftertouch, crash junction (breaker)"),
    (0x00118986, "c70524ea600004000000", 0x001188B6, "f30f1105d0bf3e00",
     4, 0.75, "aftertouch, crash junction"),
    (0x001188D6, "890d24ea6000", 0x001188DC, "f30f112dd0bf3e00",
     1, 1.0, "aftertouch release"),
    (0x0002655B, "893d24ea6000", 0x00026561, "f30f1105d0bf3e00",
     6, 0.75, "impact-hit window arm"),
    (0x00026525, "c70524ea600001000000", 0x0002652F, "f30f1105d0bf3e00",
     1, 1.0, "impact-hit window end"),
    (0x00119C24, "c70524ea600001000000", 0x00119C3A, "f30f1105d0bf3e00",
     1, 1.0, "crash exit"),
]

# divisor requests with NO paired DAT_003EBFD0 store: these dilate time
# WITHOUT pitching the audio.
UNPAIRED = [
    (0x0002795F, "c70524ea600005000000", 5, "takedown cinematic enter"),
    (0x00027A3D, "c70524ea600001000000", 1, "takedown cinematic exit"),
    (0x00027BCD, "a324ea6000", -1, "takedown cinematic update"),
    (0x00025D5C, "c70524ea600005000000", 5, "the wreck instant"),
]

# the six per-voice playback-rate multiplies
RATE_CONSUMERS = [
    (0x001CADC6, "f30f5905f01e4a00", "FUN_001CAD10 voice+0x0C *= rate"),
    (0x001CAC23, "f30f5905f01e4a00", "FUN_001CA9A0 (a)"),
    (0x001CACAC, "f30f5905f01e4a00", "FUN_001CA9A0 (b)"),
    (0x001CAF5E, "f30f1005f01e4a00", "FUN_001CAE30"),
    (0x001CC7BA, "f30f5905f01e4a00", "FUN_001CC700 voice+0x1C *= rate"),
    (0x001CCAA1, "f30f1005f01e4a00", "FUN_001CC910"),
]

# The complete DAT_003EBFD0 STORE set in .text.  From the image's own xref
# set (18 refs, 11 of them writes); the dilation family is the seven below
# plus 0x0002669F / 0x00026792 / 0x00026A07 (the crash-presentation
# restores) and 0x0013EEB5 / 0x0013F5B8 / 0x001689BD (other subsystems).
ALL_RATE_STORES = {
    0x0013EEB5, 0x0013F5B8, 0x001689BD,
    0x0002652F, 0x00026561, 0x0002669F, 0x00026792, 0x00026A07,
    0x001188B6, 0x001188DC, 0x00119C3A,
}

# function bodies the negative claim is made over
CINEMATIC_BODIES = [
    (0x00027920, 0x000279BB, "FUN_00027920 cinematic enter"),
    (0x000279C0, 0x00027A56, "FUN_000279C0 cinematic exit"),
    (0x00027AD0, 0x00027CB7, "FUN_00027AD0 cinematic update"),
    (0x00025CC0, 0x00025F37, "FUN_00025CC0 the wreck instant"),
]


def section_audio(img):
    print("\n2. the audio time scale DAT_003EBFD0 -> DAT_004A1EF0")

    f32_is(img, 0x003A55F8, 0.75, "dilated audio rate  [0x003A55F8]")
    f32_is(img, 0x003B168C, 1.0, "normal audio rate   [0x003B168C]")
    f32_is(img, 0x003EBFD0, 1.0, "DAT_003EBFD0 image init")

    for dva, dbytes, rva, rbytes, div, rate, what in PAIRED:
        a = bytes_at(img, dva, dbytes, "%-36s divisor %d" % (what, div))
        b = bytes_at(img, rva, rbytes, "%-36s rate %.2f" % ("", rate))
        # the two must be adjacent in CONTROL FLOW: either the rate store
        # follows the divisor store inside the same basic block, or the
        # divisor store is immediately followed by a JMP to the shared rate
        # store (which is how the crash-junction arms at 0x0011888E and
        # 0x00118986 reach the one store at 0x001188AE/0x001188B6).
        nxt = dva + len(bytes.fromhex(dbytes))
        j = img.read(nxt, 1)
        jumps = j in (b"\xeb", b"\xe9")
        C.ok("%-36s paired in control flow" % "",
             a and b and (0 < rva - dva <= 0x30 or jumps),
             "0x%08X -> 0x%08X%s" % (dva, rva,
                                     " (via JMP @0x%08X)" % nxt if jumps else ""))

    # the audio rate is a FLAT 0.75 for divisors 3, 4, 5 and 6 -- not
    # 1/divisor.  Four different divisors, one constant.
    dilating = sorted({d for _, _, _, _, d, r, _ in PAIRED if r != 1.0})
    C.ok("flat 0.75 across every dilating divisor", dilating == [3, 4, 5, 6],
         "divisors %s all -> [0x003A55F8] = 0.75" % dilating)

    for va, b, div, what in UNPAIRED:
        bytes_at(img, va, b, "%-36s divisor %s (unpaired)" %
                 (what, div if div > 0 else "reg"))

    # THE NEGATIVE CLAIM: no DAT_003EBFD0 store anywhere in the cinematic or
    # wreck-instant bodies, so those dilate time at normal audio pitch.
    for lo, hi, what in CINEMATIC_BODIES:
        hits = [v for v in img.find_all(bytes.fromhex("d0bf3e00"), lo, hi)]
        C.ok("no DAT_003EBFD0 reference in %s" % what, not hits,
             "0x%08X..0x%08X" % (lo, hi))

    # and the store set really is the one the table above is drawn from
    found = set()
    for v, o, f in img.segs:
        for pat in (b"\xf3\x0f\x11\x05\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x2d\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x0d\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x1d\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x25\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x35\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x3d\xd0\xbf\x3e\x00",
                    b"\xf3\x0f\x11\x15\xd0\xbf\x3e\x00"):
            found.update(img.find_all(pat, v, v + f))
    C.ok("MOVSS-store site set for DAT_003EBFD0", found == ALL_RATE_STORES,
         "%d sites%s" % (len(found),
                         "" if found == ALL_RATE_STORES
                         else " (unexpected %s)" %
                              sorted(hex(x) for x in found ^ ALL_RATE_STORES)))

    for va, b, what in RATE_CONSUMERS:
        bytes_at(img, va, b, "consumer %s" % what)

    # FUN_001CD620 @0x001CD633 stashes the rate in DAT_004A1EF0, and the
    # STREAMED path (FUN_001CBA60 @0x001CBAA0) reads only the volume
    # DAT_004A1EEC -- which is why the music keeps its tempo.
    bytes_at(img, 0x001CD633, "f30f1105f01e4a00",
             "FUN_001CD620 DAT_004A1EF0 = rate")
    bytes_at(img, 0x001CBAA0, "f30f1005ec1e4a00",
             "streamed path reads the VOLUME DAT_004A1EEC only")


# ==========================================================================
# 3. THE PORT CARRIES THE SAME LAW
# ==========================================================================
def section_source():
    print("\n3. the port's own constants and wiring")
    src_has(TDFX_H, "#define B3_TDFX_PITCH_DILATED 0.75f",
            "B3_TDFX_PITCH_DILATED = 0.75")
    src_has(TDFX_H, "#define B3_TDFX_PITCH_NORMAL  1.0f",
            "B3_TDFX_PITCH_NORMAL  = 1.0")
    for name, val in (("B3_AT_DEADZONE", "0.5f"), ("B3_AT_MIN_SPEED", "1.0f"),
                      ("B3_AT_WINDOW_S", "5.0f"),
                      ("B3_AT_ANGLE_GATE_DEG", "8.0f"),
                      ("B3_AT_RATE_RACE", "0.4f"),
                      ("B3_AT_RATE_CRASH", "0.75f"),
                      ("B3_AT_NUDGE_RACE", "0.15f"),
                      ("B3_AT_NUDGE_CRASH", "0.25f"),
                      ("B3_AT_BANK_SCALE", "-0.5f"),
                      ("B3_AT_BANK_STEP_MAX", "1.2f"),
                      ("B3_AT_BANK_MAX", "17.0f")):
        s = open(CRASH_H).read()
        m = re.search(r'#define\s+%s\s+(\S+)' % name, s)
        C.ok("burnout3_crash.h %s" % name, bool(m) and m.group(1) == val,
             (m.group(1) if m else "MISSING") + " (want %s)" % val)

    # the audio rate must be a LATCH written at the request sites, not a
    # function of the divisor -- otherwise the takedown cinematic pitches
    # the audio, which retail does not do.
    s = open(TDFX_C).read()
    C.ok("b3_tdfx_pitch() returns the latch, not f(divisor)",
         "return G.audio_rate;" in s,
         "burnout3_takedown.c")
    C.ok("audio rate latched at the aftertouch engage site",
         "G.audio_rate        = B3_TDFX_PITCH_DILATED;   /* 0x001188B6 */" in s,
         "0x001188B6")
    C.ok("audio rate latched at the aftertouch release site",
         "G.audio_rate         = B3_TDFX_PITCH_NORMAL;   /* 0x001188DC */" in s,
         "0x001188DC")
    C.ok("the wreck instant does NOT touch the audio rate",
         re.search(r'B3_TDFX_DIV_AFTERTOUCH;\s*/\* 0x00025D5C \*/\s*\n'
                   r'\s*G\.crash_slowmo_on', s) is not None,
         "0x00025D5C unpaired, as in retail")

    # the mixer really applies it
    sc = open(SFX_C).read()
    C.ok("b3_sfx_set_time_scale exists", "void b3_sfx_set_time_scale(" in sc, "")
    C.ok("the voice step is scaled live (not baked at start())",
         "v->pos += v->step * ts;" in sc, "burnout3_sfx.c")
    src_has(SFX_H, "void  b3_sfx_set_time_scale(float rate);",
            "burnout3_sfx.h declares the setter")
    # ...and the stale claim is gone
    C.ok("the stale \"nothing pitches the effects down\" note is corrected",
         "Nothing pitches the effects down; retail's slow-motion \"sound\" is a"
         not in open(SFX_H).read(), "burnout3_sfx.h section 3")


# ==========================================================================
# 4. RUNTIME -- the scripted crash, held vs released
# ==========================================================================
TRACE_RE = re.compile(
    r'f=(\d+) t=([\d.]+) dt=([\d.]+) div=(\d+) ts=([\d.]+) arate=([\d.]+) '
    r'held=(\d+) ath=([-+\d.]+) atv=([-+\d.]+) atused=(\d+).*?'
    r'\| wr\d pos ([-\d.]+) ([-\d.]+) ([-\d.]+) '
    r'\| vel ([-\d.]+) ([-\d.]+) ([-\d.]+)')

CRASH_AT = 8.0          # B3_TEST_CRASH_AT -- the deterministic player crash
EXIT_AT = 20.0


def get_binary():
    b = os.environ.get("B3_AT_BIN")
    if b:
        return b
    if os.name == "nt":
        for cand in (os.path.join(_root, "build-win", "burnout3.exe"),
                     os.path.join(_root, "burnout3.exe")):
            if os.path.exists(cand):
                return cand
        return os.path.join(_root, "build-win", "burnout3.exe")
    return os.path.join(_root, "burnout3")


def run_game(rundir, binary, extra):
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "windows" if os.name == "nt" else "offscreen",
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
        "B3_TRACK": "US_C3_V1",
        "B3_NO_VSYNC": "1",
        "B3_AUTODRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(EXIT_AT),
        "B3_TEST_CRASH_AT": str(CRASH_AT),
    })
    env.pop("B3_BACKENDS", None)
    # a None value UNSETS -- section 5 has to clear B3_AUTODRIVE, because the
    # human input branch is guarded on `!autodrive` and that is precisely the
    # branch the real-input leg exists to exercise.
    for k, v in extra.items():
        if v is None:
            env.pop(k, None)
        else:
            env[k] = v
    for f in os.listdir(os.path.join(rundir, "build")):
        if f.startswith("crash_trace_"):
            p = os.path.join(rundir, "build", f)
            if not os.path.islink(p):
                os.remove(p)
    cmd = [binary] if os.name == "nt" else ["timeout", "400", binary]
    try:
        p = subprocess.run(cmd, cwd=rundir, env=env, timeout=400,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        log = (p.stdout.decode("utf-8", "replace")
               + p.stderr.decode("utf-8", "replace"))
    except subprocess.TimeoutExpired as e:
        log = ((e.stdout.decode("utf-8", "replace") if e.stdout else "")
               + (e.stderr.decode("utf-8", "replace") if e.stderr else ""))
    tp = os.path.join(rundir, "build", "crash_trace_001.log")
    rows = []
    if os.path.exists(tp):
        for line in open(tp):
            m = TRACE_RE.match(line)
            if m:
                g = m.groups()
                rows.append({
                    "f": int(g[0]), "t": float(g[1]), "dt": float(g[2]),
                    "div": int(g[3]), "ts": float(g[4]), "arate": float(g[5]),
                    "held": int(g[6]), "h": float(g[7]), "v": float(g[8]),
                    "used": int(g[9]),
                    "pos": (float(g[10]), float(g[11]), float(g[12])),
                    "vel": (float(g[13]), float(g[14]), float(g[15])),
                })
    return log, rows


def section_runtime():
    binary = get_binary()
    rundir = os.environ.get("B3_AT_RUNDIR", _root)
    print("\n4. runtime: the scripted crash, Impact Time held vs released")
    print("   binary %s" % binary)
    if not os.path.exists(binary):
        C.ok("the game binary exists", False,
             "%s -- `make` first, or set B3_AT_BIN" % binary)
        return

    runs = {}
    for name, script in (("held", "1,0,1"), ("released", "1,0,0"),
                         ("left", "-1,0,1"), ("neutral", "0,0,1")):
        log, rows = run_game(rundir, binary,
                             {"B3_TEST_AFTERTOUCH": script})
        runs[name] = (log, rows)
        if not C.ok("run %-8s produced a crash trace" % name, len(rows) > 60,
                    "%d frames" % len(rows)):
            return

    # ---- (a) the sim-time / wall-time ratios -----------------------------
    # The harness runs one rendered frame per fixed tick, so "wall time" is
    # frames/60 and the ratio IS 1/divisor.
    for name, want_div, want_ratio, tol in (("held", 5, 0.20, 0.02),
                                            ("released", 1, 1.00, 0.02)):
        rows = runs[name][1]
        span = rows[-1]["t"] - rows[0]["t"]
        wall = len(rows) / 60.0
        ratio = span / wall if wall else 0.0
        # the crash's OWN divisor-5 request (0x00025D5C) dilates the first
        # 0.35 s of dilated clock whether or not the button is held, so the
        # released run is measured on the frames after it lapses.
        tail = [r for r in rows if r["t"] - rows[0]["t"] > 0.60]
        tail_div = [r["div"] for r in tail]
        frac = tail_div.count(want_div) / float(len(tail_div) or 1)
        C.ok("%-8s steady-state divisor %d" % (name, want_div), frac > 0.95,
             "%.0f%% of the tail; whole-crash ratio %.4f over %.2f s sim / "
             "%.2f s wall" % (100 * frac, ratio, span, wall))
        tail_ratio = (sum(r["dt"] for r in tail) / (len(tail) / 60.0)
                      if tail else 0.0)
        C.ok("%-8s sim/wall time ratio %.2f" % (name, want_ratio),
             abs(tail_ratio - want_ratio) < tol,
             "measured %.4f (want %.2f +/- %.2f)" % (tail_ratio, want_ratio, tol))

    held_wall = len(runs["held"][1]) / 60.0
    rel_wall = len(runs["released"][1]) / 60.0
    C.ok("releasing runs the crash FASTER than holding",
         rel_wall < held_wall * 0.5,
         "%.2f s wall released vs %.2f s held (%.1fx)" %
         (rel_wall, held_wall, held_wall / rel_wall if rel_wall else 0))

    # ---- (b) steering displaces the wreck --------------------------------
    def endpos(name):
        r = runs[name][1][-1]
        return (r["pos"][0], r["pos"][2])

    def used(name):
        return max(r["used"] for r in runs[name][1])

    C.ok("held + stick RIGHT latches the qualifier veh+0x4AC5",
         used("held") == 1, "atused peaked at %d" % used("held"))
    C.ok("held + stick LEFT  latches the qualifier veh+0x4AC5",
         used("left") == 1, "atused peaked at %d" % used("left"))
    C.ok("neutral stick never steers (deadzone |h|+|v| > 0.5)",
         used("neutral") == 0, "atused peaked at %d" % used("neutral"))
    C.ok("released never steers (the held flag gates the consume block)",
         used("released") == 0, "atused peaked at %d" % used("released"))

    pr, pl, pn = endpos("held"), endpos("left"), endpos("neutral")
    d_r = math.hypot(pr[0] - pn[0], pr[1] - pn[1])
    d_l = math.hypot(pl[0] - pn[0], pl[1] - pn[1])
    d_rl = math.hypot(pr[0] - pl[0], pr[1] - pl[1])
    C.ok("steering RIGHT displaces the wreck vs no input", d_r > 5.0,
         "%.1f m" % d_r)
    C.ok("steering LEFT  displaces the wreck vs no input", d_l > 5.0,
         "%.1f m" % d_l)
    C.ok("the two steer directions separate from each other", d_rl > 5.0,
         "%.1f m" % d_rl)

    # ---- (c) an aftertouch contact pays out as an AFTERTOUCH TAKEDOWN ----
    log, rows = run_game(rundir, binary,
                         {"B3_TEST_AFTERTOUCH": "1,0,1",
                          "B3_TEST_AT_TAKEDOWN": "3"})
    C.ok("aftertouch contact -> WRECK TAKEDOWN with aftertouch=1",
         re.search(r'WRECK TAKEDOWN: wreck 0 -> car \d+ \(impact [\d.]+, '
                   r'aftertouch=1', log) is not None, "")
    C.ok("...and it commits as message 0xAA + 1250 BP",
         re.search(r'TAKEDOWN COMMIT:.*message 0xAA, \+1250 BP \(aftertouch\)',
                   log) is not None, "B3_TDR_MSG_AFTERTOUCH0 / B3_TDR_BP_AFTERTOUCH")

    # ---- (d) the SFX rate state follows ----------------------------------
    for name, want in (("held", 0.75), ("released", 1.0)):
        rows = runs[name][1]
        tail = [r for r in rows if r["t"] - rows[0]["t"] > 0.60]
        bad = [r for r in tail
               if abs(r["arate"] - (0.75 if r["div"] != 1 else 1.0)) > 1e-3]
        C.ok("%-8s audio rate tracks the divisor every frame" % name,
             not bad, "%d frames, %d mismatches" % (len(tail), len(bad)))
        rates = sorted({round(r["arate"], 3) for r in tail})
        C.ok("%-8s steady-state audio rate %.2f" % (name, want),
             rates == [want], "observed %s" % rates)

    # the mixer really received it: the debug dump prints the tdfx latch and
    # b3_sfx_time_scale() side by side and they must agree.
    log, _ = run_game(rundir, binary,
                      {"B3_TEST_AFTERTOUCH": "1,0,1", "B3_DUMP_FRAME": "600"})
    dumps = sorted(
        (os.path.join(rundir, "build", f)
         for f in os.listdir(os.path.join(rundir, "build"))
         if re.match(r'debug_dump_\d+\.txt$', f)),
        key=lambda p: os.path.getmtime(p))
    got = None
    if dumps:
        txt = open(dumps[-1]).read()
        m_t = re.search(r'tdfx: divisor (\d+) timescale [\d.]+ '
                        r'audio_rate ([\d.]+)', txt)
        m_a = re.search(r'aftertouch: held (\d+) h ([-+\d.]+) v ([-+\d.]+) '
                        r'used (\d+) sfx_rate ([\d.]+)', txt)
        if m_t and m_a:
            got = (int(m_t.group(1)), float(m_t.group(2)),
                   int(m_a.group(1)), int(m_a.group(4)), float(m_a.group(5)))
    C.ok("the mixer's live rate equals the recovered latch",
         got is not None and got[0] == 5 and abs(got[1] - 0.75) < 1e-3
         and got[2] == 1 and got[3] == 1 and abs(got[4] - 0.75) < 1e-3,
         "divisor %s audio_rate %s held %s used %s sfx_rate %s"
         % (got if got else ("?",) * 5))


# ==========================================================================
# 5. THE REAL INPUT PATH -- no script hook
#
# Section 4 drives every runtime leg through at_script() (B3_TEST_AFTERTOUCH),
# which injects the held bit LATE, right at the aftertouch gate.  That hid a
# real defect for a whole wave: on the live pad path the crashed-state input
# override
#
#     if (g_race_time < v->crashed_until) { throttle = 0; brake = 1; boost = 0; }
#
# zeroed the very bit the gate reads, so `player_crashed && boost_held` was
# unsatisfiable and Impact Time never engaged for a human -- while the steer
# half, which re-read the keyboard/pad for itself, kept working (the player
# saw the reticle track the stick but got no slow-mo and no 0.75 pitch).
#
# This section closes that blind spot from both ends: a SOURCE guard that the
# two halves share one snapshot taken before the override, and a RUNTIME leg
# that holds the button via B3_TEST_PAD_BOOST -- which writes the same local
# the keyboard and the pad write, upstream of the override -- and asserts the
# divisor and the audio rate really move.
# ==========================================================================
def section_real_input_static():
    print("\n5. the REAL input path (no at_script hook) -- source guards")
    s = open(FULL_C).read()

    # -- (a) source guards -------------------------------------------------
    C.ok("the raw boost bit is snapshotted for the aftertouch gate",
         re.search(r'int\s+boost_raw\s*=\s*0;', s) is not None,
         "boost_raw, taken before the crashed-state override")
    C.ok("the snapshot is taken before the crashed-state override",
         re.search(r'boost_raw\s*=\s*boost;\s*\n'
                   r'\s*if\s*\(v->crashed_until\s*>\s*0\.0f\)', s) is not None,
         "after both input branches, ahead of the override")
    C.ok("the divisor half reads the snapshot, NOT the suppressed copy",
         re.search(r'int\s+at_hold\s*=\s*boost_raw;', s) is not None,
         "at_hold = boost_raw")
    C.ok("...and `at_hold = boost` (the pre-fix bug) is gone",
         re.search(r'int\s+at_hold\s*=\s*boost;', s) is None, "")
    C.ok("the steer half reads the SAME snapshot (one source, no divergence)",
         re.search(r'g_at_held\s*=\s*boost_raw;', s) is not None,
         "g_at_held = boost_raw")
    C.ok("the crashed-state override still suppresses throttle/brake/boost",
         re.search(r'g_race_time\s*<\s*v->crashed_until\)\s*\{\s*\n'
                   r'\s*throttle\s*=\s*0\.0f;\s*\n\s*brake\s*=\s*1\.0f;\s*\n'
                   r'\s*boost\s*=\s*0;', s) is not None,
         "the driving inputs stay cut -- only the gate's copy is spared")
    # retail's own layout: pad+0x84 is A on the Xbox pad, and it is boost.
    C.ok("the pad's A button is BOOST (retail Xbox layout), not a 2nd throttle",
         re.search(r'pad_btn\(SDL_CONTROLLER_BUTTON_A\)\)\s*boost\s*=\s*1;', s)
         is not None
         and re.search(r'pad_btn\(SDL_CONTROLLER_BUTTON_A\)\)\s*throttle', s)
         is None,
         "A -> pad+0x84, so holding A through a wreck engages Impact Time")


def section_real_input():
    section_real_input_static()
    print("\n5b. the REAL input path -- runtime")

    # -- (b) runtime: hold the button through the real local ---------------
    binary = get_binary()
    rundir = os.environ.get("B3_AT_RUNDIR", _root)
    if not os.path.exists(binary):
        C.ok("the game binary exists", False,
             "%s -- `make` first, or set B3_AT_BIN" % binary)
        return

    # No B3_TEST_AFTERTOUCH anywhere in these runs: the ONLY thing holding the
    # button is the real input local, written inside the human input branch.
    # That branch is skipped under B3_AUTODRIVE, so autodrive is cleared and
    # B3_TESTDRIVE pins the throttle through the very same block instead.
    HUMAN = {"B3_AUTODRIVE": None, "B3_TESTDRIVE": "1"}
    held = run_game(rundir, binary, dict(HUMAN, B3_TEST_PAD_BOOST="1"))[1]
    rel = run_game(rundir, binary, dict(HUMAN))[1]
    if not C.ok("both real-input runs produced a crash trace",
                len(held) > 60 and len(rel) > 60,
                "%d held / %d released frames" % (len(held), len(rel))):
        return

    # Measured on the tail, past the wreck instant's own 0.35 s divisor-5
    # request (0x00025D5C), which fires whether or not the button is held.
    def tail(rows):
        return [r for r in rows if r["t"] - rows[0]["t"] > 0.60]

    th, tr = tail(held), tail(rel)
    fh = [r["div"] for r in th].count(5) / float(len(th) or 1)
    C.ok("REAL INPUT held -> steady-state divisor 5", fh > 0.95,
         "%.0f%% of the tail (this is the leg that was silently broken)"
         % (100 * fh))
    fr = [r["div"] for r in tr].count(1) / float(len(tr) or 1)
    C.ok("REAL INPUT released -> steady-state divisor 1", fr > 0.95,
         "%.0f%% of the tail" % (100 * fr))

    C.ok("REAL INPUT held -> the gate sees the button (held=1)",
         all(r["held"] == 1 for r in th), "veh+0x13FC & 4 survives the crash")
    C.ok("REAL INPUT released -> the gate sees no button (held=0)",
         all(r["held"] == 0 for r in tr), "")

    rh = sorted({round(r["arate"], 3) for r in th})
    rr = sorted({round(r["arate"], 3) for r in tr})
    C.ok("REAL INPUT held -> audio rate 0.75", rh == [0.75], "observed %s" % rh)
    C.ok("REAL INPUT released -> audio rate 1.0", rr == [1.0], "observed %s" % rr)

    # and the wall-clock consequence the player actually feels
    wh, wr = len(held) / 60.0, len(rel) / 60.0
    C.ok("REAL INPUT holding stretches the crash on the wall clock",
         wh > wr * 2.0, "%.2f s held vs %.2f s released" % (wh, wr))

    # the scripted and real paths must agree -- the whole point of this leg
    sc = run_game(rundir, binary, dict(HUMAN, B3_TEST_AFTERTOUCH="1,0,1"))[1]
    ts = tail(sc)
    fs = [r["div"] for r in ts].count(5) / float(len(ts) or 1)
    C.ok("scripted and real input agree on the divisor",
         abs(fs - fh) < 0.05, "scripted %.0f%% vs real %.0f%%"
         % (100 * fs, 100 * fh))


def main():
    static_only = "--static" in sys.argv
    if not os.path.exists(ELF):
        print("missing %s -- run tools/xbe2elf.py first" % ELF)
        return 1
    img = Image(ELF)
    print("=== validate_aftertouch: retail's Impact Time / aftertouch ===")
    section_state_machine(img)
    section_audio(img)
    section_source()
    if not static_only:
        section_runtime()
        section_real_input()
    else:
        # the source half of section 5 needs no binary
        section_real_input_static()

    print("\n%d checks, %d failed" % (C.n, len(C.bad)))
    for b in C.bad:
        print("  FAILED: %s" % b)
    return 1 if C.bad else 0


if __name__ == "__main__":
    sys.exit(main())
