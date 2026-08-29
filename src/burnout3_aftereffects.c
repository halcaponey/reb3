/* AFTEREFFECTS — the in-race screen-space effects chain.
 *
 * Contract, architecture and the evidence split: burnout3_aftereffects.h.
 * Retail addresses: docs/RE_POSTFX.md, docs/RE_TAKEDOWN_FX.md.
 *
 * Nothing in this file copies the framebuffer and nothing reads it back. The
 * scene is rendered into a texture from the start, and every pass afterwards
 * is a shader draw from one texture into another.
 */
#include "burnout3_aftereffects.h"
#include "burnout3_rt.h"
#include "burnout3_postfx.h"
/* THE STATE SHADOW HAS TO SEE THIS FILE.  It sets depth writes, the depth
 * test, blending, culling and the colour mask around its FBO passes, and
 * burnout3_render.h is what routes those through the CPU shadow that
 * replaced glPushAttrib (GLES2 has no attribute stack).  A renderer module
 * that skips this header sets state behind the shadow's back, the shadow
 * then filters a later call as redundant when it is not, and the frame comes
 * out subtly wrong everywhere with nothing to grep for -- that was four
 * separate defects in the wave that built it.  B3_STATE_AUDIT=1 catches it. */
#include "burnout3_render.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * THE LAWS — GL-free, so tools/validate_postfx.py can compile and EXECUTE
 * them as a probe alongside burnout3_postfx.c's.
 * ========================================================================== */

float b3_afx_crash_weight(int divisor)
{
    if (divisor <= 1) return 0.0f;
    return 1.0f - 1.0f / (float)divisor;
}

float b3_afx_blur_s(float speed_mph, float boost_ramp, int divisor)
{
    /* The speed/boost half is the GLUE ramp burnout3_postfx.c already owns and
     * validate_postfx already gates -- one law, not two. The crash term is
     * INSPIRED and additive, so at divisor 1 this is EXACTLY the old value. */
    float s = b3_postfx_blur_strength(speed_mph, boost_ramp);
    return s + B3_AFX_CRASH_S * b3_afx_crash_weight(divisor);
}

float b3_afx_mask_r0(float crash_weight)
{
    /* INSPIRED: the sharp centre pulls in as time dilates. At weight 0 this is
     * exactly the old GLUE mask, so ordinary driving is unchanged. */
    float t = crash_weight;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return B3_BLUR_MASK_R0 + (B3_AFX_CRASH_MASK_R0 - B3_BLUR_MASK_R0) * t;
}

/* ==========================================================================
 * THE PHOTOREALISM LAYER'S SWITCHES — also GL-free, and for the same reason.
 *
 * They are read ONCE per process and latched, so nothing downstream can end up
 * disagreeing with the shader source that was assembled from the same answer:
 * a build that assembled the pre-wave composite because B3_PHOTO was 0 must
 * not then have an effect turned on under it by a later getenv.
 *
 * THE MASTER IS A HARD GATE, not a default.  B3_PHOTO=0 forces every one of
 * the six off no matter what the individual envs say, because the bit-identity
 * leg the recovered-pixel suites pin themselves to is only worth anything if
 * ONE variable can guarantee it.  (The suites set B3_PHOTO=0 and nothing else;
 * an operator with B3_PHOTO_SSAO=1 exported in their shell must not silently
 * turn a bit-exact gate into a statistical one.)
 * ========================================================================== */

static const char *const B3_PHOTO_ENV[B3_PHOTO_FX_COUNT] = {
    "B3_PHOTO_TONEMAP", "B3_PHOTO_SSAO",  "B3_PHOTO_ATMOS",
    "B3_PHOTO_SHADOW",  "B3_PHOTO_SSR",   "B3_PHOTO_GODRAY",
    "B3_PHOTO_LIGHTS"
};
/* ALL SIX SHIP ON.  SSR's inclusion was the one real decision -- see the note
 * over B3_PHOTO_SSR_* in the header: on the cost numbers alone it was the
 * obvious one to cut, and on the contact sheet it is the most photographic
 * thing in the frame. */
static const int B3_PHOTO_DEF[B3_PHOTO_FX_COUNT] = { 1, 1, 1, 1, 1, 1, 1 };

static int g_photo_master = -1;
static int g_photo_fx[B3_PHOTO_FX_COUNT];

static void photo_switches(void)
{
    const char *e;
    int i;
    if (g_photo_master >= 0) return;
    e = getenv("B3_PHOTO");
    g_photo_master = (e && *e) ? (atoi(e) != 0) : 1;
    for (i = 0; i < B3_PHOTO_FX_COUNT; i++) {
        if (!g_photo_master) { g_photo_fx[i] = 0; continue; }
        e = getenv(B3_PHOTO_ENV[i]);
        g_photo_fx[i] = (e && *e) ? (atoi(e) != 0) : B3_PHOTO_DEF[i];
    }
}

int b3_photo_on(void) { photo_switches(); return g_photo_master; }

int b3_photo_fx(int which)
{
    if (which < 0 || which >= B3_PHOTO_FX_COUNT) return 0;
    photo_switches();
    return g_photo_fx[which];
}

/* ---- tier 7's two GL-free asks -----------------------------------------
 *
 * UP HERE WITH THE LAWS, not down in the chain, and for the same reason
 * b3_photo_fx is: burnout3_scenery.c derives the light table while the track
 * loads -- long before there is a GL context, and on a run that may never
 * build one -- and the validator's probe build has to be able to execute both
 * without linking a single GL entry point. */
static float photo_envf(const char *name, float def)
{
    const char *e = getenv(name);
    return (e && *e) ? (float)atof(e) : def;
}

void b3_photo_light_rules(B3PhotoLightRules *out)
{
    if (!out) return;
    out->emis      = photo_envf("B3_PHOTO_LIGHT_EMIS",     B3_PHOTO_LIGHT_EMIS);
    out->emis_dark = photo_envf("B3_PHOTO_LIGHT_EMIS_DK",  B3_PHOTO_LIGHT_EMIS_DK);
    out->emis_lift = photo_envf("B3_PHOTO_LIGHT_EMIS_DL",  B3_PHOTO_LIGHT_EMIS_DL);
    out->emis_min  = photo_envf("B3_PHOTO_LIGHT_EMIS_MIN", B3_PHOTO_LIGHT_EMIS_MIN);
    out->emis_max  = photo_envf("B3_PHOTO_LIGHT_EMIS_MAX", B3_PHOTO_LIGHT_EMIS_MAX);
    out->tight     = photo_envf("B3_PHOTO_LIGHT_TIGHT",    B3_PHOTO_LIGHT_TIGHT);
    out->reach     = photo_envf("B3_PHOTO_LIGHT_REACH",    B3_PHOTO_LIGHT_REACH);
    out->rmin      = photo_envf("B3_PHOTO_LIGHT_RMIN",     B3_PHOTO_LIGHT_RMIN);
    out->rmax      = photo_envf("B3_PHOTO_LIGHT_RMAX",     B3_PHOTO_LIGHT_RMAX);
}

int b3_photo_head_slots(void)
{
    int n;
    if (!b3_photo_fx(B3_PHOTO_FX_LIGHTS)) return 0;
    /* B3_PHOTO_HEAD_ON IS NOT CONSULTED HERE, deliberately, and the reason is
     * about MEASUREMENT rather than about rendering.  This is the SHADER
     * ARRAY'S LENGTH; the switch decides whether the caller fills it, and a
     * slot the caller leaves alone is zeroed and costs a windowed falloff
     * against a colour of zero.  Letting the switch shorten the array made the
     * two legs of every beam A/B compile DIFFERENT PROGRAMS -- and the loader
     * runs on its own thread, so a program that takes a different time to
     * compile lands B3_SHOT_FRAME on a different race moment.  Measured: the
     * driving legs' world pin fell from 0.8 to 0.06 whenever the machine was
     * busy, which is a harness that cannot tell "the beams did nothing" from
     * "the beams photographed somewhere else".  One length for both legs
     * costs six idle uniform slots and buys an experiment. */
    n = (int)photo_envf("B3_PHOTO_HEAD_CARS", (float)B3_PHOTO_HEAD_CARS);
    if (n < 0)  n = 0;
    if (n > 8)  n = 8;      /* a racing grid, not a car park */
    return n;
}

int b3_photo_tail_slots(void)
{
    int n;
    if (!b3_photo_fx(B3_PHOTO_FX_LIGHTS)) return 0;
    /* B3_PHOTO_TAIL_ON is not consulted here for the reason spelled out over
     * b3_photo_head_slots(): this is the SHADER ARRAY'S LENGTH, and letting a
     * switch shorten it makes the two legs of an A/B compile different
     * programs, which lands a pinned shot on a different race moment.  The
     * switch decides whether the caller FILLS the slots; an unfilled slot is
     * uploaded as zero and costs one windowed falloff against black. */
    n = (int)photo_envf("B3_PHOTO_TAIL_CARS", (float)B3_PHOTO_TAIL_CARS);
    if (n < 0)  n = 0;
    if (n > 8)  n = 8;      /* a racing grid, not a car park */
    return n;
}

int b3_photo_boost_slots(void)
{
    int n;
    if (!b3_photo_fx(B3_PHOTO_FX_LIGHTS)) return 0;
    /* *** THE CEILING, and why this one is a POOL and not a per-car slot. ***
     *
     * AFX_LIGHT_MAX is 32.  It is not a performance budget -- the deferred
     * pass costs about 0.005 ms per slot at 1080p, so six more would be 0.03
     * ms and nobody would notice -- it is the length of three vec4 uniform
     * ARRAYS, and the length is what the ESSL 1.00 loop is unrolled against.
     * The shipped reservations are 18 streetlights + 6 beams; tier 7c's tails
     * are 6 more, which lands on 30 and leaves two.  A per-car flame slot
     * would want 36.
     *
     * COULD THE CEILING SIMPLY RISE?  The question was asked properly rather
     * than assumed away, and the answer is: not provably, and this port ships
     * on three targets.  32 slots is already 96 vec4s of fragment uniform;
     * GLES2 -- and therefore WebGL 1, and therefore the Android build through
     * gl4es -- guarantees only MAX_FRAGMENT_UNIFORM_VECTORS = 16, and every
     * other uniform in the deferred pass is drawn from the same pool.  Real
     * implementations give far more (afx_say() now prints what the running
     * context actually reports, on every target, so the next person to ask
     * this has a measurement instead of a guess), but "far more on the three
     * devices we tested" is not the same claim as "enough on every device a
     * browser runs on", and the failure mode is not graceful: afx_photo_build
     * halves the budget until the shader fits, and a halved 36 is 18, which
     * would take the beams and the tails down with it.  A ceiling that holds
     * everywhere at 32 and a pool of two is worth more than a ceiling of 36
     * that silently becomes 18 on somebody's phone.
     *
     * SO THE FLAMES SHARE TWO SLOTS.  That is not a consolation prize.  A
     * flame is lit for a few seconds at a time and dark for most of a lap,
     * which is exactly the shape a pool serves and a reservation wastes: the
     * caller gives the two slots to the two NEAREST cars that are actually
     * burning, and on the (common) frames where nobody is, the streetlights
     * get them back. */
    n = (int)photo_envf("B3_PHOTO_BOOST_LIGHTS", (float)B3_PHOTO_BOOST_LIGHTS);
    if (n < 0)  n = 0;
    if (n > 8)  n = 8;
    return n;
}

int b3_photo_light_budget(void)
{
    int n, h, t, b;
    if (!b3_photo_fx(B3_PHOTO_FX_LIGHTS)) return 0;
    n = (int)photo_envf("B3_PHOTO_LIGHT_N", (float)B3_PHOTO_LIGHT_N);
    if (n < 0)  n = 0;
    if (n > 32) n = 32;
    /* THE CAR LAMPS SIT BESIDE THE STREET'S, not inside it.  Adding the four
     * reservations rather than carving them out of one another is what makes
     * all of them real at once: B3_PHOTO_LIGHT_N is exactly what its name says
     * -- how many STREETLIGHTS a frame accumulates -- and lowering it cannot
     * put a racer's headlights, tail lamps or flame out.  The ceiling is the
     * uniform array's (AFX_LIGHT_MAX), and the lamps yield first if the sum
     * would breach it, because a lamp that is not picked this frame will be
     * picked next frame and a headlight that is not picked is a car driving
     * blind.
     *
     * AT THE SHIPPED DEFAULTS THE SUM IS EXACTLY 32: 18 + 6 + 6 + 2.  That is
     * arithmetic rather than luck -- B3_PHOTO_BOOST_LIGHTS is what was left
     * over, and the note over b3_photo_boost_slots() is where it was left over
     * FROM.  Lower any of the other three and the flames do not get the room:
     * the pool is a fixed two, and the street takes the slack. */
    h = b3_photo_head_slots();
    t = b3_photo_tail_slots();
    b = b3_photo_boost_slots();
    if (h + t + b > 32) {           /* pathological env; the street loses all */
        n = 0;
        while (h + t + b > 32) { if (b) b--; else if (t) t--; else h--; }
    } else if (n + h + t + b > 32) {
        n = 32 - (h + t + b);
    }
    return n + h + t + b;
}

#ifndef B3_AFX_NO_GL

#include <SDL2/SDL.h>
#include <GL/gl.h>
#ifdef __ANDROID__
/* ANDROID: desktop GL there is gl4es over GLES2, so the GL 2.0 entry points
 * must come from gl4es' own lookup rather than the raw driver -- the same
 * reason burnout3_postfx.c does this, and the same one-line block. */
#include <gl4esinit.h>
#define SDL_GL_GetProcAddress gl4es_GetProcAddress
#endif
/* WEB: the phase-2 note that used to stand here said this #if block was the
 * only thing in this file that would have to go once the renderer wave removed
 * gl4es from the web link.  It was right, and it has gone: <GL/gl.h> resolves
 * to web/GL/gl.h (GLES2) and plain SDL_GL_GetProcAddress returns Emscripten's
 * own WebGL entry points.  See web/GL/gl.h. */
#ifdef __EMSCRIPTEN__
#include "b3_web.h"          /* the frame profiler */
#endif

/* ------------------------------------------------------------- GL enums we
 * cannot rely on the platform header for. */
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER           0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0      0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT       0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16      0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24      0x81A6
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                  0x8058
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER           0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW            0x88E4
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER        0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER          0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS         0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS            0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0               0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE          0x812F
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER       0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER       0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES            0x8D57
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT        0x1902
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT       0x00000100
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT           0x1405
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT         0x1403
#endif

/* --------------------------------------------------------- entry points */

static unsigned (*p_glCreateShader)(unsigned);
static void (*p_glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*p_glCompileShader)(unsigned);
static void (*p_glGetShaderiv)(unsigned, unsigned, int *);
static void (*p_glGetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned, unsigned);
static void (*p_glLinkProgram)(unsigned);
static void (*p_glGetProgramiv)(unsigned, unsigned, int *);
static void (*p_glGetProgramInfoLog)(unsigned, int, int *, char *);
static void (*p_glUseProgram)(unsigned);
static void (*p_glDeleteShader)(unsigned);
static void (*p_glDeleteProgram)(unsigned);
static int  (*p_glGetUniformLocation)(unsigned, const char *);
static void (*p_glUniform1i)(int, int);
static void (*p_glUniform1f)(int, float);
static void (*p_glUniform2f)(int, float, float);
static void (*p_glUniform4f)(int, float, float, float, float);
static void (*p_glUniform4fv)(int, int, const float *);
static void (*p_glActiveTexture)(unsigned);
static int  (*p_glGetAttribLocation)(unsigned, const char *);
static void (*p_glEnableVertexAttribArray)(unsigned);
static void (*p_glDisableVertexAttribArray)(unsigned);
static void (*p_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char,
                                       int, const void *);
static void (*p_glGenBuffers)(int, unsigned *);
static void (*p_glBindBuffer)(unsigned, unsigned);
static void (*p_glBufferData)(unsigned, long, const void *, unsigned);
static void (*p_glDeleteBuffers)(int, const unsigned *);
static void (*p_glGenFramebuffers)(int, unsigned *);
static void (*p_glBindFramebuffer)(unsigned, unsigned);
static void (*p_glDeleteFramebuffers)(int, const unsigned *);
static void (*p_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned,
                                        int);
static unsigned (*p_glCheckFramebufferStatus)(unsigned);
static void (*p_glGenRenderbuffers)(int, unsigned *);
static void (*p_glBindRenderbuffer)(unsigned, unsigned);
static void (*p_glDeleteRenderbuffers)(int, const unsigned *);
static void (*p_glRenderbufferStorage)(unsigned, unsigned, int, int);
static void (*p_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned,
                                           unsigned);
/* MSAA resolve -- optional; NULL on WebGL1 and on GL < 3.0. */
static void (*p_glRenderbufferStorageMultisample)(unsigned, int, unsigned,
                                                  int, int);
static void (*p_glBlitFramebuffer)(int, int, int, int, int, int, int, int,
                                   unsigned, unsigned);
/* The photorealism layer's two, and they are OPTIONAL for the same reason the
 * MSAA pair is: their absence must cost the layer, never the chain.  They are
 * present on every GL 2.0 / GLES 2 / WebGL 1 context in practice -- this is
 * belt and braces, and it is what stops a context nobody anticipated from
 * turning "no shadows" into "no post-processing at all". */
static void (*p_glUniform3f)(int, float, float, float);
static void (*p_glUniformMatrix4fv)(int, int, unsigned char, const float *);

static int g_gl_state = -1;         /* -1 untried, 0 unavailable, 1 ready */
static int g_verbose;

/* THE REASON THE CHAIN IS NOT RUNNING, or NULL while it is.
 *
 * 2026-08-24: a user reported "no blur effects on PC" and the only way to tell
 * a chain that DECLINED from a chain that ran and was merely too faint was to
 * read a log line -- on stderr, in a wall of loader chatter, and only if the
 * operator thought to look. Every decline now records one short phrase here,
 * and burnout3_full.c's single unconditional verdict line prints it, so the
 * question "why do I have no effects" is answered by the FIRST line the game
 * prints about post-processing rather than by reading this file. */
static const char *g_why = "not initialised yet";
static char        g_why_buf[192];

/* Everything this module says goes through here, on stdout, unbuffered.
 *
 * STDOUT, not stderr (changed 2026-08-24). Nothing parses these lines -- a
 * sweep of tools/ and web/ finds no consumer -- and the whole point of them is
 * that a user who runs the game in a terminal and says "I see no blur" has
 * already produced the answer without being asked to redirect anything. The
 * older comment here argued for stderr so the two postfx modules interleaved;
 * they still do, because burnout3_postfx.c's own chatter is printf too.
 *
 * These lines are UNCONDITIONAL. B3_AFX_VERBOSE only ever adds detail -- it
 * has never gated a failure, and must not start: a platform quietly losing all
 * its effects behind a debug env is exactly the failure this exists to catch.
 *
 * A WARNING FOR WHOEVER DEBUGS THIS NEXT, because it cost hours here.
 * tools/web_smoke.py binds a FIXED port (8731), several agent worktrees run
 * smokes at once, and when the port is already held the browser is served
 * ANOTHER WORKTREE'S BUILD while every gate still passes. Every web
 * measurement taken that way describes someone else's binary. Two habits
 * prevent it: pass --port/--cdp explicitly, and check for a line that can ONLY
 * come from your build (burnout3_full.c prints a __DATE__/__TIME__ build stamp
 * at startup for exactly this). */
static void afx_say(const char *fmt, ...)
{
    va_list ap;
    fprintf(stdout, "[afx] ");
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* Record WHY the chain is not running and print it, in one call. Every early
 * return out of the build path goes through here, so there is no such thing as
 * a silent decline. */
static int afx_decline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_why_buf, sizeof g_why_buf, fmt, ap);
    va_end(ap);
    g_why = g_why_buf;
    afx_say("unavailable: %s\n", g_why_buf);
    return 0;
}

const char *b3_afx_status(void) { return g_why; }

static int afx_gl_load(void)
{
    if (g_gl_state >= 0) return g_gl_state;
    g_gl_state = 0;
    g_verbose = getenv("B3_AFX_VERBOSE") != NULL;

    /* A missing entry point is reported UNCONDITIONALLY. This chain degrades
     * to the older postfx path when it cannot be built, and a silent degrade
     * is how a platform quietly loses its effects for a release: the web
     * build did exactly that on the first try here, and the only way to find
     * out was to add this line. */
#define AFX_REQ(fn) do { *(void **)(&p_##fn) = SDL_GL_GetProcAddress(#fn); \
                         if (!p_##fn) { \
                             return afx_decline("no %s -- this GL context " \
                                                "cannot do GLSL/FBOs", #fn); \
                         } } while (0)
    AFX_REQ(glCreateShader);       AFX_REQ(glShaderSource);
    AFX_REQ(glCompileShader);      AFX_REQ(glGetShaderiv);
    AFX_REQ(glGetShaderInfoLog);   AFX_REQ(glCreateProgram);
    AFX_REQ(glAttachShader);       AFX_REQ(glLinkProgram);
    AFX_REQ(glGetProgramiv);       AFX_REQ(glGetProgramInfoLog);
    AFX_REQ(glUseProgram);         AFX_REQ(glDeleteShader);
    AFX_REQ(glDeleteProgram);      AFX_REQ(glGetUniformLocation);
    AFX_REQ(glUniform1i);          AFX_REQ(glUniform1f);
    AFX_REQ(glUniform2f);          AFX_REQ(glUniform4f);
    AFX_REQ(glActiveTexture);      AFX_REQ(glGetAttribLocation);
    AFX_REQ(glEnableVertexAttribArray);
    AFX_REQ(glDisableVertexAttribArray);
    AFX_REQ(glVertexAttribPointer);
    AFX_REQ(glGenBuffers);         AFX_REQ(glBindBuffer);
    AFX_REQ(glBufferData);         AFX_REQ(glDeleteBuffers);
    AFX_REQ(glGenFramebuffers);    AFX_REQ(glBindFramebuffer);
    AFX_REQ(glDeleteFramebuffers); AFX_REQ(glFramebufferTexture2D);
    AFX_REQ(glCheckFramebufferStatus);
    AFX_REQ(glGenRenderbuffers);   AFX_REQ(glBindRenderbuffer);
    AFX_REQ(glDeleteRenderbuffers);
    AFX_REQ(glRenderbufferStorage);
    AFX_REQ(glFramebufferRenderbuffer);
#undef AFX_REQ

    /* Optional. Their absence costs MSAA, not the chain. */
    *(void **)(&p_glRenderbufferStorageMultisample) =
        SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");
    *(void **)(&p_glBlitFramebuffer) =
        SDL_GL_GetProcAddress("glBlitFramebuffer");
    /* Optional. Their absence costs the photorealism layer, not the chain. */
    *(void **)(&p_glUniform3f) = SDL_GL_GetProcAddress("glUniform3f");
    *(void **)(&p_glUniformMatrix4fv) =
        SDL_GL_GetProcAddress("glUniformMatrix4fv");
    /* ...and its absence costs tier 7 alone: the per-source lights are the one
     * thing here that uploads an ARRAY. */
    *(void **)(&p_glUniform4fv) = SDL_GL_GetProcAddress("glUniform4fv");

    g_gl_state = 1;
    return 1;
}

/* ----------------------------------------------------------- shader build */

/* GLSL 1.10 on the desktop, ESSL 1.00 on the web: the `#ifdef GL_ES` guard is
 * the one construction both accept. No `#version` line, because adding one
 * would pin a dialect that the other target rejects. */
static const char *AFX_VS =
    "attribute vec2 aPos;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vUV = aPos;\n"
    "  gl_Position = vec4(aPos * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define AFX_FS_HEAD \
    "#ifdef GL_ES\n" \
    "precision highp float;\n" \
    "#endif\n" \
    "varying vec2 vUV;\n"

static unsigned afx_build(const char *fs_src, const char *what)
{
    unsigned vs, fs, pr;
    int ok = 0;
    char log[512];
    int n = 0;

    vs = p_glCreateShader(GL_VERTEX_SHADER);
    p_glShaderSource(vs, 1, &AFX_VS, NULL);
    p_glCompileShader(vs);
    p_glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        p_glGetShaderInfoLog(vs, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        afx_say("%s vertex shader failed: %s\n", what, log);
        p_glDeleteShader(vs);
        return 0;
    }
    fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &fs_src, NULL);
    p_glCompileShader(fs);
    p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        p_glGetShaderInfoLog(fs, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        afx_say("%s fragment shader failed: %s\n", what, log);
        p_glDeleteShader(vs);
        p_glDeleteShader(fs);
        return 0;
    }
    pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glAttachShader(pr, fs);
    p_glLinkProgram(pr);
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        p_glGetProgramInfoLog(pr, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        afx_say("%s link failed: %s\n", what, log);
        p_glDeleteProgram(pr);
        return 0;
    }
    return pr;
}

/* --------------------------------------------------------------- the passes
 *
 * DOWN — one halving. Four bilinear taps at the source's texel corners, which
 * with a 2:1 reduction is a 4x4 tent: each tap already averages a 2x2 block.
 * This is the prefilter, and it is the whole reason the blur below does not
 * ghost. The old web build sampled the FULL-RES frame with 16 offset taps,
 * which is not a smear but sixteen sharp copies of the frame -- the "multiple
 * frames at once" the user reported. Two of these halvings is retail's own
 * 640x480 -> 320x240 -> 160x120 reduction. [C] for the chain's existence and
 * its 1/4 endpoint (FUN_0003E520, RE_POSTFX 4b.2); the tap pattern is ours. */
static const char *AFX_FS_DOWN =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"          /* 1 / source size */
    "void main() {\n"
    "  vec2 o = uTexel;\n"
    "  vec3 c = texture2D(uTex, vUV + vec2(-o.x, -o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2( o.x, -o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2(-o.x,  o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2( o.x,  o.y)).rgb;\n"
    "  gl_FragColor = vec4(c * 0.25, 1.0);\n"
    "}\n";

/* RADIAL — the speed blur, run at quarter resolution.
 *
 * ***THIS PASS IS INSPIRED, NOT RECOVERED.*** Read the next paragraph before
 * citing anything here as game behaviour.
 *
 * WHAT RETAIL ACTUALLY DOES (recovered 2026-08-24, and it is NOT this) [C]:
 * the third reduction pass, FUN_0003E520 @0x0003F3A7, is a **3-tap HORIZONTAL
 * blur**. Its pixel shader (D3DPIXELSHADERDEF at 0x003EA248) decodes to
 * `out.rgb = c2*T0 + c3*T1 + c4*T2` with all three constants set to the same
 * 0.39999601 (0x003B1EE4, written to registers c2..c4 at 0x0003F4D3), and all
 * three samplers bound to the SAME surface, renderer+0x8C0 (0x0003F3B5 /
 * 0x0003F3BC / 0x0003F3C4). The three vertices' texcoords differ only in x, by
 * a constant -/+ 1.3333334 (0x003B1EE0 / 0x003B181C). So: one screen triangle,
 * three horizontal sub-texel taps, total gain 1.2.
 *
 * Three further negatives, all [C]:
 *   * `radialblurmask` is NEVER BOUND by this pass -- sampler 3 is explicitly
 *     NULLed at 0x0003F3CC, and the mask handle lives at blurState+0x40, below
 *     the +0x50 sub-struct that is the only thing either postfx function is
 *     handed (0x001AE5CC / 0x001AE601).
 *   * the 0.99 and 0.9998999834 "zoom layers" (blurState+0x10 / +0x78) are
 *     written by the constructor FUN_0002EBE0 and READ BY NOTHING: a whole-
 *     .text xref of 0x003B1758 and 0x003B18B4 finds only 0x0002EBFE and
 *     0x0002EC65, both inside that constructor, and neither literal appears
 *     anywhere in the 1937 instructions of FUN_0003DA90 + FUN_0003E520.
 *   * there is therefore no zoom-about-a-centre step anywhere in retail's
 *     recovered post path.
 *
 * WHY THIS PORT STILL GOES RADIAL. Because the brief is to be inspired by
 * Burnout 3, not to reproduce a 3-tap horizontal smear: a radial blur about
 * screen centre is what reads as SPEED to a modern eye, and it is the effect
 * the game's own presentation -- the boost FOV kick, the streaked exhaust --
 * is reaching for. The construction is ours and is marked as ours. What IS
 * kept from retail is the part that was genuinely recovered: the quarter-res
 * prefilter the taps sample, the over-unity tap gain (retail's 3 x 0.4 = 1.2;
 * the average below plus the composite's C0.a plays the same role), and the
 * present composite and gamma ramp downstream.
 *
 * INSPIRED detail: the taps are AVERAGED (energy preserving) rather than
 * summed. A sum makes the smear brighten the frame as well as soften it,
 * which is what made the old chain's edges glow; an average smears without
 * changing exposure. The mask is a radial ramp -- ours, standing in for
 * nothing, since retail has no mask here.
 *
 * THE MASK IS IN ALPHA, NOT IN THE COLOUR (changed 2026-08-24). It used to be
 * multiplied into the rgb, which was right while the composite ADDED this
 * surface -- a masked-to-black smear adds nothing at screen centre. The
 * composite now CROSS-FADES to it (see AFX_FS_COMPOSITE), and a black centre
 * would then fade the middle of the screen to black instead of leaving it
 * sharp. So the colour is the honest smear everywhere and the mask travels in
 * alpha as the per-pixel MIX WEIGHT, which is what it always meant. */
static const char *AFX_FS_RADIAL =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec4 uMask;\n"        /* x = r0, y = pow, z = zoom, w = 1/taps  */
    "void main() {\n"
    "  vec2 c = vec2(0.5, 0.5);\n"
    "  vec2 d = vUV - c;\n"
    "  float r = length(d) / 0.7071068;\n"
    "  float m = clamp((r - uMask.x) / max(1.0 - uMask.x, 1e-4), 0.0, 1.0);\n"
    "  m = pow(m, uMask.y);\n"
    "  vec3 acc = vec3(0.0);\n"
    "  float z = 1.0;\n"
    "  for (int i = 0; i < %d; i++) {\n"
    "    z *= uMask.z;\n"
    "    acc += texture2D(uTex, c + d * z).rgb;\n"
    "  }\n"
    "  gl_FragColor = vec4(acc * uMask.w, m);\n"
    "}\n";

/* BRIGHT — the bloom's highlight extraction, in RENDER-TARGET space.
 * INSPIRED; see the threshold's note in the header. A soft knee rather than a
 * hard cut, so a highlight drifting past the threshold fades in instead of
 * popping -- popping is exactly what a per-frame threshold does at speed. */
static const char *AFX_FS_BRIGHT =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "uniform float uThreshold;\n"
    "void main() {\n"
    "  vec2 o = uTexel;\n"
    "  vec3 c = texture2D(uTex, vUV + vec2(-o.x, -o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2( o.x, -o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2(-o.x,  o.y)).rgb\n"
    "         + texture2D(uTex, vUV + vec2( o.x,  o.y)).rgb;\n"
    "  c *= 0.25;\n"
    "  float l = max(max(c.r, c.g), c.b);\n"
    "  float k = clamp((l - uThreshold) / max(uThreshold, 1e-4), 0.0, 1.0);\n"
    "  gl_FragColor = vec4(c * k * k, 1.0);\n"
    "}\n";

/* BLUR — a 5-tap gaussian along uDir, run twice for a separable 2D blur.
 * The outer taps sit at 1.3333 texels so bilinear folds two samples into one:
 * five taps of reach for three fetches. INSPIRED (ordinary modern practice). */
static const char *AFX_FS_BLUR =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec2 uDir;\n"           /* (1/w, 0) or (0, 1/h) */
    "void main() {\n"
    "  vec2 o = uDir * 1.3333333;\n"
    "  vec3 c = texture2D(uTex, vUV).rgb * 0.29411765\n"
    "         + texture2D(uTex, vUV + o).rgb * 0.35294118\n"
    "         + texture2D(uTex, vUV - o).rgb * 0.35294118;\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* COMPOSITE — retail's present pass, plus the INSPIRED terms.
 *
 * [C] FUN_0003DA90 / the D3DPIXELSHADERDEF at 0x003E9EA8:
 *         out.rgb = 2 * ( scene.rgb + C0.a * blur.rgb )
 * the 2 being the combiner's SHIFTLEFTBY1 (PSRGBOutputs[0] = 0x00010C00,
 * OP field bits 15..17 = 2) and C0.a = min(s, 2) * 0.5 (0x0003DC42..0x0003DC62).
 *
 * *** THE BLUR TERM IS AN INSPIRED DEVIATION AS OF 2026-08-24. READ THIS. ***
 *
 * WHAT WENT WRONG. The recovered term is an ADD: the scene is composited at
 * FULL strength and a fraction of the blur surface is added on top. That
 * cannot blur anything. It can only add light -- the sharp image is still
 * all there underneath, at unit weight, no matter how large C0.a gets. This
 * was MEASURED, not reasoned: on a pinned 90 mph frame, driving C0.a all the
 * way to its recovered ceiling of 1.0 (B3_AFX_BLUR=8) moved the mean radial
 * gradient energy of the frame from 1.000 to 1.005 -- i.e. the picture got
 * very slightly SHARPER-edged, because everything got brighter, while the
 * frame-average delta was pure brightness. Six frames from 60 mph to 150 mph
 * + boost were visually indistinguishable. A user reported it as "I don't see
 * any blur effects on PC", and they were describing the output correctly:
 * there was no blur in it at any speed or any setting.
 *
 * Retail could live with this because retail's blur surface is a 3-tap
 * HORIZONTAL smear at 1.2 total gain (see AFX_FS_RADIAL) -- a soft glow was
 * the whole intent, and an add delivers a soft glow. This port's brief is to
 * be INSPIRED by Burnout 3, and its stated standard is that motion blur is
 * VISIBLE: perceptible by ~90 mph, dramatic under boost. An add cannot reach
 * that standard from any parameter value, so the parameter was never the bug.
 *
 * WHAT IT IS NOW. A cross-fade, with the mask as the per-pixel weight:
 *
 *     out.rgb = 2 * mix( scene.rgb, blur.rgb, C0.a * blur.a )
 *
 * Note what did NOT change. C0.a is still exactly the recovered
 * min(s,2)*0.5 -- b3_postfx_present_alpha(), untouched, still gated by
 * validate_postfx C7. uExposure still carries the recovered SHIFTLEFTBY1.
 * And at C0.a == 0 the expression is 2*scene, which is the recovered
 * equation exactly, so a stationary car presents bit-for-bit as before.
 * What changed is the BLEND OPERATOR between the two recovered endpoints,
 * and that is marked INSPIRED here, in the header, and in RE_POSTFX 4d.
 *
 * B3_AFX_PRESENT_ADD=1 restores the literal recovered add, so the [C]
 * equation stays RUNNABLE and anyone checking the evidence can still see it
 * produce its own pixels. (Same spirit as burnout3_postfx.c's
 * B3_POSTFX_PRESENT=0.) The blur surface carries its mask in alpha, so
 * `blur.rgb * blur.a` there is the identical masked colour the add used to
 * receive -- the restore is exact, not approximate.
 *
 * INSPIRED, unchanged: the bloom add (a glow SHOULD be additive, so it stays
 * an add and stays after the cross-fade), and the crash treatment's
 * desaturation + vignette. All are zero-valued when idle. */
#define AFX_COMPOSITE_MIX \
    "  c = mix(c, b.rgb, clamp(uAmt.x * b.a, 0.0, 1.0));\n"
#define AFX_COMPOSITE_ADD \
    "  c += uAmt.x * b.rgb * b.a;\n"

