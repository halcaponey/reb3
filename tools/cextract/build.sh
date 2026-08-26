#!/bin/bash
# Build `cxtract`, the C asset-extraction pipeline.
#
#   tools/cextract/build.sh [output-path]
#
# One gcc line over tools/cextract/*.c.  The glob is deliberate: the agents
# land their modules independently, and cx_main.c's registries are driven from
# cx_extract.h -- the PER-TRACK table by the CX_HAVE_* macros, the GLOBAL table
# by the per-agent CX_GLOBAL_STAGES_* lists -- so whatever subset of .c files
# is present links and runs.  A family that has not landed contributes neither
# a .c file nor a list entry, and the build is unaffected.
#
# NEVER builds into the repo root and NEVER touches the game's Makefile: the
# default output is the session scratchpad.  Pass an explicit path for
# anywhere else.
#
#   <out> --track <id>   --out <dir>   per-track stages
#   <out> --all-global   --out <dir>   car / art / audio / generator stages
#   <out> --list | --list-global       what is registered
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEFAULT_OUT="${TMPDIR:-/tmp}/cxtract"
OUT="${1:-$DEFAULT_OUT}"

# THE WMA DECODER.  cx_audio_xwb.c and cx_audio_eatrax.c decode WMA in process
# now (they used to fork ffmpeg), so the link needs either the vendored
# fixed-point decoder or the stub that reports its absence.  third_party/ is
# gitignored and fetched by tools/fetch_wma.sh, exactly as web/third_party/ is;
# a checkout that has not run it still builds, and says so at run time.
#
# The vendored sources are compiled WITHOUT -Wall -Wextra.  That is not
# laziness: they are LGPL code we do not patch, and FFmpeg's deliberate switch
# fallthroughs in wmadeci.c would otherwise put ~40 warnings in every build and
# train everyone to ignore the output.  OUR code in tools/cextract/wma/ is held
# to the same -Wall -Wextra as the rest of the tree.
RB="$ROOT/third_party/rockbox"
WMA_INC="-I$HERE/wma"
if [ -f "$RB/codecs/libwma/wmadeci.c" ]; then
    WMA_SRC="$HERE/wma/b3_wma.c"
    WMA_INC="$WMA_INC -I$HERE/wma/codecs/lib -I$RB -I$RB/codecs -I$RB/codecs/lib -I$RB/codecs/libwma"
    RB_SRC="$RB/codecs/libwma/wmadeci.c $RB/codecs/libwma/wmafixed.c \
            $RB/codecs/lib/mdct.c $RB/codecs/lib/mdct_lookup.c \
            $RB/codecs/lib/fft-ffmpeg.c $RB/codecs/lib/ffmpeg_bitstream.c"
else
    echo "note: third_party/rockbox absent -- linking the WMA stub."
    echo "      run 'sh tools/fetch_wma.sh' for music extraction."
    WMA_SRC="$HERE/wma/b3_wma_stub.c"
    RB_SRC=""
fi

mkdir -p "$(dirname "$OUT")"
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT
RB_OBJ=""
for f in $RB_SRC; do
    o="$TMPD/$(basename "$f" .c).o"
    gcc -std=c11 -O2 -w -pthread -DROCKBOX $WMA_INC -c "$f" -o "$o"
    RB_OBJ="$RB_OBJ $o"
done
# -pthread: cx_pool.c walks the per-item fleets of the slow stages on worker
# threads (see tools/cextract/cx_pool.h).  B3_JOBS=1 makes every one of them
# serial again without rebuilding.
gcc -Wall -Wextra -std=c11 -O2 -pthread $WMA_INC -DROCKBOX \
    "$HERE"/*.c "$WMA_SRC" $RB_OBJ -o "$OUT" -lz -lm
echo "built $OUT"
