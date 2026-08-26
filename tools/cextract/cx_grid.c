/* cx_grid.c -- <out_dir>/grid.bin ('B3GR' v1).
 * Port of tools/extract_start_grid.py's grid.bin half (the spec).  The
 * src/burnout3_start_grid.h that tool also writes is deliberately NOT
 * emitted here.
 *
 * Provenance [C]: the grid is the first six 0x50-byte slots of the EVENT
 * SPATIAL RECORD, located the way the game's own .bgd parser locates it --
 * FUN_0018B250 reads the event's 0x800-byte param record (offset from the
 * u32 table at file 0x198, event count at file 0x260) and then seeks to
 * {size = param+0x3BC, offset = param+0x3C0} (`mov eax,[eax+0x3c0]`
 * @0x0018B4A1, `+0x3bc` @0x0018B4D6).  Spatial records are SHARED between
 * events and are NOT at 0xC000 + slot*0x800.
 *
 * Slot layout (0x50 bytes, rows are unit vectors -- asserted here):
 *   +0x00 f32[4] right  +0x10 f32[4] up  +0x20 f32[4] at (forward)
 *   +0x30 f32[4] position                +0x40 u32 road-network node index
 *
 * Emitted in the harness's GL space (z negated, RE_NOTES 12):
 *   +0x00 char[4] 'B3GR'  +0x04 u32 version = 1  +0x08 u32 slot_count
 *   +0x0C u32 node_index_present (1)
 *   +0x10 per slot: f32 pos[3], f32 fwd[3], f32 right[3], f32 up[3], u32 node
 */
#include "cx_extract.h"
#include "cx_common_a.h"

#include <stdlib.h>

static int unit_ok(cxa_v3 v)
{
    double n2 = v.x * v.x + v.y * v.y + v.z * v.z;
    return n2 > 0.98 && n2 < 1.02;
}

int cx_extract_grid(const char *game_dir, const char *track_dir,
                    const char *track_id, const char *out_dir)
{
    cxa_bgd bgd;
    cxa_event *ev;
    cxa_gridslot slots[6];
    char *bgd_path, *out = NULL;
    const char *event = getenv("B3_EVENT");
    FILE *f;
    int k, rc = -1;

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
    if (!ev || cxa_grid(&bgd, ev, slots))
        goto done;
    for (k = 0; k < 6; k++)
        if (!unit_ok(slots[k].right) || !unit_ok(slots[k].up)
            || !unit_ok(slots[k].at))
            goto done;                  /* slot row is not a unit vector */

    out = cxa_join(out_dir, "grid.bin");
    if (!out)
        goto done;
    f = fopen(out, "wb");
    if (!f)
        goto done;
    cxa_w_bytes(f, "B3GR", 4);
    cxa_w_u32(f, 1);
    cxa_w_u32(f, 6);
    cxa_w_u32(f, 1);
    for (k = 0; k < 6; k++) {
        cxa_w_v3_gl(f, slots[k].pos);
        cxa_w_v3_gl(f, slots[k].at);
        cxa_w_v3_gl(f, slots[k].right);
        cxa_w_v3_gl(f, slots[k].up);
        cxa_w_u32(f, slots[k].node);
    }
    fclose(f);
    rc = 0;

done:
    free(out);
    cxa_bgd_close(&bgd);
    return rc;
}

int cx_extract_start_grid(const char *game_dir, const char *track_dir,
                          const char *track_id, const char *out_dir)
{
    return cx_extract_grid(game_dir, track_dir, track_id, out_dir);
}