static const char *AFX_FS_COMPOSITE =
    AFX_FS_HEAD
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uBlur;\n"
    "uniform sampler2D uBloom;\n"
    "uniform vec4 uAmt;\n"      /* x blur, y bloom, z desat, w vignette */
    "uniform float uExposure;\n"
    "%s"                        /* the photorealism layer's uniforms, or ""   */
    "void main() {\n"
    "  vec3 c = texture2D(uScene, vUV).rgb;\n"
    "  vec4 b = texture2D(uBlur, vUV);\n"
    "%s"                        /* the blend: cross-fade, or the recovered add */
    "  c += uAmt.y * texture2D(uBloom, vUV).rgb;\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  c = mix(c, vec3(l), uAmt.z);\n"
    "  float r = length(vUV - vec2(0.5)) / 0.7071068;\n"
    "  c *= 1.0 - uAmt.w * r * r;\n"
    "%s"                        /* the ending: filmic + grade, or the raw x2  */
    "}\n";

/* GAMMA — [C] FUN_0003C8A0's ramp[i] = round((i/255)^0.95 * 255), delivered
 * as a 256x1 GL_NEAREST dependent lookup so the QUANTISED retail byte table is
 * reproduced rather than a continuous pow. Same construction the old chain
 * used and the same table (b3_postfx_gamma_table). */
static const char *AFX_FS_GAMMA =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uRamp;\n"
    "void main() {\n"
    "  vec3 c = texture2D(uTex, vUV).rgb;\n"
    "  gl_FragColor = vec4(texture2D(uRamp, vec2(c.r, 0.5)).r,\n"
    "                      texture2D(uRamp, vec2(c.g, 0.5)).r,\n"
    "                      texture2D(uRamp, vec2(c.b, 0.5)).r, 1.0);\n"
    "}\n";

/* ==========================================================================
 * THE PHOTOREALISM PASSES — six effects, all INSPIRED.
 *
 * Read burnout3_aftereffects.h's PHOTOREALISM LAYER block first: it carries
 * the contract, the evidence position (none of this is retail, and none of it
 * may be cited as retail) and every constant with its reasoning.  What is here
 * is the GLSL and the plumbing.
 *
 * THE ONE IDEA.  Five of the six read the scene's DEPTH, and once the depth is
 * a texture the cheapest place to put a light is a screen-space pass over the
 * finished frame.  So the world program in burnout3_render.c is not touched at
 * all, and the track / scenery / props / cars / traffic all receive the same
 * shadow, the same occlusion and the same haze from the same code — which the
 * legacy per-pass fog could never do, because the cars were never in it.
 *
 * EVERYTHING WORKS IN WORLD SPACE, reconstructed from depth by the shared
 * `b3W()` helper below.  That is deliberate: world space is the one frame in
 * which the shadow matrix, the sun vector, the height fog and the AO radius
 * are all expressible without a second sign convention to get wrong.  The
 * inverse view-projection the caller hands in is the inverse of the SAME
 * matrix the world was drawn with, display mirror included, so there is no
 * handedness arithmetic anywhere in this file.
 *
 * THE SOURCE IS ASSEMBLED, NOT BRANCHED.  An effect that is off is not in the
 * shader: the deferred pass is concatenated out of the blocks whose switches
 * are on.  That buys two things.  A frame with three effects live does not pay
 * for the three that are not, at all -- and with B3_PHOTO=0 the composite
 * string this file hands the driver is BYTE-IDENTICAL to the one it compiled
 * before the wave, which is what makes the bit-identity gate a fact about the
 * source rather than a hope about floating point.
 * ========================================================================== */

/* The shared preamble every photorealism pass starts from: the depth fetch and
 * the depth -> world reconstruction, spelled once. */
#define AFX_FS_PHOTO_HEAD \
    AFX_FS_HEAD \
    "uniform sampler2D uDepth;\n" \
    "uniform mat4 uInvVP;\n" \
    "uniform vec4 uCam;\n"        /* xyz eye, w far                        */ \
    "uniform vec4 uNF;\n"         /* x near, y far, z sky cut, w aspect    */ \
    "vec3 b3W(vec2 uv, float d) {\n" \
    "  vec4 p = uInvVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n" \
    "  return p.xyz / p.w;\n" \
    "}\n" \
    "float b3Lin(float d) {\n" \
    "  return (2.0 * uNF.x * uNF.y)\n" \
    "       / (uNF.y + uNF.x - (d * 2.0 - 1.0) * (uNF.y - uNF.x));\n" \
    "}\n"

/* ---------------------------------------------------------------- tier 2
 * SSAO, half resolution.
 *
 * A spiral of `taps` samples over the hemisphere the reconstructed normal
 * points into, each weighted by how far above the tangent plane it sits and
 * range-checked so a sample from another surface entirely cannot occlude.  The
 * spiral is rotated per pixel by a hash of gl_FragCoord, which turns banding
 * into noise that the bilateral blur below then eats.
 *
 * THE NORMAL comes from four depth taps at FULL resolution, taking the CLOSER
 * neighbour on each axis.  Taking both and averaging is one line shorter and
 * produces a normal that leans over every silhouette in the frame, which shows
 * up as a bright halo around every car -- the classic reconstructed-normal
 * artefact, and the reason this is written the long way. */
static const char *AFX_FS_AO =
    AFX_FS_PHOTO_HEAD
    "uniform mat4 uVP;\n"
    "uniform vec2 uTexel;\n"      /* 1 / FULL resolution, for the normal    */
    "uniform vec4 uAO;\n"         /* x radius, y bias, z power, w maxdist   */
    "uniform vec4 uAOAmt;\n"      /* x strength, y gain, z sr cap, w ang bias */
    "void main() {\n"
    "  float d0 = texture2D(uDepth, vUV).r;\n"
    "  float ao = 0.0;\n"
    "  if (d0 < uNF.z) {\n"
    "    vec3 P = b3W(vUV, d0);\n"
    "    vec3 V = P - uCam.xyz;\n"
    "    float dist = length(V);\n"
    "    vec3 vn = V / max(dist, 1e-4);\n"
    /* the four neighbours, closer one per axis */
    "    float dxp = texture2D(uDepth, vUV + vec2(uTexel.x, 0.0)).r;\n"
    "    float dxm = texture2D(uDepth, vUV - vec2(uTexel.x, 0.0)).r;\n"
    "    float dyp = texture2D(uDepth, vUV + vec2(0.0, uTexel.y)).r;\n"
    "    float dym = texture2D(uDepth, vUV - vec2(0.0, uTexel.y)).r;\n"
    "    vec3 ex = (abs(dxp - d0) < abs(dxm - d0))\n"
    "            ? (b3W(vUV + vec2(uTexel.x, 0.0), dxp) - P)\n"
    "            : (P - b3W(vUV - vec2(uTexel.x, 0.0), dxm));\n"
    "    vec3 ey = (abs(dyp - d0) < abs(dym - d0))\n"
    "            ? (b3W(vUV + vec2(0.0, uTexel.y), dyp) - P)\n"
    "            : (P - b3W(vUV - vec2(0.0, uTexel.y), dym));\n"
    "    vec3 N = normalize(cross(ex, ey));\n"
    /* face the camera whatever the winding of the mirrored world made it */
    "    if (dot(N, vn) > 0.0) N = -N;\n"
    /* the world radius, projected: one extra transform, no extra uniforms */
    "    vec3 T = cross(vn, vec3(0.0, 1.0, 0.0));\n"
    "    if (dot(T, T) < 1e-6) T = vec3(1.0, 0.0, 0.0);\n"
    "    T = normalize(T);\n"
    "    vec4 c0 = uVP * vec4(P, 1.0);\n"
    "    vec4 c1 = uVP * vec4(P + T * uAO.x, 1.0);\n"
    /* THE CAP IS A KNOB, and it is the one that decides whether this effect
     * is a contact cue or a cache disaster.  A world radius projected from
     * two metres in front of the camera covers most of the screen, and a
     * spiral of taps over most of the screen is both meaningless (every
     * sample is a different surface, so the range check throws them all away)
     * and the slowest thing in the frame. */
    "    float sr0 = max(length(c1.xy / c1.w - c0.xy / c0.w) * 0.5, 1e-6);\n"
    "    float sr = clamp(sr0, uTexel.x, uAOAmt.z);\n"
    /* ---- THE RADIUS THE TAPER HAS TO USE IS THE ONE THE DISC ACTUALLY
     * COVERS, and that is the whole of the near-field blanket.
     *
     * `sr` is a SCREEN radius and the clamp above is not cosmetic: at the FOV
     * this game drives at, a 4 m world radius projected from three metres in
     * front of the camera is most of the frame, so for everything in the near
     * field the cap BINDS and every pixel samples the same fixed screen disc.
     * The range taper below, however, goes on measuring each sample against
     * `uAO.x` metres -- a radius that disc no longer represents.  What the pass
     * then reports is not occlusion at all: it is how many of the twelve fixed
     * screen offsets happen to land within eight metres, which down a road is
     * a function of DISTANCE and nothing else.  It comes out as a flat grey
     * over the whole near field, stepping in visible horizontal bands as taps
     * cross the taper one by one, and stopping dead where
     * B3_PHOTO_AO_MAXDIST's fade completes -- the "uniform blanket over the
     * near road with a straight edge at mid-distance", travelling with the
     * camera because both halves of it are camera distances.
     *
     * Rescaling the radius by how much the clamp moved the disc puts the two
     * back in correspondence: when the cap binds, the effect degrades to a
     * SMALLER-radius occlusion that still measures geometry, instead of to a
     * distance ramp that measures the projection. */
    "    float R = uAO.x * (sr / sr0);\n"
    "    float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))\n"
    "                      * 43758.5453) * 6.2831853;\n"
    "    for (int i = 0; i < %d; i++) {\n"
    "      float fi = float(i) + 0.5;\n"
    "      float ang = fi * 2.3999632 + rot;\n"
    "      float rr = sqrt(fi / %d.0);\n"
    "      vec2 suv = vUV + vec2(cos(ang), sin(ang)) * rr * sr;\n"
    "      float sd = texture2D(uDepth, suv).r;\n"
    "      vec3 dv = b3W(suv, sd) - P;\n"
    "      float l = length(dv);\n"
    /* ---- THE ANGLE BIAS, and without it this pass cannot tell a road from a
     * corner.
     *
     * `dot(N, dv) / l` is the SINE of the angle the sample sits above the
     * tangent plane, and on a flat surface the true value is zero.  What the
     * frame actually delivers is zero plus the error in a normal reconstructed
     * from two depth taps -- which at the grazing angles a racing camera lives
     * at is a couple of degrees, so the per-sample value scatters either side
     * of zero.  `max(0.0, ...)` then RECTIFIES that scatter: the negative half
     * is clamped away and the positive half is kept, so a perfectly flat road
     * returns a positive mean instead of nothing.  Multiply that DC by
     * B3_PHOTO_AO_GAIN (3.6, fitted to make a real corner visible) and by
     * B3_PHOTO_AO_STRENGTH and the result is a uniform 30-40% multiply over
     * every open surface in the near field -- a blanket, ending exactly where
     * B3_PHOTO_AO_MAXDIST's fade completes, travelling with the camera because
     * that fade is a camera distance.  That is the artefact this fixes.
     *
     * MEASURED, on US_C1_V1 frame 655 at the size the game boots into: open
     * flat road came back at -42.62 levels and a genuine wheel-to-road contact
     * at -42.82.  An occlusion term that puts the same number on both is not
     * measuring occlusion, and no amount of gain or strength can separate them
     * afterwards.
     *
     * The fix is the standard one and it is a THRESHOLD IN THE SINE, not in
     * distance: reject everything below `uAOAmt.w` and rescale what is left
     * back to 0..1, so the noise floor goes to exactly zero while a sample
     * genuinely up a wall (sine 0.7..1.0) keeps almost all of its weight and
     * the gain does not have to be re-fitted around it.  The existing
     * `uAO.y / l` term stays: it is aimed at a different failure -- two samples
     * a few centimetres apart on a curved surface -- and it has to shrink with
     * distance, which is exactly why it cannot do this job as well. */
    "      float sinE = dot(N, dv) / max(l, 1e-4)\n"
    "                 - uAO.y / max(l, 1e-3);\n"
    "      float occ = max(0.0, (sinE - uAOAmt.w)\n"
    "                           / max(1.0 - uAOAmt.w, 1e-3));\n"
    /* THE RANGE CHECK, and it falls off to TWICE the radius rather than to
     * the radius itself.  A hard cut at the radius means a wall two metres
     * from the kerb contributes exactly nothing to a kerb pixel while a wall
     * 1.9 m away contributes fully, which reads as occlusion that switches on
     * and off along a street rather than as contact.  The gentler taper is
     * also what makes the effect survive the grazing angles a racing camera
     * spends its whole life at: the screen-space tap circle is wildly
     * anisotropic in world space down a road, so most taps land far away, and
     * a hard cut threw all of them out.  (MEASURED: with the hard cut the
     * occlusion buffer on the pinned frame was white everywhere except the
     * car's own bumper -- the pass ran, cost its taps and grounded nothing.) */
    "      occ *= clamp((2.0 * R - l) / max(R, 1e-4), 0.0, 1.0);\n"
    "      ao += occ;\n"
    "    }\n"
    /* THE GAIN, and why the raw average was useless.
     *
     * `ao` here is the MEAN of the per-sample occlusions, and for a pixel a
     * human would call "clearly in a corner" most of the samples still miss --
     * a kerb edge measures about 0.15 out of 1.  Raised to a contrast power
     * that is 0.06, which multiplied by any sane strength is invisible: the
     * first cut of this pass moved the pinned frame by 0.3 levels and touched
     * 3.9% of it, i.e. it was running and nobody could see it.
     *
     * The gain maps the range the geometry actually produces onto the range
     * the eye wants, and the clamp is what keeps it from turning a deep
     * interior corner into a hole.  Gain first, THEN the contrast power, so
     * the power shapes an already-useful signal instead of crushing a tiny
     * one. */
    "    ao = clamp(ao / %d.0 * uAOAmt.y, 0.0, 1.0);\n"
    "    ao = pow(ao, uAO.z);\n"
    "    ao *= clamp((uAO.w - dist) / (uAO.w * 0.35), 0.0, 1.0);\n"
    "  }\n"
    "  gl_FragColor = vec4(vec3(1.0 - uAOAmt.x * ao), 1.0);\n"
    "}\n";

/* The AO's blur: separable, seven taps, and BILATERAL -- the gaussian weight
 * is divided by how far the tap's linear depth is from the centre's.  A plain
 * gaussian here smears the occlusion under a car out across the road behind
 * it, which reads as a grey puddle rather than as contact. */
static const char *AFX_FS_AOBLUR =
    AFX_FS_PHOTO_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec2 uDir;\n"
    "void main() {\n"
    "  float dc = b3Lin(texture2D(uDepth, vUV).r);\n"
    "  float sum = 0.0, wsum = 0.0;\n"
    "  for (int i = -3; i <= 3; i++) {\n"
    "    vec2 uv = vUV + uDir * float(i);\n"
    "    float dd = b3Lin(texture2D(uDepth, uv).r);\n"
    "    float w = exp(-float(i * i) * 0.22) / (1.0 + abs(dd - dc) * 1.5);\n"
    "    sum += texture2D(uTex, uv).r * w;\n"
    "    wsum += w;\n"
    "  }\n"
    "  gl_FragColor = vec4(vec3(sum / max(wsum, 1e-4)), 1.0);\n"
    "}\n";

/* ---------------------------------------------------------------- tier 5
 * SSR, half resolution, gated by the track's own shine mask.
 *
 * March the reflected ray in WORLD space and project each step, rather than
 * marching in screen space: the world-space step is a constant number of
 * metres, so the march has the same physical reach whether the surface is
 * two metres away or two hundred, and there is no perspective-divide
 * bookkeeping to get wrong.  A step whose projected depth is behind the depth
 * buffer by less than the thickness is a hit; by more than it, the ray has
 * passed behind an object and the march is abandoned rather than reporting a
 * false hit on the object's back face.
 *
 * A MISS IS NOT BLACK.  The alpha channel carries "did this pixel get a
 * reflection", and the deferred pass mixes by it -- so a miss leaves the pixel
 * exactly as the existing env-map path drew it. */
static const char *AFX_FS_SSR =
    AFX_FS_PHOTO_HEAD
    "uniform sampler2D uTex;\n"       /* the scene, full res                */
    "uniform sampler2D uMaskTex;\n"   /* the track's shine mask             */
    "uniform mat4 uVP;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec4 uSsr;\n"            /* x stride, y thickness, z edge, w fres */
    "void main() {\n"
    "  float m = texture2D(uMaskTex, vUV).r;\n"
    "  vec4 outc = vec4(0.0);\n"
    "  float d0 = texture2D(uDepth, vUV).r;\n"
    "  if (m > 0.004 && d0 < uNF.z) {\n"
    "    vec3 P = b3W(vUV, d0);\n"
    "    vec3 vn = normalize(P - uCam.xyz);\n"
    "    float dxp = texture2D(uDepth, vUV + vec2(uTexel.x, 0.0)).r;\n"
    "    float dxm = texture2D(uDepth, vUV - vec2(uTexel.x, 0.0)).r;\n"
    "    float dyp = texture2D(uDepth, vUV + vec2(0.0, uTexel.y)).r;\n"
    "    float dym = texture2D(uDepth, vUV - vec2(0.0, uTexel.y)).r;\n"
    "    vec3 ex = (abs(dxp - d0) < abs(dxm - d0))\n"
    "            ? (b3W(vUV + vec2(uTexel.x, 0.0), dxp) - P)\n"
    "            : (P - b3W(vUV - vec2(uTexel.x, 0.0), dxm));\n"
    "    vec3 ey = (abs(dyp - d0) < abs(dym - d0))\n"
    "            ? (b3W(vUV + vec2(0.0, uTexel.y), dyp) - P)\n"
    "            : (P - b3W(vUV - vec2(0.0, uTexel.y), dym));\n"
    "    vec3 N = normalize(cross(ex, ey));\n"
    "    if (dot(N, vn) > 0.0) N = -N;\n"
    "    vec3 R = reflect(vn, N);\n"
    "    float stride = uSsr.x * max(1.0, b3Lin(d0) * 0.035);\n"
    "    vec3 S = P + N * 0.05;\n"
    "    for (int i = 0; i < %d; i++) {\n"
    "      S += R * stride;\n"
    "      vec4 cp = uVP * vec4(S, 1.0);\n"
    "      if (cp.w <= 0.0) break;\n"
    "      vec2 suv = (cp.xy / cp.w) * 0.5 + 0.5;\n"
    "      if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;\n"
    "      float sd = texture2D(uDepth, suv).r;\n"
    "      float zs = b3Lin(sd);\n"
    "      float zr = b3Lin((cp.z / cp.w) * 0.5 + 0.5);\n"
    "      float dz = zr - zs;\n"
    "      if (dz > 0.0) {\n"
    "        if (dz < uSsr.y) {\n"
    "          vec2 e = min(suv, 1.0 - suv);\n"
    "          float fade = clamp(min(e.x, e.y) / uSsr.z, 0.0, 1.0);\n"
    "          float fres = pow(1.0 - max(0.0, -dot(vn, N)), uSsr.w);\n"
    "          outc = vec4(texture2D(uTex, suv).rgb,\n"
    "                      fade * m * clamp(0.25 + fres, 0.0, 1.0));\n"
    "        }\n"
    "        break;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  gl_FragColor = outc;\n"
    "}\n";

/* ---------------------------------------------------------------- tier 6
 * GOD RAYS, two quarter-resolution passes.
 *
 * Pass one builds the SOURCE: what the sky contributes, and only what the sky
 * contributes, falling off with distance from the sun's projected position so
 * the shafts stay a local event around the sun rather than a global glow.
 * `sky` is a hard depth cut at the far plane, which is exactly the mask the
 * brief asks for -- a shaft can never be born on a wall. */
static const char *AFX_FS_GODSRC =
    AFX_FS_PHOTO_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSun;\n"     /* xy sun uv, z radius (v units), w on-screen */
    "void main() {\n"
    "  float d = texture2D(uDepth, vUV).r;\n"
    "  float sky = step(uNF.z, d);\n"
    "  vec2 dv = (vUV - uSun.xy) * vec2(uNF.w, 1.0);\n"
    "  float w = clamp(1.0 - length(dv) / max(uSun.z, 1e-3), 0.0, 1.0);\n"
    "  gl_FragColor = vec4(texture2D(uTex, vUV).rgb * sky * w * w * uSun.w,\n"
    "                      1.0);\n"
    "}\n";

/* Pass two is the scatter itself: a decaying tap train walking back toward the
 * sun.  Ordinary Mitchell-style radial scatter, and the frame's whole cost is
 * the taps, which is why it runs on a quarter-resolution source. */
static const char *AFX_FS_GOD =
    AFX_FS_HEAD
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSun;\n"     /* xy sun uv                                  */
    "uniform vec4 uGod;\n"     /* x density, y decay, z weight               */
    "void main() {\n"
    "  vec2 uv = vUV;\n"
    "  vec2 dv = (vUV - uSun.xy) * (uGod.x / %d.0);\n"
    "  float illum = 1.0;\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int i = 0; i < %d; i++) {\n"
    "    uv -= dv;\n"
    "    acc += texture2D(uTex, uv).rgb * illum * uGod.z;\n"
    "    illum *= uGod.y;\n"
    "  }\n"
    "  gl_FragColor = vec4(acc / %d.0, 1.0);\n"
    "}\n";

/* ---------------------------------------------------------------- the
 * DEFERRED pass: tiers 2, 3, 4 and 5 land on the frame here, in one draw.
 *
 * It is assembled from the blocks below, in this order, and a block whose
 * switch is off is simply not in the string.  The order is not arbitrary:
 * occlusion and shadow are things that happen to the SURFACE, so they come
 * first and multiply the surface's own colour; the reflection REPLACES part of
 * that surface; and the haze happens to the AIR between the surface and the
 * eye, so it comes last and mixes over the finished result.  Doing the haze
 * before the shadow would put the shadow on top of the fog, which is what a
 * mountain with a crisp shadow on it looks like and is the single most common
 * way this pass gets written wrong.
 *
 * "SCALE, DO NOT DOUBLE-DARKEN" is `b3lit` below, and it is shared by the AO
 * and the shadow.  The art already carries the artists' baked occlusion in its
 * vertex colours, so a pixel that is ALREADY dark is a pixel someone decided
 * was in shade; multiplying it again produces the black holes that give
 * deferred lighting over hand-painted art its reputation.  `b3lit` ramps the
 * term's authority from LITBIAS on a black pixel to 1 on a bright one. */
static const char *AFX_LIGHT_HEAD =
    AFX_FS_PHOTO_HEAD
    "uniform sampler2D uScene;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec3 uSunDir;\n"      /* world, TOWARD the sun                  */
    "uniform vec3 uSunCol;\n"
    "float b3lit(float lum, float bias) {\n"
    "  return mix(bias, 1.0, clamp(lum * 2.2, 0.0, 1.0));\n"
    "}\n";

/* Each effect's UNIFORM DECLARATIONS travel with the effect, and are spliced
 * in ahead of main() only when its block is.  A uniform an assembled shader
 * does not declare is one the driver cannot report a location for, which is
 * exactly the behaviour wanted: the call site checks `loc >= 0` before every
 * upload, so an off effect uploads nothing. */
static const char *AFX_LIGHT_U_AO =
    "uniform sampler2D uAOTex;\n"
    "uniform float uAOLit;\n";
static const char *AFX_LIGHT_U_SHADOW =
    "uniform sampler2D uShadow;\n"
    "uniform mat4 uShVP;\n"
    "uniform vec4 uSh;\n"          /* x strength, y bias, z slope, w texel   */
    "uniform vec4 uSh2;\n"         /* x fade, y litbias, z cool, w normal off */
    "uniform vec3 uShCool;\n"
    "uniform vec4 uSunD;\n";       /* x direct, y ref N.L, z lo clamp, w hi   */
/* uSunN TRAVELS WITH THE NORMAL, not with the shadow, and it used to travel
 * with the shadow -- which compiled fine for six years' worth of
 * configurations and then failed the first time something else wanted the
 * reconstructed normal.  With tier 7 on and tier 4 off the normal block was
 * spliced and its confidence uniform was not: "undefined variable uSunN", the
 * whole deferred pass refused to build, and every effect in it stood down.
 * A block's uniforms belong to the block that USES them. */
static const char *AFX_LIGHT_U_NORMAL =
    "uniform vec2 uSunN;\n";      /* x normal-confidence far, y its fade band */
static const char *AFX_LIGHT_U_SSR =
    "uniform sampler2D uSSRTex;\n"
    "uniform float uSsrAmt;\n";
/* tier 7.  The array LENGTH is spliced, so a run with a budget of four
 * declares four and unrolls four -- an ESSL 1.00 loop bound has to be a
 * constant anyway, and this way a frame does not pay for lights it does not
 * have room for. */
static const char *AFX_LIGHT_U_LIGHTS =
    "uniform vec4 uLightP[%d];\n"   /* xyz world, w = 1/radius^2             */
    "uniform vec4 uLightC[%d];\n"   /* rgb * power, a unused                 */
    "uniform vec4 uLightD[%d];\n"   /* xyz spot AIM, w cone exponent (0=omni) */
    "uniform vec2 uLightK;\n"      /* x gain, y litbias                     */
    "uniform vec4 uHeadK;\n";      /* the BEAMS' own: gain, litbias, wrap,
                                    * and the hot core's ellipse radius   */
/* THE FADE TARGET IS THE SKY, and it arrives as five coefficients rather than
 * a texture: the dome's own horizon band (b3_sky_horizon_band) fitted to its
 * mean plus two azimuthal harmonics.  cos and sin of the view azimuth are the
 * normalised vn.xz, so the evaluation is four multiply-adds and a double-angle
 * identity -- no atan, no extra sampler and no eighth texture unit, which
 * matters because GLES2 only promises eight and this pass already binds all
 * eight (scene, depth, AO, shadow, SSR, and tier 4r's three). */
static const char *AFX_LIGHT_U_ATMOS =
    "uniform vec4 uAtm;\n"         /* x density, y height, z strength, w near */
    "uniform vec3 uAtm2;\n"        /* x sungain, y sunpow, z skylean          */
    "uniform vec3 uAtmSky;\n"      /* the horizon band's MEAN                 */
    "uniform vec3 uAtmSkyC;\n"     /* its cos(azimuth) harmonic               */
    "uniform vec3 uAtmSkyS;\n"     /* its sin(azimuth) harmonic               */
    "uniform vec3 uAtmSkyC2;\n"    /* cos(2*azimuth) -- the cloud plate's     */
    "uniform vec3 uAtmSkyS2;\n";   /* sin(2*azimuth)                          */

/* The surface NORMAL, reconstructed the same careful way the AO pass does it.
 * It is spliced in only when something below wants it (today: the shadow's
 * slope-scaled bias), because it is four depth taps and four reconstructions
 * and there is no reason for a frame without shadows to pay them. */
static const char *AFX_LIGHT_NORMAL =
    "  vec3 Nw;\n"
    "  {\n"
    "    float dxp = texture2D(uDepth, vUV + vec2(uTexel.x, 0.0)).r;\n"
    "    float dxm = texture2D(uDepth, vUV - vec2(uTexel.x, 0.0)).r;\n"
    "    float dyp = texture2D(uDepth, vUV + vec2(0.0, uTexel.y)).r;\n"
    "    float dym = texture2D(uDepth, vUV - vec2(0.0, uTexel.y)).r;\n"
    "    vec3 ex = (abs(dxp - d) < abs(dxm - d))\n"
    "            ? (b3W(vUV + vec2(uTexel.x, 0.0), dxp) - P)\n"
    "            : (P - b3W(vUV - vec2(uTexel.x, 0.0), dxm));\n"
    "    vec3 ey = (abs(dyp - d) < abs(dym - d))\n"
    "            ? (b3W(vUV + vec2(0.0, uTexel.y), dyp) - P)\n"
    "            : (P - b3W(vUV - vec2(0.0, uTexel.y), dym));\n"
    /* GUARDED.  normalize() of a zero vector is a division by zero, and a
     * NaN here does not stay here: it goes through the shadow lookup, the
     * darkening and the cool tint and comes out as a garbage pixel.  ex and ey
     * are only degenerate where all four depth taps agree exactly -- which is
     * a flat surface seen dead-on, or the far plane -- but "only" is not
     * "never" and the cost of the guard is one add. */
    "    vec3 nn = cross(ex, ey);\n"
    "    Nw = (dot(nn, nn) > 1e-12) ? normalize(nn) : -vn;\n"
    "    if (dot(Nw, vn) > 0.0) Nw = -Nw;\n"
    "  }\n"
    /* HOW MUCH THE NORMAL IS WORTH, and it falls off with distance.
     *
     * A normal reconstructed from two depth taps is only as good as the depth
     * difference between them, and down a road at a grazing angle that
     * difference collapses: past a couple of hundred metres the reconstructed
     * normal is mostly noise, and anything driven by N.L there SHIMMERS from
     * frame to frame as the camera moves.  It is the classic
     * depth-derived-normal artefact and it is exactly what a viewer reports as
     * flicker, because a shimmer is invisible on a moving road and glaring
     * next to a HUD element that is not moving at all.
     *
     * So both N.L-driven terms -- the directional shading and the shadow's
     * geometric gate -- are faded out with distance rather than trusted
     * everywhere.  The shadow MAP itself is not faded: a depth comparison
     * needs no normal and stays exact all the way to the box edge. */
    "  float nconf = clamp((uSunN.x - dist) / max(uSunN.y, 1.0), 0.0, 1.0);\n";

static const char *AFX_LIGHT_MAIN =
    "void main() {\n"
    "  float d = texture2D(uDepth, vUV).r;\n"
    "  vec3 c = texture2D(uScene, vUV).rgb;\n"
    "  float sky = step(uNF.z, d);\n"
    "  vec3 P = b3W(vUV, d);\n"
    "  vec3 Vw = P - uCam.xyz;\n"
    "  float dist = length(Vw);\n"
    "  vec3 vn = Vw / max(dist, 1e-4);\n"
    "  float lum = dot(c, vec3(0.299, 0.587, 0.114));\n";

/* AO: a bilateral UPSAMPLE, not a bilinear one.  Four taps of the half-res
 * buffer, each weighted by how close its own linear depth is to this pixel's,
 * so the occlusion does not leak across the silhouette it was computed at. */
static const char *AFX_LIGHT_AO =
    "  {\n"
    "    float zc = b3Lin(d);\n"
    "    vec2 o = uTexel;\n"
    "    float s = 0.0, wsum = 0.0, w;\n"
    "    vec2 t0 = vUV + vec2(-o.x, -o.y);\n"
    "    vec2 t1 = vUV + vec2( o.x, -o.y);\n"
    "    vec2 t2 = vUV + vec2(-o.x,  o.y);\n"
    "    vec2 t3 = vUV + vec2( o.x,  o.y);\n"
    "    w = 1.0 / (0.06 + abs(b3Lin(texture2D(uDepth, t0).r) - zc));\n"
    "    s += texture2D(uAOTex, t0).r * w; wsum += w;\n"
    "    w = 1.0 / (0.06 + abs(b3Lin(texture2D(uDepth, t1).r) - zc));\n"
    "    s += texture2D(uAOTex, t1).r * w; wsum += w;\n"
    "    w = 1.0 / (0.06 + abs(b3Lin(texture2D(uDepth, t2).r) - zc));\n"
    "    s += texture2D(uAOTex, t2).r * w; wsum += w;\n"
    "    w = 1.0 / (0.06 + abs(b3Lin(texture2D(uDepth, t3).r) - zc));\n"
    "    s += texture2D(uAOTex, t3).r * w; wsum += w;\n"
    "    float ao = s / max(wsum, 1e-4);\n"
    "    c *= mix(1.0, ao, b3lit(lum, uAOLit) * (1.0 - sky));\n"
    "  }\n";

/* SHADOW: PCF over the sun's depth map, plus the COOL.  Losing the sun does
 * not make a surface grey, it leaves the sky's light on it -- so the shadowed
 * pixel is darkened AND pulled toward a blue-lean version of its own
 * luminance.  That second half is most of what makes these read as shadows
 * rather than as a multiply, and it is why B3_PHOTO_SH_COOL exists.
 *
 * The kernel's half-width is spliced in, so a 3x3 costs a 3x3's nine taps and
 * nothing declares a loop bound the compiler cannot unroll. */
static const char *AFX_LIGHT_SHADOW =
    "  {\n"
    "    float ndl = max(0.0, dot(Nw, uSunDir));\n"
    /* ---- THE DIRECTIONAL TERM, and it is half of what makes this tier
     * read as SUNLIGHT rather than as a stencil.
     *
     * A shadow map alone answers "is the sun blocked" and says nothing about
     * "is this surface FACING the sun" -- so with shadows on and nothing else,
     * a wall in full sun and a wall turned 80 degrees away are the same
     * brightness and the geometry reads flat and uniformly lifted.  A player
     * said exactly that ("light should be more directional") before this
     * block existed.
     *
     * It is deliberately ENERGY-PRESERVING rather than a lambert: the art
     * already carries the artists' own baked lighting, so this must not be a
     * second light, it must REDISTRIBUTE the one that is there.  The gain is
     * 1.0 at uSunD.y -- the N.L a typical surface in this scene already sits
     * at -- and leans up or down from there, so a sun-facing wall gains about
     * as much as a turned-away wall loses and the frame mean barely moves.
     * b3lit keeps it off the art's own painted shade, same as everything else
     * in this pass. */
    "    {\n"
    "      float g = 1.0 + uSunD.x * (ndl - uSunD.y);\n"
    "      g = clamp(g, uSunD.z, uSunD.w);\n"
    "      c *= mix(1.0, g, b3lit(lum, uSh2.y) * (1.0 - sky) * nconf);\n"
    "    }\n"
    /* THE NORMAL OFFSET, and it is what a depth bias alone cannot do.
     *
     * Shadow acne is a SAMPLING problem, not a depth problem: one shadow texel
     * covers a patch of a sloped surface, the map holds one depth for the whole
     * patch, and half the patch is therefore behind it.  A constant depth bias
     * big enough to cover that on a steep wall is big enough to lift a shadow
     * off the ground on a flat road, which is the other half of the classic
     * trade -- acne or peter-panning, pick one.
     *
     * Moving the LOOKUP POSITION along the surface normal instead sidesteps
     * the trade: it shifts the sample off the surface by about the size of the
     * texel that is causing the trouble.  It cost the pinned frame's
     * right-hand brick wall its speckle without moving a single shadow edge on
     * the road.
     *
     * ---- WHAT THE SCALE HAS TO BE, and getting it wrong is the "large shadow
     * travelling with the car" a player reported.
     *
     * The offset is a displacement in WORLD space, but what it buys is a
     * displacement in the MAP's own plane -- the lookup lands on a different
     * texel, and the only safe amount is a texel or two.  Moving `d` metres
     * along the normal moves the lookup `d * sin(angle(N, L))` metres sideways
     * in the map, so the scale that bounds the sideways walk is exactly that
     * sine.  It is also the physically right shape: zero when the surface
     * faces the sun (a texel covers no depth range there and no offset is
     * needed) and maximal when it is edge-on to it.
     *
     * The first cut scaled by `1 / max(ndl, 0.15)` instead -- "more offset as
     * the surface turns away" is the right instinct and a reciprocal is the
     * wrong function for it.  On a road under a low sun ndl IS about 0.15, so
     * the offset hit its 6.7x ceiling and the lookup walked 3.7 m -- fifteen
     * texels -- sideways across the map.  Fifteen texels is not a bias, it is
     * a different place: every road pixel in the near field read the shadow of
     * whatever stood ten to twenty metres down-sun of it, which on a street is
     * the buildings, so the whole near road inherited their shade as one slab.
     * And because `nconf` fades the offset out with distance, the slab stopped
     * at about 200 m and travelled with the camera -- a large shadow spanning
     * the road, moving with the car, exactly as reported.
     *
     * MEASURED, on a six-frame driving capture of US_C3_V1 (the shadow tier
     * against the same frames with it off): the near-road strip went from
     * -16.05 levels of mean luma to -0.62 with the offset removed entirely,
     * and neither a 20x depth bias nor moving the cascade box changed it by a
     * hundredth of a level -- so it was never acne and never the box.  With
     * the sine the sideways walk is at most `uSh2.w` metres, about two texels,
     * which is what the wall wanted in the first place. */
    "    float nsl = sqrt(max(0.0, 1.0 - ndl * ndl));\n"
    "    vec3 Ps = P + Nw * (uSh2.w * nconf * nsl);\n"
    "    vec4 lc = uShVP * vec4(Ps, 1.0);\n"
    "    vec3 lp = lc.xyz / lc.w;\n"
    "    vec2 luv = lp.xy * 0.5 + 0.5;\n"
    "    float lz = lp.z * 0.5 + 0.5;\n"
    "    float bias = uSh.y + uSh.z * (1.0 - ndl);\n"
    "    float vis = 0.0;\n"
    "    for (int y = -%d; y <= %d; y++) {\n"
    "      for (int x = -%d; x <= %d; x++) {\n"
    "        vec2 o = vec2(float(x), float(y)) * uSh.w;\n"
    "        vis += step(lz - bias, texture2D(uShadow, luv + o).r);\n"
    "      }\n"
    "    }\n"
    "    vis /= %d.0;\n"
    "    vec2 e = min(luv, 1.0 - luv);\n"
    "    float inbox = step(0.0, min(e.x, e.y)) * step(lz, 1.0)\n"
    "                * clamp(min(e.x, e.y) / max(1.0 - uSh2.x, 1e-3),\n"
    "                        0.0, 1.0);\n"
    /* a surface whose own normal faces away from the sun is not IN shadow,
     * it is UNLIT -- the map has nothing to say about it and a PCF tap on
     * the far side of a wall is noise.  Fold the geometric term in. */
    "    float sh = (1.0 - vis) * inbox * (1.0 - sky)\n"
    "             * mix(1.0, smoothstep(0.0, 0.30, ndl), nconf)\n"
    "             * b3lit(lum, uSh2.y);\n"
    "    c *= 1.0 - uSh.x * sh;\n"
    "    c = mix(c, dot(c, vec3(0.299, 0.587, 0.114)) * uShCool,\n"
    "            uSh2.z * sh);\n"
    "  }\n";

