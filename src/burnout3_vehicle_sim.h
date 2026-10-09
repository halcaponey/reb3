// Burnout 3 vehicle simulation driven by the recovered physics parameters.
//
// PROVENANCE -- read this before trusting anything here.
//
//   REAL, recovered from default.xbe:
//     * the parameter set, names, struct offsets and 0x1D0 stride
//       (registration function FUN_00132D10, see docs/RE_NOTES.md section 8)
//     * every default value (constructor FUN_00132950)
//     * the config -> live-vehicle field mapping (FUN_00134710), including the
//       wheel array at +0x820 stride 0xC0, count byte at +0x1169
//
//   REAL, and now the whole live path: the per-frame pipeline
//     `b3_vehicle_step_full` is FUN_0011ECF0 + FUN_0011BE50's main path
//     composed from ported stages -- FUN_0011D460 tyre/drift/resistance,
//     FUN_0011AEF0 chassis contact, FUN_001239C0 + FUN_00123FD0 suspension,
//     FUN_00109560 integration -- each verified by executing the real x86
//     under Unicorn (tools/validate_port.py).
//
//   The header block that used to stand here ("the game's integrator has not
//   been reversed ... a conventional vehicle model wired to the real
//   numbers") described `b3_vehicle_step`, the scalar reconstruction that has
//   now been DELETED (ledger row PH-22).  Nothing in this file infers a force
//   equation any more; where something is still a stand-in it is marked GLUE
//   at the site and carries a ledger row.

#include <stddef.h>

#ifndef BURNOUT3_VEHICLE_SIM_H
#define BURNOUT3_VEHICLE_SIM_H

#include "burnout3_physics_params.h"

// The recovered physics config, laid out to mirror the game's 0x1D0 struct.
// Field order follows ascending struct offset so the mapping stays auditable.
typedef struct {
    float mass_kg;              // +0x0B8  Physics/Vehicle

    float front_attach_height;  // +0x0BC  Physics/Suspension/Front
    float front_spring_force;   // +0x0C0
    float front_spring_damping; // +0x0C4
    float front_spring_length;  // +0x0C8

    float rear_spring_force;    // +0x0CC  Physics/Suspension/Rear
    float rear_spring_damping;  // +0x0D0
    float rear_spring_length;   // +0x0D4
    float rear_attach_height;   // +0x0D8

    float gear[9];              // +0x0E0..+0x100 Reverse,Neutral,1st..6th,Final

    float idle_rpm;             // +0x104  Physics/Transmission/Engine
    float change_up_rpm;        // +0x108
    float change_down_rpm;      // +0x10C
    float max_rpm;              // +0x118
    float torque;               // +0x11C
    float peak_torque_revs;     // +0x120
    float falloff_torque_revs;  // +0x124

    float boost_kick_torque;    // +0x128  Physics/Transmission/Boost Kick
    float boost_kick_time;      // +0x12C

    float drag_coef;            // +0x130  Physics/Race Car/Misc
    float downforce_coef;       // +0x134

    float brake_force_height;   // +0x138  Physics/Race Car/Body Roll
    float accel_force_height;   // +0x13C
    float steer_force_height;   // +0x140
    float drift_force_height;   // +0x144

    float steer_min_angle;      // +0x148  Physics/Steering (degrees)
    float steer_max_angle;      // +0x14C
    float steer_min_velocity;   // +0x150
    float steer_max_velocity;   // +0x154
    float steer_response;       // +0x158
    float steer_drag_coef;      // +0x190

    float braking_factor;       // +0x15C  Physics/Race Car/Misc

    float slide_max;            // +0x160  Physics/Drift
    float slide_min;            // +0x164
    float turn_momentum_slow;   // +0x168
    float turn_momentum_fast;   // +0x16C
    float auto_drift_delay;     // +0x170
    float turn_rate;            // +0x174
    float min_drift_speed;      // +0x194
    float max_speed_pressure;   // +0x198
    float max_drift_angle;      // +0x1A0

    float wall_collision_penalty; // +0x178
    float lsdm_speed_limit;       // +0x17C  Physics/Race Car/LSDM
    float lsdm_steering_angle;    // +0x180
    float lsdm_torque1;           // +0x184
    float lsdm_torque2;           // +0x188
    float accel_multiplier;       // +0x18C
    float engine_braking_factor;  // +0x19C
    float max_speed_in_boost;     // +0x1A4
    float in_air_corkscrew_damping; // +0x1B0
    float min_drift_angle_in_air;   // +0x1B4

    float steer_away_time;         // +0x1BC Physics/Aggressive Driving Reaction
    float total_out_of_control_time; // +0x1C0
    float aggr_steering_max_angle;   // +0x1C4
    float aggr_steering_max_velocity;// +0x1C8
    float aggr_steering_drag_coef;   // +0x1CC
} B3PhysicsConfig;

// ---------------------------------------------------------------------------
// PORTED 1:1: the engine/transmission block, live vehicle +0x1448..+0x14D8.
// Filled from the config by FUN_00134710, reset by FUN_001214A0, updated every
// frame by FUN_00121560. All offsets and semantics verified by executing the
// real code (tools/validate_port.py, engine/transmission section).
// Defined here (before B3VehicleState) so the reconstruction can embed it.
// ---------------------------------------------------------------------------
#define B3_RPM_TO_RADS 0.10471976f   // the game's constant (FUN_00134710)
#define B3_RADS_TO_RPM_ 9.549296f    // 60 / 2pi as compiled into 0x0011D460

typedef struct {
    float gears[9];          // +0x1448 Reverse, Neutral, 1st..6th, Final
    float idle_rpm;          // +0x146C
    float change_up_rpm;     // +0x1470
    float change_down_rpm;   // +0x1474
    float unk_1478;          // +0x1478 [?] not written by FUN_00134710
    float unk_147C;          // +0x147C [?]
    float max_rpm;           // +0x1480
    float torque;            // +0x1484
    float rev_limit_rpm;     // +0x1488 live limit; randomised at the limiter
    float peak_omega;        // +0x148C peak_torque_revs    * 2pi/60
    float falloff_omega;     // +0x1490 falloff_torque_revs * 2pi/60
    float boost_kick_torque; // +0x1494
    float boost_kick_time;   // +0x1498
    float omega;             // +0x149C engine angular velocity, rad/s
    float shift_timer;       // +0x14A0 in-shift countdown (0.35/0.1/0.035 s)
    int   shifting;          // +0x14A4 shift in progress
    int   no_upshift;        // +0x14A8 set while airborne / out of race
    int   unk_14AC;          // +0x14AC
    unsigned rng_state;      // +0x14B0 PRNG state     (seed 0xFD462907)
    unsigned rng_inc;        // +0x14B4 PRNG increment (seed 0x02B9D6F8)
    int   flag_14B8;         // +0x14B8 [?] nonzero blocks all shifting
    int   unk_14BC;          // +0x14BC
    int   unk_14C0;          // +0x14C0 input-bit-0 latch (FUN_0011ECF0)
    int   unk_14C4;          // +0x14C4 input-bit-1 latch
    int   gear;              // +0x14C8 -1 reverse, 0 neutral, 1..gear_count
    int   gear_count;        // +0x14CC forward gears (counted by FUN_001214A0)
    float upshift_block;     // +0x14D0 upshift waits until this reaches 0
    float downshift_block;   // +0x14D4 downshift likewise
    // +0x14D8 is the owner vehicle pointer; passed as explicit args here.
} B3EngineTransmission;

// Config -> live copy (FUN_00134710) + reset (FUN_001214A0): gear count,
// rev limit = max rpm, PRNG seeds, neutral.
void b3_engine_transmission_init(B3EngineTransmission* t,
                                 const B3PhysicsConfig* cfg);

// Per-frame engine + gearbox update, FUN_00121560 verbatim. Returns the drive
// torque (the game stores it at v+0x1520 and splits it onto the rear wheels).
//   throttle       throttle minus brake, as the real call sites pass it
//   wheel_omega    driven (rear) wheel angular velocity, rad/s
//   kick           forces torque scale 2.0 (arg 3 at the real call sites)
//   boost          the input bit (v+0x13FC & 4), passed in EDI
//   speed_ms       owner vehicle +0xBC
//   max_boost_mph  owner vehicle +0x13D4 (config Max Speed In Boost)
//   boost_elapsed / boost_ramp_done: the boost-clock fields the real code
//   reads through (v+0x13F4): obj+0x10DC - obj+0x11C0 and byte obj+0x11F1
float b3_engine_transmission_update(B3EngineTransmission* t, float throttle,
                                    float wheel_omega, int kick, int boost,
                                    float dt, float speed_ms,
                                    float max_boost_mph, float boost_elapsed,
                                    int boost_ramp_done);

