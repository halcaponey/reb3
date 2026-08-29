/* =====================================================================
 *  burnout3_dj.c -- Crash FM, the radio DJ
 *
 *  Port of retail's radio object (statically at 0x004115E8) restricted
 *  to the parts that make the DJ speak.  docs/RE_CRASHFM.md carries the
 *  decompilation and the address for every constant here; the section
 *  numbers in the comments below point into it.
 *
 *  What is [C] (recovered) and what is [S] (glue) is marked inline and
 *  summarised in RE_CRASHFM.md section 7.  The short list of glue:
 *  48000->44100 resampling, stereo->mono downmix, lazy per-bank
 *  materialisation, a private voice, a seedable PRNG, and the music
 *  duck -- retail needs none of these because its mixer takes native
 *  rates, it streams off the disc, and it stops the song before the DJ
 *  opens his mouth.
 * ===================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "burnout3_dj.h"
#include "burnout3_isodata.h"

/* The bank is materialised lazily by the iso layer.  In build mode
 * there is nothing to ask, so the probe is weak and answers "unknown";
 * isodata.c provides the strong definition when it is linked in. */
int b3_iso_dj_available(const char *bank) __attribute__((weak));
int b3_iso_dj_available(const char *bank) { (void)bank; return -1; }

/* The music module, for the hand-over.  Weak so the validator's probe
 * can link src/burnout3_dj.c on its own; burnout3_music.c provides the
 * strong definitions in the real build. */
void b3_music_set_hold(int on) __attribute__((weak));
void b3_music_set_hold(int on) { (void)on; }
int  b3_music_skip(void) __attribute__((weak));
int  b3_music_skip(void) { return -1; }
int  b3_music_current(void) __attribute__((weak));
int  b3_music_current(void) { return -1; }

/* ===================================================================== *
 *  Recovered constants -- RE_CRASHFM.md sections 4, 5 and 6
 * ===================================================================== */

#define DJ_RATE          44100          /* the port's mixer rate         */
#define DJ_SHUF_N        0x26           /* 38: the per-track bank's count,
                                         * and retail's table size [C]   */

/* Cooldown between lines, uniform on [60, 70) seconds.  FUN_00153BE0
 * @0x153C90 and FUN_00154A70 @0x154EEB: rand01() * 10.0 + 60.0 [C] */
#define DJ_COOLDOWN_BASE 60.0f
#define DJ_COOLDOWN_SPAN 10.0f

/* Gains, .rdata @0x003EC920 [C] */
#define DJ_GAIN_SPEECH   1.000f         /* DAT_003EC928                  */
#define DJ_GAIN_IDENT    0.850f         /* DAT_003EC92C                  */
#define DJ_GAIN_MUSIC_FE 0.455f         /* DAT_003EC930, frontend        */
#define DJ_GAIN_MUSIC_RC 0.600f         /* DAT_003EC934, modes 4/5/6     */

/* The per-path speech multiplier, .rdata @0x003EC958 [C]: the in-race
 * state machine plays at 1.00, the frontend and loading machines at
 * 0.75. */
#define DJ_MUL_RACE      1.00f
#define DJ_MUL_OTHER     0.75f

/* THE MUSIC HAND-OVER [C].  Retail does not duck under the DJ -- it
 * SEQUENCES.  The song is advanced out of the way before he speaks and
 * the NEXT song starts once he has finished:
 *
 *   FUN_00154A70 state 0xB  -> FUN_00153390   advance the playlist
 *                 state 3   -> ident
 *                 states 4-8-> the speech line
 *                 state 9   -> FUN_00153230   start the next song
 *   FUN_00154800 (mode 4)   -> FUN_00153310   stop, then the intro line
 *
 * So the bus is handed over, not shared, and there is no gain law to
 * recover because there is no moment when both are audible.
 *
 * B3_DJ_DUCK=1 restores the old behaviour -- the song kept playing
 * under the line at 0.455/0.600 = 0.7583.  That number was a recovered
 * RATIO put to a use retail never put it to; it is kept only because
 * retiring it costs nothing and someone may want to A/B it. [S] */
#define DJ_MUSIC_DUCK    (DJ_GAIN_MUSIC_FE / DJ_GAIN_MUSIC_RC)

/* Retail will not cut a song that has only just started: FUN_00154A70
 * @0x00154C40 re-arms the cooldown instead when FUN_00153580() reports
 * under 20000.  The 20000 is [C]; reading it as SECONDS is [S] -- the
 * unit is stream+0x238 * stream+0x20 and was not pinned down. */
#define DJ_SONG_MIN_S    20.0f

/* How fast the duck slews, seconds.  Pure glue; matches the music
 * module's own B3_MUSIC_DUCK_SLEW so the two feel the same. */
