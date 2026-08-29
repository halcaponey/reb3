/* =====================================================================
 *  burnout3_dj.h -- Crash FM, the radio DJ
 *
 *  The recovered announcer.  See docs/RE_CRASHFM.md for the evidence;
 *  every constant below is cited there by address.
 *
 *  The short version, because it is the opposite of what everyone
 *  assumes: retail's DJ does NOT talk over the music and is NOT
 *  triggered by gameplay.  Crash FM is one radio station.  It plays a
 *  song, waits out a 60-70 second timer, fades the song, plays a
 *  station ident jingle, plays exactly one speech line, and starts the
 *  next song.  No takedown, crash, overtake or lap fires a line --
 *  there is no code path in the executable that could (RE_CRASHFM.md
 *  section 3.1).  What follows implements that, and nothing more.
 *
 *  Ownership:
 *    b3_dj_tick()          MAIN thread.  Does all file I/O and all
 *                          selection.  Never called from audio.
 *    b3_dj_next_sample()   AUDIO thread.  Reads only what tick()
 *                          published; allocates nothing.
 *
 *  Kill switch: B3_DJ=0 disables the module completely -- tick()
 *  returns immediately and next_sample() returns 0.0f, so the mix is
 *  bit-identical to a build without it.
 * ===================================================================== */

#ifndef BURNOUT3_DJ_H
#define BURNOUT3_DJ_H

/* ---- retail radio modes (RE_CRASHFM.md section 3) -------------------
 * The mode selects both the state machine and the music gain.  Only
 * OFF, FRONTEND, LOADING, RACE and RESULTS are reachable in the
 * shipped build; mode 3 is guarded out everywhere. */
#define B3_DJ_MODE_OFF       0
#define B3_DJ_MODE_FRONTEND  1
#define B3_DJ_MODE_LOADING   4
#define B3_DJ_MODE_RACE      5
#define B3_DJ_MODE_RESULTS   6

/* ---- lifecycle ------------------------------------------------------ */

/* Read the switches, seed the PRNG, build the shuffle table.  Safe to
 * call more than once; only the first call does anything. */
void b3_dj_init(void);

/* Enter a mode.  Port of Radio::SetMode, FUN_00154110 @0x00154110.
 * Changing mode resets the selection state, exactly as retail does. */
void b3_dj_set_mode(int mode);

/* Tell the radio which track's E_DJRACE bank to use.  `track_id` is the
 * port's usual B3_TRACK spelling ("US_C3_V1"); the trailing _V1 is
 * stripped to reach the bank directory ("US_C3").  Calling this also
 * pre-materialises the bank, so the lazy WMA decode lands during the
 * load screen rather than 60 seconds into the race. */
void b3_dj_set_track(const char *track_id);

/* Suppress speech without tearing the module down -- retail's +0x62C.
 * Used for pause and for split screen (RE_CRASHFM.md section 5.3). */
void b3_dj_set_suppressed(int on);

/* ---- per-frame ------------------------------------------------------ */

/* MAIN thread, once per frame, with the frame delta in seconds.  Runs
 * the state machine: counts the cooldown down, chooses the bank and the
 * line, loads and resamples them, and publishes to the audio thread. */
void b3_dj_tick(float dt);

/* MAIN thread, once per frame, from OUTSIDE the sim's inner loop.
 * Hands the music back if the radio has stopped being ticked while it
 * held the bus -- a race ending mid-line would otherwise leave the song
 * stopped for good.  Cheap and idempotent; safe to call always. */
void b3_dj_release_if_idle(void);

/* AUDIO thread, once per output frame.  Returns the DJ contribution in
 * the mixer's s16-scaled float domain, already gained.  Sums the ident
 * jingle and the speech line -- they never overlap by construction, but
 * summing costs nothing and is safe if they ever do. */
float b3_dj_next_sample(void);

/* ---- mixing --------------------------------------------------------- */

/* The CRASH FM row in the pause mixer.  Multiplies the recovered
 * balance rather than replacing it, so 1.0 means "retail's levels". */
void  b3_dj_set_master(float g);
float b3_dj_master(void);

/* How much the music should be scaled while the DJ is speaking, 1.0 =
 * not at all.  The music module multiplies this in.  See the
 * B3_DJ_MUSIC_DUCK note in the .c -- retail never ducks, because in
 * retail the song has already stopped; this is the port's stand-in and
 * is marked [S] in the docs. */
float b3_dj_music_duck(void);

/* ---- determinism ---------------------------------------------------- */

/* Seed the radio PRNG.  Retail's seeds are 0xFD462907 / 0x02B9D6F8
 * (FUN_001214A0).  B3_DJ_SEED=<n> overrides at init.  Line selection is
 * a pure function of these two words plus the state machine, so two
 * runs with the same seed produce a byte-identical mix. */
void b3_dj_seed(unsigned state, unsigned inc);

/* ---- introspection, for the validators ------------------------------ */

typedef struct {
    int   mode;             /* B3_DJ_MODE_*                             */
    int   state;            /* retail state machine, +0x5F4             */
    int   speaking;         /* a speech line is audible right now       */
    int   ident;            /* the jingle is audible right now          */
    int   line;             /* current line index, -1 = none            */
    int   lo, hi;           /* the range it was drawn from              */
    int   ident_n;          /* next ident 0..9                          */
    float cooldown;         /* seconds until the next line              */
    float duck;             /* what music_duck() is returning           */
    unsigned lines_played;  /* since init                               */
    const char *bank;       /* current bank directory name              */
} B3DjStatus;

void b3_dj_status(B3DjStatus *out);

/* 1 if the module is enabled at all (B3_DJ != 0). */
int b3_dj_on(void);

#endif /* BURNOUT3_DJ_H */
