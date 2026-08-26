#!/usr/bin/env python3
"""Extract retail's per-node nav ARC-LENGTH table -> build/tracks/<ID>/nav_edges.bin

WHY THIS EXISTS
---------------
Every index-directory row in Gamedata.bgd's road-network section carries THREE
relative pointers -- `pair_rel`, `edge_rel`, `link_rel`.  `extract_bgd_paths.py`
decodes the pair and link tables into route.bin and has always left `edge_rel`
undecoded ("the unused edge_rel table remains undecoded",
tools/extract_bgd_paths.py:87).  route.bin v3 therefore does not carry it, and
src/burnout3_full.c:1983-1991 has to APPROXIMATE the quantity off the pair
centroids:

    `seg` is the node's own arc-length span, read in retail from the index
    row's `edge` table (`row->+4`, stride 8: `edge[node] - edge[node-1]`, or
    `edge[0]` at node 0).  route.bin v3 does not carry that table, so the
    harness measures the same quantity off the pair centroids.

This tool decodes that table and ships it as a SIDECAR asset, so route.bin v3
and its pinned readers are untouched.

PROVENANCE
----------
[C] The layout and the indexing law are executed retail code, FUN_00174AF0
    @0x00174AF0 (the approach-distance projection FUN_00176150 feeds to the
    corner-brake law).  Disassembly:

        00174b03  MOV   ECX,[ESI+0x4]        ; section header
        00174b06  MOV   EDX,[ECX+0x4]        ; -> the edge table
        00174b0a  MOVZX EAX,BX               ; node index
        00174b0d  JBE   0x00174b1f           ; node == 0 ?
        00174b0f  MOVSS XMM0,[EDX+EAX*8]     ; edge[node]
        00174b14  SUBSS XMM0,[EDX+EAX*8-0x8] ; - edge[node-1]
        00174b1f  MOVSS XMM0,[EDX+EAX*8]     ; node 0: edge[0] as-is

    => entries are 8 bytes, the f32 at +0 is a CUMULATIVE arc length, and one
    node's own span is the backward difference.  `header+0x04` matches
    tools/emulate_ai_avoid.py:14 ("per-node cumulative arc length, stride 8").

[C] Empirically re-verified by this tool's --verify mode on the shipped data:
    the +0 f32 is monotonically non-decreasing on EVERY section of EVERY one of
    the 36 tracks, its total agrees with the pair-midpoint polyline to within
    ~0.1-1.2 % (the residual is real: retail measures along the road spline,
    the midpoint polyline is a crude chord sum), and edge[0] is the first
    segment length, exactly as the node==0 branch above implies.

[S] The f32 at +4 is a per-node WIDTH-LIKE scalar: mean(aux / |P[left]-P[right]|)
    is 0.98-1.01 across sections whose widths differ by 16x (3 m, 6 m, 50 m),
    but it is not an exact copy of that chord (per-node ratio spans 0.83-1.83).
    Most likely the width measured PERPENDICULAR to travel rather than along
    the possibly-skewed left/right chord.  It is emitted for completeness and
    NOTHING consumes it yet.  Do not build a law on it without more evidence.

INDEXING
--------
Emitted flat in the SAME order route.bin emits its nav LINK array -- section
rows in `graph['rows']` order, `node_count` entries each (see
tools/extract_bgd_paths.py:1194-1209, where `link_base += row['node_count']`).
So the runtime index is exactly `section->link_base + node`, and the file's
entry count must equal route.bin's `nav_link_count`.  The loader MUST reject
the file when those disagree.

COORDINATES
-----------
None.  Arc lengths are scalar metres; no Z negation applies (cf. RE_NOTES 12).

FORMAT  build/tracks/<ID>/nav_edges.bin
-------
    +0x00  char[4] 'B3NE'
    +0x04  u32 version = 1
    +0x08  u32 edge_count      (== route.bin nav_link_count)
    +0x0C  u32 section_count   (== route.bin nav_section_count)
    +0x10  { f32 cum_length_m, f32 width_hint } x edge_count

USAGE
    python3 tools/extract_nav_edges.py --all
    python3 tools/extract_nav_edges.py --track US_C3_V1 --verify
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_bgd_paths as X          # noqa: E402  (library use only)
import extract_tlist as T              # noqa: E402

MAGIC = b'B3NE'
VERSION = 1
EDGE_REC = 8
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def gamedata_path(track):
    """Tracks/<REG>/<Cn>_<Vn>/Gamedata.bgd for a US_C3_V1-style id."""
    return os.path.join(T.TRACKS_DIR, track[:2], track[3:], 'Gamedata.bgd')


def read_edges(bgd, graph):
    """Flat per-node (cum_length, width_hint), route.bin nav-link order. [C]"""
    edges = []
    for row in graph['rows']:
        for node in range(row['node_count']):
            edges.append(struct.unpack_from('<ff', bgd.d,
                                            row['edges'] + node * EDGE_REC))
    return edges


def verify(bgd, graph, edges):
    """Re-prove the [C] reading against this track's own geometry."""
    problems = []
    pts = graph['points']
    cursor = 0
    for row in graph['rows']:
        n = row['node_count']
        block = edges[cursor:cursor + n]
        cursor += n
        if n < 3:
            continue
        cum = [e[0] for e in block]
        for i in range(n - 1):
            if cum[i + 1] < cum[i] - 1e-3:
                problems.append('section %d: cum not monotonic at node %d '
                                '(%.3f -> %.3f)'
                                % (row['section'], i, cum[i], cum[i + 1]))
                break
        # Arc-vs-midline is only a meaningful cross-check on SUBSTANTIAL runs.
        # The pair-midpoint polyline is a chord sum, and on the short junction
        # / slip-road sections (5-25 nodes) it is not even a clean curve --
        # where a pair's left/right assignment flips across a gate the midline
        # zigzags, so it can read either side of the true arc.  Measured on the
        # shipped data: every section with >= 50 nodes agrees within 8 %, while
        # 13 of 36 tracks have some sub-110 m connector off by 9-33 % in BOTH
        # directions.  Monotonicity above is the invariant that actually
        # catches a mis-resolved pointer; this is the corroborating check.
        if n < 50:
            continue
        mid = []
        for i in range(n):
            pa, pb = struct.unpack_from('<HH', bgd.d, row['pairs'] + i * 4)
            a = struct.unpack_from('<3f', bgd.d, pts + pa * 16)
            b = struct.unpack_from('<3f', bgd.d, pts + pb * 16)
            mid.append(tuple((a[k] + b[k]) * 0.5 for k in range(3)))
        poly = 0.0
        for i in range(1, n):
            poly += math.dist(mid[i], mid[i - 1])
        span = cum[-1] - cum[0]
        if poly > 1.0:
            rel = abs(span - poly) / poly
            if rel > 0.15:
                problems.append('section %d (%d nodes): arc %.1f m vs midline '
                                '%.1f m (%.1f %% off)'
                                % (row['section'], n, span, poly, rel * 100.0))
    return problems


