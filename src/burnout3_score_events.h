/* burnout3_score_events.h -- the four in-race boost/BP EARN EVENTS.
 *
 * ONCOMING ("driving on the wrong side of the road"), NEAR MISS, DRIFT and
 * AIR: what opens each event, what it accumulates, what closes it, how many
 * boost units and Burnout Points it pays, and which HUD earn-callout tier it
 * lights.  Recovered from the retail default.xbe and verified by differential
 * execution of the real x86 under Unicorn (tools/validate_score_events.py).
 *
 * OWNERSHIP: this module (both files) belongs to the score-events agent.
 * The harness calls only this contract; the owning agent may extend the API
 * -- burnout3_full.c call sites are patched by the orchestrator on landing.
 *
 * PROVENANCE (all addresses are burnout3.elf VAs; .text = flat + 0x10000)
 * ---------------------------------------------------------------------
 *   FUN_001935F0  per-frame score update; computes the ONE per-frame distance
 *                 step shared by air/oncoming/drift and calls the detectors
 *                 (0x001939B5..0x00193A48).
 *   FUN_00013C10  the step's length:  sqrt(x*x + y*y + z*z)  [C, exact asm].
 *   FUN_00196940  AIR detector        (state racecar+0x10C0)
 *   FUN_00196BE0  ONCOMING detector   (state racecar+0x18FC)
 *   FUN_00196E10  DRIFT detector      (state racecar+0x10C2)
 *   FUN_00192D20  category tier tracker shared by all four events
 *   FUN_00194EE0  NEAR MISS detector  (8 tracked slots + chain)
 *   FUN_00195DD0  near-miss proximity test: OBB-vs-OBB gap in the XZ plane
 *   FUN_0018D790  writes racecar+0x18FC -- THE oncoming rule (see below)
 *   FUN_0017A530  boost award (b3_boost_award in burnout3_gameplay.h)
 *
 * THE CRASH GATE [C, FUN_001935F0 @0x001939AD..0x001939D9 + 0x00193A80]
 *   NONE of the four detectors runs while the car is crashed.  The per-frame
 *   score update tests two racecar bytes before it computes the distance step:
 *        MOV AL,[ESI+0x18fa] ; TEST AL,AL ; JNZ 0x00193a80     <- crashed
 *        MOV AL,[ESI+0x18fb] ; TEST AL,AL ; JNZ 0x00193a80     <- respawning
 *   and on either it jumps PAST FUN_00194A80/FUN_00194EE0/FUN_00196940/
 *   FUN_00196BE0/FUN_00196E10 into a reset block (0x00193A80..0x00193CE4)
 *   that wipes the near-miss slots and every category record.  So a crash
 *   CANCELS the pending near miss instead of paying it, and forfeits the
 *   in-progress air/oncoming/drift event's Category BP, stats and callout.
 *   b3_score_events_crash_reset() is that block; both entry points refuse to
 *   run while b3_score_events_suspended().
 *
 * THE ONCOMING RULE [C, FUN_0018D790 @0x0018D89F]
 *   The game does not measure a heading against traffic.  It locates the car
 *   on the road network (FUN_00174960 -> path object + u16 segment index),
 *   reads the 10-byte per-segment record
 *        rec = *(*(path + 4) + 8) + segment_index * 10
 *   and sets
 *        racecar+0x18FC = ((rec[3] & 7) == 1 || (rec[3] & 7) == 3)
 *   i.e. a 3-bit LANE-TYPE field baked into the road network marks which
 *   segments count as the wrong side.  racecar+0x18D0 = (s16)rec[0] is the
 *   route section id.  With no road network loaded the flag is forced 0.
 *
 * Marks: [C] confirmed by executing the real x86 (or exact decompilation),
 * [S] static only, [?] open, GLUE = harness-side representation bridge.
 */
#ifndef BURNOUT3_SCORE_EVENTS_H
#define BURNOUT3_SCORE_EVENTS_H

