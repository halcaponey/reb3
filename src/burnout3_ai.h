#ifndef BURNOUT3_AI_H
#define BURNOUT3_AI_H

#include <stddef.h>

/* B3AiCar points at the physics vehicle instead of duplicating ten of its
 * fields; the driver dereferences it, so consumers need the full type. */
#include "burnout3_vehicle_sim.h"
/* The AI DRIVER: the real control law that steers/throttles/brakes an AI race
 * car, RE'd from the retail XBE, replacing the harness's line-follower GLUE.
 *
 * OWNERSHIP: this module (both files) belongs to the AI/control-audit agent.
 * The harness calls only this contract; burnout3_full.c call sites are patched
 * by the orchestrator.  Integration hunks: scratchpad/ai/integration_ai.md.
 *
 * ---------------------------------------------------------------------------
 * WHAT WAS RECOVERED (docs/RE_AI.md sections 8-13; every law below has a
 * differential case in tools/validate_ai.py that runs the REAL x86 under
 * Unicorn and asserts this C reproduces it bit-for-bit-ish (1e-6)).
 *
 * The key that unlocked the previously-"unlocated AI target writers": the AI
 * object is EMBEDDED IN the racecar at racecar+0x1A00.  The navigator ctor
 * FUN_001705F0 writes the aggregate back-pointers *(rc+0x1A04)=rc,
 * *(rc+0x21A0)=rc, *(rc+0x2160)=rc -- i.e. the `this+0x004`, `this+0x7A0`,
 * `this+0x760` slots that FUN_00171E30 / FUN_00171A10 / FUN_0016AAC0
 * dereference.  Therefore
 *      racecar+0x23C0 (target steering angle)  ==  AI+0x9C0
 *      racecar+0x23C4 (target speed)           ==  AI+0x9C4
 * and both ARE written by name, in FUN_00171E30 and FUN_001724F0.  There were
 * never any hidden derived-pointer writes; the search had simply been for the
 * wrong base.
 *
 * Per-frame chain (FUN_00170820 -> FUN_00171A10), all ported here:
 *   FUN_00173690   speed cap             -> AI+0xA08     b3_ai_speed_cap
 *   FUN_0016AAC0   arbitrator            -> AI+0x770/0x780/0x784
 *   FUN_0016AE20     "target wins" leg   -> dir + time-to-target
 *                                                        b3_ai_commit_target
 *   snapshot/normalize                   -> AI+0x7B0, AI+0x9C8
 *                                                        b3_ai_frame_snapshot
 *   FUN_00171E30   steering demand       -> AI+0x9C0     b3_ai_target_angle
 *   FUN_001724F0   speed demand          -> AI+0x9C4     b3_ai_target_speed
 *     FUN_00172E80   corner-speed law                    b3_ai_corner_speed
 *   FUN_00105340   the driver (inputs)                   b3_ai_drive
 *     FUN_00104CA0   brake helper                        b3_ai_brake
 *   0x00171078     out-of-range governor                 b3_ai_oor_governor
 *
 * NOT RECOVERED (the wall, documented precisely in docs/RE_AI.md section 12):
 * the aim POINT itself (AI+0x180 / AI+0x200) comes from the nav-graph walk in
 * FUN_00175B10 over the .bgd road network (node link tables + the 16-byte
 * point pool at DAT_0073A174), which the harness does not load in that form.
 * The caller therefore supplies the aim point; everything downstream of it is
 * the game's own arithmetic.
 * ---------------------------------------------------------------------------
 */

/* ===== AI config -- registrar FUN_0016AFD0, static struct 0x0047A140 ===== */
typedef struct __attribute__((packed)) B3AiParams {
    // ---- RETAIL WINDOW 0x0000..0x0030: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    float                oor_speed_dec_rate;  /* +0x000 Out of range speed decrease rate  */
    float                oor_max_dir_deg;  /* +0x004 Max desDir angle change OOR       */
    float                angle_min_speed_deg;  /* +0x008 Angle you want min spd at         */
    float                top_speed_mps;  /* +0x00C Top speed mps                     */
    float                min_speed_mps;  /* +0x010 Min speed mps                     */
    float                car_at_weight;  /* +0x014 How much carAt affects steering   */
    float                drift_start_deg;  /* +0x018 Angle at which drift is started   */
    float                max_lock_deg;  /* +0x01C Max lock at 180 x degrees         */
    float                drift_max_lock_deg;  /* +0x020 Drift Max lock at 180 x degrees   */
    unsigned char _pad00[0xC];

    // ---- HARNESS SIDE, past the retail window: no recovered
    // offset in THIS object, so it must not squat on retail's bytes.
    float                avoid_speed_10m;  
    float                avoid_speed_20m;  
    float                avoid_speed_30m;  
    float                brake_dist_factor;  
} B3AiParams;

#define B3AIPARAMS_RETAIL_SPAN 0x0030u
_Static_assert(offsetof(B3AiParams, oor_speed_dec_rate) == 0x0000, "oor_speed_dec_rate off retail");
_Static_assert(offsetof(B3AiParams, oor_max_dir_deg) == 0x0004, "oor_max_dir_deg off retail");
_Static_assert(offsetof(B3AiParams, angle_min_speed_deg) == 0x0008, "angle_min_speed_deg off retail");
_Static_assert(offsetof(B3AiParams, top_speed_mps) == 0x000C, "top_speed_mps off retail");
_Static_assert(offsetof(B3AiParams, min_speed_mps) == 0x0010, "min_speed_mps off retail");
_Static_assert(offsetof(B3AiParams, car_at_weight) == 0x0014, "car_at_weight off retail");
_Static_assert(offsetof(B3AiParams, drift_start_deg) == 0x0018, "drift_start_deg off retail");
_Static_assert(offsetof(B3AiParams, max_lock_deg) == 0x001C, "max_lock_deg off retail");
_Static_assert(offsetof(B3AiParams, drift_max_lock_deg) == 0x0020, "drift_max_lock_deg off retail");

extern B3AiParams b3_ai_params;      /* VDB-tuned values after b3_ai_init() */

void b3_ai_init(void);               /* load the retail Data/vdb.xml column */

/* ===== per-frame view of the car (filled by the caller) ================== */
/* packed pins the retail offsets; aligned(4) pins only the struct BASE.
 * No member moves (the offsetof asserts below prove it) -- but with a
 * 4-aligned base the 4-byte fields ARE 4-aligned, so taking their address
 * is no longer undefined and -Waddress-of-packed-member goes quiet on the
 * fields that really are aligned (and still fires on any that are not). */