#define DJ_DUCK_SLEW     0.20f

/* Retail's PRNG seeds, FUN_001214A0 [C] */
#define DJ_SEED_STATE    0xFD462907u
#define DJ_SEED_INC      0x02B9D6F8u

/* ===================================================================== *
 *  The bank inventory -- RE_CRASHFM.md section 1.3
 *
 *  Entry counts are read off the disc and agree with the executable's
 *  own loop bounds.  `entries` is advisory: the real count is taken
 *  from the directory at load time, exactly as retail takes it from
 *  the bank header (+0x604).  It is here so a missing bank is
 *  recognisable rather than silently empty.
 * ===================================================================== */

#define DJ_BANK_RACE     0              /* the per-track E_DJRACE        */
#define DJ_BANK_GEN      1
#define DJ_BANK_WWW      2
#define DJ_BANK_REGION   3
#define DJ_BANK_MODE     4

static const struct { const char *dir; int entries; } DJ_FLAT[] = {
    { "DJGEN",  18 },                   /* generic station chatter       */
    { "DJWWW",  10 },                   /* web plugs                     */
    { "DJUS",   20 },                   /* region -- shipped disc is US  */
};
#define DJ_FLAT_GEN  0
#define DJ_FLAT_WWW  1
#define DJ_FLAT_REG  2

/* Game mode -> djm* bank, FUN_001551E0 @0x001551E0 [C].  The port has
 * no session object to ask, so the mode is whatever the caller last
 * set; Race is the resting value. */
static const char *const DJ_MODE_BANK[] = {
    "DJMRA",    /* 0 Race        */
    "DJMEL",    /* 1 Eliminator  */
    "DJMRA",    /* 2 (unused)    */
    "DJMRR",    /* 3 Road Rage   */
    "DJMBL",    /* 4 Burning Lap */
    "DJMRA",    /* 5 (unused)    */
    "DJMCR",    /* 6 Crash       */
};
#define DJ_MODE_BANK_N ((int)(sizeof DJ_MODE_BANK / sizeof DJ_MODE_BANK[0]))

/* The three race sets, FUN_001550B0 @0x001550B0 [C].  22 + 8 + 8 = 38,
 * exhausting the bank. */
static const struct { int lo, hi; } DJ_RACE_SET[3] = {
    { 0x00, 0x16 },                     /* set 0 -- 22 lines             */
    { 0x16, 0x1E },                     /* set 1 --  8 lines             */
    { 0x1E, 0x26 },                     /* set 2 --  8 lines             */
};

/* ===================================================================== *
 *  State
 * ===================================================================== */

/* retail's state machine, +0x5F4.  We keep retail's numbering so the
 * states line up with RE_CRASHFM.md section 5. */
#define S_MUSIC       0
#define S_IDENT_WAIT  1
#define S_PLAYING     2
#define S_IDENT_PRE   3
#define S_PICK        4
#define S_LOAD        5
#define S_READY       6
#define S_SPEAK       7
#define S_DRAIN       8
#define S_POST        9
#define S_COOLDOWN    0x0B
#define S_IDLE        0x0D

/* one decoded clip, 44100 mono */
typedef struct {
    short   *pcm;
    unsigned frames;
} DjClip;

static int      g_on = -1;              /* B3_DJ, latched                */
static int      g_log;                  /* B3_DJ_LOG                     */
static int      g_inited;

/* B3_DJ_COOLDOWN=<seconds> -- A TEST HOOK, not a tuning knob.  Retail's
 * gap is uniform on [60, 70) s (RE_CRASHFM.md section 5.1) and that is
 * what ships.  But a 60 s wait is longer than any offscreen capture the
 * gates can afford, so the validators pin it short to make the line
 * land inside a 25 s recording.  Unset, the recovered law is used and
 * the span is restored. */
static float    g_cd_base = DJ_COOLDOWN_BASE;
static float    g_cd_span = DJ_COOLDOWN_SPAN;

static char     g_dir[192] = "build/audio";
static char     g_track[32];            /* bank dir, e.g. "US_C3"        */
static char     g_bank[32];             /* the bank we drew from         */

static int      g_mode = B3_DJ_MODE_OFF;
static int      g_state = S_IDLE;
static int      g_suppressed;

/* selection, retail's +0x600 / +0x608 / +0x60C / +0x615 / +0x614 */
static int      g_line = -1;
static int      g_lo, g_hi;
static unsigned char g_shuf[DJ_SHUF_N];
static int      g_shuf_i;
static int      g_ident_n;