/* ==========================================================================
 * TIER 4r: THE RAY-TRACED SUN SHADOW.  INSPIRED, OFF BY DEFAULT.
 * ==========================================================================
 *
 * *** NOTHING IN THIS BLOCK IS A CLAIM ABOUT BURNOUT 3. ***  See
 * src/burnout3_rt.h for the whole design and for the switch; the short
 * version is that when the option is ON, the ray REPLACES the depth map's
 * term outright -- the map is not rendered at all that frame -- and when it
 * is OFF, none of the strings below is spliced and the assembled shader is
 * byte-identical to the one this file compiled before tier 4r existed.
 *
 * ----------------------------------------------- WHY IT IS A SEPARATE BLOCK
 * AFX_LIGHT_SHADOW is UNTOUCHED, deliberately.  The two blocks share their
 * opening (the directional relight) and their ending (the darken and the
 * cool tint) VERBATIM, and the honest thing to do with a shared ending is to
 * factor it -- but AFX_LIGHT_SHADOW is a hundred lines of interleaved
 * comments that other work is actively editing, and a refactor of it would
 * turn every one of those edits into a conflict for no behavioural gain.
 *
 * So the shared text is DUPLICATED and the duplication is GATED instead:
 * tools/validate_photo.py's tier-4r section asserts that the two blocks' shared
 * lines are character-identical, so a fix applied to one and not the other
 * fails the suite rather than drifting quietly.  If this ever stops being
 * the cheaper trade, factor them -- the gate will still hold.
 *
 * ------------------------------------------------------ THE ESSL 1.00 WALK
 * The dialect is the same one everything else in this file speaks (no
 * #version, `#ifdef GL_ES` + precision, one source for desktop GL, GLES2,
 * WebGL 1 and WebGL 2), and it constrains the traversal completely:
 *
 *   NO STACK.  ESSL 1.00 admits an array index only where it is a
 *   constant-index-expression, and a traversal stack pointer never is.  The
 *   BVH is therefore flattened with ESCAPE INDICES (tools/cextract/cx_bvh.c):
 *   a miss jumps to the escape, a hit descends to index + 1, and there is no
 *   stack to index.
 *
 *   NO BITWISE OPERATORS, and no floatBitsToInt.  A leaf's first triangle and
 *   its count ride in ONE float as `first * 8 + (count - 1)` and come back
 *   out with mod() and a divide -- exact, because a highp float carries 24
 *   bits and cx_bvh.c refuses a track that would need a 25th.
 *
 *   NO texelFetch, and no textureSize.  The two buffers are ordinary
 *   NEAREST-filtered textures addressed by arithmetic, and their dimensions
 *   arrive as uniforms.  The width is a power of two so `i / w` is exact.
 *
 *   CONSTANT LOOP BOUNDS.  Both counts are spliced with %d, the same way the
 *   PCF kernel and tier 7's light budget are.
 *
 * --------------------------------------------------------- THE SOFT EDGE
 * The sun is not a point: it subtends about half a degree, and THAT is what
 * makes a ray-traced shadow read as a photograph rather than as a stencil.
 * The rays are spread over a disc of that angular radius, so the penumbra
 * widens with the occluder's DISTANCE on its own -- a kerb stays sharp and a
 * rooftop two hundred metres away goes soft -- which is the one thing a
 * single-texel-size shadow map cannot do at any resolution.
 *
 * The spread pattern is a golden-angle spiral rotated by a SCREEN-LOCKED
 * hash: locked, and not animated, because there is no TAA in this port and
 * tools/validate_photo.py section 6 measures frame-to-frame change on a
 * pinned camera and calls it flicker.  A per-frame rotation would be
 * strictly better-looking in motion and would fail that gate, correctly.
 *
 * ------------------------------------------------------------- THE COST
 * Two early-outs carry most of it, and both are exact rather than
 * approximations: a SKY pixel has no surface to shade, and a pixel whose own
 * normal faces away from the sun is UNLIT rather than shadowed -- the tail
 * below already multiplies it out, so tracing it would be paying for a
 * number that is about to be multiplied by zero. */

static const char *AFX_LIGHT_U_SHADOW_RT =
    /* the same magnitudes tier 4 uses, minus the map: there is no uShadow
     * and no uShVP on this path because there is no map to sample. */
    "uniform vec4 uSh;\n"          /* x strength, y bias, z slope, w texel   */
    "uniform vec4 uSh2;\n"         /* x fade, y litbias, z cool, w normal off */
    "uniform vec3 uShCool;\n"
    "uniform vec4 uSunD;\n"        /* x direct, y ref N.L, z lo clamp, w hi   */
    "uniform sampler2D uRtNode;\n"
    "uniform sampler2D uRtTri;\n"
    "uniform vec4 uRtN;\n"   /* x node tex w, y 1/w, z 1/h, w node count     */
    "uniform vec4 uRtT;\n"   /* x tri  tex w, y 1/w, z 1/h, w unused         */
    "uniform vec4 uRt;\n"    /* x range, y origin offset, z tan(sun radius),
                              * w strength scale                             */
    /* THE FAR FIELD's two corrections.  See THE FAR FIELD in burnout3_rt.h
     * for the whole argument; the short version is that a shadow ray starts
     * at a position RECONSTRUCTED FROM DEPTH, whose world-space error grows
     * with the SQUARE of the range, and that a fixed 5 cm origin push stops
     * clearing that error at about 500 m -- past which the ray hits its own
     * triangle on half the frames, which is a flash. */
    "uniform vec4 uRt2;\n"   /* x dist bias, y N.L slope, z view grazing,
                              * w unused                                     */
    /* a component that is exactly zero would make its slab test 0 * inf */
    "float b3rtNZ(float v) {\n"
    "  return abs(v) < 1e-6 ? (v < 0.0 ? -1e-6 : 1e-6) : v;\n"
    "}\n"
    "vec4 b3rtNode(float i) {\n"
    "  float y = floor(i * uRtN.y);\n"
    "  float x = i - y * uRtN.x;\n"
    "  return texture2D(uRtNode, vec2((x + 0.5) * uRtN.y,\n"
    "                                 (y + 0.5) * uRtN.z));\n"
    "}\n"
    "vec4 b3rtTri(float i) {\n"
    "  float y = floor(i * uRtT.y);\n"
    "  float x = i - y * uRtT.x;\n"
    "  return texture2D(uRtTri, vec2((x + 0.5) * uRtT.y,\n"
    "                                (y + 0.5) * uRtT.z));\n"
    "}\n"
    /* TRANSMITTANCE, not visibility: a cut-out triangle carries the fraction
     * of its own sheet that is opaque (cx_bvh.c's OPACITY), so a chain-link
     * fence dapples instead of casting a solid rectangle. */
    /* `tmin` IS THE FAR FIELD'S OTHER HALF, and it is the robust half.  The
     * origin push above moves the ray off the surface along the surface
     * NORMAL -- but the normal is reconstructed from four depth taps, and at
     * range it is mostly noise (which this pass already knows: `nconf` fades
     * every N.L term with distance).  A push in a random direction is not a
     * push.  A tmin needs no normal at all: it simply refuses a hit closer
     * than the position's own error, which is exactly the hit that is the
     * surface itself. */
    "float b3rtTrans(vec3 P, vec3 L, float tmax, float tmin) {\n"
    "  vec3 iv = vec3(1.0 / b3rtNZ(L.x), 1.0 / b3rtNZ(L.y),\n"
    "                 1.0 / b3rtNZ(L.z));\n"
    "  float tr = 1.0;\n"
    "  float ni = 0.0;\n"
    "  for (int s = 0; s < %d; s++) {\n"
    "    if (ni >= uRtN.w) break;\n"
    "    vec4 A = b3rtNode(ni * 2.0);\n"
    "    vec4 B = b3rtNode(ni * 2.0 + 1.0);\n"
    "    vec3 q0 = (A.xyz - P) * iv;\n"
    "    vec3 q1 = (B.xyz - P) * iv;\n"
    "    vec3 nl = min(q0, q1);\n"
    "    vec3 nh = max(q0, q1);\n"
    "    float tn = max(max(nl.x, nl.y), max(nl.z, 0.0));\n"
    "    float tf = min(min(nh.x, nh.y), min(nh.z, tmax));\n"
    "    if (tn > tf) { ni = A.w; continue; }\n"
    "    if (B.w < 0.0) { ni += 1.0; continue; }\n"
    "    float cc = mod(B.w, 8.0);\n"
    "    float f0 = (B.w - cc) * 0.125;\n"
    "    for (int k = 0; k < 8; k++) {\n"
    "      if (float(k) > cc) break;\n"
    "      float ti = (f0 + float(k)) * 3.0;\n"
    "      vec4 V0 = b3rtTri(ti);\n"
    "      vec4 V1 = b3rtTri(ti + 1.0);\n"
    "      vec4 V2 = b3rtTri(ti + 2.0);\n"
    "      vec3 e1 = V1.xyz - V0.xyz;\n"
    "      vec3 e2 = V2.xyz - V0.xyz;\n"
    "      vec3 pv = cross(L, e2);\n"
    "      float dt = dot(e1, pv);\n"
    "      if (abs(dt) < 1e-8) continue;\n"
    "      float id = 1.0 / dt;\n"
    "      vec3 tv = P - V0.xyz;\n"
    "      float u = dot(tv, pv) * id;\n"
    "      if (u < 0.0 || u > 1.0) continue;\n"
    "      vec3 qv = cross(tv, e1);\n"
    "      float vv = dot(L, qv) * id;\n"
    "      if (vv < 0.0 || u + vv > 1.0) continue;\n"
    "      float th = dot(e2, qv) * id;\n"
    "      if (th <= tmin || th >= tmax) continue;\n"
    "      tr *= 1.0 - V0.w;\n"
    "    }\n"
    "    if (tr < 0.004) return 0.0;\n"
    "    ni = A.w;\n"
    "  }\n"
    "  return tr;\n"
    "}\n";

/* ==========================================================================
 * TIER 4rc: THE CARS, through a TWO-LEVEL trace.
 * ==========================================================================
 *
 * *** NOTHING IN THIS BLOCK IS A CLAIM ABOUT BURNOUT 3. ***  The Xbox drew a
 * "blobbyshadow" quad under each car (FUN_0019A7C0 / FUN_00043570, [C]) and
 * had no acceleration structure of any kind.  INSPIRED; see src/burnout3_rt.h.
 *
 * The static world's tree is built once and a car moves every frame, so a car
 * cannot be in it.  A car is RIGID, though, so its tree is built once in MODEL
 * space (tools/cextract/cx_car_bvh.c) and the whole per-frame cost is one 3x4
 * matrix per instance.  The ray transforms ITSELF into each car's space and
 * walks that car's tree; nothing is rebuilt.
 *
 * ------------------------------------------------------ THE TOP LEVEL
 * There is no acceleration structure over the INSTANCES, and that is a
 * decision rather than an omission: with a handful of them a linear scan
 * behind a bounding-sphere reject is cheaper than anything that would index
 * them, and it needs no per-frame build of its own -- which is the entire
 * point of the exercise.
 *
 * The reject is a RAY-SPHERE test in world space, before the transform, and
 * it carries the pass: a shadow ray from a road pixel is nowhere near most of
 * the field most of the time, and rejecting costs a dot product where
 * transforming and slab-testing costs eighteen multiplies.  The sphere is the
 * model box's own circumsphere (cx_car_bvh.c computes it and
 * tools/validate_car_bvh.py checks it CONTAINS the box), so a miss against it
 * is a real miss and not an approximation that could delete a shadow.
 *
 * ---------------------------------------------------- FIVE VEC4 AN INSTANCE
 * and no more, because a uniform array is the budget here:
 *
 *   uCarS   world sphere centre + radius^2.  w <= 0 is an EMPTY SLOT, which
 *           is how a frame with four cars pays for four and not for the
 *           array's length.
 *   uCarX/Y/Z  the WORLD -> MODEL 3x4, one row each with the translation in
 *           w.  The pose is rigid and orthonormal, so this is the draw
 *           matrix's transpose and the game hands it over already inverted.
 *   uCarN   the model's root and end node, in the PACKED buffer's indices.
 *
 * The model's BOX is not uploaded, deliberately: the root node's box IS the
 * model's box, so the first slab test of the walk already does that job.
 *
 * ------------------------------------------------------------- ONE TEXTURE
 * Nodes and triangles share it, with the triangles starting at uRtC.w.  The
 * world's tree gets two textures; this one does not get to, because ESSL
 * 1.00's guaranteed minimum is EIGHT fragment texture image units and the
 * deferred pass already binds scene, depth, AO, shadow, SSR and the world's
 * two.  This is the eighth. */

static const char *AFX_LIGHT_U_SHADOW_RT_CARS =
    "uniform sampler2D uRtCar;\n"
    "uniform vec4 uRtC;\n"   /* x tex w, y 1/w, z 1/h, w triangle base texel */
    "uniform vec4 uCarS[%d];\n"   /* xyz world centre, w radius^2 (<=0 empty) */
    "uniform vec4 uCarX[%d];\n"   /* world->model row 0 + translation         */
    "uniform vec4 uCarY[%d];\n"
    "uniform vec4 uCarZ[%d];\n"
    "uniform vec4 uCarN[%d];\n"   /* x root node, y end node                  */
    "vec4 b3rtCarTex(float i) {\n"
    "  float y = floor(i * uRtC.y);\n"
    "  float x = i - y * uRtC.x;\n"
    "  return texture2D(uRtCar, vec2((x + 0.5) * uRtC.y,\n"
    "                                (y + 0.5) * uRtC.z));\n"
    "}\n"
    /* The SAME stackless escape walk b3rtTrans runs, over one model's own run
     * of nodes.  It starts at `root` and stops when the index reaches `end` --
     * which is what keeps one car's traversal out of the next car's tree, and
     * is why cx_car_bvh.c writes each model's escapes already offset. */
    "float b3rtCarWalk(vec3 P, vec3 L, float tmax, float tmin,\n"
    "                  float root, float end) {\n"
    "  vec3 iv = vec3(1.0 / b3rtNZ(L.x), 1.0 / b3rtNZ(L.y),\n"
    "                 1.0 / b3rtNZ(L.z));\n"
    "  float tr = 1.0;\n"
    "  float ni = root;\n"
    "  for (int s = 0; s < %d; s++) {\n"
    "    if (ni >= end) break;\n"
    "    vec4 A = b3rtCarTex(ni * 2.0);\n"
    "    vec4 B = b3rtCarTex(ni * 2.0 + 1.0);\n"
    "    vec3 q0 = (A.xyz - P) * iv;\n"
    "    vec3 q1 = (B.xyz - P) * iv;\n"
    "    vec3 nl = min(q0, q1);\n"
    "    vec3 nh = max(q0, q1);\n"
    "    float tn = max(max(nl.x, nl.y), max(nl.z, 0.0));\n"
    "    float tf = min(min(nh.x, nh.y), min(nh.z, tmax));\n"
    "    if (tn > tf) { ni = A.w; continue; }\n"
    "    if (B.w < 0.0) { ni += 1.0; continue; }\n"
    "    float cc = mod(B.w, 8.0);\n"
    "    float f0 = (B.w - cc) * 0.125;\n"
    "    for (int k = 0; k < 8; k++) {\n"
    "      if (float(k) > cc) break;\n"
    "      float ti = uRtC.w + (f0 + float(k)) * 3.0;\n"
    "      vec4 V0 = b3rtCarTex(ti);\n"
    "      vec4 V1 = b3rtCarTex(ti + 1.0);\n"
    "      vec4 V2 = b3rtCarTex(ti + 2.0);\n"
    "      vec3 e1 = V1.xyz - V0.xyz;\n"
    "      vec3 e2 = V2.xyz - V0.xyz;\n"
    "      vec3 pv = cross(L, e2);\n"
    "      float dt = dot(e1, pv);\n"
    "      if (abs(dt) < 1e-8) continue;\n"
    "      float id = 1.0 / dt;\n"
    "      vec3 tv = P - V0.xyz;\n"
    "      float u = dot(tv, pv) * id;\n"
    "      if (u < 0.0 || u > 1.0) continue;\n"
    "      vec3 qv = cross(tv, e1);\n"
    "      float vv = dot(L, qv) * id;\n"
    "      if (vv < 0.0 || u + vv > 1.0) continue;\n"
    "      float th = dot(e2, qv) * id;\n"
    "      if (th <= tmin || th >= tmax) continue;\n"
    "      tr *= 1.0 - V0.w;\n"
    "    }\n"
    "    if (tr < 0.004) return 0.0;\n"
    "    ni = A.w;\n"
    "  }\n"
    "  return tr;\n"
    "}\n"
    "float b3rtCars(vec3 P, vec3 L, float tmax, float tmin) {\n"
    "  float tr = 1.0;\n"
    "  for (int i = 0; i < %d; i++) {\n"
    "    vec4 sp = uCarS[i];\n"
    "    if (sp.w <= 0.0) continue;\n"
    /* ray vs the instance's world sphere, and the whole pass leans on it */
    "    vec3 oc = sp.xyz - P;\n"
    "    float bq = dot(oc, L);\n"
    "    float hq = dot(oc, oc) - bq * bq;\n"
    "    if (hq > sp.w) continue;\n"
    "    float rq = sqrt(max(sp.w - hq, 0.0));\n"
    "    if (bq + rq < tmin || bq - rq > tmax) continue;\n"
    /* into the car's own space.  The pose is rigid, so the rotation carries
     * the direction unscaled and `tmax`/`tmin` mean the same on both sides --
     * which is what lets one budget serve the whole two-level trace. */
    "    vec3 Pm = vec3(dot(uCarX[i].xyz, P) + uCarX[i].w,\n"
    "                   dot(uCarY[i].xyz, P) + uCarY[i].w,\n"
    "                   dot(uCarZ[i].xyz, P) + uCarZ[i].w);\n"
    "    vec3 Lm = vec3(dot(uCarX[i].xyz, L), dot(uCarY[i].xyz, L),\n"
    "                   dot(uCarZ[i].xyz, L));\n"
    "    tr *= b3rtCarWalk(Pm, Lm, tmax, tmin, uCarN[i].x, uCarN[i].y);\n"
    "    if (tr < 0.004) return 0.0;\n"
    "  }\n"
    "  return tr;\n"
    "}\n";

/* THE STUB, and it is spliced whenever the cars are not.
 *
 * The ray loop calls b3rtCars() unconditionally, so the block below is what a
 * frame with no car trees compiles instead.  Writing the call out and letting
 * the compiler fold a constant 1.0 is deliberate: the alternative is a second
 * %s in AFX_LIGHT_SHADOW_RT, and that block's first and last paragraphs are
 * held character-identical to tier 4's by tools/validate_photo.py.  A format
 * string is a much easier thing to drift than a function that returns one. */
static const char *AFX_LIGHT_U_SHADOW_RT_NOCARS =
    "float b3rtCars(vec3 P, vec3 L, float tmax, float tmin) {\n"
    "  return 1.0;\n"
    "}\n";

/* The block itself.  Its FIRST and LAST paragraphs are character-identical
 * to AFX_LIGHT_SHADOW's, and validate_photo's tier-4r section keeps them
 * that way -- see WHY IT IS A SEPARATE BLOCK above. */
static const char *AFX_LIGHT_SHADOW_RT =
    "  {\n"
    "    float ndl = max(0.0, dot(Nw, uSunDir));\n"
    "    {\n"
    "      float g = 1.0 + uSunD.x * (ndl - uSunD.y);\n"
    "      g = clamp(g, uSunD.z, uSunD.w);\n"
    "      c *= mix(1.0, g, b3lit(lum, uSh2.y) * (1.0 - sky) * nconf);\n"
    "    }\n"
    /* ---- the ray, and the two exact early-outs before it ---- */
    "    float vis = 1.0;\n"
    "    if (sky < 0.5 && ndl > 0.0) {\n"
    /* THE ORIGIN.  A world position reconstructed from a depth buffer sits
     * ON the surface and carries the depth's own quantisation with it, so a
     * ray fired straight from it hits its own triangle.  One step along the
     * surface normal clears that, and unlike the map's normal offset it has
     * no texel to be measured in and no distance fade: it is a fixed few
     * centimetres, and nothing about the frame changes it. */
    /* ...AND IT GROWS WITH THE RANGE, because the error it is clearing does.
     * A depth buffer's world-space quantisation goes as dist^2 / near: about
     * 5 mm at 200 m with this camera, 4 cm at 600 m, 10 cm at 900 m.  A flat
     * 5 cm therefore stops covering it somewhere around half a kilometre, and
     * past there the ray starts alternately just above and just below its own
     * triangle as the camera moves -- which a player correctly reported as
     * "things far away have a shimmer / flash".  It is shadow acne, and a ray
     * having no texel to fall behind never meant it had no position error to
     * fall behind.  B3_RT_DIST_BIAS=0 restores the flat offset, which is what
     * makes the defect measurable rather than a story. */
    /* ...AND IT IS SLOPE-SCALED, which is the OTHER half and was the surprise.
     *
     * tier 4r shipped without a slope term under the argument that the whole
     * bias apparatus is a sampling artefact of a MAP.  Half of that is right:
     * a ray has no texel to fall behind.  It does have a PIXEL to fall behind,
     * and down a road at a grazing angle one pixel spans TENS OF METRES along
     * the view ray, so the single reconstructed position stands for a surface
     * that is nowhere near flat under it.  Two terms, and each was measured
     * on its own:
     *
     *   uRt2.y  the sun's own grazing angle, 1/max(N.L, 0.1) -- the standard
     *           slope scale, and what tier 4c's uSh.z does for the map.
     *   uRt2.z  the VIEW's grazing angle, 1/max(|N.V|, 0.1) -- which the map
     *           does NOT have and does not need, because a map's error is in
     *           the light's frame and a depth-reconstructed position's error
     *           is in the CAMERA's.  This is the one that reaches the road at
     *           the horizon.
     *
     * Both are blends toward the reciprocal rather than the reciprocal
     * itself, so 0 restores the flat term exactly and the defect can be
     * measured back into existence. */
    "      float ndv = abs(dot(Nw, vn));\n"
    "      float slope = 1.0 + uRt2.y * (1.0 / max(ndl, 0.1) - 1.0)\n"
    "                        + uRt2.z * (1.0 / max(ndv, 0.1) - 1.0);\n"
    "      float dbias = (uRt.y + uRt2.x * dist * dist) * slope;\n"
    "      vec3 Ps = P + Nw * dbias;\n"
    /* THE CONE.  A golden-angle spiral over a disc of the sun's own angular
     * radius, rotated per pixel by a SCREEN-LOCKED hash -- locked because
     * there is no TAA here and an animated one would be flicker. */
    "      vec3 tx = normalize(cross(uSunDir,\n"
    "                    abs(uSunDir.y) > 0.9 ? vec3(1.0, 0.0, 0.0)\n"
    "                                         : vec3(0.0, 1.0, 0.0)));\n"
    "      vec3 ty = cross(uSunDir, tx);\n"
    "      float rot = fract(sin(dot(gl_FragCoord.xy,\n"
    "                                vec2(12.9898, 78.233))) * 43758.5453)\n"
    "                * 6.2831853;\n"
    /* THE CONE IS NOT NARROWED WITH DISTANCE, and that is a MEASUREMENT
     * rather than an omission.
     *
     * The obvious second suspect for the far-field shimmer was this spiral:
     * four samples of a penumbra metres wide have real variance, and `rot` is
     * locked to the SCREEN, so sub-pixel camera motion slides a distant
     * surface onto a different pixel, hands it a different rotation and
     * resamples that variance.  It is a good story.  It is not what was
     * happening: collapsing the cone to a single hard ray (B3_RT_SUN_DEG=0)
     * removed 4% of the flicker, and going from 4 rays to 32 removed 1%.
     * The bias terms above removed 62% on their own.  So the cone stays as it
     * is -- narrowing it would have cost the contact hardness that is the
     * whole reason the option exists, in exchange for nothing. */
    "      float acc = 0.0;\n"
    "      for (int r = 0; r < %d; r++) {\n"
    "        float fr = (float(r) + 0.5) / %d.0;\n"
    "        float a = rot + float(r) * 2.3999632;\n"
    "        vec2 off = vec2(cos(a), sin(a)) * sqrt(fr) * uRt.z;\n"
    "        vec3 Ld = normalize(uSunDir + tx * off.x + ty * off.y);\n"
    /* THE STATIC WORLD, THEN THE CARS, and the product of the two is the
     * transmittance along that ray.  Multiplying is exactly right rather than
     * merely convenient: transmittance composes, so a ray that passes through
     * a chain-link fence AND under a car is darkened by both, and one that is
     * already blocked by the world costs the cars nothing because b3rtTrans
     * has already returned zero and the multiply is by zero.  When there are
     * no car trees b3rtCars() is the constant 1.0 above. */
    "        acc += b3rtTrans(Ps, Ld, uRt.x, dbias)\n"
    "             * b3rtCars(Ps, Ld, uRt.x, dbias);\n"
    "      }\n"
    "      vis = acc / %d.0;\n"
    "    }\n"
    /* `inbox` IS ALWAYS 1 HERE, and it is written out rather than folded
     * away on purpose: the four lines after it are character-identical to
     * tier 4's, which is what validate_photo's tier-4r section checks and
     * what stops a fix to one block quietly missing the other.  A ray has no
     * box to leave -- that whole boundary policy is a property of a map -- so
     * the compiler removes it and the reader keeps the diff. */
    "    float inbox = 1.0;\n"
    /* a surface whose own normal faces away from the sun is not IN shadow,
     * it is UNLIT -- the map has nothing to say about it and a PCF tap on
     * the far side of a wall is noise.  Fold the geometric term in. */
    "    float sh = (1.0 - vis) * inbox * (1.0 - sky)\n"
    "             * mix(1.0, smoothstep(0.0, 0.30, ndl), nconf)\n"
    "             * b3lit(lum, uSh2.y);\n"
    "    c *= 1.0 - uSh.x * uRt.w * sh;\n"
    "    c = mix(c, dot(c, vec3(0.299, 0.587, 0.114)) * uShCool,\n"
    "            uSh2.z * sh);\n"
    "  }\n";

/* TIER 7: the per-source lights, accumulated against depth and the
 * reconstructed normal.
 *
 * It is an ordinary deferred point-light loop and the only interesting parts
 * are the two things it does NOT do.
 *
 * It does not use an inverse-square falloff.  A physical point light has no
 * range, and a light with no range in a screen-space pass is a light that has
 * to be evaluated for every pixel of the frame; the windowed falloff below is
 * the standard bounded stand-in and it is what makes the nearest-N pick sound
 * -- a light outside its own radius contributes exactly zero, so dropping it
 * from the frame's list is not an approximation.
 *
 * And it does not double-brighten the art.  `b3lit` is the same ramp the
 * occlusion and the shadow use, run the other way up: the artists already
 * painted a pool of light under every lamp post they cared about, so a lamp
 * adds most of its light to a surface that is dark and very little to one
 * that is already bright.  Without it every painted highlight in the frame
 * gets a second one on top and the street reads as a bloom filter.
 *
 * ---------------------------------------------------------------------------
 * ...AND THE HEADLIGHTS ARE NOT SCENERY LAMPS.  This is the third round on one
 * user report -- "I still cannot see the cars' headlights on the track" -- and
 * the first two rounds fixed real defects that were not the one being seen.
 * Round three MEASURED it, and the number is not ambiguous.  Rendering a
 * 10-second drive twice, B3_PHOTO_HEAD_ON 1 and 0, and differencing the road
 * ahead of the chase car at the user's own 2048x1536:
 *
 *     US_C1_V1 (day)   99th-pct road delta   0.06,  1.67,  2.33 levels
 *     US_P1_V1 (dusk)  99th-pct road delta   2.00,  3.33,  5.00 levels
 *
 * ...on the open-road frames, i.e. under one level of 8-bit quantisation on
 * some of them.  The frames that DID pool (84, 88, 154 levels) are the tunnel
 * and the barrier: the frames where the beam hits something VERTICAL.
 *
 * That is the whole story, and it is N dot L.  The recovered type-0 lamp sits
 * a MEASURED 0.615 m above the road (b3_ground_probe under the beam origin, a
 * real height for a real car), so a beam reaching 10 m down the road meets it
 * at 0.0613 of full incidence and at 30 m at 0.0205.  A tunnel wall in the
 * same beam gets ~1.0.  A headlight was therefore delivering two to six per
 * cent of its energy to the one surface the whole feature exists to light,
 * and a hundred per cent to the wall beside it -- which is exactly why the
 * curated tunnel shot measured +66 levels and the user, driving, saw nothing.
 *
 * The physics is right and the picture is wrong, which is a look problem, and
 * the fix is the standard one: a WRAP term on the beams only.  `mix(nl, 1.0,
 * wrap)` is Valve's half-Lambert with the same justification it always had --
 * a headlight is not a point, its housing and the road's own scattering fill
 * the grazing angles the cosine throws away -- and at wrap 0.85 the road at
 * 10 m goes from 0.061 to 0.859, which is the fourteen-fold the measurement
 * says is missing.  It does NOT lift the tunnel wall (mix(1,1,w) is 1), so the
 * case that already worked is left exactly where it was.
 *
 * The beams get their own GAIN and their own lit-bias with it.  Sharing the
 * lamps' uLightK is what forced the caller to smuggle a ratio through the
 * per-light COLOUR to cancel a day curve it did not want; two more floats
 * retire that, and they let the beams answer the other half of the mandate --
 * a pool that reads on SUNLIT tarmac needs a bias near 1, where a streetlight
 * at noon still wants the lamps' restrained 0.55. */
static const char *AFX_LIGHT_LIGHTS =
    "  {\n"
    "    vec3 acc = vec3(0.0);\n"
    "    vec3 hac = vec3(0.0);\n"
    "    for (int i = 0; i < %d; i++) {\n"
    "      vec3 dl = uLightP[i].xyz - P;\n"
    "      float d2 = dot(dl, dl);\n"
    "      float att = clamp(1.0 - d2 * uLightP[i].w, 0.0, 1.0);\n"
    "      att *= att;\n"
    "      vec3  ld = dl * inversesqrt(max(d2, 1e-4));\n"
    "      float nl = max(0.0, dot(Nw, ld));\n"
    /* THE SPOT BRANCH IS THE HEADLIGHT BRANCH, and it needs no second flag: a
     * scenery lamp ships w == 0 and is omni by construction, a beam ships the
     * car's own aim and its two cone cosines.  `-ld` is the direction from the
     * LAMP to the pixel, which is what the aim has to be compared against.
     *
     * AN ELLIPSE, NOT A CONE, and this is the second half of why the pools did
     * not read.  The first cut used pow(cos, k), which is a BELL -- all peak,
     * no rim -- so once the wrap term gave the road the energy it was missing,
     * what the road showed was a smooth gradient over most of the visible
     * tarmac, which the eye files as "the exposure went up" rather than "that
     * car has its lights on".  Swapping the bell for a round spotlight with a
     * hot core and a smoothstepped rim did not fix it either, and the reason
     * is the CAMERA: the chase view looks down the same axis the beam points
     * along, so a round footprint is seen edge-on and its rims fall outside
     * the frame near the car and converge on the horizon far from it.  You
     * cannot see the shape of a cone you are standing inside.
     *
     * What a real headlight has, and what a round spotlight does not, is a
     * CUTOFF: the beam is wide across the road and shallow in elevation, so
     * its footprint is a bounded patch of tarmac with a far edge you can point
     * at.  That edge is the whole signature, and it survives being viewed
     * along the beam because it runs ACROSS the view.
     *
     * So the angular test is elliptical, in tangent space: the pixel's offset
     * from the aim is split into a horizontal and a vertical part against the
     * beam's own frame, each divided by its own spread, and the two are summed
     * in quadrature.  e < uHeadK.w is the hot core, e == 1 is the rim.  The
     * beam frame is built from world up rather than the car's, which is exact
     * for a level car and a cosmetic difference on a rolled one -- and it
     * costs a dot, a cross and a normalise instead of a second uniform array.
     *
     * WORKED THROUGH on the shipped angles (aim 0.062 = 3.55 degrees down,
     * cutoff 3.2, spread 30, hot 0.35, lamp a measured 0.615 m up): the pool
     * starts 5.2 m ahead, holds FULL power from 6.5 m to 21 m, is still at
     * 77% at 30 m and 34% at 45 m before the range window takes it, and at
     * 15 m it is full out to +-4.0 m of the lane and gone by +-8.0 m.  The
     * chase camera hides the first ten metres behind the car, which is why
     * the hot core is where it is rather than under the bumper. */
    "      if (uLightD[i].w > 0.0) {\n"
    "        vec3  A = uLightD[i].xyz;\n"
    "        vec3  v = -ld;\n"
    "        float f = dot(v, A);\n"
    "        float sp = 0.0;\n"
    "        if (f > 1e-4) {\n"
    "          vec3 u0 = vec3(0.0, 1.0, 0.0) - A * A.y;\n"
    "          float ul = length(u0);\n"
    "          vec3 Uu = ul > 1e-3 ? u0 / ul : vec3(0.0, 0.0, 1.0);\n"
    "          vec3 Rr = cross(Uu, A);\n"
    "          float x = dot(v, Rr) / f;\n"
    "          float y = dot(v, Uu) / f;\n"
    "          float e = x * x * uLightC[i].a + y * y * uLightD[i].w;\n"
    "          sp = 1.0 - smoothstep(uHeadK.w, 1.0, e);\n"
    "        }\n"
    "        hac += uLightC[i].rgb\n"
    "             * (att * mix(nl, 1.0, uHeadK.z) * sp);\n"
    "      } else {\n"
    "        acc += uLightC[i].rgb * (att * nl);\n"
    "      }\n"
    "    }\n"
    "    float lb = clamp(lum * 2.2, 0.0, 1.0);\n"
    "    c += (acc * uLightK.x * mix(1.0, uLightK.y, lb)\n"
    "       +  hac * uHeadK.x  * mix(1.0, uHeadK.y,  lb)) * (1.0 - sky);\n"
    "  }\n";