// Neutral/reverse gear engagement from FUN_0011ECF0 (verified by executing
// it): decides 1st vs reverse from throttle/brake/forward velocity, and swaps
// throttle/brake while in reverse exactly as the game does.
void b3_gear_engage(B3EngineTransmission* t, float* throttle, float* brake,
                    float fwd_vel, float speed_ms);

// Drive-torque application from FUN_0011D460: torque splits 50/50 onto wheels
// 2 and 3 (rear), then every wheel integrates omega += torque * 0.04 * dt and
// clears its torque accumulator.
void b3_drive_torque_to_wheels(float drive_torque, float dt, int wheel_count,
                               float wheel_torque[], float wheel_omega[]);

// Live per-vehicle simulation state.
typedef struct {
    float rpm;
    int   gear;        // index into B3PhysicsConfig.gear[]
    float speed;       // m/s along heading
    float drift_angle; // degrees
    float slide;
    float boost_timer;
    float suspension_compression[4];
    // Per-wheel spring state for the ported FUN_00123FD0 spring/damper
    // (cur/prev mirror wheel +0x64/+0x60; force is the verified equation's
    // output, the contact model feeding it is harness glue).
    float susp_cur[4];
    float susp_prev[4];
    float susp_force[4];
    int   susp_ready;
    // Real drivetrain state driven by the ported FUN_00121560 logic.
    B3EngineTransmission trans;
    int   trans_ready; // 0 until b3_vehicle_step initialises trans
} B3VehicleState;

// Populate cfg with the recovered compiled-in defaults.
void b3_physics_defaults(B3PhysicsConfig* cfg);

// Set the config field owning a given 0x1D0-struct offset (the offsets from
// burnout3_physics_params.h). Used both by b3_physics_defaults() and to apply
// the per-car VDB overrides from build/cars/car_physics.bin.
void b3_config_set_by_offset(B3PhysicsConfig* cfg, unsigned offset,
                             float value);

// Engine torque at a given rpm, from the real torque/peak/falloff triple.
float b3_engine_torque(const B3PhysicsConfig* cfg, float rpm);

// Steering angle in degrees for the current speed, interpolating the real
// min/max angle over the real min/max velocity window.
float b3_steer_angle(const B3PhysicsConfig* cfg, float speed);

// (PH-22: `b3_vehicle_step()`, the retired scalar reconstruction, is gone --
// see the note where it used to live in burnout3_vehicle_sim.c.  Both of its
// GLUE marks are superseded by recovered code in b3_vehicle_step_full.)

// ---------------------------------------------------------------------------
// Blocks ported 1:1 from the game (see the bottom of burnout3_vehicle_sim.c).
// These are the real equations.
// ---------------------------------------------------------------------------
#include "burnout3_vehicle_struct.h"

typedef struct {
    float vel_dir[3];        // vehicle +0xB0..+0xB8
    float speed_ms;          // +0xBC
    float speed_mph;         // +0x13D4 -- CORRECTED: this live field is the
                             // config's Max Speed In Boost (mph), copied by
                             // FUN_00134710 from config +0x1A4; the test below
                             // is a top-speed gate, not a unit conversion
    float resist_coef;       // +0x1360 -- config Drag Coefficient (+0x130)
    float change_up_rpm;     // +0x1470
    float engine_omega_rads; // +0x149C
    int   gear_current;      // +0x14C8
    int   gear_target;       // +0x14CC -- CORRECTED: count of forward gears
                             // (FUN_001214A0); the "shift" test is really
                             // "not in top gear and past change-up rpm"
    int   drivetrain_flag_a; // +0x1444 -- boost input latch (v+0x13FC bit 2)
    int   drivetrain_flag_b; // +0x1446
    int   extra_enabled;     // gate on the quadratic term
    float k_quad;            // +0x13C0 -- config Steering Drag Coef (+0x190)
    float q;                 // +0x1408 -- smoothed steering input
} B3ResistanceIn;

// Longitudinal resistance. Transcribed from 0x0011D460; opposes vel_dir.
// out_force is a 4-vector: xyz is the force, w is speed*scale (rate of work),
// matching the game's accumulator at +0x0F0..+0x0FC.
void b3_resistance_force(const B3ResistanceIn* in, float out_force[4]);

// Vertical force (gravity + downforce). Transcribed from 0x0011D460.
float b3_vertical_force(float downforce_coef, float speed_ms, float mass_kg);

// Gear change-up test from 0x0011D460.
int b3_should_shift_up(float change_up_rpm, float engine_omega_rads);

// ---------------------------------------------------------------------------
// Suspension solver blocks, ported 1:1 from FUN_00123FD0 / FUN_001239C0 and
// verified by executing the real functions under Unicorn
// (tools/validate_port.py, suspension section). See burnout3_vehicle_sim.c
// for the recovered wheel-record field map (v+0x820, stride 0xC0).
// ---------------------------------------------------------------------------

// Contact-path spring/damper (FUN_00123FD0). Returns the scalar force along
// the contact normal; updates cur (wheel+0x64) / prev (wheel+0x60) and the
// bump flag (wheel+0xB2) exactly as the game does. in_race = byte v+0x210.
float b3_wheel_spring_damper(float k, float c, float len, float attach,
                             float* cur, float* prev, float dt,
                             int in_race, int* bump_flag);

// normal * F into the force accumulator (v+0xF0) and r x F into the torque
// accumulator (v+0x100), r taken from the frame origin (FUN_00106590).
void b3_wheel_force_apply(const float normal[4], float f,
                          const float wheel_pos[4], const float frame_pos[4],
                          float force_acc[4], float torque_acc[4]);

// No-contact droop: returns the relaxed stored spring length (wheel+0x60).
float b3_wheel_droop(float k, float len, float attach, float dt);

// Wheel spin/omega integration from the FUN_00123FD0 tail (wheel+0x58/+0x5C).
void b3_wheel_spin_update(float* spin, float* omega, int decay_enabled,
                          int contact, float dt);

// FUN_001239C0 airborne path: synthesised contact point/normal from the
// frame up axis when the wheel ray hits nothing.
void b3_wheel_prepass_airborne(const float up[4], float h,
                               const float pos_prev[4],
                               float contact_pt[4], float normal[4]);

// ---------------------------------------------------------------------------
// Tyre grip -- FUN_0011D460's per-wheel force loop, ported 1:1 and verified
// by executing the real function (tools/validate_port.py, tyre section).
// See burnout3_vehicle_sim.c for the recovered constants and RE_NOTES 11.
// ---------------------------------------------------------------------------

// The core tyre force law. Given the wheel's velocity components in the
// (possibly steered) wheel basis and its rolling surface speed, produces the
// scalar lateral / longitudinal forces (to be applied along the lat/fwd axes).
//   front_axle    wheels 0/1 use the front constants, 2/3 the rear
//   throttle      live +0x1400 (reduces cornering stiffness)
//   speed_ms      +0xBC; mass_kg +0x1F0
//   vlat/vlong    dot(lat_axis, contact point velocity), dot(fwd_axis, ...)
//   roll_speed    wheel omega * wheel radius
void b3_tyre_grip(int front_axle, float throttle, float speed_ms,
                  float mass_kg, float vlat, float vlong, float roll_speed,
                  float* out_flat, float* out_flong);

// Wheel reaction: the longitudinal tyre force back-torques the wheel
// (torque -= F_long * radius) and omega integrates torque * 0.04 * dt.
void b3_tyre_wheel_reaction(float flong, float radius, float dt,
                            float* wheel_torque, float* wheel_omega);

