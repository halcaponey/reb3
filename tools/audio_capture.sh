#!/usr/bin/env bash
# audio_capture.sh -- capture the harness's OWN MIX to a file, offscreen and
# inaudible, next to a frame-by-frame drive log.
#
# THE USER MUST NEVER SEE OR HEAR ANYTHING, and both halves of that are the
# first two exports.  SDL's `disk` audio driver is a real SDL_audio backend
# that calls the engine's audio_callback exactly as a sound card would, paces
# itself at the buffer's real duration, and writes the bytes to a FILE instead
# of to a device -- so this captures the mix the player would hear without any
# device being opened at all.  Nothing here reaches the speakers.
#
#   tools/audio_capture.sh <out.raw> <drive_log.txt> [seconds]
#
# The raw file is 44100 Hz mono signed-16 little-endian: exactly what
# audio_device_open() asks SDL for, which is what makes it directly comparable
# to the web port's ring.
export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=disk

cd "$(dirname "$0")/.." || exit 1

OUT=${1:-/tmp/b3_mix.raw}
LOG=${2:-/tmp/b3_drive.txt}
SECS=${3:-25}

export SDL_DISKAUDIOFILE="$OUT"
export B3_DRIVE_LOG="$LOG"
# No disc path is compiled in.  $B3_ISO (or $B3_GAME_ROOT) names YOUR own
# copy; without either, the engine falls back to a pre-extracted build/ tree.
: "${B3_ISO:=${B3_GAME_ROOT:-}}"
[ -n "$B3_ISO" ] && export B3_ISO
export B3_AUTODRIVE=1
export B3_EXIT_AT="$SECS"

# NO B3_FIXED_DT HERE, and that is the point.  The disk driver's clock is WALL
# time (it paces itself at each buffer's real duration), so a fixed-dt run --
# which offscreen advances sim time ~3x faster than the wall -- puts the drive
# log on a different clock from the PCM and there is nothing in either file to
# reconcile them with.  Left alone, the sim's dt IS the frame's real dt, so
# `race_time` in the log is wall seconds and the two line up directly.
# tools/validate_engine_audio.py still recovers the one remaining unknown, the
# gap between the device opening and the race starting, from the two durations.

rm -f "$OUT" "$LOG"
timeout 900 ./burnout3
echo "---- captured $(stat -c %s "$OUT" 2>/dev/null || echo 0) bytes of PCM"