#include <stddef.h>

#include "burnout3_gameplay.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Earn-callout categories -- the game's own callout-table order (0x003C8390),
 * identical to burnout3_hud.h's B3_HUD_CAT_*. */
enum {
    B3_SE_CAT_AIR      = 0,
    B3_SE_CAT_DRIFT    = 1,
    B3_SE_CAT_ONCOMING = 2,
    B3_SE_CAT_NEARMISS = 3
};

/* The score object's announcement ids (score+0x254).  A closing event may
 * claim the HUD callout only when the pending id is <= its own id, so these
 * values ARE the priority order.  [C: `CMP [score+0x254], <own id> ; JLE`
 * at 0x00196F21 drift, 0x00196A2E air, 0x00196CF1 oncoming, 0x00195167
 * near miss -- the immediate in each compare IS that event's id.] */
enum {
    B3_SE_EVID_DRIFT    = 0x71,
    B3_SE_EVID_AIR      = 0x72,
    B3_SE_EVID_ONCOMING = 0x73,
    B3_SE_EVID_NEARMISS = 0x74
};

/* The "Score/..." parameters these detectors consume.  Named exactly as the
 * registrar FUN_00190430 registers them; addresses are the storage globals.
 * b3_score_events_init() loads the VDB-TUNED values the shipped game plays
 * with (Data/vdb.xml); the compiled-in defaults are in the comments and in
 * b3_score_params_defaults(), which the differential test uses so that the
 * emulated original and this C read identical numbers. */
typedef struct {
    /* "Score/Boost/Air" */
    float air_min_m;        /* 0x3F73A0 Minimum Air Distance (Metres)      */
    float air_boost_per_m;  /* 0x3F73A4 Boost value per Air metre          */
    /* "Score/Boost/Oncoming" */
    float onc_min_mph;      /* 0x3F73A8 Minimum Oncoming Speed (MPH)       */
    float onc_min_m;        /* 0x3F73AC Minimum Oncoming Distance (Metres) */
    float onc_boost_per_m;  /* 0x3F73B0 Boost value per Oncoming metre     */
    /* "Score/Boost/Drift" */
    float drift_min_mph;    /* 0x3F73B4 Minimum Drift Speed (MPH)          */
    float drift_min_m;      /* 0x3F73B8 Minimum Drift Distance (Metres)    */
    float drift_boost_per_m;/* 0x3F73BC Boost value per Drift metre        */
    /* "Score/Boost/Near Miss" */
    float nm_dist;          /* 0x3F73C0 Near Miss Distance (Metres)        */
    float nm_min_mph;       /* 0x3F73C4 Minimum Near Miss Speed (MPH)      */
    float nm_chain_mph;     /* 0x3F73C8 Minimum Near Miss Chain Speed (MPH)*/
    float nm_boost;         /* 0x3F73CC Boost value per Near Miss          */
    float nm_chain_time;    /* 0x3F73D0 Near Miss Chain Time (Seconds)     */
    /* "Score/Burnout Points" */
    int   air_cat_bp[4];    /* 0x3F7490 Air Category BP                    */
    int   onc_cat_bp[4];    /* 0x3F74A0 Oncoming Category BP               */
    int   drift_cat_bp[4];  /* 0x3F74B0 Drift Category BP                  */
    int   nm_cat_bp[4];     /* 0x3F74E4 Near Miss Category BP              */
    int   nm_bp_per_link;   /* 0x3F7480 Near Miss BP (per chain link)      */
    /* "Score/BP Categories" minima (the tier thresholds) */
    float air_minima[4];    /* 0x3F7550 Air Category Minima (Metres)       */
    float onc_minima[4];    /* 0x3F7560 Oncoming Category Minima (Metres)  */
    float drift_minima[4];  /* 0x3F7570 Drift Category Minima (Metres)     */
    float nm_minima[4];     /* 0x3F75A4 Near Miss Category Minima (Counts) */
    /* "Score/Boost/Aggressive" + "Score/Boost/Takedowns" -- the RUBBING
     * event's parameters.  Names verbatim from the registrar FUN_00190430;
     * values read out of build/burnout3.elf's .data at the cited address. */
    float rub_min_s;        /* 0x3F73F0 Min Contact Time For Rubbing (s)   */
    float rub_boost_per_s;  /* 0x3F73F4 Boost value for rubbing (units)    */
    float crash_wait_s;     /* 0x3F7404 Maximum Crash Wait Time (Seconds)  */
    float rub_grace_s;      /* 0x3F7408 Maximum Crash Wait Time - No Slam  */
    int   rub_cat_bp[3];    /* 0x3F74CC Rubbing Category BP (THREE tiers)  */
    float rub_minima[3];    /* 0x3F758C, score+0x570; count 3 @0x001933A2  */
} B3ScoreParams;