typedef struct __attribute__((packed, aligned(4))) B3AiCar {
    // ---- RETAIL WINDOW 0x0000..0x2460: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0x10];
    float                right[3];  /* racecar +0x10 */
    unsigned char _pad01[0x14];
    float                fwd[3];  /* racecar +0x30 */
    unsigned char _pad02[0x4];
    float                pos[3];  /* racecar +0x40 */
    unsigned char _pad03[0x11A2];
    unsigned char        boosting;  /* racecar byte +0x11EE (boost record burning) */
    unsigned char        commit_boost;  /* racecar byte +0x11EF (min-burn latch), WRITTEN */
    unsigned char _pad04[0x1];
    unsigned char        boost_ramp_done;  /* racecar byte +0x11F1                        */
    unsigned char _pad05[0x15A];
    int                  traffic_class;  /* racecar +0x134C == 0                        */
    unsigned char _pad06[0x5BC];
    float                crash_timer;  /* racecar +0x190C (>= 0.5 => not "stuck")     */
    unsigned char _pad07[0x10];
    int                  race_mode;  /* racecar +0x1920 (1 = normal racing)         */
    unsigned char _pad08[0x864];
    int                  attack_active;  /* racecar byte +0x2188  == drift_enable       */
    unsigned char _pad09[0x4];
    int                  brake_suppressed;  /* rc+0x2190 != 0 && rc+0x2189 (bVar3)         */
    unsigned char _pad0A[0x264];
    int                  ooc_mode;  /* racecar +0x23F8 (0/1/2)                     */
    unsigned char _pad0B[0x17];
    unsigned char        attack_left;  /* +0x2413               == drift_left         */
    unsigned char        attack_right;  /* +0x2414               == drift_right        */
    unsigned char        attack_commit;  /* +0x2415               == drift_commit       */
    unsigned char _pad0C[0x3];
    int                  wants_boost;  /* +0x2419  (FUN_00171D90's latch)             */
    unsigned char _pad0D[0x33];
    int                  free_speed_floor;  /* racecar +0x2450 == 1 => no min-speed floor  */
    unsigned char _pad0E[0xC];

    // ---- HARNESS SIDE, past the retail window: no recovered
    // offset in THIS object, so it must not squat on retail's bytes.
    int                  engage_boost;  /* call the verified FUN_0017A5B0 gate         */
    int                  ooc_window;  /* inside the slam + aggressor window          */
    int                  ooc_countersteer;  /* !FUN_00198190(): throw opposite lock        */
    float                clock;  /* DAT_0060EA20 race clock                     */
    struct B3VehicleFull *veh;
} B3AiCar;

#define B3AICAR_RETAIL_SPAN 0x2460u
_Static_assert(offsetof(B3AiCar, right) == 0x0010, "right off retail");
_Static_assert(offsetof(B3AiCar, fwd) == 0x0030, "fwd off retail");
_Static_assert(offsetof(B3AiCar, pos) == 0x0040, "pos off retail");
_Static_assert(offsetof(B3AiCar, boosting) == 0x11EE, "boosting off retail");
_Static_assert(offsetof(B3AiCar, commit_boost) == 0x11EF, "commit_boost off retail");
_Static_assert(offsetof(B3AiCar, boost_ramp_done) == 0x11F1, "boost_ramp_done off retail");
_Static_assert(offsetof(B3AiCar, traffic_class) == 0x134C, "traffic_class off retail");
_Static_assert(offsetof(B3AiCar, crash_timer) == 0x190C, "crash_timer off retail");
_Static_assert(offsetof(B3AiCar, race_mode) == 0x1920, "race_mode off retail");
_Static_assert(offsetof(B3AiCar, attack_active) == 0x2188, "attack_active off retail");
_Static_assert(offsetof(B3AiCar, brake_suppressed) == 0x2190, "brake_suppressed off retail");
_Static_assert(offsetof(B3AiCar, ooc_mode) == 0x23F8, "ooc_mode off retail");
_Static_assert(offsetof(B3AiCar, attack_left) == 0x2413, "attack_left off retail");
_Static_assert(offsetof(B3AiCar, attack_right) == 0x2414, "attack_right off retail");
_Static_assert(offsetof(B3AiCar, attack_commit) == 0x2415, "attack_commit off retail");
_Static_assert(offsetof(B3AiCar, wants_boost) == 0x2419, "wants_boost off retail");
_Static_assert(offsetof(B3AiCar, free_speed_floor) == 0x2450, "free_speed_floor off retail");




/* ===== persistent AI-object state (one per AI car) ====================== */
/* packed pins the retail offsets; aligned(4) pins only the struct BASE.
 * No member moves (the offsetof asserts below prove it) -- but with a
 * 4-aligned base the 4-byte fields ARE 4-aligned, so taking their address
 * is no longer undefined and -Waddress-of-packed-member goes quiet on the
 * fields that really are aligned (and still fires on any that are not). */
typedef struct __attribute__((packed, aligned(4))) B3AiState {
    // ---- RETAIL WINDOW 0x0000..0x1600: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0x1F8];
    int                  target_mode;  /* AI +0x1F8                                   */
    unsigned char _pad01[0x17];
    int                  boost_scale_flag;  /* AI byte +0x213 -> x1.45 corner speed        */
    unsigned char _pad02[0x559];
    float                des_dir[4];  /* AI +0x770 arbitrated desired direction      */
    float                max_speed;  /* AI +0x780 arbitrated speed ceiling          */
    float                time_to_target;  /* AI +0x784 dist/speed, seconds               */
    unsigned char _pad03[0x28];
    float                des_dir_n[4];  /* AI +0x7B0 normalized snapshot (this frame)  */
    unsigned char _pad04[0x200];
    float                target_angle;  /* AI +0x9C0 target steering angle, DEGREES    */
    float                target_speed;  /* AI +0x9C4 target speed, m/s                 */
    float                t2t_snap;  /* AI +0x9C8                                   */
    float                prev_angle;  /* AI +0x9CC slew-limiter memory               */
    float                corner_speed;  /* AI +0x9D0                                   */
    float                steer_err;  /* AI +0x9D4 yaw-rate error (drift path)       */
    unsigned char _pad05[0x4];
    float                catchup_bonus;  /* AI +0x9DC FUN_001734C0's warp speed (m/s)   */
    float                pace_aggression;  /* AI +0x9E0 = AI+0x9EC (pace rec +0x90)      */
    float                pace_e4;  /* AI +0x9E4 (pace rec +0x97)                 */
    float                catchup_window;  /* AI +0x9E8 (pace rec +0x93) race fraction   */
    float                pace_ec;  /* AI +0x9EC (pace rec +0x90)                 */
    float                pace_f0;  /* AI +0x9F0 (pace rec +0x92)                 */
    float                pace_f4;  /* AI +0x9F4 (pace rec +0x91)                 */
    int                  pace_mode;  /* AI +0x9F8 (pace rec +0x95, signed char)    */
    unsigned char _pad06[0x8];
    float                pace_a04;  /* AI +0xA04 cleared when catch-up expires     */
    float                speed_cap;  /* AI +0xA08 hard cap (FUN_00173690)           */
    float                catchup_gate;  /* AI +0xA0C race-clock gate, init -1.0        */
    unsigned char _pad07[0x21];
    unsigned char        catchup_expired;  /* AI +0xA31 one-shot latch (FUN_001734C0)     */
    unsigned char _pad08[0xBCE];
} B3AiState;

