#!/usr/bin/env python3
"""Per-track drive test: every extracted track, full asset load + a real
autodrive field, asserting the things a player notices first.

Per track:
  assets   geometry loads (vert/tri floor), all material groups resolve,
           the PER-TRACK collision world loads (not the global fallback),
           the retail nav graph loads
  driving  every car: no fall-through (sustained below-route without any
           surface in the probe column, outside crash windows), actually
           moves (top speed floor), and advances along the route
           (wrap-aware progress accumulation)

The in-game monitor is B3_TRACK_TEST=1 in src/burnout3_full.c; its own
history is instructive: the first sampler probed the MIRRORED map (harness
coords through a game-space wrapper), the second read the route-line
fallback as "-39 m under ground", and the third counted Silver Lake's jumps
as falls until the below-route discriminator went in.  Trust it only with
the [tracktest] fields it prints today.

Run: python3 tools/validate_tracks.py [--seconds N] [--tracks A,B,...]
                                      [--jobs N]

--jobs N runs N tracks concurrently (each its own game process; the sim is
per-process deterministic, so parallelism only contends for wall clock --
the per-run timeout is scaled by the job count to absorb that).
"""
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MIN_VERTS = 10000
MIN_TRIS = 10000
MIN_COLLISION_TRIS = 5000
MIN_MPH = 30.0
MIN_PSPAN = 0.05
MIN_CLEAR = -3.0


def run_track(track, seconds, jobs):
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
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
        "B3_AUTODRIVE": "1",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(seconds),
        "B3_TRACK": track,
        "B3_TRACK_TEST": "1",
    })
    # concurrent runs contend for wall clock; the cap scales with the
    # fleet so a slow neighbour cannot turn into a fake "0 cars reported"
    cap = max(180, seconds * 8) * (1 if jobs <= 1 else 2 + jobs // 4)
    p = subprocess.run(["timeout", str(cap), "./burnout3"],
                       cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.stdout.decode("utf-8", "replace")


def evaluate(track, seconds, log):
    """Returns (lines, passes, fails) for one track's report."""
    lines = []
    counts = [0, 0]

    def check(name, ok, detail):
        counts[0 if ok else 1] += 1
        lines.append("  %-46s %s   %s"
                     % (name, "OK  " if ok else "FAIL", detail))
        return 0 if ok else 1

    m = re.search(r'REAL track geometry: (\d+) verts, (\d+) tris', log)
    check("geometry loads", bool(m) and int(m.group(1)) >= MIN_VERTS
          and int(m.group(2)) >= MIN_TRIS,
          m.group(0)[20:] if m else "no geometry line")

    m = re.search(r'REAL textures: (\d+) loaded, (\d+) groups unresolved',
                  log)
    check("materials resolve", bool(m) and int(m.group(2)) == 0
          and int(m.group(1)) > 0,
          m.group(0)[15:] if m else "no texture line")

    m = re.search(r'collision: build/tracks/%s/collision\.bin' % track, log)
    m2 = re.search(r'GAME collision world: (\d+) triangles', log)
    check("per-track collision loads",
          bool(m) and bool(m2) and int(m2.group(1)) >= MIN_COLLISION_TRIS,
          (m2.group(0)[21:] if m2 else "none")
          + ("" if m else "  [GLOBAL FALLBACK]"))

    m = re.search(r'retail nav: (\d+) points, (\d+) sections, (\d+) plans',
                  log)
    check("nav graph loads", bool(m) and int(m.group(1)) > 0
          and int(m.group(2)) > 0,
          m.group(0)[12:] if m else "no nav line")

    cars = re.findall(r'\[tracktest\] car(\d) falls=(\d+) probe_miss=(\d+) '
                      r'min_clear=([-\d.]+) p_span=([\d.]+) laps=(\d+) '
                      r'max_mph=([\d.]+)', log)
    check("drive report present", len(cars) >= 2,
          "%d cars reported" % len(cars))
    if cars:
        falls = sum(int(c[1]) for c in cars)
        minc = min(float(c[3]) for c in cars)
        slow = [c[0] for c in cars if float(c[6]) < MIN_MPH]
        # the progress floor is calibrated at 60 sim-s; scale it so a
        # shorter run does not read slow-but-moving fields as stuck
        pspan_floor = MIN_PSPAN * (seconds / 60.0)
        stuck = [c[0] for c in cars if float(c[4]) < pspan_floor]
        check("no car falls through the world", falls == 0,
              "%d sustained below-route falls" % falls)
        check("no car sinks into the road",
              minc > MIN_CLEAR and minc < 90.0,
              "min clearance %.2f m" % minc)
        check("every car reaches speed", not slow,
              ("cars below %d mph: %s" % (MIN_MPH, slow))
              if slow else "all >= %d mph" % MIN_MPH)
        check("every car advances along the route", not stuck,
              ("cars stuck: %s spans=%s (floor %.3f)"
               % (stuck, [c[4] for c in cars], pspan_floor))
              if stuck else
              "min span %.3f" % min(float(c[4]) for c in cars))

    return lines, counts[0], counts[1]


def main():
    seconds = 60
    only = None
    jobs = 1
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])
    if "--tracks" in sys.argv:
        only = sys.argv[sys.argv.index("--tracks") + 1].split(",")
    if "--jobs" in sys.argv:
        jobs = max(1, int(sys.argv[sys.argv.index("--jobs") + 1]))

    tracks = sorted(
        t for t in os.listdir(os.path.join(ROOT, "build", "tracks"))
        if os.path.isfile(os.path.join(ROOT, "build", "tracks", t,
                                       "route.bin")))
    if only:
        tracks = [t for t in tracks if t in only]

    passes = fails = 0
    if jobs <= 1:
        results = ((t, run_track(t, seconds, 1)) for t in tracks)
        for track, log in results:
            print("\n%s (%d sim-s):" % (track, seconds))
            lines, p, f = evaluate(track, seconds, log)
            for ln in lines:
                print(ln)
            sys.stdout.flush()
            passes += p
            fails += f
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {t: ex.submit(run_track, t, seconds, jobs)
                    for t in tracks}
            # print in track order as each becomes ready, so the log reads
            # the same as the serial suite
            for track in tracks:
                log = futs[track].result()
                print("\n%s (%d sim-s):" % (track, seconds))
                lines, p, f = evaluate(track, seconds, log)
                for ln in lines:
                    print(ln)
                sys.stdout.flush()
                passes += p
                fails += f

    print("\n%d/%d track drive checks pass over %d tracks"
          % (passes, passes + fails, len(tracks)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