// Airborne attitude dampers: the 4 compiled-in records (up axis gain 1000,
// right axis gain 10, at z = half_1d8*0.5 / half_1e8*2.0, z negated when
// reversing). frame = 4x4 row matrix (right/up/at/pos), vel4 = velocity with
// w = speed, omega = +0xD0. Accumulates into force_acc/torque_acc.
// THIS resolves the old "-20.0*dir.x" mystery term.
void b3_airborne_damper(const float frame[4][4], const float vel4[4],
                        const float omega[4], float half_1d8, float half_1e8,
                        float force_acc[4], float torque_acc[4]);

// Gravity body pin (tail of FUN_0011D460): when speed > 1 and frame at.y > 0,
// 20*mass of world-vertical gravity is removed and re-applied along the
// body's -up axis (DAT_0040a8a0 = (0,-20,0,0)) -- road-holding on banking.
void b3_gravity_body_pin(float mass_kg, float speed_ms, float at_y,
                         const float up[4], float force_acc[4]);

// Brake / engine-brake force scalar along the unit travel direction:
//   (engine_braking * offthrottle - braking_factor * brake * 20000)
//     * (speed + 1) / 70
// offthrottle = 0 while drifting or throttle > 0.1. While drifting the force
// is zeroed for the first 0.3 s and applied at the frame origin instead of
// pos + up*brake_force_height.
float b3_brake_drag_scalar(float engine_braking, float braking_factor,
                           float throttle, float brake, float speed_ms,
                           int drift_state, float drift_time);

// Drift-mode lateral force blend (FUN_0011D460, state 1/2):
//   mag = (1 - slide*2/3) * (dot(dir, at)*6400 + 4200), sign of flat,
//   scaled by drift_time below 1 s;  result = flat*(1-slide) + mag*slide
float b3_drift_lateral_blend(float flat, float slide_1440, float drift_time,
                             float dir_dot_at);

// Drift-mode yaw torque (replaces the wheel loop's up-axis torque) and its
// turn-momentum cap. state is +0x1524 (1 = left, 2 = right).
float b3_drift_yaw_torque(int state, float steer, float v_1414,
                          float slide_1440, float turn_rate,
                          float cos_max_drift, float dir_dot_right);
float b3_yaw_torque_cap(float speed_mph, float turn_momentum_slow,
                        float turn_momentum_fast, float drift_time);

// ---------------------------------------------------------------------------
// Steering scheduler + slew (FUN_0011ECF0), verified by executing it.
// live_1384/live_1380 are config Steer max/min velocity (+0x154/+0x150);
// they act as (base angle, speed offset): angle shrinks 1.4 deg per m/s.
// ---------------------------------------------------------------------------
float b3_steer_schedule(float speed_ms, float live_1384, float live_1380,
                        float steer_min_angle, float steer_max_angle);
float b3_steer_slew(float prev, float raw, float steer_response);

// ---------------------------------------------------------------------------
// Rigid-body integration -- FUN_00109560, ported 1:1 and verified
// (tools/validate_port.py, integrator section). Field map (live vehicle):
//   +0x204->frame rows right/up/at/pos, +0xB0 vel (w = speed at +0xBC),
//   +0xC0 unit travel dir, +0xD0 omega, +0xE0 angular momentum,
//   +0xF0/+0x100 force/torque accumulators, +0x110/+0x120 impulse
//   accumulators, +0x130 deflection, +0x10 BODY inverse inertia rows,
//   +0x40 world inverse inertia rows, +0x70 inverse frame transform.
// ---------------------------------------------------------------------------
// The dynamics window of the live vehicle, at RETAIL's own offsets: this
// struct sits at vehicle+0 and every field below +0x140 lands exactly where
// the game puts it. Declared in OFFSET order and packed, so the compiler adds
// no padding of its own.
//
// `frame` IS THE EXCEPTION, and deliberately so. Retail does not inline the
// transform in the vehicle: v+0x204 holds a POINTER to a separate frame object
// (4x4 + file ptr, aliased again at v+0xCC0 -- see CTX0 in
// tools/emulate_pipeline.py). The port inlines it instead, which is a genuine
// data-model divergence, not just a layout one: matching retail means the
// vehicle stores a pointer and the matrix lives in its own allocation. Until
// that separation is done, the matrix is parked past the parity window at
// +0x140 where it collides with nothing, and is marked here so it cannot be
// mistaken for a recovered offset.
typedef struct B3RigidBody {
    unsigned char _pad00[0x10];
    float inv_inertia_body[3][4]; // +0x10
    float inv_inertia_world[3][4];// +0x40   (rebuilt as Rt*I0*R)
    float inv_frame[4][4];        // +0x70   (rebuilt)
    float vel[4];                 // +0xB0 xyz velocity; [3] = speed (+0xBC)
    float dir[4];                 // +0xC0 unit travel direction
    float omega[4];               // +0xD0
    float angmom[4];              // +0xE0
    float force_acc[4];           // +0xF0   (cleared by the integrator)
    float torque_acc[4];          // +0x100  (cleared)
    float imp_force[4];           // +0x110  (cleared)
    float imp_torque[4];          // +0x120  (cleared)
    float deflection[4];          // +0x130  (added to pos, cleared)
    // Retail stores a POINTER to the frame object here, not the matrix:
    // v+0x204 -> {4x4 + file ptr}, aliased again at v+0xCC0 (CTX0 in
    // tools/emulate_pipeline.py). Pointer-to-row keeps every `frame[r][c]`
    // site indexing identically while the 64-byte matrix leaves the retail
    // window -- inlined it collided with contact_pt_160 at +0x160 whatever
    // offset it was parked at, which is what blocked B3VehicleFull parity.
    //
    // EVERY body must be bound to storage before a row is touched, and AFTER
    // any memset that would zero the pointer. b3_rigid_body_bind_frame does
    // it; B3_RIGID_BODY_LOCAL declares a local body already bound.
    //
    // THE SLOT IS PINNED TO 8 BYTES, and that is load-bearing rather than
    // decorative: this pointer is the LAST member, so sizeof(B3RigidBody) --
    // and therefore every retail offset in B3VehicleFull after it, which
    // reaches contact_pt_160 through `_pad00[0x18]` -- would otherwise be a
    // function of the target's POINTER WIDTH.  On a 32-bit target (the wasm
    // build, web/) the struct would end at 0x144 instead of 0x148 and slide
    // the whole 0x160..0x1A00 window down by 4.  The union costs nothing on
    // 64-bit, where the pointer already fills it.
    union {
        float (*frame)[4];        // GLUE +0x140 (retail: pointer at +0x204)
        unsigned char _frame_slot[8];
    };
} B3RigidBody;

_Static_assert(sizeof(B3RigidBody) == 0x148, "B3RigidBody span (see _frame_slot)");

_Static_assert(offsetof(B3RigidBody, inv_inertia_body)  == 0x010, "rb I0");
_Static_assert(offsetof(B3RigidBody, inv_inertia_world) == 0x040, "rb Iw");
_Static_assert(offsetof(B3RigidBody, inv_frame)         == 0x070, "rb invF");
_Static_assert(offsetof(B3RigidBody, vel)               == 0x0B0, "rb vel");
_Static_assert(offsetof(B3RigidBody, dir)               == 0x0C0, "rb dir");
_Static_assert(offsetof(B3RigidBody, omega)             == 0x0D0, "rb omega");
_Static_assert(offsetof(B3RigidBody, angmom)            == 0x0E0, "rb angmom");
_Static_assert(offsetof(B3RigidBody, force_acc)         == 0x0F0, "rb force");
_Static_assert(offsetof(B3RigidBody, torque_acc)        == 0x100, "rb torque");
_Static_assert(offsetof(B3RigidBody, imp_force)         == 0x110, "rb impF");
_Static_assert(offsetof(B3RigidBody, imp_torque)        == 0x120, "rb impT");
_Static_assert(offsetof(B3RigidBody, deflection)        == 0x130, "rb defl");
_Static_assert(offsetof(B3RigidBody, frame)             == 0x140, "rb frame slot");

// One integration step. in_race = byte +0x210, state6 = (+0x215 == 6),
// com_height = +0x1F4 (gravity torque application height while in race).
void b3_rigid_body_integrate(B3RigidBody* rb, float mass_kg, float com_height,
                             int in_race, int state6, float dt);