#define B3AISTATE_RETAIL_SPAN 0x1600u
_Static_assert(offsetof(B3AiState, target_mode) == 0x01F8, "target_mode off retail");
_Static_assert(offsetof(B3AiState, boost_scale_flag) == 0x0213, "boost_scale_flag off retail");
_Static_assert(offsetof(B3AiState, des_dir) == 0x0770, "des_dir off retail");
_Static_assert(offsetof(B3AiState, max_speed) == 0x0780, "max_speed off retail");
_Static_assert(offsetof(B3AiState, time_to_target) == 0x0784, "time_to_target off retail");
_Static_assert(offsetof(B3AiState, des_dir_n) == 0x07B0, "des_dir_n off retail");
_Static_assert(offsetof(B3AiState, target_angle) == 0x09C0, "target_angle off retail");
_Static_assert(offsetof(B3AiState, target_speed) == 0x09C4, "target_speed off retail");
_Static_assert(offsetof(B3AiState, t2t_snap) == 0x09C8, "t2t_snap off retail");
_Static_assert(offsetof(B3AiState, prev_angle) == 0x09CC, "prev_angle off retail");
_Static_assert(offsetof(B3AiState, corner_speed) == 0x09D0, "corner_speed off retail");
_Static_assert(offsetof(B3AiState, steer_err) == 0x09D4, "steer_err off retail");
_Static_assert(offsetof(B3AiState, catchup_bonus) == 0x09DC, "catchup_bonus off retail");
_Static_assert(offsetof(B3AiState, pace_aggression) == 0x09E0, "pace_aggression off retail");
_Static_assert(offsetof(B3AiState, pace_e4) == 0x09E4, "pace_e4 off retail");
_Static_assert(offsetof(B3AiState, catchup_window) == 0x09E8, "catchup_window off retail");
_Static_assert(offsetof(B3AiState, pace_ec) == 0x09EC, "pace_ec off retail");
_Static_assert(offsetof(B3AiState, pace_f0) == 0x09F0, "pace_f0 off retail");
_Static_assert(offsetof(B3AiState, pace_f4) == 0x09F4, "pace_f4 off retail");
_Static_assert(offsetof(B3AiState, pace_mode) == 0x09F8, "pace_mode off retail");
_Static_assert(offsetof(B3AiState, pace_a04) == 0x0A04, "pace_a04 off retail");
_Static_assert(offsetof(B3AiState, speed_cap) == 0x0A08, "speed_cap off retail");
_Static_assert(offsetof(B3AiState, catchup_gate) == 0x0A0C, "catchup_gate off retail");
_Static_assert(offsetof(B3AiState, catchup_expired) == 0x0A31, "catchup_expired off retail");
_Static_assert(sizeof(B3AiState) == B3AISTATE_RETAIL_SPAN, "B3AiState span");


/* ===== driver outputs (the physics vehicle's input block) =============== */
/* B3AiInputs is GONE. It was an object retail does not have.
 *
 * FUN_00105340 writes its results straight into the physics vehicle -- the
 * throttle at v+0x1400, brake at +0x1404, steer at +0x1408, the gear at
 * +0x14C8 -- which is why the sidecar reads them back from VEH+off after the
 * call. The port invented a struct to receive them, laid it out at those
 * VEHICLE offsets, and then put ONE racecar field (the +0x11EF boost latch)
 * inside it, so the transfer sent that byte to VEH+0x11EF: the wrong object,
 * the same defect B3AiCar had. Every other field duplicated a B3VehicleFull
 * field at an identical offset.
 *
 * The driver now writes the vehicle it is given, through B3AiCar's own veh
 * pointer, and the +0x11EF latch lives in the racecar view where it belongs.
 */



void b3_ai_vehicle_state_init(B3VehicleFull* veh);
void b3_ai_state_init(B3AiState* s, B3VehicleFull* veh);

/* --- individual stages (each has its own differential case) ------------- */

/* FUN_0016AE20: desired direction := normalize(target_point - car pos);
 * time-to-target := |d| / speed  (or |d| when speed <= 1).  Returns |d|. */
float b3_ai_commit_target(B3AiState* s, const B3AiCar* c,
                          const float target_point[3]);

/* FUN_00171A10 head: AI+0x7B0 := normalize(AI+0x770); AI+0x9C8 := AI+0x784 */
void b3_ai_frame_snapshot(B3AiState* s);

/* FUN_00171E30 -> AI+0x9C0 target steering angle (degrees) + AI+0x9D4 */
void b3_ai_target_angle(B3AiState* s, const B3AiCar* c);

/* FUN_00172E80 corner-speed law (returns the speed; pure) */
float b3_ai_corner_speed(const B3AiState* s, const B3AiCar* c,
                         float max_speed);

/* FUN_001724F0 -> AI+0x9C4 target speed (m/s).  `catchup_bonus` is
 * FUN_001734C0's output (0 when the catch-up branch is inactive). */
void b3_ai_target_speed(B3AiState* s, const B3AiCar* c, float catchup_bonus);

/* ======================================================================== */
/* THE RUBBER BAND -- FUN_00106370 / FUN_001734C0 / FUN_00173690.           */
/*                                                                          */
/* Retail runs three player-relative laws this port had never wired.  All   */
/* three hang off ONE per-vehicle block that FUN_00106370 refreshes at the  */
/* top of the per-car update FUN_00104A90 (@0x00104AA6):                    */
/*                                                                          */
/*   vehicle +0x1554  the human player this car is paired with              */
/*                    (`ncars == 1 ? 0 : grid_slot & 1` -- split screen)    */
/*   vehicle +0x1558  1 = I AM the player, 2 = I am AHEAD of my player,     */
/*                    3 = I am BEHIND my player.  The 2/3 split is          */
/*                    FUN_00194200(me+0x10D0, player+0x10D0) @0x00106407,   */
/*                    the lap / checkpoint / distance / grid comparator.    */
/*   vehicle +0x155C  clamp((dist2_to_player - 1600) / 18000, 0, 1)         */
/*                    @0x0010642E..0x00106493 -- 0 at 40 m, 1 at 140 m.     */
/*                                                                          */
/* and on the per-opponent PACE RECORD FUN_00172870 reads out of the .bgd   */
/* event param block at `param + grid_slot*0x98` (tools/gen_ai_pace.py ->   */
/* src/burnout3_ai_pace.h).  Byte +0x93 * 0.01 is AI+0x9E8, the fraction of */
/* the race for which catch-up stays armed.  docs/RE_AI.md section 17.      */
/* ======================================================================== */

#define B3AI_RANK_IS_PLAYER        1   /* vehicle +0x1558 == 1 */
#define B3AI_RANK_AHEAD_OF_PLAYER  2   /* vehicle +0x1558 == 2 */
#define B3AI_RANK_BEHIND_PLAYER    3   /* vehicle +0x1558 == 3 */

/* FUN_00106370 @0x00106370.  `ahead_of_player` is FUN_00194200's result
 * (1 = this car outranks its player).  For the human car itself pass
 * race_mode 0 and the +0x1558 = 1 / +0x155C = 0 answer comes straight back. */
void b3_ai_player_rel(int race_mode, int ncars, int grid_slot,
                      int ahead_of_player, float dist2_to_player,
                      int* player_slot, int* rank_1558, float* gap_155c);

/* Everything FUN_001734C0 and FUN_00173690 read, in retail's own terms. */
typedef struct B3AiCatchupIn {
    /* the race-progress fraction, @0x00173516..0x00173538:
     *   (FUN_00194380(rc+0x10D0) - rc+0x135C) / (rc+0x1394 * track_length) */
    float       race_fraction;
    int         dist_valid;    /* rc+0x135C != -1.0 && rc+0x1394 >= 1        */
    int         mode_2450;     /* racecar +0x2450 (1 = skip the place test)  */
    int         veh_valid;     /* racecar +0x2440 != 0                       */
    int         veh_in_range;  /* vehicle byte +0x1550 (fully simulated)     */
    int         veh_rank;      /* vehicle +0x1558, see B3AI_RANK_*           */
    float       veh_gap_norm;  /* vehicle +0x155C, 0..1                      */
    int         my_place;      /* (i16) racecar +0x10D0, 1-based             */
    int         place_window;  /* DAT_003A29EC[DAT_0073BB50] -- always 1     */
    const int*  place;         /* (i16) +0x10D0 of every car (DAT_0073A1A8)  */
    const int*  race_mode;     /* +0x1920 of every car (0 = the human)       */
    int         ncars;         /* DAT_0073A19C                               */
    float       race_clock;    /* racecar +0x10DC, for the AI+0xA0C gate      */
} B3AiCatchupIn;