/* Live parameter block; b3_score_events_init() fills it with the VDB tune. */
extern B3ScoreParams b3_score_params;

void b3_score_params_vdb(B3ScoreParams* p);       /* retail tuned values  */
void b3_score_params_defaults(B3ScoreParams* p);  /* compiled-in defaults */

/* m/s -> mph.  The SCORING side uses the TRUE constant 2.2369363, not the
 * 2.2374146 the physics uses (docs/RE_GAMEPLAY.md 5).  [C @0x0038994C] */
#define B3_SE_MPH_PER_MS 2.2369363f

/* ------------------------------------------------------------------ *
 * The 0x1C-byte category record handled by FUN_00192D20.  Instances in
 * the score object: air +0x358, oncoming +0x374, drift +0x390, near-miss
 * chain +0x418.  Field comments give the record-relative offsets.
 * ------------------------------------------------------------------ */
/* packed pins the retail offsets; aligned(4) pins only the struct BASE.
 * No member moves (the offsetof asserts below prove it) -- but with a
 * 4-aligned base the 4-byte fields ARE 4-aligned, so taking their address
 * is no longer undefined and -Waddress-of-packed-member goes quiet on the
 * fields that really are aligned (and still fires on any that are not). */
typedef struct __attribute__((packed, aligned(4))) B3CatRecord {
    // ---- RETAIL WINDOW 0x0000..0x001C: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    float                value;  /* +0x00 accumulated metres (near miss: count) */
    float                clock;  /* +0x04 last update clock                     */
    float                prev_value;  /* +0x08                                       */
    unsigned char _pad00[0x4];
    unsigned char        active;  /* +0x10 the event-open flag */
    signed char          tier;  /* +0x11 current tier, -1 = none               */
    signed char          prev_tier;  /* +0x12                                       */
    signed char          count;  /* +0x13 number of thresholds                  */
    unsigned char _pad01[0x8];
} B3CatRecord;

#define B3CATRECORD_RETAIL_SPAN 0x001Cu
_Static_assert(offsetof(B3CatRecord, value) == 0x0000, "value off retail");
_Static_assert(offsetof(B3CatRecord, clock) == 0x0004, "clock off retail");
_Static_assert(offsetof(B3CatRecord, prev_value) == 0x0008, "prev_value off retail");
_Static_assert(offsetof(B3CatRecord, active) == 0x0010, "active off retail");
_Static_assert(offsetof(B3CatRecord, tier) == 0x0011, "tier off retail");
_Static_assert(offsetof(B3CatRecord, prev_tier) == 0x0012, "prev_tier off retail");
_Static_assert(offsetof(B3CatRecord, count) == 0x0013, "count off retail");


/* FUN_00192D20 verbatim: store value/prev/clock, then -- only while
 * `tier < count-1` -- scan the minima top-down and take the highest index
 * whose minimum <= value.  Never downgrades within an event. */
