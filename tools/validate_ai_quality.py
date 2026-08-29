#!/usr/bin/env python3
"""Per-track RIVAL DRIVING-QUALITY gate.

WHY THIS EXISTS
---------------
`tools/validate_tracks.py` proves a track LOADS and that its cars move: no
falls, no sinking, >= 30 mph somewhere, some route progress.  None of that
notices a rival that spends the lap grinding a barrier, or one that crawls a
sector because the corner-brake law read the wrong segment length.  The user
report this exists to close is exactly that: "the opponent racers seem to be
driving poorly on other maps".

It measures three things per track, all from instrumentation that ALREADY
exists in src/burnout3_full.c:

  wall crashes   `[aicrash] SUMMARY ... carN=n(tdX/tfY/wlZ) ... RIVALS=n`
                 emitted at B3_EXIT_AT under B3_AI_CRASHLOG.  `wl` is kind 2,
                 "world" -- a wreck that was neither a takedown nor a traffic
                 hit, i.e. the car put itself into the scenery.  This is the
                 direct wall-grinding metric.
  speed band     `max_mph` from the `[tracktest]` line, per car.
  progress       `p_span`, the same wrap-aware forward-delta accumulation
                 validate_tracks.py gates on, reused here so a "quiet" track
                 that simply stopped driving cannot pass by having no crashes.

A/B MODE
--------
`--ab` runs every selected track TWICE, once with B3_NAV_CLAMP=0 and once
with B3_NAV_CLAMP=1, and reports the delta.  That is the measurement the
corridor push-back patch (retail 0x00170C60, FUN_00173E40's wall push) has to
win before its default is flipped: rival wall crashes DOWN, p_span not worse.

GAME LOCK
---------
The repo runs long verification suites; a concurrent game run has already
corrupted one suite result.  This script refuses to start when another
validator or ./burnout3 is live, and holds
<scratch>/game.lock (or --lock-dir) for its whole run.

USAGE
    python3 tools/validate_ai_quality.py --seconds 90
    python3 tools/validate_ai_quality.py --tracks EU_M1_V1,US_P1_V2 --ab
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    '..'))
DEFAULT_LOCK = os.path.join(ROOT, 'build', 'game.lock')

# ---- thresholds -----------------------------------------------------------
# Calibrated per 60 sim-seconds and scaled by the run length.  These are
# HARNESS thresholds, not retail constants -- they encode "a rival that drives
# acceptably", and the only retail-sourced number here is the takedown/traffic
# vs world split that ai_crash_note already makes.
MAX_RIVAL_WALL_PER_60S = 2.0     # wrecks/rival/60 s attributed to the world
MIN_MEAN_MPH = 45.0              # mean of per-car max_mph across the field
MIN_PSPAN_PER_60S = 0.05         # same floor validate_tracks.py uses

CRASH_RE = re.compile(r'car(\d)=(\d+)\(td(\d+)/tf(\d+)/wl(\d+)\)')
SUMMARY_RE = re.compile(r'\[aicrash\] SUMMARY[^\n]*')
TRACKTEST_RE = re.compile(
    r'\[tracktest\] car(\d) falls=(\d+) probe_miss=(\d+) '
    r'min_clear=([-\d.]+) p_span=([\d.]+) laps=(\d+) max_mph=([\d.]+)')


def busy():
    """Any other validator or game process live?"""
    try:
        ps = subprocess.run(['ps', 'aux'], stdout=subprocess.PIPE,
                            text=True).stdout
    except Exception:
        return None
    mine = str(os.getpid())
    for line in ps.splitlines():
        if 'validate_ai_quality' in line:
            continue
        if re.search(r'validate_\w+\.py|/burnout3\b|\./burnout3', line):
            if mine not in line.split()[:2]:
                return line.strip()
    return None


class Lock(object):
    def __init__(self, path):
        self.path = path

    def __enter__(self):
        for _ in range(3):
            try:
                os.makedirs(self.path)
                return self
            except OSError:
                time.sleep(1.0)
        raise SystemExit('game lock %s is held; refusing to run' % self.path)

    def __exit__(self, *a):
        try:
            os.rmdir(self.path)
        except OSError:
            pass


def run_track(track, seconds, clamp):
    env = dict(os.environ)
    env.update({
        'SDL_VIDEODRIVER': 'offscreen',
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
        'SDL_AUDIODRIVER': 'dummy',
        'B3_FIXED_DT': '0.0166667',
        'B3_PACE_MAX_TICKS': '1',
        'B3_EXIT_AT': str(seconds),
        'B3_AUTODRIVE': '1',
        'B3_TRACK_TEST': '1',
        'B3_AI_CRASHLOG': '1',
        'B3_TRACK': track,
    })
    if clamp is not None:
        env['B3_NAV_CLAMP'] = '1' if clamp else '0'
    p = subprocess.run(['timeout', str(max(180, seconds * 8)), './burnout3'],
                       cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.stdout.decode('utf-8', 'replace')


def measure(log, seconds):
    """-> dict or None when the run produced no usable report."""
    cars = TRACKTEST_RE.findall(log)
    summary = SUMMARY_RE.search(log)
    if not cars:
        return None
    scale = seconds / 60.0
    mph = [float(c[6]) for c in cars]
    spans = [float(c[4]) for c in cars]
    out = {
        'cars': len(cars),
        'mean_mph': sum(mph) / len(mph),
        'min_mph': min(mph),
        'min_pspan': min(spans),
        'falls': sum(int(c[1]) for c in cars),
        'rival_wall': None,
        'rival_wall_per60': None,
    }
    if summary:
        per_car = CRASH_RE.findall(summary.group(0))
        # slot 0 is the player; rivals are 1..n
        wall = sum(int(c[4]) for c in per_car if c[0] != '0')
        rivals = max(1, len([c for c in per_car if c[0] != '0']))
        out['rival_wall'] = wall
        out['rival_wall_per60'] = wall / rivals / max(1e-6, scale)
    return out


def grade(m, seconds):
    """-> list of failure strings."""
    bad = []
    scale = seconds / 60.0
    if m['falls']:
        bad.append('%d fall(s) through the world' % m['falls'])
    if m['mean_mph'] < MIN_MEAN_MPH:
        bad.append('mean max_mph %.1f < %.1f' % (m['mean_mph'], MIN_MEAN_MPH))
    if m['min_pspan'] < MIN_PSPAN_PER_60S * scale:
        bad.append('min p_span %.3f < %.3f'
                   % (m['min_pspan'], MIN_PSPAN_PER_60S * scale))
    if (m['rival_wall_per60'] is not None
            and m['rival_wall_per60'] > MAX_RIVAL_WALL_PER_60S):
        bad.append('rival wall wrecks %.2f/60s > %.2f'
                   % (m['rival_wall_per60'], MAX_RIVAL_WALL_PER_60S))
    return bad


def tracks_with_route():
    base = os.path.join(ROOT, 'build', 'tracks')
    return sorted(d for d in os.listdir(base)
                  if os.path.exists(os.path.join(base, d, 'route.bin')))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seconds', type=int, default=90)
    ap.add_argument('--tracks', help='comma-separated ids (default: all)')
    ap.add_argument('--ab', action='store_true',
                    help='run each track with B3_NAV_CLAMP off AND on')
    ap.add_argument('--lock-dir', default=DEFAULT_LOCK)
    ap.add_argument('--jobs', type=int, default=1)
    ap.add_argument('--force', action='store_true',
                    help='skip the concurrent-run check (dangerous)')
    args = ap.parse_args()

    if not args.force:
        other = busy()
        if other:
            print('REFUSING: another run is live:\n  %s' % other)
            return 2
    if not os.path.exists(os.path.join(ROOT, 'burnout3')):
        print('REFUSING: ./burnout3 not built')
        return 2

    tracks = (args.tracks.split(',') if args.tracks else tracks_with_route())
    fails = 0
    rows = []
    with Lock(args.lock_dir):
        # every (track, variant) run is an independent game process; with
        # --jobs they all launch through one pool and the report below
        # just consumes the futures in order
        pool = None
        futs = {}
        if args.jobs > 1:
            import concurrent.futures
            pool = concurrent.futures.ThreadPoolExecutor(
                max_workers=args.jobs)
            for t in tracks:
                vs = [(False, 'off'), (True, 'on')] if args.ab \
                    else [(None, '-')]
                for clamp, label in vs:
                    futs[(t, label)] = pool.submit(
                        run_track, t, args.seconds, clamp)
        for t in tracks:
            variants = [(False, 'off'), (True, 'on')] if args.ab \
                else [(None, '-')]
            got = {}
            for clamp, label in variants:
                log = (futs[(t, label)].result() if pool
                       else run_track(t, args.seconds, clamp))
                m = measure(log, args.seconds)
                got[label] = m
                if m is None:
                    print('%-10s %-4s NO REPORT (crash or timeout)'
                          % (t, label))
                    fails += 1
                    continue
                bad = grade(m, args.seconds)
                status = 'ok  ' if not bad else 'FAIL'
                if bad and not args.ab:
                    fails += 1
                print('%-10s %-4s %s cars=%d mean_mph=%5.1f min_pspan=%.3f '
                      'rival_wall=%s (%.2f/60s) %s'
                      % (t, label, status, m['cars'], m['mean_mph'],
                         m['min_pspan'],
                         m['rival_wall'] if m['rival_wall'] is not None
                         else '?',
                         m['rival_wall_per60'] or 0.0,
                         '; '.join(bad)))
            if args.ab and got.get('off') and got.get('on'):
                a, b = got['off'], got['on']
                dw = (b['rival_wall'] or 0) - (a['rival_wall'] or 0)
                dp = b['min_pspan'] - a['min_pspan']
                verdict = 'BETTER' if (dw < 0 and dp >= -0.01) else (
                    'WORSE' if (dw > 0 or dp < -0.02) else 'neutral')
                rows.append((t, dw, dp, verdict))
                print('%-10s  ->  d_wall=%+d  d_pspan=%+.3f  %s'
                      % (t, dw, dp, verdict))

    if args.ab and rows:
        better = sum(1 for r in rows if r[3] == 'BETTER')
        worse = sum(1 for r in rows if r[3] == 'WORSE')
        print('\nA/B over %d tracks: %d better, %d worse, %d neutral'
              % (len(rows), better, worse, len(rows) - better - worse))
        print('total d_wall = %+d' % sum(r[1] for r in rows))
        return 1 if worse > better else 0

    print('\n%d/%d tracks pass the rival driving-quality gate'
          % (len(tracks) - fails, len(tracks)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