/* FUN_001734C0 @0x001734C0.  Returns the new AI+0x9C4 target speed and writes
 * AI+0x9DC (the off-camera warp speed) and the AI+0xA31 expiry latch. */
float b3_ai_catchup(B3AiState* s, const B3AiCatchupIn* in, float spd);

/* FUN_001724F0's racer branch with the catch-up in retail's own position:
 * the corner-law speed, then FUN_00172FA0 (identity while the attack machine
 * is idle), then FUN_001734C0 behind its two gates.  Prefer this over calling
 * b3_ai_target_speed() twice -- b3_ai_target_angle()'s 2.4/8.1 deg per frame
 * slew limiter is STATEFUL in AI+0x9CC, so running the chain twice in one
 * frame slews the wheel twice. */
void b3_ai_target_speed_rb(B3AiState* s, const B3AiCar* c,
                           const B3AiCatchupIn* rb);

/* FUN_00173690 @0x00173690 -> AI+0xA08.  `section_min` / `section_max` are
 * FUN_00172BC0 / FUN_00172D20; with no per-section factor table loaded they
 * return "Min speed mps" (@0x00172D05) and "Top speed mps" (@0x00172E65). */
void b3_ai_speed_cap(B3AiState* s, const B3AiCatchupIn* in,
                     float player_speed, float section_min, float section_max);

/* FUN_00104CA0: route a brake amount to brake or (in reverse) throttle */
void b3_ai_brake(B3AiState* s, B3AiCar* c,
                 float amount);

/* FUN_00105340: the driver.  `reverse_aim_dot` is FUN_000FF0E0's
 * clamp(dot(racecar+0x18E0, car right), -1, 1) used only while reversing. */
void b3_ai_drive(B3AiState* s, B3AiCar* c,
                 float dt, float reverse_aim_dot);

/* The chain WITHOUT the drive, so a caller that overrides the speed demand
 * can do it before the one driver call retail makes per frame. */
void b3_ai_plan(B3AiState* s, B3AiCar* c,
                const float target_point[3], float arbitrated_max_speed,
                float catchup_bonus);

/* Same, but with AI+0xA08 supplied by the caller.  b3_ai_plan() pins the cap
 * to "Top speed mps", which is only FUN_00173690's catch-up-still-armed arm;
 * a caller that runs b3_ai_speed_cap() first passes the real answer here. */
void b3_ai_plan_ex(B3AiState* s, B3AiCar* c,
                   const float target_point[3], float arbitrated_max_speed,
                   float catchup_bonus, float speed_cap);

/* The whole chain ONCE with the rubber band in retail's position. */
void b3_ai_plan_rb(B3AiState* s, B3AiCar* c,
                   const float target_point[3], float arbitrated_max_speed,
                   float speed_cap, const B3AiCatchupIn* rb);

/* Whole chain in one call: aim point + arbitrated ceiling in, inputs out. */
void b3_ai_update(B3AiState* s, B3AiCar* c,
                  const float target_point[3], float arbitrated_max_speed,
                  float catchup_bonus, float dt);

/* 0x00171078 (tail of the out-of-range mover FUN_00170B30): the speed
 * governor.  Returns the commanded speed for FUN_001204C0. */
float b3_ai_oor_governor(float speed_ms, float speed_mph,
                         float target_speed_ms);

/* FUN_00104D30: the driver DISPATCHER -- retail's real per-frame entry.
 * Clears the stop flag, dispatches on racecar+0x134C (0 = traffic), drops the
 * steer below 0.1 m/s, and DERIVES a racer's v+0x1400 from v+0x1414 * v+0x13BC
 * clamped to 1.  Call this, not b3_ai_drive, from a per-frame step. */
void b3_ai_dispatch(B3AiState* s, B3AiCar* c, float dt, float reverse_aim_dot,
                    int ai_enable);

/* FUN_00105150 (traffic driver) steering + speed band; the traffic cars use
 * the same MaxLock/180 conversion but a 5 mph brake excess. */
void b3_ai_traffic_drive(B3AiCar* c,
                         float target_angle_deg, float target_speed_ms);

/* Avoidance close-range speed caps (AI/Avoidance, [C] params / [S] use) */
float b3_ai_avoid_speed_cap(float speed, float dist_ahead_m);

/* =========================================================================
 * THE AGGRESSION (ATTACK / SLAM) STATE MACHINE  -- docs/RE_AI.md section 14
 *
 * Object: a sub-object of the AI object at AI+0x170 == racecar+0x1B70.
 * Proof: FUN_00175A10 @0x00175A1F does `LEA ECX,[ESI+0x170]; CALL 0x00169490`
 * with ESI = the AI/target-follower base, and FUN_0016AF10 reads exactly the
 * same bytes through the racecar base (AI+0x191/+0x192/+0x1B8 == aggro+0x21/
 * +0x22/+0x48).
 *
 * How it reaches the wheels -- three separate channels, all recovered:
 *   1. STEERING.  The machine writes an aim point into aggro+0x10 and a
 *      validity byte into aggro+0x20.  FUN_0016AE20 (@0x0016AE2C) tests that
 *      byte and, when set, builds the desired direction from the aggression
 *      aim INSTEAD of the racing line.  Everything downstream (the 2.4/8.1
 *      slew limiter, MaxLock/180) is the ordinary driver.  This is what
 *      manufactures the lateral closing speed a full slam needs: state 3
 *      aims 5 m to the OUTSIDE (`Steer out distance`), then state 4 aims
 *      dead at the victim.
 *   2. SPEED.  FUN_0016AF10 turns aggro+0x21/+0x22/+0x48 into AI+0x790 (mode
 *      4 = close in, 5 = lock to the target's speed) and AI+0x794 (target),
 *      which FUN_00172FA0 consumes inside FUN_001724F0.
 *   3. BOOST.  FUN_00172FA0 arms AI+0xA17/+0xA2C, FUN_00171D90 latches
 *      AI+0xA19 = the driver's wants-boost.
 *
 * Every field carries its retail offset.  Ported functions:
 *   FUN_00169490  ctor/reset                b3_aggro_init
 *   FUN_00169540  the state machine         b3_aggro_update
 *   FUN_00169BD0  target selection          (internal)
 *   FUN_00169D70  can-slam predicate        (internal, -> can_slam)
 *   FUN_00169E80  state 4 tick              (internal)
 *   FUN_0016A0A0  positioning aim           (internal)
 *   FUN_0016A310  reset to idle             (internal)
 *   FUN_0016A360  aim-valid gate            (internal)
 *   FUN_0016A3E0  block-range predicate     (internal, -> block_range)
 *   FUN_0016A4E0  block aim                 (internal)
 *   FUN_0016A620  slam-speed predicate      (internal, -> want_slam_speed)
 *   FUN_0016A7D0  per-frame measure         (internal)
 *   FUN_0016A8C0  rubbed-blind timer        (internal)
 *   FUN_0016A950  rival retaliation         (internal)
 *   FUN_001716D0  |lateral offset|          (internal)
 *   FUN_001717B0  signed longitudinal gap   (internal)
 *   FUN_0016AF10  -> AI+0x790 / AI+0x794    b3_aggro_arbitrate
 *   FUN_00172FA0  aggression speed demand   b3_ai_aggro_speed
 *   FUN_00171D90  boost latch               b3_ai_boost_latch
 *   FUN_00171BE0  drift-lock flags          b3_ai_drift_flags
 * ========================================================================= */