static float    g_cooldown = DJ_COOLDOWN_BASE;
static float    g_clock;                /* sum of every dt tick() saw   */
static float    g_song_since;           /* clock when the song started  */
static int      g_duck_mode;            /* B3_DJ_DUCK=1: share, not seq */
static int      g_holding;              /* we stopped the music         */
static int      g_ticked;               /* tick() ran since the last
                                         * watchdog sweep               */
static float    g_master = 1.0f;
static unsigned g_lines;

/* the two clips and the audio-thread cursors.  The main thread only
 * writes `pcm`/`frames` while the corresponding `_play` flag is 0, and
 * the audio thread only reads them while it is 1, so the handoff needs
 * no lock -- the same discipline the crash bed uses. */
static DjClip   g_speech, g_identc;
static volatile int      g_speech_play, g_ident_play;
static volatile unsigned g_speech_pos,  g_ident_pos;
static float    g_speech_gain = DJ_GAIN_SPEECH;

/* the duck, slewed on the audio thread toward what tick() asks for */
static volatile float g_duck_target = 1.0f;
static float          g_duck = 1.0f;

/* ---- the radio PRNG, RE_CRASHFM.md section 4.1 [C] ------------------ */
static unsigned g_rx = DJ_SEED_STATE;
static unsigned g_ry = DJ_SEED_INC;

static unsigned dj_rng(void) {
    g_rx = g_rx * 0x10000u + (g_rx >> 16) + g_ry;
    g_ry = g_ry + g_rx;
    return g_rx;
}

static float dj_rand01(void) {
    return (float)((double)dj_rng() * 2.3283064365386963e-10);
}

void b3_dj_seed(unsigned state, unsigned inc) {
    g_rx = state ? state : DJ_SEED_STATE;
    g_ry = inc;
}

/* ===================================================================== *
 *  Switches
 * ===================================================================== */

static void dj_switches(void) {
    const char *e;
    if (g_on >= 0) return;              /* read once, latched            */
    e = getenv("B3_DJ");
    g_on = (e && *e) ? (atoi(e) != 0) : 1;
    g_log = getenv("B3_DJ_LOG") ? 1 : 0;
    e = getenv("B3_AUDIO_DIR");
    if (e && *e) snprintf(g_dir, sizeof g_dir, "%s", e);
    e = getenv("B3_DJ_SEED");
    if (e && *e) {
        unsigned s = (unsigned)strtoul(e, NULL, 0);
        b3_dj_seed(s, DJ_SEED_INC);
    }
    e = getenv("B3_DJ_COOLDOWN");
    if (e && *e) { g_cd_base = (float)atof(e); g_cd_span = 0.0f; }
    e = getenv("B3_DJ_DUCK");
    g_duck_mode = (e && *e) ? (atoi(e) != 0) : 0;
}

/* ---- the hand-over, RE_CRASHFM.md section 6.3 [C] ------------------- */

/* Take the bus: retail's FUN_00153390 (advance) / FUN_00153310 (stop). */
static void dj_music_hold(void) {
    if (g_duck_mode || g_holding) return;
    g_holding = 1;
    b3_music_set_hold(1);
}

/* Give it back by starting the NEXT song -- retail's FUN_00153230 after
 * the playlist was advanced, which is why this is skip() and not a
 * resume of the song that was interrupted. */
static void dj_music_release(void) {
    if (!g_holding) return;
    g_holding = 0;
    b3_music_set_hold(0);
    b3_music_skip();
    g_song_since = g_clock;
}

int b3_dj_on(void) { dj_switches(); return g_on; }

/* ===================================================================== *
 *  The shuffle table -- RE_CRASHFM.md section 4.2 [C]
 * ===================================================================== */

/* Fisher-Yates with the radio PRNG, FUN_001556C0 @0x001556C0. */
static void dj_shuffle(void) {
    for (int i = 0; i < DJ_SHUF_N; i++) {
        int j = (int)(dj_rng() % (unsigned)(DJ_SHUF_N - i)) + i;
        unsigned char t = g_shuf[j]; g_shuf[j] = g_shuf[i]; g_shuf[i] = t;
    }
}

/* FUN_001553D0 @0x001553D0: reseed the cursor, then shuffle. */
static void dj_shuffle_reseed(void) {
    g_shuf_i = (int)(dj_rng() % DJ_SHUF_N);
    dj_shuffle();
}

/* The pick, FUN_00154A70 @0x154D4A [C].  Cyclic walk of the table with
 * EXACTLY ONE retry if the draw repeats the previous line -- not a
 * loop.  A second collision is accepted, and reproducing that is the
 * difference between parity and "close enough". */
