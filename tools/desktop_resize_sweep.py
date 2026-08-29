#!/usr/bin/env python3
"""
desktop_resize_sweep -- the DESKTOP half of tools/web_resize_sweep.py.

The web has had a resize gate since the ResizeObserver work; the desktop has
had only the in-game B3_RESIZE_SWEEP env and no harness around it, and that is
how the drag freeze got in.  Everything here is one binary, offscreen and mute.

WHAT A RESIZE COSTS, and why one leg is not enough.  afx_resize() rebuilds the
whole chain -- fifteen colour targets, the depth-attachment format probe, the
multisampled colour+depth renderbuffer pair, seventeen framebuffer completeness
checks -- whenever the window size differs from the last one by ANY amount.  A
programmatic resize is one such event.  A DRAGGED BORDER is one per frame:

    STEPS   four sizes, well apart, 30 frames between them.  The ordinary
            resize, and the property is the old one -- every step rebuilds and
            the chain never retires.  This is what used to be the whole test.

    FLOOD   twenty resizes in two seconds, mid-race, RT on and off.  The
            playtest report ("resizing the window freezes the game") in its
            mildest form.  MUST SURVIVE and must land on the final size.

    DRAG    one resize PER FRAME for two seconds -- what a border drag
            actually delivers.  Gates the SETTLE (resize_settle() in
            src/burnout3_full.c): the rebuild count must be bounded by the
            one-a-second cap rather than tracking the step count.

    CONTROL the same drag with B3_RESIZE_SETTLE=0, i.e. the pre-fix engine.
            Not a failure leg -- it is the measurement that gives DRAG's
            number a meaning, and it fails only if the knob stops working.

    OSC     a size that never holds still, two values alternating every frame.
            A fractional-scaling compositor rounding against its own configure
            produces exactly this, and it is the shape that does not END: with
            no cap the chain rebuilds for the rest of the run.

    PAUSED  a flood with the SETTINGS MENU OPEN.  The overlay runs between
            frame_begin and frame_end with the scene target bound, and it is
            the one place a rebuild could tear a target down underneath the
            frame drawing into it.

Usage:
    python3 tools/desktop_resize_sweep.py
    python3 tools/desktop_resize_sweep.py --only flood --verbose
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, "burnout3")
TRACK = "US_C3_V1"

_fail = 0
_pass = 0


def check(ok, what, detail=""):
    global _fail, _pass
    if ok:
        _pass += 1
        print("  PASS  %s" % what)
    else:
        _fail += 1
        print("  FAIL  %s" % what)
    if detail:
        print("        %s" % detail)
    return ok


def run(name, sweep, every, rt, exit_at, extra=None, timeout=600):
    """One offscreen run.  Returns (returncode, stdout)."""
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIODRIVER"] = "dummy"
    env.update({
        "B3_TRACK": TRACK, "B3_RES": "640x480", "B3_TESTDRIVE": "1",
        "B3_FIXED_DT": "0.0166667", "B3_PACE_MAX_TICKS": "1",
        "B3_AFX": "1", "B3_PHOTO": "1", "B3_MUSIC_SEED": "1",
        "B3_RT": str(rt),
        "B3_RESIZE_SWEEP": sweep,
        "B3_RESIZE_SWEEP_EVERY": str(every),
        "B3_EXIT_AT": str(exit_at),
    })
    # the harness must not inherit a developer's own pins
    for k in ("B3_RESIZE_SETTLE", "B3_RESIZE_SETTLE_MAX", "B3_MSAA",
              "B3_SHOT", "B3_SHOT_FRAME", "B3_PAUSE_AT", "B3_PAUSE_KEYS"):
        env.pop(k, None)
    if extra:
        env.update(extra)
    p = subprocess.run(["timeout", str(timeout), GAME], cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


RE_STEP = re.compile(r"resize sweep step (\d+) -> (\d+)x(\d+)")
RE_READY = re.compile(r"\[afx\] chain ready (\d+)x(\d+) .*?rebuilt from")
RE_ANY_READY = re.compile(r"\[afx\] chain ready (\d+)x(\d+)")


def facts(log):
    steps = RE_STEP.findall(log)
    return {
        "steps": len(steps),
        "last_size": (steps[-1][1], steps[-1][2]) if steps else None,
        "rebuilds": len(RE_READY.findall(log)),
        "ready_sizes": RE_ANY_READY.findall(log),
        "complete": "resize sweep complete" in log,
        "retired": "THE CHAIN IS RETIRED" in log,
        "exited": "B3_EXIT_AT" in log,
        "fatal": "FATAL" in log,
    }


def drag_list(n, w0=1100, h0=760, dw=3, dh=2):
    """A continuous drag: n sizes, each a few pixels from the last."""
    out = []
    w, h = w0, h0
    for _ in range(n):
        w += dw
        h += dh
        out.append("%dx%d" % (w, h))
    return ",".join(out)


def survives(tag, rc, log, f, final_expected=True):
    ok = check(rc == 0 and not f["fatal"],
               "%s: the run survives the resize traffic and exits cleanly"
               % tag,
               "rc=%d, %d step(s) applied" % (rc, f["steps"]))
    check(not f["retired"],
          "%s: ...and the chain is NOT retired -- the frame never falls back "
          "to the legacy postfx path" % tag)
    check(f["complete"], "%s: ...and every step in the list was applied" % tag)
    if final_expected and f["last_size"] and f["ready_sizes"]:
        want = "%sx%s" % f["last_size"]
        got = "%sx%s" % tuple(f["ready_sizes"][-1])
        check(got == want,
              "%s: ...and the chain SETTLES ON THE FINAL SIZE -- a throttle "
              "that dropped the last resize would leave the window's own "
              "pixels stretched for the rest of the run" % tag,
              "chain at %s, window at %s" % (got, want))
    return ok


def leg_steps(rt):
    tag = "STEPS rt=%d" % rt
    print("\n== %s: four sizes, 30 frames apart ==" % tag)
    rc, log = run("steps", "800x600,1024x768,640x480,1280x720", 30, rt, 25)
    f = facts(log)
    survives(tag, rc, log, f)
    check(f["rebuilds"] == f["steps"],
          "%s: an ordinary resize still rebuilds ONCE PER STEP -- the settle "
          "delays a rebuild, it must never swallow one" % tag,
          "%d rebuild(s) for %d step(s)" % (f["rebuilds"], f["steps"]))
    return log


def leg_flood(rt):
    """The playtest report: twenty resizes in two seconds, mid-race."""
    tag = "FLOOD rt=%d" % rt
    print("\n== %s: 20 resizes in 2 s, mid-race ==" % tag)
    # 20 steps, one every 6 frames = 2.0 s at the pinned 60 Hz tick
    sweep = drag_list(20, 700, 520, 31, 19)
    rc, log = run("flood", sweep, 6, rt, 25)
    f = facts(log)
    survives(tag, rc, log, f)
    return log


def leg_drag(rt, settle_off=False):
    """What a dragged border actually delivers: one resize per frame."""
    tag = "CONTROL rt=%d" % rt if settle_off else "DRAG rt=%d" % rt
    n = 120                                   # 2 s of dragging at 60 Hz
    print("\n== %s: one resize PER FRAME, %d of them ==" % (tag, n))
    extra = {"B3_RESIZE_SETTLE": "0"} if settle_off else None
    rc, log = run("drag", drag_list(n), 1, rt, 25, extra=extra)
    f = facts(log)
    survives(tag, rc, log, f)
    if settle_off:
        # Not a property of the shipped engine -- the measurement that gives
        # the DRAG leg's number a scale, and the proof the knob still works.
        check(f["rebuilds"] >= n - 2,
              "%s: with the settle OFF the chain rebuilds once per resize -- "
              "this is the cost the settle removes, and B3_RESIZE_SETTLE=0 "
              "still reaches it" % tag,
              "%d rebuild(s) for %d step(s)" % (f["rebuilds"], f["steps"]))
    else:
        # The cap is one rebuild per B3_RESIZE_SETTLE_MAX frames (60), plus
        # the one that lands when the drag stops.  Anything near the step
        # count means the throttle is not running.
        cap = n // 60 + 3
        check(f["rebuilds"] <= cap,
              "*** %s: A DRAG IS NOT A REBUILD STORM *** -- %d resizes cost "
              "at most %d chain rebuilds, not %d"
              % (tag, n, cap, n),
              "%d rebuild(s) for %d step(s)" % (f["rebuilds"], f["steps"]))
    return log


def leg_osc(rt):
    """A size that never holds still -- the shape that does not end."""
    tag = "OSC rt=%d" % rt
    n = 300
    print("\n== %s: two sizes alternating every frame, %d of them =="
          % (tag, n))
    sweep = ",".join(("1280x800" if i % 2 else "1281x801") for i in range(n))
    rc, log = run("osc", sweep, 1, rt, 25)
    f = facts(log)
    survives(tag, rc, log, f, final_expected=False)
    check(f["rebuilds"] <= n // 60 + 3,
          "*** %s: AN OSCILLATING WINDOW SIZE COSTS A BOUNDED NUMBER OF "
          "REBUILDS *** -- a drag ends, a compositor rounding loop does not, "
          "and without the cap this is a rebuild every frame for the rest of "
          "the run" % tag,
          "%d rebuild(s) for %d step(s)" % (f["rebuilds"], f["steps"]))
    return log


def leg_paused(rt):
    """A flood with the SETTINGS MENU OPEN."""
    tag = "PAUSED rt=%d" % rt
    print("\n== %s: 20 resizes with the settings menu open ==" % tag)
    sweep = drag_list(20, 700, 520, 31, 19)
    # the overlay freezes the race clock, so the run is ended by a frame
    # number rather than by B3_EXIT_AT (which would never arrive)
    extra = {"B3_PAUSE_AT": "200", "B3_PAUSE_ROW": "4",
             "B3_SHOT": os.path.join(ROOT, "build", "resize_paused.bmp"),
             "B3_SHOT_FRAME": "420"}
    rc, log = run("paused", sweep, 6, rt, 900, extra=extra)
    f = facts(log)
    check(rc == 0 and not f["fatal"],
          "%s: the run survives a resize flood taken while the pause overlay "
          "is drawing -- the overlay runs between frame_begin and frame_end "
          "with the scene target bound" % tag,
          "rc=%d, %d step(s) applied" % (rc, f["steps"]))
    check(not f["retired"], "%s: ...and the chain is NOT retired" % tag)
    check(f["complete"], "%s: ...and every step in the list was applied" % tag)
    return log


LEGS = {
    "steps":   lambda: (leg_steps(0), leg_steps(1)),
    "flood":   lambda: (leg_flood(0), leg_flood(1)),
    "drag":    lambda: (leg_drag(1), leg_drag(1, settle_off=True)),
    "osc":     lambda: leg_osc(1),
    "paused":  lambda: leg_paused(0),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", action="append", choices=sorted(LEGS),
                    help="run only these legs (repeatable)")
    args = ap.parse_args()

    if not os.path.exists(GAME):
        print("no %s -- run `make` first" % GAME)
        return 2

    print("desktop_resize_sweep -- the resize path, offscreen and mute")
    for name in (args.only or sorted(LEGS)):
        LEGS[name]()

    print("\n%d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
