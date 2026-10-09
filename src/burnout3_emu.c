/* The web and Android builds pass -D_GNU_SOURCE on the command line (musl gates
 * realpath/strdup/strtok_r on it where glibc is laxer), and an unguarded
 * #define here redefines it with a different body -- a warning on every wasm
 * and NDK build.  Guarded, the native build still defines it exactly as before. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "burnout3_emu.h"
#include "burnout3_carcol.h"   /* B3CarContact: the ccol reply's harness tail */
#include "burnout3_backend.h"
#include <stdio.h>
/* All diagnostics go to STDERR: several validators parse the stdout of
 * drivers that link these objects, and a stray status line breaks them. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

static pid_t  g_pid  = -1;
static FILE*  g_out;              // -> sidecar stdin
static FILE*  g_in;               // <- sidecar stdout
static int    g_ready;
static int    g_tried;
static double g_last_ms, g_sum_ms;
static long   g_calls;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* A blocking read that a SIGNAL cannot kill.
 *
 * fgets() returns NULL both at real end-of-file and when a signal lands
 * mid-read (errno EINTR), and the two are indistinguishable without asking.
 * The game runs under SDL, which arms timers, so signals arrive constantly --
 * and the AI path alone reads 360 times a second, which is why it was the one
 * that kept reporting "sidecar lost: eof on ai" against a sidecar that was
 * alive and well the whole time. Replaying its exact 17,927 calls into a
 * standalone server reproduced nothing: every reply came back.
 *
 * Retry while the stream is merely interrupted; a real EOF still returns NULL.
 */
static char* emu_gets(char* buf, int n, FILE* f)
{
    for (;;) {
        char* r = fgets(buf, n, f);
        if (r) return r;
        if (feof(f)) return NULL;               /* the child really is gone */
        if (ferror(f) && errno == EINTR) {      /* just a signal -- read on */
            clearerr(f);
            continue;
        }
        return NULL;
    }
}

static void emu_fail(const char* why)
{
    if (g_ready) {
        /* Say HOW it went. A Python traceback lands in build/emu_server.log,
         * but a native crash inside Unicorn leaves that file empty and only
         * the wait status distinguishes it from a clean exit. */
        int st = 0;
        fprintf(stderr, "[emu] sidecar lost: %s", why);
#ifndef _WIN32
        if (g_pid > 0 && waitpid(g_pid, &st, WNOHANG) == g_pid) {
            if (WIFSIGNALED(st))
                fprintf(stderr, " -- child killed by signal %d", WTERMSIG(st));
            else if (WIFEXITED(st))
                fprintf(stderr, " -- child exited %d", WEXITSTATUS(st));
            g_pid = -1;
        } else {
            fprintf(stderr, " -- child still running");
        }
#endif
        fputc('\n', stderr);
    }
    g_ready = 0;
    /* Every retail backend speaks to this one sidecar, so losing it takes them
     * all down -- not just physics.  Demoting only B3_FEAT_PHYSICS left the
     * other nine still believing they were on retail, so each one went on
     * writing to a dead pipe and reporting its own failure separately. */
    for (int f = 0; f < B3_FEAT_COUNT; f++)
        if (b3_backend_get((B3Feature)f) == B3_BACKEND_RETAIL)
            b3_backend_demote((B3Feature)f, why);
}

static void b3_emu_ranges_forget(int car);

int b3_emu_ready(void) { return g_ready; }
double b3_emu_last_ms(void) { return g_last_ms; }
double b3_emu_avg_ms(void) { return g_calls ? g_sum_ms / (double)g_calls : 0.0; }

int b3_emu_init(void)
{
    if (g_tried) return g_ready;
    g_tried = 1;

#ifdef _WIN32
    fprintf(stderr, "[emu] retail sidecar (Unicorn x86 bridge) is currently disabled on Windows native\n");
    return 0;
#else
    if (access("build/burnout3.elf", R_OK) != 0) {
        fprintf(stderr, "[emu] build/burnout3.elf is missing -- retail backends need it\n"
               "      (python3 tools/xbe2elf.py \"$B3_GAME_ROOT/default.xbe\" "
               "build/burnout3.elf)\n");
        return 0;
    }

    int to_child[2], from_child[2];
    if (pipe(to_child) || pipe(from_child)) { perror("[emu] pipe"); return 0; }

    // The sidecar must never inherit our stdin: a stray read would eat the
    // game's own input stream.
    pid_t pid = fork();
    if (pid < 0) { perror("[emu] fork"); return 0; }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        // Python tracebacks go to a file, not the game's console: a crash in
        // the sidecar is the thing you most need to read afterwards, and
        // interleaving it with the frame log loses it.
        FILE* err = fopen("build/emu_server.log", "w");
        if (err) { dup2(fileno(err), STDERR_FILENO); fclose(err); }
        execlp("python3", "python3", "-u", "tools/b3_emu_server.py", (char*)NULL);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    g_pid = pid;
    g_out = fdopen(to_child[1], "w");
    g_in  = fdopen(from_child[0], "r");
    if (!g_out || !g_in) { emu_fail("fdopen"); return 0; }

    fputs("hello\n", g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in) || strncmp(line, "ok", 2) != 0) {
        fprintf(stderr, "[emu] sidecar did not answer hello (python3 missing, or an "
               "import failed -- check stderr above)\n");
        emu_fail("no hello");
        return 0;
    }
    g_ready = 1;
    b3_emu_ranges_forget(-1);       /* a new sidecar knows no range tables */
    fprintf(stderr, "[emu] retail sidecar up: %s", line);
    return 1;
#endif
}

void b3_emu_shutdown(void)
{
    if (g_out) { fputs("bye\n", g_out); fflush(g_out); fclose(g_out); g_out = NULL; }
    if (g_in)  { fclose(g_in); g_in = NULL; }
#ifndef _WIN32
    if (g_pid > 0) {
        int st;
        kill(g_pid, SIGTERM);
        waitpid(g_pid, &st, 0);
        g_pid = -1;
    }
#endif
    g_ready = 0;
}

static int b3_emu_send_ranges(int car);

int b3_emu_seed(int car, float x, float y, float z, float yaw)
{
    if (!g_ready) return 0;
    fprintf(g_out, "seed %d %.9g %.9g %.9g %.9g\n", car, x, y, z, yaw);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on seed"); return 0; }
    if (strncmp(line, "ok", 2) != 0) {
        fprintf(stderr, "[emu] seed: %s", line);
        return 0;
    }
    b3_emu_ranges_forget(car);      /* a re-seeded car needs them again */
    return b3_emu_send_ranges(car);
}