// Contact impulse (FUN_00106720): given a contact normal, the point and the
// point's velocity, returns the SIGNED impulse magnitude j and writes n*|j|
// into out_imp. FUN_00123FD0's suspension bottom-out block acts on j > 0.
float b3_contact_impulse(const B3RigidBody* rb, float mass_kg,
                         const float n[4], const float pt[4],
                         const float vpt[4], float restitution,
                         float out_imp[4]);

// ---------------------------------------------------------------------------
// FUN_00109EA0 [C] -- the shared BODY-vs-WORLD contact resolution that every
// rigid body in the game runs once per frame, through vtable slot +0x10 of
// its class (props FUN_0011A490 @0x0011A706, panels/debris FUN_001072A0
// @0x001073CF, racecar @0x00122F81), immediately AFTER the local polygon soup
// is gathered (FUN_00109D20) and BEFORE the class's own update slot +0 runs
// the integrator.  The narrow phase (FUN_00107950 for every body with
// +0x20C == 1, FUN_0010AAD0 otherwise) produces exactly one contact; that
// contact is this struct.  Everything after it is b3_rigid_body_world_contact.
//
// The default restitution is the rigid-body ctor's +0x1F8 = [0x003A69C4] =
// 0.1 (FUN_00109270 @0x001094C5); the class-7 WHEEL setup raises it to 0.7
// ([0x003B17D8] @0x00106AC0) and nothing else in the image overrides it.
// ---------------------------------------------------------------------------
typedef struct {
    float point[4];    // +0x160 contact point, world
    float normal[4];   // +0x170 contact normal, world, out of the surface
    float pushout[4];  // the penetration correction: -> +0x180, += +0x130
} B3WorldContact;

// The body bytes retail leaves behind, which its callers read: FUN_00106D00
// (class 7) branches on `grounded`, the takedown/steer-away scheduler reads
// `impact`, and `sleep` is the +0x20E freeze latch.
typedef struct {
    float impact;      // +0x194  the signed impulse magnitude, 0 if none
    float pushout[4];  // +0x180  the push-out that was applied this frame
    int   impulsed;    // +0x213  an impulse was applied (j > 0)
    int   grounded;    // +0x212  the narrow phase produced a contact
    int   sleep;       // +0x20E  the settle test latched "freeze"
    int   valid;       // +0x198  +0x160/+0x170/+0x180 hold this frame's data
} B3WorldContactResult;

// ---------------------------------------------------------------------------
// PH-05 -- the class-7 (panel / debris piece) body.  Its per-frame update is
// slot +0 of the vtable at 0x003B1108 = FUN_00106D00; the collision manager
// FUN_00110AF0 drives it once per frame for every allocated slot of the
// 0x40-entry pool at gameworld+0xD3380 (stride 0x4E0, FUN_00110390
// @0x0011037E / FUN_00110780 @0x00110772), immediately after slot +0x10
// (FUN_001072A0) has resolved the piece against the world.
// The bytes the update branches on, gathered into one struct:
// ---------------------------------------------------------------------------
typedef struct {
    int   suppress_4d0;   // +0x4D0 one-shot "skip one update" latch
    int   unit_216;       // +0x216 streaming unit, 0xFF = outside every unit
    int   attach_2ba;     // +0x2BA 0 free, 1 pinned, 2 the 0.6-damp variant
    int   grounded_212;   // +0x212 the narrow phase reported a contact
    float normal[4];      // +0x170 that contact's normal
} B3Class7State;

// FUN_00106D00 [C] verbatim (0x00106D00..0x00106EE8), minus the +0x2BA == 1
// pinned-pose presentation fix-up at the tail, which belongs to the panel
// module.  Consumes and clears the accumulators through
// b3_rigid_body_integrate, exactly as retail's tail call to FUN_00109560 does.
void b3_rigid_body_class7_update(B3RigidBody* rb, float mass_kg,
                                 float com_height, B3Class7State* st,
                                 float dt);

// FUN_00107950 [C] -- the OBB narrow phase every body with +0x20C == 1 runs
// (props @0x0011A12B, every class-7 piece @0x00106A36/0x00106ADB/0x00106C5B),
// specialised to a soup of ONE surface polygon: the body's box [bbmin, bbmax]
// (retail +0x1E0 / +0x1D0, filled from the model bbox @0x0011A0A8) is cut by
// the plane through `plane_pt` with unit normal `plane_n`, and the contact is
// the cross-section's vertex centroid, the plane normal and a push-out of
// normal * (the box's depth below the plane along the normal, floored at
// [0x003B194C] = 0.005).  Returns 0 when there is no contact -- which is the
// same "no hit" retail reports at 0x00107E78/0x00107C97.
int b3_rigid_body_obb_plane_contact(const B3RigidBody* rb,
                                    const float bbmin[3], const float bbmax[3],
                                    const float plane_pt[3],
                                    const float plane_n[3],
                                    B3WorldContact* out);

// One polygon of the world soup the narrow phase walks: retail's record is
// 0x40 bytes with the three vertices at +0x00/+0x10/+0x20 and the normal at
// +0x30 (FUN_00107950 reads them at exactly those offsets).
typedef struct {
    float v[3][3];     // the three vertices, WORLD
    float n[3];        // the unit face normal, WORLD
} B3WorldPoly;

// FUN_00107950 [C] over the WHOLE soup, which is what retail actually runs:
// every polygon's real vertices are clipped against the box, the surviving
// faces' normals are SUMMED and their clipped centroids AVERAGED, and the
// result is ONE contact -- the single B3WorldContact its caller FUN_00109EA0
// consumes once per frame.
//
// Prefer this to b3_rigid_body_obb_plane_contact whenever the caller has the
// real polygons.  The plane form fabricates a square of half-size
// |box dims| + 1 around `plane_pt`, so it only agrees with retail while that
// point lies under the body; handed a triangle vertex it misses every face
// larger than ~6 m (78.7% of the near-vertical faces on US_C3_V1).  And
// resolving the plane form once PER POLYGON is not the same thing at all:
// that applies N impulses and N push-outs where retail applies one.
int b3_rigid_body_obb_soup_contact(const B3RigidBody* rb,
                                   const float bbmin[3], const float bbmax[3],
                                   const B3WorldPoly* polys, int npoly,
                                   B3WorldContact* out);

// cls = body +0x215 (1/2/3 = the racecar states, 6 knocked prop, 7
// panel/debris piece; anything else takes the 0.875 arm),
// attach_mode = +0x2BA (class 7 only), restitution = +0x1F8.
// `c` NULL reproduces retail's no-contact early-out at 0x00109ECA.
// `out` may be NULL if the caller only wants the body mutated.
void b3_rigid_body_world_contact(B3RigidBody* rb, float mass_kg, int cls,
                                 int attach_mode, float restitution,
                                 const B3WorldContact* c,
                                 B3WorldContactResult* out);


// FUN_000FF270's retail re-orthonormalisation (@0x000FF27D..0x000FF544), all
// THREE branches: the rows are normalised, the already-most-orthogonal pair
// is kept and the other two rebuilt --
//   A @0x000FF332  r0 = ^(r1 x r2); r2 = ^(r0 x r1)   (also L0 <= 0)
//   B @0x000FF3D5  r1 = ^(r2 x r0); r0 = ^(r1 x r2)   (also L1 <= 0)
//   C @0x000FF37C  r2 = ^(r0 x r1); r1 = ^(r2 x r0)   (also L2 <= 0)
// selected by a = |r2.r1|, b = |r0.r2|, c = |r1.r0|.  A car's frame always
// lands in B; a body tumbling at 5-15 rad/s (prop, wreck, panel) takes A and
// C every few frames, which is why only-B diverged (PH-02).
void b3_mat_orthonormalize(float m[4][4]);

// ===========================================================================
// FULL PIPELINE -- the game's per-frame vehicle update (FUN_0011BE50 main
// path + FUN_0011ECF0 input stage), composed from the ported stages over a
// real vehicle struct with 4 independent wheels. Acceptance: the multi-frame
// differential trajectory in tools/validate_port.py (full-pipeline section)
// against the real code running under Unicorn (tools/emulate_pipeline.py).
// See docs/RE_NOTES.md section 14 for the per-stage evidence.
// ===========================================================================