static int dj_pick(int lo, int hi) {
    int span = hi - lo;
    int prev = g_line;
    int cur;
    if (span <= 0) return lo;
    g_shuf_i = (g_shuf_i + 1) % DJ_SHUF_N;
    cur = g_shuf[g_shuf_i] % span + lo;
    if (cur == prev) {
        g_shuf_i = (g_shuf_i + 1) % DJ_SHUF_N;
        cur = g_shuf[g_shuf_i] % span + lo;
    }
    return cur;
}

/* ===================================================================== *
 *  WAV loading -- GLUE
 *
 *  The extraction stage writes each entry at its native 48000/stereo.
 *  The mixer wants 44100 mono.  We do both conversions here, once, on
 *  the main thread, with the same linear cursor the crash bed uses for
 *  its 32000 -> 44100 beds (burnout3_music.c crash_pump).
 * ===================================================================== */

static unsigned rd_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void clip_free(DjClip *c) {
    if (c->pcm) free(c->pcm);
    c->pcm = NULL;
    c->frames = 0;
}

/* Load a RIFF/WAVE, downmix to mono, resample to 44100.  Returns 0 on
 * any problem -- a missing line is not fatal, the radio just stays
 * quiet, which is also what retail does when a bank fails to open. */
static int clip_load(const char *path, DjClip *out) {
    unsigned char hdr[12], ch[8];
    unsigned rate = 0, chans = 0, bits = 0;
    long data_off = -1; unsigned data_len = 0;
    FILE *f;

    out->pcm = NULL; out->frames = 0;

    f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(hdr, 1, 12, f) != 12
        || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return 0;
    }
    /* walk the chunks; the extractor emits a LIST after fmt, so we
     * cannot assume data comes second */
    while (fread(ch, 1, 8, f) == 8) {
        unsigned len = rd_u32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fm[16];
            unsigned want = len < 16 ? len : 16;
            if (fread(fm, 1, want, f) != want) break;
            if (want >= 16) {
                chans = rd_u16(fm + 2);
                rate  = rd_u32(fm + 4);
                bits  = rd_u16(fm + 14);
            }
            if (len > want) fseek(f, (long)(len - want), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            data_off = ftell(f);
            data_len = len;
            break;
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
        if (len & 1) fseek(f, 1, SEEK_CUR);
    }
    if (data_off < 0 || bits != 16 || !chans || !rate) { fclose(f); return 0; }

    {
        unsigned src_frames = data_len / (2u * chans);
        double   ratio = (double)rate / (double)DJ_RATE;
        unsigned dst_frames, i;
        short   *src, *dst;
        double   frac = 0.0;
        unsigned si = 0;

        if (!src_frames) { fclose(f); return 0; }
        dst_frames = (unsigned)((double)src_frames / ratio);
        if (!dst_frames) { fclose(f); return 0; }

        src = (short *)malloc((size_t)src_frames * chans * sizeof(short));
        dst = (short *)malloc((size_t)dst_frames * sizeof(short));
        if (!src || !dst) { free(src); free(dst); fclose(f); return 0; }

        fseek(f, data_off, SEEK_SET);
        if (fread(src, 2, (size_t)src_frames * chans, f)
                != (size_t)src_frames * chans) {
            free(src); free(dst); fclose(f); return 0;
        }
        fclose(f);

        /* downmix in place to mono, then linear-resample */
        if (chans > 1) {
            for (i = 0; i < src_frames; i++) {
                long acc = 0;
                for (unsigned c = 0; c < chans; c++)
                    acc += src[i * chans + c];
                src[i] = (short)(acc / (long)chans);
            }
        }
        for (i = 0; i < dst_frames; i++) {
            unsigned a = si, b = (si + 1 < src_frames) ? si + 1 : si;
            float va = (float)src[a], vb = (float)src[b];
            dst[i] = (short)(va + (vb - va) * (float)frac);
            frac += ratio;
            while (frac >= 1.0) { frac -= 1.0; si++; }
            if (si >= src_frames) { dst_frames = i + 1; break; }
        }
        free(src);
        out->pcm = dst;
        out->frames = dst_frames;
        return 1;
    }
}

/* How many NNN.wav files a bank directory holds.  Retail reads the
 * count out of the bank header (+0x604); we count files, which is the
 * same number by construction. */
static int bank_entries(const char *bank) {
    int n = 0;
    for (;;) {
        char p[320];
        FILE *f;
        snprintf(p, sizeof p, "%s/%s/%03d.wav", g_dir, bank, n);
        f = fopen(p, "rb");
        if (!f) break;
        fclose(f);
        n++;
        if (n > 512) break;              /* paranoia                     */
    }
    return n;
}

/* ===================================================================== *
 *  Bank selection -- RE_CRASHFM.md section 2.3
 * ===================================================================== */