static const char *AFX_LIGHT_SSR =
    "  {\n"
    "    vec4 r = texture2D(uSSRTex, vUV);\n"
    "    c = mix(c, r.rgb, clamp(r.a * uSsrAmt, 0.0, 1.0) * (1.0 - sky));\n"
    "  }\n";

/* ATMOSPHERICS.  An exponential in distance times an exponential in HEIGHT,
 * with the height term integrated along the ray by its midpoint -- which is
 * the standard cheap approximation and is exact for a horizontal ray, i.e. for
 * the racing camera, essentially always.
 *
 * THE COLOUR IT FADES TO IS THE SKY IT SILHOUETTES AGAINST, and getting that
 * wrong is not a subtle error -- it is the difference between a distant ridge
 * and a light-grey cut-out pasted over the weather.  This pass used to fade
 * toward a mix of the track's sun COLOUR and a compiled-in daylight blue, and
 * both halves of that were wrong on any track whose sky is not a bright day:
 *
 *   * enviro.dat +0x60 is a TINT, not a radiance.  All 36 shipped tracks
 *     store it with a component at 0.90 or above -- the darkest is US_M1's
 *     (0.937, 0.741, 0.561), a warm dusk HUE at full brightness.  There is no
 *     track whose sun colour is dim, so no track whose haze the sun colour
 *     ever dimmed, and the "dusk dims the scatter" reading was never true.
 *   * the sky blue was a constant, 0.50/0.64/0.86.  On US_M1 -- a coastal
 *     storm, sky measured at 12..40 levels -- the two together put the far
 *     band at 152 against a sky at 72, i.e. the distant city came out TWICE
 *     as bright as the weather behind it, in flat neutral grey.
 *
 * So the target now comes from the DOME: b3_sky_horizon_band() runs the sky
 * pass's own two blends, per azimuth, over the elevation band distant
 * geometry occupies, and hands back the colour the dome writes to the same
 * render target this pass is reading.  It arrives fitted to a mean and two
 * azimuthal harmonics (uAtmSky, uAtmSkyC/S, uAtmSkyC2/S2 -- see
 * afx_atm_sky_fit), so the fade target follows the view around the compass:
 * brighter on the sun's side, because the LUT's own glow pass already put it
 * there, and dark where the storm plate is, because the cloud pass did.
 *
 * The sun may still colour the scatter, but only its HUE: `warm` is uSunCol
 * renormalised to the SKY's luminance in this direction, so leaning into it
 * turns a distant ridge gold without ever lifting it above the sky.  The
 * forward lobe's extra BRIGHTNESS stays where it always was, in the amount
 * `f`, where saturating at 1 is a floor on how bad it can get.
 *
 * AND IT ADDS AIRLIGHT, IT DOES NOT SUBTRACT THE OBJECT'S OWN.  This block
 * always claimed to be "additive to the recovered fog, not a replacement for
 * it" (docs/PHOTOREALISM.md tier 3) and then wrote a plain lerp, which is
 * both halves of the transport equation -- and the extinction half is
 * retail's fog, already applied to this very pixel by the world pass over the
 * track's own fog_start/fog_end/fog_far.  Doing it twice is why, with the
 * target corrected, the far city came out DARKER with the tier on than with
 * it off (26.4 against 28.3 on the repro frame): the pass was taking light
 * away from geometry that the fog had already taken it from.  The `max` is
 * the whole fix -- per channel, the target can only ever lift. */
static const char *AFX_LIGHT_ATMOS =
    "  {\n"
    "    const vec3 W709 = vec3(0.2126, 0.7152, 0.0722);\n"
    "    float t = max(0.0, dist - uAtm.w);\n"
    "    float hm = max(0.0, (P.y + uCam.y) * 0.5);\n"
    "    float hf = exp(-hm / uAtm.y);\n"
    "    float f = 1.0 - exp(-t * uAtm.x * hf);\n"
    "    float sun = pow(max(0.0, dot(vn, -uSunDir)), uAtm2.y);\n"
    "    float hl = max(length(vn.xz), 1e-4);\n"
    "    float ca = vn.x / hl, sa = vn.z / hl;\n"
    "    vec3 skyc = max(uAtmSky + uAtmSkyC * ca + uAtmSkyS * sa\n"
    "                    + uAtmSkyC2 * (ca * ca - sa * sa)\n"
    "                    + uAtmSkyS2 * (2.0 * ca * sa), vec3(0.0));\n"
    "    vec3 warm = uSunCol * (dot(skyc, W709)\n"
    "                           / max(dot(uSunCol, W709), 1e-4));\n"
    "    vec3 base = mix(warm, skyc, uAtm2.z);\n"
    "    vec3 col = mix(base, warm, sun);\n"
    "    f = clamp(f * uAtm.z * (1.0 + uAtm2.x * sun), 0.0, 1.0);\n"
    "    c = mix(c, max(col, c), f * (1.0 - sky));\n"
    "  }\n";

static const char *AFX_LIGHT_TAIL =
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* ---------------------------------------------------------------- tier 1
 * THE FILMIC ENDING, spliced onto the composite in place of its raw
 * `c * uExposure`.
 *
 * `b3Grade` is a 16x16x16 lookup, stored as one 256x16 texture with the blue
 * slices laid out along x and interpolated by hand -- ESSL 1.00 has no 3-D
 * sampler, and a pair of 2-D fetches plus a mix is what every engine that has
 * ever shipped a LUT on GLES does.  The table itself is GENERATED by
 * afx_photo_lut_build() from the six grade constants; there is no asset. */
/* The god rays land HERE, before the curve, and that placement is the whole
 * argument for having done tier 1 first: added after the curve they would
 * clip, and a clipped light shaft is a white wedge with a hard edge.  Added
 * before it, the shoulder catches them and the frame GLOWS instead. */
#define AFX_COMPOSITE_GOD \
    "  c += uPhoto.x * texture2D(uGodTex, vUV).rgb;\n"

/* THE CURVE, and the one thing about it that is easy to get badly wrong.
 *
 * ACES is a fit of a curve over LINEAR scene radiance.  What arrives here is
 * not that: the scene target is an 8-bit surface holding what the world
 * program wrote, which is display-referred -- the artists' vertex colours and
 * the textures are already gamma-encoded, because the Xbox had nowhere else to
 * put them.  Feeding display-referred values to a filmic fit lifts the whole
 * picture, and it did: the first cut of this pass measured +37 levels of frame
 * mean on the pinned frame, which is not a tonemap, it is a wash.
 *
 * So the value is LINEARISED first, curved, and re-encoded.  Gamma 2.0 rather
 * than 2.2 -- x*x and sqrt(x), two instructions instead of two pow() calls,
 * and the 0.2 of exponent between them is far smaller than the error already
 * in calling the source sRGB at all.
 *
 * NOTE WHERE THE RECOVERED x2 IS.  It is still there, still uExposure, still
 * applied to the display-referred value exactly as the [C] SHIFTLEFTBY1 did --
 * the squaring happens after it, so `(c*2)^2` is the recovered exposure
 * expressed in linear space, and uPhoto.y is the only new number.
 *
 * uPhoto.y IS MEASURED, not chosen: 0.724 is the value that puts the pinned
 * frame's mid-tone back exactly where the recovered clamp had it (solve
 * ACES(0.1296*K) = 0.5184 for the frame's own mid-grey, K = 2.896 = 4*0.724).
 * What the frame gains is the top: a render-target 0.5 used to land on 255 and
 * so did a 0.8, and they now land on 217 and 243 -- two stops that existed in
 * the render target and were being thrown away at the very last step. */
#define AFX_COMPOSITE_TONEMAP \
    "  vec3 x = c * uExposure;\n" \
    "  x = max(x * x * uPhoto.y, 0.0);\n" \
    "  x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);\n" \
    "  x = sqrt(clamp(x, 0.0, 1.0));\n" \
    "  float N = %d.0;\n" \
    "  float bq = x.b * (N - 1.0);\n" \
    "  float b0 = floor(bq);\n" \
    "  float b1 = min(b0 + 1.0, N - 1.0);\n" \
    "  vec2 uvb = vec2((x.r * (N - 1.0) + 0.5) / (N * N),\n" \
    "                  (x.g * (N - 1.0) + 0.5) / N);\n" \
    "  vec3 g0 = texture2D(uLut, uvb + vec2(b0 / N, 0.0)).rgb;\n" \
    "  vec3 g1 = texture2D(uLut, uvb + vec2(b1 / N, 0.0)).rgb;\n" \
    "  gl_FragColor = vec4(mix(g0, g1, bq - b0), 1.0);\n"

/* ...and the recovered ending, byte for byte as it was.  With B3_PHOTO=0 this
 * is what the composite is assembled from, which is the whole bit-identity
 * argument: not "the same maths", the same STRING. */
#define AFX_COMPOSITE_PLAIN \
    "  gl_FragColor = vec4(c * uExposure, 1.0);\n"

/* ------------------------------------------------------------- the targets */

typedef struct AfxTarget {
    unsigned fbo, tex;
    int w, h;
} AfxTarget;

/* T_LIT onward are the photorealism layer's, and are allocated ONLY when the
 * effects that need them are live -- a target with fbo == 0 is skipped by
 * afx_resize, so B3_PHOTO=0 costs no video memory as well as no passes. */
enum { T_SCENE, T_UI, T_HALF, T_QUARTER, T_BLUR, T_BLOOM_A, T_BLOOM_B,
       T_LIT, T_AO, T_AO_B, T_SSR, T_GOD_A, T_GOD_B,
       T_COUNT };

static AfxTarget g_t[T_COUNT];
static unsigned  g_depth_rb;
static unsigned  g_ms_fbo, g_ms_color_rb, g_ms_depth_rb;
static int       g_samples;         /* 0 = no MSAA path */
static const char *g_ms_why;        /* why not, when MSAA was asked for */
static int       g_ms_checked;      /* the first resolve's glGetError is read */
static int       g_ms_banned;       /* ...and if it failed, never re-armed */

/* A REBUILD THE SIZE DID NOT ASK FOR.
 *
 * afx_resize() early-outs on an unchanged size, which is right for a resize
 * and wrong for a SAMPLE-COUNT change: the sample count is not a size, and the
 * multisampled pair is rebuilt inside the very function that just decided
 * nothing had changed.  This flag is how the settings menu says "rebuild
 * anyway", and it goes through afx_resize_or_retire() exactly as a real resize
 * does -- which is the point, because that path is the hardened one
 * (tools/web_resize_sweep.py gates it) and a second rebuild path would be a
 * second thing to get wrong. */
static int g_force_rebuild;

/* WHY it was banned.  There are two ways to lose MSAA for a run now -- a
 * rejected resolve blit and a rebuild that would not complete with it on --
 * and reporting one as the other sends the next reader to the wrong code. */
static const char *g_ms_ban_why;
static unsigned  g_vbo;
static unsigned  g_ramp_tex;
static int       g_w, g_h;
static int       g_active;          /* 1 between frame_begin and frame_end   */
static int       g_ready = -1;      /* -1 untried, 0 unavailable, 1 built    */
static int       g_depth_bits;      /* 24 or 16 -- whichever the driver took  */
static int       g_taps = B3_AFX_TAPS;   /* B3_AFX_TAPS overrides            */

/* NAMED, because the photorealism layer's helpers take one by pointer.  It
 * used to be an anonymous struct declared inline with its twelve instances,
 * which was fine while every user was a call site that named the instance. */
typedef struct AfxProg {
    unsigned prog;
    int uTex, uTexel, uMask, uThreshold, uDir;
    int uScene, uBlur, uBloom, uAmt, uExposure, uRamp;
    int aPos;
    /* the photorealism layer's; -1 whenever the assembled source did not
     * declare them, which is the "an off effect uploads nothing" contract */
    int uDepth, uInvVP, uVP, uCam, uNF;
    int uAO, uAOAmt, uAOTex, uAOLit;
    int uShadow, uShVP, uSh, uSh2, uShCool, uSunDir, uSunCol;
    int uRtNode, uRtTri, uRtN, uRtT, uRt, uRt2;
    int uRtCar, uRtC, uCarS, uCarX, uCarY, uCarZ, uCarN;
    int uSunD, uSunN;
    int uSSRTex, uSsrAmt, uSsr, uMaskTex;
    int uAtm, uAtm2, uAtmSky, uAtmSkyC, uAtmSkyS, uAtmSkyC2, uAtmSkyS2;
    int uLightP, uLightC, uLightD, uLightK, uHeadK;
    int uSun, uGod, uGodTex, uPhoto, uLut;
} AfxProg;

static AfxProg g_p_down, g_p_radial, g_p_bright, g_p_blur, g_p_composite,
               g_p_gamma, g_p_ao, g_p_aoblur, g_p_ssr, g_p_godsrc, g_p_god,
               g_p_light;

/* ==========================================================================
 * THE PHOTOREALISM LAYER'S STATE
 * ========================================================================== */

/* What the caller publishes, once a frame. */
static B3PhotoCamera g_pcam;
static int   g_pcam_frame;          /* set by the setter, cleared on use     */
static float g_sun_dir[3] = { 0.0f, 1.0f, 0.0f };
static float g_sun_rgb[3] = { 1.0f, 1.0f, 1.0f };
static int   g_sun_have;
static unsigned g_shadow_tex;
static float g_shadow_vp[16];
static int   g_shadow_size;
/* tier 7: what the caller picked this frame.  32 is B3_PHOTO_LIGHT_N's own
 * ceiling, so the arrays are sized to the knob's range rather than to a
 * second number that could drift out of step with it. */
#define AFX_LIGHT_MAX 32
static float g_light_p[AFX_LIGHT_MAX * 4];
static float g_light_c[AFX_LIGHT_MAX * 4];
static float g_light_d[AFX_LIGHT_MAX * 4];
static float g_light_dusk;

/* What this module owns. */
static unsigned g_depth_tex;        /* the scene's depth, as a TEXTURE       */
static int      g_depth_is_tex;     /* ...when the driver would give us one  */
static unsigned g_lut_tex;          /* the generated grade cube, 256x16      */
static int      g_photo_live[B3_PHOTO_FX_COUNT];  /* after the GL reality check */
static int      g_photo_depth_ok = -1;
/* B3_PHOTO_DEBUG=ao puts the raw occlusion buffer on the screen; see the note
 * at its one use site.  A diagnostic, not a feature -- it bypasses the whole
 * deferred pass, so the frame it produces is not a game frame. */
static int      g_ao_debug;
/* Did the shine mask render this frame?  Not a live flag -- see the long note
 * at its one clear site: an effect whose block is already compiled into the
 * deferred shader stands down by uploading a ZERO, never by having its bind
 * and its upload skipped. */
static int      g_ssr_ok = 1;
static char     g_photo_line[352];

/* ---- tier 4r: the ray-traced sun shadow's GL state --------------------
 *
 * `g_rt_built` is what the DEFERRED SHADER was assembled with, not what the
 * user wants: the two differ for exactly one frame after the pause menu's
 * toggle, and afx_rt_sync() closes the gap by REBUILDING the program.  That
 * is the honest way to switch an assembled shader -- an `if (uRtOn)` around
 * the traversal would leave every off frame paying the register pressure of
 * a walk it never takes, and the whole reason this file assembles rather
 * than branches is that a frame must not pay for an effect it is not
 * running.  A rebuild is a few milliseconds, once, when a human presses a
 * key. */
static unsigned g_rt_node_tex, g_rt_tri_tex;
static int      g_rt_built;         /* the shader HAS the traversal in it  */
static int      g_rt_ok;            /* ...and the textures are uploaded    */
static const char *g_rt_why;        /* why not, when it was asked for      */

/* ---- tier 4rc: the CARS' state ---------------------------------------
 *
 * `g_rtc_n` is the shader array's LENGTH and is a property of the B3_RT_CARS
 * mode alone; `g_rtc_live` is how many slots this frame filled.  Keeping them
 * apart is the same discipline b3_photo_head_slots() documents: the length is
 * the SHADER'S, and letting the frame's contents change it makes two legs of a
 * measurement compile different programs. */
#define AFX_RTC_MAX 24
/* A CAR TREE'S OWN TRAVERSAL BUDGET.  The world's is 256 because a shadow ray
 * crossing a city meets a lot of boxes before it meets a blocker; a car is
 * about two thousand triangles inside a box four metres long, and no model in
 * the fleet builds deeper than 16 (tools/validate_car_bvh.py reports it).  A
 * stackless walk visits at most every node once, so 64 is more than the whole
 * tree for anything the fleet contains -- and the same "a budget that binds
 * DELETES shadows silently" warning the world's carries applies here, which is
 * why this is generous rather than tuned to the average. */
#define AFX_RTC_STEPS 64
static unsigned g_rtc_tex;
static int      g_rtc_built;        /* the shader HAS the car walk in it   */
static int      g_rtc_ok;           /* ...and the texture is uploaded      */
static int      g_rtc_n;            /* the array's length                  */
static int      g_rtc_live;         /* slots filled this frame             */
static unsigned g_rtc_uploaded_gen;
static float    g_rtc_s[AFX_RTC_MAX * 4];
static float    g_rtc_x[AFX_RTC_MAX * 4];
static float    g_rtc_y[AFX_RTC_MAX * 4];
static float    g_rtc_z[AFX_RTC_MAX * 4];
static float    g_rtc_node[AFX_RTC_MAX * 4];
/* which RACER slots went in, so the blob pass can ask.  A bitmask rather than
 * a scan: the blob pass asks once per car and the answer must not depend on
 * the order the instances were added in. */
static unsigned g_rtc_traced;

/* The runtime knobs.  EVERY constant in the header is overridable by an env of
 * the same name, and they are gathered in this one function so "what can I
 * turn" has a single answer.  Read once, latched, like the switches. */
static struct {
    float tm_shoulder, tm_exposure;
    float ao_radius, ao_strength, ao_bias, ao_power, ao_maxdist, ao_litbias;
    float ao_gain, ao_srcap, ao_angbias;
    float atm_density, atm_height, atm_strength, atm_sungain, atm_sunpow;
    float atm_skylean, atm_near;
    float sh_extent, sh_ahead, sh_depth, sh_strength, sh_bias, sh_slope;
    float sh_fade, sh_litbias, sh_cool, sh_normoff;
    float sun_direct, sun_ref, sun_lo, sun_hi, sun_nconf, sun_nband;
    float ssr_stride, ssr_thick, ssr_strength, ssr_edge, ssr_fresnel;
    float gr_density, gr_decay, gr_weight, gr_strength, gr_margin, gr_radius;
    float li_gain, li_day, li_dusk, li_elev, li_elev_w, li_warm, li_warm_w;
    float li_far, li_ahead, li_litbias;
    float hd_gain, hd_day, hd_wrap, hd_lit, hd_hot;
    int   sh_size, sh_pcf, ao_taps, ssr_steps, gr_taps, li_n;
} g_pk;
static int g_pk_read;

static void afx_photo_read_knobs(void)
{
    const char *e;
    if (g_pk_read) return;
    g_pk_read = 1;
    g_ao_debug = ((e = getenv("B3_PHOTO_DEBUG")) && *e
                  && strcmp(e, "ao") == 0);
#define PKF(field, macro, env) do { \
        g_pk.field = (macro); \
        if ((e = getenv(env)) && *e) g_pk.field = (float)atof(e); \
    } while (0)
#define PKI(field, macro, env, lo, hi) do { \
        g_pk.field = (macro); \
        if ((e = getenv(env)) && *e) g_pk.field = atoi(e); \
        if (g_pk.field < (lo)) g_pk.field = (lo); \
        if (g_pk.field > (hi)) g_pk.field = (hi); \
    } while (0)
    PKF(tm_shoulder,  B3_PHOTO_TM_SHOULDER,  "B3_PHOTO_TM_SHOULDER");
    PKF(tm_exposure,  B3_PHOTO_TM_EXPOSURE,  "B3_PHOTO_TM_EXPOSURE");
    PKF(ao_radius,    B3_PHOTO_AO_RADIUS,    "B3_PHOTO_AO_RADIUS");
    PKF(ao_strength,  B3_PHOTO_AO_STRENGTH,  "B3_PHOTO_AO_STRENGTH");
    PKF(ao_bias,      B3_PHOTO_AO_BIAS,      "B3_PHOTO_AO_BIAS");
    PKF(ao_power,     B3_PHOTO_AO_POWER,     "B3_PHOTO_AO_POWER");
    PKF(ao_maxdist,   B3_PHOTO_AO_MAXDIST,   "B3_PHOTO_AO_MAXDIST");
    PKF(ao_gain,      B3_PHOTO_AO_GAIN,      "B3_PHOTO_AO_GAIN");
    PKF(ao_srcap,     B3_PHOTO_AO_SRCAP,     "B3_PHOTO_AO_SRCAP");
    PKF(ao_angbias,   B3_PHOTO_AO_ANGBIAS,   "B3_PHOTO_AO_ANGBIAS");
    PKF(ao_litbias,   B3_PHOTO_AO_LITBIAS,   "B3_PHOTO_AO_LITBIAS");
    PKF(atm_density,  B3_PHOTO_ATM_DENSITY,  "B3_PHOTO_ATM_DENSITY");
    PKF(atm_height,   B3_PHOTO_ATM_HEIGHT,   "B3_PHOTO_ATM_HEIGHT");
    PKF(atm_strength, B3_PHOTO_ATM_STRENGTH, "B3_PHOTO_ATM_STRENGTH");
    PKF(atm_sungain,  B3_PHOTO_ATM_SUNGAIN,  "B3_PHOTO_ATM_SUNGAIN");
    PKF(atm_sunpow,   B3_PHOTO_ATM_SUNPOW,   "B3_PHOTO_ATM_SUNPOW");
    PKF(atm_skylean,  B3_PHOTO_ATM_SKYLEAN,  "B3_PHOTO_ATM_SKYLEAN");
    PKF(atm_near,     B3_PHOTO_ATM_NEAR,     "B3_PHOTO_ATM_NEAR");
    PKF(sh_extent,    B3_PHOTO_SH_EXTENT,    "B3_PHOTO_SH_EXTENT");
    PKF(sh_ahead,     B3_PHOTO_SH_AHEAD,     "B3_PHOTO_SH_AHEAD");
    PKF(sh_depth,     B3_PHOTO_SH_DEPTH,     "B3_PHOTO_SH_DEPTH");
    PKF(sh_strength,  B3_PHOTO_SH_STRENGTH,  "B3_PHOTO_SH_STRENGTH");
    PKF(sh_bias,      B3_PHOTO_SH_BIAS,      "B3_PHOTO_SH_BIAS");
    PKF(sh_slope,     B3_PHOTO_SH_SLOPE,     "B3_PHOTO_SH_SLOPE");
    PKF(sh_fade,      B3_PHOTO_SH_FADE,      "B3_PHOTO_SH_FADE");
    PKF(sh_litbias,   B3_PHOTO_SH_LITBIAS,   "B3_PHOTO_SH_LITBIAS");
    PKF(sh_cool,      B3_PHOTO_SH_COOL,      "B3_PHOTO_SH_COOL");
    PKF(sh_normoff,   B3_PHOTO_SH_NORMOFF,   "B3_PHOTO_SH_NORMOFF");
    PKF(sun_direct,   B3_PHOTO_SUN_DIRECT,   "B3_PHOTO_SUN_DIRECT");
    PKF(sun_ref,      B3_PHOTO_SUN_REF,      "B3_PHOTO_SUN_REF");
    PKF(sun_lo,       B3_PHOTO_SUN_LO,       "B3_PHOTO_SUN_LO");
    PKF(sun_hi,       B3_PHOTO_SUN_HI,       "B3_PHOTO_SUN_HI");
    PKF(sun_nconf,    B3_PHOTO_SUN_NCONF,    "B3_PHOTO_SUN_NCONF");
    PKF(sun_nband,    B3_PHOTO_SUN_NBAND,    "B3_PHOTO_SUN_NBAND");
    PKF(ssr_stride,   B3_PHOTO_SSR_STRIDE,   "B3_PHOTO_SSR_STRIDE");
    PKF(ssr_thick,    B3_PHOTO_SSR_THICK,    "B3_PHOTO_SSR_THICK");
    PKF(ssr_strength, B3_PHOTO_SSR_STRENGTH, "B3_PHOTO_SSR_STRENGTH");
    PKF(ssr_edge,     B3_PHOTO_SSR_EDGE,     "B3_PHOTO_SSR_EDGE");
    PKF(ssr_fresnel,  B3_PHOTO_SSR_FRESNEL,  "B3_PHOTO_SSR_FRESNEL");
    PKF(gr_density,   B3_PHOTO_GR_DENSITY,   "B3_PHOTO_GR_DENSITY");
    PKF(gr_decay,     B3_PHOTO_GR_DECAY,     "B3_PHOTO_GR_DECAY");
    PKF(gr_weight,    B3_PHOTO_GR_WEIGHT,    "B3_PHOTO_GR_WEIGHT");
    PKF(gr_strength,  B3_PHOTO_GR_STRENGTH,  "B3_PHOTO_GR_STRENGTH");
    PKF(gr_margin,    B3_PHOTO_GR_MARGIN,    "B3_PHOTO_GR_MARGIN");
    PKF(gr_radius,    B3_PHOTO_GR_RADIUS,    "B3_PHOTO_GR_RADIUS");
    PKI(sh_size,      B3_PHOTO_SH_SIZE,  "B3_PHOTO_SH_SIZE",  256, 4096);
    PKI(sh_pcf,       B3_PHOTO_SH_PCF,   "B3_PHOTO_SH_PCF",     0, 3);
    PKI(ao_taps,      B3_PHOTO_AO_TAPS,  "B3_PHOTO_AO_TAPS",    4, 32);
    PKI(ssr_steps,    B3_PHOTO_SSR_STEPS,"B3_PHOTO_SSR_STEPS",  4, 64);
    PKI(gr_taps,      B3_PHOTO_GR_TAPS,  "B3_PHOTO_GR_TAPS",    4, 64);
    PKF(li_gain,      B3_PHOTO_LIGHT_GAIN,     "B3_PHOTO_LIGHT_GAIN");
    PKF(li_day,       B3_PHOTO_LIGHT_DAY,      "B3_PHOTO_LIGHT_DAY");
    PKF(li_dusk,      B3_PHOTO_LIGHT_DUSK,     "B3_PHOTO_LIGHT_DUSK");
    PKF(li_elev,      B3_PHOTO_LIGHT_ELEV,     "B3_PHOTO_LIGHT_ELEV");
    PKF(li_elev_w,    B3_PHOTO_LIGHT_ELEV_W,   "B3_PHOTO_LIGHT_ELEV_W");
    PKF(li_warm,      B3_PHOTO_LIGHT_WARM,     "B3_PHOTO_LIGHT_WARM");
    PKF(li_warm_w,    B3_PHOTO_LIGHT_WARM_W,   "B3_PHOTO_LIGHT_WARM_W");
    PKF(li_far,       B3_PHOTO_LIGHT_FAR,      "B3_PHOTO_LIGHT_FAR");
    PKF(li_ahead,     B3_PHOTO_LIGHT_AHEAD,    "B3_PHOTO_LIGHT_AHEAD");
    PKF(li_litbias,   B3_PHOTO_LIGHT_LITBIAS,  "B3_PHOTO_LIGHT_LITBIAS");
    PKF(hd_gain,      B3_PHOTO_HEAD_GAIN,      "B3_PHOTO_HEAD_GAIN");
    PKF(hd_day,       B3_PHOTO_HEAD_DAY,       "B3_PHOTO_HEAD_DAY");
    PKF(hd_wrap,      B3_PHOTO_HEAD_WRAP,      "B3_PHOTO_HEAD_WRAP");
    /* ...AND THE MENU'S ROW ON TOP OF THE HEADER DEFAULT, BUT UNDER THE ENV.
     * b3_afx_head_env_forced() is 1 exactly when $B3_PHOTO_HEAD_WRAP or
     * $B3_PHOTO_HEAD_ON is set, which is the case PKF has already handled --
     * so this reads "the file may move a knob the operator has not pinned",
     * which is the same rule the other two settings rows follow. */
    if (!b3_afx_head_env_forced()) g_pk.hd_wrap = b3_afx_head_wrap();
    PKF(hd_lit,       B3_PHOTO_HEAD_LIT,       "B3_PHOTO_HEAD_LIT");
    PKF(hd_hot,       B3_PHOTO_HEAD_HOT,       "B3_PHOTO_HEAD_HOT");
    /* the budget has ONE answer, and it is the one the caller sorts to:
     * b3_photo_light_budget() is GL-free and public, so the shader's array
     * length and burnout3_full.c's pick cannot drift apart */
    g_pk.li_n = b3_photo_light_budget();
#undef PKF
#undef PKI
}


/* Which effects want a readable DEPTH TEXTURE.  Tier 1 is the only one that
 * does not -- it is a curve over a colour -- which is why the tonemap survives
 * on a context that cannot give us a depth texture at all. */
static int afx_photo_wants_depth(void)
{
    return b3_photo_fx(B3_PHOTO_FX_SSAO)   || b3_photo_fx(B3_PHOTO_FX_ATMOS)
        || b3_photo_fx(B3_PHOTO_FX_SHADOW) || b3_photo_fx(B3_PHOTO_FX_SSR)
        || b3_photo_fx(B3_PHOTO_FX_GODRAY) || b3_photo_fx(B3_PHOTO_FX_LIGHTS);
}

/* Is there a DEFERRED pass to run at all?  Tiers 2/3/4/5 land in it; tier 6
 * lands in the composite and tier 1 IS the composite's ending. */
static int afx_photo_wants_light(void)
{
    return g_photo_live[B3_PHOTO_FX_SSAO]  || g_photo_live[B3_PHOTO_FX_ATMOS]
        || g_photo_live[B3_PHOTO_FX_SHADOW]|| g_photo_live[B3_PHOTO_FX_SSR]
        || g_photo_live[B3_PHOTO_FX_LIGHTS];
}

void b3_photo_set_camera(const B3PhotoCamera *cam)
{
    if (!cam) { g_pcam_frame = 0; return; }
    g_pcam = *cam;
    g_pcam_frame = 1;
}

void b3_photo_set_sun(const float dir_toward[3], const float rgb[3], int have)
{
    int i;
    if (dir_toward) for (i = 0; i < 3; i++) g_sun_dir[i] = dir_toward[i];
    if (rgb)        for (i = 0; i < 3; i++) g_sun_rgb[i] = rgb[i];
    g_sun_have = have;
}

void b3_photo_set_shadow(unsigned tex, const float light_vp[16], int size)
{
    g_shadow_tex  = tex;
    g_shadow_size = size;
    if (light_vp) memcpy(g_shadow_vp, light_vp, sizeof g_shadow_vp);
}

void b3_photo_set_lights(const float *pos4, const float *col4,
                         const float *dir4, int n, float dusk)
{
    if (n < 0) n = 0;
    if (n > AFX_LIGHT_MAX) n = AFX_LIGHT_MAX;
    if (n && pos4 && col4) {
        memcpy(g_light_p, pos4, (size_t)n * 4 * sizeof(float));
        memcpy(g_light_c, col4, (size_t)n * 4 * sizeof(float));
        if (dir4) memcpy(g_light_d, dir4, (size_t)n * 4 * sizeof(float));
        else      memset(g_light_d, 0, (size_t)n * 4 * sizeof(float));
    } else {
        n = 0;
    }
    /* THE TAIL IS ZEROED rather than left stale.  The shader's loop bound is
     * the BUDGET, not this count -- an ESSL 1.00 loop cannot take a uniform
     * bound -- so every slot is evaluated every frame and a slot holding last
     * frame's light would be a lamp that stayed lit after the street it was on
     * went out of range.  A zeroed slot has 1/radius^2 == 0, so its windowed
     * falloff is `1 - d2 * 0` clamped, which is 1... hence the COLOUR is what
     * has to be zero, and it is. */
    memset(g_light_p + (size_t)n * 4, 0,
           (size_t)(AFX_LIGHT_MAX - n) * 4 * sizeof(float));
    memset(g_light_c + (size_t)n * 4, 0,
           (size_t)(AFX_LIGHT_MAX - n) * 4 * sizeof(float));
    memset(g_light_d + (size_t)n * 4, 0,
           (size_t)(AFX_LIGHT_MAX - n) * 4 * sizeof(float));
    g_light_dusk = dusk;
}

const char *b3_photo_status(void) { return g_photo_line; }

/* THE AUTHORED GRADE, generated rather than shipped.
 *
 * A 16x16x16 cube laid out as one 256x16 RGBA image: blue picks the 16-wide
 * slice along x, red picks the texel inside it, green picks the row.  The
 * transform each cell gets is, in order: a filmic S-curve about 0.5 for
 * contrast, a saturation about the SAME luma weights the composite's crash
 * desaturation uses, and then a lift/gain split-tone -- lift acting at black,
 * gain at white, so the two tints do not fight in the midtones.
 *
 * It is written out here rather than fetched because a derived asset that can
 * be regenerated from six numbers in a header is not an asset problem; and a
 * LUT nobody can rebuild is exactly the kind of thing this project refuses to
 * take on trust. */