int b3_emu_soup(int car, const float* tris, int count)
{
    if (!g_ready || count <= 0) return 0;
    if (count > B3_EMU_SOUP_CAP) count = B3_EMU_SOUP_CAP;
    fprintf(g_out, "soup %d %d", car, count);
    for (int i = 0; i < count * 13; i++) fprintf(g_out, " %.9g", tris[i]);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on soup"); return 0; }
    if (strncmp(line, "ok", 2) != 0) { fprintf(stderr, "[emu] soup: %s", line); return 0; }
    return 1;
}

int b3_emu_step(int car, float throttle, float brake, float steer,
                int boost, float dt, B3EmuState* out)
{
    if (!g_ready || !out) return 0;
    double t0 = now_ms();

    fprintf(g_out, "step %d %.9g %.9g %.9g %d %.9g\n",
            car, throttle, brake, steer, boost ? 1 : 0, dt);
    fflush(g_out);

    // The reply is one line of 29 floats; it is bounded, so a fixed buffer is
    // fine, but a short read means the sidecar died mid-frame.
    char line[2048];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on step"); return 0; }
    if (strncmp(line, "st ", 3) != 0) {
        fprintf(stderr, "[emu] step: %s", line);
        emu_fail("bad reply");
        return 0;
    }

    float v[B3_EMU_FIELDS];
    char* p = line + 3;
    for (int i = 0; i < B3_EMU_FIELDS; i++) {
        char* end;
        v[i] = strtof(p, &end);
        if (end == p) { emu_fail("short reply"); return 0; }
        p = end;
    }
    memcpy(out, v, sizeof v);

    g_last_ms = now_ms() - t0;
    g_sum_ms += g_last_ms;
    g_calls++;
    if ((g_calls % 600) == 0)
        fprintf(stderr, "[emu] %ld retail frames, %.2f ms avg, %.2f ms last "
               "(budget 16.67)\n", g_calls, b3_emu_avg_ms(), g_last_ms);
    return 1;
}


// ---------------------------------------------------------------------------
// RAW WINDOW TRANSFER -- what shape parity bought.
//
// B3VehicleFull is byte-identical to retail's vehicle object over
// 0x0000..0x1A00, so the port's own struct bytes go straight to the emulator
// and come straight back. No field is converted in either direction; there is
// no marshalling layer left to disagree with itself.
//
// Two things still have to be handled explicitly, and neither is a data
// translation:
//   * seven pointer slots inside the window (soup, frame object, racecar,
//     config, self) hold host addresses that mean nothing in the emulator's
//     address space. The sidecar preserves its own across the write.
//   * the 4x4 travels separately, because retail does not keep it in the
//     vehicle -- v+0x204 points at its own object.
// ---------------------------------------------------------------------------
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char* in, size_t n, char* out)
{
    size_t i = 0, o = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (unsigned)in[i+1] << 8 | in[i+2];
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >>  6) & 63]; out[o++] = B64[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16 | ((i + 1 < n) ? (unsigned)in[i+1] << 8 : 0);
        out[o++] = B64[(v >> 18) & 63]; out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char* in, unsigned char* out, size_t cap)
{
    size_t o = 0; int q[4], k = 0;
    for (const char* p = in; *p && *p != '\n' && *p != ' '; p++) {
        int v = b64_val((unsigned char)*p);
        if (v < 0) { if (*p == '=') break; else continue; }
        q[k++] = v;
        if (k == 4) {
            unsigned w = (unsigned)q[0] << 18 | (unsigned)q[1] << 12
                       | (unsigned)q[2] << 6  | (unsigned)q[3];
            if (o < cap) out[o++] = (unsigned char)(w >> 16);
            if (o < cap) out[o++] = (unsigned char)(w >> 8);
            if (o < cap) out[o++] = (unsigned char)w;
            k = 0;
        }
    }
    if (k == 2) {
        unsigned w = (unsigned)q[0] << 18 | (unsigned)q[1] << 12;
        if (o < cap) out[o++] = (unsigned char)(w >> 16);
    } else if (k == 3) {
        unsigned w = (unsigned)q[0] << 18 | (unsigned)q[1] << 12 | (unsigned)q[2] << 6;
        if (o < cap) out[o++] = (unsigned char)(w >> 16);
        if (o < cap) out[o++] = (unsigned char)(w >> 8);
    }
    return o;
}


// ---------------------------------------------------------------------------
// Is the raw-window transfer safe to use yet?  NO -- and this is the finding
// that matters most from the parity work.
//
// Shape parity aligns the fields we have RECOVERED: 83 of them, at retail's
// own offsets. It does not make B3VehicleFull a complete model of retail's
// 0x1A00 vehicle object. The rest of that window is `_pad` here, and retail's
// own code reads it -- config and racecar pointers, wheel frame links, class
// and state bytes we have not recovered yet. The emulator seeds all of that
// when it builds its vehicle; writing the port's window over the top replaces
// it with zeros, and the substep loop then spins on NaN (measured: an
// all-zero window hangs FUN_0011BE50 past a 60M-instruction budget).
//
// So byte-level sharing needs COVERAGE as well as alignment. Until the port
// models the whole object, this path stays off and physics=retail falls back
// to the RE port rather than shipping something that hangs. The alternative --
// copying only the recovered ranges and leaving the emulator's seed intact --
// is a scatter-copy driven by the parity table, and it is the next step.
// ---------------------------------------------------------------------------
int b3_emu_window_ready(void)
{
    static int warned = 0;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[emu] physics=retail: the raw-window transfer needs the port to\n"
               "      model ALL of retail's 0x1A00 vehicle object, not just the\n"
               "      83 recovered fields. Falling back to the RE port; see\n"
               "      docs/RE_SHAPE_PARITY.md 'coverage, not just alignment'.\n");
    }
    return 0;
}


// ---------------------------------------------------------------------------
// SCATTER TRANSFER -- the recovered ranges only.
//
// Shape parity put our fields at retail's offsets; it did not make the port a
// complete model of retail's 0x1A00 vehicle. Writing the whole window replaces
// the ~81% we have not recovered with zeros, and the substep loop then spins
// on NaN. So only the 82 recovered ranges move, each at the SAME offset on
// both sides, and everything else stays as the emulator seeded it.
//
// Nothing is converted in either direction -- this is a partial byte copy
// between two address spaces, which is what parity bought.
// ---------------------------------------------------------------------------
#include "burnout3_vehicle_ranges.h"

/* The range table is a compile-time constant and the sidecar just stores it
 * (do_ranges -> RANGES[car]), so it only has to travel once per car per
 * session.  b3_emu_step_ai was re-sending it on EVERY call -- a whole extra
 * blocking round trip per car per frame, measured at 1.00 calls/frame in the
 * in-game profile purely as waste.  Cleared on (re-)seed and on sidecar
 * restart, which are the only two things that invalidate the sidecar's copy. */
#define B3_EMU_MAX_CARS 64
static unsigned char g_ranges_sent[B3_EMU_MAX_CARS];

