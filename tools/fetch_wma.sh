#!/bin/sh
# tools/fetch_wma.sh -- fetch the vendored WMA decoder into third_party/.
#
# ============================================================ WHY THIS EXISTS
# Every audio payload on the Burnout 3 disc is WMA.  All 885 of them, in all 33
# wave banks -- the 44 EA TRAX songs, the crash-FM DJ banter, the FMV beds.
# Measured from the entries' own ASF headers (WAVEFORMATEX in the Stream
# Properties object), not from the XWB format dword, which only says "tag 2 =
# WMA" and stops there:
#
#     wFormatTag 0x0161 = WMAv2, 2 ch, 16-bit, 160 kb/s CBR, in every entry
#       * 44100 Hz -- the 44 EA TRAX + the 34 Movie entries   (78 payloads)
#       * 48000 Hz -- every DJ / crash-FM / per-track bank    (807 payloads)
#
# WMAv2 STANDARD, never Pro, never Lossless, never Voice.  That is the whole
# reason this is a 250 KB fixed-point library and not a libavcodec build:
# Rockbox's libwma decodes exactly WMAv1/v2 standard and nothing else, which is
# exactly the set this disc contains.
#
# It also has to run in a browser.  WebCodecs has no WMA decoder in any
# shipping engine -- the codec registry is a closed list and 'wmav2' is not on
# it -- so the web port cannot delegate this to the host the way it can for
# AAC or Opus.  Fixed-point C that compiles to wasm with no toolchain of its
# own is the only shape that works on every target at once.
#
# ================================================================== LICENSING
# Rockbox's libwma is a fixed-point derivative of FFmpeg's wmadec and carries
# FFmpeg's LGPL 2.1-or-later, as do the MDCT/FFT/bitstream helpers taken with
# it.  Each file keeps its own header; nothing is relicensed and nothing is
# modified -- the adaptation lives entirely in tools/cextract/wma/, which is
# this project's own code.  LGPL compliance for a source-distributed project
# that links these files unmodified is satisfied by keeping them separable and
# attributed, which is precisely why they are FETCHED rather than copied into
# this repository's history.
#
# ==================================================================== USAGE
#     sh tools/fetch_wma.sh          # fetch into third_party/rockbox/
#     sh tools/fetch_wma.sh --check  # verify what is already there, fetch none
#
# third_party/ is gitignored, exactly as web/third_party/ and
# android/third_party/ are.  `make` degrades gracefully without it: the music
# stages compile to a stub that reports "no WMA decoder" and the game prints
# the same one-line notice it printed when the stages shelled out to ffmpeg.
set -e

# The pin.  Rockbox master, 2026-08-25.  Bump this deliberately and re-run
# with --check to see what moved.
REV=a8f8aa40b94f553f452af64ce2809058f2db24fd
BASE="https://raw.githubusercontent.com/Rockbox/rockbox/$REV/lib/rbcodec"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$HERE/third_party/rockbox"

# The file list, and it is deliberately EXPLICIT rather than a directory sweep.
# A sweep would silently pick up whatever Rockbox adds next; this list is the
# statement of what the decoder actually needs, and a fetch failure on any one
# of them is a hard error rather than a mystery link error later.
#
#   codecs/libwma  the decoder proper (wmadeci.c is ffmpeg's wmadec, fixed)
#   codecs/lib     its MDCT/FFT, its bitstream reader, its VLC tables
#   codecs/libasf  the WAVEFORMATEX struct wma_decode_init() takes.  ONLY the
#                  header: asf.c is Rockbox's streaming packet reader, written
#                  against `ci->read_filebuf`, and our payloads are whole ASF
#                  files already in memory.  tools/cextract/wma/b3_asf.c is
#                  this project's own in-memory demuxer instead.
FILES="
codecs/libwma/wmadeci.c
codecs/libwma/wmafixed.c
codecs/libwma/wmafixed.h
codecs/libwma/wmadec.h
codecs/libwma/wmadata.h
codecs/libwma/types.h
codecs/libasf/asf.h
codecs/lib/ffmpeg_bitstream.c
codecs/lib/ffmpeg_get_bits.h
codecs/lib/ffmpeg_put_bits.h
codecs/lib/ffmpeg_bswap.h
codecs/lib/ffmpeg_intreadwrite.h
codecs/lib/fft-ffmpeg.c
codecs/lib/fft-ffmpeg_arm.h
codecs/lib/fft-ffmpeg_cf.h
codecs/lib/fft.h
codecs/lib/mdct.c
codecs/lib/mdct.h
codecs/lib/mdct_lookup.c
codecs/lib/mdct_lookup.h
codecs/lib/codeclib_misc.h
codecs/lib/asm_arm.h
codecs/lib/asm_mcf5249.h
"

# sha256 of each file AT $REV.  Written by --record; checked on every fetch.
# This is the integrity gate: a raw.githubusercontent fetch of a pinned commit
# is already content-addressed by git, but nothing in `curl | sh` checks that,
# and a corrupted or truncated download of a fixed-point DSP file fails as bad
# AUDIO rather than as a build error.
MANIFEST="$HERE/tools/fetch_wma.sha256"

mode=fetch
case "$1" in
    --check)  mode=check ;;
    --record) mode=record ;;
    "")       ;;
    *) echo "usage: $0 [--check|--record]" >&2; exit 2 ;;
esac

if [ "$mode" = check ]; then
    if [ ! -d "$DEST" ]; then
        echo "third_party/rockbox: ABSENT  (sh tools/fetch_wma.sh)"
        exit 1
    fi
    (cd "$DEST" && sha256sum -c "$MANIFEST") || exit 1
    echo "third_party/rockbox: OK, rockbox $REV"
    exit 0
fi

echo "== fetching Rockbox libwma @ $REV"
echo "   -> $DEST"
for f in $FILES; do
    d="$DEST/$(dirname "$f")"
    mkdir -p "$d"
    if ! curl -fsSL "$BASE/$f" -o "$DEST/$f"; then
        echo "FAILED: $f" >&2
        exit 1
    fi
    printf '   %s\n' "$f"
done

if [ "$mode" = record ]; then
    (cd "$DEST" && sha256sum $FILES) > "$MANIFEST"
    echo "recorded $MANIFEST"
else
    if [ -f "$MANIFEST" ]; then
        (cd "$DEST" && sha256sum -c "$MANIFEST" >/dev/null) || {
            echo "CHECKSUM MISMATCH -- the pin moved or a download is corrupt" >&2
            exit 1
        }
        echo "== checksums OK"
    fi
fi
echo "== done.  now: make"
