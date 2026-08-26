/* cx_paths.c -- route.bin + traffic_paths.bin.
 *
 * Ports tools/extract_bgd_paths.py's write_route_bin() and the RIDX/pool
 * half of tools/extract_traffic.py (write_traffic_paths_bin + traffic_mix).
 * Those tools are the byte-level spec; the .bgd walk and the geometry
 * recovery live in cx_common_a.c.
 *
 * route.bin ('B3RT' v3), little-endian, all points in the harness's GL space
 * (z negated once, RE_NOTES 12) so a loader can memcpy them:
 *   +0x00 char[4] 'B3RT'   +0x04 u32 version=3   +0x08 u32 wall_count
 *   +0x0C u32 centerline_count  +0x10 u32 oncoming_count
 *   +0x14 u32 route_count  +0x18 u32 route_start  +0x1C f32 lap_length
 *   +0x20 u32 flags  bit0 = arrays reversed to match the race direction,
 *                    bit1 = the route is a drive line and the wall strands
 *                           are synthesised around it
 *   +0x24 u32 strip_pairs
 *   +0x28 wall_a, wall_b, centerline, oncoming, route, strip_a, strip_b
 *         then the nav graph (counts, points, sections, pairs, links, plans)
 *
 * traffic_paths.bin ('B3TP' v4): the RIDX point pool + per-path pair /
 * distance / branch rows, FUN_001A28B0's TDESC pool windows, and the retail
 * spawn policy's own tables (per-class model lists, (path,row) -> (manager
 * record, slot) bindings, per-(record,slot) speed + rate rows).          [C]
 */
#include "cx_extract.h"
#include "cx_common_a.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- route.bin */