static void afx_photo_lut_build(void)
{
    const int N = B3_PHOTO_LUT_N;
    unsigned char *px;
    int r, g, b, i;
    const char *e;
    /* THE GRADE IS TUNABLE FROM THE OUTSIDE, like every other constant in this
     * wave -- a look is settled by looking, and a look nobody can turn without
     * a recompile does not get settled.  The table is rebuilt from whatever
     * these come out as, so B3_PHOTO_GRADE_SAT=1.4 is a whole different grade
     * one run away. */
    float lift[3] = { B3_PHOTO_GRADE_LIFT_R, B3_PHOTO_GRADE_LIFT_G,
                      B3_PHOTO_GRADE_LIFT_B };
    float gain[3] = { B3_PHOTO_GRADE_GAIN_R, B3_PHOTO_GRADE_GAIN_G,
                      B3_PHOTO_GRADE_GAIN_B };
    float sat = B3_PHOTO_GRADE_SAT, con = B3_PHOTO_GRADE_CONTRAST;
    if ((e = getenv("B3_PHOTO_GRADE_LIFT_R")) && *e) lift[0] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_LIFT_G")) && *e) lift[1] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_LIFT_B")) && *e) lift[2] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_GAIN_R")) && *e) gain[0] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_GAIN_G")) && *e) gain[1] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_GAIN_B")) && *e) gain[2] = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_SAT"))      && *e) sat = (float)atof(e);
    if ((e = getenv("B3_PHOTO_GRADE_CONTRAST")) && *e) con = (float)atof(e);

    px = (unsigned char *)malloc((size_t)N * N * N * 4);
    if (!px) return;
    for (b = 0; b < N; b++) {
        for (g = 0; g < N; g++) {
            for (r = 0; r < N; r++) {
                float c[3];
                float lum;
                int x = b * N + r;              /* the slice, then within it */
                unsigned char *o = px + ((size_t)g * N * N + x) * 4;
                c[0] = (float)r / (float)(N - 1);
                c[1] = (float)g / (float)(N - 1);
                c[2] = (float)b / (float)(N - 1);
                /* 1. contrast, as a smooth S about 0.5 */
                for (i = 0; i < 3; i++) {
                    float v = c[i] - 0.5f;
                    c[i] = 0.5f + v * con
                                / (1.0f + fabsf(v) * 0.45f
                                   * (con - 1.0f) * 4.0f);
                }
                /* 2. saturation, about the composite's own luma weights */
                lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                for (i = 0; i < 3; i++)
                    c[i] = lum + (c[i] - lum) * sat;
                /* 3. the split tone: lift at black, gain at white */
                for (i = 0; i < 3; i++) {
                    float v = c[i];
                    if (v < 0.0f) v = 0.0f;
                    if (v > 1.0f) v = 1.0f;
                    c[i] = (v * gain[i] + lift[i] * (1.0f - v));
                    if (c[i] < 0.0f) c[i] = 0.0f;
                    if (c[i] > 1.0f) c[i] = 1.0f;
                }
                o[0] = (unsigned char)(c[0] * 255.0f + 0.5f);
                o[1] = (unsigned char)(c[1] * 255.0f + 0.5f);
                o[2] = (unsigned char)(c[2] * 255.0f + 0.5f);
                o[3] = 255;
            }
        }
    }
    glGenTextures(1, &g_lut_tex);
    glBindTexture(GL_TEXTURE_2D, g_lut_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, N * N, N, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
}

/* ------------------------------------------------------- tier 3's TARGET
 *
 * The dome's horizon band, reduced to a mean and TWO azimuthal harmonics:
 *
 *     sky(a) ~= m + c1*cos a + s1*sin a + c2*cos 2a + s2*sin 2a
 *     m = <b>,  c_h = 2<b cos ha>,  s_h = 2<b sin ha>
 *
 * which is the least-squares fit as well as the Fourier one, because the n
 * bins are uniform in `a`.  Five vec3s instead of a sampler: see the note
 * over AFX_LIGHT_U_ATMOS for why the eighth texture unit was not spent, and
 * note the shader needs no trig for any of it -- cos a and sin a ARE the
 * normalised vn.xz, and the second harmonic is the double-angle identity.
 *
 * WHY TWO AND NOT ONE.  The band has two independent azimuthal sources and
 * they sit on different harmonics.  The LUT's glow pass warms the sky on the
 * sun's side, which is a single lobe -- harmonic 1.  The cloud plate wraps
 * TWICE around the dome (the horizon ring's tc1.u is 2*u0), so every one of
 * its features lands on the EVEN harmonics and harmonic 1 cannot see it at
 * all: measured on US_M1 the first harmonic came out identically zero and the
 * fit was no better than the mean (mean residual 0.0441, peak 0.1104), while
 * adding the second cut it to 0.0135 / 0.0595.  A third and fourth buy 0.0119
 * and are not taken.
 *
 * B3_PHOTO_ATM_SKYSRC=0 keeps the compiled-in constant instead, which is also
 * exactly what a track with no gradient sheet gets -- four ship without one.
 * B3_PHOTO_ATM_SKYLO/_SKYHI sweep the elevation span the band is read over
 * (defaults B3_SKY_HORIZON_LO/HI; see the note over b3_sky_horizon_band).
 * B3_PHOTO_ATM_VERBOSE=1 prints the fitted band and the fit's own residual
 * once a second, which is how the harmonic count above was settled;
 * B3_PHOTO_ATM_DUMPBAND=1 adds the raw bins it was fitted to. */
static void afx_atm_sky_fit(float m[3], float c1[3], float s1[3],
                            float c2[3], float s2[3])
{
    static float band[B3_SKY_HORIZON_N * 3];
    static int   want = -1, verbose, dumpband;
    static float ylo, yhi;
    const int    n = B3_SKY_HORIZON_N;
    int          k, i;

    if (want < 0) {
        const char *e = getenv("B3_PHOTO_ATM_SKYSRC");
        want = (e && *e) ? (atoi(e) != 0) : 1;
        e = getenv("B3_PHOTO_ATM_SKYLO");
        ylo = (e && *e) ? (float)atof(e) : B3_SKY_HORIZON_LO;
        e = getenv("B3_PHOTO_ATM_SKYHI");
        yhi = (e && *e) ? (float)atof(e) : B3_SKY_HORIZON_HI;
        verbose  = getenv("B3_PHOTO_ATM_VERBOSE")  != NULL;
        dumpband = getenv("B3_PHOTO_ATM_DUMPBAND") != NULL;
    }
    m[0] = B3_PHOTO_ATM_SKY_R;
    m[1] = B3_PHOTO_ATM_SKY_G;
    m[2] = B3_PHOTO_ATM_SKY_B;
    for (i = 0; i < 3; i++) c1[i] = s1[i] = c2[i] = s2[i] = 0.0f;
    if (!want || !b3_sky_horizon_band(n, ylo, yhi, band)) return;

    for (i = 0; i < 3; i++) m[i] = 0.0f;
    for (k = 0; k < n; k++) {
        float a  = 6.2831853f * ((float)k + 0.5f) / (float)n;
        float ca = cosf(a), sa = sinf(a);
        float c2a = ca * ca - sa * sa, s2a = 2.0f * ca * sa;
        for (i = 0; i < 3; i++) {
            float b = band[k * 3 + i];
            m[i]  += b;
            c1[i] += b * ca;
            s1[i] += b * sa;
            c2[i] += b * c2a;
            s2[i] += b * s2a;
        }
    }
    for (i = 0; i < 3; i++) {
        m[i]  /= (float)n;
        c1[i] *= 2.0f / (float)n;
        s1[i] *= 2.0f / (float)n;
        c2[i] *= 2.0f / (float)n;
        s2[i] *= 2.0f / (float)n;
    }

    if (verbose) {
        static int tick;
        if ((tick++ % 60) == 0) {
            float err = 0.0f, peak = 0.0f;
            for (k = 0; k < n; k++) {
                float a  = 6.2831853f * ((float)k + 0.5f) / (float)n;
                float ca = cosf(a), sa = sinf(a);
                for (i = 0; i < 3; i++) {
                    float fit = m[i] + c1[i] * ca + s1[i] * sa
                              + c2[i] * (ca * ca - sa * sa)
                              + s2[i] * (2.0f * ca * sa);
                    float d = fabsf(fit - band[k * 3 + i]);
                    err += d;
                    if (d > peak) peak = d;
                }
            }
            printf("[photo] atm sky band mean %.4f %.4f %.4f  h1 %+.4f %+.4f "
                   "%+.4f / %+.4f %+.4f %+.4f  h2 %+.4f %+.4f %+.4f / %+.4f "
                   "%+.4f %+.4f  fit err mean %.4f peak %.4f\n",
                   m[0], m[1], m[2], c1[0], c1[1], c1[2], s1[0], s1[1], s1[2],
                   c2[0], c2[1], c2[2], s2[0], s2[1], s2[2],
                   err / (float)(n * 3), peak);
            if (dumpband) {
                printf("[photo] atm band:");
                for (k = 0; k < n; k++)
                    printf(" %.4f,%.4f,%.4f", band[k * 3], band[k * 3 + 1],
                           band[k * 3 + 2]);
                printf("\n");
            }
        }
    }
}

/* runtime knobs */
static float g_k_blur = 1.0f, g_k_bloom = 1.0f, g_k_crash = 1.0f;
static float g_k_exposure = B3_PRESENT_SHIFT;
static int   g_knobs;

/* THE PINNED-FRAME MEASUREMENT OVERRIDE.
 *
 * B3_AFX_FORCE_MPH / B3_AFX_FORCE_BOOST / B3_AFX_FORCE_DIVISOR replace the
 * three B3AfxInputs the caller hands in, and nothing else -- the simulation
 * is not touched, so the WORLD is bit-identical between two runs that differ
 * only in these.  That is the whole point: a speed sweep taken by driving
 * faster changes the camera, the traffic and the lighting as well as the
 * blur, and none of those frames are comparable.  With these, frame 400 of
 * every leg of the sweep is the same photograph with a different amount of
 * smear on it, so a pixel difference is the effect and only the effect.
 *
 * tools/afx_sweep.py drives them.  Off unless set. */
static float g_force_mph = -1.0f, g_force_boost = -1.0f;
static int   g_force_div = -1;

static void afx_read_knobs(void)
{
    const char *e;
    if (g_knobs) return;
    g_knobs = 1;
    if ((e = getenv("B3_AFX_BLUR"))     && *e) g_k_blur     = (float)atof(e);
    if ((e = getenv("B3_AFX_BLOOM"))    && *e) g_k_bloom    = (float)atof(e);
    if ((e = getenv("B3_AFX_CRASH"))    && *e) g_k_crash    = (float)atof(e);
    if ((e = getenv("B3_AFX_EXPOSURE")) && *e) g_k_exposure = (float)atof(e);
    if ((e = getenv("B3_AFX_FORCE_MPH"))   && *e) g_force_mph   = (float)atof(e);
    if ((e = getenv("B3_AFX_FORCE_BOOST")) && *e) g_force_boost = (float)atof(e);
    if ((e = getenv("B3_AFX_FORCE_DIVISOR")) && *e) g_force_div = atoi(e);
    if (g_force_mph >= 0.0f || g_force_boost >= 0.0f || g_force_div > 0)
        afx_say("MEASUREMENT OVERRIDE: mph %.1f boost %.2f divisor %d "
                "(-1 = take the game's own)\n",
                g_force_mph, g_force_boost, g_force_div);
}

static void afx_tex_alloc(unsigned tex, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    /* RGBA/UNSIGNED_BYTE is the one colour-renderable texture format WebGL 1
     * GUARANTEES for a colour attachment; it is equally fine everywhere else. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static int afx_target_make(AfxTarget *t, int w, int h, const char *what)
{
    unsigned st;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!t->fbo) p_glGenFramebuffers(1, &t->fbo);
    if (!t->tex) glGenTextures(1, &t->tex);
    afx_tex_alloc(t->tex, w, h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, t->tex, 0);
    t->w = w; t->h = h;
    st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        return afx_decline("target %s %dx%d incomplete (status 0x%04X)",
                           what, w, h, st);
    return 1;
}

/* The scene target needs depth. 24-bit if the driver takes it, 16-bit
 * otherwise -- WebGL 1 only guarantees DEPTH_COMPONENT16, and 16 bits of
 * depth over a 10 km draw distance is a real z-fighting risk, so the 24-bit
 * attempt comes first and its success is worth printing under B3_AFX_VERBOSE. */
/* THE DEPTH AS A TEXTURE, which is what the photorealism layer runs on.
 *
 * Five of the six effects read the scene's depth, and a RENDERBUFFER cannot be
 * sampled -- so when any of them is live the scene's depth attachment becomes a
 * texture instead.  Four format spellings are tried in order, because the same
 * capability is spelled differently on the four targets this port runs on:
 *
 *   DEPTH_COMPONENT24 + UNSIGNED_INT     desktop GL (ARB_depth_texture, core
 *                                        since 1.4) and WebGL 2 / GLES 3
 *   DEPTH_COMPONENT16 + UNSIGNED_SHORT   the same, at half the precision
 *   DEPTH_COMPONENT   + UNSIGNED_INT     WebGL 1 with WEBGL_depth_texture,
 *                                        whose internal format is UNSIZED
 *   DEPTH_COMPONENT   + UNSIGNED_SHORT   ...and the 16-bit spelling of it
 *
 * NEAREST filtering, always: a depth texture is not linearly filterable on ES
 * without a comparison sampler, and asking for LINEAR is an incomplete texture
 * rather than an error you can see.
 *
 * A DECLINE HERE IS NOT A CHAIN DECLINE.  If no spelling completes, the caller
 * puts the renderbuffer back and the five depth-reading effects stand down with
 * a printed reason; the tonemap and the rest of the chain carry on. */
static int afx_scene_depth_texture(int w, int h)
{
    static const unsigned ifmt[4] = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16,
                                      GL_DEPTH_COMPONENT,   GL_DEPTH_COMPONENT };
    static const unsigned type[4] = { GL_UNSIGNED_INT, GL_UNSIGNED_SHORT,
                                      GL_UNSIGNED_INT, GL_UNSIGNED_SHORT };
    static const int bits[4] = { 24, 16, 24, 16 };
    int i;
    if (!g_depth_tex) glGenTextures(1, &g_depth_tex);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
    /* the renderbuffer must not be attached as well, or the completeness check
     * below is answering a question about the wrong attachment */
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, 0);
    for (i = 0; i < 4; i++) {
        glBindTexture(GL_TEXTURE_2D, g_depth_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        (void)glGetError();
        glTexImage2D(GL_TEXTURE_2D, 0, (int)ifmt[i], w, h, 0,
                     GL_DEPTH_COMPONENT, type[i], NULL);
        if (glGetError()) continue;
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_TEXTURE_2D, g_depth_tex, 0);
        if (glGetError()) continue;
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER)
                == GL_FRAMEBUFFER_COMPLETE) {
            g_depth_bits   = bits[i];
            g_depth_is_tex = 1;
            return 1;
        }
    }
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_TEXTURE_2D, 0, 0);
    g_depth_is_tex = 0;
    return 0;
}

static int afx_scene_depth(int w, int h)
{
    static const unsigned fmts[2] = { GL_DEPTH_COMPONENT24,
                                      GL_DEPTH_COMPONENT16 };
    int i;

    /* THE TEXTURE FIRST, and only when something is going to read it.  With
     * B3_PHOTO=0 this branch is not taken at all and what follows is the
     * pre-wave function, unchanged -- which is the bit-identity contract
     * reaching all the way down to the attachment. */
    g_depth_is_tex = 0;
    if (g_photo_depth_ok != 0 && afx_photo_wants_depth()) {
        if (afx_scene_depth_texture(w, h)) {
            g_photo_depth_ok = 1;
            return 1;
        }
        if (g_photo_depth_ok < 0) {
            g_photo_depth_ok = 0;
            afx_say("PHOTO: this context will not give a sampleable depth "
                    "texture at %dx%d (tried 24/16-bit, sized and unsized) -- "
                    "SSAO, atmospherics, shadows, SSR and god rays stand "
                    "down; the tonemap and grade carry on\n", w, h);
        }
    }

    if (!g_depth_rb) p_glGenRenderbuffers(1, &g_depth_rb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
    for (i = 0; i < 2; i++) {
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_depth_rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, fmts[i], w, h);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                    GL_RENDERBUFFER, g_depth_rb);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER)
                == GL_FRAMEBUFFER_COMPLETE) {
            g_depth_bits = (i == 0) ? 24 : 16;
            return 1;
        }
    }
    return 0;
}

/* HOW MANY SAMPLES THE SCENE TARGET WANTS.  Pure env, no GL: see the long note
 * over B3_AFX_MSAA_DEF for why asking the driver was the bug this replaces.
 * The GL-side clamp (GL_MAX_SAMPLES, and the FBO completeness check inside
 * afx_ms_make) happens at the call site, where a context exists. */
static int afx_msaa_want(void)
{
    return b3_afx_msaa_want();
}

/* ------------------------------------------------- THE MSAA SETTING -----
 *
 * The pause menu's second row, and the same shape the first one has: an env
 * that wins outright, a file underneath it, and a row that GREYS rather than
 * showing a value the renderer is not using.
 *
 * IT SHARES build/settings.cfg WITH RAY TRACING, and the sharing works
 * because both writers preserve the keys they do not own -- which is exactly
 * why b3_rt_save() was written that way in the first place ("a settings file
 * that lost the caller's other entries every time one of them changed would
 * be a settings file nobody could add to").  This is the caller that block
 * was anticipating.
 *
 * THE VALUE IS NOT A BOOLEAN, so `set` takes the next value in a cycle rather
 * than a negation: OFF -> 2x -> 4x -> OFF.  The GL-side clamp is unchanged and
 * still lives at the call site (GL_MAX_SAMPLES plus afx_ms_make's own FBO
 * completeness check), because a menu has no context to ask. */
#define AFX_SET_CFG "build/settings.cfg"
static int g_msaa_want = -1;      /* -1 = not yet read                     */
static int g_msaa_forced;         /* an env decided it                     */

static void afx_msaa_read(void)
{
    FILE *f;
    const char *e;

    if (g_msaa_want >= 0) return;
    g_msaa_want = B3_AFX_MSAA_DEF;

    f = fopen(AFX_SET_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (fscanf(f, "%31s %f", k, &v) == 2)
            if (!strcmp(k, "msaa")) g_msaa_want = (int)v;
        fclose(f);
    }
    /* the env wins outright and says so, exactly as $B3_RT does: a harness
     * that pinned B3_MSAA must not be quietly overruled by whatever the last
     * person to open the menu left in the file */
    if (((e = getenv("B3_AFX_MSAA")) && *e) ||
        ((e = getenv("B3_MSAA")) && *e)) {
        g_msaa_want = atoi(e);
        g_msaa_forced = 1;
    }
    if (g_msaa_want < 0)  g_msaa_want = 0;
    if (g_msaa_want > 16) g_msaa_want = 16;
}

int b3_afx_msaa_want(void)
{
    afx_msaa_read();
    return g_msaa_want;
}

int b3_afx_msaa_env_forced(void)
{
    afx_msaa_read();
    return g_msaa_forced;
}

int b3_afx_msaa_live(void)
{
    return g_samples;
}

const char *b3_afx_msaa_why(void)
{
    return g_ms_why;
}

void b3_afx_msaa_set(int n)
{
    afx_msaa_read();
    if (n < 0)  n = 0;
    if (n > 16) n = 16;
    if (n == g_msaa_want) return;
    g_msaa_want = n;
    /* THE CHAIN REBUILDS ON THE NEXT FRAME, through the resize path.  It is
     * not rebuilt here: this is called from the pause overlay, which runs
     * between b3_afx_frame_begin() and b3_afx_frame_end() with the scene
     * target bound and half the chain's state live.  Tearing the targets down
     * underneath the frame that is drawing into them is the one way to make a
     * settings toggle a crash. */
    g_force_rebuild = 1;
}

void b3_afx_msaa_save(void)
{
    char  keys[16][32];
    float vals[16];
    int   n = 0, i;
    FILE *f;

    afx_msaa_read();
    f = fopen(AFX_SET_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (n < 16 && fscanf(f, "%31s %f", k, &v) == 2) {
            if (!strcmp(k, "msaa")) continue;
            snprintf(keys[n], sizeof keys[n], "%s", k);
            vals[n] = v;
            n++;
        }
        fclose(f);
    }
    f = fopen(AFX_SET_CFG, "w");
    if (!f) return;
    fprintf(f, "msaa %d\n", g_msaa_want);
    for (i = 0; i < n; i++) fprintf(f, "%s %.4f\n", keys[i], vals[i]);
    fclose(f);
}

/* ------------------------------------------------- THE HEADLIGHTS SETTING --
 *
 * The pause menu's third row, and the cheapest of the three: RAY TRACING
 * rebuilds an assembled shader and MSAA rebuilds the whole chain, while this
 * one moves a FLOAT IN A UNIFORM that is uploaded every frame anyway.  Nothing
 * is rebuilt, nothing is reallocated, and the change is visible on the next
 * frame.
 *
 * IT IS A WRAP AND NOT A GAIN, which is the whole finding behind the row --
 * see the table over B3_PHOTO_HEAD_WRAP.  Gain scales the tunnel wall and the
 * blown road by the same factor; wrap is a grazing-incidence term and moves
 * the road while leaving the wall alone.  "Brightness" as a player means it is
 * the road pool, so this is the knob a brightness row should be wired to.
 *
 * FOUR STOPS, OFF -> LOW -> MED -> HIGH, one more than the MSAA row because
 * the report that produced it was about TASTE: MED is the shipped default,
 * HIGH is the pre-playtest look to the digit, and LOW exists because a user
 * who says "too bright" once may say it twice and should not have to find an
 * environment variable to be heard the second time.
 *
 * $B3_PHOTO_HEAD_WRAP and $B3_PHOTO_HEAD_ON win outright and GREY the row, the
 * same rule $B3_RT and $B3_MSAA follow: a harness that pinned the beams must
 * not be quietly overruled by whatever the last person to open the menu left
 * in build/settings.cfg.  Every beam leg in tools/validate_photo.py sets
 * B3_PHOTO_HEAD_ON, so the suite is hermetic against this file by
 * construction. */
static int g_head_want = -1;      /* -1 = not yet read                     */
static int g_head_forced;

static void afx_head_read(void)
{
    FILE *f;
    const char *e;

    if (g_head_want >= 0) return;
    g_head_want = B3_AFX_HEAD_DEF;

    f = fopen(AFX_SET_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (fscanf(f, "%31s %f", k, &v) == 2)
            if (!strcmp(k, "headlights")) g_head_want = (int)v;
        fclose(f);
    }
    if ((((e = getenv("B3_PHOTO_HEAD_WRAP")) && *e))
        || (((e = getenv("B3_PHOTO_HEAD_ON")) && *e))) {
        g_head_forced = 1;
        /* the row still has to SHOW something true, so read the pin back into
         * the nearest stop rather than leaving it on the file's answer */
        if ((e = getenv("B3_PHOTO_HEAD_ON")) && *e && atoi(e) == 0)
            g_head_want = B3_AFX_HEAD_OFF;
        else if ((e = getenv("B3_PHOTO_HEAD_WRAP")) && *e) {
            float w = (float)atof(e);
            g_head_want = (w >= (B3_PHOTO_HEAD_WRAP_MED
                                 + B3_PHOTO_HEAD_WRAP_HIGH) * 0.5f)
                            ? B3_AFX_HEAD_HIGH
                        : (w >= (B3_PHOTO_HEAD_WRAP_LOW
                                 + B3_PHOTO_HEAD_WRAP_MED) * 0.5f)
                            ? B3_AFX_HEAD_MED
                            : B3_AFX_HEAD_LOW;
        }
    }
    if (g_head_want < 0) g_head_want = 0;
    if (g_head_want > B3_AFX_HEAD_HIGH) g_head_want = B3_AFX_HEAD_HIGH;
}

int b3_afx_head_want(void)       { afx_head_read(); return g_head_want; }
int b3_afx_head_env_forced(void) { afx_head_read(); return g_head_forced; }
int b3_afx_head_on(void)         { afx_head_read();
                                   return g_head_want != B3_AFX_HEAD_OFF; }

const char *b3_afx_head_name(void)
{
    static const char *const N[] = { "OFF", "LOW", "MED", "HIGH" };
    afx_head_read();
    return N[g_head_want & 3];
}

/* WHAT EACH STOP MEANS, and each of the three is an env of its own -- the
 * house rule this file's constants all follow, gated by validate_photo's
 * "every tunable constant is overridable by an env of the same name".  It is
 * not ceremony here either: a player who wants the row but not these three
 * numbers can re-map the stops without giving up the menu, which is the whole
 * point of a taste control. */
float b3_afx_head_wrap(void)
{
    const char *e;
    float v;
    afx_head_read();
    switch (g_head_want) {
    case B3_AFX_HEAD_LOW:
        v = B3_PHOTO_HEAD_WRAP_LOW;
        if ((e = getenv("B3_PHOTO_HEAD_WRAP_LOW")) && *e) v = (float)atof(e);
        return v;
    case B3_AFX_HEAD_HIGH:
        v = B3_PHOTO_HEAD_WRAP_HIGH;
        if ((e = getenv("B3_PHOTO_HEAD_WRAP_HIGH")) && *e) v = (float)atof(e);
        return v;
    default:
        v = B3_PHOTO_HEAD_WRAP_MED;
        if ((e = getenv("B3_PHOTO_HEAD_WRAP_MED")) && *e) v = (float)atof(e);
        return v;
    }
}

void b3_afx_head_set(int n)
{
    afx_head_read();
    if (n < 0) n = 0;
    if (n > B3_AFX_HEAD_HIGH) n = B3_AFX_HEAD_HIGH;
    if (n == g_head_want) return;
    g_head_want = n;
    /* THE KNOB CACHE IS THE ONLY THING THAT HAS TO BE TOLD.  g_pk is read once
     * per run, so a setting that only changed g_head_want would be a setting
     * that did nothing until the next process.  Dropping the flag re-reads
     * every knob from its env on the next frame, which is idempotent -- and it
     * is the whole cost of this row, against a chain rebuild for MSAA. */
    g_pk_read = 0;
}

void b3_afx_head_save(void)
{
    char  keys[16][32];
    float vals[16];
    int   n = 0, i;
    FILE *f;

    afx_head_read();
    f = fopen(AFX_SET_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (n < 16 && fscanf(f, "%31s %f", k, &v) == 2) {
            if (!strcmp(k, "headlights")) continue;
            snprintf(keys[n], sizeof keys[n], "%s", k);
            vals[n] = v;
            n++;
        }
        fclose(f);
    }
    f = fopen(AFX_SET_CFG, "w");
    if (!f) return;
    fprintf(f, "headlights %d\n", g_head_want);
    for (i = 0; i < n; i++) fprintf(f, "%s %.4f\n", keys[i], vals[i]);
    fclose(f);
}

/* MSAA. The scene is drawn into a MULTISAMPLED renderbuffer pair and resolved
 * into the scene texture with one GPU-side blit; everything downstream reads
 * the resolved TEXTURE, so no pass in this chain ever samples a multisample
 * surface. Needs glRenderbufferStorageMultisample + glBlitFramebuffer (GL 3.0 /
 * GLES 3 / WebGL 2); where they are absent the chain simply runs without MSAA,
 * which is what a WebGL 1 context still gets.
 *
 * THE COLOUR FORMATS HAVE TO MATCH, and this is the one place it is easy to get
 * wrong on the web. GLES 3 / WebGL 2 raise INVALID_OPERATION on a MULTISAMPLE
 * resolve blit whose read and draw attachments do not have identical internal
 * formats. The pair here is RGBA8 (this renderbuffer) against the scene
 * texture, allocated GL_RGBA/GL_UNSIGNED_BYTE -- which ES 3 table 3.2 gives the
 * effective internal format RGBA8, so they do match. That is a two-document
 * argument, so it is also CHECKED AT RUNTIME: the first resolve reads
 * glGetError() and switches MSAA off for the rest of the run if the blit was
 * rejected, rather than shipping a frame that is quietly the clear colour. */
static int afx_ms_make(int w, int h, int samples)
{
    if (!p_glRenderbufferStorageMultisample || !p_glBlitFramebuffer) return 0;
    if (samples < 2) return 0;
    if (!g_ms_fbo) p_glGenFramebuffers(1, &g_ms_fbo);
    if (!g_ms_color_rb) p_glGenRenderbuffers(1, &g_ms_color_rb);
    if (!g_ms_depth_rb) p_glGenRenderbuffers(1, &g_ms_depth_rb);

    p_glBindFramebuffer(GL_FRAMEBUFFER, g_ms_fbo);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_ms_color_rb);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8,
                                       w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_RENDERBUFFER, g_ms_color_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_ms_depth_rb);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                       GL_DEPTH_COMPONENT24, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g_ms_depth_rb);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        return 1;

    /* try 16-bit depth before giving MSAA up */
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_ms_depth_rb);
    p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                       GL_DEPTH_COMPONENT16, w, h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g_ms_depth_rb);
    return p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void afx_progs_locs(void)
{
    /* Every pass gets every location looked up; a program that does not
     * declare a given uniform simply reports -1 and the call sites skip it.
     * One table beats six bespoke ones. */
#define L(P) do { \
        (P).aPos       = p_glGetAttribLocation((P).prog, "aPos"); \
        (P).uTex       = p_glGetUniformLocation((P).prog, "uTex"); \
        (P).uTexel     = p_glGetUniformLocation((P).prog, "uTexel"); \
        (P).uMask      = p_glGetUniformLocation((P).prog, "uMask"); \
        (P).uThreshold = p_glGetUniformLocation((P).prog, "uThreshold"); \
        (P).uDir       = p_glGetUniformLocation((P).prog, "uDir"); \
        (P).uScene     = p_glGetUniformLocation((P).prog, "uScene"); \
        (P).uBlur      = p_glGetUniformLocation((P).prog, "uBlur"); \
        (P).uBloom     = p_glGetUniformLocation((P).prog, "uBloom"); \
        (P).uAmt       = p_glGetUniformLocation((P).prog, "uAmt"); \
        (P).uExposure  = p_glGetUniformLocation((P).prog, "uExposure"); \
        (P).uRamp      = p_glGetUniformLocation((P).prog, "uRamp"); \
    } while (0)
    /* The photorealism layer's, in a second table for the same reason the
     * first one exists: a program that did not declare a given uniform reports
     * -1 and every call site already skips on that.  An effect whose block was
     * not spliced into the shader therefore uploads nothing, automatically. */
#define LP(P) do { \
        (P).uDepth   = p_glGetUniformLocation((P).prog, "uDepth"); \
        (P).uInvVP   = p_glGetUniformLocation((P).prog, "uInvVP"); \
        (P).uVP      = p_glGetUniformLocation((P).prog, "uVP"); \
        (P).uCam     = p_glGetUniformLocation((P).prog, "uCam"); \
        (P).uNF      = p_glGetUniformLocation((P).prog, "uNF"); \
        (P).uAO      = p_glGetUniformLocation((P).prog, "uAO"); \
        (P).uAOAmt   = p_glGetUniformLocation((P).prog, "uAOAmt"); \
        (P).uAOTex   = p_glGetUniformLocation((P).prog, "uAOTex"); \
        (P).uAOLit   = p_glGetUniformLocation((P).prog, "uAOLit"); \
        (P).uShadow  = p_glGetUniformLocation((P).prog, "uShadow"); \
        (P).uShVP    = p_glGetUniformLocation((P).prog, "uShVP"); \
        (P).uSh      = p_glGetUniformLocation((P).prog, "uSh"); \
        (P).uSh2     = p_glGetUniformLocation((P).prog, "uSh2"); \
        (P).uShCool  = p_glGetUniformLocation((P).prog, "uShCool"); \
        (P).uSunD    = p_glGetUniformLocation((P).prog, "uSunD"); \
        (P).uSunN    = p_glGetUniformLocation((P).prog, "uSunN"); \
        (P).uSunDir  = p_glGetUniformLocation((P).prog, "uSunDir"); \
        (P).uSunCol  = p_glGetUniformLocation((P).prog, "uSunCol"); \
        (P).uRtNode  = p_glGetUniformLocation((P).prog, "uRtNode"); \
        (P).uRtTri   = p_glGetUniformLocation((P).prog, "uRtTri"); \
        (P).uRtN     = p_glGetUniformLocation((P).prog, "uRtN"); \
        (P).uRtT     = p_glGetUniformLocation((P).prog, "uRtT"); \
        (P).uRt      = p_glGetUniformLocation((P).prog, "uRt"); \
        (P).uRt2     = p_glGetUniformLocation((P).prog, "uRt2"); \
        (P).uRtCar   = p_glGetUniformLocation((P).prog, "uRtCar"); \
        (P).uRtC     = p_glGetUniformLocation((P).prog, "uRtC"); \
        (P).uCarS    = p_glGetUniformLocation((P).prog, "uCarS"); \
        (P).uCarX    = p_glGetUniformLocation((P).prog, "uCarX"); \
        (P).uCarY    = p_glGetUniformLocation((P).prog, "uCarY"); \
        (P).uCarZ    = p_glGetUniformLocation((P).prog, "uCarZ"); \
        (P).uCarN    = p_glGetUniformLocation((P).prog, "uCarN"); \
        (P).uSSRTex  = p_glGetUniformLocation((P).prog, "uSSRTex"); \
        (P).uSsrAmt  = p_glGetUniformLocation((P).prog, "uSsrAmt"); \
        (P).uSsr     = p_glGetUniformLocation((P).prog, "uSsr"); \
        (P).uMaskTex = p_glGetUniformLocation((P).prog, "uMaskTex"); \
        (P).uAtm     = p_glGetUniformLocation((P).prog, "uAtm"); \
        (P).uAtm2    = p_glGetUniformLocation((P).prog, "uAtm2"); \
        (P).uAtmSky  = p_glGetUniformLocation((P).prog, "uAtmSky"); \
        (P).uAtmSkyC = p_glGetUniformLocation((P).prog, "uAtmSkyC"); \
        (P).uAtmSkyS = p_glGetUniformLocation((P).prog, "uAtmSkyS"); \
        (P).uAtmSkyC2 = p_glGetUniformLocation((P).prog, "uAtmSkyC2"); \
        (P).uAtmSkyS2 = p_glGetUniformLocation((P).prog, "uAtmSkyS2"); \
        (P).uLightP  = p_glGetUniformLocation((P).prog, "uLightP[0]"); \
        (P).uLightC  = p_glGetUniformLocation((P).prog, "uLightC[0]"); \
        (P).uLightD  = p_glGetUniformLocation((P).prog, "uLightD[0]"); \
        (P).uLightK  = p_glGetUniformLocation((P).prog, "uLightK"); \
        (P).uHeadK   = p_glGetUniformLocation((P).prog, "uHeadK"); \
        (P).uSun     = p_glGetUniformLocation((P).prog, "uSun"); \
        (P).uGod     = p_glGetUniformLocation((P).prog, "uGod"); \
        (P).uGodTex  = p_glGetUniformLocation((P).prog, "uGodTex"); \
        (P).uPhoto   = p_glGetUniformLocation((P).prog, "uPhoto"); \
        (P).uLut     = p_glGetUniformLocation((P).prog, "uLut"); \
    } while (0)
    L(g_p_down);   L(g_p_radial);    L(g_p_bright);
    L(g_p_blur);   L(g_p_composite); L(g_p_gamma);
    if (g_p_ao.prog)     { L(g_p_ao);     LP(g_p_ao);     }
    if (g_p_aoblur.prog) { L(g_p_aoblur); LP(g_p_aoblur); }
    if (g_p_ssr.prog)    { L(g_p_ssr);    LP(g_p_ssr);    }
    if (g_p_godsrc.prog) { L(g_p_godsrc); LP(g_p_godsrc); }
    if (g_p_god.prog)    { L(g_p_god);    LP(g_p_god);    }
    if (g_p_light.prog)  { L(g_p_light);  LP(g_p_light);  }
    LP(g_p_composite);