/* `minima` is passed in rather than stored: a host pointer inside a
 * retail-shaped record would squat on the game's bytes, and it also
 * inflated the record past retail's 0x1C stride. */
void b3_cat_track(B3CatRecord* r, const float* minima, float value,
                  float clock);

/* Event-end reset (the tail of each detector): prev := value, tier := -1. */
void b3_cat_reset(B3CatRecord* r, float clock);

/* ------------------------------------------------------------------ *
 * Oriented box exactly as FUN_00195DD0 reads it.
 *   m     world matrix, rows 0..2 = right/up/at axes, row 3 = translation
 *         (the game's racecar+0x10 matrix; == B3RigidBody.frame)
 *   bmax  the .bgv +0xE80 corner (B3VehicleFull.half_ext)
 *   bmin  the .bgv +0xE90 corner (B3VehicleFull.center_off)
 * Only rows 0 and 2 and the X/Z components are used: the test is a 2-D OBB
 * gap in the ground plane, with a separate +-4 m height gate on row3.y.
 * ------------------------------------------------------------------ */
typedef struct {
    float m[4][4];
    float bmax[4];
    float bmin[4];
} B3ScoreObb;

/* FUN_00195DD0 verbatim.  1 when the two boxes' closest-point gap in XZ is
 * below `dist` AND |dy| <= 4.  NOTE: this is a BOX-TO-BOX GAP, not a centre
 * distance -- "Near Miss Distance (Metres)" is clearance, not separation. */
int b3_score_obb_near(const B3ScoreObb* me, const B3ScoreObb* other,
                      float dist);

/* ------------------------------------------------------------------ *
 * Per-car detector state (the score-object subset these events own).
 * ------------------------------------------------------------------ */
#define B3_SE_NM_SLOTS 8

/* Opponent RACERS the score object carries per-opponent state for.  [C: the
 * constructor FUN_00192EA0 @0x00193361 seeds exactly six entries -- MOV EDX,6
 * over score+0x510 / +0x528 / +0x540 / +0x55E, and 0x510 + 6*4 == 0x528.] */
#define B3_SE_RUB_CARS 6

/* packed pins the retail offsets; aligned(4) pins only the struct BASE.
 * No member moves (the offsetof asserts below prove it) -- but with a
 * 4-aligned base the 4-byte fields ARE 4-aligned, so taking their address
 * is no longer undefined and -Waddress-of-packed-member goes quiet on the
 * fields that really are aligned (and still fires on any that are not). */
