#!/usr/bin/env python3
"""Frames of flight: a cone and a signpost clipped at racing speed.

The prop-contact suite says a knocked prop leaves its transform, tumbles and
comes to rest; that is a number.  This is the picture behind it, and it is the
instrument that would have caught the defect the numbers hid for a wave -- a
prop falling THROUGH the road looks identical to a prop flying away if all you
read is "max travel".

One headless run per prop class, `B3_SHOT_SEQ` writing a numbered BMP every
`--every` frames from the frame the scenario's first knock lands on, then
photo_strip's contact sheet.  Nothing here is a test: it renders, it does not
assert.  The assertions live in tools/validate_prop_contact.py (in-game) and
tools/validate_props.py (against the real x86 under Unicorn).

  python3 tools/prop_flight_strip.py [--track US_C1_V1] [--out build/props]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

KNOCK = re.compile(r'\[propknock\] t=([\d.]+) inst (\d+)')
# `[prop] car C hit inst N model M class K ...` -- carries the CAR and the
# prop CLASS, which [propknock] does not.  Needed because the camera follows
# car 0: the first knock in a run is often an AI flattening something off
# screen, and a strip of that is a strip of nothing.
PLAYER_HIT = re.compile(r'\[prop\] car 0 hit inst (\d+) model \d+ class (\d+)')


def run(env_extra, seconds, seqdir=None, first=None, every=None):
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen",     # never a window on the desktop
        "SDL_AUDIODRIVER": "dummy",
        "B3_PHOTO": "0",
        "B3_AUTODRIVE": "1",
        "B3_FIXED_DT": "0.0166667",         # deterministic tick
        "B3_PACE_MAX_TICKS": "1",
        "B3_PROP_TRACE": "1",
        "B3_EXIT_AT": str(seconds),
    })
    env.update(env_extra)
    if seqdir:
        env["B3_SHOT_SEQ"] = seqdir
        env["B3_SHOT_FIRST"] = str(first)
        env["B3_SHOT_EVERY"] = str(every)
    p = subprocess.run(["timeout", str(max(180, seconds * 10)), "./burnout3"],
                       cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return (p.stdout.decode("utf-8", "replace")
            + p.stderr.decode("utf-8", "replace"))


def strip(track, prop_class, name, out_dir, n, every, lead, camside):
    base = {"B3_TRACK": track, "B3_SCENARIO": "props:8",
            "B3_SCENARIO_PROPCLASS": str(prop_class)}
    # The chase camera looks along the car's nose, which is the one direction a
    # clipped prop does NOT go: it puts the whole event under the bonnet and
    # forty metres away.  B3_CAMSIDE parks the camera broadside at 7 m, which
    # is the frame the reaction actually happens in.
    if camside:
        base["B3_CAMSIDE"] = "1"
    # PASS 1: when does the first knock of this class actually land?  The
    # scenario aims the car, but how long it takes to get there is not fixed,
    # so the capture window is measured rather than guessed.
    log = run(base, 40)
    # the first prop of THIS class the PLAYER hit, and when it was promoted
    want = None
    for m in PLAYER_HIT.finditer(log):
        if int(m.group(2)) == prop_class:
            want = int(m.group(1))
            break
    t = None
    for m in KNOCK.finditer(log):
        if want is None or int(m.group(2)) == want:
            t = float(m.group(1))
            break
    if t is None:
        print("  %s: the player never hit a class-%d prop -- skipped"
              % (name, prop_class))
        return None
    frame = int(t * 60.0)
    first = max(1, frame - lead)
    print("  %s: first knock t=%.2f (frame %d), capturing from %d every %d"
          % (name, t, frame, first, every))
    seq = os.path.join(out_dir, name + "_seq")
    shutil.rmtree(seq, ignore_errors=True)
    os.makedirs(seq, exist_ok=True)
    # PASS 2: same deterministic conditions, now with the camera running.
    run(base, int(t) + 6, seqdir=seq, first=first, every=every)
    frames = sorted(f for f in os.listdir(seq) if f.endswith(".bmp"))[:n]
    if not frames:
        print("  %s: no frames written -- skipped" % name)
        return None
    import photo_strip
    paths = [os.path.join(seq, f) for f in frames]
    labels = ["%s  t=%+.2fs" % (name, (first + i * every) / 60.0 - t)
              for i in range(len(paths))]
    out = os.path.join(out_dir, name + "_flight.png")
    photo_strip.contact_sheet(paths, labels, out, cols=4, width=520)
    print("  %s: %d frames -> %s" % (name, len(paths), out))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--track", default="US_C1_V1")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "props"))
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--every", type=int, default=4)
    ap.add_argument("--lead", type=int, default=6)
    ap.add_argument("--chase", action="store_true",
                    help="use the chase camera instead of the broadside one")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    print("prop flight strips, track %s" % a.track)
    made = []
    for cls, name in ((1, "cone"), (6, "signpost")):
        p = strip(a.track, cls, name, a.out, a.frames, a.every, a.lead,
                  not a.chase)
        if p:
            made.append(p)
    if not made:
        return 1
    print("\n".join(made))
    return 0


if __name__ == "__main__":
    sys.exit(main())
