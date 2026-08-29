#!/usr/bin/env python3
"""In-game PROP-CONTACT suite -- does the car actually hit the track props,
and do they react?

The companion of tools/validate_collision_scenarios.py, which asks the same
two questions of traffic, opponents and walls.  The differential suite
tools/validate_props.py drives the recovered functions one at a time under
Unicorn; this one runs the WHOLE GAME headless and asserts on behaviour a
player notices:

  * a prop the car's swept box passed THROUGH must be contacted -- driving
    over a cone line and leaving cones standing is the reported defect
  * a contact must DELIVER: the prop has to leave its authored transform,
    tumble, and come to rest or be retired
  * the tall class-6 SIGNPOSTS have to be hittable at all.  They were not:
    the harness gave every prop a sphere of radius max(halfX, halfZ) at its
    mid height, which for the 1.12 x 6.14 x 0.42 m WF_sign_prop is a 0.56 m
    ball floating 3.07 m above the road -- out of a car's reach on every
    frame of every lap.  The gate is now retail's own FUN_001084E0, a
    15-axis separating-axis test between the car's box and the prop's model
    bbox under its instance transform (see src/burnout3_props.c for the
    address-by-address recovery).
  * the answer must be the same on two identical runs

The measurement lives in the game behind B3_PROP_AUDIT, so this file only
sets up, runs, parses and asserts.  The audit does NOT reuse the live gate's
verdict -- that would make the answer true by construction.  It sweeps the
car's box from its previous frame to its current one, blending the frame
axes, and asks the recovered SAT at 0.25 m intervals; a prop it marks swept
that the live path never marks hit is a MISS, and misses are what this suite
is for.

  [propaudit]  t= swept N admitted N missed N (cones a/b, signposts c/d) ...
  [propflight] t= knocked N flew N tumbled N atrest N maxdisp M signposts x/y

Run: python3 tools/validate_prop_contact.py [--seconds N] [--track ID]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# US_C1_V1 is the reference track for this suite: 455 prop instances, of
# which 375 are class-1 cones and 15 are the class-6 WF_sign_prop signposts,
# so both reported defects have samples on one track.  Nothing here is track
# specific -- --track runs it anywhere -- but the sample-count floors below
# are calibrated on this one.
TRACK = "US_C1_V1"

AUDIT_RE = re.compile(
    r'\[propaudit\] t=([\d.]+) swept (\d+) admitted (\d+) missed (\d+) '
    r'\(cones (\d+)/(\d+), signposts (\d+)/(\d+)\) untracked (\d+)')
FLIGHT_RE = re.compile(
    r'\[propflight\] t=([\d.]+) knocked (\d+) flew (\d+) tumbled (\d+) '
    r'atrest (\d+) maxdisp ([\d.]+) signposts (\d+)/(\d+) flew')
ARM_RE = re.compile(
    r'\[proparm\] t=([\d.]+) aface (\d+) bface (\d+) edge (\d+) lost (\d+) '
    r'creep ([\d.]+)')


def run_game(sim_seconds, env_extra, track):
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen",   # never a window on the user's desktop
        "SDL_AUDIODRIVER": "dummy",
        # Same pin, and the same reasoning, as validate_collision_scenarios:
        # this suite owns its conditions and the photoreal wave is proven
        # bit-identical at B3_PHOTO=0 by tools/validate_photo.py section 2.
        "B3_PHOTO": "0",
        "B3_TRACK": track,
        "B3_AUTODRIVE": "1",
        "B3_FIXED_DT": "0.0166667",       # deterministic tick
        "B3_PACE_MAX_TICKS": "1",         # no catch-up multi-ticking
        "B3_PROP_AUDIT": "1",
        "B3_EXIT_AT": str(sim_seconds),
    })
    env.update(env_extra)
    p = subprocess.run(["timeout", str(max(120, sim_seconds * 8)), "./burnout3"],
                       cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return (p.stdout.decode("utf-8", "replace")
            + p.stderr.decode("utf-8", "replace"))


def parse(log):
    """The LAST audit/flight pair in a run is that run's verdict."""
    a = AUDIT_RE.findall(log)
    f = FLIGHT_RE.findall(log)
    out = {"placements": len(re.findall(r'\[scenario\] prop hit try', log))}
    if a:
        m = a[-1]
        out.update(swept=int(m[1]), admitted=int(m[2]), missed=int(m[3]),
                   cone_hit=int(m[4]), cone_swept=int(m[5]),
                   sign_hit=int(m[6]), sign_swept=int(m[7]),
                   untracked=int(m[8]))
    if f:
        m = f[-1]
        out.update(knocked=int(m[1]), flew=int(m[2]), tumbled=int(m[3]),
                   atrest=int(m[4]), maxdisp=float(m[5]),
                   sign_flew=int(m[6]), sign_knocked=int(m[7]))
    m = ARM_RE.findall(log)
    if m:
        x = m[-1]
        out.update(aface=int(x[1]), bface=int(x[2]), edge=int(x[3]),
                   lost=int(x[4]), creep=float(x[5]))
    out["audit_lines"] = [x[1:] for x in a]
    return out