#undef L
#undef LP

    /* sampler units, set once */
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_composite.prog);
    if (g_p_composite.uScene >= 0) p_glUniform1i(g_p_composite.uScene, 0);
    if (g_p_composite.uBlur  >= 0) p_glUniform1i(g_p_composite.uBlur,  1);
    if (g_p_composite.uBloom >= 0) p_glUniform1i(g_p_composite.uBloom, 2);
    if (g_p_composite.uGodTex >= 0) p_glUniform1i(g_p_composite.uGodTex, 3);
    if (g_p_composite.uLut    >= 0) p_glUniform1i(g_p_composite.uLut,    4);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_gamma.prog);
    if (g_p_gamma.uTex  >= 0) p_glUniform1i(g_p_gamma.uTex,  0);
    if (g_p_gamma.uRamp >= 0) p_glUniform1i(g_p_gamma.uRamp, 1);
    /* The photorealism passes' sampler units, in the one place they are
     * decided: unit 0 is always "the colour this pass reads", unit 1 is always
     * the depth, and the effect-specific inputs start at 2. */
    if (g_p_ao.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_ao.prog);
        if (g_p_ao.uDepth >= 0) p_glUniform1i(g_p_ao.uDepth, 1);
    }
    if (g_p_aoblur.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_aoblur.prog);
        if (g_p_aoblur.uTex   >= 0) p_glUniform1i(g_p_aoblur.uTex,   0);
        if (g_p_aoblur.uDepth >= 0) p_glUniform1i(g_p_aoblur.uDepth, 1);
    }
    if (g_p_ssr.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_ssr.prog);
        if (g_p_ssr.uTex     >= 0) p_glUniform1i(g_p_ssr.uTex,     0);
        if (g_p_ssr.uDepth   >= 0) p_glUniform1i(g_p_ssr.uDepth,   1);
        if (g_p_ssr.uMaskTex >= 0) p_glUniform1i(g_p_ssr.uMaskTex, 2);
    }
    if (g_p_godsrc.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_godsrc.prog);
        if (g_p_godsrc.uTex   >= 0) p_glUniform1i(g_p_godsrc.uTex,   0);
        if (g_p_godsrc.uDepth >= 0) p_glUniform1i(g_p_godsrc.uDepth, 1);
    }
    if (g_p_god.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_god.prog);
        if (g_p_god.uTex >= 0) p_glUniform1i(g_p_god.uTex, 0);
    }
    if (g_p_light.prog) {
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_light.prog);
        if (g_p_light.uScene  >= 0) p_glUniform1i(g_p_light.uScene,  0);
        if (g_p_light.uDepth  >= 0) p_glUniform1i(g_p_light.uDepth,  1);
        if (g_p_light.uAOTex  >= 0) p_glUniform1i(g_p_light.uAOTex,  2);
        if (g_p_light.uShadow >= 0) p_glUniform1i(g_p_light.uShadow, 3);
        if (g_p_light.uSSRTex >= 0) p_glUniform1i(g_p_light.uSSRTex, 4);
        /* tier 4r's two data textures.  Units 5 and 6 were free: the whole
         * table above stops at 4, and the BVH is the first thing in this
         * file that reads a buffer rather than a picture. */
        if (g_p_light.uRtNode >= 0) p_glUniform1i(g_p_light.uRtNode, 5);
        if (g_p_light.uRtTri  >= 0) p_glUniform1i(g_p_light.uRtTri,  6);
        /* tier 4rc's ONE texture, on the EIGHTH and last unit ESSL 1.00
         * guarantees.  The cars' nodes and triangles share it for exactly
         * that reason -- see ONE TEXTURE over AFX_LIGHT_U_SHADOW_RT_CARS. */
        if (g_p_light.uRtCar  >= 0) p_glUniform1i(g_p_light.uRtCar,  7);
    }
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, 0);
}

/* Append to a bounded buffer, tracking the cursor.  The assembled deferred
 * shader is built out of a dozen fragments and the alternative -- a chain of
 * strncat calls -- rescans the whole string every time and silently truncates
 * with no way to notice.  This one reports. */
typedef struct { char *p; size_t cap, len; int over; } AfxStr;

static void afx_put(AfxStr *s, const char *frag)
{
    size_t n = strlen(frag);
    if (s->len + n + 1 > s->cap) { s->over = 1; return; }
    memcpy(s->p + s->len, frag, n + 1);
    s->len += n;
}

/* CAN THIS CONTEXT SAMPLE A DEPTH TEXTURE AT ALL?
 *
 * Asked HERE, on a disposable 16x16 framebuffer, and not later where the real
 * one is built -- because the answer decides which blocks get spliced into the
 * deferred shader, and the shaders are assembled before the targets are made.
 * Finding out afterwards would mean either rebuilding every program or
 * shipping a shader that samples an attachment that is not there.
 *
 * The probe is exactly the real thing in miniature: the same four format
 * spellings, the same completeness check.  A driver that passes this and then
 * refuses the full-size attachment still lands on afx_scene_depth's own
 * fallback, so the probe is an optimisation of the failure path, not a
 * substitute for checking it. */
static int afx_photo_probe_depth(void)
{
    static const unsigned ifmt[4] = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16,
                                      GL_DEPTH_COMPONENT,   GL_DEPTH_COMPONENT };
    static const unsigned type[4] = { GL_UNSIGNED_INT, GL_UNSIGNED_SHORT,
                                      GL_UNSIGNED_INT, GL_UNSIGNED_SHORT };
    unsigned fbo = 0, col = 0, dep = 0;
    int i, ok = 0;

    p_glGenFramebuffers(1, &fbo);
    glGenTextures(1, &col);
    glGenTextures(1, &dep);
    afx_tex_alloc(col, 16, 16);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, col, 0);
    for (i = 0; i < 4 && !ok; i++) {
        glBindTexture(GL_TEXTURE_2D, dep);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        (void)glGetError();
        glTexImage2D(GL_TEXTURE_2D, 0, (int)ifmt[i], 16, 16, 0,
                     GL_DEPTH_COMPONENT, type[i], NULL);
        if (glGetError()) continue;
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_TEXTURE_2D, dep, 0);
        if (glGetError()) continue;
        ok = (p_glCheckFramebufferStatus(GL_FRAMEBUFFER)
              == GL_FRAMEBUFFER_COMPLETE);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &col);
    glDeleteTextures(1, &dep);
    (void)glGetError();
    return ok;
}

/* ONE LINE SAYING WHAT THE LAYER IS ACTUALLY DOING.
 *
 * The same argument as afx_decline()'s: an effect that silently stood down is
 * indistinguishable from an effect that was never asked for, and this file's
 * history is a list of features that were quietly lost on one platform.  The
 * caller prints this next to the post-path verdict, so the first thing the
 * game says about post-processing also says which of the six survived. */
static void afx_photo_verdict(void)
{
    static const char *const NAME[B3_PHOTO_FX_COUNT] = {
        "tonemap", "ssao", "atmos", "shadow", "ssr", "godray", "lights"
    };
    size_t at = 0;
    int i, any = 0;
    if (!b3_photo_on()) {
        snprintf(g_photo_line, sizeof g_photo_line,
                 "photo: OFF (B3_PHOTO=0) -- the pre-wave frame, bit for bit");
        return;
    }
    at += (size_t)snprintf(g_photo_line + at, sizeof g_photo_line - at,
                           "photo: on");
    for (i = 0; i < B3_PHOTO_FX_COUNT; i++) {
        if (!g_photo_live[i]) continue;
        /* WHICH SHADOW.  "shadow" alone has meant a 2048 map for as long as
         * the tier has existed and it must not quietly start meaning
         * something else -- the two look different and cost differently. */
        if (i == B3_PHOTO_FX_SHADOW && g_rt_built) {
            at += (size_t)snprintf(g_photo_line + at,
                                   sizeof g_photo_line - at,
                                   " shadow(RAY %d tris)", b3_rt_tri_count());
            any = 1;
            if (at >= sizeof g_photo_line) return;
            continue;
        }
        at += (size_t)snprintf(g_photo_line + at, sizeof g_photo_line - at,
                               " %s", NAME[i]);
        any = 1;
        if (at >= sizeof g_photo_line) return;
    }
    /* WHY THE RAY IS NOT ANSWERING, when it was asked to.  The option is a
     * user-facing switch now, so "I turned it on and nothing happened" has
     * to have an answer on the one line the game prints about the layer --
     * no world for this track, no float textures on this context, or a
     * traversal the driver would not compile. */
    if (b3_rt_want() && !g_rt_built && g_rt_why)
        at += (size_t)snprintf(g_photo_line + at, sizeof g_photo_line - at,
                               " (no RAY: %s)", g_rt_why);
    if (at >= sizeof g_photo_line) return;
    if (!any)
        snprintf(g_photo_line + at, sizeof g_photo_line - at,
                 " -- but nothing survived the context check");
    else
        for (i = 0; i < B3_PHOTO_FX_COUNT; i++) {
            if (g_photo_live[i] || !b3_photo_fx(i)) continue;
            at += (size_t)snprintf(g_photo_line + at, sizeof g_photo_line - at,
                                   " (no %s)", NAME[i]);
            if (at >= sizeof g_photo_line) return;
        }
}

/* ASSEMBLE THE DEFERRED PASS out of the blocks whose effects are live.  See
 * the long note over AFX_LIGHT_HEAD for why the order is what it is (surface
 * first, then the air) and why an off effect is ABSENT rather than branched
 * around. */
static int afx_photo_build_light(void)
{
    /* THE TWO BUFFERS GREW WITH TIER 4r, and by more than its own length.
     * The traversal's uniform block alone is 2.4 KB of GLSL (it carries the
     * two fetch helpers and the whole stackless walk), so `sh` had to take a
     * block bigger than a PCF kernel -- and with the walk spliced in, the
     * assembled deferred shader reaches about 8.1 KB, which the old 8192
     * would have refused with "did not fit its buffer" and stood the whole
     * surface down.  Both are stack, and both targets have megabytes of it
     * (the web link sets -sSTACK_SIZE=33554432). */
    char buf[16384], sh[4096];
    AfxStr s;
    afx_photo_read_knobs();
    s.p = buf; s.cap = sizeof buf; s.len = 0; s.over = 0;
    buf[0] = 0;

    afx_put(&s, AFX_LIGHT_HEAD);
    if (g_photo_live[B3_PHOTO_FX_SSAO])   afx_put(&s, AFX_LIGHT_U_AO);
    /* tier 4r replaces tier 4's block ENTIRELY -- both the uniforms and the
     * body -- so with ray tracing off not one character of it is spliced and
     * the assembled source is the pre-4r one, byte for byte. */
    if (g_photo_live[B3_PHOTO_FX_SHADOW] && g_rt_built) {
        B3RtKnobs rk;
        b3_rt_knobs(&rk);
        snprintf(sh, sizeof sh, AFX_LIGHT_U_SHADOW_RT, rk.steps);
        afx_put(&s, sh);
        /* tier 4rc rides along, or does not: the ray loop calls b3rtCars()
         * either way and one of these two defines it.  A CAR TREE'S BUDGET IS
         * ITS OWN and is much smaller than the world's -- a car is a couple of
         * thousand triangles in a box a ray crosses once, so the world's 256
         * would be paying for a depth this tree cannot reach.  Measured over
         * the fleet, no model exceeds depth 16. */
        if (g_rtc_built) {
            snprintf(sh, sizeof sh, AFX_LIGHT_U_SHADOW_RT_CARS,
                     g_rtc_n, g_rtc_n, g_rtc_n, g_rtc_n, g_rtc_n,
                     AFX_RTC_STEPS, g_rtc_n);
            afx_put(&s, sh);
        } else {
            afx_put(&s, AFX_LIGHT_U_SHADOW_RT_NOCARS);
        }
    } else if (g_photo_live[B3_PHOTO_FX_SHADOW]) {
        afx_put(&s, AFX_LIGHT_U_SHADOW);
    }
    if (g_photo_live[B3_PHOTO_FX_SSR])    afx_put(&s, AFX_LIGHT_U_SSR);
    if (g_photo_live[B3_PHOTO_FX_ATMOS])  afx_put(&s, AFX_LIGHT_U_ATMOS);
    if (g_photo_live[B3_PHOTO_FX_LIGHTS]) {
        snprintf(sh, sizeof sh, AFX_LIGHT_U_LIGHTS,
                 g_pk.li_n, g_pk.li_n, g_pk.li_n);
        afx_put(&s, sh);
    }
    if (g_photo_live[B3_PHOTO_FX_SHADOW] || g_photo_live[B3_PHOTO_FX_LIGHTS])
        afx_put(&s, AFX_LIGHT_U_NORMAL);
    afx_put(&s, AFX_LIGHT_MAIN);
    /* the reconstructed normal is four depth taps and four reconstructions, so
     * it is spliced in only when something below wants it -- the shadow's
     * slope-scaled bias, and now tier 7's N.L */
    if (g_photo_live[B3_PHOTO_FX_SHADOW] || g_photo_live[B3_PHOTO_FX_LIGHTS])
        afx_put(&s, AFX_LIGHT_NORMAL);
    if (g_photo_live[B3_PHOTO_FX_SSAO])   afx_put(&s, AFX_LIGHT_AO);
    if (g_photo_live[B3_PHOTO_FX_SHADOW] && g_rt_built) {
        B3RtKnobs rk;
        b3_rt_knobs(&rk);
        snprintf(sh, sizeof sh, AFX_LIGHT_SHADOW_RT,
                 rk.rays, rk.rays, rk.rays);
        afx_put(&s, sh);
    } else if (g_photo_live[B3_PHOTO_FX_SHADOW]) {
        int k = g_pk.sh_pcf;
        int n = (2 * k + 1) * (2 * k + 1);
        snprintf(sh, sizeof sh, AFX_LIGHT_SHADOW, k, k, k, k, n);
        afx_put(&s, sh);
    }
    /* AFTER the sun's shadow and BEFORE the reflection and the air: a lamp
     * lights a surface the sun cannot reach, which is the whole point of it,
     * so it must be added to a pixel the shadow has already darkened rather
     * than to one it is about to. */
    if (g_photo_live[B3_PHOTO_FX_LIGHTS]) {
        snprintf(sh, sizeof sh, AFX_LIGHT_LIGHTS, g_pk.li_n);
        afx_put(&s, sh);
    }
    if (g_photo_live[B3_PHOTO_FX_SSR])    afx_put(&s, AFX_LIGHT_SSR);
    if (g_photo_live[B3_PHOTO_FX_ATMOS])  afx_put(&s, AFX_LIGHT_ATMOS);
    afx_put(&s, AFX_LIGHT_TAIL);
    if (s.over) {
        afx_say("PHOTO: the deferred shader did not fit its buffer -- the "
                "layer's surface effects stand down\n");
        return 0;
    }
    g_p_light.prog = afx_build(buf, "photo-light");
    return g_p_light.prog != 0;
}

/* --------------------------------------------- tier 4r's GL side --------
 *
 * Two NEAREST-filtered RGBA32F textures and one question asked once a frame:
 * does the shader that is compiled match the option the user has set?
 *
 * A FLOAT TEXTURE IS THE ONE CAPABILITY THIS FEATURE REQUIRES, and it is
 * asked for the way everything else in this file asks -- by trying the
 * spellings in order and reading glGetError, not by parsing a version
 * string.  The sized GL_RGBA32F is core in desktop GL 3.0+, GLES 3 and
 * WebGL 2; the unsized GL_RGBA + GL_FLOAT is what desktop GL 2.1 with
 * ARB_texture_float and WebGL 1 with OES_texture_float take.  A context that
 * takes neither loses the option and keeps the game -- which is exactly what
 * the cascade is still there for. */

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_MAX_TEXTURE_SIZE
#define GL_MAX_TEXTURE_SIZE 0x0D33
#endif

static int afx_rt_tex_upload(unsigned *tex, const float *data, int texels,
                             int w, int rows)
{
    int    full = texels / w;
    int    rem  = texels - full * w;
    float *pad;

    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* NEAREST is not a quality choice, it is the only correct one: these
     * texels are numbers, and an interpolated node index is nonsense.  It is
     * also what keeps the format requirement to OES_texture_float rather
     * than OES_texture_float_LINEAR. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    while (glGetError() != GL_NO_ERROR) { }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, rows, 0,
                 GL_RGBA, GL_FLOAT, NULL);
    if (glGetError() != GL_NO_ERROR) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, rows, 0,
                     GL_RGBA, GL_FLOAT, NULL);
        if (glGetError() != GL_NO_ERROR) {
            g_rt_why = "this context has no float textures";
            return 0;
        }
    }
    /* the whole rows straight out of the loader's buffer, then ONE padded
     * row for the tail -- a second full-size copy of a 40 MB buffer just to
     * round it up to the texture width is not a thing to allocate */
    if (full > 0)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, full,
                        GL_RGBA, GL_FLOAT, data);
    if (rem > 0) {
        pad = (float *)calloc((size_t)w * 4, sizeof *pad);
        if (!pad) { g_rt_why = "out of memory"; return 0; }
        memcpy(pad, data + (size_t)full * (size_t)w * 4,
               (size_t)rem * 4 * sizeof *pad);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, full, w, 1,
                        GL_RGBA, GL_FLOAT, pad);
        free(pad);
    }
    if (glGetError() != GL_NO_ERROR) {
        g_rt_why = "the driver refused the BVH upload";
        return 0;
    }
    return 1;
}

static void afx_rt_free(void)
{
    if (g_rt_node_tex) { glDeleteTextures(1, &g_rt_node_tex); g_rt_node_tex = 0; }
    if (g_rt_tri_tex)  { glDeleteTextures(1, &g_rt_tri_tex);  g_rt_tri_tex = 0; }
    g_rt_ok = 0;
}

/* ---------------------------------------------- tier 4rc's GL side ------ */

static void afx_rtc_free(void)
{
    if (g_rtc_tex) { glDeleteTextures(1, &g_rtc_tex); g_rtc_tex = 0; }
    g_rtc_ok = 0;
    g_rtc_uploaded_gen = 0;
}

/* THE ARRAY'S LENGTH, and it is a property of the MODE and nothing else.
 *
 * Six racers plus the player is a grid, so `racers` reserves eight -- one more
 * than the game ever puts on a start line, which costs one compare a ray and
 * removes a whole class of "what happens with a full field" question.  `all`
 * adds room for the traffic a track can have near the camera; past that the
 * furthest instances are simply not uploaded, which degrades to fewer traced
 * cars rather than to a wrong picture. */
static int afx_rtc_budget(void)
{
    int n;
    switch (b3_rt_cars_mode()) {
    case B3_RT_CARS_OFF:    return 0;
    case B3_RT_CARS_ALL:    n = 20; break;
    default:                n = 8;  break;
    }
    {
        const char *e = getenv("B3_RT_CAR_N");
        if (e && *e) n = atoi(e);
    }
    if (n < 0) n = 0;
    if (n > AFX_RTC_MAX) n = AFX_RTC_MAX;
    return n;
}

int b3_afx_rt_car_budget(void)
{
    return (g_rtc_built && g_rtc_ok) ? g_rtc_n : 0;
}

int b3_afx_rt_car_traced(int slot)
{
    if (!g_rtc_built || !g_rtc_ok || slot < 0 || slot >= 32) return 0;
    return (g_rtc_traced >> slot) & 1u;
}

void b3_afx_rt_cars_begin(void)
{
    int i;
    g_rtc_live = 0;
    g_rtc_traced = 0;
    /* EVERY slot zeroed, not just the ones past `live`: the loop bound is the
     * ARRAY's length, so a slot this frame does not fill still runs and must
     * carry a zero radius rather than the car that was there two corners ago.
     * The same rule, and the same reason, as tier 7's light array. */
    for (i = 0; i < AFX_RTC_MAX * 4; i++) g_rtc_s[i] = 0.0f;
}

void b3_afx_rt_car_add(int slot, int model, const float pos[3],
                       const float rot9[9])
{
    B3RtCarModel m;
    float       *S, *X, *Y, *Z, *N;
    float        c[3], r;
    int          i;

    if (!g_rtc_built || !g_rtc_ok) return;
    if (g_rtc_live >= g_rtc_n || model < 0 || !pos || !rot9) return;
    if (!b3_rt_car_packed(model, &m)) return;

    /* B3_RT_CAR_TRACE=0 -- THE CASTER DIAGNOSTIC, and it earns its four lines
     * the same way B3_PHOTO_SH_CASTERS does.
     *
     * "Does this car cast?" can only be answered by rendering the SAME frame
     * with and without that car in the trace -- and every other way of
     * arranging that changes something else too: removing the car removes its
     * bodywork, and emptying the budget recompiles the shader AND brings its
     * blob back.  This drops the instance from the RAY while leaving the
     * program, the blob decision and the pose exactly where they were, so the
     * difference between the two frames is the shadow and nothing else.  It is
     * what tools/validate_photo.py section 13 measures. */
    {
        static int trace = -1;
        if (trace < 0) {
            const char *e = getenv("B3_RT_CAR_TRACE");
            trace = (e && *e) ? (*e != '0') : 1;
        }
        if (!trace) {
            if (slot >= 0 && slot < 32) g_rtc_traced |= 1u << slot;
            return;
        }
    }

    i = g_rtc_live++;
    S = g_rtc_s + i * 4;
    X = g_rtc_x + i * 4;
    Y = g_rtc_y + i * 4;
    Z = g_rtc_z + i * 4;
    N = g_rtc_node + i * 4;

    /* THE WORLD SPHERE.  The model box's centre carried out by the pose; the
     * radius is the model's own circumsphere, which is rotation-invariant --
     * which is the entire reason a sphere is the reject and not a box. */
    {
        float mc[3];
        for (i = 0; i < 3; i++) mc[i] = (m.lo[i] + m.hi[i]) * 0.5f;
        c[0] = pos[0] + rot9[0] * mc[0] + rot9[1] * mc[1] + rot9[2] * mc[2];
        c[1] = pos[1] + rot9[3] * mc[0] + rot9[4] * mc[1] + rot9[5] * mc[2];
        c[2] = pos[2] + rot9[6] * mc[0] + rot9[7] * mc[1] + rot9[8] * mc[2];
        r = m.radius;
    }
    S[0] = c[0]; S[1] = c[1]; S[2] = c[2];
    S[3] = r * r;

    /* THE INVERSE, and it is a TRANSPOSE because the pose is a rotation.
     * rot9 is ROW-MAJOR object->world (world.x = R[0]*m.x + R[1]*m.y +
     * R[2]*m.z, the convention car_lamp_pose() and the corona pass share), so
     * world->model reads DOWN its columns.  Getting this backwards is not
     * subtle and has bitten this codebase before -- the headlights were placed
     * by the car's rotation run backwards and lit the road behind the car --
     * so it is written out rather than expressed as a loop. */
    X[0] = rot9[0]; X[1] = rot9[3]; X[2] = rot9[6];
    Y[0] = rot9[1]; Y[1] = rot9[4]; Y[2] = rot9[7];
    Z[0] = rot9[2]; Z[1] = rot9[5]; Z[2] = rot9[8];
    X[3] = -(X[0] * pos[0] + X[1] * pos[1] + X[2] * pos[2]);
    Y[3] = -(Y[0] * pos[0] + Y[1] * pos[1] + Y[2] * pos[2]);
    Z[3] = -(Z[0] * pos[0] + Z[1] * pos[1] + Z[2] * pos[2]);

    N[0] = m.root;
    N[1] = m.end;
    N[2] = N[3] = 0.0f;

    if (slot >= 0 && slot < 32) g_rtc_traced |= 1u << slot;
}

/* WHAT THE USER WANTS, filtered through what is possible.  The ray REPLACES
 * the map's term rather than adding one, so it needs the shadow tier live to
 * have a term to replace -- and the master gate takes precedence over
 * everything, because B3_PHOTO=0 has to be a fact about the source and not a
 * preference. */
static int g_rt_refused;     /* the RT variant failed to compile, once */

static int afx_rt_want(void)
{
    return !g_rt_refused && b3_photo_on() &&
           g_photo_live[B3_PHOTO_FX_SHADOW] &&
           b3_rt_want() && b3_rt_world_ready();
}

/* Upload the current world, if it is not already up there.  Keyed on the
 * loader's GENERATION and not on its buffer pointer: a track change frees the
 * old buffers and allocates new ones, and malloc handing back the address it
 * has just freed is not a hypothetical -- it is the likeliest outcome.  A
 * counter cannot alias. */
static unsigned g_rt_uploaded_gen;      /* 0 = nothing uploaded */

/* THE CARS' texture, and it is uploaded on the SELECTION's generation rather
 * than the world's: the fleet does not change when the track does, but the
 * PACKED SUBSET changes whenever the game loads a different set of cars.  The
 * same counter-not-pointer rule the world's upload documents -- a freed buffer
 * and its replacement are the two addresses malloc is most likely to reuse. */
static void afx_rtc_upload(void)
{
    int w, rows;

    if (b3_rt_car_gen() == 0u || b3_rt_car_texels() <= 0) {
        afx_rtc_free();
        return;
    }
    if (g_rtc_ok && g_rtc_uploaded_gen == b3_rt_car_gen()) return;
    afx_rtc_free();

    w    = b3_rt_tex_w();
    rows = b3_rt_car_rows();
    if (rows <= 0) return;
    if (!afx_rt_tex_upload(&g_rtc_tex, b3_rt_car_data(),
                           b3_rt_car_texels(), w, rows)) {
        afx_rtc_free();
        return;
    }
    g_rtc_uploaded_gen = b3_rt_car_gen();
    g_rtc_ok = 1;
    afx_say("PHOTO: car ray tracing -- %d model%s, %d nodes / %d triangles "
            "in one %dx%d float texture (%.1f MB)\n",
            b3_rt_car_selected(), b3_rt_car_selected() == 1 ? "" : "s",
            b3_rt_car_node_count(), b3_rt_car_tri_count(), w, rows,
            (double)b3_rt_car_texels() * 16.0 / (1024.0 * 1024.0));
}

static void afx_rt_upload(void)
{
    int maxtex = 0, w;

    if (g_rt_ok && g_rt_uploaded_gen == b3_rt_world_gen()) return;
    afx_rt_free();
    g_rt_uploaded_gen = 0;
    if (!b3_rt_world_ready()) { g_rt_why = b3_rt_status(); return; }

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maxtex < 64) maxtex = 2048;
    /* A POWER-OF-TWO WIDTH, because the shader's `index / width` has to be
     * EXACT -- it is a row number, and a row number that is off by one reads
     * a different node.  A power of two makes 1/width exact in a float and
     * the division a multiply. */
    w = maxtex < 2048 ? maxtex : 2048;
    b3_rt_tex_set_width(w);
    w = b3_rt_tex_w();
    if (b3_rt_node_rows() > maxtex || b3_rt_tri_rows() > maxtex) {
        g_rt_why = "the BVH does not fit this context's texture size";
        return;
    }

    if (!afx_rt_tex_upload(&g_rt_node_tex, b3_rt_node_data(),
                           b3_rt_node_texels(), w, b3_rt_node_rows()) ||
        !afx_rt_tex_upload(&g_rt_tri_tex, b3_rt_tri_data(),
                           b3_rt_tri_texels(), w, b3_rt_tri_rows())) {
        afx_rt_free();
        return;
    }
    g_rt_uploaded_gen = b3_rt_world_gen();
    g_rt_ok = 1;
    g_rt_why = NULL;
    afx_say("PHOTO: ray tracing ON -- %d nodes / %d triangles in two "
            "%dx%d float textures (%.1f MB)\n",
            b3_rt_node_count(), b3_rt_tri_count(), w,
            b3_rt_node_rows() + b3_rt_tri_rows(),
            (double)(b3_rt_node_texels() + b3_rt_tri_texels()) * 16.0
            / (1024.0 * 1024.0));
}

int b3_afx_rt_active(void) { return g_rt_built && g_rt_ok; }

/* THE ONE QUESTION, once a frame.  When the answer changes the deferred
 * program is REBUILT -- see the note over g_rt_built for why that is the
 * honest way to switch an assembled shader rather than a shame. */
static void afx_rt_sync(void)
{
    int want = afx_rt_want();
    int cars;

    /* The car texture is uploaded BESIDE the world's, not inside it.  Nesting
     * it in afx_rt_upload()'s success path looked tidier and was wrong: that
     * function early-outs when the WORLD is already up to date, so a change to
     * the car SELECTION with no track change would never have reached the
     * upload.  The two artefacts have separate generations because they change
     * at separate times -- the fleet does not vary by track. */
    if (want) { afx_rt_upload(); afx_rtc_upload(); }
    else if (g_rt_ok || g_rt_node_tex) { afx_rt_free(); afx_rtc_free(); }

    want = want && g_rt_ok;

    /* THE CARS RIDE ON THE WORLD.  Tier 4rc is not a tier of its own: it adds
     * occluders to a trace that has to be running for there to be anything to
     * add them to, so it stands down whenever the ray does and never the other
     * way round. */
    cars = want && g_rtc_ok && afx_rtc_budget() > 0;
    if (cars) g_rtc_n = afx_rtc_budget();

    /* ---- THE ARMING LINE, and this is the only place that knows the whole
     * verdict.  One line, once, whichever way it lands.
     *
     * It exists because the option is OFF by default, needs the photorealism
     * layer AND its shadow tier, and can additionally be refused by the
     * context -- four different reasons, each of which looks from the outside
     * exactly like "the knob I set does nothing".  A user set B3_RT_RAYS=128,
     * saw the frame rate not move, and reported the knob as dead; the knob
     * was fine and the FEATURE was off.  Guarded on the program existing,
     * because before that the answer is not final yet. */
    if (g_p_light.prog) {
        const char *why = NULL;
        if (!b3_photo_on())
            why = "B3_PHOTO=0 gates the whole layer";
        else if (!g_photo_live[B3_PHOTO_FX_SHADOW])
            why = "the sun-shadow tier is down -- the ray REPLACES its term, "
                  "so there is nothing here for it to replace";
        else if (!b3_rt_want())
            why = b3_rt_env_forced() ? "B3_RT=0"
                                     : "the settings menu; B3_RT=1 overrides";
        else if (g_rt_why)
            why = g_rt_why;
        b3_rt_announce(want, why);
    }

    if (want == g_rt_built && cars == g_rtc_built) return;
    if (!g_p_light.prog) { g_rt_built = 0; g_rtc_built = 0; return; }

    g_rt_built  = want;
    g_rtc_built = cars;
    /* the old program goes now, not at shutdown: a user toggling the option
     * back and forth would otherwise leak one program per press */
    if (g_p_light.prog) { p_glDeleteProgram(g_p_light.prog);
                          g_p_light.prog = 0; }
    if (!afx_photo_build_light()) {
        /* the RT variant would not compile: fall straight back to the map,
         * say so once, and never try again this run */
        afx_say("PHOTO: the ray-traced shadow would not build on this "
                "context -- the depth-map cascade stays\n");
        g_rt_built = 0;
        g_rtc_built = 0;
        g_rt_refused = 1;
        g_rt_why = "the traversal would not compile here";
        afx_rt_free();
        afx_rtc_free();
        afx_photo_build_light();
    }
    afx_progs_locs();
    afx_photo_verdict();
}

