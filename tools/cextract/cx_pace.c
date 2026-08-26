/* cx_pace.c -- <out_dir>/pace.bin ('B3PC' v1): the per-event AI PACE RECORDS
 * and the event's lap count, straight out of Tracks/<REG>/<Cn_Vn>/Gamedata.bgd.
 *
 * Port of tools/gen_ai_pace.py, which until now baked the same bytes into
 * src/burnout3_ai_pace.h for all 36 tracks at once.  The generator is the
 * spec; this stage emits ONE track's rows as a runtime asset so nothing is
 * compiled in.  Byte-for-byte agreement with the python original is the
 * acceptance gate (tools/validate_no_baked_data.py section 3).
 *
 * ============================================================ PROVENANCE [C]
 * The rubber band in Burnout 3 is event DATA, not a constant: how long each
 * rival keeps its catch-up licence is one byte per grid slot inside the
 * event's 0x800-byte param record.  The consumer is FUN_00172870, the AI
 * navigator's pace-record reader, called once per car from the AI constructor
 * FUN_001718A0.  Its record branch is 0x001728DD..0x00172A20:
 *
 *     slot = (s8) racecar+0x19BC                       ; the GRID SLOT 0..5
 *     if (DAT_0073A170 == 0 || slot >= DAT_0073A180)   ; no record for it
 *         ... defaults, AI+0x9E8 = 0 ...               ; @0x001728F0
 *     else:
 *         rec = DAT_0073A170 + slot * 0x98             ; @0x00172937 imul 0x98
 *         AI+0x9E4 = rec[0x97] * 0.01
 *         AI+0x9E8 = rec[0x93] * 0.01   <- THE CATCH-UP WINDOW
 *         AI+0x9EC = rec[0x90] * 0.01   <- aggression, also into AI+0x9E0
 *         AI+0x9F0 = rec[0x92] * 0.01
 *         AI+0x9F4 = rec[0x91] * 0.01
 *         AI+0x9F8 = (s8) rec[0x95]
 *         AI+0xA12 = (rec[0x96] & 1) == 1
 *
 * and the table base is installed by the .bgd loader FUN_0018B250:
 *
 *     @0x0018B478   DAT_0073A170 = <the 0x800-byte event PARAM record>
 *     @0x0018B48A   DAT_0073A180 = param[0x3B4]        ; opponent count
 *
 * So a pace record is `param + grid_slot * 0x98`, six of them tiling
 * 0x000..0x390, immediately below the medal thresholds at +0x3A0/+0x3A4/
 * +0x3A8 and the opponent count at +0x3B4.  The tiling is the cross-check:
 * 0x10 + 16*4 + 16*4 + 8 == 0x98 exactly, and 6 * 0x98 == 0x390 lands exactly
 * on the first documented field.
 *
 * rec+0x94 is not read by any retail code path; it is carried verbatim.
 *
 * The lap count in the same record is P+0x3B8 (RE_BGD 3, cx_common_a.h) --
 * the number the track-select screen shows and the harness races to.  It
 * rides along here because it is the same record and the same walk.
 *
 * ================================================================ THE FILTER
 * gen_ai_pace.py accepts an event slot only when its param offset is
 * `0x3000 <= ro <= 0xB800 and ro % 0x800 == 0` and the opponent count at
 * +0x3B4 is 1..6.  Unused slots keep stale tool memory in both the id and the
 * offset table (RE_BGD), so that gate is what separates a real event from a
 * leftover.  It is reproduced here EXACTLY; cxa_bgd_open's own bounds check
 * is looser and would admit rows the python tool drops.
 *
 * ================================================================= pace.bin
 *   +0x00 char[4] 'B3PC'   +0x04 u32 version = 1
 *   +0x08 u32 event_count  +0x0C u32 slots_per_event = 6
 *   +0x10 event_count x 72-byte record, in .bgd event-slot order:
 *           char[16] id     base-40 event id, NUL padded ("OFFSGRCF")
 *           u32      laps   param+0x3B8
 *           u32      n_opp  param+0x3B4
 *           u8[6][8] pace   param + slot*0x98 + 0x90 .. +0x97
 *                           = { aggression, f91, f92, catchup,
 *                               raw94, mode, flags, f97 }
 * Little-endian throughout.  No coordinates, so no GL reflection applies.
 */
#include "cx_extract.h"
#include "cx_common_a.h"

#include <stdlib.h>
#include <string.h>

#define CXP_REC_STRIDE  0x98    /* @0x00172937  imul $0x98                 */
#define CXP_N_SLOTS     6       /* the shipped race field size (RE_BGD 3)  */
#define CXP_PACE_OFF    0x90    /* the byte block FUN_00172870 reads       */
#define CXP_N_OPP_OFF   0x3B4   /* @0x0018B484  DAT_0073A180               */
#define CXP_LAPS_OFF    0x3B8   /* RE_BGD 3                                */
#define CXP_PARAM_LO    0x3000  /* gen_ai_pace.py's param-offset window    */
#define CXP_PARAM_HI    0xB800

int cx_extract_pace(const char *game_dir, const char *track_dir,
                    const char *track_id, const char *out_dir)
{
    cxa_bgd bgd;
    char *bgd_path, *out = NULL;
    FILE *f = NULL;
    int i, k, kept = 0, rc = -1;
    long count_at;

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

    out = cxa_join(out_dir, "pace.bin");
    if (!out)
        goto done;
    f = fopen(out, "wb");
    if (!f)
        goto done;

    cxa_w_bytes(f, "B3PC", 4);
    cxa_w_u32(f, 1);
    count_at = ftell(f);
    cxa_w_u32(f, 0);                    /* event_count, back-patched below */
    cxa_w_u32(f, CXP_N_SLOTS);

    for (i = 0; i < bgd.nevents; i++) {
        const cxa_event *e = &bgd.events[i];
        uint32_t n_opp;
        char id[16];

        /* gen_ai_pace.py's gate, verbatim */
        if (e->param < CXP_PARAM_LO || e->param > CXP_PARAM_HI
            || (e->param % 0x800) != 0)
            continue;
        if ((size_t)e->param + 0x800 > bgd.n)
            continue;
        n_opp = cxa_u32(bgd.d, e->param + CXP_N_OPP_OFF);
        if (n_opp == 0 || n_opp > CXP_N_SLOTS)
            continue;

        memset(id, 0, sizeof id);
        strncpy(id, e->id, sizeof id - 1);
        cxa_w_bytes(f, id, sizeof id);
        cxa_w_u32(f, cxa_u32(bgd.d, e->param + CXP_LAPS_OFF));
        cxa_w_u32(f, n_opp);
        for (k = 0; k < CXP_N_SLOTS; k++)
            cxa_w_bytes(f, bgd.d + e->param + (size_t)k * CXP_REC_STRIDE
                                 + CXP_PACE_OFF, 8);
        kept++;
    }

    if (fseek(f, count_at, SEEK_SET) != 0)
        goto done;
    cxa_w_u32(f, (uint32_t)kept);
    if (fclose(f) != 0) {
        f = NULL;
        goto done;
    }
    f = NULL;
    rc = kept > 0 ? 0 : -1;

done:
    if (f)
        fclose(f);
    free(out);
    cxa_bgd_close(&bgd);
    return rc;
}
