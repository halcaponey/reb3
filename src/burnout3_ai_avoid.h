/* burnout3_ai_avoid.h -- the AI AVOIDANCE stage, retail `AI+0x2B0`.
 *
 * The 256-strip lateral risk profile and its chooser, factored out of
 * burnout3_full.c so it can be differentially validated against the real
 * x86 (tools/validate_ai_avoid.py runs retail's own FUN_00170260 /
 * FUN_00170100 / FUN_0016F400 / FUN_0016FCD0 / FUN_0016F000 under Unicorn
 * and diffs this module field by field).
 *
 * Retail field map (docs/RE_AI.md section 15.1, corrected by this pass):
 *
 *   avoid+0x000  left road-edge point at the car's node      [C 0x00170260]
 *   avoid+0x010  right road-edge point -- the frame ORIGIN   [C 0x00170260]
 *   avoid+0x020  the car's world position                    [C 0x00170260]
 *   avoid+0x030  unit axis right-edge -> left-edge, y = 0    [C 0x00170260]
 *   avoid+0x040  u8  type[256]                               [C 0x0016D2F0]
 *   avoid+0x140  u8  time[256]   x 8/255 -> 0..8 s           [C 0x0016F400]
 *   avoid+0x240  s16 dist[256]   x 1000/65536 -> +-500 m     [C 0x0016F400]
 *   avoid+0x440  u8  the car's own footprint strip COUNT     [C 0x00170100]
 *   avoid+0x441  u8  the car's own footprint HIGH strip      [C 0x00170100]
 *   avoid+0x442  u8  the car's own footprint LOW strip       [C 0x00170100]
 *   avoid+0x444  f32 road width at the node                  [C 0x00170260]
 *   avoid+0x448  f32 the car's distance from the LEFT edge   [C 0x00170260]
 *   avoid+0x44C  f32 the car's distance from the RIGHT edge  [C 0x00170260]
 *
 * The world -> strip map, proved by executing FUN_0016F400:
 *
 *   strip(p) = (dot(p - right_edge, axis) - lat_right) * 5.0 + 128
 *
 * Because the car's own `lat_right` is subtracted, the frame ORIGIN cancels:
 * any lateral coordinate measured along the same axis with the same metre
 * scale yields the identical strip.  Only the AXIS DIRECTION, the road
 * WIDTH and the car's offset inside it are load-bearing.        [C, measured]
 *
 * Strip TYPE codes -- measured from the four stampers, and NOT what
 * docs/RE_AI.md section 15.1 said:
 *   1  soft no-go / road end, single link      FUN_0016F6C0 @0x0016F78C
 *   2  soft no-go / road end, both links       FUN_0016F6C0 @0x0016F786
 *   3  a vehicle from the global physics list  FUN_0016EC70 @0x0016EFB4
 *   4  a vehicle from the proximity list       FUN_0016EB60 @0x0016EC30
 *   5  a WRECKED racecar                       FUN_0016EA40 @0x0016EB18
 *   6  a live racecar                          FUN_0016EA40 @0x0016EB18
 *   7  HARD no-go (wall)                       FUN_0016F6C0 @0x0016F9A8
 */
#ifndef BURNOUT3_AI_AVOID_H
#define BURNOUT3_AI_AVOID_H

#define B3_AV_N          256
#define B3_AV_MID        128
#define B3_AV_STRIPS_PER_M  5.0f   /* DAT_005A96EC <- DAT_003B1694     [C] */
#define B3_AV_PITCH      0.2f
#define B3_AV_TMAX       8.0f      /* u8 0xFF * 8/255                  [C] */
#define B3_AV_DMAX       500.0f    /* s16 32767 * 1000/65536           [C] */
#define B3_AV_TQ         31.875f   /* 255/8, TRUNCATED   [C, measured] */
#define B3_AV_DQ         65.534f   /* 32767*0.002,       [C, measured] */
#define B3_AV_THREAT     4.0f      /* DAT_003B1690                     [C] */
#define B3_AV_SLACK      0.2f      /* DAT_003A69B4                     [C] */
#define B3_AV_SIDE_GATE  2.0f      /* the `time < 2.0` side-sum gate   [C] */
#define B3_AV_EDGE_T     1.0f      /* DAT_003B168C, the clear-branch
                                    * EDGE-SCAN occupancy gate         [C] */

/* strip type codes -- see the header comment                          [C] */
#define B3_AVT_NONE      0
#define B3_AVT_SOFT1     1
#define B3_AVT_SOFT2     2
#define B3_AVT_VEHICLE   3
#define B3_AVT_PROX      4
#define B3_AVT_WRECK     5
#define B3_AVT_RACER     6
#define B3_AVT_HARD      7

/* The registered AI/Avoidance tune block, 0x0047A17C..0x0047A1C8, retail
 * Data/vdb.xml column (docs/RE_AI.md section 1).  Runtime-loaded in retail
 * -- .bss in the image -- so these are the shipped values, not literals. */
typedef struct {
    float lookahead_rc;   /* 47A17C  AVOID: LookAhead dist racecars   20   */
    float soft_nogo_t;    /* 47A180  Soft No Go offset time           2.5  */
    float soft_nogo_d;    /* 47A184  Soft No Go offset distance       101  */
    float hard_nogo_t;    /* 47A188  Hard No Go offset time           0    */
    float hard_nogo_d;    /* 47A18C  Hard No Go offset distance       200  */
    float discard_d;      /* 47A190  Dist to discard fatal racecar    100  */
    float discard_v;      /* 47A194  Vert dist to discard             5    */
    float sweep_dt;       /* 47A198  dt between start and end vehicle 0.2  */
    float spd_10;         /* 47A19C  Speed when car is <10m away      26.2 */
    float spd_20;         /* 47A1A0  Speed when car is <20m away      40   */
    float spd_30;         /* 47A1A4  Speed when car is <30m away      60   */
    float spd_r95;        /* 47A1A8  Speed when risk is >0.95         16   */
    float spd_r90;        /* 47A1AC  Speed when risk is >0.9          30   */
    float steer_f;        /* 47A1B0  Steering factor big=>extreme     5.1  */
    float extra_d;        /* 47A1B4  Extra softNoGo dist for future   5    */
    float extra_t;        /* 47A1B8  Extra softNoGo time for future   0.08 */
} B3AiAvoidParams;