/* FUN_001551E0 @0x001551E0 -- game mode to djm* bank [C].  Retail asks
 * the session object; we use the radio mode we were handed, which for
 * the port is always the Race entry. */
static const char *dj_bank_mode(void) {
    int m = 0;                           /* Race                         */
    return DJ_MODE_BANK[m < DJ_MODE_BANK_N ? m : 0];
}

/* FUN_001554A0 @0x001554A0 -- the idle selector [C].
 *   rng & 0xF == 0x0  -> DJWWW,   whole bank   (1/16)
 *              == 0x7  -> DJGEN,   whole bank   (1/16)
 *              == 0xF  -> djm*,    whole bank   (1/16)
 *   otherwise          -> per-track race set 0  (13/16) */
static void dj_select_idle(char *bank, int cap, int *lo, int *hi) {
    unsigned r = dj_rng() & 0xFu;
    *lo = 0; *hi = 0;                    /* 0,0 = whole bank             */
    if (r == 0x0u)      snprintf(bank, (size_t)cap, "%s", DJ_FLAT[DJ_FLAT_WWW].dir);
    else if (r == 0x7u) snprintf(bank, (size_t)cap, "%s", DJ_FLAT[DJ_FLAT_GEN].dir);
    else if (r == 0xFu) snprintf(bank, (size_t)cap, "%s", dj_bank_mode());
    else {
        snprintf(bank, (size_t)cap, "%s", g_track);
        *lo = DJ_RACE_SET[0].lo;
        *hi = DJ_RACE_SET[0].hi;
    }
}

/* FUN_00155590 @0x00155590 -- the loading selector [C].  FUN_00017310()
 * is [?] and treated as false, which is the common case. */
static void dj_select_loading(char *bank, int cap, int *lo, int *hi) {
    *lo = 0; *hi = 0;
    if (dj_rng() & 1u) {
        snprintf(bank, (size_t)cap, "%s", dj_bank_mode());
    } else {
        snprintf(bank, (size_t)cap, "%s", g_track);
        *lo = DJ_RACE_SET[0].lo;
        *hi = DJ_RACE_SET[0].hi;
    }
}

/* FUN_00155630 @0x00155630 -- the results selector [C].  DAT_00550886
 * is [?] and treated as false. */
static void dj_select_results(char *bank, int cap, int *lo, int *hi) {
    *lo = 0; *hi = 0;
    if ((dj_rng() & 7u) == 0u) {
        snprintf(bank, (size_t)cap, "%s", DJ_FLAT[DJ_FLAT_REG].dir);
    } else {
        snprintf(bank, (size_t)cap, "%s", g_track);
        *lo = DJ_RACE_SET[2].lo;
        *hi = DJ_RACE_SET[2].hi;
    }
}

static void dj_select(char *bank, int cap, int *lo, int *hi) {
    switch (g_mode) {
    case B3_DJ_MODE_LOADING: dj_select_loading(bank, cap, lo, hi); break;
    case B3_DJ_MODE_RESULTS: dj_select_results(bank, cap, lo, hi); break;
    default:                 dj_select_idle(bank, cap, lo, hi);    break;
    }
}

/* ===================================================================== *
 *  Lifecycle
 * ===================================================================== */

void b3_dj_init(void) {
    dj_switches();
    if (g_inited) return;
    g_inited = 1;
    /* the constructor's identity fill, FUN_00153B00 @0x00153B00 [C] */
    for (int i = 0; i < DJ_SHUF_N; i++) g_shuf[i] = (unsigned char)i;
    dj_shuffle_reseed();
    g_cooldown = g_cd_base + dj_rand01() * g_cd_span;
    snprintf(g_track, sizeof g_track, "%s", "US_C3");
}

void b3_dj_set_track(const char *track_id) {
    size_t n;
    b3_dj_init();
    /* A NULL id means "whatever the port defaulted to" -- it must still
     * fall through to the prefetch below.  Returning early here cost a
     * 2.4 s materialise stall in the middle of a wasm race, because
     * B3_TRACK is normally unset and the bank was then first touched
     * from the state machine sixty seconds in. */
    if (track_id && *track_id) {
        snprintf(g_track, sizeof g_track, "%s", track_id);
        /* the bank directory is the track id without the _V1 revision */
        n = strlen(g_track);
        if (n > 3 && !strcmp(g_track + n - 3, "_V1")) g_track[n - 3] = '\0';
    }

    dj_switches();
    if (!g_on) return;
    /* PREFETCH EVERY BANK THE IDLE SELECTOR CAN REACH, here on the load
     * path, so nothing decodes during a race.
     *
     * The per-track bank is 13/16 of the draws, but the other three
     * sixteenths -- DJWWW, DJGEN and the mode bank -- were decoding
     * mid-race the first time each came up, which is a stall on the
     * frame thread and, before the loading screen learned better, a
     * loading screen over a live race.  They are small: the four
     * together cost about as much as the per-track bank alone.
     *
     * fopen() goes through the iso shim, which materialises the unit. */
    {
        const char *banks[4];
        int n = 0, i;
        banks[n++] = g_track;
        banks[n++] = DJ_FLAT[DJ_FLAT_WWW].dir;
        banks[n++] = DJ_FLAT[DJ_FLAT_GEN].dir;
        banks[n++] = dj_bank_mode();
        for (i = 0; i < n; i++) {
            char p[320];
            FILE *f;
            if (!banks[i] || !*banks[i]) continue;
            if (b3_iso_dj_available(banks[i]) == 0) continue;
            snprintf(p, sizeof p, "%s/%s/000.wav", g_dir, banks[i]);
            f = fopen(p, "rb");
            if (f) fclose(f);
        }
    }
}