// Ground interface, implemented by the harness (placeholder: route heights)
// or by the test driver (flat plane y=0). The collision agent will supply
// the real mesh-backed version. Returns the surface type (>= 0) or -1 for
// no ground; fills the ground height at (x,z) and the surface normal.
// This stands in for FUN_0011BC60 (soup collection) + the per-poly ray
// tests of FUN_001239C0/FUN_00123790.
int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]);

// Optional override: when set, the pipeline queries this instead of
// b3_ground_probe. The harness installs a mesh-first/route-fallback wrapper
// here (the collision module's mesh probe with the route line as the
// placeholder until it is fully live); test drivers leave it NULL and
// provide b3_ground_probe directly.
extern int (*b3_ground_probe_hook)(float x, float y, float z,
                                   float* out_height, float out_normal[3]);

// The live wheel record at RETAIL's own layout: vehicle+0x820, stride 0xC0,
// every field at the offset the game puts it.
//
// Declared in OFFSET order with explicit padding, and packed so the compiler
// adds none of its own -- the previous logical ordering put `spin` at 0x60
// instead of retail's 0x58 and made the whole record 0x80 instead of 0xC0, so
// no retail code could have walked this array.
//
// local_x / local_z / frame_y used to live here. They are HARNESS state with
// no retail offset, so they would have been squatting on bytes the game owns;
// they now live in B3VehicleFull's wheel_local_x/_z/wheel_frame_y arrays.
typedef struct {
    float world_pos[4];       // +0x00 (written by FUN_00123FD0)
    float contact_pt[4];      // +0x10
    float normal[4];          // +0x20 contact normal
    float prev_pos[4];        // +0x30 prev-frame world pos (BE50 restore)
    float prev_contact[4];    // +0x40
    float radius;             // +0x50
    float torque;             // +0x54
    float spin;               // +0x58
    float omega;              // +0x5C
    float prev_len;           // +0x60
    float cur_len;            // +0x64 (pre-pass output)
    unsigned char _pad68[0x0C];
    float attach;             // +0x74
    unsigned char _pad78[0x38];
    unsigned short surface;   // +0xB0
    unsigned char bump;       // +0xB2
    unsigned char contact;    // +0xB3
    unsigned char force_flag; // +0xB4
    unsigned char _padB5[0x0B];
} B3WheelSim;

#define B3_WHEEL_RETAIL_STRIDE 0xC0u
_Static_assert(sizeof(B3WheelSim) == B3_WHEEL_RETAIL_STRIDE,
               "wheel stride must match retail's 0xC0");
_Static_assert(offsetof(B3WheelSim, radius)   == 0x50, "wheel radius");
_Static_assert(offsetof(B3WheelSim, torque)   == 0x54, "wheel torque");
_Static_assert(offsetof(B3WheelSim, spin)     == 0x58, "wheel spin");
_Static_assert(offsetof(B3WheelSim, omega)    == 0x5C, "wheel omega");
_Static_assert(offsetof(B3WheelSim, prev_len) == 0x60, "wheel prev_len");
_Static_assert(offsetof(B3WheelSim, cur_len)  == 0x64, "wheel cur_len");
_Static_assert(offsetof(B3WheelSim, attach)   == 0x74, "wheel attach");
_Static_assert(offsetof(B3WheelSim, surface)  == 0xB0, "wheel surface");
_Static_assert(offsetof(B3WheelSim, contact)  == 0xB3, "wheel contact");

// ---------------------------------------------------------------------------
// FUN_0011AEF0's WORLD -- the chassis polygon soup, live vehicle +0x200.
//
// Retail record layout, read straight off the gather loop at
// 0x0011B000..0x0011B033 (EAX = [veh+0x200]):
//     [+0x00] int    poly count            (`mov ecx,[eax]`      @0x0011AFE8)
//     [+0x04] void*  poly base, stride 0x40 (`mov edx,[eax+4]` + `add ebx,0x40`)
//     [+0x08] u16*   surface flags, one per poly (`mov cx,[edx+esi*2]`)
// A poly record is four 16-byte vectors -- p0, p1, p2, normal -- which is
// exactly `B3CrashPoly` in burnout3_crash.h.
//
// THE ORDER THAT MATTERS.  `FUN_0011BC60` fills this record ONCE per frame,
// @0x0011BF43, OUTSIDE the substep loop; `FUN_0011AEF0` then re-reads the
// frozen set on EVERY substep @0x0011C0B7.  `soup_freeze` is called at the
// first address and `chassis_resolve` at the second, so a port that installs
// both reproduces retail's cadence exactly.  Both default to NULL, which is
// the "no world" configuration (the resolve returns 0 -- FUN_0011BE50's own
// eax == 0 arm) and leaves the pipeline byte-identical to a soup-less run.
// ---------------------------------------------------------------------------
#ifndef B3_CRASH_POLY_DEFINED
#define B3_CRASH_POLY_DEFINED
typedef struct B3CrashPoly {
    float p0[4], p1[4], p2[4], n[4];
} B3CrashPoly;
#endif

typedef struct {
    int count;                          // veh+0x200 -> [+0x00]
    const struct B3CrashPoly* polys;    //             [+0x04] stride 0x40
    const unsigned short* flags;        //             [+0x08] u16 per poly
} B3ChassisSoup;