typedef struct __attribute__((packed, aligned(4))) B3ScoreEvents {
    // ---- RETAIL WINDOW 0x0000..0x1900: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0x358];
    B3CatRecord          air;  /* score+0x358 */
    B3CatRecord          onc;  /* score+0x374 */
    B3CatRecord          drift;  /* score+0x390 */
    unsigned char _pad01[0x1C];
    unsigned char        air_scored;  /* score+0x3C8 */
    unsigned char        onc_scored;  /* score+0x3C9 */
    unsigned char        drift_scored;  /* score+0x3CA */
    unsigned char _pad02[0x1];
    int                  nm_total;  /* score+0x3CC lifetime near misses    */
    int                  nm_chain;  /* score+0x3D0 links in the live chain */
    unsigned char _pad03[0x8];
    float                prev_clock;  /* score+0x3DC previous frame's clock  */
    float                nm_last;  /* score+0x3E0 last link's clock       */
    float                nm_chain_end;  /* score+0x3E4                         */
    signed char          nm_id[B3_SE_NM_SLOTS];  /* score+0x3E8, -1 = free */
    float                nm_seen[B3_SE_NM_SLOTS];  /* score+0x3F0 last-proximity clock */
    unsigned char        nm_armed[B3_SE_NM_SLOTS];  /* score+0x410 */
    B3CatRecord          nm;  /* score+0x418 (value = chain length)          */
    unsigned char _pad04[0xDC];
    float                rub_last[B3_SE_RUB_CARS];  /* score+0x510  last-contact clock   */
    float                rub_time[B3_SE_RUB_CARS];  /* score+0x528  contact timer, s     */
    float                rub_start[B3_SE_RUB_CARS];  /* score+0x540  contact start clock  */
    unsigned char        rub_prev_touch[B3_SE_RUB_CARS];  /* score+0x558           */
    unsigned char        rub_touch[B3_SE_RUB_CARS];  /* score+0x55E           */
    B3CatRecord          rub;  /* score+0x564 value / +0x568 clock / */
    int                  rub_target;  /* score+0x580 -- the opponent being rubbed */
    unsigned char _pad05[0x137C];

    // ---- HARNESS SIDE, past the retail window: no recovered
    // offset in THIS object, so it must not squat on retail's bytes.
    float                air_total;  
    float                air_best;  
    int                  air_count;  
    float                onc_total;  
    float                onc_best;  
    float                drift_total;  
    float                drift_best;  
    int                  bp;  
    int                  bp_event;  
    int                  callout_id;  /* 0 = none, else B3_SE_EVID_*                     */
    int                  callout_cat;  /* B3_SE_CAT_*  -> b3_hud_boost_event(cat, tier)   */
    int                  callout_tier;  /* 0..3                                            */
    unsigned char        rc_crashed;  /* racecar+0x18FA */
    unsigned char        rc_respawning;  /* racecar+0x18FB */
    unsigned char        race_finished;  
    signed char          rub_payout_tier;  
    int                  rub_payout_target;  
} B3ScoreEvents;

#define B3SCOREEVENTS_RETAIL_SPAN 0x1900u
_Static_assert(offsetof(B3ScoreEvents, air) == 0x0358, "air off retail");
_Static_assert(offsetof(B3ScoreEvents, onc) == 0x0374, "onc off retail");
_Static_assert(offsetof(B3ScoreEvents, drift) == 0x0390, "drift off retail");
_Static_assert(offsetof(B3ScoreEvents, air_scored) == 0x03C8, "air_scored off retail");
_Static_assert(offsetof(B3ScoreEvents, onc_scored) == 0x03C9, "onc_scored off retail");
_Static_assert(offsetof(B3ScoreEvents, drift_scored) == 0x03CA, "drift_scored off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_total) == 0x03CC, "nm_total off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_chain) == 0x03D0, "nm_chain off retail");
_Static_assert(offsetof(B3ScoreEvents, prev_clock) == 0x03DC, "prev_clock off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_last) == 0x03E0, "nm_last off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_chain_end) == 0x03E4, "nm_chain_end off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_id) == 0x03E8, "nm_id off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_seen) == 0x03F0, "nm_seen off retail");
_Static_assert(offsetof(B3ScoreEvents, nm_armed) == 0x0410, "nm_armed off retail");
_Static_assert(offsetof(B3ScoreEvents, nm) == 0x0418, "nm off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_last) == 0x0510, "rub_last off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_time) == 0x0528, "rub_time off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_start) == 0x0540, "rub_start off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_prev_touch) == 0x0558, "rub_prev_touch off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_touch) == 0x055E, "rub_touch off retail");
_Static_assert(offsetof(B3ScoreEvents, rub) == 0x0564, "rub off retail");
_Static_assert(offsetof(B3ScoreEvents, rub_target) == 0x0580, "rub_target off retail");


/* Module init: loads b3_score_params with the retail VDB tune. */
void b3_score_events_init(void);

/* Per-car reset (race start / crash: a crash resets every chain+record). */
void b3_score_events_reset(B3ScoreEvents* s);