CHECKS = []


def check(name, ok, detail):
    CHECKS.append((name, bool(ok), detail))
    print("  %-56s %s   %s" % (name, "OK  " if ok else "FAIL", detail))
    return 0 if ok else 1


def main():
    seconds = 90
    track = TRACK
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])
    if "--track" in sys.argv:
        track = sys.argv[sys.argv.index("--track") + 1]

    fails = 0

    # ---- 1. the scripted CONE-LINE drive --------------------------------
    # B3_SCENARIO=props aims the player at the nearest still-standing prop of
    # B3_SCENARIO_PROPCLASS from 35 m and lets go.  Cones ship in lines, so
    # one placement carries the car through several.
    print("\ncone-line drive (B3_SCENARIO=props, class 1, %d s, %s):"
          % (seconds, track))
    cone = parse(run_game(seconds, {"B3_SCENARIO": "props:8",
                                    "B3_SCENARIO_PROPCLASS": "1"}, track))
    if "swept" not in cone:
        return check("cone drive produced an audit line", False,
                     "no [propaudit] in the log") or 1

    # A suite that only checks "nothing was missed" passes trivially on a run
    # that touched nothing, so the sample floor comes first.
    fails += check("cone drive: the drive reaches cones at all",
                   cone["cone_swept"] >= 20,
                   "%d cones swept by the car's box (>= 20)"
                   % cone["cone_swept"])
    # THE defect: geometry says the box went through it, the gate said no.
    fails += check("cone drive: every swept prop is contacted",
                   cone["missed"] == 0,
                   "%d swept, %d admitted, %d MISSED"
                   % (cone["swept"], cone["admitted"], cone["missed"]))
    fails += check("cone drive: admission >= geometric overlap count",
                   cone["admitted"] >= cone["swept"],
                   "admitted %d vs swept %d"
                   % (cone["admitted"], cone["swept"]))
    fails += check("cone drive: every cone the box swept is contacted",
                   cone["cone_hit"] >= cone["cone_swept"],
                   "%d/%d cones" % (cone["cone_hit"], cone["cone_swept"]))
    # A contact that leaves the prop standing is no better than no contact.
    fails += check("cone drive: a contacted prop leaves its transform",
                   cone.get("knocked", 0) >= 1
                   and cone.get("flew", 0) == cone.get("knocked", 0),
                   "%d knocked, %d moved more than 0.5 m"
                   % (cone.get("knocked", 0), cone.get("flew", 0)))
    fails += check("cone drive: knocked props tumble",
                   cone.get("knocked", 0) >= 1
                   and cone.get("tumbled", 0) >= 0.8 * cone["knocked"],
                   "%d/%d turned past 60 deg"
                   % (cone.get("tumbled", 0), cone.get("knocked", 0)))
    # Retail gives a knocked prop one of 16 bodies and stops it two ways: the
    # world resolve freezes it (FUN_00109EA0's sleep latch) or the pool
    # retires it to slot type 8 (FUN_00114730 @0x0011480C).  Neither is a
    # prop that never stops.
    fails += check("cone drive: knocked props come to rest or retire",
                   cone.get("knocked", 0) >= 1
                   and cone.get("atrest", 0) >= 0.5 * cone["knocked"],
                   "%d/%d frozen or retired (max travel %.0f m)"
                   % (cone.get("atrest", 0), cone.get("knocked", 0),
                      cone.get("maxdisp", 0.0)))

    # ---- THE POST-CONTACT LEGS (INTEGRATION_NOTE.md section 12) ----------
    # These guard the two defects that hid behind "max travel", which on its
    # own cannot tell a prop flying away from a prop falling through the road.
    #
    # (a) NOTHING LEAVES THE WORLD.  The single plane-per-polygon narrow phase
    #     dropped resting props through the surface; once below it, the
    #     gather's own y-band could not reach the road again and they fell to
    #     the drag terminal velocity until the pool recycled them, which is
    #     where the 900 m came from.  b3_props_update retires a prop that has
    #     no soup, no surface under it and is 25 m below its own ground; that
    #     retirement is a LAST RESORT: every prop it fires on is one the narrow
    #     phase let through, so this is a BOUND and not a zero.  It is not zero
    #     because it honestly is not zero -- a prop landing where two surfaces
    #     meet can still be pushed through by the summed-normal contact, which
    #     is the one part of section 12 that is bounded rather than fixed.
    #     Before the wave this was unbounded: the props kept their bodies and
    #     fell for the rest of the run.
    lost_cap = max(3, int(0.05 * cone.get("knocked", 0)))
    fails += check("cone drive: props almost never leave the world",
                   cone.get("lost", 0) <= lost_cap,
                   "%d of %d knocked retired for leaving the world (<= %d)"
                   % (cone.get("lost", 0), cone.get("knocked", 0), lost_cap))
    # (b) A SETTLED PROP IS ACTUALLY STILL.  FUN_00109560 @0x00109692 zeroes
    #     the velocity of a body whose +0x20E latch is up.  The port raised
    #     the latch and never acted on it, so settled props crept upward at
    #     0.15-0.31 m/s for the rest of the race.
    #     NOT asserted as bit-zero: the freeze deliberately does not clear the
    #     IMPULSE accumulator +0x110 (retail rejoins the normal path
    #     @0x00109728 so a car can still knock a settled prop), so a frozen
    #     body that also took a world-contact impulse the same frame carries
    #     it.  The band is well under the defect it guards.
    fails += check("cone drive: a frozen prop does not creep",
                   cone.get("creep", 1.0) <= 0.05,
                   "fastest frozen body %.4f m/s (want <= 0.05; the defect "
                   "this guards sat at 0.15-0.31)" % cone.get("creep", -1.0))
    # (c) THE CONTACT POINT ARMS.  FUN_001084E0 picks the point by which axis
    #     won, and the port took box A's support point unconditionally -- for
    #     86% of contacts, the wrong box.  A drive that exercises only one arm
    #     would let the other two rot, so assert all three are reached.
    fails += check("cone drive: all three contact-point arms are exercised",
                   cone.get("aface", 0) > 0 and cone.get("bface", 0) > 0
                   and cone.get("edge", 0) > 0,
                   "A-face %d, B-face %d, edge %d"
                   % (cone.get("aface", 0), cone.get("bface", 0),
                      cone.get("edge", 0)))

    # ---- 2. the SIGNPOST hit --------------------------------------------
    # The class-6 signposts were the un-hittable family: 0 of every one the
    # car drove through was contacted before the box gate landed.
    print("\nsignpost hit (B3_SCENARIO=props, class 6, %d s, %s):"
          % (seconds, track))
    sign = parse(run_game(seconds, {"B3_SCENARIO": "props:8",
                                    "B3_SCENARIO_PROPCLASS": "6",
                                    "B3_SCENARIO_SPEED": "45"}, track))
    if "swept" not in sign:
        return check("sign drive produced an audit line", False,
                     "no [propaudit] in the log") or 1
    fails += check("sign drive: the drive reaches signposts",
                   sign["sign_swept"] >= 2,
                   "%d signposts swept by the car's box (>= 2)"
                   % sign["sign_swept"])
    fails += check("sign drive: signposts are hittable",
                   sign["sign_hit"] >= sign["sign_swept"],
                   "%d/%d signposts contacted"
                   % (sign["sign_hit"], sign["sign_swept"]))
    fails += check("sign drive: a hit signpost FLIES",
                   sign.get("sign_knocked", 0) >= 1
                   and sign.get("sign_flew", 0) == sign.get("sign_knocked", 0),
                   "%d/%d knocked signposts left their transform by > 0.5 m"
                   % (sign.get("sign_flew", 0), sign.get("sign_knocked", 0)))
    fails += check("sign drive: nothing swept is missed",
                   sign["missed"] == 0,
                   "%d swept, %d admitted, %d MISSED"
                   % (sign["swept"], sign["admitted"], sign["missed"]))

    # ---- 3. DETERMINISM --------------------------------------------------
    # B3_FIXED_DT plus the scripted placement should make the whole run
    # reproducible; a gate that depends on frame timing would show up here as
    # a different admitted count.  Compare the whole per-second series, not
    # just the last line, so a divergence that heals is still caught.
    print("\ndeterminism (two identical cone-line runs):")
    a = parse(run_game(45, {"B3_SCENARIO": "props:8",
                            "B3_SCENARIO_PROPCLASS": "1"}, track))
    b = parse(run_game(45, {"B3_SCENARIO": "props:8",
                            "B3_SCENARIO_PROPCLASS": "1"}, track))
    fails += check("determinism: the audit series is identical",
                   a["audit_lines"] == b["audit_lines"]
                   and len(a["audit_lines"]) > 5,
                   "%d audit lines, %s"
                   % (len(a["audit_lines"]),
                      "identical" if a["audit_lines"] == b["audit_lines"]
                      else "DIVERGED"))
    fails += check("determinism: the same props are knocked",
                   a.get("knocked") == b.get("knocked")
                   and a.get("maxdisp") == b.get("maxdisp"),
                   "run1 knocked %s maxdisp %s / run2 knocked %s maxdisp %s"
                   % (a.get("knocked"), a.get("maxdisp"),
                      b.get("knocked"), b.get("maxdisp")))

    # ---- 4. FREE-RUNNING autodrive, no scenario --------------------------
    # The scripted legs guarantee samples; this one asserts the same contract
    # on the props an ordinary lap happens to meet, which is what the user
    # sees.
    print("\nfree-running autodrive (no scenario, %d s, %s):" % (seconds, track))
    free = parse(run_game(seconds, {}, track))
    if "swept" in free:
        fails += check("autodrive: props are met at all",
                       free["swept"] >= 10,
                       "%d props swept" % free["swept"])
        fails += check("autodrive: every swept prop is contacted",
                       free["missed"] == 0,
                       "%d swept, %d admitted, %d MISSED"
                       % (free["swept"], free["admitted"], free["missed"]))
        fails += check("autodrive: no contact without a swept sample",
                       free["untracked"] == 0,
                       "%d untracked contacts" % free["untracked"])
    else:
        fails += check("autodrive produced an audit line", False,
                       "no [propaudit] in the log")

    total = len(CHECKS)
    print("\n%d/%d prop contact checks pass "
          "(cone-line drive, signpost hit, determinism, free autodrive)"
          % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
