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
    "void main() {\n"
    "  vec3 c = texture2D(uScene, vUV).rgb;\n"
    "  vec4 b = texture2D(uBlur, vUV);\n"
    "%s"                        /* the blend: cross-fade, or the recovered add */
    "  c += uAmt.y * texture2D(uBloom, vUV).rgb;\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  c = mix(c, vec3(l), uAmt.z);\n"
    "  float r = length(vUV - vec2(0.5)) / 0.7071068;\n"
    "  c *= 1.0 - uAmt.w * r * r;\n"
    "  gl_FragColor = vec4(c * uExposure, 1.0);\n"
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

/* ------------------------------------------------------------- the targets */

typedef struct AfxTarget {
    unsigned fbo, tex;
    int w, h;
} AfxTarget;

enum { T_SCENE, T_UI, T_HALF, T_QUARTER, T_BLUR, T_BLOOM_A, T_BLOOM_B,
       T_COUNT };

static AfxTarget g_t[T_COUNT];
static unsigned  g_depth_rb;
static unsigned  g_ms_fbo, g_ms_color_rb, g_ms_depth_rb;
static int       g_samples;         /* 0 = no MSAA path */
static const char *g_ms_why;        /* why not, when MSAA was asked for */
static int       g_ms_checked;      /* the first resolve's glGetError is read */
static int       g_ms_banned;       /* ...and if it failed, never re-armed */
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

static struct {
    unsigned prog;
    int uTex, uTexel, uMask, uThreshold, uDir;
    int uScene, uBlur, uBloom, uAmt, uExposure, uRamp;
    int aPos;
} g_p_down, g_p_radial, g_p_bright, g_p_blur, g_p_composite, g_p_gamma;

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
static int afx_scene_depth(int w, int h)
{
    static const unsigned fmts[2] = { GL_DEPTH_COMPONENT24,
                                      GL_DEPTH_COMPONENT16 };
    int i;
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
    const char *e;
    int n;
    if ((e = getenv("B3_AFX_MSAA")) && *e) n = atoi(e);
    else if ((e = getenv("B3_MSAA")) && *e) n = atoi(e);
    else n = B3_AFX_MSAA_DEF;
    if (n < 0)  n = 0;
    if (n > 16) n = 16;
    return n;
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
    L(g_p_down);   L(g_p_radial);    L(g_p_bright);
    L(g_p_blur);   L(g_p_composite); L(g_p_gamma);
#undef L

    /* sampler units, set once */
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_composite.prog);
    if (g_p_composite.uScene >= 0) p_glUniform1i(g_p_composite.uScene, 0);
    if (g_p_composite.uBlur  >= 0) p_glUniform1i(g_p_composite.uBlur,  1);
    if (g_p_composite.uBloom >= 0) p_glUniform1i(g_p_composite.uBloom, 2);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_gamma.prog);
    if (g_p_gamma.uTex  >= 0) p_glUniform1i(g_p_gamma.uTex,  0);
    if (g_p_gamma.uRamp >= 0) p_glUniform1i(g_p_gamma.uRamp, 1);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, 0);
}

static int afx_build_all(void)
{
    char radial[1400], composite[1400];
    unsigned char ramp[256], rgba[256 * 4];
    int i;
    const char *e;
    int add_mode;
    static const float TRI[6] = { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f };

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
    snprintf(composite, sizeof composite, AFX_FS_COMPOSITE,
             add_mode ? AFX_COMPOSITE_ADD : AFX_COMPOSITE_MIX);
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
    afx_progs_locs();

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
    if (!g_depth_rb || !g_t[T_SCENE].fbo) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_t[T_SCENE].fbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, 0);
}

static int afx_resize(int w, int h)
{
    int ok = 1;
    if (g_w == w && g_h == h) return 1;

    afx_scene_depth_detach();
    ok &= afx_target_make(&g_t[T_SCENE],   w, h,         "scene");
    ok &= afx_target_make(&g_t[T_UI],      w, h,         "ui");
    ok &= afx_target_make(&g_t[T_HALF],    w / 2, h / 2, "half");
    ok &= afx_target_make(&g_t[T_QUARTER], w / 4, h / 4, "quarter");
    ok &= afx_target_make(&g_t[T_BLUR],    w / 4, h / 4, "blur");
    ok &= afx_target_make(&g_t[T_BLOOM_A], w / 8, h / 8, "bloomA");
    ok &= afx_target_make(&g_t[T_BLOOM_B], w / 8, h / 8, "bloomB");
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

void b3_afx_scene_done(int w, int h, const B3AfxInputs *in)
{
    float crash, s, alpha, r0, desat, vig, boost, bloom_amt;
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
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_ms_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_t[T_SCENE].fbo);
        if (!g_ms_checked) (void)glGetError();     /* start from a clean slate */
        p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
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

    /* 1-2. the prefilter: two halvings, full -> 1/2 -> 1/4 */
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_down.prog);
    if (g_p_down.uTex >= 0) p_glUniform1i(g_p_down.uTex, 0);
    afx_bind_target(&g_t[T_HALF]);
    afx_bind_tex(0, g_t[T_SCENE].tex);
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

    /* 5. the composite, into the UI target -- which stays bound for the HUD */
    afx_bind_target(&g_t[T_UI]);
    afx_bind_tex(0, g_t[T_SCENE].tex);
    afx_bind_tex(1, alpha > 0.002f ? g_t[T_BLUR].tex : 0);
    afx_bind_tex(2, g_k_bloom > 0.0f ? g_t[T_BLOOM_A].tex : 0);
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g_p_composite.prog);
    if (g_p_composite.uAmt >= 0)
        p_glUniform4f(g_p_composite.uAmt,
                      alpha > 0.002f ? alpha : 0.0f,
                      bloom_amt, desat, vig);
    if (g_p_composite.uExposure >= 0)
        p_glUniform1f(g_p_composite.uExposure, g_k_exposure);
    afx_draw(g_p_composite.prog, g_p_composite.aPos);

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
    if (g_p_down.prog)      p_glDeleteProgram(g_p_down.prog);
    if (g_p_radial.prog)    p_glDeleteProgram(g_p_radial.prog);
    if (g_p_bright.prog)    p_glDeleteProgram(g_p_bright.prog);
    if (g_p_blur.prog)      p_glDeleteProgram(g_p_blur.prog);
    if (g_p_composite.prog) p_glDeleteProgram(g_p_composite.prog);
    if (g_p_gamma.prog)     p_glDeleteProgram(g_p_gamma.prog);
    memset(&g_p_down, 0, sizeof g_p_down);
    memset(&g_p_radial, 0, sizeof g_p_radial);
    memset(&g_p_bright, 0, sizeof g_p_bright);
    memset(&g_p_blur, 0, sizeof g_p_blur);
    memset(&g_p_composite, 0, sizeof g_p_composite);
    memset(&g_p_gamma, 0, sizeof g_p_gamma);
    g_ready = -1;
    g_w = g_h = 0;
}

#else  /* B3_AFX_NO_GL — the validator probe build */

int  b3_afx_frame_begin(int w, int h) { (void)w; (void)h; return 0; }
void b3_afx_scene_done(int w, int h, const B3AfxInputs *in)
{ (void)w; (void)h; (void)in; }
int  b3_afx_frame_end(int w, int h) { (void)w; (void)h; return 0; }
void b3_afx_shutdown(void) {}
const char *b3_afx_status(void) { return "built without GL (validator probe)"; }

#endif