extern const B3AiAvoidParams B3_AI_AVOID_VDB;

/* The six arbitrator thresholds, 0x0047A164..0x0047A178.  Layout is
 * {Risk, RiskTotal, RiskCurrent} x {straight, corner}.                [C] */
#define B3_AV_RISK        1.0f   /* 47A164 Risk threshold                  */
#define B3_AV_RISK_TOT    2.0f   /* 47A168 Total risk threshold            */
#define B3_AV_RISK_CUR    5.0f   /* 47A16C Current risk threshold          */
#define B3_AV_CRISK       0.5f   /* 47A170 Corner Risk threshold           */
#define B3_AV_CRISK_TOT   1.0f   /* 47A174 Total corner risk               */
#define B3_AV_CRISK_CUR   4.0f   /* 47A178 Current corner risk threshold   */

typedef struct {
    float         left[3], right[3], car[3], axis[3];
    float         width, lat_left, lat_right;
    unsigned char type[B3_AV_N];
    unsigned char time[B3_AV_N];   /* quantised exactly as retail  [C] */
    short         dist[B3_AV_N];
    unsigned char win, hi, lo;     /* +0x440 / +0x441 / +0x442          */
    float         spd_gate;        /* +0x48C, the 50.0 dmin type filter */
    /* chooser outputs */
    float         aim[3], dir[3];
    float         speed;           /* +0x488 avoidance speed ceiling    */
    float         tt;              /* +0x484 time-to-target             */
    float         risk_mean_r, risk_mean_l;   /* +0x490 / +0x494       */
    float         risk_tot_r, risk_tot_l;     /* +0x498 / +0x49C       */
    unsigned char state;           /* +0x4AC: 0x10 none, 1 / 2 = side   */
    unsigned char phase;           /* +0x4AF: rebuild 1 frame in 3      */
    float         frac;            /* the type 2..4 risk fraction       */
    float         dmin;            /* min dist over the middle half     */
    float         dband;           /* min dist over the WHOLE footprint
                                    * band -- what the AVOID branch's
                                    * forward lead uses  [C @0x0016CBED] */
    unsigned char clear_straight;  /* clear branch: the edge scan found
                                    * the aim side occupied, so the
                                    * lateral offset is cancelled
                                    * [C @0x0016CEC8]                    */
    int           target_strip;    /* the chosen strip (ra or rb)       */
    int           cand_r, cand_l;  /* the two walk candidates           */
    int           aim_strip_cached;/* FUN_0016F000 on the clear branch  */
    int           valid;
} B3AiAvoid;

/* --- the frame, FUN_00170260 -------------------------------------------
 * NAMING: `left` is retail's avoid+0x00 and `right` its avoid+0x10.  Retail
 * fills +0x00 from the node pair's SLOT-0 vertex and +0x10 from slot 1
 * (0x001702EA / 0x0017037E / 0x001703DB), so the caller must pass the pair in
 * SLOT ORDER -- which side of travel that lands on is the data's business,
 * not this function's.  See ai_avoid_frame_from_nav.                    [C] */
void b3_avoid_frame(B3AiAvoid* a, const float left[3], const float right[3],
                    const float car[3]);

/* --- the car's own footprint band, FUN_00170100 -------------------------
 * `ext_w` is racecar+0x2444 (the car WIDTH) and `ext_l` racecar+0x2448 (the
 * LENGTH); the ARGUMENT ORDER is load-bearing, see the note over the
 * definition.                                                          [C] */
void b3_avoid_band(B3AiAvoid* a, const float road_fwd[3],
                   const float road_right[3], const float car_fwd[3],
                   float ext_w, float ext_l);

/* --- reset, FUN_0016D2F0 @0x0016D32F ------------------------------------ */
void b3_avoid_clear(B3AiAvoid* a);

/* --- world -> strip, FUN_0016F400's index map --------------------------- */
int  b3_avoid_strip(const B3AiAvoid* a, const float p[3]);

/* --- paint one span, FUN_0016F400 --------------------------------------- */
void b3_avoid_paint_span(B3AiAvoid* a, int lo, int hi, int kind,
                         float t, float d);
/* the cross-section form: two world points (the left and right extent) */
void b3_avoid_paint(B3AiAvoid* a, const float pl[3], const float pr[3],
                    int kind, float t, float d);

/* --- the risk query at a strip, FUN_0016FCD0 ---------------------------- */
void b3_avoid_query(const B3AiAvoid* a, int strip, float* mean,
                    float* total, float* maxr);

/* --- the clear-path aim strip, FUN_0016F000 ----------------------------- */
int  b3_avoid_aim_strip(const B3AiAvoid* a);

/* --- the chooser, FUN_0016C4B0 ------------------------------------------ */
void b3_avoid_choose(B3AiAvoid* a, const B3AiAvoidParams* p,
                     float corner_ceiling, int mode);

/* helpers exposed for the differential probe */
float b3_avoid_time(const B3AiAvoid* a, int i);
float b3_avoid_dist(const B3AiAvoid* a, int i);

#endif /* BURNOUT3_AI_AVOID_H */
