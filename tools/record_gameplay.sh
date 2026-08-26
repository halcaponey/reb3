#!/usr/bin/env bash
# ===========================================================================
# record_gameplay.sh -- play the harness at 4K and record it with sound.
#
#   ./tools/record_gameplay.sh [outfile.mp4] [seconds]
#
# Records a pixel-perfect 3840x2160 capture (no scaling) plus the game's own
# audio, using NVENC on the RTX 3090 so the encode costs almost no CPU and
# does not slow the game down.
#
# Why it is built this way on this machine:
#   * the desktop is 5120x2880, so a 3840x2160 game window fits WHOLE -- the
#     capture is a straight pixel copy, never a rescale;
#   * the session is Wayland, and ffmpeg's x11grab only sees XWayland, so the
#     game is forced onto the X11 backend (SDL_VIDEODRIVER=x11).  It still
#     renders through the same GL path;
#   * audio is taken from the default sink's MONITOR, which is the mix the
#     game is actually playing -- not a microphone.
#
# Add a mic (commentary) with:  MIC=1 ./tools/record_gameplay.sh
# List the audio devices with:  ffmpeg -sources pulse
#
# Knobs: W H FPS CODEC CQ GAIN MSAA MIC MONITOR MICSRC  (all env vars)
# ===========================================================================
set -euo pipefail

OUT=${1:-gameplay_4k.mp4}
DUR=${2:-0}                       # 0 = record until you quit the game
W=${W:-3840}
H=${H:-2160}
FPS=${FPS:-60}
# the monitor of the default sink = what you hear
MONITOR=${MONITOR:-alsa_output.pci-0000_00_1b.0.analog-stereo.monitor}
MICSRC=${MICSRC:-alsa_input.usb-BLUE_MICROPHONE_Blue_Snowball_201805-00.mono-fallback}
CODEC=${CODEC:-hevc_nvenc}        # hevc_nvenc | h264_nvenc | av1_nvenc
CQ=${CQ:-19}                      # lower = better quality, larger file
GAIN=${GAIN:-0}                   # dB of gain on the game audio

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$here"

[ -x ./burnout3 ] || { echo "build first: make -j4" >&2; exit 1; }

# If that monitor is gone (different sink, rebooted into HDMI audio, ...) fall
# back to whatever monitor PipeWire is offering rather than recording silence.
SRCS=$(ffmpeg -hide_banner -sources pulse 2>/dev/null || true)
if ! echo "$SRCS" | grep -qF "$MONITOR"; then
    ALT=$(echo "$SRCS" | grep -oE '[^ ]+\.monitor' | head -1) || true
    if [ -n "${ALT:-}" ]; then
        echo "!! $MONITOR is gone; using $ALT"
        MONITOR=$ALT
    else
        echo "!! no monitor source found -- the video will have no game audio"
    fi
fi

# The game already antialiases itself (B3_MSAA, default 4).  A 3090 has the
# headroom for 8x at 4K, and edge quality is what a recording shows off, so
# push it up here -- this is the cheap way to get a glossier capture, far
# cheaper than rendering above 4K and downscaling.
MSAA=${MSAA:-8}

echo "== launching the game at ${W}x${H} (XWayland) =="
# B3_DRIVE_LOG is deliberately NOT set: it writes ~22 MB per minute.
SDL_VIDEODRIVER=x11 B3_RES=${W}x${H} B3_MSAA=$MSAA ./burnout3 &
GAME=$!
trap 'kill $GAME 2>/dev/null || true' EXIT