void b3_dj_set_mode(int mode) {
    dj_switches();
    if (!g_on) return;
    if (mode == g_mode) return;
    g_mode = mode;
    /* FUN_00154110: a mode change resets the selection state [C].
     *
     * NOTE the clips are NOT freed here.  The audio thread may be inside
     * next_sample() holding g_speech.pcm, and clearing the flag does not
     * evict it -- it may already be past the test.  The buffers are only
     * ever freed in S_LOAD, by which point the previous line has been
     * finished for a whole cooldown, which is the one moment no reader
     * can be in flight.  That is what makes the handoff lock-free. */
    g_line = -1;
    g_lo = g_hi = 0;
    g_speech_play = 0;
    g_speech_pos = 0;
    if (mode == B3_DJ_MODE_OFF) {
        g_state = S_IDLE;
        g_ident_play = 0;
        g_duck_target = 1.0f;
        /* the radio is going away -- do not leave the song stopped
         * behind it.  Pause does NOT come through here (that is
         * suppression, which keeps the mode), so this cannot fire a
         * spurious track change every time the player hits Escape. */
        dj_music_release();
    } else if (mode == B3_DJ_MODE_LOADING) {
        /* THE RACE-START INTRO [C].  Mode 4's machine, FUN_00154800
         * @0x00154800, has NO cooldown gate at all: case 0 stops the
         * music, case 0xB falls straight through to case 4, and case 4
         * starts the ident and picks a line.  Entering mode 4 therefore
         * speaks IMMEDIATELY, whatever the timer says.
         *
         * The game enters it on the pre-race branch of FUN_00026D30 --
         *   0002703F  B8 04000000   MOV EAX, 4
         *             B9 E8154100   MOV ECX, 0x004115E8   ; the radio
         *             E8 CCD01200   CALL 0x00154110       ; Radio::SetMode
         * -- and only later, when the session state becomes 2, sets mode
         * 5 for the between-songs cycle.  So retail's DJ introduces the
         * event and THEN the music starts, which is what a player
         * remembers as "he talks at the start of the race". */
        g_state = S_IDENT_PRE;
    } else {
        g_state = S_COOLDOWN;
    }
    if (mode != B3_DJ_MODE_OFF) {
        /* the event is starting and so is its first song -- the "do not
         * cut a song that has only just started" gate is measured from
         * here, not from process start */
        g_song_since = g_clock;
    }
    /* NOTE what is NOT here: a cooldown reset.  The entire executable
     * writes +0x610 in exactly three places -- FUN_00153BE0 @0x00153CCA
     * (boot, once) and FUN_00154A70 @0x00154C5F / @0x00154EF9 (after a
     * line).  No mode change touches it.  Resetting it here, which this
     * module used to do, restarted the 60-70 s wait on every mode change
     * and was pure invention. [C] */
    if (g_log)
        printf("[dj] mode %d, cooldown %.1fs\n", mode, (double)g_cooldown);
}

void b3_dj_set_suppressed(int on) { g_suppressed = on ? 1 : 0; }

/* THE STUCK-MUSIC WATCHDOG.  b3_dj_tick() only runs while the sim's
 * inner loop runs -- racing or crashed.  A race that ENDS while the
 * radio happens to be holding the bus would leave the song stopped for
 * ever, because the state machine that would have released it is no
 * longer being called.
 *
 * Called every frame from outside that loop.  A frame that saw a tick
 * just re-arms; two sweeps with no tick between them mean the radio has
 * genuinely stopped being driven, and the music is handed back.  The
 * one-frame hysteresis is what keeps it from firing in the gap between
 * taking the bus and starting the line, when nothing is audible yet. */