static void b3_emu_ranges_forget(int car)
{
    if (car < 0) memset(g_ranges_sent, 0, sizeof g_ranges_sent);
    else if (car < B3_EMU_MAX_CARS) g_ranges_sent[car] = 0;
}

static int b3_emu_send_ranges(int car)
{
    if (!g_ready) return 0;
    if (car >= 0 && car < B3_EMU_MAX_CARS && g_ranges_sent[car]) return 1;
    fprintf(g_out, "ranges %d %u", car, (unsigned)B3_VEHICLE_RANGE_COUNT);
    for (size_t i = 0; i < B3_VEHICLE_RANGE_COUNT; i++)
        fprintf(g_out, " %u %u", B3_VEHICLE_RANGES[i].off,
                B3_VEHICLE_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on ranges"); return 0; }
    if (strncmp(line, "ok", 2) != 0) { fprintf(stderr, "[emu] ranges: %s", line); return 0; }
    if (car >= 0 && car < B3_EMU_MAX_CARS) g_ranges_sent[car] = 1;
    return 1;
}

static unsigned b3_emu_range_bytes(void)
{
    static unsigned n = 0;
    if (!n)
        for (size_t i = 0; i < B3_VEHICLE_RANGE_COUNT; i++)
            n += B3_VEHICLE_RANGES[i].len;
    return n;
}


// ---------------------------------------------------------------------------
// CAR-VS-CAR through retail's own resolver.
//
// Same principle as the physics path: the port's structures are retail-shaped,
// so its bytes go straight in. B3RigidBody scatters to veh+0x00, bbmax/bbmin/
// mass to veh+0x1D0/+0x1E0/+0x1F0, and B3CarHull is byte-identical to the
// 0x600 hull record so it needs no conversion at all. Retail picks the arm --
// FUN_001121F0 both-alive, FUN_00113960 when either is wrecked.
//
// Verified against emulate_carcol on an identical head-on: both report
// hit=1 slam=1 impact=16058.5 crash_a=1.
// ---------------------------------------------------------------------------
int b3_emu_carcol(const void* rbA, const void* extraA, const void* hullA,
                  const float frameA[4][4], int crashedA, int typeA,
                  const void* rbB, const void* extraB, const void* hullB,
                  const float frameB[4][4], int crashedB, int typeB,
                  void* out_contact, void* out_rbA, void* out_rbB,
                  int* out_crash_a, int* out_crash_b)
{
    /* the physics path is what normally starts the sidecar; carcol can be on
     * retail with physics on the port, so bring it up here too (idempotent) */
    if (!g_ready && !b3_emu_init()) return 0;
    double t0 = now_ms();

    static char e[8][4 * ((0x600 + 2) / 3) + 8];
    const void* src[8] = { rbA, extraA, hullA, frameA, rbB, extraB, hullB, frameB };
    const unsigned len[8] = { B3_EMU_RB_LEN, B3_EMU_EXTRA_LEN, B3_EMU_HULL_LEN, 64,
                              B3_EMU_RB_LEN, B3_EMU_EXTRA_LEN, B3_EMU_HULL_LEN, 64 };
    for (int i = 0; i < 8; i++)
        b64_encode((const unsigned char*)src[i], len[i], e[i]);

    fprintf(g_out, "ccol %d %d %d %d %s %s %s %s %s %s %s %s\n",
            crashedA ? 1 : 0, typeA, crashedB ? 1 : 0, typeB,
            e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    fflush(g_out);

    static char line[4 * ((0x600 + 2) / 3) * 3 + 512];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on ccol"); return 0; }
    if (strncmp(line, "cc ", 3) != 0) {
        fprintf(stderr, "[emu] ccol: %s", line);
        b3_backend_demote(B3_FEAT_CARCOL, "bad reply");
        return 0;
    }
    char* tok[9]; int n = 0;
    for (char* q = strtok(line, " \n"); q && n < 9; q = strtok(NULL, " \n")) tok[n++] = q;
    if (n < 6) { emu_fail("short ccol reply"); return 0; }
    /* The reply's first blob is the 0x30-byte RETAIL window only.  The
     * harness-side fields past it (event/attacker_is_b/strength/vn_mph) must
     * be written here or the caller's stack-local contact keeps garbage in
     * them -- carcol_slam_racers() gates on event >= 1, so a stale zero
     * silently dropped every slam and a stale nonzero reported phantom ones. */
    memset(out_contact, 0, sizeof(B3CarContact));
    b64_decode(tok[1], (unsigned char*)out_contact, 0x30);
    b64_decode(tok[2], (unsigned char*)out_rbA, B3_EMU_RB_LEN);
    b64_decode(tok[3], (unsigned char*)out_rbB, B3_EMU_RB_LEN);
    if (out_crash_a) *out_crash_a = atoi(tok[4]);
    if (out_crash_b) *out_crash_b = atoi(tok[5]);
    {
        B3CarContact* oc = (B3CarContact*)out_contact;
        if (n >= 9) {
            /* retail's own game-context vtable+0x64 slam notify, captured by
             * the session at STUB_SLAM: the kind IS the port's event id
             * (RE_CARCOL 5) */
            oc->event         = atoi(tok[6]);
            oc->attacker_is_b = atoi(tok[7]);
            oc->strength      = (float)atof(tok[8]);
        }
        /* impact = (mA+mB) * |vn_mph| * 0.1 * 0.5 (the retail window field),
         * so vn_mph inverts exactly from the masses the caller sent */
        float msum = ((const float*)extraA)[8] + ((const float*)extraB)[8];
        if (msum > 1.0f) oc->vn_mph = oc->impact / (0.05f * msum);
    }

    g_last_ms = now_ms() - t0;
    g_sum_ms += g_last_ms;
    g_calls++;
    return 1;
}


// ---------------------------------------------------------------------------
// TAKEDOWN RULES through retail's own slam gate (FUN_00197BE0).
//
// td_rules is STATEFUL, unlike the stateless car-vs-car resolve: the emulated
// world owns per-car score state between calls. B3TdCar is retail-shaped now,
// so the port's cars scatter in at the racecar's own offsets -- 33 ranges,
// 155 bytes each -- and the verdict plus the mutated cars come back. Nothing
// is converted in either direction.
// ---------------------------------------------------------------------------
#include "burnout3_tdcar_ranges.h"

static int b3_emu_send_tdranges(void)
{
    if (!g_ready) return 0;
    fprintf(g_out, "tdranges %u", (unsigned)B3_TDCAR_RANGE_COUNT);
    for (size_t i = 0; i < B3_TDCAR_RANGE_COUNT; i++)
        fprintf(g_out, " %u %u", B3_TDCAR_RANGES[i].off, B3_TDCAR_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on tdranges"); return 0; }
    return strncmp(line, "ok", 2) == 0;
}


// ---------------------------------------------------------------------------
// SCORE EVENTS through retail's own regparm3 entries.
//
// B3ScoreEvents is retail-shaped over the score object's first 0x600, so the
// port's 22 recovered ranges (299 bytes) go in at the game's own offsets,
// FUN_00197920 (near-miss cancel) or FUN_001979E0 (rubbing mark) runs, and the
// same ranges come back. Nothing is converted in either direction.
// ---------------------------------------------------------------------------
#include "burnout3_score_ranges.h"

static int b3_emu_send_scranges(void)
{
    if (!g_ready) return 0;
    fprintf(g_out, "scranges %u", (unsigned)B3_SCORE_RANGE_COUNT);
    for (size_t i = 0; i < B3_SCORE_RANGE_COUNT; i++)
        fprintf(g_out, " %u %u", B3_SCORE_RANGES[i].off, B3_SCORE_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on scranges"); return 0; }
    return strncmp(line, "ok", 2) == 0;
}


// ---------------------------------------------------------------------------
// FOLLOW CAMERA through retail's own FUN_0015E550.
//
// The camera's interface IS a matrix and scalars -- that is the function's real
// signature -- and B3TdfxCamera already holds eye/quat/fov/pitch/yaw, so the
// result lands by direct assignment. There is no second struct shape in the
// middle to convert between.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AI DRIVER through retail's own FUN_00105340.
//
// TWO objects, each at its own base, because that is what retail has: the
// racecar (B3AiCar, RC+off) and the physics vehicle (B3VehicleFull, VEH+off).
// The driver reads speed, heading, gear and rpm out of the vehicle and writes
// throttle/brake/steer back into it; the racecar carries the boost latch.
//
// This used to send a single struct that mixed both objects' offsets, so ten
// vehicle fields landed inside the racecar. B3AiCar is a pure racecar view
// now and points at the vehicle the way retail does at racecar+0x2440, so the
// vehicle side needs no table of its own -- B3_VEHICLE_RANGES already covers
// it, the same 82 recovered ranges the physics path uses. Nothing is
// converted in either direction.
// ---------------------------------------------------------------------------
#include "burnout3_ai_ranges.h"
#include "burnout3_vehicle_ranges.h"

static int b3_emu_send_airanges(void)
{
    if (!g_ready) return 0;
    /* The AI object is EMBEDDED in the racecar at +0x1A00, so its ranges go
     * to the racecar's base with that bias applied here -- the struct keeps
     * retail's AI-relative offsets. Without this the driver reads a zeroed
     * target speed at AI+0x9C4 and commands no throttle at all: the car
     * coasted to a standstill, which is how the omission was found. */
    fprintf(g_out, "airanges %u %u",
            (unsigned)(B3_AICAR_RANGES_COUNT + B3_AISTATE_RANGES_COUNT),
            (unsigned)B3_AIVEH_RANGES_COUNT);
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++)
        fprintf(g_out, " %u %u", B3_AICAR_RANGES[i].off, B3_AICAR_RANGES[i].len);
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++)
        fprintf(g_out, " %u %u",
                B3_AI_OBJECT_OFFSET + B3_AISTATE_RANGES[i].off,
                B3_AISTATE_RANGES[i].len);
    /* The driver's own vehicle interface -- NOT B3_VEHICLE_RANGES. Handing it
     * the whole vehicle is over-transfer: it overwrites state the emulator
     * seeded coherently with state that is only coherent inside the port. */
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++)
        fprintf(g_out, " %u %u", B3_AIVEH_RANGES[i].off, B3_AIVEH_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on airanges"); return 0; }
    return strncmp(line, "ok", 2) == 0;
}

// car -> the racecar view, veh -> the physics vehicle. Both are written back:
// the driver's outputs land in the vehicle, its boost latch in the racecar.
unsigned long g_emu_ai_calls = 0;   /* proof the retail path actually ran */

int b3_emu_ai_drive(int which, void* car, void* state, void* veh, float clock,
                    float dt, const float frame[4][4])
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return 0;
    if (!sent && !(sent = b3_emu_send_airanges())) return 0;

    unsigned nc = 0, nv = 0;
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++) nc += B3_AICAR_RANGES[i].len;
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++) nc += B3_AISTATE_RANGES[i].len;
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++) nv += B3_AIVEH_RANGES[i].len;

    static unsigned char fc[512], fv[4096];
    static char ec[4 * ((512 + 2) / 3) + 8], ev[4 * ((4096 + 2) / 3) + 8];
    if (nc > sizeof fc || nv > sizeof fv) return 0;

    unsigned pos = 0;
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++) {
        memcpy(fc + pos, (const unsigned char*)car + B3_AICAR_RANGES[i].off,
               B3_AICAR_RANGES[i].len);
        pos += B3_AICAR_RANGES[i].len;
    }
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++) {
        memcpy(fc + pos, (const unsigned char*)state + B3_AISTATE_RANGES[i].off,
               B3_AISTATE_RANGES[i].len);
        pos += B3_AISTATE_RANGES[i].len;
    }
    pos = 0;
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++) {
        memcpy(fv + pos, (const unsigned char*)veh + B3_AIVEH_RANGES[i].off,
               B3_AIVEH_RANGES[i].len);
        pos += B3_AIVEH_RANGES[i].len;
    }
    b64_encode(fc, nc, ec);
    b64_encode(fv, nv, ev);

    /* The car's 4x4 goes to the RenderWare frame behind vehicle+0x204, which
     * is where the driver reads its forward/right rows from. Without it the
     * driver steered our car using the orientation the emulator seeded. */
    char ef[4 * ((64 + 2) / 3) + 8];
    b64_encode((const unsigned char*)frame, 64, ef);

    fprintf(g_out, "ai %d %.9g %.9g %s %s %s\n", which, clock, dt, ec, ev, ef);
    fflush(g_out);

    char line[8192];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on ai"); return 0; }
    if (strncmp(line, "ai ", 3) != 0) {
        fprintf(stderr, "[emu] ai: %s", line);
        b3_backend_demote(B3_FEAT_AI, "bad reply");
        return 0;
    }
    {
        char* sp = strchr(line + 3, ' ');
        if (!sp) { b3_backend_demote(B3_FEAT_AI, "short ai reply"); return 0; }
        *sp = 0;
        if (b64_decode(line + 3, fc, sizeof fc) != nc) return 0;
        if (b64_decode(sp + 1, fv, sizeof fv) != nv) return 0;
    }
    pos = 0;
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++) {
        memcpy((unsigned char*)car + B3_AICAR_RANGES[i].off, fc + pos,
               B3_AICAR_RANGES[i].len);
        pos += B3_AICAR_RANGES[i].len;
    }
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++) {
        memcpy((unsigned char*)state + B3_AISTATE_RANGES[i].off, fc + pos,
               B3_AISTATE_RANGES[i].len);
        pos += B3_AISTATE_RANGES[i].len;
    }
    pos = 0;
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++) {
        memcpy((unsigned char*)veh + B3_AIVEH_RANGES[i].off, fv + pos,
               B3_AIVEH_RANGES[i].len);
        pos += B3_AIVEH_RANGES[i].len;
    }
    g_emu_ai_calls++;
    return 1;
}


