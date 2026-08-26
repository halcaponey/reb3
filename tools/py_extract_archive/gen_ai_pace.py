#!/usr/bin/env python3
"""Extract retail's per-opponent AI PACE RECORDS out of every Gamedata.bgd.

SUPERSEDED AS A GENERATOR, KEPT AS THE SPEC
-------------------------------------------
This tool used to bake its output into src/burnout3_ai_pace.h -- 221 events x
6 slots of GAME DATA compiled into the port.  It does not any more: the port
reads build/tracks/<id>/pace.bin, written per track by tools/cextract's `pace`
stage (tools/cextract/cx_pace.c) and consumed by
src/burnout3_ai_pace_runtime.h.  Writing into src/ is REFUSED below so the
header cannot come back by accident.

What stays is this file's decode, which is the acceptance spec: `read_track()`
is what tools/validate_no_baked_data.py section 3 diffs every pace.bin
against, and the docstring below is the provenance for both.

ONE KNOWN DEFECT, deliberately not fixed here: `b40()` omits the final
reversal FUN_001AECC0 performs, so the event ids this file reports are spelled
BACKWARDS ("FCRGSFFO" for OFFSGRCF).  The C extractor uses cxa_b40(), which is
correct; the validator compares against the reversed spelling on purpose.
Leaving it alone keeps this module byte-comparable with the header it used to
emit.

WHAT THIS IS
------------
The rubber band in Burnout 3 is not a constant: how long each rival keeps its
catch-up licence is *event data*, one byte per grid slot, shipped inside every
Tracks/<REG>/<Cn_Vn>/Gamedata.bgd.

The consumer is FUN_00172870 (`burnout3.elf`, .text = flat + 0x10000), the AI
navigator's pace-record reader, called once per car from the AI constructor
FUN_001718A0.  Its record branch is at 0x001728DD..0x00172A20:

    slot = (s8) racecar+0x19BC                       # the GRID SLOT, 0..5
    if (DAT_0073A170 == 0 || slot >= DAT_0073A180)   # no record for this slot
        ... defaults, AI+0x9E8 = 0 ...               # @0x001728F0
    else:
        rec = DAT_0073A170 + slot * 0x98             # @0x00172937 (imul 0x98)
        AI+0x9E4 = rec[0x97] * 0.01
        AI+0x9E8 = rec[0x93] * 0.01     <-- THE CATCH-UP WINDOW
        AI+0x9EC = rec[0x90] * 0.01     <-- aggression, also copied to AI+0x9E0
        AI+0x9F0 = rec[0x92] * 0.01
        AI+0x9F4 = rec[0x91] * 0.01
        AI+0x9F8 = (s8) rec[0x95]
        AI+0xA12 = (rec[0x96] & 1) == 1
        ... plus the two 16-entry per-section factor tables at rec+0x10 and
            rec+0x50, lerped into AI+0x7C0 / AI+0x8C0 ...

and the table base is installed by the .bgd loader FUN_0018B250:

    @0x0018B478   DAT_0073A170 = <the 0x800-byte event PARAM record>
    @0x0018B48A   DAT_0073A180 = param[0x3B4]        # opponent count

So a pace record is `param_record + grid_slot * 0x98`, six of them tiling
0x000..0x390, immediately below the medal thresholds RE_BGD already documents
at +0x3A0/+0x3A4/+0x3A8 and the opponent count at +0x3B4.  That tiling is the
cross-check: 0x10 + 16*4 + 16*4 + 8 == 0x98 exactly, and 6 * 0x98 == 0x390
lands exactly on the first documented field.  [C]

Slot 0's first 0x10 bytes are the event's packed car ID (RE_BGD section 3);
FUN_00172870 never reads rec[0x00..0x0F], so the overlap is harmless -- and
slot 0 is the human player, who has no AI navigator at all.

WHAT THE DATA SAYS
------------------
Across the 36 shipped tracks the catch-up window rises monotonically with the
grid slot in every standard race: the car starting last keeps its licence for
0.78..0.97 of the race, the car starting second for 0.15..0.33.  Road Rage
(FEGRRFFO) pins slot 1 -- and on several tracks every slot -- to 1.00, i.e.
catch-up for the whole event.  None of that is tunable in this port; it is
read out of the shipped files by this script.

Usage:
    python3 tools/gen_ai_pace.py [--root <game dir>] [--out src/burnout3_ai_pace.h]
"""
import argparse
import glob
import os
import struct
import sys
import os as _os, sys as _sys
_sys.path[:0] = [_os.path.dirname(_os.path.abspath(__file__)),
                 _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))]