/* ---- the per-frame detector inputs -------------------------------- */
typedef struct __attribute__((packed)) B3ScoreFrame {
    // ---- RETAIL WINDOW 0x0000..0x1900: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0x64];
    float                speed_mph;  /* racecar+0x64 * B3_SE_MPH_PER_MS                  */
    unsigned char _pad01[0x1058];
    unsigned char        airborne;  /* racecar+0x10C0                                   */
    unsigned char _pad02[0x1];
    unsigned char        drifting;  /* racecar+0x10C2                                   */
    unsigned char _pad03[0x19];
    float                clock;  /* race clock, racecar+0x10DC                      */
    unsigned char _pad04[0x81C];
    int                  oncoming;  /* racecar+0x18FC -- the lane-type flag above       */

    // ---- HARNESS SIDE, past the retail window: no recovered
    // offset in THIS object, so it must not squat on retail's bytes.
    float                dist_step;  /* |pos - pos_prev| this frame, metres (FUN_00013C10) */
} B3ScoreFrame;

#define B3SCOREFRAME_RETAIL_SPAN 0x1900u
_Static_assert(offsetof(B3ScoreFrame, speed_mph) == 0x0064, "speed_mph off retail");
_Static_assert(offsetof(B3ScoreFrame, airborne) == 0x10C0, "airborne off retail");
_Static_assert(offsetof(B3ScoreFrame, drifting) == 0x10C2, "drifting off retail");
_Static_assert(offsetof(B3ScoreFrame, clock) == 0x10DC, "clock off retail");
_Static_assert(offsetof(B3ScoreFrame, oncoming) == 0x18FC, "oncoming off retail");
/* NOTE: the crash gate's two inputs (racecar+0x18FA/+0x18FB) are deliberately
 * NOT fields of this struct -- callers build a B3ScoreFrame field by field on
 * the stack, and silently adding members would leave them uninitialised.
 * They are module state instead; see b3_score_events_set_crash() below.
 * ###  INTEGRATION REQUIRED: a caller that never calls that setter keeps the
 * ###  pre-fix behaviour (a crash into traffic pays a NEAR MISS as the wreck
 * ###  separates).  See scratchpad integration_scoregate.md for the patch.  */

/* ------------------------------------------------------------------ *
 * THE CRASH GATE -- FUN_001935F0 @0x001939AD..0x001939D9, reset block at
 * 0x00193A80..0x00193CE4.  [C: the differential test executes the REAL
 * slice from 0x001939AD and diffs the whole score image.]
 *
 * While `racecar+0x18FA` (crashed) or `racecar+0x18FB` (being re-placed on
 * the track after a crash) is set, the per-frame score update jumps straight
 * past the distance step and EVERY detector into a reset block.  Concretely,
 * every frame of a crash:
 *
 *     all 8 near-miss slot ids := -1   (score+0x3E8..0x3EF, one 0xFFFFFFFF
 *                                       store pair @0x00193A89/0x00193A8F)
 *     near-miss chain          := 0    (score+0x3D0 @0x00193AA0)
 *     air/onc/drift `scored`   := 0    (score+0x3C8/9/A @0x00193AAC..)
 *     air, oncoming, drift and the near-miss CHAIN records all take the
 *     standard end-of-event reset (prev := value, prev_tier := tier,
 *     clock := now, value := 0, tier := -1) and their `active` byte := 0
 *     (0x00193B36 / 0x00193B76 / 0x00193BB3 / 0x00193CA7)
 *
 * and, deliberately, NOT: the armed flags (score+0x410), the last-seen
 * clocks (+0x3F0), the lifetime near-miss count (+0x3CC), the previous-frame
 * clock (+0x3DC) -- FUN_00194EE0 is what advances that and it never runs --
 * the BP totals, the boost meter or the pending callout.  Nothing is PAID:
 * an oncoming run ended by a crash forfeits its Category BP, its total/best
 * stats and its HUD callout, and an armed near-miss slot is destroyed rather
 * than awarded.  (The block also resets the rubbing/tailgate/grinding records
 * at score+0x564/+0x598/+0x5C4 and the per-opponent arrays -- other modules.)
 * ------------------------------------------------------------------ */