// ---------------------------------------------------------------------------
// CHASSIS-VS-WORLD RESOLVE through retail's own FUN_0011AEF0.
//
// Retail calls this at 0x0011C0B7, INSIDE the substep loop, between the tyre
// force pass and the suspension pre-pass -- and that position is the whole
// point: everything the resolve produces is an accumulator write (+0xF0 force,
// +0x110 impulse, +0x120 angular impulse, +0x130 deflection) which the
// integrator at 0x0011C160 consumes and clears at the end of the SAME substep.
// The port calls it from the same place, through B3VehicleFull.chassis_resolve,
// so switching backends does not move it.
//
// It runs on the PIPELINE session for this car, because that vehicle has a
// real veh+0x200 soup behind it -- the frozen collision set the resolve reads
// -- uploaded with the same `soup` command physics=retail uses.
// ---------------------------------------------------------------------------
#include "burnout3_crash_ranges.h"

static int b3_emu_send_crashranges(void)
{
    if (!g_ready) return 0;
    fprintf(g_out, "crashranges %u", (unsigned)B3_CRASHVEH_RANGES_COUNT);
    for (size_t i = 0; i < B3_CRASHVEH_RANGES_COUNT; i++)
        fprintf(g_out, " %u %u", B3_CRASHVEH_RANGES[i].off,
                B3_CRASHVEH_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on crashranges"); return 0; }
    return strncmp(line, "ok", 2) == 0;
}

unsigned long g_emu_crash_calls = 0;

// Returns 1 on success and stores FUN_0011AEF0's own return value (the wall
// contact count) in *out_n. Returns 0 if retail could not be reached, so the
// caller falls back to the port.
int b3_emu_chassis_resolve(int car, void* veh, int* out_n)
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return 0;
    if (!sent && !(sent = b3_emu_send_crashranges())) return 0;

    unsigned n = 0;
    for (size_t i = 0; i < B3_CRASHVEH_RANGES_COUNT; i++) n += B3_CRASHVEH_RANGES[i].len;

    static unsigned char fb[1024];
    static char eb[4 * ((1024 + 2) / 3) + 8];
    if (n > sizeof fb) return 0;

    unsigned pos = 0;
    for (size_t i = 0; i < B3_CRASHVEH_RANGES_COUNT; i++) {
        memcpy(fb + pos, (const unsigned char*)veh + B3_CRASHVEH_RANGES[i].off,
               B3_CRASHVEH_RANGES[i].len);
        pos += B3_CRASHVEH_RANGES[i].len;
    }
    b64_encode(fb, n, eb);
    fprintf(g_out, "crashres %d %s\n", car, eb);
    fflush(g_out);

    char line[2048];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on crashres"); return 0; }
    if (strncmp(line, "cres ", 5) != 0) {
        fprintf(stderr, "[emu] crashres: %s", line);
        b3_backend_demote(B3_FEAT_CRASH, "bad reply");
        return 0;
    }
    char* p = line + 5;
    long eax = strtol(p, &p, 10);
    while (*p == ' ') p++;
    if (b64_decode(p, fb, sizeof fb) != n) return 0;
    pos = 0;
    for (size_t i = 0; i < B3_CRASHVEH_RANGES_COUNT; i++) {
        memcpy((unsigned char*)veh + B3_CRASHVEH_RANGES[i].off, fb + pos,
               B3_CRASHVEH_RANGES[i].len);
        pos += B3_CRASHVEH_RANGES[i].len;
    }
    if (out_n) *out_n = (int)eax;
    g_emu_crash_calls++;
    return 1;
}