# Wait for the window to map, then read its real geometry.
#
# xwininfo -root -tree lines look like
#     0x160000a "Burnout 3: Takedown": ("burnout3" "burnout3")  1280x720+50+124  +1920+1154
#                                       ^ WM class              ^ size+rel        ^ ABSOLUTE
# Two traps here, both of which silently record the WRONG rectangle:
#   * mutter reparents the window inside a "mutter-x11-frames" decoration frame
#     carrying the SAME TITLE at a larger size (+100 wide, +174 tall), so a
#     title match grabs the title bar and borders as well;
#   * a loose title match also hits unrelated windows -- a Ghidra CodeBrowser
#     on burnout3.elf has "burnout3" right there in its title, and this script
#     happily recorded 20 s of Ghidra before the match was tightened.
# Keying on the WM CLASS avoids both.  Deliberately no looser fallback:
# capturing someone else's window is worse than waiting another half second.
for _ in $(seq 1 60); do
    sleep 0.5
    TREE=$(DISPLAY=:0 xwininfo -root -tree 2>/dev/null \
           | grep -aE '\("burnout3" "burnout3"\)') || true
    [ -n "${TREE:-}" ] || continue
    # prefer the child whose size is exactly what we asked the game for
    # NB `|| true`: with `set -e -o pipefail` a non-matching grep inside a
    # command substitution aborts the whole script.
    LINE=$(echo "$TREE" | grep -a "  ${W}x${H}+" | head -1) || true
    [ -n "$LINE" ] || LINE=$(echo "$TREE" | head -1)
    [ -n "${LINE:-}" ] || continue
    SIZE=$(echo "$LINE" | awk '{print $(NF-1)}')      # WxH+rx+ry
    ABS=$(echo "$LINE"  | awk '{print $NF}')          # +X+Y
    CW=${SIZE%%x*}; R=${SIZE#*x}; CH=${R%%+*}
    X=$(echo "$ABS" | cut -d+ -f2)
    Y=$(echo "$ABS" | cut -d+ -f3)
    break
done

if [ -z "${X:-}" ]; then
    echo "!! could not find the game window; capturing the top-left ${W}x${H}"
    X=0; Y=0; CW=$W; CH=$H
fi
# h264/hevc want even dimensions
CW=$((CW - CW % 2)); CH=$((CH - CH % 2))
echo "== capturing ${CW}x${CH} at +${X},+${Y} -> $OUT (${CODEC}, cq ${CQ}) =="

DURARG=()
[ "$DUR" != "0" ] && DURARG=(-t "$DUR")

# The monitor carries the mix at the CURRENT desktop volume, so a low system
# slider records a quiet file.  GAIN=<dB> compensates without touching the game.
AUDIN=(-f pulse -ac 2 -i "$MONITOR")
AMAP=(-map 1:a -filter:a "volume=${GAIN}dB" -c:a aac -b:a 192k)
if [ "${MIC:-0}" != "0" ]; then
    AUDIN+=(-f pulse -ac 1 -i "$MICSRC")
    # game + mic mixed into one stereo track
    AMAP=(-filter_complex
          "[1:a]volume=${GAIN}dB[g];[g][2:a]amix=inputs=2:duration=first[a]"
          -map "[a]" -c:a aac -b:a 192k)
fi

# ffmpeg reads a fifo on stdin so that quitting the GAME can send it a clean
# "q" -- an mp4 killed mid-write has no moov atom and will not play.  Just
# close the game window when you are done; the file finalises itself.
FIFO=$(mktemp -u "${TMPDIR:-/tmp}/b3rec.XXXXXX")
mkfifo "$FIFO"
trap 'kill $GAME 2>/dev/null || true; rm -f "$FIFO"' EXIT

ffmpeg -hide_banner -y \
    -thread_queue_size 1024 -framerate "$FPS" \
    -f x11grab -draw_mouse 0 -video_size "${CW}x${CH}" -i ":0.0+${X},${Y}" \
    -thread_queue_size 1024 "${AUDIN[@]}" \
    "${DURARG[@]}" \
    -map 0:v -c:v "$CODEC" -preset p5 -tune hq -rc vbr -cq "$CQ" \
    -b:v 0 -pix_fmt yuv420p -g $((FPS * 2)) \
    "${AMAP[@]}" \
    -movflags +faststart "$OUT" < "$FIFO" &
FF=$!
exec 3>"$FIFO"            # hold the write end open so ffmpeg does not see EOF

# `|| true` throughout: a non-zero exit from either child must not skip the
# finalise step under `set -e`.
if [ "$DUR" != "0" ]; then
    wait "$FF" || true    # fixed length: ffmpeg stops itself, then close the game
    kill "$GAME" 2>/dev/null || true
else
    wait "$GAME" || true  # open-ended: play until you quit, then finalise
    echo "== game exited, finalising $OUT =="
    printf q >&3
    wait "$FF" || true
fi
exec 3>&-
ls -lh "$OUT"
