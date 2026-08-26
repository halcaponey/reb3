#!/bin/sh
# Extract every track the game ships into build/tracks/<id>/ -- the full
# per-track pipeline (mesh, routes, collision, traffic, grid, props,
# textures, envmap, light probes, nav edges).
#
# The per-track stages run through the C extractor (tools/cextract), which
# is byte-identical to the archived Python pipeline (see
# tools/py_extract_archive/README.md and the gate
# tools/cextract/verify_cextract.py).  The one remaining Python call is
# extract_traffic.py for its VEHICLE-ASSET half (build/cars/*) -- its
# track-side traffic.bin is identical to cxtract's, so the overwrite is a
# no-op by construction.
#
# Track ids come from the extracted directory list.  Safe to re-run.
cd "$(dirname "$0")/.." || exit 1

CX=build/cxtract
if [ ! -x "$CX" ] || [ tools/cextract/cx_main.c -nt "$CX" ]; then
    echo "== building cxtract"
    bash tools/cextract/build.sh "$CX" || exit 1
fi

for d in build/tracks/*/; do
    t=$(basename "$d")
    [ -f "build/tracks/$t/route.bin" ] && [ -f "build/tracks/$t/collision.bin" ] \
        && [ -f "build/tracks/$t/traffic.bin" ] && [ -f "build/tracks/$t/grid.bin" ] \
        && [ -f "build/tracks/$t/track.obj" ] && [ -d "build/tracks/$t/textures" ] \
        && [ -f "build/tracks/$t/nav_edges.bin" ] \
        && { echo "== $t: complete, skipping"; continue; }
    echo "== $t"
    "$CX" --track "$t" --out "build/tracks/$t" \
        || { echo "   cxtract FAILED"; continue; }
    # vehicle assets only; the track-side outputs are already cxtract's
    python3 tools/extract_traffic.py --track "$t" >/dev/null 2>&1 \
        || echo "   traffic vehicle assets FAILED"
done
echo "-- done; complete tracks:"
for d in build/tracks/*/; do
    t=$(basename "$d")
    [ -f "build/tracks/$t/route.bin" ] && [ -f "build/tracks/$t/track.obj" ] && echo "   $t"
done