// ---------------------------------------------------------------------------
// TRAFFIC spawn choices through retail's own FUN_001A5E30 / FUN_001A5F90.
//
// The traffic feature's per-frame half is the population law FUN_001A6070,
// which reads the traffic MANAGER object this tree does not map. It stays on
// the port, and validate_traffic_mix.py checks it the way it always has --
// as a model replay against constants read from the image. What IS callable
// standalone, with conventions that suite already exercises, is the pair of
// choices a spawning car makes: which model inside its class, and which paint.
//
// Both draw from the manager RNG, so the port's state/carry travel with the
// call and the advanced pair comes back. One stream, whichever side draws it
// -- otherwise flipping the backend would silently fork the sequence and
// every later spawn would differ for reasons that have nothing to do with
// the function under test.
// ---------------------------------------------------------------------------
unsigned long g_emu_traffic_calls = 0;

int b3_emu_traffic_model(unsigned* rng_state, unsigned* rng_carry, int cls,
                         unsigned total, const unsigned* weights, unsigned n)
{
    if (!g_ready && !b3_emu_init()) return -2;
    fprintf(g_out, "tmodel %u %u %d %u", *rng_state, *rng_carry, cls, total);
    for (unsigned i = 0; i < n; i++) fprintf(g_out, " %u", weights[i]);
    fputc('\n', g_out);
    fflush(g_out);

    char line[512];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on tmodel"); return -2; }
    if (strncmp(line, "tm ", 3) != 0) {
        fprintf(stderr, "[emu] tmodel: %s", line);
        b3_backend_demote(B3_FEAT_TRAFFIC, "bad reply");
        return -2;
    }
    int idx; unsigned st, cy;
    if (sscanf(line + 3, "%d %u %u", &idx, &st, &cy) != 3) return -2;
    *rng_state = st; *rng_carry = cy;
    g_emu_traffic_calls++;
    return idx;
}

int b3_emu_traffic_paint(unsigned* rng_state, unsigned* rng_carry,
                         const unsigned char colours[8])
{
    if (!g_ready && !b3_emu_init()) return -2;
    fprintf(g_out, "tpaint %u %u", *rng_state, *rng_carry);
    for (int i = 0; i < 8; i++) fprintf(g_out, " %u", (unsigned)colours[i]);
    fputc('\n', g_out);
    fflush(g_out);

    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on tpaint"); return -2; }
    if (strncmp(line, "tp ", 3) != 0) {
        fprintf(stderr, "[emu] tpaint: %s", line);
        b3_backend_demote(B3_FEAT_TRAFFIC, "bad reply");
        return -2;
    }
    int idx; unsigned st, cy;
    if (sscanf(line + 3, "%d %u %u", &idx, &st, &cy) != 3) return -2;
    *rng_state = st; *rng_carry = cy;
    g_emu_traffic_calls++;
    return idx;
}