static int write_route_bin(const char *path, cxa_analysis *a)
{
    cxa_bgd *b = a->bgd;
    cxa_graph graph;
    cxa_navplan *plans = NULL;
    uint32_t nplans = 0, s, node;
    uint32_t pair_total = 0, link_total = 0, section_total;
    const cxa_v3 *rl, *ol;
    int rl_n, ol_n, i;
    FILE *f;

    if (cxa_nav_graph(b, a->ev, &graph))
        return -1;
    if (cxa_nav_plans(b, a->ev, &plans, &nplans)) {
        cxa_graph_free(&graph);
        return -1;
    }
    rl   = a->raceline ? a->raceline->pts : NULL;
    rl_n = a->raceline ? a->raceline->count : 0;
    ol   = a->oncoming ? a->oncoming->pts : NULL;
    ol_n = a->oncoming ? a->oncoming->count : 0;
    section_total = graph.nrows;
    for (s = 0; s < graph.nrows; s++) {
        pair_total += graph.rows[s].node_count + 1;
        link_total += graph.rows[s].node_count;
    }

    f = fopen(path, "wb");
    if (!f) {
        cxa_graph_free(&graph);
        free(plans);
        return -1;
    }
    cxa_w_bytes(f, "B3RT", 4);
    cxa_w_u32(f, 3);
    cxa_w_u32(f, (uint32_t)a->wall_n);
    cxa_w_u32(f, (uint32_t)rl_n);
    cxa_w_u32(f, (uint32_t)ol_n);
    cxa_w_u32(f, (uint32_t)a->route_n);
    cxa_w_u32(f, (uint32_t)a->route_start);
    cxa_w_f32(f, a->net.lap);
    cxa_w_u32(f, (a->reversed_walls ? 1u : 0u) | (a->route_is_loop ? 2u : 0u));
    cxa_w_u32(f, (uint32_t)a->cor.pairs);
    for (i = 0; i < a->wall_n; i++) cxa_w_v3_gl(f, a->wall_a[i]);
    for (i = 0; i < a->wall_n; i++) cxa_w_v3_gl(f, a->wall_b[i]);
    for (i = 0; i < rl_n; i++)      cxa_w_v3_gl(f, rl[i]);
    for (i = 0; i < ol_n; i++)      cxa_w_v3_gl(f, ol[i]);
    for (i = 0; i < a->route_n; i++) cxa_w_v3_gl(f, a->route[i]);
    for (i = 0; i < a->cor.pairs; i++) cxa_w_v3_gl(f, a->cor.A[i]);
    for (i = 0; i < a->cor.pairs; i++) cxa_w_v3_gl(f, a->cor.B[i]);

    cxa_w_u32(f, graph.point_count);
    cxa_w_u32(f, section_total);
    cxa_w_u32(f, pair_total);
    cxa_w_u32(f, link_total);
    cxa_w_u32(f, nplans);
    for (s = 0; s < graph.point_count; s++)
        cxa_w_v3_gl(f, cxa_f3(b->d, graph.points + (size_t)s * 16));
    {
        uint32_t pair_base = 0, link_base = 0;
        for (s = 0; s < graph.nrows; s++) {
            cxa_w_u32(f, pair_base);
            cxa_w_u32(f, link_base);
            cxa_w_u16(f, graph.rows[s].node_count);
            cxa_w_u16(f, graph.rows[s].flags);
            pair_base += graph.rows[s].node_count + 1;
            link_base += graph.rows[s].node_count;
        }
    }
    for (s = 0; s < graph.nrows; s++) {
        /* each section includes one terminal look-ahead guard pair */
        for (node = 0; node <= graph.rows[s].node_count; node++) {
            size_t o = graph.rows[s].pairs + (size_t)node * 4;
            cxa_w_u16(f, cxa_u16(b->d, o));
            cxa_w_u16(f, cxa_u16(b->d, o + 2));
        }
    }
    for (s = 0; s < graph.nrows; s++) {
        /* row+link_rel -> node_count x 10-byte records: forward is
         * {u8 section@+4, u16 node@+6}, reverse {u8 section@+5, u16 node@+8}
         * (FUN_00175B10 walks these exact fields).                     [C] */
        for (node = 0; node < graph.rows[s].node_count; node++) {
            size_t o = graph.rows[s].links + (size_t)node * 10;
            cxa_w_u16(f, cxa_u16(b->d, o));          /* anchor    */
            cxa_w_u16(f, cxa_u16(b->d, o + 2));      /* link_data */
            cxa_w_u8(f, b->d[o + 4]);                /* forward_section */
            cxa_w_u8(f, b->d[o + 5]);                /* reverse_section */
            cxa_w_u16(f, cxa_u16(b->d, o + 6));      /* forward_node */
            cxa_w_u16(f, cxa_u16(b->d, o + 8));      /* reverse_node */
        }
    }
    for (s = 0; s < nplans; s++) {
        cxa_w_u16(f, plans[s].node_a);
        cxa_w_u16(f, plans[s].node_b);
        cxa_w_u16(f, plans[s].node_c);
        cxa_w_u16(f, plans[s].speed);
        cxa_w_u8(f, plans[s].section);
        cxa_w_u8(f, plans[s].byte9);
        cxa_w_u8(f, plans[s].flags);
        cxa_w_u8(f, plans[s].byte11);
    }
    fclose(f);
    cxa_graph_free(&graph);
    free(plans);
    return 0;
}

/* -------------------------------------------------------- traffic_paths.bin
 * The retail spawn policy's own tables, straight out of the TDESC. [C]
 *   * per-class MODEL lists -- FUN_001A5E30's jump table (@0x001A5F10) maps
 *     the class code onto the six TDESC list offsets {ptr, count, total};
 *     the model is drawn with `rng % total` against the running sum of each
 *     record's u32 weight at +0x10, its paint with `rng % 100` against the
 *     eight percentage bytes at +0x08..+0x0F (FUN_001A5F90).
 *   * (path,row) -> (manager record, slot) BINDINGS -- TDESC+0x3C/+0x40 rows
 *     inverted exactly as FUN_001A13F0 @0x001A1BF3 does.
 *   * per-(record,slot) SPEED and per-class RATE tables -- schedule row 0's
 *     stage-2 (4-byte mph) and stage-3 (0x1C-byte, seven floats) tables; row
 *     0 is the one whose trigger (+0x50) is 0.
 */

typedef struct { uint32_t path_id, record, slot, start_row; } cx_binding;

static int cmp_binding(const void *pa, const void *pb)
{
    const cx_binding *a = (const cx_binding *)pa, *b = (const cx_binding *)pb;
    if (a->path_id != b->path_id) return a->path_id < b->path_id ? -1 : 1;
    if (a->record  != b->record)  return a->record  < b->record  ? -1 : 1;
    if (a->slot    != b->slot)    return a->slot    < b->slot    ? -1 : 1;
    if (a->start_row != b->start_row) return a->start_row < b->start_row ? -1 : 1;
    return 0;
}

