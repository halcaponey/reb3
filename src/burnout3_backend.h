// Per-feature backend selection: run the PORT's reverse-engineered C, or call
// the RETAIL code itself under emulation.
//
// Driven by build/backends.cfg -- one line per feature:
//
//     physics   retail
//     ai        re
//
// "re"     = this project's recovered C (the default, always available)
// "retail" = the game's own x86, executed under Unicorn via the sidecar in
//            tools/b3_emu_server.py
//
// A feature with no line, an unknown value, or an unavailable sidecar falls
// back to "re" and says so once on stdout. The file is read once at startup;
// B3_BACKENDS=<path> overrides its location.
#ifndef BURNOUT3_BACKEND_H
#define BURNOUT3_BACKEND_H

typedef enum { B3_BACKEND_RE = 0, B3_BACKEND_RETAIL = 1 } B3Backend;

// Feature ids. Keep in sync with B3_FEATURE_NAMES in the .c.
typedef enum {
    B3_FEAT_PHYSICS = 0,   // b3_vehicle_step_full  <-> FUN_0011ECF0 + BE50 chain
    B3_FEAT_CARCOL,        // car-vs-car            <-> FUN_001121F0 / FUN_00113960
    B3_FEAT_AI,            // AI driver             <-> FUN_00105340
    B3_FEAT_TRAFFIC,       // traffic driver/pool   <-> FUN_00105150 / FUN_001A6070
    B3_FEAT_TD_RULES,      // takedown + authority  <-> FUN_00197BE0 / FUN_00105BD0
    B3_FEAT_SCORE,         // score events          <-> FUN_00197920 / FUN_001979E0
    B3_FEAT_CRASH,         // crash entry/commit    <-> FUN_0011FC60 / FUN_0010DCA0
    B3_FEAT_CAMERA,        // follow + collide      <-> FUN_0015E550 / FUN_00162A90
    B3_FEAT_SFX,           // sfx selection         <-> FUN_00141010
    B3_FEAT_HUD,           // hud logic             <-> FUN_0004D310
    B3_FEAT_COUNT
} B3Feature;

// Reads build/backends.cfg (creating it with all-"re" defaults if absent).
// Safe to call more than once; only the first call does the work.
void      b3_backend_init(void);

// Force EVERY feature onto one backend, ignoring the cfg file.  Must be
// called before the first b3_backend_get()/init (main's CLI parse is the
// intended caller: --re / --retail / --backend=re|retail).
void      b3_backend_force_all(B3Backend b);
B3Backend b3_backend_get(B3Feature f);
const char* b3_backend_name(B3Feature f);

// True when the feature should run retail code AND the sidecar came up.
int       b3_backend_use_retail(B3Feature f);

// Called by the emulation layer when the sidecar fails, so a feature can be
// demoted to "re" mid-run without the caller having to care.
void      b3_backend_demote(B3Feature f, const char* why);

#endif // BURNOUT3_BACKEND_H