// The full vehicle: rigid body + wheels + drivetrain + the live config
// copies FUN_00134710 installs (named by their live offsets).
typedef struct B3VehicleFull {
    // ---- RETAIL WINDOW 0x0000..0x1A00: every field where the game puts
    // it, packed with explicit padding, asserted below the struct.
    B3RigidBody rb;            // +0x0000  dynamics (see B3RigidBody)
    unsigned char _pad00[0x18];
    float                  contact_pt_160[4];  // +0x160 world contact point
    float                  contact_n_170[4];  // +0x170 contact normal (wall: flattened)
    unsigned char _pad01[0x10];
    unsigned short         surface_190;  // +0x190 surface flags of the contact
    unsigned char _pad02[0x2];
    float                  impact_194;  // +0x194 impact magnitude
    int                    contact_state_198;  // +0x198 0 none / 1 wall / 2 ground
    unsigned char _pad03[0x34];
    float                  half_ext[4];  // +0x1D0 (.bgv +0xE80)
    float                  center_off[4];  // +0x1E0 (.bgv +0xE90)
    float                  mass;  // +0x1F0
    float                  com_height;  // +0x1F4 = (half_ext.y - center_off.y) * 0.1
    unsigned char _pad04[0x16];
    // The sleep flag FUN_0011AEF0 and the suspension pass both consult. It
    // lived only in B3CrashVehicle, the fourth partial view of this object.
    unsigned char          asleep_020E;  // +0x20E (FUN_00125100 clears it)
    unsigned char _pad04b[0x2];
    unsigned char          landed_211;  // +0x211 "landed on a car" (tail unported)
    unsigned char          contact_212;  // +0x0212
    unsigned char          contact_213;  // +0x0213
    unsigned char _pad05[0x1];
    unsigned char          class_215;  // +0x0215
    unsigned char _pad06[0x10E];
    float                  grip_scalar;  // ctx+0x324
    unsigned char _pad07[0x174];
    float                  ground_clear;  // ctx+0x49C
    unsigned char _pad08[0x380];
    B3WheelSim             wheel[4];  // +0x0820
    unsigned char _pad09[0x640];
    unsigned short         surface_1160;  // +0x1160
    unsigned char _pad0A[0x2];
    float                  steer_deg_1164;  // +0x1164
    unsigned char _pad0B[0x1];
    unsigned char          wheel_count;  // +0x1169  BYTE in retail, not int:
                                         // an int here is unaligned at 0x1169
    unsigned char _pad0C_head[0x66];     // +0x116A .. 0x11D0
    B3CrashPoly            crash_floor_poly[6]; // +0x11D0 (6 x 0x40 = 0x180: 0x11D0 .. 0x1350)
    unsigned char          flags_1350;   // +0x1350 (extra barrier count gate)
    unsigned char          flags_1351;   // +0x1351 (crash-floor active flag)
    unsigned char _pad0C_tail[1];        // +0x1352
    unsigned char          flags_1353;  // +0x1353 bit0|bit2 disable, bit3 blocks crash
    unsigned char _pad0D[0xC];
    float                  resist_1360;  // +0x1360
    float                  downforce_1364;  // +0x1364
    float                  brake_h_1368;  // +0x1368
    float                  accel_h_136C;  // +0x136C
    float                  steer_h_1370;  // +0x1370
    float                  drift_h_1374;  // +0x1374
    float                  steer_min_1378;  // +0x1378
    float                  steer_max_137C;  // +0x137C
    float                  steer_v0_1380;  // +0x1380
    float                  steer_base_1384;  // +0x1384
    float                  steer_resp_1388;  // +0x1388
    float                  brakef_138C;  // +0x138C
    float                  slide_max_1390;  // +0x1390
    float                  slide_min_1394;  // +0x1394
    float                  turn_slow_1398;  // +0x1398
    float                  turn_fast_139C;  // +0x139C
    float                  autodrift_13A0;  // +0x13A0
    float                  turn_rate_13A4;  // +0x13A4
    float                  surface_grip_13A8;  // +0x13A8 class-0 velocity scrub factor
    float                  lsdm_limit_13AC;  // +0x13AC
    float                  lsdm_angle_13B0;  // +0x13B0
    float                  lsdm_t1_13B4;  // +0x13B4
    float                  lsdm_t2_13B8;  // +0x13B8
    float                  accel_mult_13BC;  // +0x13BC
    float                  kquad_13C0;  // +0x13C0
    float                  mindrift_13C4;  // +0x13C4
    float                  maxpress_13C8;  // +0x13C8
    float                  engbrake_13CC;  // +0x13CC
    float                  cos_maxdrift_13D0;  // +0x13D0
    float                  maxboost_13D4;  // +0x13D4
    float                  corkscrew_13D8;  // +0x13D8
    float                  cos90_mindrift_air_13DC;  // +0x13DC
    float                  aggr_time_13E0;  // Steer Away Time (s)
    float                  aggr_total_13E4;  // Total Out-Of-Control Time (s)
    float                  aggr_angle_13E8;  // Aggressive Steering Max Angle (deg)
    float                  aggr_vel_13EC;  // Aggressive Steering Max Velocity
    float                  aggr_drag_13F0;  // Aggressive Steering Drag Coef
    unsigned char _pad0E[0x8];
    unsigned char          input_bits_13FC;  // +0x13FC
    unsigned char _pad0F[0x3];
    float                  throttle_1400;  // +0x1400
    float                  brake_1404;  // +0x1404
    float                  steer_1408;  // +0x1408
    unsigned char _pad10[0x8];
    float                  throttle_raw_1414;  // +0x1414
    unsigned char _pad11[0x4];
    float                  thr_prev_141C;  // +0x141C
    float                  brake_prev_1420;  // +0x1420
    float                  steer_prev_1424;  // +0x1424
    unsigned char _pad12[0x4];
    float                  drift_time_142C;  // +0x142C
    float                  slide_prev_1430;  // +0x1430
    float                  drift_dir_1434;  // +0x1434 flipped when gear == -1
    float                  drift_timer_1438;  // +0x1438
    float                  airtime_143C;  // +0x143C
    float                  slide_1440;  // +0x1440
    unsigned char          boost_1444;  // +0x1444
    unsigned char _pad13[0x1];
    unsigned char          flag_b_1446;  // +0x1446
    unsigned char _pad14[0x1];
    B3EngineTransmission   trans;  // +0x1448..+0x14D4
    unsigned char _pad15[0x48];
    float                  drive_torque_1520;  // +0x1520
    int                    drift_state_1524;  // +0x1524
    unsigned char _pad16[0x4];
    float                  timer_152C;  // +0x152C
    unsigned char _pad17[0x4];
    float                  authority_1534;  // +0x1534 driver authority (crash thresholds)
    unsigned char _pad18[0x4];
    unsigned char          hit_side_153C;  // +0x153C
    unsigned char          byte_153D;  // +0x153D
    unsigned char          no_scrub_153E;  // +0x153E non-class-0: skip the 0.99 scrub
    unsigned char _pad19[0x11];
    // The AI driver's LSDM gate. A BYTE: validate_ai.py seeds it with wb()
    // (s.wb(VEH+0x1550, 0/1)), and B3AiCar carried it as an int -- the same
    // type defect found 14 times elsewhere. Modelled here because it is the
    // vehicle's byte, not the racecar's.
    unsigned char          lsdm_active_1550;  // +0x1550 LSDM engaged
    unsigned char _pad19c[0x1];
    // The driver's stop flag. Also a BYTE -- validate_ai.py reads it back
    // with rb(VEH+0x1552) -- and it too was an int in the struct the port
    // invented for the driver's outputs.
    unsigned char          stop_flag_1552;  // +0x1552 driver stop
    unsigned char _pad19d[0x19];
    // The AI driver's per-car scratch. These lived in B3AiState, which is a
    // view of the AI OBJECT at racecar+0x1A00 -- so they sat at vehicle
    // offsets inside an AI-shaped struct, the same conflation B3AiCar had,
    // and it blocked the AI object from being transferred at all.
    float                  prev_throttle_156C;  // +0x156C throttle carried across frames
    float                  brake_hold_1570;     // +0x1570 brake hold timer
    float                  dither_1574;         // +0x1574 throttle-dither cycle
    float                  stuck_arm_1578;      // +0x1578 stuck arm (-1 = disarmed)
    float                  reverse_timer_157C;  // +0x157C reverse burst (-1 = idle)
    unsigned char _pad19b[0x110];
    float                  ooc_wall_1690;  // ... +0x1690 second (wall/spin) stamp
    unsigned char _pad1A[0x36C];

    // ---- HARNESS SIDE, past the retail window. Nothing here carries a
    // recovered VEHICLE offset (annotations naming config/owner/racecar
    // belong to other objects), so none of it squats on the game's bytes.
    // Which emulator pipeline session this car maps to, so a retail backend
    // reached through a hook (chassis_resolve gets only the vehicle) can name
    // the session whose veh+0x200 soup belongs to THIS car. -1 = unassigned.
    int                    emu_slot;
    /* harness-side: 1 while the RETAIL EMULATOR owns this car's physics step
     * (b3_emu_slot_of assigned it a live slot), 0 while the port steps it.
     * Ownership is PER CAR -- B3_EMU_CARS caps how many ride the emulator
     * (default 1, the player) -- so any "physics is retail" decision taken
     * per FEATURE instead of per car is wrong for everyone past the cap. */
    int                    emu_owned;
    float                  rb_frame_store[4][4];  // HARNESS backing for rb.frame; retail
    float                  wheel_local_x[4];  // wheel frame row3 x (.bgv +0xB80 attach)
    float                  wheel_local_z[4];  // wheel frame row3 z
    float                  wheel_frame_y[4];  // wheel frame row3 y (visual drop)
    float                  front_attach;  
    float                  front_damp;  
    float                  front_k;  
    float                  front_len;  
    float                  rear_attach;  
    float                  rear_damp;  
    float                  rear_k;  
    float                  rear_len;  
    unsigned char          f1168;  
    float                  cfg_steer_max_14C;  // config +0x14C  Steering Max Angle
    float                  cfg_steer_maxvel_154;  // config +0x154  Steering Max Velocity
    float                  cfg_steer_drag_190;  // config +0x190  Steering Drag Coef
    float                  ooc_slam_1598;  // owner(+0x13F4)+0x1198 -> +0x1598 slam stamp
    float                  launch_time_1350;  // owner +0x1350 (the flag_b 3 s clear)
    int                    racecar_class_1920;  // racecar+0x1920 (2 never wall-crashes)
    int                    is_class0;  // racecar class == 0 -> the second poly set
    int                    party_mode;  // FUN_00017310 (crash-party thresholds)
    int                    crash_fired;  // the FUN_0010DCA0 call retail makes
    float                  crash_dv;  
    float                  crash_dv_thr;  
    float                  crash_headon;  
    float                  crash_headon_thr;  
    unsigned char          surf_bit15_15CC;  // racecar+0x15CC := surface >> 15
    float                  clock;  // DAT_0060EA20 mirror
    float                  boost_elapsed;  
    int                    boost_ramp_done;  
    // shapes the generator does not model (function pointers etc.):
    B3ChassisSoup soup;                       // veh+0x200
    void* soup_user;                          // harness cookie
    int (*soup_freeze)(void* user, struct B3VehicleFull* v);  // FUN_0011BC60
    int (*chassis_resolve)(struct B3VehicleFull* v);          // FUN_0011AEF0
    void (*live_panels_step)(void* user, struct B3VehicleFull* v, float dt); // FUN_00126D40/FUN_00126520
    void* soup_ground_user;
    /* `wheel_gate` distinguishes retail's TWO rays into the same veh+0x200
     * soup, which are NOT filtered alike:
     *   1  the per-wheel ray, FUN_001239C0 @0x00123CEF -> FUN_00123790.  That
     *      one applies the SURFACE GATE @0x00123799..0x0012383E [C]: a
     *      racing car (veh+0x215 in {1,2,3}, veh+0x210 == 0) skips every
     *      polygon whose surface low byte is > 0x14 and != 0x26.
     *   0  the under-body 30 m clearance ray, FUN_001239C0
     *      @0x00123EC6..0x00123F97 [C], which walks the same soup but calls
     *      FUN_001B2230 directly @0x00123F00 with NO gate at all.
     * Passing the flag is what keeps the port from gating both or neither. */
    int (*soup_ground_ray)(void* user, const float start[3],
                           const float end[3], float* hit_t,
                           float normal[3], int wheel_gate);
} B3VehicleFull;

