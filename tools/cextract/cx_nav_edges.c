/* cx_nav_edges.c -- <out_dir>/nav_edges.bin ('B3NE' v1).
 * Port of tools/extract_nav_edges.py (the spec).
 *
 * PROVENANCE
 * [C] The layout and the indexing law are executed retail code, FUN_00174AF0
 *     (the approach-distance projection FUN_00176150 feeds to the
 *     corner-brake law):
 *
 *        00174b03  MOV   ECX,[ESI+0x4]        ; section header
 *        00174b06  MOV   EDX,[ECX+0x4]        ; -> the edge table
 *        00174b0f  MOVSS XMM0,[EDX+EAX*8]     ; edge[node]
 *        00174b14  SUBSS XMM0,[EDX+EAX*8-0x8] ; - edge[node-1]
 *        00174b1f  MOVSS XMM0,[EDX+EAX*8]     ; node 0: edge[0] as-is
 *
 *     => entries are 8 bytes, the f32 at +0 is a CUMULATIVE arc length and a
 *     node's own span is the backward difference.
 * [S] The f32 at +4 is a per-node WIDTH-LIKE scalar.  It is emitted for
 *     completeness and NOTHING consumes it yet.
 *
 * INDEXING: flat, in the SAME order route.bin emits its nav LINK array --
 * index-directory rows in order, node_count entries each -- so the runtime
 * index is exactly `section->link_base + node`, and the file's entry count
 * MUST equal route.bin's nav_link_count.  This tool refuses to write when
 * they disagree, exactly as the python original does.
 *
 * COORDINATES: none.  Arc lengths are scalar metres, no Z negation.
 *
 * FORMAT
 *     +0x00 char[4] 'B3NE'   +0x04 u32 version = 1
 *     +0x08 u32 edge_count   +0x0C u32 section_count
 *     +0x10 { f32 cum_length_m, f32 width_hint } x edge_count
 */
#include "cx_extract.h"
#include "cx_common_a.h"

#include <stdlib.h>
#include <string.h>

/* nav_link_count from the track's route.bin, or -1 when it is absent */
static long route_bin_link_count(const char *out_dir)
{
    char *path = cxa_join(out_dir, "route.bin");
    unsigned char head[0x28];
    FILE *f;
    uint32_t ver, wall, center, onc, route, strips;
    long geometry;
    unsigned char counts[20];

    if (!path)
        return -1;
    f = fopen(path, "rb");
    free(path);
    if (!f)
        return -1;
    if (fread(head, 1, 0x28, f) != 0x28 || memcmp(head, "B3RT", 4)) {
        fclose(f);
        return -1;
    }
    ver    = cxa_u32(head, 0x04);
    wall   = cxa_u32(head, 0x08);
    center = cxa_u32(head, 0x0C);
    onc    = cxa_u32(head, 0x10);
    route  = cxa_u32(head, 0x14);
    strips = cxa_u32(head, 0x24);
    if (ver != 3) {
        fclose(f);
        return -1;
    }
    geometry = (long)wall * 2 + center + onc + route + (long)strips * 2;
    if (fseek(f, geometry * 12, SEEK_CUR) != 0
        || fread(counts, 1, 20, f) != 20) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return (long)cxa_u32(counts, 12);       /* nav_link_count */
}

int cx_extract_nav_edges(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir)
{
    cxa_bgd bgd;
    cxa_event *ev;
    cxa_graph graph;
    char *bgd_path, *out = NULL;
    const char *event = getenv("B3_EVENT");
    uint32_t s, node, total = 0;
    long expected;
    FILE *f;
    int rc = -1;

    (void)game_dir;
    (void)track_id;
    bgd_path = cxa_join(track_dir, "Gamedata.bgd");
    if (!bgd_path)
        return -1;
    if (cxa_bgd_open(&bgd, bgd_path)) {
        free(bgd_path);
        cxa_bgd_close(&bgd);
        return -1;
    }
    free(bgd_path);
    ev = cxa_bgd_event(&bgd, event && *event ? event : CXA_DEFAULT_EVENT);
    if (!ev || cxa_nav_graph(&bgd, ev, &graph)) {
        cxa_bgd_close(&bgd);
        return -1;
    }
    for (s = 0; s < graph.nrows; s++)
        total += graph.rows[s].node_count;

    expected = route_bin_link_count(out_dir);
    if (expected >= 0 && (uint32_t)expected != total)
        goto done;                      /* route.bin disagrees: refuse */

    out = cxa_join(out_dir, "nav_edges.bin");
    if (!out)
        goto done;
    f = fopen(out, "wb");
    if (!f)
        goto done;
    cxa_w_bytes(f, "B3NE", 4);
    cxa_w_u32(f, 1);
    cxa_w_u32(f, total);
    cxa_w_u32(f, graph.nrows);
    for (s = 0; s < graph.nrows; s++) {
        for (node = 0; node < graph.rows[s].node_count; node++) {
            size_t o = graph.rows[s].edges + (size_t)node * 8;
            cxa_w_f32(f, cxa_f32(bgd.d, o));        /* cumulative arc length */
            cxa_w_f32(f, cxa_f32(bgd.d, o + 4));    /* width hint [S]        */
        }
    }
    fclose(f);
    rc = 0;

done:
    free(out);
    cxa_graph_free(&graph);
    cxa_bgd_close(&bgd);
    return rc;
}
