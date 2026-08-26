// Call RETAIL's own code, under emulation, from the live game.
//
// A sidecar process (tools/b3_emu_server.py) holds a warmed Unicorn session
// per car and runs the game's real per-frame vehicle pipeline -- FUN_0011ECF0
// plus FUN_0011BE50's substep chain -- against the retail vehicle struct. This
// header is the C side of that pipe.
//
// Selected per feature in build/backends.cfg; see burnout3_backend.h.
//
// COST (measured, steady state): ~1.3 ms per car per frame, against a 16.7 ms
// budget at 60 fps. A six-car grid is ~7.6 ms. The FIRST call for a car costs
// ~8.7 ms while Unicorn builds its translation cache, so the sidecar warms
// each car on creation rather than making the player feel it on the grid.
#ifndef BURNOUT3_EMU_H
#define BURNOUT3_EMU_H

typedef struct B3EmuState {
    float pos[3];
    float vel[3];
    float speed;
    float dir[3];
    float right[3];
    float up[3];
    float at[3];
    float omega[3];
    float rpm, gear, torque, steer_deg, drift, slide, airborne;
    float wheel_cur[4];
} B3EmuState;

#define B3_EMU_FIELDS 32

// Starts the sidecar. Returns 1 on success, 0 if it could not be started (no
// python3, no build/burnout3.elf, import failure). Safe to call repeatedly.
int  b3_emu_init(void);
void b3_emu_shutdown(void);
int  b3_emu_ready(void);

// Place a car's retail body. Call once before the first step for that car.
int  b3_emu_seed(int car, float x, float y, float z, float yaw);

// One retail frame. Returns 1 and fills `out` on success; 0 means the sidecar
// failed and the caller must fall back to the RE port for this frame.
int  b3_emu_step(int car, float throttle, float brake, float steer,
                 int boost, float dt, B3EmuState* out);

// Upload the polygon soup retail's chassis-vs-world resolve (FUN_0011AEF0)
// and wheel rays walk. `tris` is 13 floats each: v0 v1 v2 normal type, in
// GAME space. Re-send when the car crosses into new geometry -- not per frame.
int  b3_emu_soup(int car, const float* tris, int count);
#define B3_EMU_SOUP_CAP 256   /* must not exceed emulate_pipeline.SOUP_MAX */

// Largest vehicle window we will ship (retail's object is 0x1A00).
#define B3_EMU_WINDOW_MAX 0x2000

// Hand retail the port's OWN struct bytes and take them back. Byte-for-byte;
// no field is converted in either direction. `frame` is the 4x4, which travels
// separately because retail keeps it in its own object (v+0x204), not inline.
#include <stddef.h>
// Hand retail our RECOVERED ranges and take them back -- same offset on both
// sides, nothing converted, and the parts of retail's object we have not
// recovered are left exactly as the emulator seeded them.
/* Push the port's vehicle state across ONCE, when retail takes the wheel.
   Uses B3_VEHICLE_STATE_RANGES -- no field retail rebuilds or clears. */
int  b3_emu_handover(int car, const void* vehicle, const float frame[4][4]);

int  b3_emu_step_ranges(int car, float throttle, float brake, float steer,
                        int boost, float dt, void* vehicle, float frame[4][4]);

// Car-vs-car through retail's own resolver, over the port's bytes.
#define B3_EMU_RB_LEN    0x148u   /* sizeof B3RigidBody's retail window */
#define B3_EMU_EXTRA_LEN 36u      /* bbmax[4] bbmin[4] mass */
#define B3_EMU_HULL_LEN  0x600u   /* the retail hull record */
int  b3_emu_carcol(const void* rbA, const void* extraA, const void* hullA,
                   const float frameA[4][4], int crashedA, int typeA,
                   const void* rbB, const void* extraB, const void* hullB,
                   const float frameB[4][4], int crashedB, int typeB,
                   void* out_contact, void* out_rbA, void* out_rbB,
                   int* out_crash_a, int* out_crash_b);