static int afx_build_all(void)
{
    char radial[1400], composite[6400], comp_u[256], comp_tail[2800];
    char scratch[2600];
    unsigned char ramp[256], rgba[256 * 4];
    int i;
    const char *e;
    int add_mode;
    static const float TRI[6] = { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f };

    /* THE REALITY CHECK.  Up to here the six switches are what the operator
     * asked for; from here they are what this GL context can actually do.
     * The two entry points and the depth texture are the only hard
     * requirements, and losing them costs the layer, never the chain. */
    afx_photo_read_knobs();
    for (i = 0; i < B3_PHOTO_FX_COUNT; i++) g_photo_live[i] = b3_photo_fx(i);
    if (b3_photo_on() && (!p_glUniform3f || !p_glUniformMatrix4fv)) {
        for (i = 0; i < B3_PHOTO_FX_COUNT; i++) g_photo_live[i] = 0;
        afx_say("PHOTO: no glUniform3f/glUniformMatrix4fv on this context -- "
                "the whole photorealism layer stands down\n");
    }
    if (afx_photo_wants_depth()) {
        g_photo_depth_ok = afx_photo_probe_depth();
        if (!g_photo_depth_ok) {
            g_photo_live[B3_PHOTO_FX_SSAO]   = 0;
            g_photo_live[B3_PHOTO_FX_ATMOS]  = 0;
            g_photo_live[B3_PHOTO_FX_SHADOW] = 0;
            g_photo_live[B3_PHOTO_FX_SSR]    = 0;
            g_photo_live[B3_PHOTO_FX_GODRAY] = 0;
            g_photo_live[B3_PHOTO_FX_LIGHTS] = 0;
            afx_say("PHOTO: this context will not sample a depth texture "
                    "(tried 24/16-bit, sized and unsized) -- SSAO, "
                    "atmospherics, shadows, SSR, god rays and the per-source "
                    "lights stand down; the tonemap and grade carry on\n");
        }
    }

    /* B3_AFX_TAPS=<n> overrides the tap count. The taps are a ZOOM trail --
     * each one steps 1% further in (B3_BLUR_ZOOM_A) -- so the count sets the
     * LENGTH of the streak, not just its smoothness: 0.99^n is 0.85 at 16 and
     * 0.75 at 28. It is a tuning knob because the right answer is a look, and
     * a look is settled by looking (tools/afx_sweep.py). */
    g_taps = B3_AFX_TAPS;
    if ((e = getenv("B3_AFX_TAPS")) && *e) {
        g_taps = atoi(e);
        if (g_taps < 2)  g_taps = 2;
        if (g_taps > 64) g_taps = 64;
    }
    snprintf(radial, sizeof radial, AFX_FS_RADIAL, g_taps);

    /* B3_AFX_PRESENT_ADD=1 -- the literal recovered composite. See the long
     * note over AFX_FS_COMPOSITE: the default is an INSPIRED cross-fade
     * because the recovered ADD cannot blur anything, and this env keeps the
     * [C] equation runnable rather than merely documented. */
    e = getenv("B3_AFX_PRESENT_ADD");
    add_mode = (e && *e && atoi(e) != 0);

    /* THE COMPOSITE'S ENDING is where tier 1 and tier 6 land, and it is
     * assembled rather than branched for the reason this whole wave turns on:
     * with B3_PHOTO=0 both fragments below are empty / the recovered line, so
     * the string handed to the driver is BYTE-IDENTICAL to the pre-wave one
     * and "bit-identical output" is a fact about the source rather than a hope
     * about the optimiser. */
    comp_u[0] = 0;
    comp_tail[0] = 0;
    if (g_photo_live[B3_PHOTO_FX_TONEMAP] || g_photo_live[B3_PHOTO_FX_GODRAY]) {
        snprintf(comp_u, sizeof comp_u,
                 "uniform vec4 uPhoto;\n%s%s",
                 g_photo_live[B3_PHOTO_FX_GODRAY]
                     ? "uniform sampler2D uGodTex;\n" : "",
                 g_photo_live[B3_PHOTO_FX_TONEMAP]
                     ? "uniform sampler2D uLut;\n" : "");
        if (g_photo_live[B3_PHOTO_FX_GODRAY])
            snprintf(comp_tail, sizeof comp_tail, "%s", AFX_COMPOSITE_GOD);
        if (g_photo_live[B3_PHOTO_FX_TONEMAP]) {
            size_t at = strlen(comp_tail);
            snprintf(scratch, sizeof scratch, AFX_COMPOSITE_TONEMAP,
                     B3_PHOTO_LUT_N);
            snprintf(comp_tail + at, sizeof comp_tail - at, "%s", scratch);
        } else {
            size_t at = strlen(comp_tail);
            snprintf(comp_tail + at, sizeof comp_tail - at, "%s",
                     AFX_COMPOSITE_PLAIN);
        }
    } else {
        snprintf(comp_tail, sizeof comp_tail, "%s", AFX_COMPOSITE_PLAIN);
    }
    snprintf(composite, sizeof composite, AFX_FS_COMPOSITE, comp_u,
             add_mode ? AFX_COMPOSITE_ADD : AFX_COMPOSITE_MIX, comp_tail);
    if (add_mode)
        afx_say("B3_AFX_PRESENT_ADD=1: the recovered ADD composite "
                "(out = 2*(scene + C0.a*blur)) -- no cross-fade, so the "
                "smear will read as a glow, not as blur\n");

    g_p_down.prog      = afx_build(AFX_FS_DOWN,      "down");
    g_p_radial.prog    = afx_build(radial,           "radial");
    g_p_bright.prog    = afx_build(AFX_FS_BRIGHT,    "bright");
    g_p_blur.prog      = afx_build(AFX_FS_BLUR,      "blur");
    g_p_composite.prog = afx_build(composite,        "composite");
    g_p_gamma.prog     = afx_build(AFX_FS_GAMMA,     "gamma");
    if (!g_p_down.prog || !g_p_radial.prog || !g_p_bright.prog
        || !g_p_blur.prog || !g_p_composite.prog || !g_p_gamma.prog)
        return afx_decline("shader build failed (%s%s%s%s%s%s) -- the compile "
                           "log is the [afx] line above",
                           g_p_down.prog      ? "" : "down ",
                           g_p_radial.prog    ? "" : "radial ",
                           g_p_bright.prog    ? "" : "bright ",
                           g_p_blur.prog      ? "" : "blur ",
                           g_p_composite.prog ? "" : "composite ",
                           g_p_gamma.prog     ? "" : "gamma");

    /* THE PHOTOREALISM PASSES.  A build failure here is NEVER a chain
     * decline: the effect whose program would not compile switches itself off,
     * says so once, and everything else carries on.  A driver that cannot
     * compile the SSAO loop must not cost the player their motion blur. */
    if (g_photo_live[B3_PHOTO_FX_SSAO]) {
        snprintf(scratch, sizeof scratch, AFX_FS_AO,
                 g_pk.ao_taps, g_pk.ao_taps, g_pk.ao_taps);
        g_p_ao.prog     = afx_build(scratch,       "photo-ao");
        g_p_aoblur.prog = afx_build(AFX_FS_AOBLUR, "photo-aoblur");
        if (!g_p_ao.prog || !g_p_aoblur.prog) {
            g_photo_live[B3_PHOTO_FX_SSAO] = 0;
            afx_say("PHOTO: the SSAO shaders would not build -- SSAO off\n");
        }
    }
    if (g_photo_live[B3_PHOTO_FX_SSR]) {
        snprintf(scratch, sizeof scratch, AFX_FS_SSR, g_pk.ssr_steps);
        g_p_ssr.prog = afx_build(scratch, "photo-ssr");
        if (!g_p_ssr.prog) {
            g_photo_live[B3_PHOTO_FX_SSR] = 0;
            afx_say("PHOTO: the SSR shader would not build -- SSR off\n");
        }
    }
    if (g_photo_live[B3_PHOTO_FX_GODRAY]) {
        snprintf(scratch, sizeof scratch, AFX_FS_GOD,
                 g_pk.gr_taps, g_pk.gr_taps, g_pk.gr_taps);
        g_p_godsrc.prog = afx_build(AFX_FS_GODSRC, "photo-godsrc");
        g_p_god.prog    = afx_build(scratch,       "photo-god");
        if (!g_p_godsrc.prog || !g_p_god.prog) {
            g_photo_live[B3_PHOTO_FX_GODRAY] = 0;
            afx_say("PHOTO: the god-ray shaders would not build -- off\n");
        }
    }
    /* A BUDGET OF ZERO IS THE TIER OFF, not a shader with an empty array:
     * `uniform vec4 uLightP[0]` does not compile anywhere, and B3_PHOTO_LIGHT_N
     * is an operator-facing number.  glUniform4fv is the same story: it is the
     * one array upload in this file, and a context without it loses tier 7 and
     * nothing else. */
    if (g_photo_live[B3_PHOTO_FX_LIGHTS] && (g_pk.li_n < 1 || !p_glUniform4fv)) {
        g_photo_live[B3_PHOTO_FX_LIGHTS] = 0;
        if (!p_glUniform4fv)
            afx_say("PHOTO: no glUniform4fv on this context -- the per-source "
                    "lights stand down\n");
    }
    /* TIER 7 GIVES GROUND BEFORE THE OTHERS DO.  Its two uniform ARRAYS are
     * the only thing in the deferred pass whose size the operator sets, and
     * GLES2 -- and therefore WebGL 1 -- guarantees only sixteen fragment
     * uniform vectors.  Every real implementation gives far more, but "the
     * whole deferred pass would not build" is much too big a price for a
     * budget that was set too high, so the retry halves the budget until the
     * shader fits and only then gives up the tier. */
    /* tier 4r, BEFORE the first assembly: a run that boots with the option
     * already on should compile the traversal once, not compile the map
     * variant and throw it away on frame 1. */
    if (afx_rt_want()) {
        afx_rt_upload();
        g_rt_built = g_rt_ok;
    }
    /* *** WHAT THIS CONTEXT ACTUALLY GIVES US, printed rather than assumed.
     *
     * The paragraph above says GLES2 guarantees sixteen fragment uniform
     * vectors and that "every real implementation gives far more".  That was
     * true when it was written and it was still a guess, and the first time
     * anyone needed the real number was tier 7d: the flames wanted six slots,
     * the array had two, and the whole question was whether AFX_LIGHT_MAX
     * could rise (see THE CEILING over b3_photo_boost_slots()).  It cannot be
     * decided from a spec minimum and it cannot be decided from this box
     * alone, so the number goes in the log on EVERY target -- desktop GL, the
     * Android gl4es link and the web build all reach this line -- and the next
     * person to ask has three measurements instead of an argument.
     *
     * GL_MAX_FRAGMENT_UNIFORM_VECTORS is the GLES2/WebGL spelling and counts
     * vec4s; desktop GL spells the same capacity GL_MAX_FRAGMENT_UNIFORM_
     * COMPONENTS and counts floats.  Ask for both, report in vec4s, and say
     * which spelling answered -- a context that answers neither leaves the
     * number at zero rather than inventing one.  A GL error from the
     * unsupported enum is swallowed here on purpose: this is a diagnostic, and
     * a diagnostic that dirties the error state for the caller after it would
     * be worse than no diagnostic. */
    {
        int vecs = 0, comps = 0, guard;
        const char *how = "neither enum";
        /* THE DRAIN IS BOUNDED.  `while (glGetError())` is the idiom, and it
         * is an infinite loop on a driver that has lost its context and
         * decided to keep saying so -- which is not a hypothetical on a
         * suspended Android surface.  Eight is more than any one call can
         * queue. */
        glGetIntegerv(0x8DFD /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */, &vecs);
        for (guard = 0; guard < 8 && glGetError() != GL_NO_ERROR; guard++) { }
        if (vecs > 0) {
            how = "MAX_FRAGMENT_UNIFORM_VECTORS";
        } else {
            glGetIntegerv(0x8B49 /* ..._COMPONENTS */, &comps);
            for (guard = 0; guard < 8 && glGetError() != GL_NO_ERROR; guard++) { }
            if (comps > 0) { vecs = comps / 4; how = "MAX_FRAGMENT_UNIFORM_COMPONENTS/4"; }
        }
        afx_say("PHOTO: fragment uniform capacity %d vec4 (%s); tier 7 asks "
                "for %d of them (3 arrays x %d lights) with AFX_LIGHT_MAX %d\n",
                vecs, how, g_pk.li_n * 3, g_pk.li_n, AFX_LIGHT_MAX);
    }
    if (afx_photo_wants_light()) {
        while (!afx_photo_build_light()) {
            if (g_photo_live[B3_PHOTO_FX_LIGHTS] && g_pk.li_n > 1) {
                g_pk.li_n /= 2;
                afx_say("PHOTO: the deferred shader would not build -- "
                        "retrying with a light budget of %d\n", g_pk.li_n);
                continue;
            }
            if (g_photo_live[B3_PHOTO_FX_LIGHTS]) {
                g_photo_live[B3_PHOTO_FX_LIGHTS] = 0;
                afx_say("PHOTO: the deferred shader would not build even with "
                        "one light -- the per-source lights stand down\n");
                if (afx_photo_wants_light()) continue;
                break;
            }
            g_photo_live[B3_PHOTO_FX_SSAO]   = 0;
            g_photo_live[B3_PHOTO_FX_ATMOS]  = 0;
            g_photo_live[B3_PHOTO_FX_SHADOW] = 0;
            g_photo_live[B3_PHOTO_FX_SSR]    = 0;
            break;
        }
    }
    afx_progs_locs();
    if (g_photo_live[B3_PHOTO_FX_TONEMAP]) afx_photo_lut_build();
    afx_photo_verdict();

    /* one full-screen triangle, once */
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (long)sizeof TRI, TRI, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* the gamma ramp LUT */
    b3_postfx_gamma_table(ramp);
    for (i = 0; i < 256; i++) {
        rgba[i * 4 + 0] = ramp[i];
        rgba[i * 4 + 1] = ramp[i];
        rgba[i * 4 + 2] = ramp[i];
        rgba[i * 4 + 3] = 255;
    }
    glGenTextures(1, &g_ramp_tex);
    glBindTexture(GL_TEXTURE_2D, g_ramp_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return 1;
}

/* THE STALE DEPTH ATTACHMENT -- the whole reason a resize used to kill the
 * chain, and it is worth spelling out because the symptom named the wrong
 * thing.
 *
 * afx_resize() rebuilds the SCENE TARGET's colour texture first and checks the
 * framebuffer straight afterwards.  On the first build that check sees a
 * colour-only FBO and passes.  On every LATER build it sees the colour at the
 * NEW size and g_depth_rb -- still attached, still allocated at the OLD size,
 * because afx_scene_depth() does not run until seven targets later.  A GLES2 /
 * WebGL 1 framebuffer requires every attachment to have the SAME dimensions,
 * so the check comes back GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS (0x8CD9) and
 * the chain declines -- at ANY new size, odd or even; the one-pixel viewport
 * wobble that a user hit was only the trigger, never the cause.
 *
 * (0x8CD9 is INCOMPLETE_DIMENSIONS, not INCOMPLETE_MULTISAMPLE, which is
 * 0x8D56.  The two get confused because MSAA arrived in this file at the same
 * time; the MSAA pair is rebuilt whole by afx_ms_make() and was never stale.)
 *
 * DETACHING FIRST restores the first build's conditions for every build: the
 * completeness check sees colour alone, and afx_scene_depth() re-attaches the
 * depth renderbuffer at the new size a few lines later.  Nothing draws in
 * between.  GL 3.0+ dropped the same-dimensions rule, which is exactly why the
 * desktop never showed this and the web did. */
static void afx_scene_depth_detach(void)
{
    if (!g_t[T_SCENE].fbo) return;
    if (!g_depth_rb && !g_depth_tex) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, 0);
    /* ...and the TEXTURE spelling of the same stale attachment, which the
     * photorealism layer added and which has exactly the same property: a
     * depth attachment left over at the old size is what makes the rebuild's
     * completeness check answer INCOMPLETE_DIMENSIONS on WebGL. */
    if (g_depth_tex)
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_TEXTURE_2D, 0, 0);
}

static int afx_resize(int w, int h)
{
    int ok = 1;
    if (g_w == w && g_h == h && !g_force_rebuild) return 1;
    g_force_rebuild = 0;

    afx_scene_depth_detach();
    ok &= afx_target_make(&g_t[T_SCENE],   w, h,         "scene");
    ok &= afx_target_make(&g_t[T_UI],      w, h,         "ui");
    ok &= afx_target_make(&g_t[T_HALF],    w / 2, h / 2, "half");
    ok &= afx_target_make(&g_t[T_QUARTER], w / 4, h / 4, "quarter");
    ok &= afx_target_make(&g_t[T_BLUR],    w / 4, h / 4, "blur");
    ok &= afx_target_make(&g_t[T_BLOOM_A], w / 8, h / 8, "bloomA");
    ok &= afx_target_make(&g_t[T_BLOOM_B], w / 8, h / 8, "bloomB");
    /* THE PHOTOREALISM LAYER'S TARGETS, and note they are made on exactly the
     * same footing as the seven above -- through afx_target_make, inside this
     * one function, checked the same way.  That is what puts them under
     * tools/web_resize_sweep.py: a new target that skipped this would survive
     * every gate in the tree and then fail the first time a browser window
     * changed size, which is the defect this file's longest comment is about.
     * They are made ONLY when their effect is live, so B3_PHOTO=0 allocates
     * exactly what the pre-wave build allocated. */
    if (ok && afx_photo_wants_light())
        ok &= afx_target_make(&g_t[T_LIT],   w, h,         "photo-lit");
    if (ok && g_photo_live[B3_PHOTO_FX_SSAO]) {
        ok &= afx_target_make(&g_t[T_AO],    w / 2, h / 2, "photo-ao");
        ok &= afx_target_make(&g_t[T_AO_B],  w / 2, h / 2, "photo-aoB");
    }
    if (ok && g_photo_live[B3_PHOTO_FX_SSR])
        ok &= afx_target_make(&g_t[T_SSR],   w / 2, h / 2, "photo-ssr");
    if (ok && g_photo_live[B3_PHOTO_FX_GODRAY]) {
        ok &= afx_target_make(&g_t[T_GOD_A], w / 4, h / 4, "photo-godA");
        ok &= afx_target_make(&g_t[T_GOD_B], w / 4, h / 4, "photo-godB");
    }
    if (ok) {
        ok = afx_scene_depth(w, h);
        if (!ok)
            ok = afx_decline("no usable depth attachment at %dx%d "
                             "(tried 24-bit then 16-bit)", w, h);
    }
    if (!ok) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    /* MSAA is best-effort: it never fails the chain. */
    g_samples = 0;
    g_ms_why  = NULL;
    {
        int want = g_ms_banned ? 0 : afx_msaa_want();
        int cap  = 0;
        if (g_ms_banned)
            g_ms_why = g_ms_ban_why ? g_ms_ban_why
                     : "multisampling was retired earlier in this run";
        if (want > 1) {
            /* GL_MAX_SAMPLES is core in GL 3.0 / GLES 3 / WebGL 2 -- i.e. in
             * exactly the contexts that have the two entry points afx_ms_make
             * needs -- so it is only asked for once those are known present.
             * Asking a WebGL 1 context would be an INVALID_ENUM on the log the
             * web smoke gates against. */
            if (!p_glRenderbufferStorageMultisample || !p_glBlitFramebuffer) {
                g_ms_why = "this context has no glBlitFramebuffer/"
                           "glRenderbufferStorageMultisample (GL < 3.0 / "
                           "WebGL 1)";
            } else {
                glGetIntegerv(GL_MAX_SAMPLES, &cap);
                (void)glGetError();          /* never let a probe leak */
                if (cap > 1 && want > cap) want = cap;
                if (afx_ms_make(w, h, want)) g_samples = want;
                else g_ms_why = "the driver would not complete a multisampled "
                                "colour+depth FBO at this size";
            }
        }
        if (g_verbose)
            afx_say("msaa: wanted %d, max %d, got %d%s%s\n",
                    afx_msaa_want(), cap, g_samples,
                    g_ms_why ? " -- " : "", g_ms_why ? g_ms_why : "");
    }

    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* A REBUILD SAYS SO, in the same words as the first build.  afx_init()
     * announces build #1 and used to be the only line this chain ever printed
     * about its size, so a resize -- the one moment the chain is most likely
     * to lose a feature or fail outright -- was silent on the way through and
     * loud only on the way down.  One line per rebuild pairs with web/b3_web.c's
     * own "render resolution" announcement, and it is what
     * tools/web_resize_sweep.py gates each step on.  Resizes are rare (see
     * B3_WEB_RES_DEADBAND), so this is not chatter. */
    if (g_ready == 1)
        afx_say("chain ready %dx%d (depth %d-bit, msaa %d, %d taps) "
                "-- rebuilt from %dx%d\n",
                w, h, g_depth_bits, g_samples, g_taps, g_w, g_h);
    g_w = w; g_h = h;
    return 1;
}

/* A REBUILD THAT WILL NOT COMPLETE MUST COST ONE LINE, NOT ONE LINE A FRAME.
 *
 * afx_resize() failing used to leave g_ready at 1, so the next frame called it
 * again, failed again and PRINTED again -- for the rest of the run.  A user
 * whose viewport moved by one pixel got the legacy postfx path (already the
 * slower of the two at 1080p) plus a console line every 16 ms, and the second
 * of those is a performance defect in its own right: printf here is a proxied
 * console.log on the web, on the frame loop.
 *
 * So a failed rebuild walks a ladder, once, and stops:
 *
 *   1. try again with MSAA RETIRED.  This mirrors the resolve-refusal already
 *      in b3_afx_scene_done(): a driver that will not give a multisampled
 *      colour+depth pair at the new size costs multisampling, not the chain.
 *   2. still incomplete -> retire the CHAIN for the run (g_ready = 0), say so
 *      once, and let render_frame()'s legacy postfx path take the frame.
 *
 * Either way afx_init() never asks the driver the same losing question twice. */
static int afx_resize_or_retire(int w, int h)
{
    if (afx_resize(w, h)) return 1;

    if (!g_ms_banned && afx_msaa_want() > 1) {
        g_ms_banned  = 1;
        g_samples    = 0;
        g_ms_ban_why = "a rebuild at a new size would not complete with it on";
        g_ms_why     = g_ms_ban_why;
        if (afx_resize(w, h)) {
            afx_say("rebuild at %dx%d needed MSAA OFF -- multisampling is "
                    "retired for the rest of this run; the chain carries on "
                    "single-sampled\n", w, h);
            return 1;
        }
    }

    g_ready = 0;
    afx_say("rebuild at %dx%d could not be completed -- THE CHAIN IS RETIRED "
            "for the rest of this run and the legacy postfx path takes the "
            "frame (%s)\n", w, h, g_why ? g_why : "reason not recorded");
    return 0;
}

static int afx_init(int w, int h)
{
    const char *e;
    if (g_ready == 0) return 0;
    if (g_ready == 1) return afx_resize_or_retire(w, h);

    g_ready = 0;
    afx_read_knobs();
    /* THE PLATFORM DEFAULT: ON EVERYWHERE NOW, one switch (B3_AFX) overriding
     * either way.  The code was always identical on both -- this was a
     * default, not a second path.
     *
     * IT SHIPPED OFF ON THE WEB, and the two reasons were measured, not
     * guessed.  Both were gl4es', and both are gone with it:
     *
     *   * COST.  On the real GPU (headless Chrome -> ANGLE/Vulkan, RTX 3090)
     *     at 640x480 the chain cost about +2.3 ms of render_frame against a
     *     1.5 ms budget -- at eight passes the bill was gl4es' per-DRAW CPU
     *     overhead, not fill, which is why the same chain was ~0.05 ms on the
     *     desktop GL path.  Re-measured on the direct-WebGL link, same machine,
     *     same track, same session, median of the last eight racing frames:
     *
     *         render_frame  5.39 ms off  ->  5.76 ms on   = +0.37 ms
     *
     *     Both hold 60.0 fps.  The overhead was the compatibility layer.
     *
     *   * CORRECTNESS.  The web frame used to lose its sky -- pinned frame 900
     *     on US_C3_V1 came back with the gradient and cloud layers dark.  It
     *     does not any more.  Same pinned frame, direct WebGL:
     *
     *         sky rows 0-96   mean 88.2 -> 96.0, std 55.5 -> 58.4
     *         top fifth       94.6% of pixels above 16, max 255
     *
     *     i.e. the sky is intact and slightly BRIGHTER, which is the chain's
     *     exposure doing its job, and it keeps its structure rather than
     *     becoming a flat fill.  Frame-wide clipping goes 1.80% -> 3.11%,
     *     which is retail's x2 composite.
     *
     *     Note what this corrects: the depth attachment is STILL 16-bit here
     *     (the context is WebGL 1, so DEPTH_COMPONENT24 is not available -- it
     *     is core in WebGL 2, not in this one).  So the sky loss was never the
     *     depth precision it was attributed to; it was gl4es' draw path. */
    e = getenv("B3_AFX");
    if (e && *e) {
        /* Unconditional, and it records the reason: "B3_AFX=0 is set" is one
         * of the eight ways a user ends up with no effects, and it must be as
         * askable as a driver failure. (It was gated on B3_AFX_VERBOSE.) */
        if (atoi(e) == 0)
            return afx_decline("switched off by B3_AFX=%s", e);
    }
    /* g_verbose is set inside afx_gl_load(); read it here too so a decline
     * BEFORE that call still honours the env. */
    g_verbose = getenv("B3_AFX_VERBOSE") != NULL;
    if (!afx_gl_load()) return 0;
    if (!afx_build_all()) return 0;
    if (!afx_resize(w, h)) return 0;
    g_ready = 1;
    g_why   = NULL;
    afx_say("chain ready %dx%d (depth %d-bit, msaa %d, %d taps)\n",
            w, h, g_depth_bits, g_samples, g_taps);
    /* MSAA WAS ASKED FOR AND IS NOT RUNNING is its own line, unconditional and
     * separate from the one above, for the same reason afx_decline() exists: a
     * platform quietly losing a feature reads exactly like a platform that was
     * never asked. */
    if (!g_samples && afx_msaa_want() > 1)
        afx_say("MSAA %dx requested but NOT running -- %s\n",
                afx_msaa_want(),
                g_ms_why ? g_ms_why : "reason not recorded");
    return 1;
}

/* ---------------------------------------------------------------- drawing */

static void afx_bind_target(const AfxTarget *t)
{
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
}

static void afx_draw(unsigned prog, int aPos)
{
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, prog);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    if (aPos >= 0) {
        p_glEnableVertexAttribArray((unsigned)aPos);
        p_glVertexAttribPointer((unsigned)aPos, 2, GL_FLOAT, 0, 0, (void *)0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (aPos >= 0) p_glDisableVertexAttribArray((unsigned)aPos);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void afx_bind_tex(int unit, unsigned tex)
{
    /* THROUGH THE STATE SHADOW, not straight to p_glActiveTexture.  Every
     * texture bind is recorded against the ACTIVE UNIT, so a unit switch the
     * shadow cannot see makes it file this pass' binds in unit 0's slot -- and
     * the bind cache then filters a later bind as redundant when it is not.
     * burnout3_postfx.c had exactly this, through its own p_glActiveTexture,
     * and it was worth 3.6-4.5% of the pinned frame.  B3_STATE_AUDIT=1 reported
     * 41 disagreements a frame here (shadow 0, driver 329/330 -- this chain's
     * own colour attachments) until this call went through the shim. */
    b3r_gl_active_texture_via((void (*)(unsigned))p_glActiveTexture,
                              GL_TEXTURE0 + (unsigned)unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

/* The state every pass wants, and the state the engine wants back. Explicit
 * calls, not glPushAttrib -- the attribute stack is compatibility-profile only
 * and goes away with the fixed-function pipeline. */
static void afx_state_enter(void)
{
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void afx_state_leave(void)
{
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, 0);
    afx_bind_tex(2, 0);
    afx_bind_tex(1, 0);
    afx_bind_tex(0, 0);
    glDepthMask(GL_TRUE);
}

/* ==========================================================================
 * THE PHOTOREALISM PASSES, driven
 * ========================================================================== */

static void afx_u_mat(int loc, const float m[16])
{
    if (loc >= 0 && p_glUniformMatrix4fv) p_glUniformMatrix4fv(loc, 1, 0, m);
}
static void afx_u3(int loc, const float v0, const float v1, const float v2)
{
    if (loc >= 0 && p_glUniform3f) p_glUniform3f(loc, v0, v1, v2);
}

/* THE SKY CUT.  Everything depth-driven needs one number: the depth value at
 * which "this pixel is the sky dome and not the world" becomes true.  The dome
 * is drawn with depth WRITES OFF (FUN_00032580 [C]), so sky pixels keep the
 * cleared depth of 1.0 exactly -- but a 16-bit depth attachment quantises, and
 * the far plane is 10 km, so the cut sits a hair under 1 rather than at it. */
static float afx_photo_sky_cut(void)
{
    return (g_depth_bits >= 24) ? 0.99999f : 0.9995f;
}

/* Every photorealism pass wants the same four camera uniforms; this is the one
 * place they are uploaded, so a new pass cannot get the reconstruction subtly
 * wrong by spelling it differently. */
static void afx_photo_cam(const AfxProg *P)
{
    afx_u_mat(P->uInvVP, g_pcam.inv_vp);
    afx_u_mat(P->uVP,    g_pcam.vp);
    if (P->uCam >= 0)
        p_glUniform4f(P->uCam, g_pcam.eye[0], g_pcam.eye[1], g_pcam.eye[2],
                      g_pcam.far_z);
    if (P->uNF >= 0)
        p_glUniform4f(P->uNF, g_pcam.near_z, g_pcam.far_z,
                      afx_photo_sky_cut(),
                      (float)g_t[T_SCENE].w / (float)g_t[T_SCENE].h);
}

/* Where the sun lands on screen, and how much of it is on screen at all.
 * A directional light is a point at infinity, so it projects as a w = 0
 * homogeneous vector: `vp * vec4(dir, 0)`.  A non-positive w means the sun is
 * behind the camera and the shafts are simply not drawn. */
static int afx_photo_sun_uv(float out_uv[2], float *out_weight)
{
    const float *M = g_pcam.vp;
    float x, y, w, u, v, over = 0.0f, t;
    if (!g_sun_have) return 0;
    /* column-major mat4 times (dir, 0) */
    x = M[0] * g_sun_dir[0] + M[4] * g_sun_dir[1] + M[8]  * g_sun_dir[2];
    y = M[1] * g_sun_dir[0] + M[5] * g_sun_dir[1] + M[9]  * g_sun_dir[2];
    w = M[3] * g_sun_dir[0] + M[7] * g_sun_dir[1] + M[11] * g_sun_dir[2];
    if (w <= 1e-6f) return 0;                    /* the sun is behind us */
    u = (x / w) * 0.5f + 0.5f;
    v = (y / w) * 0.5f + 0.5f;
    t = -u;            if (t > over) over = t;
    t = u - 1.0f;      if (t > over) over = t;
    t = -v;            if (t > over) over = t;
    t = v - 1.0f;      if (t > over) over = t;
    if (over >= g_pk.gr_margin) return 0;
    out_uv[0] = u;
    out_uv[1] = v;
    *out_weight = 1.0f - over / (g_pk.gr_margin > 1e-4f ? g_pk.gr_margin : 1.0f);
    return 1;
}

/* Tiers 2, 4, 5 and 3, in one deferred pass over the finished scene, plus the
 * half-resolution buffers two of them need first.  Returns the texture the
 * rest of the chain should treat as "the scene": T_LIT when the pass ran,
 * T_SCENE when it did not, so every caller downstream is unchanged. */
/* the pass B3_PHOTO_SH_DUMP photographs -- the same 1-based counter, and the
 * same env, burnout3_render.c keys the map itself to. */
static long afx_photo_dump_pass(void)
{
    static long want = -1;
    if (want < 0) {
        const char *e = getenv("B3_PHOTO_SH_DUMP_AT");
        want = (e && *e) ? atol(e) : 1;
        if (want < 1) want = 1;
    }
    return want;
}

static unsigned afx_photo_surface(int w, int h)
{
    unsigned scene = g_t[T_SCENE].tex;
    if (!afx_photo_wants_light() || !g_pcam_frame || !g_t[T_LIT].fbo)
        return scene;

    /* B3_PHOTO_SH_DUMP's OTHER HALF: the scene depth this pass reconstructs
     * world positions from, and the camera it reconstructs them with.  A
     * shadow map alone cannot answer "is the lookup landing where the pixel
     * actually is" -- that question needs the receiver as well as the
     * occluder, and this is the receiver.  Same pass number as the map, so the
     * two files are the same frame.  DIAGNOSTIC; costs nothing when unset. */
    {
        const char *dp = getenv("B3_PHOTO_SH_DUMP");
        static long pass = 0;
        pass++;
        if (dp && *dp && g_depth_is_tex && pass == afx_photo_dump_pass()) {
            float *px = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
            char path[512];
            if (px) {
                p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
                glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, px);
                snprintf(path, sizeof path, "%s.depth.pgm", dp);
                FILE *f = fopen(path, "wb");
                if (f) {
                    unsigned short *row =
                        (unsigned short *)malloc((size_t)w * sizeof(short));
                    fprintf(f, "P5\n%d %d\n65535\n", w, h);
                    if (row) {
                        int y, x;
                        for (y = h - 1; y >= 0; y--) {
                            for (x = 0; x < w; x++) {
                                float v = px[(size_t)y * w + x];
                                unsigned u = (unsigned)(v * 65535.0f + 0.5f);
                                if (u > 65535u) u = 65535u;
                                row[x] = (unsigned short)
                                         ((u >> 8) | ((u & 0xFFu) << 8));
                            }
                            fwrite(row, 2, (size_t)w, f);
                        }
                        free(row);
                    }
                    fclose(f);
                }
                snprintf(path, sizeof path, "%s.cam.txt", dp);
                f = fopen(path, "w");
                if (f) {
                    int k;
                    fprintf(f, "size %d %d\nnear %.9g\nfar %.9g\n", w, h,
                            g_pcam.near_z, g_pcam.far_z);
                    fprintf(f, "eye %.9g %.9g %.9g\n", g_pcam.eye[0],
                            g_pcam.eye[1], g_pcam.eye[2]);
                    fprintf(f, "aosize %d %d\nfullsize %d %d\n",
                            g_t[T_AO].w, g_t[T_AO].h,
                            g_t[T_SCENE].w, g_t[T_SCENE].h);
                    fprintf(f, "vp");
                    for (k = 0; k < 16; k++)
                        fprintf(f, " %.9g", g_pcam.vp[k]);
                    fprintf(f, "\ninvvp");
                    for (k = 0; k < 16; k++)
                        fprintf(f, " %.9g", g_pcam.inv_vp[k]);
                    fprintf(f, "\nshvp");
                    for (k = 0; k < 16; k++)
                        fprintf(f, " %.9g", g_shadow_vp[k]);
                    fprintf(f, "\nshsize %d\nnormoff %.9g\n",
                            g_shadow_size,
                            (2.0f * g_pk.sh_extent
                             / (float)(g_shadow_size > 0 ? g_shadow_size
                                                         : g_pk.sh_size))
                            * g_pk.sh_normoff);
                    fclose(f);
                }
                free(px);
                afx_say("PHOTO: scene depth + camera @pass %ld -> %s.depth.pgm"
                        " / %s.cam.txt\n", pass, dp, dp);
            }
        }
    }

    /* ---- SSAO, at half resolution, then blurred bilaterally in place ---- */
    if (g_photo_live[B3_PHOTO_FX_SSAO] && g_p_ao.prog && g_t[T_AO].fbo) {
        afx_bind_target(&g_t[T_AO]);
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_ao.prog);
        afx_bind_tex(1, g_depth_tex);
        afx_photo_cam(&g_p_ao);
        /* ---- THE PASS' OWN TEXEL, not the scene's, and the difference is the
         * near-field blanket.
         *
         * This pass rasterises at HALF resolution, and every use of `uTexel`
         * inside it is a step to a NEIGHBOURING PIXEL OF ITS OWN GRID: the four
         * depth taps the surface normal is reconstructed from, and the floor
         * under the tap radius.  Handing it the scene's full-resolution texel
         * halves both.  The floor is harmless; the normal is not.
         *
         * From a half-res texel centre, a half-res-wide step lands on the next
         * pixel this pass will shade, and `ex` is a real world vector.  A
         * full-res step lands INSIDE the same half-res pixel, and on a surface
         * whose depth barely changes across the screen row -- a road, which is
         * most of a racing frame -- `ex` collapses to the difference between
         * two samples of the same tarmac.  `cross(ex, ey)` then has no
         * horizontal axis to work with and comes out wherever the noise points.
         *
         * MEASURED on US_C1_V1 frame 655 at 2048x1536, over the road band 6-40
         * m out, on the half-res grid the pass actually runs on: with the
         * scene's texel the reconstructed normal was UP (N.y > 0.95) on 47.2%
         * of it and its MEDIAN N.y was 0.031 -- horizontal, on flat road.  With
         * this pass' own texel, 64.1% and a median of 0.991.
         *
         * A horizontal normal on a road is not a small error.  It turns the
         * sampling hemisphere on its side, so every tap up or down the road
         * sits far above that bogus tangent plane, every one of them scores
         * near-full occlusion, and the pixel goes dark -- uniformly, across the
         * whole near field, which is exactly the blanket a player reported. */
        if (g_p_ao.uTexel >= 0)
            p_glUniform2f(g_p_ao.uTexel, 1.0f / (float)g_t[T_AO].w,
                          1.0f / (float)g_t[T_AO].h);
        if (g_p_ao.uAO >= 0)
            p_glUniform4f(g_p_ao.uAO, g_pk.ao_radius, g_pk.ao_bias,
                          g_pk.ao_power, g_pk.ao_maxdist);
        if (g_p_ao.uAOAmt >= 0)
            p_glUniform4f(g_p_ao.uAOAmt, g_pk.ao_strength, g_pk.ao_gain,
                          g_pk.ao_srcap, g_pk.ao_angbias);
        afx_draw(g_p_ao.prog, g_p_ao.aPos);

        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_aoblur.prog);
        afx_photo_cam(&g_p_aoblur);
        afx_bind_target(&g_t[T_AO_B]);
        afx_bind_tex(0, g_t[T_AO].tex);
        afx_bind_tex(1, g_depth_tex);
        if (g_p_aoblur.uDir >= 0)
            p_glUniform2f(g_p_aoblur.uDir, 1.0f / (float)g_t[T_AO].w, 0.0f);
        afx_draw(g_p_aoblur.prog, g_p_aoblur.aPos);

        afx_bind_target(&g_t[T_AO]);
        afx_bind_tex(0, g_t[T_AO_B].tex);
        if (g_p_aoblur.uDir >= 0)
            p_glUniform2f(g_p_aoblur.uDir, 0.0f, 1.0f / (float)g_t[T_AO_B].h);
        afx_draw(g_p_aoblur.prog, g_p_aoblur.aPos);

        /* B3_PHOTO_SH_DUMP's third file: the finished occlusion buffer, so the
         * pass can be checked against its own source instead of against the
         * frame it ends up multiplying. */
        {
            const char *dp = getenv("B3_PHOTO_SH_DUMP");
            static long pass = 0;
            pass++;
            if (dp && *dp && pass == afx_photo_dump_pass()) {
                int aw = g_t[T_AO].w, ah = g_t[T_AO].h;
                unsigned char *px =
                    (unsigned char *)malloc((size_t)aw * (size_t)ah * 4);
                char path[512];
                if (px) {
                    glReadPixels(0, 0, aw, ah, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    snprintf(path, sizeof path, "%s.ao.pgm", dp);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        int y, x;
                        fprintf(f, "P5\n%d %d\n255\n", aw, ah);
                        for (y = ah - 1; y >= 0; y--)
                            for (x = 0; x < aw; x++)
                                fputc(px[((size_t)y * aw + x) * 4], f);
                        fclose(f);
                        afx_say("PHOTO: occlusion buffer %dx%d -> %s\n",
                                aw, ah, path);
                    }
                    free(px);
                }
            }
        }
    }

    /* ---- SSR, at half resolution, under the track's own shine mask ------ */
    if (g_photo_live[B3_PHOTO_FX_SSR] && g_p_ssr.prog && g_t[T_SSR].fbo) {
        /* the mask is a GEOMETRY pass, so it owns a framebuffer for the length
         * of one draw and hands it back; bind our own target after it */
        unsigned mask = b3r_shine_mask_render(g_depth_tex, w, h);
        {
            static int said;
            if (!said && getenv("B3_PHOTO_VERBOSE")) {
                said = 1;
                afx_say("PHOTO ssr: shine mask texture %u\n", mask);
            }
        }
        if (!mask) {
            /* ZERO THE STRENGTH, DO NOT CLEAR THE LIVE FLAG, and the
             * difference is a large dark frame.
             *
             * `g_photo_live[]` is what the deferred shader was ASSEMBLED from,
             * once, at build time -- so clearing it here does not remove the
             * SSR block from a program that is already compiled.  What it
             * removes is the block's TEXTURE BIND and its uniform upload, and
             * the block then runs anyway: it samples an unbound sampler2D,
             * which returns (0,0,0,1) by the spec, and mixes toward it by the
             * uSsrAmt the program is still holding from the last frame that
             * did upload one.  That is `mix(scene, black, 0.85)` over
             * everything the depth cut does not exclude -- the whole world
             * dropped to a sixth of its brightness, from a stand-down that
             * meant to cost one effect.
             *
             * The shadow already had the right shape for this a tier earlier
             * (`g_shadow_tex ? sh_strength : 0`): keep the effect live so its
             * bind and its upload keep happening, and make the number it
             * uploads zero.  Then the block is a multiply by zero, which is
             * what "off" is supposed to mean in an assembled shader. */
            if (g_ssr_ok) {
                g_ssr_ok = 0;
                afx_say("PHOTO: no shine mask on this track -- the SSR term "
                        "is zeroed (its block stays in the shader, so the "
                        "bind and the upload have to stay too)\n");
            }
        } else {
            g_ssr_ok = 1;
        afx_bind_target(&g_t[T_SSR]);
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_ssr.prog);
        afx_bind_tex(0, scene);
        afx_bind_tex(1, g_depth_tex);
        afx_bind_tex(2, mask);
        afx_photo_cam(&g_p_ssr);
        if (g_p_ssr.uTexel >= 0)
            p_glUniform2f(g_p_ssr.uTexel, 1.0f / (float)w, 1.0f / (float)h);
        if (g_p_ssr.uSsr >= 0)
            p_glUniform4f(g_p_ssr.uSsr, g_pk.ssr_stride, g_pk.ssr_thick,
                          g_pk.ssr_edge, g_pk.ssr_fresnel);
        afx_draw(g_p_ssr.prog, g_p_ssr.aPos);
        }
    }

    /* ---- B3_PHOTO_DEBUG=ao: put the occlusion buffer ON THE SCREEN ------
     *
     * Every one of these effects is an intermediate buffer that only ever
     * reaches the frame multiplied by something else, and "the SSAO looks
     * wrong" is unanswerable from the composited image -- the first tuning
     * pass here spent its time arguing about whether a banding artefact was
     * the occlusion, the bilateral blur or the road's own geometry, and the
     * only thing that settled it was looking at the buffer.
     *
     * The blur program with a ZERO direction is a weighted copy, so this
     * needs no shader of its own. */
    if (g_ao_debug && g_photo_live[B3_PHOTO_FX_SSAO] && g_p_aoblur.prog) {
        afx_bind_target(&g_t[T_LIT]);
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_aoblur.prog);
        afx_photo_cam(&g_p_aoblur);
        afx_bind_tex(0, g_t[T_AO].tex);
        afx_bind_tex(1, g_depth_tex);
        if (g_p_aoblur.uDir >= 0) p_glUniform2f(g_p_aoblur.uDir, 0.0f, 0.0f);
        afx_draw(g_p_aoblur.prog, g_p_aoblur.aPos);
        return g_t[T_LIT].tex;
    }

    /* ---- the deferred pass ---------------------------------------------- */
    afx_rt_sync();
    afx_bind_target(&g_t[T_LIT]);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_light.prog);
    afx_bind_tex(0, scene);
    afx_bind_tex(1, g_depth_tex);
    if (g_photo_live[B3_PHOTO_FX_SSAO]) afx_bind_tex(2, g_t[T_AO].tex);
    if (g_photo_live[B3_PHOTO_FX_SHADOW]) afx_bind_tex(3, g_shadow_tex);
    if (g_photo_live[B3_PHOTO_FX_SSR])  afx_bind_tex(4, g_t[T_SSR].tex);
    afx_photo_cam(&g_p_light);
    if (g_p_light.uTexel >= 0)
        p_glUniform2f(g_p_light.uTexel, 1.0f / (float)w, 1.0f / (float)h);
    afx_u3(g_p_light.uSunDir, g_sun_dir[0], g_sun_dir[1], g_sun_dir[2]);
    afx_u3(g_p_light.uSunCol, g_sun_rgb[0], g_sun_rgb[1], g_sun_rgb[2]);
    if (g_p_light.uAOLit >= 0) p_glUniform1f(g_p_light.uAOLit, g_pk.ao_litbias);
    if (g_photo_live[B3_PHOTO_FX_SHADOW]) {
        int sz = g_shadow_size > 0 ? g_shadow_size : g_pk.sh_size;
        afx_u_mat(g_p_light.uShVP, g_shadow_vp);
        if (g_p_light.uSh >= 0)
            p_glUniform4f(g_p_light.uSh,
                          /* a frame whose shadow pass did not run this time
                           * asks for zero strength rather than for a stale
                           * matrix's worth of wrong shadow */
                          /* ...unless tier 4r is the one answering: the
                           * ray needs no map, so the map's absence is not a
                           * reason to stand the SUN's shadow down. */
                          (g_rt_ok || g_shadow_tex) ? g_pk.sh_strength : 0.0f,
                          g_pk.sh_bias, g_pk.sh_slope, 1.0f / (float)sz);
        if (g_p_light.uSh2 >= 0)
            p_glUniform4f(g_p_light.uSh2, g_pk.sh_fade, g_pk.sh_litbias,
                          g_pk.sh_cool,
                          /* the normal offset in WORLD METRES: the shadow
                           * map's own texel footprint times a small factor,
                           * so it scales with the map size and the box the
                           * operator chose rather than being a magic 0.5 */
                          (2.0f * g_pk.sh_extent / (float)sz) * g_pk.sh_normoff);
        afx_u3(g_p_light.uShCool, B3_PHOTO_SH_COOL_R, B3_PHOTO_SH_COOL_G,
               B3_PHOTO_SH_COOL_B);
        if (g_p_light.uSunD >= 0)
            p_glUniform4f(g_p_light.uSunD, g_pk.sun_direct, g_pk.sun_ref,
                          g_pk.sun_lo, g_pk.sun_hi);
    }
    /* tier 4r: the two data textures and the ray's own four numbers.  It
     * is keyed on g_rt_built -- what the SHADER has in it -- rather than on
     * what the user wants, because a bind whose block is not compiled is a
     * bind onto a sampler that does not exist. */
    if (g_rt_built) {
        B3RtKnobs rk;
        float tw = (float)b3_rt_tex_w();
        b3_rt_knobs(&rk);
        afx_bind_tex(5, g_rt_node_tex);
        afx_bind_tex(6, g_rt_tri_tex);
        if (g_p_light.uRtN >= 0)
            p_glUniform4f(g_p_light.uRtN, tw, 1.0f / tw,
                          1.0f / (float)(b3_rt_node_rows() > 0
                                         ? b3_rt_node_rows() : 1),
                          (float)b3_rt_node_count());
        if (g_p_light.uRtT >= 0)
            p_glUniform4f(g_p_light.uRtT, tw, 1.0f / tw,
                          1.0f / (float)(b3_rt_tri_rows() > 0
                                         ? b3_rt_tri_rows() : 1), 0.0f);
        if (g_p_light.uRt >= 0)
            p_glUniform4f(g_p_light.uRt, rk.range, rk.origin,
                          /* tan of the sun's angular RADIUS: the whole of
                           * the soft edge, and the reason the penumbra
                           * widens with the occluder's distance without
                           * anything having to measure that distance */
                          (float)tan((double)rk.sun_deg * 3.14159265358979
                                     / 180.0),
                          /* a world that is not there stands the term down
                           * by uploading a ZERO, never by skipping a bind --
                           * the same rule the SSR note above spells out */
                          g_rt_ok ? rk.strength : 0.0f);
        /* THE FAR FIELD's three -- see THE FAR FIELD in src/burnout3_rt.h
         * for what each one corrects and tools/rt_shimmer.py for the
         * measurement that chose them.  Each is a BLEND from the old
         * behaviour, so zeroing its env restores tier 4r exactly as it
         * shipped and puts the shimmer back, which is what makes the fix a
         * measurement rather than a story. */
        if (g_p_light.uRt2 >= 0)
            p_glUniform4f(g_p_light.uRt2, rk.dist_bias, rk.slope,
                          rk.graze, 0.0f);
        /* tier 4rc: the cars' one texture and their five arrays.  Keyed on
         * g_rtc_built -- what the SHADER has in it -- for the same reason the
         * block above is: a bind onto a sampler that does not exist is not a
         * no-op, it is an error the driver reports on the next draw. */
        if (g_rtc_built) {
            afx_bind_tex(7, g_rtc_tex);
            if (g_p_light.uRtC >= 0)
                p_glUniform4f(g_p_light.uRtC, tw, 1.0f / tw,
                              1.0f / (float)(b3_rt_car_rows() > 0
                                             ? b3_rt_car_rows() : 1),
                              (float)b3_rt_car_tri_base());
            if (p_glUniform4fv) {
                /* THE WHOLE ARRAY, not g_rtc_live of it -- the loop bound is
                 * the array's length and a slot this frame did not fill has
                 * been zeroed by b3_afx_rt_cars_begin().  Same rule, same
                 * reason, as tier 7's lights. */
                if (g_p_light.uCarS >= 0)
                    p_glUniform4fv(g_p_light.uCarS, g_rtc_n, g_rtc_s);
                if (g_p_light.uCarX >= 0)
                    p_glUniform4fv(g_p_light.uCarX, g_rtc_n, g_rtc_x);
                if (g_p_light.uCarY >= 0)
                    p_glUniform4fv(g_p_light.uCarY, g_rtc_n, g_rtc_y);
                if (g_p_light.uCarZ >= 0)
                    p_glUniform4fv(g_p_light.uCarZ, g_rtc_n, g_rtc_z);
                if (g_p_light.uCarN >= 0)
                    p_glUniform4fv(g_p_light.uCarN, g_rtc_n, g_rtc_node);
            }
        }
    }
    /* with the normal, not with the shadow -- see AFX_LIGHT_U_NORMAL */
    if (g_p_light.uSunN >= 0)
        p_glUniform2f(g_p_light.uSunN, g_pk.sun_nconf, g_pk.sun_nband);
    if (g_photo_live[B3_PHOTO_FX_SSR] && g_p_light.uSsrAmt >= 0)
        p_glUniform1f(g_p_light.uSsrAmt,
                      g_ssr_ok ? g_pk.ssr_strength : 0.0f);
    if (g_photo_live[B3_PHOTO_FX_ATMOS]) {
        if (g_p_light.uAtm >= 0)
            p_glUniform4f(g_p_light.uAtm, g_pk.atm_density, g_pk.atm_height,
                          g_pk.atm_strength, g_pk.atm_near);
        afx_u3(g_p_light.uAtm2, g_pk.atm_sungain, g_pk.atm_sunpow,
               g_pk.atm_skylean);
        {
            float m[3], c1[3], s1[3], c2[3], s2[3];
            afx_atm_sky_fit(m, c1, s1, c2, s2);
            afx_u3(g_p_light.uAtmSky,   m[0],  m[1],  m[2]);
            afx_u3(g_p_light.uAtmSkyC,  c1[0], c1[1], c1[2]);
            afx_u3(g_p_light.uAtmSkyS,  s1[0], s1[1], s1[2]);
            afx_u3(g_p_light.uAtmSkyC2, c2[0], c2[1], c2[2]);
            afx_u3(g_p_light.uAtmSkyS2, s2[0], s2[1], s2[2]);
        }
    }
    if (g_photo_live[B3_PHOTO_FX_LIGHTS]) {
        /* THE WHOLE ARRAY, not g_light_n of it, and the setter's memset is
         * why: the loop bound is the budget, so a slot the caller did not
         * fill this frame still runs and must be uploaded as zero rather than
         * left holding a lamp from the street before last. */
        if (g_p_light.uLightP >= 0 && p_glUniform4fv)
            p_glUniform4fv(g_p_light.uLightP, g_pk.li_n, g_light_p);
        if (g_p_light.uLightC >= 0 && p_glUniform4fv)
            p_glUniform4fv(g_p_light.uLightC, g_pk.li_n, g_light_c);
        if (g_p_light.uLightD >= 0 && p_glUniform4fv)
            p_glUniform4fv(g_p_light.uLightD, g_pk.li_n, g_light_d);
        if (g_p_light.uLightK >= 0) {
            /* DAY IS SUBTLE, DUSK SINGS, and the number in between is the
             * track's own hour -- see B3_PHOTO_LIGHT_DUSK.  A lamp at noon is
             * a lamp that is on and losing, which is exactly what mixing the
             * day gain toward 1 does. */
            float g = g_pk.li_gain
                    * (g_pk.li_day + (1.0f - g_pk.li_day) * g_light_dusk);
            p_glUniform2f(g_p_light.uLightK, g, g_pk.li_litbias);
        }
        if (g_p_light.uHeadK >= 0 && p_glUniform4f) {
            /* THE BEAMS' OWN CURVE, stated once and here.  Same shape as the
             * lamps' above -- a floor plus the track's own hour -- but its own
             * floor, its own gain and its own lit-bias, which is what lets a
             * headlight read on sunlit tarmac while a streetlight at noon goes
             * on quietly losing to the sun.  The caller used to divide the
             * lamps' gain out of the per-light COLOUR to fake this; it does
             * not any more. */
            float hg = g_pk.hd_gain
                     * (g_pk.hd_day + (1.0f - g_pk.hd_day) * g_light_dusk);
            p_glUniform4f(g_p_light.uHeadK, hg, g_pk.hd_lit,
                          g_pk.hd_wrap, g_pk.hd_hot);
        }
    }
    afx_draw(g_p_light.prog, g_p_light.aPos);
    afx_bind_tex(4, 0);
    afx_bind_tex(3, 0);
    afx_bind_tex(2, 0);
    return g_t[T_LIT].tex;
}

