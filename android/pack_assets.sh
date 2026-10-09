#!/usr/bin/env bash
# ===========================================================================
# pack_assets.sh -- build the Android asset payload from the SAME build/ tree
# the desktop harness reads.
#
#   ./android/pack_assets.sh [TRACK_ID]        (default: $B3_TRACK or US_C3_V1)
#
# Output: android/app/src/main/assets/burnout3_assets.zip, whose entries are
# rooted at "build/..." so that MainActivity's extractor + the native chdir()
# reproduce the desktop working directory exactly.  Re-run this after ANY
# extractor rerun (tools/extract_*.py) and rebuild the APK -- that is the
# whole refresh story; nothing is hand-copied.
#
# The full build/ tree is ~6.7 GiB (4.3 GiB of it audio, 722 MiB of music,
# plus gigabytes of debug_dump_*.bmp captures).  What goes in the APK is the
# subset the harness actually opens for ONE track; the knobs below widen it.
#
# Environment knobs:
#   B3_MUSIC_TRACKS=N   EA TRAX streams to bundle       (default 2, 0 = none)
#   B3_PACK_CRASH_AUDIO=1  add build/audio/rws_crash*   (default off, ~147 MiB)
#   B3_PACK_ALL_CARS=0  restrict build/cars to .obj/.hull/... only (see below)
#   B3_ZIP_LEVEL=N      deflate level                   (default 6)
# ===========================================================================
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
src="$root/build"
out="$here/app/src/main/assets/burnout3_assets.zip"

track="${1:-${B3_TRACK:-US_C3_V1}}"
music_n="${B3_MUSIC_TRACKS:-2}"
zip_level="${B3_ZIP_LEVEL:-6}"

[ -d "$src" ] || { echo "no build/ tree at $src -- run the extractors first" >&2; exit 1; }
tdir="$src/tracks/$track"
[ -d "$tdir" ] || { echo "no build/tracks/$track" >&2; exit 1; }

stage=$(mktemp -d -t b3assets.XXXXXX)
trap 'rm -rf "$stage"' EXIT
b="$stage/build"
mkdir -p "$b"

# Hard-link where we can (same filesystem): staging must not copy ~200 MiB.
link() {  # link <src-file-or-dir> <dest-path-under-build/>
    local s="$1" d="$b/$2"
    [ -e "$s" ] || { echo "  (skip, absent) $s"; return 0; }
    s=$(readlink -f "$s" 2>/dev/null || echo "$s")
    mkdir -p "$(dirname "$d")"
    rm -rf "$d"
    cp -al "$s" "$d" 2>/dev/null || { rm -rf "$d"; cp -aL "$s" "$d"; }
}

echo "== packing track $track =="

# -- 1. the active track's geometry, promoted to the top-level names the
#       harness hard-codes (build/track.obj, build/collision.bin,
#       build/textures/).  Taking them from tracks/<ID>/ rather than from the
#       loose top-level copies keeps the payload self-consistent even when
#       the checkout currently has a DIFFERENT track installed.
link "$tdir/track.obj"      track.obj
link "$tdir/track.mtl"      track.mtl
link "$tdir/collision.bin"  collision.bin
link "$tdir/textures"       textures
[ -e "$b/track.obj" ]     || link "$src/track.obj"     track.obj
[ -e "$b/track.mtl" ]     || link "$src/track.mtl"     track.mtl
[ -e "$b/collision.bin" ] || link "$src/collision.bin" collision.bin
[ -e "$b/textures" ]      || link "$src/textures"      textures

# -- 2. per-track data the code reads out of build/tracks/<ID>/ by name.
for f in envmap.png light_probes.bin props.bin route.bin grid.bin traffic.bin nav_edges.bin traffic_paths.bin pace.bin; do
    link "$tdir/$f" "tracks/$track/$f"
done

# -- 3. art banks
if [ "${B3_PACK_ALL_CARS:-0}" = "1" ]; then
    link "$src/cars"        cars
else
    # Asset diet: shared metadata + 8 roster slots + track traffic cars
    for f in roster.bin car_physics.bin carbvh.bin; do
        [ -e "$src/cars/$f" ] && link "$src/cars/$f" "cars/$f"
    done
    python3 -c "
import os, struct, sys

track, src = sys.argv[1], sys.argv[2]
roster_path = os.path.join(src, 'cars', 'roster.bin')
traffic_path = os.path.join(src, 'tracks', track, 'traffic.bin')