/* AI/Aggressive Driving + .../Slam config (registrar FUN_0016AFD0, the
 * 0x0047A204..0x0047A25C block; defaults -> retail Data/vdb.xml column). */
typedef struct __attribute__((packed)) B3AiAggroParams {
    // ---- RETAIL WINDOW 0x0000..0x0130: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0xB8];
    float                close_match_m;  /* +0x0B8 Driver: how close to start matching   */
    float                sticky_dist_m;  /* +0x0BC Driver: max dist, sticky matching     */
    float                sticky_mph;  /* +0x0C0 Driver: max speed diff, sticky match  */
    float                min_aggression;  /* +0x0C4 Min. aggression before attacking      */
    float                min_wait_s;  /* +0x0C8 Min. time between attacks   (UNUSED)  */
    float                max_wait_s;  /* +0x0CC Max. time between attacks             */
    float                dist_ahead_m;  /* +0x0D0 Max dist apart to begin, when ahead   */
    float                dist_behind_m;  /* +0x0D4 Max dist apart to begin, when behind  */
    float                min_target_mph;  /* +0x0D8 Min. target speed to consider         */
    float                slow_factor;  /* +0x0DC How much slower while speed matching  */
    float                boost_dist_m;  /* +0x0E0 How far in front to boost             */
    float                boost_aggro_m;  /* +0x0E4 Extra dist to boost vs aggression     */
    float                start_delay_s;  /* +0x0E8 Wait at race start                    */
    float                immunity_s;  /* +0x0EC How long after hitting something      */
    float                block_min_s;  /* +0x0F0 Slam: min time to block               */
    float                block_max_s;  /* +0x0F4 Slam: max time to block               */
    float                block_dist_m;  /* +0x0F8 Slam: max distance ahead to block     */
    float                separation_m;  /* +0x0FC Slam: preferred separation  (UNUSED)  */
    float                position_time_s;  /* +0x100 Slam: max time to get into position   */
    float                ahead_gap_m;  /* +0x104 Slam: max distance apart, when ahead  */
    float                speed_diff_mph;  /* +0x108 Slam: max difference in speeds        */
    float                steer_out_m;  /* +0x10C Slam: steer out distance              */
    float                steer_out_s;  /* +0x110 Slam: steer out time                  */
    float                slam_s;  /* +0x114 Slam: slam time                       */
    float                max_cos_off_lane;  /* +0x118 Slam: max cos angle off lane          */
    float                commit_s;  /* +0x11C Slam: committed after (UNUSED here)   */
    unsigned char _pad01[0x10];
} B3AiAggroParams;

#define B3AIAGGROPARAMS_RETAIL_SPAN 0x0130u
_Static_assert(offsetof(B3AiAggroParams, close_match_m) == 0x00B8, "close_match_m off retail");
_Static_assert(offsetof(B3AiAggroParams, sticky_dist_m) == 0x00BC, "sticky_dist_m off retail");
_Static_assert(offsetof(B3AiAggroParams, sticky_mph) == 0x00C0, "sticky_mph off retail");
_Static_assert(offsetof(B3AiAggroParams, min_aggression) == 0x00C4, "min_aggression off retail");
_Static_assert(offsetof(B3AiAggroParams, min_wait_s) == 0x00C8, "min_wait_s off retail");
_Static_assert(offsetof(B3AiAggroParams, max_wait_s) == 0x00CC, "max_wait_s off retail");
_Static_assert(offsetof(B3AiAggroParams, dist_ahead_m) == 0x00D0, "dist_ahead_m off retail");
_Static_assert(offsetof(B3AiAggroParams, dist_behind_m) == 0x00D4, "dist_behind_m off retail");
_Static_assert(offsetof(B3AiAggroParams, min_target_mph) == 0x00D8, "min_target_mph off retail");
_Static_assert(offsetof(B3AiAggroParams, slow_factor) == 0x00DC, "slow_factor off retail");
_Static_assert(offsetof(B3AiAggroParams, boost_dist_m) == 0x00E0, "boost_dist_m off retail");
_Static_assert(offsetof(B3AiAggroParams, boost_aggro_m) == 0x00E4, "boost_aggro_m off retail");
_Static_assert(offsetof(B3AiAggroParams, start_delay_s) == 0x00E8, "start_delay_s off retail");
_Static_assert(offsetof(B3AiAggroParams, immunity_s) == 0x00EC, "immunity_s off retail");
_Static_assert(offsetof(B3AiAggroParams, block_min_s) == 0x00F0, "block_min_s off retail");
_Static_assert(offsetof(B3AiAggroParams, block_max_s) == 0x00F4, "block_max_s off retail");
_Static_assert(offsetof(B3AiAggroParams, block_dist_m) == 0x00F8, "block_dist_m off retail");
_Static_assert(offsetof(B3AiAggroParams, separation_m) == 0x00FC, "separation_m off retail");
_Static_assert(offsetof(B3AiAggroParams, position_time_s) == 0x0100, "position_time_s off retail");
_Static_assert(offsetof(B3AiAggroParams, ahead_gap_m) == 0x0104, "ahead_gap_m off retail");
_Static_assert(offsetof(B3AiAggroParams, speed_diff_mph) == 0x0108, "speed_diff_mph off retail");
_Static_assert(offsetof(B3AiAggroParams, steer_out_m) == 0x010C, "steer_out_m off retail");
_Static_assert(offsetof(B3AiAggroParams, steer_out_s) == 0x0110, "steer_out_s off retail");
_Static_assert(offsetof(B3AiAggroParams, slam_s) == 0x0114, "slam_s off retail");
_Static_assert(offsetof(B3AiAggroParams, max_cos_off_lane) == 0x0118, "max_cos_off_lane off retail");
_Static_assert(offsetof(B3AiAggroParams, commit_s) == 0x011C, "commit_s off retail");

extern B3AiAggroParams b3_ai_aggro_params;

/* AI/Target +0x0A0 (0x0047A1E0) -- the drift-commit time threshold that
 * FUN_00171BE0 compares |aim - pos| / speed against.  Registered five times
 * onto the same slot by FUN_0016AFD0 (RE_AI section 1's note); the live
 * value is whatever bound last. */
extern float b3_ai_drift_apex_time;