void b3_dj_release_if_idle(void) {
    if (!g_holding) return;
    if (g_ticked) { g_ticked = 0; return; }
    dj_music_release();
}

void  b3_dj_set_master(float g) {
    if (g < 0.0f) g = 0.0f;
    if (g > 4.0f) g = 4.0f;
    g_master = g;
}
float b3_dj_master(void) { return g_master; }
float b3_dj_music_duck(void) { return g_duck; }

/* ===================================================================== *
 *  The state machine -- RE_CRASHFM.md section 5
 * ===================================================================== */

/* FUN_001543B0 @0x001543B0 -- the ident jingle.  Sequential 0..9,
 * wrapping at 10, NOT random [C].  The extraction stage lands them at
 * awd_Ident<N>/ident.wav. */
static int dj_ident_load(void) {
    char p[320];
    /* Free the PREVIOUS jingle here, not when it stopped: clip_load()
     * overwrites the pointer, so without this every line leaked one
     * ident buffer (~180 KB).  This is the same safe moment S_LOAD uses
     * for the speech clip -- a whole cooldown after the last reader. */
    clip_free(&g_identc);
    snprintf(p, sizeof p, "%s/awd_Ident%d/ident.wav", g_dir, g_ident_n);
    g_ident_n++;
    if (g_ident_n == 10) g_ident_n = 0;   /* retail's wrap [C]           */
    return clip_load(p, &g_identc);
}

void b3_dj_tick(float dt) {
    dj_switches();
    if (!g_on) return;
    if (!g_inited) b3_dj_init();
    g_ticked = 1;                       /* for b3_dj_release_if_idle()  */
    /* The radio's own clock, stamped into the B3_DJ_LOG line.  It exists
     * because the gap between lines was being INFERRED from soak wall
     * time, which is not the clock the cooldown runs on -- the radio
     * ticks while the player is wrecked and g_race_time does not, so the
     * two legitimately diverge.  Reading the radio's own accumulator
     * settles that directly.  See RE_CRASHFM.md section 7.1. */
    g_clock += dt;

    /* Suppression, RE_CRASHFM.md section 5.3 [C].  Retail short-circuits
     * the machine to the idle state; so do we, and we release the duck
     * so a pause never leaves the music quiet. */
    if (g_suppressed || g_mode == B3_DJ_MODE_OFF) {
        if (g_speech_play || g_ident_play) {
            g_speech_play = g_ident_play = 0;
            g_state = S_POST;
        }
        g_duck_target = 1.0f;
        return;
    }

    switch (g_state) {

    case S_COOLDOWN:
        /* the only trigger there is: a timer [C] */
        g_cooldown -= dt;
        if (g_cooldown > 0.0f) break;
        /* ...but retail will not cut a song that has only just started:
         * FUN_00154A70 @0x00154C40 re-arms the cooldown instead. [C] */
        if (!g_duck_mode && b3_music_current() >= 0
            && g_clock - g_song_since < DJ_SONG_MIN_S) {
            g_cooldown = g_cd_base + dj_rand01() * g_cd_span;
            if (g_log)
                printf("[dj] song only %.1fs old, waiting %.1fs\n",
                       (double)(g_clock - g_song_since), (double)g_cooldown);
            break;
        }
        g_state = S_IDENT_PRE;
        /* fall through -- retail reaches state 3 in the same frame */

    case S_IDENT_PRE:
        /* Take the bus BEFORE the jingle, exactly where retail advances
         * the playlist on its way from state 0xB to state 3. [C] */
        dj_music_hold();
        if (!dj_ident_load()) {           /* no jingle on disc: skip it   */
            g_state = S_PICK;
            break;
        }
        g_ident_pos = 0;
        g_ident_play = 1;
        g_state = S_PICK;
        break;

    case S_PICK: {
        int n;
        dj_select(g_bank, (int)sizeof g_bank, &g_lo, &g_hi);
        n = bank_entries(g_bank);
        if (n <= 0) {                     /* bank missing -- try again    */
            if (g_log) printf("[dj] bank %s empty under %s\n", g_bank, g_dir);
            g_state = S_POST;
            break;
        }
        /* hi == 0 is retail's "whole bank" sentinel; the consumer
         * rewrites it from the bank's real entry count [C] */
        if (g_hi <= 0) g_hi = n;
        if (g_hi > n) g_hi = n;
        if (g_lo >= g_hi) g_lo = 0;
        g_state = S_LOAD;
        break;
    }

    case S_LOAD: {
        char p[320];
        g_line = dj_pick(g_lo, g_hi);
        snprintf(p, sizeof p, "%s/%s/%03d.wav", g_dir, g_bank, g_line);
        clip_free(&g_speech);
        if (!clip_load(p, &g_speech)) {
            if (g_log) printf("[dj] could not load %s\n", p);
            g_state = S_POST;
            break;
        }
        g_state = S_SPEAK;
        break;
    }

    case S_SPEAK:
        /* retail waits for the ident to finish before the line starts
         * (state 7) -- every line is introduced by a jingle [C] */
        if (g_ident_play) break;
        g_speech_pos = 0;
        g_speech_gain = DJ_GAIN_SPEECH
                      * (g_mode == B3_DJ_MODE_RACE ? DJ_MUL_RACE : DJ_MUL_OTHER);
        g_speech_play = 1;
        /* only the legacy share-the-bus mode ducks; the sequenced path
         * has nothing to duck, the song is already gone */
        if (g_duck_mode) g_duck_target = DJ_MUSIC_DUCK;
        g_lines++;
        if (g_log)
            printf("[dj] t=%.1f line %s/%03d  [%d,%d)  gain %.3f\n",
                   (double)g_clock, g_bank, g_line, g_lo, g_hi,
                   (double)g_speech_gain);
        g_state = S_DRAIN;
        break;

    case S_DRAIN:
        if (g_speech_play) break;
        g_duck_target = 1.0f;
        g_state = S_POST;
        break;

    case S_POST:
        /* no clip_free here either -- see the note in b3_dj_set_mode */
        g_duck_target = 1.0f;
        /* give the bus back: retail's state 9 starts the NEXT song [C] */
        dj_music_release();
        /* retail's state 9, FUN_00154A70 @0x00154EF9 [C] */
        g_cooldown = g_cd_base + dj_rand01() * g_cd_span;
        /* [S] GLUE.  Retail's game drives the mode: the session moves
         * 1 -> 2 and the radio follows 4 -> 5.  This port has no
         * distinct loading phase to hang mode 4 on -- it goes straight
         * to RACING -- so the handoff happens here instead, when the
         * intro line has finished.  The timing works out the same: the
         * cooldown re-armed just above is the one that decides when the
         * next line lands, so it is still a song-length away. */
        if (g_mode == B3_DJ_MODE_LOADING) {
            g_mode = B3_DJ_MODE_RACE;
            if (g_log)
                printf("[dj] intro over -> race mode, next in %.1fs\n",
                       (double)g_cooldown);
        }
        g_state = S_COOLDOWN;
        break;

    default:
        g_state = S_COOLDOWN;
        break;
    }
}