static int write_traffic_paths_bin(const char *path, cxa_bgd *b,
                                   const cxa_tpaths *paths,
                                   const cxa_tdesc *td)
{
    FILE *f;
    uint32_t request_count = 0, i, k, request_base;
    cx_binding *bindings;
    uint32_t nbind = 0, nentries = 0;
    /* (record,slot) -> speed/rate, python dict semantics: last write wins */
    struct cx_row {
        uint32_t key;
        double   mph, rate[7];
        int      has_speed, has_rate;
    } *table;
    uint32_t ntable = 0, tcap;

    for (i = 0; i < td->npool_windows; i++)
        request_count += td->pool_windows[i].nrequests;
    for (i = 0; i < 6; i++)
        nentries += td->lists[i].nrecords;

    tcap = 1;
    if (td->nschedules) {
        tcap += td->schedules[0].nspeeds + td->schedules[0].nrates;
    }
    bindings = (cx_binding *)calloc(td->nrecords * 8 + 1, sizeof(cx_binding));
    table = calloc(tcap, sizeof *table);
    if (!bindings || !table) { free(bindings); free(table); return -1; }
    for (i = 0; i < td->nrecords; i++) {
        for (k = 0; k < td->records[i].nslots; k++) {
            bindings[nbind].path_id   = td->records[i].slots[k].path_id;
            bindings[nbind].record    = i;
            bindings[nbind].slot      = k;
            bindings[nbind].start_row = td->records[i].slots[k].start_row;
            nbind++;
        }
    }
    qsort(bindings, nbind, sizeof(cx_binding), cmp_binding);

    /* only schedule row 0 -- the state the manager installs before the race
     * starts; rows 1..n are progress-keyed updates that stay [?] */
    if (td->nschedules && !td->schedules[0].trigger
        && !td->schedules[0].trigger_hi) {
        const cxa_schedule *row = &td->schedules[0];
        for (i = 0; i < row->nspeeds; i++) {
            uint32_t key = ((uint32_t)row->speeds[i].record << 8)
                         | row->speeds[i].slot;
            for (k = 0; k < ntable && table[k].key != key; k++)
                ;
            if (k == ntable)
                table[ntable++].key = key;
            table[k].mph = row->speeds[i].mph;
            table[k].has_speed = 1;
        }
        for (i = 0; i < row->nrates; i++) {
            uint32_t key = ((uint32_t)row->rates[i].record << 8)
                         | row->rates[i].slot;
            for (k = 0; k < ntable && table[k].key != key; k++)
                ;
            if (k == ntable)
                table[ntable++].key = key;
            memcpy(table[k].rate, row->rates[i].rate, 7 * sizeof(double));
            table[k].has_rate = 1;
        }
    }
    /* sorted(set(speeds) + set(rates)) -- key is (record, slot) */
    for (i = 1; i < ntable; i++) {          /* insertion sort on the key */
        struct cx_row cur = table[i];
        int j = (int)i - 1;
        while (j >= 0 && table[j].key > cur.key) {
            table[j + 1] = table[j];
            j--;
        }
        table[j + 1] = cur;
    }

    f = fopen(path, "wb");
    if (!f) { free(bindings); free(table); return -1; }
    cxa_w_bytes(f, "B3TP", 4);
    cxa_w_u32(f, 4);
    cxa_w_u32(f, paths->point_count);
    cxa_w_u32(f, paths->npaths);
    cxa_w_u32(f, td->npool_windows);
    cxa_w_u32(f, request_count);
    for (i = 0; i < paths->point_count; i++)
        cxa_w_v3_gl(f, cxa_f3(b->d, paths->points + (size_t)i * 16));
    for (i = 0; i < paths->npaths; i++) {
        const cxa_tpath *p = &paths->paths[i];
        cxa_w_u32(f, p->row_count);
        /* pairs and distances go out exactly as they sit in the file */
        cxa_w_bytes(f, b->d + p->pairs_off, (size_t)p->row_count * 4);
        cxa_w_bytes(f, b->d + p->dist_off, (size_t)p->row_count * 8);
        cxa_w_bytes(f, b->d + p->aux_off, (size_t)p->row_count * 0x12);
    }
    request_base = 0;
    for (i = 0; i < td->npool_windows; i++) {
        const cxa_poolwin *w = &td->pool_windows[i];
        cxa_w_u32(f, w->first_progress);
        cxa_w_u32(f, w->last_progress);
        cxa_w_u32(f, request_base);
        cxa_w_u8(f, w->nrequests);
        cxa_w_u8(f, w->refresh_count);
        cxa_w_u16(f, 0);
        request_base += w->nrequests;
    }
    for (i = 0; i < td->npool_windows; i++) {
        const cxa_poolwin *w = &td->pool_windows[i];
        for (k = 0; k < w->nrequests; k++) {
            cxa_w_u16(f, w->requests[k].first_row);
            cxa_w_u16(f, w->requests[k].last_row);
            cxa_w_u8(f, w->requests[k].path_id);
            cxa_w_u8(f, w->requests[k].direction);
        }
    }
    cxa_w_u32(f, 6);                        /* one class list per TDESC list */
    cxa_w_u32(f, nentries);
    cxa_w_u32(f, nbind);
    cxa_w_u32(f, ntable);
    {
        uint32_t base = 0;
        for (i = 0; i < 6; i++) {
            cxa_w_u32(f, td->lists[i].cls);
            cxa_w_u32(f, base);
            cxa_w_u32(f, td->lists[i].nrecords);
            cxa_w_u32(f, td->lists[i].total);
            base += td->lists[i].nrecords;
        }
    }
    for (i = 0; i < 6; i++) {
        for (k = 0; k < td->lists[i].nrecords; k++) {
            const cxa_listrec *r = &td->lists[i].records[k];
            cxa_w_pad(f, r->id, 16);
            cxa_w_u32(f, r->weight);
            cxa_w_bytes(f, r->colours, 8);
            cxa_w_u32(f, 0);
        }
    }
    for (i = 0; i < nbind; i++) {
        cxa_w_u8(f, bindings[i].path_id);
        cxa_w_u8(f, bindings[i].record);
        cxa_w_u8(f, bindings[i].slot);
        cxa_w_u8(f, 0);
        cxa_w_u32(f, bindings[i].start_row);
    }
    for (i = 0; i < ntable; i++) {
        int t;
        cxa_w_u8(f, table[i].key >> 8);              /* manager record */
        cxa_w_u8(f, table[i].key & 0xff);            /* slot           */
        cxa_w_u16(f, 0);
        cxa_w_f32(f, table[i].has_speed ? table[i].mph : 0.0);
        for (t = 0; t < 6; t++)                      /* rate[0..5] only */
            cxa_w_f32(f, table[i].has_rate ? table[i].rate[t] : 0.0);
    }
    fclose(f);
    free(bindings);
    free(table);
    return 0;
}