/* Mirror the two racecar bytes into the state.  MUST be called once per frame
 * per car BEFORE either detector entry point; both of them consult the state
 * this sets and neither can see the racecar itself. */
void b3_score_events_set_crash(B3ScoreEvents* s, int crashed, int respawning);

/* 1 when the gate at 0x001939AD/0x001939D1 takes the crash branch, i.e. no
 * detector may run this frame. */
int b3_score_events_suspended(const B3ScoreEvents* s);

/* The reset block at 0x00193A80.  b3_score_events_frame() calls it for you
 * on a suspended frame; exposed for callers that drive the two entry points
 * in the game's own order (near miss first) and want the gate explicit. */
void b3_score_events_crash_reset(B3ScoreEvents* s, float clock);

/* AIR + ONCOMING + DRIFT for one car, in the game's own call order
 * (FUN_00196940, FUN_00196BE0, FUN_00196E10).  Awards boost into `bar`
 * through b3_boost_award and accumulates BP into s->bp/s->bp_event.
 *
 * On a suspended frame (b3_score_events_set_crash() saw either racecar byte
 * set) it runs b3_score_events_crash_reset() and NO detector. */
void b3_score_events_frame(B3ScoreEvents* s, B3BoostBar* bar,
                           const B3ScoreFrame* in);

/* NEAR MISS for one car (FUN_00194EE0).  `others`/`ids` are the proximity
 * candidates -- in the game an 8-entry-per-car broadphase list
 * (DAT_00649A8E, counts DAT_00649B3C) indexing the 0x180-stride object table
 * at 0x625FB0; the harness passes its traffic cars directly (GLUE at the
 * broadphase only -- the test and the bookkeeping below are the game's).
 * ids must be 0..126 and stable per vehicle.
 *
 * Does NOTHING while b3_score_events_suspended() -- FUN_00194EE0 is one of
 * the calls the crash gate jumps over, so a crashing car neither arms nor
 * pays a near miss, and the previous-frame clock (score+0x3DC) does not
 * advance.  The state must therefore be current: either call
 * b3_score_events_set_crash() first, or call b3_score_events_frame() (which
 * sets it) before this in the same frame. */
void b3_score_events_near_miss(B3ScoreEvents* s, B3BoostBar* bar,
                               const B3ScoreObb* me, float speed_mph,
                               float clock,
                               const B3ScoreObb* others, const int* ids,
                               int n_others);

/* ------------------------------------------------------------------ *
 * CONTACT -- what a collision does to the score object.
 *
 * The collision dispatcher at 0x00027500 switches on a contact class and
 * notifies the score object.  Class 1 (@0x00027525) is plain car-on-car
 * RUBBING and calls FUN_001979E0 for BOTH cars; the generic contact notify
 * FUN_00197920 is called from 0x000273D0 (a two-argument thunk over
 * `score_of(a), b`) and inline at 0x00026A70.  Both take the score object
 * through *(racecar+0x13F4) + 0x10D0.
 * ------------------------------------------------------------------ */

/* FUN_00197920 @0x00197920 -- THE NEAR-MISS CANCEL.  `id` is the other
 * vehicle's stable id (object+0x177, the same namespace the near-miss slots
 * use).  Verbatim:
 *
 *   if (score+0x27C == 3) return;              // race over
 *   if (racecar+0x18FA) return;                // already crashed
 *   for i in 0..7: if (slot_id[i] == id) goto disarm;
 *   for i in 0..7: if (slot_id[i] == -1) { slot_id[i]=id; seen[i]=now;
 *                                          goto disarm; }
 *   return;                                    // no free slot: nothing
 * disarm:
 *   armed[i] = 0;                              // 0x001979D4
 *
 * The slot is CLAIMED but never armed, and FUN_00194EE0's proximity scan
 * only arms a slot it took from the FREE list -- so once you have touched a
 * vehicle it cannot pay you a near miss until its slot is released (1 s
 * after the last proximity, 0x00195469).  This is why ramming a car is a
 * takedown and not a "GREAT NEAR MISS!".
 *
 * (The function also latches the tailgate/psyche-out bytes score+0x5E5/
 * +0x5E6/+0x5E8 from the shunt window score+0x5EC/+0x5F0 -- that record is
 * not modelled here.) */