// ---------------------------------------------------------------------------
// HUD EVENT TICKER through retail's own FUN_0004D310.
//
// The interface is a shared object at identical offsets, both ends:
//   in  -- B3HudTickIn IS the score object's B3CatRecord (value +0x00,
//          clock +0x04, prev +0x08, open +0x10, tier +0x11, prev_tier +0x12,
//          count +0x13), probed at score+0x374/0x390/0x418/0x598/0x5C4/0x564
//   out -- B3TickRow IS the element's row slot (obj+0x570 + i*0x28: live
//          +0x00, timer +0x08, y +0x10, tier +0x18, flash +0x1C, phase +0x20,
//          pulse +0x24)
//
// so the switch moves values that already mean the same thing at the same
// offsets. What does NOT cross is the draw node: retail builds a 2D node per
// row and this harness draws its own, which is the LOOK the parity rules
// leave relaxed. The row STATE -- which rows are live, their tier, timer and
// stacking order -- is the logic, and that is what comes back.
// ---------------------------------------------------------------------------
unsigned long g_emu_hud_calls = 0;

int b3_emu_hud_tick(float dt, const float recs[6][7],
                    float rows_out[7][7], int order_out[7], int* n_out)
{
    if (!g_ready && !b3_emu_init()) return 0;
    fprintf(g_out, "hudtick %.9g", dt);
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 7; j++) fprintf(g_out, " %.9g", recs[i][j]);
    fputc('\n', g_out);
    fflush(g_out);

    char line[2048];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on hudtick"); return 0; }
    if (strncmp(line, "ht ", 3) != 0) {
        fprintf(stderr, "[emu] hudtick: %s", line);
        b3_backend_demote(B3_FEAT_HUD, "bad reply");
        return 0;
    }
    char* p = line + 3;
    long n = strtol(p, &p, 10);
    if (n < 0 || n > 7) return 0;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 7; j++) {
            char* e;
            float v = strtof(p, &e);
            if (e == p) return 0;
            p = e;
            rows_out[i][j] = v;
        }
    for (long i = 0; i < n; i++) {
        char* e;
        long v = strtol(p, &e, 10);
        if (e == p) return 0;
        p = e;
        order_out[i] = (int)v;
    }
    *n_out = (int)n;
    g_emu_hud_calls++;
    return 1;
}


// ---------------------------------------------------------------------------
// SFX EMITTERS through the game's own event law.
//
// The boundary is PlaySound3D (0x001CD8D0): everything before it is the law
// -- which wave, what gain, what playback rate -- and everything after is the
// Xbox's 3D DirectSound voice manager, which has no meaning off the console
// and which this harness replaces with its own mixer (declared GLUE in
// burnout3_sfx.c). So the emitter runs for real and the captured
// {wave, gain, pitch} comes back to be mixed here.
//
// Note this is NOT FUN_00141010, the trigger the HUD ticker fires: that one
// walks straight into the voice manager and is stubbed even in
// tools/emulate_hud_ticker.py.
// ---------------------------------------------------------------------------
unsigned long g_emu_sfx_calls = 0;

int b3_emu_sfx_fire(unsigned addr, int kind, float mag,
                    B3EmuVoice out_v[4], int* n_out)
{
    if (!g_ready && !b3_emu_init()) return 0;
    fprintf(g_out, "sfxfire %u %d %.9g\n", addr, kind, mag);
    fflush(g_out);

    char line[1024];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on sfxfire"); return 0; }
    if (strncmp(line, "sx ", 3) != 0) {
        fprintf(stderr, "[emu] sfxfire: %s", line);
        b3_backend_demote(B3_FEAT_SFX, "bad reply");
        return 0;
    }
    char* p = line + 3;
    long n = strtol(p, &p, 10);
    if (n < 0 || n > 4) return 0;
    for (long i = 0; i < n; i++) {
        while (*p == ' ') p++;
        char* w = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - w);
        if (len >= sizeof out_v[i].wave) len = sizeof out_v[i].wave - 1;
        memcpy(out_v[i].wave, w, len);
        out_v[i].wave[len] = 0;
        out_v[i].gain  = strtof(p, &p);
        out_v[i].pitch = strtof(p, &p);
    }
    *n_out = (int)n;
    g_emu_sfx_calls++;
    return 1;
}


// ---------------------------------------------------------------------------
// DRIVER + STEP IN ONE SESSION (ai=retail AND physics=retail).
//
// Retail runs FUN_00105340 and FUN_0011BE50 over the SAME vehicle object: the
// driver writes v+0x1400 and the step reads it. Running them in two Unicorn
// sessions is this port's artifact, and it costs correctness -- the AI has to
// be fed from the physics MIRROR, which is one frame old and covers 23% of
// the object, and its view goes incoherent (measured: 1026 rpm at 21 m/s in
// gear 2). Throttle then reached the step in 7% of frames, against 41% with
// the port's own AI, and the car would not accelerate.
//
// So when both are retail the driver runs inside the PIPELINE session and its
// output is the input the step consumes. Nothing shuttles between sessions.
// ---------------------------------------------------------------------------
int b3_emu_step_ai(int car, int boost, float dt,
                   const void* aicar, const void* aistate,
                   void* vehicle, float frame[4][4])
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return 0;
    if (!sent && !(sent = b3_emu_send_airanges())) return 0;
    if (!b3_emu_send_ranges(car)) return 0;

    unsigned nc = 0, nv = b3_emu_range_bytes();
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++) nc += B3_AICAR_RANGES[i].len;
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++) nc += B3_AISTATE_RANGES[i].len;

    static unsigned char fc[1024], fv[8192], fa[1024];
    static char ec[4 * ((1024 + 2) / 3) + 8];
    if (nc > sizeof fc || nv > sizeof fv) return 0;

    unsigned pos = 0;
    for (size_t i = 0; i < B3_AICAR_RANGES_COUNT; i++) {
        memcpy(fc + pos, (const unsigned char*)aicar + B3_AICAR_RANGES[i].off,
               B3_AICAR_RANGES[i].len);
        pos += B3_AICAR_RANGES[i].len;
    }
    for (size_t i = 0; i < B3_AISTATE_RANGES_COUNT; i++) {
        memcpy(fc + pos, (const unsigned char*)aistate + B3_AISTATE_RANGES[i].off,
               B3_AISTATE_RANGES[i].len);
        pos += B3_AISTATE_RANGES[i].len;
    }
    b64_encode(fc, nc, ec);

    fprintf(g_out, "stepra %d %d %.9g %s\n", car, boost ? 1 : 0, dt, ec);
    fflush(g_out);

    static char line[4 * ((8192 + 2) / 3) + 4096];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on stepra"); return 0; }
    if (strncmp(line, "rnga ", 5) != 0) {
        fprintf(stderr, "[emu] stepra: %s", line);
        b3_backend_demote(B3_FEAT_PHYSICS, "bad reply");
        return 0;
    }
    char* w = line + 5;
    char* s1 = strchr(w, ' ');       if (!s1) return 0;  *s1 = 0;
    char* s2 = strchr(s1 + 1, ' ');  if (!s2) return 0;  *s2 = 0;
    if (b64_decode(w, fv, sizeof fv) != nv) { emu_fail("stepra size"); return 0; }
    b64_decode(s1 + 1, (unsigned char*)frame, 64);

    unsigned na = 0;
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++) na += B3_AIVEH_RANGES[i].len;
    if (na > sizeof fa) return 0;
    if (b64_decode(s2 + 1, fa, sizeof fa) != na) { emu_fail("stepra ai size"); return 0; }

    pos = 0;
    for (size_t i = 0; i < B3_VEHICLE_RANGE_COUNT; i++) {
        memcpy((unsigned char*)vehicle + B3_VEHICLE_RANGES[i].off, fv + pos,
               B3_VEHICLE_RANGES[i].len);
        pos += B3_VEHICLE_RANGES[i].len;
    }
    /* the driver block too: retail owns it in this mode, so mirror it back */
    pos = 0;
    for (size_t i = 0; i < B3_AIVEH_RANGES_COUNT; i++) {
        memcpy((unsigned char*)vehicle + B3_AIVEH_RANGES[i].off, fa + pos,
               B3_AIVEH_RANGES[i].len);
        pos += B3_AIVEH_RANGES[i].len;
    }
    g_emu_ai_calls++;
    return 1;
}