/* --------------------------------------------------------------- the stage */

int cx_extract_paths(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir)
{
    cxa_analysis a;
    cxa_tdesc td;
    cxa_tpaths paths;
    char *bgd_path, *out;
    const char *event = getenv("B3_EVENT");
    int rc = -1;

    (void)game_dir;
    (void)track_id;
    bgd_path = cxa_join(track_dir, "Gamedata.bgd");
    if (!bgd_path)
        return -1;
    if (cxa_analyse(&a, bgd_path, event && *event ? event : CXA_DEFAULT_EVENT,
                    out_dir)) {
        free(bgd_path);
        cxa_analysis_free(&a);
        return -1;
    }
    free(bgd_path);

    out = cxa_join(out_dir, "route.bin");
    if (!out || write_route_bin(out, &a))
        goto done;
    free(out);

    out = cxa_join(out_dir, "traffic_paths.bin");
    if (!out)
        goto done;
    if (cxa_read_tdesc(a.bgd, a.ev, &td)) {
        cxa_tdesc_free(&td);
        goto done;
    }
    if (cxa_traffic_paths(a.bgd, a.ev, &paths)) {
        cxa_tdesc_free(&td);
        cxa_tpaths_free(&paths);
        goto done;
    }
    rc = write_traffic_paths_bin(out, a.bgd, &paths, &td);
    cxa_tdesc_free(&td);
    cxa_tpaths_free(&paths);

done:
    free(out);
    cxa_analysis_free(&a);
    return rc;
}

int cx_extract_bgd_paths(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir)
{
    return cx_extract_paths(game_dir, track_dir, track_id, out_dir);
}