/* ===================================================================== *
 *  The mixer voice -- AUDIO THREAD
 * ===================================================================== */

float b3_dj_next_sample(void) {
    float acc = 0.0f;

    if (g_on <= 0) return 0.0f;

    /* Pause has to be honoured HERE, not only in tick(): while the game
     * is paused the sim's inner loop does not run, so tick() never gets
     * the chance, and a line already in the air would carry on talking
     * over a paused game.  Retail's equivalent is the DAT_00463AF4 gate
     * (RE_CRASHFM.md section 5.3). */
    if (g_suppressed) {
        g_speech_play = g_ident_play = 0;
        g_duck = 1.0f;
        return 0.0f;
    }

    /* slew the duck one output frame toward the target */
    {
        const float step = 1.0f / (DJ_DUCK_SLEW * (float)DJ_RATE);
        float t = g_duck_target;
        if (g_duck < t) { g_duck += step; if (g_duck > t) g_duck = t; }
        else if (g_duck > t) { g_duck -= step; if (g_duck < t) g_duck = t; }
    }

    if (g_ident_play) {
        unsigned p = g_ident_pos;
        if (g_identc.pcm && p < g_identc.frames) {
            acc += (float)g_identc.pcm[p] * DJ_GAIN_IDENT * g_master;
            g_ident_pos = p + 1;
        } else {
            g_ident_play = 0;
        }
    }

    if (g_speech_play) {
        unsigned p = g_speech_pos;
        if (g_speech.pcm && p < g_speech.frames) {
            acc += (float)g_speech.pcm[p] * g_speech_gain * g_master;
            g_speech_pos = p + 1;
        } else {
            g_speech_play = 0;
        }
    }

    return acc;
}

/* ===================================================================== *
 *  Introspection
 * ===================================================================== */

void b3_dj_status(B3DjStatus *out) {
    if (!out) return;
    out->mode         = g_mode;
    out->state        = g_state;
    out->speaking     = g_speech_play;
    out->ident        = g_ident_play;
    out->line         = g_line;
    out->lo           = g_lo;
    out->hi           = g_hi;
    out->ident_n      = g_ident_n;
    out->cooldown     = g_cooldown;
    out->duck         = g_duck;
    out->lines_played = g_lines;
    out->bank         = g_bank;
}