/* Tier 6, off the quarter-resolution prefilter the bloom already made.
 * Returns the texture the composite should add, or 0 for "no shafts this
 * frame" -- which is the ordinary answer whenever the sun is behind the car. */
static unsigned afx_photo_godrays(void)
{
    float uv[2] = { 0.0f, 0.0f }, weight = 0.0f;
    int got;
    if (!g_photo_live[B3_PHOTO_FX_GODRAY] || !g_p_god.prog
        || !g_t[T_GOD_A].fbo || !g_pcam_frame)
        return 0;
    got = afx_photo_sun_uv(uv, &weight);
    /* WHERE IS THE SUN.  "The god rays do nothing" has two very different
     * causes -- the sun is behind the car (correct, and the ordinary case on
     * a good half of any lap) or the projection is wrong -- and no way at all
     * to tell them apart from the outside.  B3_PHOTO_VERBOSE=1 prints the
     * answer every 120 frames, which is also how you FIND a frame where the
     * shafts are worth photographing. */
    if (getenv("B3_PHOTO_VERBOSE")) {
        static int n;
        if ((n++ % 120) == 0)
            afx_say("PHOTO godray f%d: sun (%.3f %.3f %.3f) -> %s "
                    "uv (%.3f %.3f) weight %.3f\n", n - 1,
                    g_sun_dir[0], g_sun_dir[1], g_sun_dir[2],
                    got ? "ON SCREEN" : "off/behind", uv[0], uv[1], weight);
    }
    if (!got) return 0;

    afx_bind_target(&g_t[T_GOD_A]);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_godsrc.prog);
    afx_bind_tex(0, g_t[T_QUARTER].tex);
    afx_bind_tex(1, g_depth_tex);
    afx_photo_cam(&g_p_godsrc);
    if (g_p_godsrc.uSun >= 0)
        p_glUniform4f(g_p_godsrc.uSun, uv[0], uv[1], g_pk.gr_radius, weight);
    afx_draw(g_p_godsrc.prog, g_p_godsrc.aPos);

    afx_bind_target(&g_t[T_GOD_B]);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_god.prog);
    afx_bind_tex(0, g_t[T_GOD_A].tex);
    if (g_p_god.uSun >= 0)
        p_glUniform4f(g_p_god.uSun, uv[0], uv[1], 0.0f, weight);
    if (g_p_god.uGod >= 0)
        p_glUniform4f(g_p_god.uGod, g_pk.gr_density, g_pk.gr_decay,
                      g_pk.gr_weight, 0.0f);
    afx_draw(g_p_god.prog, g_p_god.aPos);
    return g_t[T_GOD_B].tex;
}

/* ------------------------------------------------------------- the hooks */

int b3_afx_frame_begin(int w, int h)
{
    if (w <= 0 || h <= 0) {
        /* The one decline that used to be genuinely silent. It happens when
         * the window is minimised, so it must not spam -- but it must be
         * askable, which is what g_why is for. */
        g_why = "drawable is 0 x 0 (window minimised?)";
        return 0;
    }
    if (!afx_init(w, h)) return 0;

    /* Draw the scene into the multisampled pair when we have one, otherwise
     * straight into the scene texture. Everything downstream reads the scene
     * TEXTURE either way. */
    if (g_samples) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_ms_fbo);
    } else {
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
    }
    glViewport(0, 0, w, h);
    /* Depth ON and writable before the world draws. The passes below run with
     * depth testing off and the old chain used to restore it by popping a
     * glPushAttrib; this chain does not use the attribute stack (it goes away
     * with the fixed-function pipeline), so the scene's entry state is set
     * here explicitly instead of inherited from whatever the last pass left. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g_active = 1;
    return 1;
}

void b3_afx_rebind_scene(int w, int h)
{
    if (!g_active) return;
    /* the same choice b3_afx_frame_begin makes: the multisampled pair when we
     * have one, the scene texture's own FBO otherwise.  NO CLEAR -- see the
     * contract in the header. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_samples ? g_ms_fbo : g_t[T_SCENE].fbo);
    glViewport(0, 0, w, h);
    /* THE SAME TWO LINES b3_afx_frame_begin SETS, and raw for the same reason
     * it sets them raw: this is the scene's ENTRY state, and the point of
     * having it in two places is that the world after a shadow pass starts
     * from exactly where the world without one starts.  A caller that runs a
     * pass of its own between the clear and the first draw has to hand the
     * frame back in the condition it borrowed it in, and this is that. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void b3_afx_scene_done(int w, int h, const B3AfxInputs *in)
{
    float crash, s, alpha, r0, desat, vig, boost, bloom_amt;
    unsigned scene_tex, god_tex;
    B3AfxInputs zero;
#ifdef __EMSCRIPTEN__
    double t_prof = b3_web_now_ms();
#endif

    if (!g_active) return;
    if (!in) { memset(&zero, 0, sizeof zero); zero.divisor = 1; in = &zero; }

    /* RESOLVE MSAA INTO THE SCENE TEXTURE.  One GPU-side blit, colour only --
     * the depth buffer dies with the frame and no pass below reads it.
     * GL_NEAREST because a multisample resolve requires the source and
     * destination rectangles to be the same size anyway, and a filter is only
     * meaningful when they are not.
     *
     * Everything after this point reads g_t[T_SCENE].tex, a single-sampled
     * texture: the two downsamples, the radial pass, the bright pass and the
     * composite all sample the RESOLVED image, and none of them can see the
     * multisample renderbuffer at all (it has no texture handle to bind).
     *
     * THE FIRST RESOLVE IS CHECKED.  See the note over afx_ms_make: the format
     * match this depends on is a spec argument, and a rejected blit is a silent
     * no-op that would leave the scene texture holding the clear colour. */
    if (g_samples) {
        /* THE DEPTH IS RESOLVED TOO when the photorealism layer is reading it.
         *
         * Without this the layer's depth texture is whatever was in it before
         * MSAA was turned on, i.e. nothing -- the world's depth went into the
         * MULTISAMPLED renderbuffer and never came out.  A depth resolve from a
         * multisampled read framebuffer to a single-sampled draw one is legal
         * on GL 3.0+ / GLES 3 / WebGL 2 with matching formats and a NEAREST
         * filter, which is exactly this pair, and it costs one extra bit in an
         * existing blit rather than a second call.
         *
         * It is CHECKED, on the same first-resolve glGetError the colour half
         * already had, because "legal with matching formats" is a spec
         * argument and a rejected blit is a silent no-op that would leave the
         * layer reading a cleared depth buffer -- i.e. a frame where every
         * pixel is sky, which is a very confusing bug to find from the
         * outside. */
        unsigned mask = GL_COLOR_BUFFER_BIT;
        if (g_depth_is_tex && afx_photo_wants_depth())
            mask |= GL_DEPTH_BUFFER_BIT;
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ms_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_t[T_SCENE].fbo);
        if (!g_ms_checked) (void)glGetError();     /* start from a clean slate */
        p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, mask, GL_NEAREST);
        if (!g_ms_checked) {
            unsigned err = glGetError();
            g_ms_checked = 1;
            if (err) {
                afx_say("MSAA RESOLVE REJECTED (glGetError 0x%04X) -- "
                        "multisampling off for the rest of this run; the "
                        "chain carries on single-sampled\n", err);
                g_samples    = 0;
                g_ms_banned  = 1;
                g_ms_ban_why = "the resolve blit was rejected by the driver";
                g_ms_why     = g_ms_ban_why;
            }
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    afx_read_knobs();
    if (g_force_mph >= 0.0f || g_force_boost >= 0.0f || g_force_div > 0) {
        zero = *in;
        if (g_force_mph   >= 0.0f) zero.speed_mph  = g_force_mph;
        if (g_force_boost >= 0.0f) zero.boost_ramp = g_force_boost;
        if (g_force_div    >  0)   zero.divisor    = g_force_div;
        in = &zero;
    }
    crash = b3_afx_crash_weight(in->divisor) * g_k_crash;
    if (crash < 0.0f) crash = 0.0f;
    if (crash > 1.0f) crash = 1.0f;
    s     = b3_afx_blur_s(in->speed_mph, in->boost_ramp, in->divisor);
    /* the knob scales the whole smear, crash term included */
    s    *= g_k_blur;
    alpha = b3_postfx_present_alpha(s);   /* [C] min(s, 2) * 0.5 */
    r0    = b3_afx_mask_r0(crash);
    desat = B3_AFX_CRASH_DESAT * crash;
    vig   = B3_AFX_CRASH_VIGNETTE * crash;

    /* the recovered FOV ramp's shape, 1 - (r - 1)^2, reused for the bloom's
     * boost lift -- see B3_AFX_BLOOM_BOOST. Peaks at r == 1. */
    boost = in->boost_ramp - 1.0f;
    boost = 1.0f - boost * boost;
    if (boost < 0.0f) boost = 0.0f;
    if (boost > 1.0f) boost = 1.0f;
    bloom_amt = (g_k_bloom > 0.0f)
              ? B3_AFX_BLOOM_GAIN * g_k_bloom
                * (1.0f + B3_AFX_BLOOM_BOOST * boost)
              : 0.0f;

    afx_state_enter();

    /* 0. THE PHOTOREALISM LAYER'S SURFACE PASSES — occlusion, shadow,
     * reflection and haze, in one deferred draw over the finished scene.  It
     * hands back the texture everything below should treat as "the scene",
     * which is T_SCENE unchanged when the layer is off or standing down: with
     * B3_PHOTO=0 this call does nothing and returns the same texture the next
     * line used to read unconditionally.
     *
     * IT RUNS FIRST, BEFORE THE PREFILTER, and that ordering is the point: the
     * blur smears the LIT frame and the bloom blooms the LIT frame, so a
     * sunlit wall next to its own shadow blooms and a shadowed one does not.
     * Lighting after the prefilter would have been one target cheaper and
     * would have put a sharp shadow edge inside a blurred image. */
    scene_tex = afx_photo_surface(w, h);

    /* 1-2. the prefilter: two halvings, full -> 1/2 -> 1/4 */
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_down.prog);
    if (g_p_down.uTex >= 0) p_glUniform1i(g_p_down.uTex, 0);
    afx_bind_target(&g_t[T_HALF]);
    afx_bind_tex(0, scene_tex);
    if (g_p_down.uTexel >= 0)
        p_glUniform2f(g_p_down.uTexel, 1.0f / (float)g_t[T_SCENE].w,
                      1.0f / (float)g_t[T_SCENE].h);
    afx_draw(g_p_down.prog, g_p_down.aPos);

    afx_bind_target(&g_t[T_QUARTER]);
    afx_bind_tex(0, g_t[T_HALF].tex);
    if (g_p_down.uTexel >= 0)
        p_glUniform2f(g_p_down.uTexel, 1.0f / (float)g_t[T_HALF].w,
                      1.0f / (float)g_t[T_HALF].h);
    afx_draw(g_p_down.prog, g_p_down.aPos);

    /* 3. the radial smear, on the quarter -- 16 taps at 1/16 the cost */
    if (alpha > 0.002f) {
        afx_bind_target(&g_t[T_BLUR]);
        afx_bind_tex(0, g_t[T_QUARTER].tex);
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_radial.prog);
        if (g_p_radial.uTex >= 0) p_glUniform1i(g_p_radial.uTex, 0);
        if (g_p_radial.uMask >= 0)
            p_glUniform4f(g_p_radial.uMask, r0, B3_BLUR_MASK_POW,
                          B3_BLUR_ZOOM_A, 1.0f / (float)g_taps);
        afx_draw(g_p_radial.prog, g_p_radial.aPos);
    }

    /* 4. the bloom: bright-pass to 1/8, then a separable blur there */
    if (g_k_bloom > 0.0f) {
        afx_bind_target(&g_t[T_BLOOM_A]);
        afx_bind_tex(0, g_t[T_QUARTER].tex);
        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_bright.prog);
        if (g_p_bright.uTex >= 0) p_glUniform1i(g_p_bright.uTex, 0);
        if (g_p_bright.uTexel >= 0)
            p_glUniform2f(g_p_bright.uTexel, 1.0f / (float)g_t[T_QUARTER].w,
                          1.0f / (float)g_t[T_QUARTER].h);
        if (g_p_bright.uThreshold >= 0)
            p_glUniform1f(g_p_bright.uThreshold, B3_AFX_BLOOM_THRESHOLD);
        afx_draw(g_p_bright.prog, g_p_bright.aPos);

        b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_blur.prog);
        if (g_p_blur.uTex >= 0) p_glUniform1i(g_p_blur.uTex, 0);
        afx_bind_target(&g_t[T_BLOOM_B]);
        afx_bind_tex(0, g_t[T_BLOOM_A].tex);
        if (g_p_blur.uDir >= 0)
            p_glUniform2f(g_p_blur.uDir, 1.0f / (float)g_t[T_BLOOM_A].w, 0.0f);
        afx_draw(g_p_blur.prog, g_p_blur.aPos);

        afx_bind_target(&g_t[T_BLOOM_A]);
        afx_bind_tex(0, g_t[T_BLOOM_B].tex);
        if (g_p_blur.uDir >= 0)
            p_glUniform2f(g_p_blur.uDir, 0.0f, 1.0f / (float)g_t[T_BLOOM_B].h);
        afx_draw(g_p_blur.prog, g_p_blur.aPos);
    }

    /* 4b. TIER 6, the god rays, off the quarter-resolution prefilter the bloom
     * has just finished with. Zero when the sun is behind the camera. */
    god_tex = afx_photo_godrays();

    /* 5. the composite, into the UI target -- which stays bound for the HUD */
    afx_bind_target(&g_t[T_UI]);
    afx_bind_tex(0, scene_tex);
    afx_bind_tex(1, alpha > 0.002f ? g_t[T_BLUR].tex : 0);
    afx_bind_tex(2, g_k_bloom > 0.0f ? g_t[T_BLOOM_A].tex : 0);
    if (g_photo_live[B3_PHOTO_FX_GODRAY])  afx_bind_tex(3, god_tex);
    if (g_photo_live[B3_PHOTO_FX_TONEMAP]) afx_bind_tex(4, g_lut_tex);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_composite.prog);
    if (g_p_composite.uAmt >= 0)
        p_glUniform4f(g_p_composite.uAmt,
                      alpha > 0.002f ? alpha : 0.0f,
                      bloom_amt, desat, vig);
    if (g_p_composite.uExposure >= 0)
        p_glUniform1f(g_p_composite.uExposure, g_k_exposure);
    /* TIERS 1 AND 6's two numbers.  x is the shaft strength, and it is zero
     * whenever the sun did not project on screen -- so the god-ray add costs a
     * multiply by zero rather than a branch.  y is the tonemap's exposure
     * lift, which puts the mid-tone back where the recovered clamp had it now
     * that the shoulder is catching the top. */
    if (g_p_composite.uPhoto >= 0)
        p_glUniform4f(g_p_composite.uPhoto,
                      god_tex ? g_pk.gr_strength : 0.0f,
                      g_photo_live[B3_PHOTO_FX_TONEMAP]
                          ? g_pk.tm_exposure : 1.0f,
                      g_pk.tm_shoulder, 0.0f);
    afx_draw(g_p_composite.prog, g_p_composite.aPos);
    afx_bind_tex(4, 0);
    afx_bind_tex(3, 0);

    afx_state_leave();
    /* leave the UI target bound and the viewport full-size for the HUD */
    afx_bind_target(&g_t[T_UI]);
    glViewport(0, 0, w, h);
#ifdef __EMSCRIPTEN__
    b3_web_prof(B3_WEB_PROF_BLUR, b3_web_now_ms() - t_prof);
#endif
}

int b3_afx_frame_end(int w, int h)
{
#ifdef __EMSCRIPTEN__
    double t_prof = b3_web_now_ms();
#endif
    if (!g_active) return 0;
    g_active = 0;

    afx_state_enter();
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    afx_bind_tex(1, g_ramp_tex);
    afx_bind_tex(0, g_t[T_UI].tex);
    afx_draw(g_p_gamma.prog, g_p_gamma.aPos);
    afx_state_leave();
#ifdef __EMSCRIPTEN__
    b3_web_prof(B3_WEB_PROF_GAMMA, b3_web_now_ms() - t_prof);
#endif
    return 1;
}

void b3_afx_shutdown(void)
{
    int i;
    /* WAS `g_ready != 1`, which leaked the whole chain the moment
     * afx_resize_or_retire() could set g_ready to 0 with every object still
     * alive.  The real precondition is that the ENTRY POINTS were loaded --
     * glDelete* on a zero name is a defined no-op, so a half-built chain is
     * safe to walk. */
    if (g_gl_state != 1) return;
    for (i = 0; i < T_COUNT; i++) {
        if (g_t[i].fbo) p_glDeleteFramebuffers(1, &g_t[i].fbo);
        if (g_t[i].tex) glDeleteTextures(1, &g_t[i].tex);
        g_t[i].fbo = g_t[i].tex = 0;
    }
    if (g_depth_tex)   glDeleteTextures(1, &g_depth_tex);
    if (g_lut_tex)     glDeleteTextures(1, &g_lut_tex);
    g_depth_tex = g_lut_tex = 0;
    g_depth_is_tex = 0;
    g_photo_depth_ok = -1;
    g_pk_read = 0;
    if (g_depth_rb)    p_glDeleteRenderbuffers(1, &g_depth_rb);
    if (g_ms_color_rb) p_glDeleteRenderbuffers(1, &g_ms_color_rb);
    if (g_ms_depth_rb) p_glDeleteRenderbuffers(1, &g_ms_depth_rb);
    if (g_ms_fbo)      p_glDeleteFramebuffers(1, &g_ms_fbo);
    if (g_vbo)         p_glDeleteBuffers(1, &g_vbo);
    if (g_ramp_tex)    glDeleteTextures(1, &g_ramp_tex);
    g_depth_rb = g_ms_color_rb = g_ms_depth_rb = g_ms_fbo = 0;
    g_samples = g_ms_checked = g_ms_banned = 0;
    g_ms_why  = NULL;
    g_ms_ban_why = NULL;
    g_vbo = g_ramp_tex = 0;
    afx_rt_free();
    g_rt_uploaded_gen = 0;
    g_rt_built = 0;
    if (g_p_down.prog)      p_glDeleteProgram(g_p_down.prog);
    if (g_p_radial.prog)    p_glDeleteProgram(g_p_radial.prog);
    if (g_p_bright.prog)    p_glDeleteProgram(g_p_bright.prog);
    if (g_p_blur.prog)      p_glDeleteProgram(g_p_blur.prog);
    if (g_p_composite.prog) p_glDeleteProgram(g_p_composite.prog);
    if (g_p_gamma.prog)     p_glDeleteProgram(g_p_gamma.prog);
    if (g_p_ao.prog)        p_glDeleteProgram(g_p_ao.prog);
    if (g_p_aoblur.prog)    p_glDeleteProgram(g_p_aoblur.prog);
    if (g_p_ssr.prog)       p_glDeleteProgram(g_p_ssr.prog);
    if (g_p_godsrc.prog)    p_glDeleteProgram(g_p_godsrc.prog);
    if (g_p_god.prog)       p_glDeleteProgram(g_p_god.prog);
    if (g_p_light.prog)     p_glDeleteProgram(g_p_light.prog);
    memset(&g_p_down, 0, sizeof g_p_down);
    memset(&g_p_radial, 0, sizeof g_p_radial);
    memset(&g_p_bright, 0, sizeof g_p_bright);
    memset(&g_p_blur, 0, sizeof g_p_blur);
    memset(&g_p_composite, 0, sizeof g_p_composite);
    memset(&g_p_gamma, 0, sizeof g_p_gamma);
    memset(&g_p_ao, 0, sizeof g_p_ao);
    memset(&g_p_aoblur, 0, sizeof g_p_aoblur);
    memset(&g_p_ssr, 0, sizeof g_p_ssr);
    memset(&g_p_godsrc, 0, sizeof g_p_godsrc);
    memset(&g_p_god, 0, sizeof g_p_god);
    memset(&g_p_light, 0, sizeof g_p_light);
    g_ready = -1;
    g_w = g_h = 0;
}

#else  /* B3_AFX_NO_GL — the validator probe build */

int  b3_afx_frame_begin(int w, int h) { (void)w; (void)h; return 0; }
void b3_afx_scene_done(int w, int h, const B3AfxInputs *in)
{ (void)w; (void)h; (void)in; }
int  b3_afx_frame_end(int w, int h) { (void)w; (void)h; return 0; }
void b3_afx_rebind_scene(int w, int h) { (void)w; (void)h; }
void b3_afx_shutdown(void) {}
const char *b3_afx_status(void) { return "built without GL (validator probe)"; }
/* tier 4r's "is the ray answering": there is no shader here to have compiled
 * it into, so the answer is no.  b3_rt_want() itself IS reachable in a probe
 * build -- it is GL-free and lives in src/burnout3_rt.c. */
int b3_afx_rt_active(void) { return 0; }
int b3_afx_rt_car_budget(void) { return 0; }
int b3_afx_rt_car_traced(int slot) { (void)slot; return 0; }
void b3_afx_rt_cars_begin(void) {}
void b3_afx_rt_car_add(int slot, int model, const float pos[3],
                       const float rot9[9])
{ (void)slot; (void)model; (void)pos; (void)rot9; }

/* The photorealism layer's publishers, in the probe build.  b3_photo_on() and
 * b3_photo_fx() are NOT here -- they are GL-free by construction and live up
 * with the other laws, which is what lets tools/validate_postfx.py execute the
 * master gate rather than read it. */
void b3_photo_set_camera(const B3PhotoCamera *cam) { (void)cam; }
void b3_photo_set_sun(const float d[3], const float rgb[3], int have)
{ (void)d; (void)rgb; (void)have; }
void b3_photo_set_shadow(unsigned tex, const float vp[16], int size)
{ (void)tex; (void)vp; (void)size; }
void b3_photo_set_lights(const float *p, const float *c, const float *d,
                         int n, float dusk)
{ (void)p; (void)c; (void)d; (void)n; (void)dusk; }
const char *b3_photo_status(void)
{ return "built without GL (validator probe)"; }

#endif