// Retail's own slam gate over the port's racecar bytes. Returns the verdict,
// or -1 if the sidecar is unavailable (caller falls back to the RE path).
/* clock: the race clock, a GLOBAL in the emulated world, not a struct field
 * -- without it retail stamps aggressor_time = 0.00 and the port's crash-wait
 * window (clock <= stamp + MAX_CRASH_WAIT) can never pass. */
int  b3_emu_td_slam(float clock, int ncars, int attacker, int victim, float strength,
                    int type_byte, void* cars, size_t car_stride);

// Retail's own score-event entries over the port's score object.
// `which` is "contact" (FUN_00197920) or "mark" (FUN_001979E0).
int  b3_emu_score(const char* which, int arg, void* score);

// Retail's own follow camera (FUN_0015E550).
int  b3_emu_cam(const float car_rows[12], float speed_ms, float boost_ramp,
                float dt, float yaw_deg, float pitch_deg, int look_back,
                float out_eye[3], float out_quat[4],
                float* out_fov, float* out_pitch, float* out_yaw);

// Retail's own AI driver (FUN_00105340) over the port's gather structs.
/* which: 0 = FUN_00105340 (AI racer driver), 1 = FUN_00105150 (traffic) */
/* clock/dt are the RUNTIME globals DAT_0060EA20 / DAT_0060EA1C, not struct
 * fields, so they cannot ride the scatter transfer -- they go on the line.
 * FUN_00105340's launch dither stores a DEADLINE (clock + 2.0) in v+0x1574
 * and cuts the throttle until the clock passes it, so a session whose clock
 * never advances cuts the throttle forever. */
int  b3_emu_ai_drive(int which, void* car, void* state, void* veh, float clock,
                     float dt,
                     const float frame[4][4]);
extern unsigned long g_emu_ai_calls;  /* completed retail driver calls */
int  b3_emu_chassis_resolve(int car, void* veh, int* out_n);
extern unsigned long g_emu_crash_calls;
extern unsigned long g_emu_traffic_calls;

/* The HUD event ticker. recs[i] = one B3CatRecord in B3HudTickIn field order
   (value, clock, prev, open, tier, prev_tier, count); rows_out[i] = one row
   slot (live, timer, y, tier, flash, phase, pulse); order_out = the live
   rows newest-first. */
int  b3_emu_hud_tick(float dt, const float recs[6][7],
                     float rows_out[7][7], int order_out[7], int* n_out);
extern unsigned long g_emu_hud_calls;

/* One voice as the game's own emitter asked for it, captured at the
   PlaySound3D boundary. */
typedef struct { char wave[16]; float gain, pitch; } B3EmuVoice;
int  b3_emu_sfx_fire(unsigned addr, int kind, float mag,
                     B3EmuVoice out_v[4], int* n_out);
extern unsigned long g_emu_sfx_calls;

/* ai=retail AND physics=retail: run FUN_00105340 and the step in ONE session,
   the way retail does. aicar is a B3AiCar, aistate a B3AiState. */
int  b3_emu_step_ai(int car, int boost, float dt,
                    const void* aicar, const void* aistate,
                    void* vehicle, float frame[4][4]);

/* Traffic spawn choices. Both advance the shared manager RNG in place.
   Return the chosen index, or -2 if retail could not be reached. */
int  b3_emu_traffic_model(unsigned* rng_state, unsigned* rng_carry, int cls,
                          unsigned total, const unsigned* weights, unsigned n);
int  b3_emu_traffic_paint(unsigned* rng_state, unsigned* rng_carry,
                          const unsigned char colours[8]);

// 0 until the port models the whole retail vehicle object -- see the .c.
int  b3_emu_window_ready(void);

int  b3_emu_step_window(int car, float throttle, float brake, float steer,
                        int boost, float dt, void* window, size_t win_len,
                        float frame[4][4]);

// Rolling cost of b3_emu_step, milliseconds, for the HUD/telemetry.
double b3_emu_last_ms(void);
double b3_emu_avg_ms(void);

#endif // BURNOUT3_EMU_H