void b3_score_events_contact(B3ScoreEvents* s, int id, float clock);

/* FUN_001979E0 @0x001979E0 -- the RUBBING contact mark.  `idx` is the other
 * car's RACER slot (racecar+0x19BC), 0..B3_SE_RUB_CARS-1.  Verbatim:
 *
 *   if (score+0x27C == 3) return;
 *   if (racecar+0x18FA) return;
 *   score+0x55E[idx] = 1;                      // in contact THIS frame
 *   score+0x510[idx] = score+0xC;              // last-contact clock
 */
void b3_score_events_mark_contact(B3ScoreEvents* s, int idx, float clock);

/* FUN_00194A80 @0x00194A80 -- RUBBING, one frame.
 *
 * Per opponent: while in contact the timer at score+0x528[i] grows by the
 * frame step and, past "Min Contact Time For Rubbing", opens the event;
 * out of contact the timer survives "Maximum Crash Wait Time - No Slam"
 * before it is zeroed (so a bouncing scrape stays one rub).  While the
 * event is open it accumulates seconds into the score+0x564 record, re-tiers
 * against score+0x570's minima and pays "Boost value for rubbing" * dt.
 *
 * `slam[i]` is retail's SHUNT SUPPRESSION verdict for opponent i
 * (0x00194B73..0x00194C22): a contact that is part of a slam in the last
 * "Maximum Crash Wait Time" second is a TAKEDOWN attempt, not rubbing, and
 * zeroes the timer instead of growing it.  Retail computes it as
 *
 *   recent(t) := (t >= 0) && (now <= t + 1.0)
 *   slam[i]   = ( other.aggressor(+0x16BC) == me
 *                 && ( recent(other'.slam_time(+0x1598))
 *                      || recent(other'.evt2(+0x1690)) ) )
 *            || ( score+0x5EC == other
 *                 && ( recent(my.slam_time(+0x1598))
 *                      || recent(my.evt2(+0x1690)) ) )
 *
 * where other' is *(other_racecar+0x1198).  The two +0x1690 terms are the
 * only [?] in the expression; pass 0 for an opponent you have no slam
 * information for and the suppression is simply inactive for it.
 *
 * `dt` is the frame step the game keeps in DAT_004AE1FC / DAT_0060EA1C.
 * `n` is the number of opponent slots in use.  May be called on a suspended
 * frame -- it does nothing then, exactly like every other detector. */
void b3_score_events_rubbing(B3ScoreEvents* s, B3BoostBar* bar,
                             float clock, float dt,
                             const unsigned char* slam, int n);

/* FUN_001935F0's tail @0x001940A1 -- rotate the in-contact flags into
 * score+0x558 and clear score+0x55E.  Call once per frame per car AFTER
 * b3_score_events_rubbing(); contacts reported after it land on the next
 * frame, which is the game's own arrangement (collision runs in the physics
 * step, the score object is updated in the score step). */
void b3_score_events_frame_end(B3ScoreEvents* s);

/* score+0x27C == 3.  Both contact notifies refuse to run once the race is
 * over; default 0 (racing), so a caller that never sets it is unaffected. */
void b3_score_events_set_race_finished(B3ScoreEvents* s, int finished);

/* Take the pending earn callout, if any.  Returns 1 and fills cat/tier,
 * clearing the pending flag (the score object's +0x134).  Feed straight to
 * b3_hud_boost_event(cat, tier). */
int b3_score_events_take_callout(B3ScoreEvents* s, int* cat, int* tier);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_SCORE_EVENTS_H */