int b3_emu_cam(const float car_rows[12], float speed_ms, float boost_ramp,
               float dt, float yaw_deg, float pitch_deg, int look_back,
               float out_eye[3], float out_quat[4],
               float* out_fov, float* out_pitch, float* out_yaw)
{
    if (!g_ready && !b3_emu_init()) return 0;
    fprintf(g_out, "cam");
    for (int i = 0; i < 12; i++) fprintf(g_out, " %.9g", car_rows[i]);
    fprintf(g_out, " %.9g %.9g %.9g %.9g %.9g %d\n",
            speed_ms, boost_ramp, dt, yaw_deg, pitch_deg, look_back ? 1 : 0);
    fflush(g_out);

    char line[512];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on cam"); return 0; }
    if (strncmp(line, "cam ", 4) != 0) {
        fprintf(stderr, "[emu] cam: %s", line);
        b3_backend_demote(B3_FEAT_CAMERA, "bad reply");
        return 0;
    }
    float v[10];
    char* p = line + 4;
    for (int i = 0; i < 10; i++) {
        char* e;
        v[i] = strtof(p, &e);
        if (e == p) { emu_fail("short cam reply"); return 0; }
        p = e;
    }
    for (int i = 0; i < 3; i++) out_eye[i] = v[i];
    for (int i = 0; i < 4; i++) out_quat[i] = v[3 + i];
    *out_fov = v[7]; *out_pitch = v[8]; *out_yaw = v[9];
    return 1;
}

int b3_emu_score(const char* which, int arg, void* score)
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return 0;
    if (!sent && !(sent = b3_emu_send_scranges())) return 0;

    unsigned per = 0;
    for (size_t i = 0; i < B3_SCORE_RANGE_COUNT; i++) per += B3_SCORE_RANGES[i].len;

    static unsigned char flat[2048];
    static char enc[4 * ((2048 + 2) / 3) + 8];
    if (per > sizeof flat) return 0;

    unsigned pos = 0;
    for (size_t i = 0; i < B3_SCORE_RANGE_COUNT; i++) {
        memcpy(flat + pos, (const unsigned char*)score + B3_SCORE_RANGES[i].off,
               B3_SCORE_RANGES[i].len);
        pos += B3_SCORE_RANGES[i].len;
    }
    b64_encode(flat, per, enc);

    fprintf(g_out, "score %s %d %s\n", which, arg, enc);
    fflush(g_out);

    static char line[4 * ((2048 + 2) / 3) + 256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on score"); return 0; }
    if (strncmp(line, "sc ", 3) != 0) {
        fprintf(stderr, "[emu] score: %s", line);
        b3_backend_demote(B3_FEAT_SCORE, "bad reply");
        return 0;
    }
    if (b64_decode(line + 3, flat, sizeof flat) != per) return 0;

    pos = 0;
    for (size_t i = 0; i < B3_SCORE_RANGE_COUNT; i++) {
        memcpy((unsigned char*)score + B3_SCORE_RANGES[i].off, flat + pos,
               B3_SCORE_RANGES[i].len);
        pos += B3_SCORE_RANGES[i].len;
    }
    return 1;
}

int b3_emu_td_slam(float clock, int ncars, int attacker, int victim,
                   float strength, int type_byte, void* cars, size_t car_stride)
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return -1;
    if (!sent && !(sent = b3_emu_send_tdranges())) return -1;

    unsigned per = 0;
    for (size_t i = 0; i < B3_TDCAR_RANGE_COUNT; i++) per += B3_TDCAR_RANGES[i].len;

    static unsigned char flat[8192];
    static char enc[4 * ((8192 + 2) / 3) + 8];
    unsigned total = per * (unsigned)ncars;
    if (total > sizeof flat) return -1;

    unsigned pos = 0;
    for (int c = 0; c < ncars; c++) {
        const unsigned char* car = (const unsigned char*)cars + (size_t)c * car_stride;
        for (size_t i = 0; i < B3_TDCAR_RANGE_COUNT; i++) {
            memcpy(flat + pos, car + B3_TDCAR_RANGES[i].off, B3_TDCAR_RANGES[i].len);
            pos += B3_TDCAR_RANGES[i].len;
        }
    }
    b64_encode(flat, total, enc);

    fprintf(g_out, "tdslam %.9g %d %d %d %.9g %d %s\n",
            clock, ncars, attacker, victim, strength, type_byte, enc);
    fflush(g_out);

    static char line[4 * ((8192 + 2) / 3) + 256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on tdslam"); return -1; }
    if (strncmp(line, "td ", 3) != 0) {
        fprintf(stderr, "[emu] tdslam: %s", line);
        b3_backend_demote(B3_FEAT_TD_RULES, "bad reply");
        return -1;
    }
    int verdict = atoi(line + 3);
    char* sp = strchr(line + 3, ' ');
    if (!sp) { emu_fail("short tdslam reply"); return -1; }
    if (b64_decode(sp + 1, flat, sizeof flat) != total) return -1;

    pos = 0;
    for (int c = 0; c < ncars; c++) {
        unsigned char* car = (unsigned char*)cars + (size_t)c * car_stride;
        for (size_t i = 0; i < B3_TDCAR_RANGE_COUNT; i++) {
            memcpy(car + B3_TDCAR_RANGES[i].off, flat + pos, B3_TDCAR_RANGES[i].len);
            pos += B3_TDCAR_RANGES[i].len;
        }
    }
    return verdict;
}