cars = set()
if os.path.exists(roster_path):
    with open(roster_path, 'rb') as f:
        f.seek(8)
        n = struct.unpack('<I', f.read(4))[0]
        f.seek(24)
        seen = 0
        for _ in range(n):
            rec = f.read(64)
            fn = rec[:16].split(b'\x00')[0].decode()
            cls = rec[16:24].split(b'\x00')[0].decode()
            kind = struct.unpack('<I', rec[40:44])[0]
            if kind == 0:
                base = fn.split('.')[0]
                cars.add(f'{cls}_{base}')
                seen += 1
                if seen >= 8:
                    break

player_env = os.environ.get('B3_PLAYER_CAR')
if player_env:
    cars.add(player_env)

if os.path.exists(traffic_path):
    with open(traffic_path, 'rb') as f:
        f.seek(8)
        ntraf = struct.unpack('<I', f.read(4))[0]
        f.seek(28)
        for _ in range(ntraf):
            rec = f.read(72)
            cls = rec[16:24].split(b'\x00')[0].decode()
            car = rec[24:40].split(b'\x00')[0].decode()
            cars.add(f'{cls}_{car}')

for c in sorted(cars):
    print(c)
" "$track" "$src" | while read -r car_prefix; do
        [ -n "$car_prefix" ] || continue
        for cf in "$src/cars/${car_prefix}"*; do
            [ -e "$cf" ] && link "$cf" "cars/$(basename "$cf")"
        done
        if [ -d "$src/cars/parts/${car_prefix}" ]; then
            link "$src/cars/parts/${car_prefix}" "cars/parts/${car_prefix}"
        fi
    done
fi
link "$src/frontend"    frontend
link "$src/carfx"       carfx
link "$src/particlefx"  particlefx
link "$src/boostfx"     boostfx
for f in "$src"/postfx/"$track"_*; do link "$f" "postfx/$(basename "$f")"; done

# -- 4. loose files
link "$src/Globalus.bin" Globalus.bin
link "$src/mixer.cfg"    mixer.cfg

# -- 5. audio.  b3_sfx_init() reads build/audio/<bank>/<file>.wav for exactly
#       four banks; load_real_audio() reads the player car's four rpm-labelled
#       engine loops out of awd_pveh_<CLASS>_<CarN>_high/.  Everything else
#       under build/audio (4.3 GiB: per-track ambience, EATrax0/1, Movie,
#       DJ*, the *_low variants, the .wma twins) is left out.
for bank in awd_crashmod awd_single awd_generic "awd_$track"; do
    link "$src/audio/$bank" "audio/$bank"
done
for d in "$src"/audio/awd_pveh_*_high; do
    [ -d "$d" ] || continue
    n=$(basename "$d")
    for w in "$d"/eng_*.wav; do
        [ -e "$w" ] && link "$w" "audio/$n/$(basename "$w")"
    done
done
if [ "${B3_PACK_CRASH_AUDIO:-0}" = "1" ]; then
    for d in "$src"/audio/rws_crash*; do
        [ -d "$d" ] && link "$d" "audio/$(basename "$d")"
    done
fi

# -- 6. music: b3_music_init() scans build/music/track_%02d.wav upward and
#       stops at the first gap, so a prefix of the 44 EA TRAX streams works.
if [ "$music_n" -gt 0 ]; then
    i=0
    while [ "$i" -lt "$music_n" ]; do
        f=$(printf "%s/music/track_%02d.wav" "$src" "$i")
        [ -e "$f" ] || break
        link "$f" "music/$(basename "$f")"
        i=$((i + 1))
    done
    link "$src/music/eatrax.txt" music/eatrax.txt
fi

# -- 7. stamp.  MainActivity compares the stamp inside the APK with the one
#       on disk and re-extracts when they differ -- this is what makes a
#       repack after an extractor rerun actually land on the device.
{
    echo "track=$track"
    echo "packed=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host=$(hostname)"
    ( cd "$stage" && find build -type f -printf '%s %p\n' | sort | sha256sum )
} > "$b/ASSET_STAMP"

mkdir -p "$(dirname "$out")"
rm -f "$out"
if command -v zip >/dev/null 2>&1; then
    ( cd "$stage" && zip -qr -"$zip_level" -X "$out" build )
else
    python3 -c "
import os, sys, zipfile

out_path, stage_dir, comp_level = sys.argv[1], sys.argv[2], int(sys.argv[3])
with zipfile.ZipFile(out_path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=comp_level) as zf:
    for root, dirs, files in os.walk(os.path.join(stage_dir, 'build'), followlinks=True):
        for file in files:
            full = os.path.join(root, file)
            if os.path.isfile(full):
                rel = os.path.relpath(full, stage_dir)
                zf.write(full, rel)
" "$out" "$stage" "$zip_level"
fi

echo
echo "staged (uncompressed): $(du -sh "$stage" | cut -f1)"
echo "wrote $out ($(du -h "$out" | cut -f1))"
( cd "$stage" && du -sh build/* | sort -h | tail -12 )