from b3_paths import game_path, game_root  # noqa: E402

DEFAULT_ROOT = game_root()

# base-40 packed 12-char IDs -- the charset FUN_001AECC0 decodes (RE_BGD).
CS = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_"

REC_STRIDE = 0x98        # @0x00172937  imul $0x98
N_SLOTS = 6              # the shipped race field size (RE_BGD section 3)
PACE_OFF = 0x90          # the byte block FUN_00172870 reads
N_OPP_OFF = 0x3B4        # @0x0018B484  DAT_0073A180
ROOT_IDS = 0x08          # 18 packed event IDs
ROOT_RECS = 0x198        # 18 u32 offsets of the 0x800 param records


def b40(v):
    s = ''
    for _ in range(12):
        s += CS[v % 40]
        v //= 40
    return s.strip()


def read_track(path):
    """-> (track_id, [ {id, n_opp, slots:[bytes8]} ]) or None."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 0x4000 or struct.unpack_from('<I', d, 4)[0] != 0x320178:
        return None
    parts = path.replace('\\', '/').split('/')
    track_id = "%s_%s" % (parts[-3], parts[-2])
    events = []
    for i in range(18):
        idv = struct.unpack_from('<Q', d, ROOT_IDS + i * 8)[0]
        ro = struct.unpack_from('<I', d, ROOT_RECS + i * 4)[0]
        # unused slots keep stale tool memory in both tables (RE_BGD)
        if not (0x3000 <= ro <= 0xB800 and ro % 0x800 == 0):
            continue
        if ro + 0x800 > len(d):
            continue
        n_opp = struct.unpack_from('<I', d, ro + N_OPP_OFF)[0]
        if n_opp == 0 or n_opp > N_SLOTS:
            continue
        slots = [d[ro + k * REC_STRIDE + PACE_OFF:
                   ro + k * REC_STRIDE + PACE_OFF + 8] for k in range(N_SLOTS)]
        events.append({'id': b40(idv), 'n_opp': n_opp, 'slots': slots})
    return track_id, events


HEADER = '''// GENERATED by tools/gen_ai_pace.py -- do not edit by hand.
//
// Retail's per-opponent AI pace records, read out of the shipped
// Tracks/<REG>/<Cn_Vn>/Gamedata.bgd event param blocks at
// `param + grid_slot * 0x98 + 0x90`.  The consumer is FUN_00172870
// @0x001728DD (see the generator's docstring for the full decode); every
// byte below is file data, none of it is tuned here.  [C]
//
//   aggression  rec+0x90 * 0.01 -> AI+0x9EC and AI+0x9E0
//   f91         rec+0x91 * 0.01 -> AI+0x9F4
//   f92         rec+0x92 * 0.01 -> AI+0x9F0
//   catchup     rec+0x93 * 0.01 -> AI+0x9E8, the fraction of the race for
//                                  which FUN_001734C0's catch-up stays armed
//   mode        (s8) rec+0x95   -> AI+0x9F8
//   flags       rec+0x96 & 1    -> AI+0xA12
//   f97         rec+0x97 * 0.01 -> AI+0x9E4
//
// rec+0x94 is not read by any retail code path and is carried verbatim.

#ifndef BURNOUT3_AI_PACE_H
#define BURNOUT3_AI_PACE_H

#define B3_AI_PACE_SLOTS 6

typedef struct B3AiPaceRec {
    unsigned char aggression, f91, f92, catchup, raw94, mode, flags, f97;
} B3AiPaceRec;

typedef struct B3AiPaceEvent {
    const char*  track;      /* "US_C3_V1"                        */
    const char*  event;      /* "FCRGSFFO" -- the mode-name table  */
    unsigned char n_opp;     /* param+0x3B4                        */
    B3AiPaceRec  slot[B3_AI_PACE_SLOTS];
} B3AiPaceEvent;

/* The offline forward single race -- the port's default event.
 * .data 0x3E9CD8's mode-name table: Off + SgRc + F. */
#define B3_AI_PACE_DEFAULT_EVENT "FCRGSFFO"

'''

FOOTER = '''
static const int B3_AI_PACE_COUNT =
    (int)(sizeof(B3_AI_PACE) / sizeof(B3_AI_PACE[0]));

/* FUN_00172870's lookup, minus the DAT_0073A170 indirection: the loader has
 * already chosen an event, so the port names it.  Returns NULL when the track
 * or event is unknown, which is retail's no-record branch (@0x001728F0):
 * AI+0x9E8 = 0, i.e. catch-up expires on the first frame. */
static inline const B3AiPaceEvent*
b3_ai_pace_find(const char* track, const char* event)
{
    int i;
    const char* a;
    const char* b;
    if (!track) return 0;
    if (!event || !*event) event = B3_AI_PACE_DEFAULT_EVENT;
    for (i = 0; i < B3_AI_PACE_COUNT; i++) {
        for (a = B3_AI_PACE[i].track, b = track; *a && *a == *b; a++, b++) { }
        if (*a || *b) continue;
        for (a = B3_AI_PACE[i].event, b = event; *a && *a == *b; a++, b++) { }
        if (*a || *b) continue;
        return &B3_AI_PACE[i];
    }
    return 0;
}

/* AI+0x9E8 for one grid slot.  Retail's own gate is `slot >= DAT_0073A180`
 * (@0x001728DD), the opponent count, so a slot past the field gets 0. */
static inline float b3_ai_pace_catchup_window(const B3AiPaceEvent* ev, int slot)
{
    if (!ev || slot < 0 || slot >= B3_AI_PACE_SLOTS) return 0.0f;
    if (slot >= (int)ev->n_opp) return 0.0f;
    return (float)ev->slot[slot].catchup * 0.01f;
}

/* AI+0x9EC / AI+0x9E0, the per-opponent aggression the attack machine gates on
 * ("Min. aggression before we start attacking" = 0.002). */
static inline float b3_ai_pace_aggression(const B3AiPaceEvent* ev, int slot)
{
    if (!ev || slot < 0 || slot >= B3_AI_PACE_SLOTS) return 0.0f;
    if (slot >= (int)ev->n_opp) return 0.0f;
    return (float)ev->slot[slot].aggression * 0.01f;
}

#endif /* BURNOUT3_AI_PACE_H */
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=DEFAULT_ROOT)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = a.out
    if not out:
        sys.exit("tools/gen_ai_pace.py no longer writes a src/ header: the "
                 "port reads build/tracks/<id>/pace.bin.\n"
                 "  extract it:  tools/cextract/build.sh && cxtract --track "
                 "<id> --only pace --out build/tracks/<id>\n"
                 "  (pass --out <path> to dump the old header shape "
                 "somewhere OUTSIDE src/ for comparison)")
    if os.path.abspath(out).startswith(os.path.join(here, 'src') + os.sep):
        sys.exit("refusing to write %s: baked game data does not belong in "
                 "src/ (tools/validate_no_baked_data.py enforces this)" % out)

    files = sorted(glob.glob(os.path.join(a.root, 'Tracks', '*', '*',
                                          'Gamedata.bgd')))
    if not files:
        sys.exit("no Gamedata.bgd under %s/Tracks" % a.root)

    rows = []
    for f in files:
        r = read_track(f)
        if not r:
            print("  skip (bad header): %s" % f)
            continue
        track, events = r
        for ev in events:
            rows.append((track, ev))

    body = ["static const B3AiPaceEvent B3_AI_PACE[] = {"]
    for track, ev in rows:
        body.append('    { "%s", "%s", %d, {' % (track, ev['id'], ev['n_opp']))
        for k, s in enumerate(ev['slots']):
            body.append('        { %3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d },'
                        '  /* slot %d  catch-up %.2f */'
                        % (s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                           k, s[3] / 100.0))
        body.append('    } },')
    body.append("};")

    with open(out, 'w') as fh:
        fh.write(HEADER)
        fh.write("\n".join(body))
        fh.write("\n")
        fh.write(FOOTER)

    print("wrote %s: %d events over %d tracks" %
          (out, len(rows), len(set(t for t, _ in rows))))
    std = [ev for t, ev in rows if ev['id'] == 'FCRGSFFO']
    if std:
        lo = min(e['slots'][5][3] for e in std) / 100.0
        hi = max(e['slots'][5][3] for e in std) / 100.0
        print("  FCRGSFFO last-place catch-up window: %.2f .. %.2f of the race"
              % (lo, hi))


if __name__ == '__main__':
    main()