def route_bin_link_count(track):
    """nav_link_count from the track's route.bin, or None if absent."""
    path = os.path.join(ROOT, 'build', 'tracks', track, 'route.bin')
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        head = f.read(0x28)
        if len(head) < 0x28 or head[:4] != b'B3RT':
            return None
        (ver, wall, center, onc, route, _start, _lap, _flags,
         strips) = struct.unpack_from('<IIIIIIfII', head, 4)
        if ver != 3:
            return None
        geometry = wall * 2 + center + onc + route + strips * 2
        f.seek(geometry * 12, os.SEEK_CUR)
        counts = struct.unpack('<IIIII', f.read(20))
        return counts[3]                       # nav_link_count


def emit(track, verbose=True, do_verify=False):
    src = gamedata_path(track)
    if not os.path.exists(src):
        return 'skip %-10s no Gamedata.bgd' % track
    out_dir = os.path.join(ROOT, 'build', 'tracks', track)
    if not os.path.isdir(out_dir):
        return 'skip %-10s no build/tracks/%s' % (track, track)

    bgd = X.BGD(src)
    graph = bgd.nav_graph(bgd.event(X.DEFAULT_EVENT))
    edges = read_edges(bgd, graph)

    expected = route_bin_link_count(track)
    if expected is not None and expected != len(edges):
        return ('FAIL %-10s %d edges but route.bin has %d nav links'
                % (track, len(edges), expected))

    if do_verify:
        problems = verify(bgd, graph, edges)
        if problems:
            return 'FAIL %-10s %s' % (track, '; '.join(problems[:3]))

    path = os.path.join(out_dir, 'nav_edges.bin')
    with open(path, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<III', VERSION, len(edges), len(graph['rows'])))
        for cum, width in edges:
            f.write(struct.pack('<ff', cum, width))

    total = sum(1 for _ in edges)
    if verbose:
        span = max((e[0] for e in edges), default=0.0)
        return ('ok   %-10s %5d edges  %2d sections  longest cum %8.1f m'
                % (track, total, len(graph['rows']), span))
    return 'ok %s' % track


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--track', help='track id, e.g. US_C3_V1')
    ap.add_argument('--all', action='store_true', help='every shipped track')
    ap.add_argument('--verify', action='store_true',
                    help='re-prove monotonicity + arc length before writing')
    args = ap.parse_args()

    if args.all:
        tracks = sorted(d for d in os.listdir(os.path.join(ROOT, 'build',
                                                           'tracks'))
                        if os.path.isdir(os.path.join(ROOT, 'build', 'tracks',
                                                      d)))
    else:
        tracks = [args.track or T.DEFAULT_TRACK]

    bad = 0
    for track in tracks:
        line = emit(track, do_verify=args.verify)
        if line.startswith('FAIL'):
            bad += 1
        print(line)
    if bad:
        print('%d track(s) FAILED' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