/* --- the aggression object (AI+0x170) ---------------------------------- */
enum {
    B3_AGGRO_IDLE      = 0,  /* look for a target                           */
    B3_AGGRO_APPROACH  = 1,  /* get into slamming position (30 s)           */
    B3_AGGRO_RETRY     = 2,  /* 1 s re-arm after a failed attempt           */
    B3_AGGRO_STEER_OUT = 3,  /* pull `steer out distance` clear (0.5 s)     */
    B3_AGGRO_SLAM      = 4,  /* aim at the victim (0.75 s)                  */
    B3_AGGRO_COOLDOWN  = 5,  /* wait aggression x max_wait seconds          */
    B3_AGGRO_RECOIL    = 6,  /* steer away after contact (0.5 s)            */
    B3_AGGRO_BLOCK     = 7   /* sit in front of the target                  */
};

typedef struct {
    int   state;             /* +0x00                                       */
    float aim[4];            /* +0x10 attack aim point (world) == AI+0x180  */
    unsigned char aim_valid; /* +0x20 == AI+0x190: use `aim` this frame     */
    unsigned char attacking; /* +0x21 == AI+0x191: arbitrator speed gate    */
    unsigned char slam_speed;/* +0x22 == AI+0x192: mode 5 instead of 4      */
    unsigned char blocked;   /* +0x23 == AI+0x193: hit something recently   */
    unsigned char hit;       /* +0x24 == AI+0x194: contact, set externally  */
    float blind_time;        /* +0x4C rubbed-blind window (FUN_0016A8C0)    */
    float blind_phase;       /* +0x50                                       */
    unsigned int blind_bits; /* +0x54 rotating mask                         */
    unsigned char blind_arm; /* +0x58                                       */
    unsigned char blind_out; /* -> own racecar +0x18FD                      */
    float timer;             /* +0x2C state deadline (-1 = never)           */
    float entered;           /* +0x30 clock when the state was entered      */
    float side;              /* +0x34 committed side, +-1                   */
    float lateral;           /* +0x38 |lateral offset| to the target (m)    */
    float longitudinal;      /* +0x3C signed gap, + = target ahead (m)      */
    unsigned char can_slam;      /* +0x40 FUN_00169D70                      */
    unsigned char block_range;   /* +0x41 FUN_0016A3E0                      */
    unsigned char want_slam_speed;/*+0x42 FUN_0016A620                      */
    int   target;            /* +0x48 rival index, -1 = none                */
} B3AiAggro;

/* One car as the aggression machine sees it.  Slot `i` of the world array
 * corresponds to racecar `DAT_0073A1D0 + i*0x27E0` in retail. */
typedef struct {
    float pos[4];            /* racecar +0x40                               */
    float fwd[4];            /* racecar +0x30                               */
    float right[4];          /* racecar +0x10                               */
    float road_dir[4];       /* racecar +0x18E0 (road direction at the node)*/
    float speed_ms;          /* physics vehicle +0xBC                       */
    float track_dist;        /* FUN_00194380(racecar+0x10D0), metres        */
    float aggression;        /* racecar +0x23E0 (per-opponent, 0..1)        */
    float car_width;         /* racecar +0x2444                             */
    float car_length;        /* racecar +0x2448                             */
    float ooc_time;          /* (racecar+0x1198)+0x1598, -1 = never         */
    float slammed_time;      /* racecar +0x16C0 last time we were hit, -1   */
    int   slammed_by;        /* racecar +0x16BC aggressor index, -1 = none  */
    float race_time;         /* racecar +0x10DC                             */
    int   race_mode;         /* racecar +0x1920 (0 = player, 1 = AI racer)  */
    int   car_class;         /* racecar +0x134C (0 = traffic)               */
    int   mode2450;          /* racecar +0x2450                             */
    int   rival;             /* racecar +0x1650 slot index, -1 = none       */
    int   wrecked;           /* racecar +0x18FA                             */
    int   in_takedown;       /* racecar +0x27D8                             */
    int   boosting;          /* racecar +0x11EE                             */
    float boost_start;       /* racecar +0x11C0 (boost-record start time)   */
    int   race_progress_zero;/* FUN_00194430(this car) == 0                 */
    int   progress_gate;     /* racecar +0x1394 > 0 (checked on the ATTACKER)*/
    int   no_slam_speed;     /* racecar +0x2431                             */
    int   drift_zone;        /* racecar +0x1C12 (AI+0x212)                  */
    int   node_open;         /* node link byte +2 == 0xFF (no junction)     */
    int   steer_ok;          /* physics vehicle byte +0x1550                */
    int   player_slot;       /* physics vehicle byte +0x1554 (fallback)     */
    const float* lateral_to; /* racecar +0x18A4[k], per-slot lateral offset */
    const float* last_hit;   /* racecar +0x15E0[k], per-slot last-hit time  */
} B3AiAggroCar;

typedef struct {
    const B3AiAggroCar* cars;
    int   ncars;             /* DAT_0073A1C0                                */
    float clock;             /* DAT_0060EA20                                */
    float dt;                /* DAT_0060EA1C                                */
    int   track_loaded;      /* DAT_0073A164                                */
} B3AiAggroWorld;

void  b3_aggro_init(B3AiAggro* a, float clock);              /* FUN_00169490 */
void  b3_aggro_update(B3AiAggro* a, const B3AiAggroWorld* w, int self);

/* FUN_0016AF10: aggro flags -> AI+0x790 mode (0/4/5) + AI+0x794 target.
 * Returns the mode; *target receives the rival index (-1 when idle). */
int   b3_aggro_arbitrate(const B3AiAggro* a, const B3AiAggroWorld* w,
                         int self, int* target);

/* --- the speed leg: FUN_00172FA0's persistent state (AI+0x9D8..+0xA30) --- */
typedef struct __attribute__((packed)) B3AiAggroSpeed {
    // ---- RETAIL WINDOW 0x0000..0x1600: fields at the offsets the
    // game uses. Packed with explicit padding; asserted below.
    unsigned char _pad00[0x790];
    int                  mode;  /* AI+0x790  0 / 1 / 2 / 3 / 4 / 5             */
    int                  target;  /* AI+0x794  rival index, -1                   */
    unsigned char _pad01[0x240];
    float                matched;  /* AI+0x9D8                                    */
    unsigned char _pad02[0x3B];
    unsigned char        want_boost;  /* AI+0xA17                                    */
    unsigned char        boost_now;  /* AI+0xA18                                    */
    unsigned char        wants_boost;  /*AI+0xA19 -> the driver                      */
    unsigned char _pad03[0x2];
    int                  last_target;  /* AI+0xA1C                                    */
    float                last_own_speed;  /* AI+0xA20                                    */
    float                last_tgt_speed;  /* AI+0xA24                                    */
    float                boost_rearm;  /* AI+0xA28                                    */
    float                boost_until;  /* AI+0xA2C (-1 = disarmed)                    */
    unsigned char        acquired_ahead;  /* AI+0xA30                               */
    unsigned char _pad04[0xB3F];
    float                brake_hold_out;  /* the v+0x1570 write at 0x00173457            */
    unsigned char _pad05[0x8C];

    // ---- HARNESS SIDE, past the retail window: no recovered
    // offset in THIS object, so it must not squat on retail's bytes.
    int                  brake_hold_valid;  
} B3AiAggroSpeed;