#define B3_VEHICLE_RETAIL_SPAN 0x1A00u
_Static_assert(offsetof(B3VehicleFull, rb) == 0, "rb at vehicle+0");
_Static_assert(offsetof(B3VehicleFull, contact_pt_160) == 0x0160, "contact_pt_160 off retail");
_Static_assert(offsetof(B3VehicleFull, contact_n_170) == 0x0170, "contact_n_170 off retail");
_Static_assert(offsetof(B3VehicleFull, surface_190) == 0x0190, "surface_190 off retail");
_Static_assert(offsetof(B3VehicleFull, impact_194) == 0x0194, "impact_194 off retail");
_Static_assert(offsetof(B3VehicleFull, contact_state_198) == 0x0198, "contact_state_198 off retail");
_Static_assert(offsetof(B3VehicleFull, half_ext) == 0x01D0, "half_ext off retail");
_Static_assert(offsetof(B3VehicleFull, center_off) == 0x01E0, "center_off off retail");
_Static_assert(offsetof(B3VehicleFull, mass) == 0x01F0, "mass off retail");
_Static_assert(offsetof(B3VehicleFull, com_height) == 0x01F4, "com_height off retail");
_Static_assert(offsetof(B3VehicleFull, asleep_020E) == 0x020E, "asleep_020E off retail");
_Static_assert(offsetof(B3VehicleFull, landed_211) == 0x0211, "landed_211 off retail");
_Static_assert(offsetof(B3VehicleFull, contact_212) == 0x0212, "contact_212 off retail");
_Static_assert(offsetof(B3VehicleFull, contact_213) == 0x0213, "contact_213 off retail");
_Static_assert(offsetof(B3VehicleFull, class_215) == 0x0215, "class_215 off retail");
_Static_assert(offsetof(B3VehicleFull, grip_scalar) == 0x0324, "grip_scalar off retail");
_Static_assert(offsetof(B3VehicleFull, ground_clear) == 0x049C, "ground_clear off retail");
_Static_assert(offsetof(B3VehicleFull, wheel) == 0x0820, "wheel off retail");
_Static_assert(offsetof(B3VehicleFull, surface_1160) == 0x1160, "surface_1160 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_deg_1164) == 0x1164, "steer_deg_1164 off retail");
_Static_assert(offsetof(B3VehicleFull, wheel_count) == 0x1169, "wheel_count off retail");
_Static_assert(offsetof(B3VehicleFull, crash_floor_poly) == 0x11D0, "crash_floor_poly off retail");
_Static_assert(offsetof(B3VehicleFull, flags_1350) == 0x1350, "flags_1350 off retail");
_Static_assert(offsetof(B3VehicleFull, flags_1351) == 0x1351, "flags_1351 off retail");
_Static_assert(offsetof(B3VehicleFull, flags_1353) == 0x1353, "flags_1353 off retail");
_Static_assert(offsetof(B3VehicleFull, resist_1360) == 0x1360, "resist_1360 off retail");
_Static_assert(offsetof(B3VehicleFull, downforce_1364) == 0x1364, "downforce_1364 off retail");
_Static_assert(offsetof(B3VehicleFull, brake_h_1368) == 0x1368, "brake_h_1368 off retail");
_Static_assert(offsetof(B3VehicleFull, accel_h_136C) == 0x136C, "accel_h_136C off retail");
_Static_assert(offsetof(B3VehicleFull, steer_h_1370) == 0x1370, "steer_h_1370 off retail");
_Static_assert(offsetof(B3VehicleFull, drift_h_1374) == 0x1374, "drift_h_1374 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_min_1378) == 0x1378, "steer_min_1378 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_max_137C) == 0x137C, "steer_max_137C off retail");
_Static_assert(offsetof(B3VehicleFull, steer_v0_1380) == 0x1380, "steer_v0_1380 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_base_1384) == 0x1384, "steer_base_1384 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_resp_1388) == 0x1388, "steer_resp_1388 off retail");
_Static_assert(offsetof(B3VehicleFull, brakef_138C) == 0x138C, "brakef_138C off retail");
_Static_assert(offsetof(B3VehicleFull, slide_max_1390) == 0x1390, "slide_max_1390 off retail");
_Static_assert(offsetof(B3VehicleFull, slide_min_1394) == 0x1394, "slide_min_1394 off retail");
_Static_assert(offsetof(B3VehicleFull, turn_slow_1398) == 0x1398, "turn_slow_1398 off retail");
_Static_assert(offsetof(B3VehicleFull, turn_fast_139C) == 0x139C, "turn_fast_139C off retail");
_Static_assert(offsetof(B3VehicleFull, autodrift_13A0) == 0x13A0, "autodrift_13A0 off retail");
_Static_assert(offsetof(B3VehicleFull, turn_rate_13A4) == 0x13A4, "turn_rate_13A4 off retail");
_Static_assert(offsetof(B3VehicleFull, surface_grip_13A8) == 0x13A8, "surface_grip_13A8 off retail");
_Static_assert(offsetof(B3VehicleFull, lsdm_limit_13AC) == 0x13AC, "lsdm_limit_13AC off retail");
_Static_assert(offsetof(B3VehicleFull, lsdm_angle_13B0) == 0x13B0, "lsdm_angle_13B0 off retail");
_Static_assert(offsetof(B3VehicleFull, lsdm_t1_13B4) == 0x13B4, "lsdm_t1_13B4 off retail");
_Static_assert(offsetof(B3VehicleFull, lsdm_t2_13B8) == 0x13B8, "lsdm_t2_13B8 off retail");
_Static_assert(offsetof(B3VehicleFull, accel_mult_13BC) == 0x13BC, "accel_mult_13BC off retail");
_Static_assert(offsetof(B3VehicleFull, kquad_13C0) == 0x13C0, "kquad_13C0 off retail");
_Static_assert(offsetof(B3VehicleFull, mindrift_13C4) == 0x13C4, "mindrift_13C4 off retail");
_Static_assert(offsetof(B3VehicleFull, maxpress_13C8) == 0x13C8, "maxpress_13C8 off retail");
_Static_assert(offsetof(B3VehicleFull, engbrake_13CC) == 0x13CC, "engbrake_13CC off retail");
_Static_assert(offsetof(B3VehicleFull, cos_maxdrift_13D0) == 0x13D0, "cos_maxdrift_13D0 off retail");
_Static_assert(offsetof(B3VehicleFull, maxboost_13D4) == 0x13D4, "maxboost_13D4 off retail");
_Static_assert(offsetof(B3VehicleFull, corkscrew_13D8) == 0x13D8, "corkscrew_13D8 off retail");
_Static_assert(offsetof(B3VehicleFull, cos90_mindrift_air_13DC) == 0x13DC, "cos90_mindrift_air_13DC off retail");
_Static_assert(offsetof(B3VehicleFull, aggr_time_13E0) == 0x13E0, "aggr_time_13E0 off retail");
_Static_assert(offsetof(B3VehicleFull, aggr_total_13E4) == 0x13E4, "aggr_total_13E4 off retail");
_Static_assert(offsetof(B3VehicleFull, aggr_angle_13E8) == 0x13E8, "aggr_angle_13E8 off retail");
_Static_assert(offsetof(B3VehicleFull, aggr_vel_13EC) == 0x13EC, "aggr_vel_13EC off retail");
_Static_assert(offsetof(B3VehicleFull, aggr_drag_13F0) == 0x13F0, "aggr_drag_13F0 off retail");
_Static_assert(offsetof(B3VehicleFull, input_bits_13FC) == 0x13FC, "input_bits_13FC off retail");
_Static_assert(offsetof(B3VehicleFull, throttle_1400) == 0x1400, "throttle_1400 off retail");
_Static_assert(offsetof(B3VehicleFull, brake_1404) == 0x1404, "brake_1404 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_1408) == 0x1408, "steer_1408 off retail");
_Static_assert(offsetof(B3VehicleFull, throttle_raw_1414) == 0x1414, "throttle_raw_1414 off retail");
_Static_assert(offsetof(B3VehicleFull, thr_prev_141C) == 0x141C, "thr_prev_141C off retail");
_Static_assert(offsetof(B3VehicleFull, brake_prev_1420) == 0x1420, "brake_prev_1420 off retail");
_Static_assert(offsetof(B3VehicleFull, steer_prev_1424) == 0x1424, "steer_prev_1424 off retail");
_Static_assert(offsetof(B3VehicleFull, drift_time_142C) == 0x142C, "drift_time_142C off retail");
_Static_assert(offsetof(B3VehicleFull, slide_prev_1430) == 0x1430, "slide_prev_1430 off retail");
_Static_assert(offsetof(B3VehicleFull, drift_dir_1434) == 0x1434, "drift_dir_1434 off retail");
_Static_assert(offsetof(B3VehicleFull, drift_timer_1438) == 0x1438, "drift_timer_1438 off retail");
_Static_assert(offsetof(B3VehicleFull, airtime_143C) == 0x143C, "airtime_143C off retail");
_Static_assert(offsetof(B3VehicleFull, slide_1440) == 0x1440, "slide_1440 off retail");
_Static_assert(offsetof(B3VehicleFull, boost_1444) == 0x1444, "boost_1444 off retail");
_Static_assert(offsetof(B3VehicleFull, flag_b_1446) == 0x1446, "flag_b_1446 off retail");
_Static_assert(offsetof(B3VehicleFull, trans) == 0x1448, "trans off retail");
_Static_assert(offsetof(B3VehicleFull, drive_torque_1520) == 0x1520, "drive_torque_1520 off retail");
_Static_assert(offsetof(B3VehicleFull, drift_state_1524) == 0x1524, "drift_state_1524 off retail");
_Static_assert(offsetof(B3VehicleFull, timer_152C) == 0x152C, "timer_152C off retail");
_Static_assert(offsetof(B3VehicleFull, authority_1534) == 0x1534, "authority_1534 off retail");
_Static_assert(offsetof(B3VehicleFull, hit_side_153C) == 0x153C, "hit_side_153C off retail");
_Static_assert(offsetof(B3VehicleFull, byte_153D) == 0x153D, "byte_153D off retail");
_Static_assert(offsetof(B3VehicleFull, no_scrub_153E) == 0x153E, "no_scrub_153E off retail");
_Static_assert(offsetof(B3VehicleFull, prev_throttle_156C) == 0x156C, "prev_throttle_156C off retail");
_Static_assert(offsetof(B3VehicleFull, brake_hold_1570) == 0x1570, "brake_hold_1570 off retail");
_Static_assert(offsetof(B3VehicleFull, dither_1574) == 0x1574, "dither_1574 off retail");
_Static_assert(offsetof(B3VehicleFull, stuck_arm_1578) == 0x1578, "stuck_arm_1578 off retail");
_Static_assert(offsetof(B3VehicleFull, reverse_timer_157C) == 0x157C, "reverse_timer_157C off retail");
_Static_assert(offsetof(B3VehicleFull, stop_flag_1552) == 0x1552, "stop_flag_1552 off retail");
_Static_assert(offsetof(B3VehicleFull, lsdm_active_1550) == 0x1550, "lsdm_active_1550 off retail");
_Static_assert(offsetof(B3VehicleFull, ooc_wall_1690) == 0x1690, "ooc_wall_1690 off retail");

// ---------------------------------------------------------------------------
// Aggressive driving reaction / "steer away" -- FUN_0011ECF0's head
// (0x0011ED17..0x0011EF29) plus the forced-lock write at 0x0011F309.
// While the owner is inside an out-of-control window the live steering trio
// (+0x137C max angle, +0x1384 schedule base, +0x13C0 steering drag) is
// REPLACED by the aggressive set, and during the first Steer-Away-Time the
// steering input itself is driven to full lock away from the hit side.
// ---------------------------------------------------------------------------
typedef struct {
    int window;   // stack [esp+0xE]: inside Total Out-Of-Control Time
    int phase1;   // stack [esp+0xD]: inside Steer Away Time -> forced lock
    int event2;   // stack [esp+0xF]: the +0x1690 stamp won (0.6x windows)
} B3SteerAway;

// Runs the select and rewrites v->steer_max_137C / steer_base_1384 /
// kquad_13C0 exactly as the real function does.
void b3_steer_away_envelope(B3VehicleFull* v, B3SteerAway* out);

void b3_vehicle_full_init(B3VehicleFull* v, const B3PhysicsConfig* cfg,
                          const float wheels_xz[4][2], float radius,
                          const float half_ext[4], const float center_off[4],
                          const float inv_inertia_diag[3],
                          const float pos[3], float heading_rad);

// One frame of the real pipeline: input stage (FUN_0011ECF0 incl. engine),
// the once-per-frame soup freeze (FUN_0011BC60 @0x0011BF43) then
// FUN_0011BE50's main path: 2 substeps at dt/2 of
// {FUN_0011D460, FUN_0011AEF0, FUN_001239C0, FUN_00123FD0, stop-check,
//  FUN_00109560}, wheel prev restore, timers.  The FUN_0011AEF0 slot is the
// `chassis_resolve` hook (NULL = no world, the eax == 0 arm).
void b3_vehicle_step_full(B3VehicleFull* v, float throttle, float brake,
                          float steer, int boost, float dt);

// LSDM low-speed drive model, FUN_0011C7C0 (called by the force pass when
// speed*2.2374 < LsdmSpeedLimit or in reverse).
void b3_lsdm_update(B3VehicleFull* v, float dt);

// Rebuild the derived matrices (inverse frame transform + world inverse
// inertia = Rt.I0.R) after the frame rows/pos were edited externally
// (harness pose placement, collision push-out). The integrator refreshes
// them every step; this covers state edits between steps.
void b3_vehicle_full_refresh_derived(B3VehicleFull* v);

// Point a rigid body at its frame storage. Call AFTER any memset of the body.
//
// ZEROES the storage as well as binding it, and that is not optional: callers
// have always relied on `memset(&rb, 0, sizeof rb)` to clear the 4x4 along
// with everything else. Now that the matrix lives outside the struct, that
// memset no longer reaches it, so an unzeroed body inherits whatever was on
// the stack -- which showed up as an intermittent NaN in the physics about one
// run in three, with every suite still green.
static inline void b3_rigid_body_bind_frame(B3RigidBody* rb, float (*store)[4])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) store[r][c] = 0.0f;
    rb->frame = store;
}

// Declare a local body with its own storage, already bound. Use this instead
// of a bare `B3RigidBody x;` -- a bare one has a NULL frame and faults on the
// first row write.
#define B3_RIGID_BODY_LOCAL(name) \
    B3RigidBody name; float name##__frame_store[4][4]; \
    b3_rigid_body_bind_frame(&(name), name##__frame_store)

// FUN_00125790: build 6 crash-floor / barrier polygons at veh+0x11D0 and set +0x1351.
// mode is 1 if director zone type == 2, else 0.
void b3_vehicle_set_crash_floor(B3VehicleFull* v, const float points[6][4],
                                const float basis[3][4], int mode);
void b3_vehicle_clear_crash_floor(B3VehicleFull* v);

#endif // BURNOUT3_VEHICLE_SIM_H