// ---------------------------------------------------------------------------
// THE HANDOVER.
//
// A retail-owned step is a co-simulation, not a snapshot compare. Retail owns
// the vehicle for as long as it is driving it, so the port pushes its state
// ACROSS ONCE and mirrors the result from then on. Writing the recovered
// ranges every frame is what dropped the car through the floor: the port's
// copies of the fields retail REBUILDS (inv_frame, inv_inertia_world) and
// CLEARS (the four accumulators) go stale the moment the port's own step stops
// running, and handing them back put retail's suspension in the wrong basis.
//
// B3_VEHICLE_STATE_RANGES is that handover set; B3_VEHICLE_RANGES stays the
// read-back set, so the port keeps a complete mirror and can take the wheel
// again at any frame.
// ---------------------------------------------------------------------------
static int b3_emu_send_state_ranges(void)
{
    if (!g_ready) return 0;
    fprintf(g_out, "hranges %u", (unsigned)B3_VEHICLE_STATE_RANGE_COUNT);
    for (size_t i = 0; i < B3_VEHICLE_STATE_RANGE_COUNT; i++)
        fprintf(g_out, " %u %u", B3_VEHICLE_STATE_RANGES[i].off,
                B3_VEHICLE_STATE_RANGES[i].len);
    fputc('\n', g_out);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on hranges"); return 0; }
    return strncmp(line, "ok", 2) == 0;
}

int b3_emu_handover(int car, const void* vehicle, const float frame[4][4])
{
    static int sent = 0;
    if (!g_ready && !b3_emu_init()) return 0;
    if (!sent && !(sent = b3_emu_send_state_ranges())) return 0;

    unsigned nb = 0;
    for (size_t i = 0; i < B3_VEHICLE_STATE_RANGE_COUNT; i++)
        nb += B3_VEHICLE_STATE_RANGES[i].len;

    static unsigned char flat[8192];
    static char enc[4 * ((8192 + 2) / 3) + 8], fenc[128];
    if (nb > sizeof flat) return 0;

    unsigned pos = 0;
    for (size_t i = 0; i < B3_VEHICLE_STATE_RANGE_COUNT; i++) {
        memcpy(flat + pos,
               (const unsigned char*)vehicle + B3_VEHICLE_STATE_RANGES[i].off,
               B3_VEHICLE_STATE_RANGES[i].len);
        pos += B3_VEHICLE_STATE_RANGES[i].len;
    }
    b64_encode(flat, nb, enc);
    b64_encode((const unsigned char*)frame, 64, fenc);

    fprintf(g_out, "hstate %d %s %s\n", car, enc, fenc);
    fflush(g_out);
    char line[256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on hstate"); return 0; }
    if (strncmp(line, "ok", 2) != 0) {
        fprintf(stderr, "[emu] hstate: %s", line);
        b3_backend_demote(B3_FEAT_PHYSICS, "bad handover");
        return 0;
    }
    return 1;
}

int b3_emu_step_ranges(int car, float throttle, float brake, float steer,
                       int boost, float dt, void* vehicle, float frame[4][4])
{
    if (!g_ready || !vehicle) return 0;
    double t0 = now_ms();
    unsigned nb = b3_emu_range_bytes();

    static unsigned char flat[8192];
    static char enc[4 * ((8192 + 2) / 3) + 8], fenc[128];
    if (nb > sizeof flat) return 0;

    /* Inputs only. Retail owns the vehicle between handovers, so nothing of
     * the port's state is written here -- see b3_emu_handover above. */
    unsigned pos = 0;
    (void)enc; (void)fenc;
    fprintf(g_out, "stepr %d %.9g %.9g %.9g %d %.9g\n",
            car, throttle, brake, steer, boost ? 1 : 0, dt);
    fflush(g_out);

    static char line[4 * ((8192 + 2) / 3) + 256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on stepr"); return 0; }
    if (strncmp(line, "rng ", 4) != 0) {
        fprintf(stderr, "[emu] stepr: %s", line);
        emu_fail("bad reply");
        return 0;
    }
    char* w = line + 4;
    char* sp = strchr(w, ' ');
    if (!sp) { emu_fail("short stepr reply"); return 0; }
    *sp = '\0';
    if (b64_decode(w, flat, sizeof flat) != nb) { emu_fail("range size mismatch"); return 0; }
    b64_decode(sp + 1, (unsigned char*)frame, 64);

    pos = 0;
    for (size_t i = 0; i < B3_VEHICLE_RANGE_COUNT; i++) {
        memcpy((unsigned char*)vehicle + B3_VEHICLE_RANGES[i].off, flat + pos,
               B3_VEHICLE_RANGES[i].len);
        pos += B3_VEHICLE_RANGES[i].len;
    }

    g_last_ms = now_ms() - t0;
    g_sum_ms += g_last_ms;
    g_calls++;
    if ((g_calls % 600) == 0)
        fprintf(stderr, "[emu] %ld retail frames, %.2f ms avg (budget 16.67)\n",
               g_calls, b3_emu_avg_ms());
    return 1;
}

int b3_emu_step_window(int car, float throttle, float brake, float steer,
                       int boost, float dt, void* window, size_t win_len,
                       float frame[4][4])
{
    if (!g_ready || !window) return 0;
    double t0 = now_ms();

    static char enc[4 * ((B3_EMU_WINDOW_MAX + 2) / 3) + 8];
    static char fenc[128];
    if (win_len > B3_EMU_WINDOW_MAX) return 0;
    b64_encode((const unsigned char*)window, win_len, enc);
    b64_encode((const unsigned char*)frame, 64, fenc);

    fprintf(g_out, "stepraw %d %.9g %.9g %.9g %d %.9g %s %s\n",
            car, throttle, brake, steer, boost ? 1 : 0, dt, enc, fenc);
    fflush(g_out);

    static char line[4 * ((B3_EMU_WINDOW_MAX + 2) / 3) + 256];
    if (!emu_gets(line, (int)sizeof line, g_in)) { emu_fail("eof on stepraw"); return 0; }
    if (strncmp(line, "raw ", 4) != 0) {
        fprintf(stderr, "[emu] stepraw: %s", line);
        emu_fail("bad reply");
        return 0;
    }
    char* w = line + 4;
    char* sp = strchr(w, ' ');
    if (!sp) { emu_fail("short stepraw reply"); return 0; }
    *sp = '\0';
    if (b64_decode(w, (unsigned char*)window, win_len) != win_len) {
        emu_fail("window size mismatch"); return 0;
    }
    b64_decode(sp + 1, (unsigned char*)frame, 64);

    g_last_ms = now_ms() - t0;
    g_sum_ms += g_last_ms;
    g_calls++;
    return 1;
}

// b3_emu_apply -- the field-by-field marshaller that used to copy 32 values
// between the emulated body and the port's struct -- has been DELETED. It
// existed only because the two structs had different shapes; now that
// B3VehicleFull sits at retail's offsets, the bytes are the same bytes and
// b3_emu_step_window ships them directly.