#define B3AIAGGROSPEED_RETAIL_SPAN 0x1600u
_Static_assert(offsetof(B3AiAggroSpeed, mode) == 0x0790, "mode off retail");
_Static_assert(offsetof(B3AiAggroSpeed, target) == 0x0794, "target off retail");
_Static_assert(offsetof(B3AiAggroSpeed, matched) == 0x09D8, "matched off retail");
_Static_assert(offsetof(B3AiAggroSpeed, want_boost) == 0x0A17, "want_boost off retail");
_Static_assert(offsetof(B3AiAggroSpeed, boost_now) == 0x0A18, "boost_now off retail");
_Static_assert(offsetof(B3AiAggroSpeed, wants_boost) == 0x0A19, "wants_boost off retail");
_Static_assert(offsetof(B3AiAggroSpeed, last_target) == 0x0A1C, "last_target off retail");
_Static_assert(offsetof(B3AiAggroSpeed, last_own_speed) == 0x0A20, "last_own_speed off retail");
_Static_assert(offsetof(B3AiAggroSpeed, last_tgt_speed) == 0x0A24, "last_tgt_speed off retail");
_Static_assert(offsetof(B3AiAggroSpeed, boost_rearm) == 0x0A28, "boost_rearm off retail");
_Static_assert(offsetof(B3AiAggroSpeed, boost_until) == 0x0A2C, "boost_until off retail");
_Static_assert(offsetof(B3AiAggroSpeed, acquired_ahead) == 0x0A30, "acquired_ahead off retail");
_Static_assert(offsetof(B3AiAggroSpeed, brake_hold_out) == 0x1570, "brake_hold_out off retail");

void  b3_aggro_speed_init(B3AiAggroSpeed* s);
/* FUN_00172FA0.  `spd` is FUN_00172E80's corner speed; returns the demand. */
float b3_ai_aggro_speed(B3AiAggroSpeed* s, const B3AiAggroWorld* w, int self,
                        float aggression, float spd);
/* FUN_00171D90: AI+0xA17/+0xA18 -> AI+0xA19 (the driver's wants-boost). */
void  b3_ai_boost_latch(B3AiAggroSpeed* s, float clock);

/* FUN_00171BE0: the drift-lock flags racecar+0x2413/+0x2414/+0x2415.
 * `drift_enable` is racecar+0x2188, `mode` is AI+0x1F4, `aim` is AI+0x200. */
void  b3_ai_drift_flags(int drift_enable, int mode, float target_angle_deg,
                        const float aim[4], const float pos[4],
                        const float fwd[4], float speed_ms,
                        int* out_left, int* out_right, int* out_commit);

/* ======================================================================== */
/* section 16: the ROUTE DRIVER's wheel + watchdogs -- FUN_00170820          */
/*                                                                          */
/* FUN_00170820 is the racecar vtable's per-frame AI entry (vtable base      */
/* 0x003B1204, slot +0x04; slot +0x20 = FUN_00171650 the re-placer, slot     */
/* +0x24 = FUN_00170B30 the out-of-range mover).  Everything below is read   */
/* from its disassembly and the two helpers it owns.                         */
/* ======================================================================== */

/* --- the 50 s / 50 s route-alternation square wave, FUN_00170820 head ----
 * @0x00170827..0x0017089C.  Two countdowns at racecar+0x1BE4 / +0x1BE8
 * (= AI+0x1E4 / +0x1E8) alternately drive the flag racecar+0x1BF0
 * (= AI+0x1F0); each expiry reloads the OTHER countdown with
 * DAT_003B16B8 = 50.0.  AI+0x1F0 is the type-1/type-3 admissibility gate in
 * the selector span FUN_00178310 (RE_AI 12), so retail's junction policy
 * flips every fifty seconds.  FUN_00175A10 starts it at 1.  [C-disasm] */
#define B3_AI_ROUTE_ALT_S        50.0f   /* DAT_003B16B8                    */

/* --- FUN_001712E0, the off-world watchdog ------------------------------
 * dy = racecar+0x44 - bilinear(node's four point heights) - vehicle+0x870.
 *   dy > -1.0            -> latch racecar+0x245A = racecar+0x18D0, clear the
 *                           counter (this is the LAST VALID NODE);
 *   dy < -5.0            -> racecar+0x2460++, at 0x3D frames FUN_001714F0;
 *   otherwise            -> clear the counter.                       [C]   */
#define B3_AI_ROAD_LATCH_M        1.0f   /* the -1.0 comparison @0x0017142B */
#define B3_AI_OFFWORLD_DROP_M     5.0f   /* the -5.0 comparison @0x00171446 */
#define B3_AI_OFFWORLD_FRAMES     61     /* 0x3D @0x00171459                */

/* --- FUN_00170820 @0x001708EE, the stuck rescue -------------------------
 * `CMP dword [ESI+0x1904],0xC8 / JLE` then
 * `MOVZX EAX,word [ESI+0x245A]; SUB EAX,8; XOR ECX,ECX; CALL FUN_001714F0`
 * followed by `FUN_001204C0(v, DAT_0047A150 * 0x003A5958)` --
 * "Min speed mps" (VDB 20) x 0.44704 = 8.9408 m/s = exactly 20 mph.
 * racecar+0x1904 is NOT a slow-speed counter: an exhaustive disp32 sweep of
 * all fourteen executable PT_LOADs finds exactly four references --
 *   0018d6b8  mov  [ebx+0x1904],eax   FUN_0018D0E0 spawn init
 *   0018d840  inc  [ebx+0x1904]       FUN_0018D790, when FUN_00174960 (the
 *                                     nav-graph cursor walk) returns 0
 *   001708ee  cmp  [esi+0x1904],0xC8  this test
 *   00170926  mov  [esi+0x1904],ebx   cleared by the rescue
 * -- so it counts CONSECUTIVE NAV-WALK FAILURES, and the 5 mph rule of
 * RE_AI 3 is the DRIVER's own reverse burst, a different mechanism.  [C]  */
#define B3_AI_NAVFAIL_FRAMES     200     /* strictly greater than 0xC8      */
#define B3_AI_RESCUE_BACK_NODES    8     /* SUB EAX,0x8                     */
#define B3_AI_RESCUE_SPEED_MS  8.9408f   /* "Min speed mps" x 0.44704       */

typedef struct {
    int   ai_enable;       /* racecar +0x19A8  AI drives this car           */
    int   ai_wheel;        /* racecar +0x27D8  the cinematic wheel flag     */
    int   skip_once;       /* racecar +0x245E  skip ONE AI update           */
    int   navfail_frames;  /* racecar +0x1904                              */
    int   below_frames;    /* racecar +0x2460                              */
    int   last_node;       /* racecar +0x245A  last node at road height    */
    float road_dy;         /* racecar +0x244C  height above the road       */
    int   route_alt;       /* AI +0x1F0                                    */
    float alt_t[2];        /* AI +0x1E4 / +0x1E8                           */
} B3AiWheel;

/* FUN_001705F0 / FUN_0018D0E0's spawn values (+0x245E = 0, +0x2460 = 0,
 * +0x244C = 0, +0x245A = node, +0x1904 = 0) plus FUN_00175A10's +0x1F0 = 1. */
void b3_ai_wheel_init(B3AiWheel* w, int node);

/* FUN_00170820 @0x00170827: advance the square wave.  Returns the (possibly
 * flipped) AI+0x1F0. */
int  b3_ai_route_alt(B3AiWheel* w, float dt);

/* FUN_0018CB60(racecar, _, on) -- the ONLY writer of racecar+0x27D8 that
 * carries handover semantics.  On a rising edge it zeroes the second half of
 * the direction block (racecar+0x19D0..+0x19DC) and calls FUN_00179760, the
 * navigator reset; on a falling edge it puts the physics vehicle's steering
 * authority v+0x1534 back to 1.0 and clears drift state 4.  Returns
 * B3_AI_WHEEL_TAKE / B3_AI_WHEEL_GIVE / 0 (no edge); the caller performs the
 * navigator reset on TAKE. */
enum { B3_AI_WHEEL_NONE = 0, B3_AI_WHEEL_TAKE = 1, B3_AI_WHEEL_GIVE = 2 };
int  b3_ai_wheel_set(B3AiWheel* w, B3AiState* s, B3VehicleFull* veh, int on);

/* FUN_00171650 (vtable +0x20), the re-place handler: after it has moved the
 * car, retail sets racecar+0x245E so the very next FUN_00170820 returns
 * before touching the brain.  Call after any re-place. */
void b3_ai_replace_done(B3AiWheel* w);

/* FUN_00170820's own gate: 1 = run the AI this frame.  Consumes +0x245E. */
int  b3_ai_wheel_gate(B3AiWheel* w, int have_section, int have_vehicle);

/* --- FUN_00176150, the corner-BRAKE law -> AI+0x1D0 --------------------
 * Read instruction for instruction at 0x00176150..0x00176283:
 *
 *   if (AI+0x1FC != 0 || AI+0x298 <= 0.0 [0x003B16E0])
 *       AI+0x1D0 = racecar+0x2408                        // the hard cap
 *   else {
 *       cs = AI+0x298;  if (AI+0x213) cs *= 1.02 [0x003A2C44];
 *       if (AI+0x1F8 == 0)                               // target mode 0
 *           AI+0x1D0 = DAT_0047A1CC * (X - D) + cs
 *       else
 *           AI+0x1D0 = cs;
 *   }
 *   AI+0x1D0 = min(AI+0x1D0, racecar+0x2408)
 *
 * BOTH terms under the factor are DISTANCES, and the harness only had one of
 * them:
 *
 *   X = FUN_00174A90(AI+0x1D4, to = AI+0x214, from = AI+0x1D8)   @0x001761C4
 *   D = FUN_00174AF0(AI+0x1D4, AI+0x1D8, racecar_pos)            @0x00176204
 *
 * X is the ARC LENGTH from the car's own navigator node (AI+0x1D8) to the
 * planner's corner node (AI+0x214 = the plan record's `+0`), summed off the
 * per-node cumulative table the section's index row carries:
 *
 *   x1 = to   ? edge[to-1]   : 0                          @0x00174A93
 *   x2 = from ? edge[from-1] : 0                          @0x00174AAA
 *   X  = (from <= to) ? x1 - x2                           @0x00174AC1
 *                     : edge[node_count-1] - x2 + x1      @0x00174AD3
 *
 * and D is the car's SIGNED longitudinal offset INSIDE that same node,
 * clamped by the node's own segment length:
 *
 *   D = min(seg,  dot(carpos - pool[pair[node].point_a],
 *                     normalize(FUN_00174740(section, node))))
 *
 * so `X - D` is the distance still to run to the corner, and the law is the
 * ordinary braking ramp "allowed = corner value + factor * distance to go",
 * saturating at AI+0xA08 far out and falling to AI+0x298 on top of the node.
 *
 * THE HARNESS PASSED `cs` WHERE X BELONGS -- `factor*(cs - D) + cs` -- which
 * has NO dependence on the distance to the corner: a bend 400 m away was
 * braked for exactly as hard as one being entered, and on top of the node the
 * demand came out at 1.6 x cs instead of cs.  Executed side by side against
 * the real function the two disagree on 7 of 12 cases.
 *
 * That mistake survived because tools/validate_ai.py's Unicorn oracle stubbed
 * FUN_00174A90 with a bare `RET` -- "its companion, no output we need".  A
 * bare RET leaves XMM0 holding whatever the last SSE instruction put there,
 * and at that point that is `MOVSS XMM0,[EDI+0x298]` @0x00176169: the corner
 * value itself.  The oracle was handed the port's own wrong quantity and
 * dutifully agreed with it.
 *
 * NOTE ON UNITS: AI+0x298 is a corner RADIUS in metres, not a speed, and
 * there is no other candidate -- an exhaustive disp32 sweep of the image
 * gives AI+0x298 exactly THREE writers and THREE readers:
 *   W @0x00175AAE  0.0                       (FUN_00175B10's reset)
 *   W @0x00176C65  DAT_003A3408 = 5000.0     (the "no corner here" sentinel)
 *   W @0x00177055  (float)plan_record.u16@+6 (FUN_001772A0 @0x0017735E,
 *                  cvtsi2ss straight off the u16, no scale)
 *   R @0x00176169  this law, as `cs`
 *   R @0x00177A9F  `AI+0x298 / DAT_0047A1DC` clamped to 0..1 -- the lane
 *                  law, and DAT_0047A1DC is the registered VDB param
 *                  "Corner radius to give max offset pos" = 300
 *   R @0x001787F4  `AI+0x1EC > AI+0x298`, next to a SEPARATE speed test
 *                  `vehicle+0xBC > DAT_0047A1D8` -- so even here retail does
 *                  not compare it against a speed
 * Two of the three readers treat it as a radius; this one does not, and that
 * is the whole of it.  There is no undiscovered corner-SPEED table behind
 * AI+0x1D0: retail feeds the radius number into a speed expression and lets
 * the `min` with AI+0xA08 absorb it.  Deriving a speed from the radius
 * (v = sqrt(mu*g*r) or similar) would be GLUE, not recovery.
 * Retail nonetheless feeds it to this expression as a speed and mins the
 * result with AI+0xA08, so on shipped route data AI+0x1D0 sits at the hard
 * cap almost everywhere.  Measured over all 36 extracted tracks, at every
 * node that has a plan record and with D at its largest (the far end of the
 * node): 4501 of 177639 nodes -- 2.5% pooled, 0.4% (EU_C2_V1) to 9.3%
 * (EU_C1_V2) per track -- come out below the 88 m/s cap, median 72..79 m/s
 * where they do.  A further 33..54% of nodes carry no plan record at all and
 * fall through to the curvature scan regardless.  That is retail's behaviour
 * reproduced, not endorsed: anything that consumes this ceiling must expect
 * it to be nearly inert, which is why B3_NAV_SPEED defaults OFF and the
 * curvature scan stays in charge.
 *
 * `brake_dist` below is retail's `X - D`.  [C-disasm] */
float b3_ai_corner_brake(float corner_speed, float brake_dist, int boost_scale,
                         float speed_cap, int target_mode, int mode_1fc);

/* FUN_001712E0.  `node` is racecar+0x18D0, `dy` is
 * pos.y - road_surface_y - vehicle+0x870.  Returns 1 when the 61-frame
 * off-world rescue (FUN_001714F0) must fire. */
int  b3_ai_offworld(B3AiWheel* w, int node, float dy);

/* FUN_0018D790 @0x0018D840 + FUN_00170820 @0x001708EE.  `walk_ok` is
 * FUN_00174960's return.  Returns 1 when the stuck rescue must fire; the
 * caller then re-places at `w->last_node - B3_AI_RESCUE_BACK_NODES` and
 * commands B3_AI_RESCUE_SPEED_MS. */
int  b3_ai_navfail(B3AiWheel* w, int walk_ok);

#endif
