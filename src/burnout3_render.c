/* burnout3_render.c -- see burnout3_render.h for what this is and why. */
/* This file IMPLEMENTS the state shadow, so it must see the real GL entry
 * points rather than the shims the header points every other module at. */
#define B3R_NO_GL_SHADOW 1
#include "burnout3_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <SDL2/SDL_video.h>
#ifdef __ANDROID__
/* ANDROID PORT (android/): desktop GL there is gl4es on top of GLES2, so the
 * GL 2.0 entry points below must be resolved by gl4es' OWN lookup --
 * SDL_GL_GetProcAddress would hand back the system driver's GLES2 symbols,
 * which sit outside the program and uniform state gl4es maintains. */
#include <gl4esinit.h>
#define SDL_GL_GetProcAddress gl4es_GetProcAddress
#endif
/* WEB PORT (web/): gl4es IS GONE from this link, and so is the override.  The
 * only reason one was ever needed is that gl4es MANGLED its exports to
 * gl4es_gl* under __EMSCRIPTEN__ to avoid colliding with Emscripten's own
 * WebGL symbols, so its entry points had to be found through its own lookup.
 * Those Emscripten symbols are now the ones the engine wants, and plain
 * SDL_GL_GetProcAddress -- the same call the desktop uses -- returns them.
 * <GL/gl.h> resolves to web/GL/gl.h, which is GLES2.  See that file. */

/* GL 2.0 tokens a 1.x <GL/gl.h> may not carry. */
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS  0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER    0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW     0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW    0x88E8
#endif

/* ====================================================================== *
 *  THE PROGRAM
 *
 *  ONE program covers every migrated pass, because between them the passes
 *  only ever used four fixed-function features and all four are one line of
 *  GLSL: texture MODULATE, the GL_COMBINE "texture alpha times primary rgb"
 *  the shine pass programmed by hand, the GREATER alpha test, and linear fog.
 *  Keeping it to one program is not tidiness -- it is what stops gl4es
 *  recompiling shader VARIANTS (fpe_CustomShader) and re-issuing
 *  glUseProgram between batches.
 *
 *  Deliberately GLSL 1.10 with no #version line: a desktop compatibility
 *  context compiles it as 1.10 and gl4es' shaderconv rewrites it to
 *  "#version 100 / precision highp float" for GLES2 (src/gl/shaderconv.c:510
 *  -- non-FPE shaders get highp when the hardware has it, which matters here
 *  because the fog coordinate runs to thousands of world units).  No
 *  `precision` line is written by hand; shaderconv comments those out.
 * ====================================================================== */

/* THE TRANSFORM IS A CPU-COMPOSED uMVP.
 *
 * It was `ftransform()` for the whole migration, and deliberately: ftransform
 * is defined to be invariant with the fixed-function pipeline, which is what
 * let every migrated pass be compared BIT FOR BIT against the pass it
 * replaced.  Twelve passes and about 2 900 lines later there is nothing left
 * on that pipeline to compare against -- and ftransform() is also the single
 * thing tying this renderer to a compatibility profile WebGL does not have.
 *
 * So the transform moves onto the CPU, and the price was measured before it
 * was paid.  On the pinned frame, track pass alone:
 *
 *     ftransform()                   0 pixels (the definition of invariant)
 *     uProj * (uMV * v), two mat4s   4.69% of pixels moved, 0.082% by > 8/255
 *     one CPU-composed uMVP          1.69% moved, 0.80% by > 8/255
 *
 * The middle row is what this is: two matrices, multiplied in the shader in
 * the order GL specifies, so the association matches the fixed-function one
 * as closely as anything can without BEING it.  All of the residue is
 * sub-pixel vertex drift -- no structural change, no lost geometry.
 *
 * gl_Color / gl_MultiTexCoord0 / gl_Normal go with it, to generic attributes
 * at the locations B3R_ATTR_* names.  Those locations are shared with the
 * recovered carfx program, because the same vertex buffers feed both.
 *
 * The GLSL is written to compile BOTH as desktop GLSL 1.10 and as ES 1.00:
 * no #version line, and the precision block guarded on GL_ES, which only the
 * ES compiler defines. */
static const char* B3R_VS =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "uniform mat4  uMVP;\n"
    "uniform mat4  uMV;\n"
    "uniform vec2  uUVOff;\n"
    "uniform vec4  uColor;\n"
    "uniform float uFogFar;\n"
    "attribute vec3 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "varying vec2  vUV;\n"
    "varying vec4  vCol;\n"
    "varying float vFog;\n"
    "void main(){\n"
    "  vec4 p = vec4(aPos, 1.0);\n"
    "  gl_Position = uMVP * p;\n"
    /* the animated-material scroll: vertex-shader constant 0x63 added to
     * v9.xy, `add oT0.xy, v9.xy, c[99].xy` in every world vertex program */
    "  vUV  = aUV + uUVOff;\n"
    "  vCol = aCol * uColor;\n"
    /* MIN oFog, r12.z, c[120].z -- the last instruction of every world vertex
     * program (0x003E88C0 +15 and siblings), and the reason the
     * fixed-function fog table alone could never reproduce this ramp. */
    "  vFog = min(abs((uMV * p).z), uFogFar);\n"
    "}\n";

static const char* B3R_FS =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "uniform sampler2D uTex;\n"
    "uniform int   uMode;\n"
    "uniform float uAlphaRef;\n"
    "uniform int   uFogOn;\n"
    "uniform vec3  uFogColor;\n"
    "uniform vec2  uFogSE;\n"
    "varying vec2  vUV;\n"
    "varying vec4  vCol;\n"
    "varying float vFog;\n"
    "void main(){\n"
    "  vec4 c = vCol;\n"
    /* B3R_TEX_MODULATE: rgb = 2*tex*vcol is what every world pixel shader
     * computes; the x2 is already in the stored vertex colour, so this is a
     * plain GL_MODULATE.  Fragment alpha = tex.a * primary.a is the class-6
     * output alpha tex.a * C0.a exactly. */
    "  if (uMode == 1) c = texture2D(uTex, vUV) * vCol;\n"
    /* B3R_TEX_SHINE: the class-1 pixel shader's stage 1,
     * R0.rgb = R0.a * C0.rgb + R0.rgb, drawn as its own additive pass -- the
     * legacy spelling was GL_COMBINE / MODULATE(TEXTURE.alpha, PRIMARY.rgb),
     * with the alpha combiner left at its default tex.a * primary.a. */
    "  else if (uMode == 2) {\n"
    "    vec4 t = texture2D(uTex, vUV);\n"
    "    c = vec4(t.a * vCol.rgb, t.a * vCol.a);\n"
    "  }\n"
    /* D3DCMP_GREATER against D3DRS_ALPHAREF.  uAlphaRef < 0 is "no test": no
     * alpha can be <= a negative reference, so the branch is uniform, always
     * taken the same way, and free. */
    "  if (c.a <= uAlphaRef) discard;\n"
    "  if (uFogOn != 0) {\n"
    "    float f = clamp((uFogSE.x - vFog) * uFogSE.y, 0.0, 1.0);\n"
    "    c.rgb = mix(uFogColor, c.rgb, f);\n"
    "  }\n"
    "  gl_FragColor = c;\n"
    "}\n";

/* ---- entry points ------------------------------------------------------ */
static unsigned (*p_glCreateShader)(unsigned);
static void     (*p_glShaderSource)(unsigned, int, const char* const*, const int*);
static void     (*p_glCompileShader)(unsigned);
static void     (*p_glGetShaderiv)(unsigned, unsigned, int*);
static void     (*p_glGetShaderInfoLog)(unsigned, int, int*, char*);
static unsigned (*p_glCreateProgram)(void);
static void     (*p_glAttachShader)(unsigned, unsigned);
static void     (*p_glLinkProgram)(unsigned);
static void     (*p_glGetProgramiv)(unsigned, unsigned, int*);
static void     (*p_glGetProgramInfoLog)(unsigned, int, int*, char*);
static void     (*p_glUseProgram)(unsigned);
static void     (*p_glDeleteShader)(unsigned);
static void     (*p_glDeleteProgram)(unsigned);
static int      (*p_glGetUniformLocation)(unsigned, const char*);
static void     (*p_glUniform1i)(int, int);
static void     (*p_glUniform1f)(int, float);
static void     (*p_glUniform2f)(int, float, float);
static void     (*p_glUniform3f)(int, float, float, float);
static void     (*p_glUniform4f)(int, float, float, float, float);
static void     (*p_glBindAttribLocation)(unsigned, unsigned, const char*);
static void     (*p_glUniformMatrix4fv)(int, int, unsigned char, const float*);
static void     (*p_glEnableVertexAttribArray)(unsigned);
static void     (*p_glDisableVertexAttribArray)(unsigned);
static void     (*p_glVertexAttribPointer)(unsigned, int, unsigned,
                                           unsigned char, int, const void*);
static void     (*p_glVertexAttrib4f)(unsigned, float, float, float,
                                      float);
static void     (*p_glGenBuffers)(int, unsigned*);
static void     (*p_glDeleteBuffers)(int, const unsigned*);
static void     (*p_glBindBuffer)(unsigned, unsigned);
static void     (*p_glBufferData)(unsigned, long, const void*, unsigned);
static void     (*p_glBufferSubData)(unsigned, long, long, const void*);

/* ---- module state ------------------------------------------------------ */

static int      g_state = -1;      /* -1 untried, 0 unavailable, 1 ready */
static unsigned g_prog;
static int      g_depth;           /* b3r_begin nesting                  */
static int      g_model_pushed;    /* b3r_model() owns a matrix-stack level */
/* trackmesh_gl_single_sided()'s decision, so the retained passes cull the
 * world exactly as the legacy ones did and hand the same answer back. */
static int      g_world_cull = -1;
static int      g_stats[B3R_STAT_COUNT];

static struct {
    int mvp, mv, uvoff, color, fogfar;
    int tex, mode, aref, fogon, fogcol, fogse;
} g_u;

/* Everything the module has already told GL, so a redundant call is free.
 * This cache is the whole point: the state a batch asks for is USUALLY the
 * state the last batch left. */
static struct {
    float mvp[16], mv[16], color[4], uvoff[2];
    float fogfar, aref, fogcol[3], fogse[2];
    int   mode, fogon;
    unsigned tex;
    int   blend, depth_mask, depth_test, depth_func, cull;
    unsigned vbo_a, vbo_b;   /* which buffers the attributes point into */
    int   split;
} g_c;

static float g_proj[16], g_view[16];

static void b3r_matrix_init(void);   /* defined with the matrix stack below */

/* The layout the client arrays are currently pointed at, so re-asserting the
 * same one costs nothing.  gl4es caches the resulting glVertexAttribPointer
 * per slot too (realize_glenv), so a whole pass off one buffer issues no
 * vertex setup at all. */
static B3RVtxFmt g_fmt_cur;
static int       g_fmt_valid;

/* ---- uniform setters, cached ------------------------------------------ */
static void u_mat(int loc, float* cache, const float* v) {
    if (loc < 0) return;
    if (memcmp(cache, v, 16 * sizeof(float)) == 0) return;
    memcpy(cache, v, 16 * sizeof(float));
    p_glUniformMatrix4fv(loc, 1, 0, v);
}
static void u_1i(int loc, int* cache, int v) {
    if (loc < 0 || *cache == v) return;
    *cache = v;
    p_glUniform1i(loc, v);
}
static void u_1f(int loc, float* cache, float v) {
    if (loc < 0 || *cache == v) return;
    *cache = v;
    p_glUniform1f(loc, v);
}
static void u_2f(int loc, float* cache, float x, float y) {
    if (loc < 0 || (cache[0] == x && cache[1] == y)) return;
    cache[0] = x; cache[1] = y;
    p_glUniform2f(loc, x, y);
}
static void u_3f(int loc, float* cache, float x, float y, float z) {
    if (loc < 0 || (cache[0] == x && cache[1] == y && cache[2] == z)) return;
    cache[0] = x; cache[1] = y; cache[2] = z;
    p_glUniform3f(loc, x, y, z);
}
static void u_4f(int loc, float* cache, float x, float y, float z, float w) {
    if (loc < 0 || (cache[0] == x && cache[1] == y
                    && cache[2] == z && cache[3] == w)) return;
    cache[0] = x; cache[1] = y; cache[2] = z; cache[3] = w;
    p_glUniform4f(loc, x, y, z, w);
}

/* ---- init -------------------------------------------------------------- */
static unsigned compile_one(unsigned type, const char* src, const char* what) {
    unsigned s = p_glCreateShader(type);
    int ok = 0;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        int n = 0;
        p_glGetShaderInfoLog(s, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[b3r] %s shader failed: %s\n", what, log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

/* THERE IS NO SECOND RENDERER TO SWITCH TO ANY MORE.  While the migration was
 * running, B3_RENDER=legacy put the fixed-function path back so the two could
 * be compared at a pinned frame; the moment a pass' gate went green its
 * fixed-function code was deleted, and git history is the reference now.  What
 * is left is the one question a caller still has to be able to ask: did the
 * program come up? */
int b3r_active(void) { return g_state == 1; }

/* RETRY, DO NOT LATCH.  This is called as soon as the GL context exists, but
 * on the WEB PORT the GL 2.0 surface is gl4es', and gl4es initialises LATER
 * than the SDL context does -- the first attempt legitimately finds nothing
 * and the one behind the first draw finds everything.  (It also must not gate
 * on SDL_GL_GetCurrentContext(): the web port creates its context on the game
 * thread through emscripten's own path, so SDL's bookkeeping does not see it
 * and that call returns NULL for the whole run.  Gating on it left the world,
 * the props, the scenery and the HUD drawing nothing at all, with the smoke's
 * PIXELS ON CANVAS gate still green off the sky dome.) */
int b3r_init(void) {
    if (g_state == 1) return 1;
    static int moaned;

#define B3R_GET(fn) do { *(void**)(&p_##fn) = SDL_GL_GetProcAddress(#fn);      \
                         if (!p_##fn) {                                        \
                             if (!moaned) {                                    \
                                 moaned = 1;                                   \
                                 fprintf(stderr, "[b3r] %s not resolvable "    \
                                         "yet -- will retry at the first "     \
                                         "draw\n", #fn);                       \
                             }                                                 \
                             return 0; } } while (0)
    B3R_GET(glCreateShader);   B3R_GET(glShaderSource);
    B3R_GET(glCompileShader);  B3R_GET(glGetShaderiv);
    B3R_GET(glGetShaderInfoLog);
    B3R_GET(glCreateProgram);  B3R_GET(glAttachShader);
    B3R_GET(glLinkProgram);    B3R_GET(glGetProgramiv);
    B3R_GET(glGetProgramInfoLog);
    B3R_GET(glUseProgram);     B3R_GET(glDeleteShader);
    B3R_GET(glDeleteProgram);  B3R_GET(glGetUniformLocation);
    B3R_GET(glUniform1i);      B3R_GET(glUniform1f);
    B3R_GET(glUniform2f);      B3R_GET(glUniform3f);
    B3R_GET(glUniform4f);      B3R_GET(glUniformMatrix4fv);
    B3R_GET(glBindAttribLocation);
    B3R_GET(glEnableVertexAttribArray);
    B3R_GET(glDisableVertexAttribArray);
    B3R_GET(glVertexAttribPointer);
    B3R_GET(glVertexAttrib4f);
    B3R_GET(glGenBuffers);     B3R_GET(glDeleteBuffers);
    B3R_GET(glBindBuffer);     B3R_GET(glBufferData);
    B3R_GET(glBufferSubData);
#undef B3R_GET

    unsigned vs = compile_one(GL_VERTEX_SHADER, B3R_VS, "vertex");
    if (!vs) { g_state = 0; return 0; }
    unsigned fs = compile_one(GL_FRAGMENT_SHADER, B3R_FS, "fragment");
    if (!fs) { p_glDeleteShader(vs); g_state = 0; return 0; }
    unsigned pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glAttachShader(pr, fs);
    /* Bound rather than queried, so the SAME buffer can feed this program and
     * the recovered carfx one without re-pointing anything: b3r_attr_bind()
     * is what carfx calls to agree. */
    b3r_attr_bind(pr);
    p_glLinkProgram(pr);
    int ok = 0;
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        char log[1024];
        int n = 0;
        p_glGetProgramInfoLog(pr, (int)sizeof log, &n, log);
        log[(n > 0 && n < (int)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[b3r] link failed: %s\n", log);
        p_glDeleteProgram(pr);
        g_state = 0;
        return 0;
    }
    g_prog = pr;
    g_u.mvp    = p_glGetUniformLocation(pr, "uMVP");
    g_u.mv     = p_glGetUniformLocation(pr, "uMV");
    g_u.uvoff  = p_glGetUniformLocation(pr, "uUVOff");
    g_u.color  = p_glGetUniformLocation(pr, "uColor");
    g_u.fogfar = p_glGetUniformLocation(pr, "uFogFar");
    g_u.tex    = p_glGetUniformLocation(pr, "uTex");
    g_u.mode   = p_glGetUniformLocation(pr, "uMode");
    g_u.aref   = p_glGetUniformLocation(pr, "uAlphaRef");
    g_u.fogon  = p_glGetUniformLocation(pr, "uFogOn");
    g_u.fogcol = p_glGetUniformLocation(pr, "uFogColor");
    g_u.fogse  = p_glGetUniformLocation(pr, "uFogSE");
    /* The sampler binding lives in the PROGRAM object, so it is set once here
     * and never again -- one call for the whole run rather than one a pass. */
    b3r_use_program_via(p_glUseProgram, pr);
    if (g_u.tex >= 0) p_glUniform1i(g_u.tex, 0);
    b3r_use_program_via(p_glUseProgram, 0);
    b3r_matrix_init();
    g_state = 1;
    printf("[b3r] retained renderer ready (program %u)\n", g_prog);
    return 1;
}

void b3r_shutdown(void) {
    if (g_state != 1) { g_state = -1; return; }
    b3r_track_free();
    if (p_glDeleteProgram) p_glDeleteProgram(g_prog);
    g_prog = 0;
    g_state = -1;
}

/* ====================================================================== *
 *  THE MATRIX STACK
 *
 *  WHY THIS EXISTS AND WHY THE GL CALLS ARE STILL IN IT.
 *
 *  The retained path's vertex stage used to be `ftransform()`, which reads
 *  the FIXED-FUNCTION matrix stack.  That was a deliberate choice while the
 *  migration was running: ftransform() is defined to be invariant with the
 *  fixed-function pipeline, which is what let every migrated pass be compared
 *  BIT FOR BIT against the pass it replaced.  It is also the single biggest
 *  thing keeping gl4es alive -- 150 glPushMatrix / glRotatef / glOrtho call
 *  sites across this harness, none of which WebGL has.
 *
 *  So the stack moved onto the CPU.  While the migration ran, every operation
 *  below did BOTH halves -- its own stack AND the fixed-function call -- so
 *  that the passes not yet converted could keep reading the GL one.  All of
 *  them are converted now (no shader in this tree reads a compatibility
 *  builtin, and nothing draws through the fixed-function stage), so the GL
 *  half is gone.  It was deleted HERE, in one place, rather than at the 150
 *  call sites that were converted to this API.
 *
 *  Layout is GL's: column-major, m[col*4 + row], so m[12..14] is the
 *  translation and the array can be handed to glLoadMatrixf unchanged.
 * ====================================================================== */
#define B3R_STACK_DEPTH 24

typedef struct {
    float m[B3R_STACK_DEPTH][16];
    int   top;
} B3RMatStack;

static B3RMatStack g_ms[2];        /* [B3R_MAT_MODELVIEW], [B3R_MAT_PROJECTION] */
static int         g_ms_which = B3R_MAT_MODELVIEW;
static int         g_ms_dirty = 1; /* uMVP / uMV need re-uploading            */

static void mat_identity(float* o) {
    memset(o, 0, 16 * sizeof(float));
    o[0] = o[5] = o[10] = o[15] = 1.0f;
}

/* o = a * b, both column-major.  The association is the one GL specifies for
 * glMultMatrix: the new matrix is applied BEFORE what is already there. */
static void mat_mul(float* o, const float* a, const float* b) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0]
                         + a[1 * 4 + r] * b[c * 4 + 1]
                         + a[2 * 4 + r] * b[c * 4 + 2]
                         + a[3 * 4 + r] * b[c * 4 + 3];
    memcpy(o, t, sizeof t);
}

static float* ms_cur(void) { return g_ms[g_ms_which].m[g_ms[g_ms_which].top]; }

static void ms_apply(const float* m) {
    mat_mul(ms_cur(), ms_cur(), m);
    g_ms_dirty = 1;
}

void b3r_matrix_mode(int which) {
    g_ms_which = (which == B3R_MAT_PROJECTION) ? B3R_MAT_PROJECTION
                                               : B3R_MAT_MODELVIEW;
}

void b3r_push(void) {
    B3RMatStack* st = &g_ms[g_ms_which];
    if (st->top + 1 < B3R_STACK_DEPTH) {
        memcpy(st->m[st->top + 1], st->m[st->top], 16 * sizeof(float));
        st->top++;
    }
}

void b3r_pop(void) {
    B3RMatStack* st = &g_ms[g_ms_which];
    if (st->top > 0) st->top--;
    g_ms_dirty = 1;
}

void b3r_identity(void) {
    mat_identity(ms_cur());
    g_ms_dirty = 1;
}

void b3r_load(const float m[16]) {
    memcpy(ms_cur(), m, 16 * sizeof(float));
    g_ms_dirty = 1;
}

void b3r_mult(const float m[16]) {
    ms_apply(m);
}

void b3r_translate(float x, float y, float z) {
    float m[16];
    mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    ms_apply(m);
}

void b3r_scale(float x, float y, float z) {
    float m[16];
    mat_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    ms_apply(m);
}

/* glRotatef's own definition (GL 2.1 spec, table 2.10): a normalised axis and
 * the Rodrigues form.  A zero-length axis is a no-op, as it is in GL. */
void b3r_rotate(float deg, float x, float y, float z) {
    float m[16], len = sqrtf(x * x + y * y + z * z);
    if (len > 1e-12f) {
        float c, s1, one_c;
        x /= len; y /= len; z /= len;
        c = cosf(deg * 0.017453292519943295f);
        s1 = sinf(deg * 0.017453292519943295f);
        one_c = 1.0f - c;
        mat_identity(m);
        m[0]  = x * x * one_c + c;
        m[1]  = y * x * one_c + z * s1;
        m[2]  = x * z * one_c - y * s1;
        m[4]  = x * y * one_c - z * s1;
        m[5]  = y * y * one_c + c;
        m[6]  = y * z * one_c + x * s1;
        m[8]  = x * z * one_c + y * s1;
        m[9]  = y * z * one_c - x * s1;
        m[10] = z * z * one_c + c;
        ms_apply(m);
    }
}

void b3r_ortho(float l, float r, float b, float t, float n, float f) {
    float m[16];
    mat_identity(m);
    m[0]  =  2.0f / (r - l);
    m[5]  =  2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    ms_apply(m);
}

void b3r_frustum(float l, float r, float b, float t, float n, float f) {
    float m[16];
    memset(m, 0, sizeof m);
    m[0]  =  2.0f * n / (r - l);
    m[5]  =  2.0f * n / (t - b);
    m[8]  =  (r + l) / (r - l);
    m[9]  =  (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
    ms_apply(m);
}

const float* b3r_mat(int which) {
    const B3RMatStack* st =
        &g_ms[(which == B3R_MAT_PROJECTION) ? B3R_MAT_PROJECTION
                                            : B3R_MAT_MODELVIEW];
    return st->m[st->top];
}

static void b3r_matrix_init(void) {
    mat_identity(g_ms[0].m[0]);
    mat_identity(g_ms[1].m[0]);
    g_ms[0].top = g_ms[1].top = 0;
    g_ms_dirty = 1;
}

/* THE SHARED ATTRIBUTE LAYOUT.  Every program that draws this harness'
 * vertex buffers binds these four names to these four locations, so a buffer
 * pointed once feeds any of them.  Called before glLinkProgram. */
void b3r_attr_bind(unsigned prog) {
    if (!p_glBindAttribLocation) return;
    p_glBindAttribLocation(prog, B3R_ATTR_POS, "aPos");
    p_glBindAttribLocation(prog, B3R_ATTR_UV,  "aUV");
    p_glBindAttribLocation(prog, B3R_ATTR_COL, "aCol");
    p_glBindAttribLocation(prog, B3R_ATTR_NRM, "aNrm");
}

/* uMVP = projection * modelview, recomputed only when the stack moved.  Every
 * draw path calls b3r_sync_matrices() first; a caller with its OWN program
 * (carfx) reads the same two matrices through b3r_mvp() / b3r_mv(). */
static float g_mvp_cache[16];

const float* b3r_mvp(void) {
    mat_mul(g_mvp_cache, b3r_mat(B3R_MAT_PROJECTION),
            b3r_mat(B3R_MAT_MODELVIEW));
    return g_mvp_cache;
}
const float* b3r_mv(void) { return b3r_mat(B3R_MAT_MODELVIEW); }

static void b3r_sync_matrices(void) {
    if (g_state != 1 || !g_ms_dirty) return;
    g_ms_dirty = 0;
    u_mat(g_u.mvp, g_c.mvp, b3r_mvp());
    u_mat(g_u.mv,  g_c.mv,  b3r_mat(B3R_MAT_MODELVIEW));
}

/* ---- camera ------------------------------------------------------------ */
void b3r_set_camera(const float proj[16], const float view[16]) {
    memcpy(g_proj, proj, sizeof g_proj);
    memcpy(g_view, view, sizeof g_view);
}
const float* b3r_view(void) { return g_view; }
const float* b3r_proj(void) { return g_proj; }

/* ====================================================================== *
 *  THE STATE SHADOW  (see burnout3_render.h for why it exists)
 *
 *  Every field glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT |
 *  GL_DEPTH_BUFFER_BIT) actually protected in this harness, mirrored on the
 *  CPU.  The initial values are GL's OWN defaults, which is what the context
 *  really starts at; b3r_init() does not reprogram any of them.
 * ====================================================================== */
typedef struct {
    unsigned char blend, depth_test, cull, depth_mask;
    unsigned char cmask[4];
    unsigned      blend_src, blend_dst, blend_eq, depth_func, front_face;
    unsigned      tex2d;
} B3RGLShadow;

static B3RGLShadow g_gl = {
    0, 0, 0, 1,                       /* BLEND / DEPTH_TEST / CULL_FACE off,
                                       * depth writes on                      */
    { 1, 1, 1, 1 },
    GL_ONE, GL_ZERO, 0x8006 /*GL_FUNC_ADD*/, GL_LESS, GL_CCW,
    0
};

static B3RGLShadow g_gl_stack[8];
static int         g_gl_sp;

static unsigned g_tex_unit;                 /* GL_TEXTURE0 + n            */
static unsigned g_tex_bound[8];

/* B3_STATE_AUDIT=1 -- compare the shadow against the DRIVER at every push and
 * report any field that has drifted.  This is how a shadow is proved, and it
 * is the only way to find a field some pass sets behind the shims' back: the
 * symptom of one is a filtered-out state call, and the symptom of THAT is a
 * frame that is subtly wrong everywhere with nothing to grep for. */
static int audit_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("B3_STATE_AUDIT"); v = e && *e != '0'; }
    return v;
}

static void audit_field(const char* name, int shadow, int real) {
    static long n;
    if (shadow == real) return;
    if (n++ > 40) return;
    fprintf(stderr, "[b3r audit] %s: shadow %d, driver %d\n",
            name, shadow, real);
}

static void audit_push(void) {
    GLint v = 0;
    if (!audit_on()) return;
    audit_field("BLEND",      g_gl.blend,      glIsEnabled(GL_BLEND));
    audit_field("DEPTH_TEST", g_gl.depth_test, glIsEnabled(GL_DEPTH_TEST));
    audit_field("CULL_FACE",  g_gl.cull,       glIsEnabled(GL_CULL_FACE));
    glGetIntegerv(GL_DEPTH_WRITEMASK, &v); audit_field("DEPTH_WRITEMASK", g_gl.depth_mask, v);
    glGetIntegerv(GL_DEPTH_FUNC,      &v); audit_field("DEPTH_FUNC",  (int)g_gl.depth_func, v);
    glGetIntegerv(GL_BLEND_SRC_RGB,   &v); audit_field("BLEND_SRC",   (int)g_gl.blend_src, v);
    glGetIntegerv(GL_BLEND_DST_RGB,   &v); audit_field("BLEND_DST",   (int)g_gl.blend_dst, v);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &v); audit_field("BLEND_EQ", (int)g_gl.blend_eq, v);
    glGetIntegerv(GL_FRONT_FACE,      &v); audit_field("FRONT_FACE",  (int)g_gl.front_face, v);
    {
        GLboolean cm[4] = { 0, 0, 0, 0 };
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        audit_field("COLOR_WRITEMASK_R", g_gl.cmask[0], cm[0] ? 1 : 0);
        audit_field("COLOR_WRITEMASK_A", g_gl.cmask[3], cm[3] ? 1 : 0);
    }
    /* The unknown sentinel is not a disagreement -- it is the absence of a
     * claim, and the next bind is issued unconditionally because of it. */
    if (g_tex_bound[0] != (unsigned)-1) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
        audit_field("TEXTURE_BINDING_2D", (int)g_tex_bound[0], v);
    }
}

/* B3_STATE_NOFILTER=1 -- issue every state call, skipping the redundancy
 * filter.  If a frame changes under this, some pass is moving state behind
 * the shims and the shadow is lying; if it does not, the filter is sound. */
static int nofilter(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("B3_STATE_NOFILTER"); v = e && *e != '0'; }
    return v;
}

void b3r_gl_enable(unsigned cap) {
    audit_push();
    switch (cap) {
    case GL_BLEND:      if (g_gl.blend && !nofilter())      return; g_gl.blend      = 1; break;
    case GL_DEPTH_TEST: if (g_gl.depth_test && !nofilter()) return; g_gl.depth_test = 1; break;
    case GL_CULL_FACE:  if (g_gl.cull && !nofilter())       return; g_gl.cull       = 1; break;
    default: break;   /* anything else is passed through unshadowed */
    }
    glEnable((GLenum)cap);
}

void b3r_gl_disable(unsigned cap) {
    audit_push();
    switch (cap) {
    case GL_BLEND:      if (!g_gl.blend && !nofilter())      return; g_gl.blend      = 0; break;
    case GL_DEPTH_TEST: if (!g_gl.depth_test && !nofilter()) return; g_gl.depth_test = 0; break;
    case GL_CULL_FACE:  if (!g_gl.cull && !nofilter())       return; g_gl.cull       = 0; break;
    default: break;
    }
    glDisable((GLenum)cap);
}

int b3r_gl_is_enabled(unsigned cap) {
    switch (cap) {
    case GL_BLEND:      return g_gl.blend;
    case GL_DEPTH_TEST: return g_gl.depth_test;
    case GL_CULL_FACE:  return g_gl.cull;
    default: break;
    }
    return glIsEnabled((GLenum)cap);
}

/* THE TEXTURE BINDING, per unit.  b3r_state() already kept a cache of unit
 * 0's binding so it could skip redundant binds -- but 38 call sites bind
 * textures directly (every loader, every recovered postfx/carfx pass), and
 * none of them told that cache.  It therefore went stale in the ordinary
 * course of a frame, and a stale "already bound" is a SKIPPED bind: the
 * B3_STATE_AUDIT trace showed 20+ disagreements in a single frame (shadow
 * 112, driver 171 and so on).  Routing the binds through here makes the cache
 * true by construction, and folds the redundant ones out on the way. */

/* THE ACTIVE UNIT.  Every bind is recorded against it, so anything that moves
 * it must come through here -- postfx switches to GL_TEXTURE1 and back for the
 * two-sampler passes using its OWN GetProcAddress'd pointer, and while those
 * four calls were invisible the per-unit bind cache recorded unit 1's binds in
 * unit 0's slot.  Filtering binds on top of that moved 3.6-4.5% of every
 * pinned frame.  b3r_gl_active_texture_via() is how a caller with its own
 * entry point joins in. */
void b3r_gl_active_texture_via(void (*act)(unsigned), unsigned unit) {
    unsigned n = unit - GL_TEXTURE0;
    if (n >= sizeof g_tex_bound / sizeof g_tex_bound[0]) { act(unit); return; }
    if (g_tex_unit == n && !nofilter()) return;
    g_tex_unit = n;
    act(unit);
}

static void b3r_act_core(unsigned unit) { glActiveTexture((GLenum)unit); }

void b3r_gl_active_texture(unsigned unit) {
    b3r_gl_active_texture_via(b3r_act_core, unit);
}

void b3r_gl_bind_texture(unsigned target, unsigned tex) {
    if (target != GL_TEXTURE_2D) { glBindTexture((GLenum)target, tex); return; }
    /* (unsigned)-1 IS THIS FILE'S "I DO NOT KNOW" SENTINEL, not a texture.
     * b3r_use_flat() and the gen/delete hooks write it to force the next bind
     * to be issued, and b3r_state_push() captures whatever g_c.tex holds -- so
     * it can arrive here, and it must NOT reach the driver.
     *
     * Desktop GL hides this completely: glBindTexture accepts ANY unused name
     * and quietly creates an empty texture object called 0xFFFFFFFF, so the
     * frame is at worst one texture short.  WebGL has no integer names -- it
     * maps them to objects -- so the call is INVALID_OPERATION, the unit is
     * left bound to nothing, and then EVERY later draw whose program has a
     * sampler is rejected with INVALID_OPERATION as well.
     *
     * That is what made the car body invisible on the web while the desktop
     * was perfect: 2 266 of these in a single frame, found with
     * -sGL_ASSERTIONS=1 ("glBindTexture called with a nonexisting texture ID
     * -1!").  The pixel gates could never have seen it -- they compared the
     * web against ITSELF. */
    if (tex == (unsigned)-1) {
        g_tex_bound[g_tex_unit] = (unsigned)-1;   /* stay unknown */
        if (g_tex_unit == 0) g_c.tex = (unsigned)-1;
        return;
    }
    if (g_tex_bound[g_tex_unit] == tex && !nofilter()) return;
    g_tex_bound[g_tex_unit] = tex;
    if (g_tex_unit == 0) g_c.tex = tex;     /* the cache b3r_state() reads */
    glBindTexture(GL_TEXTURE_2D, tex);
}

/* THE INVALIDATION THAT MAKES THAT FILTER LEGAL, and it is not optional: a
 * texture NAME is not a stable identity.  glDeleteTextures unbinds the object
 * from every unit AND releases the name, and glGenTextures then hands the same
 * name back out for a DIFFERENT texture -- so a cache keyed on the name is
 * wrong in both directions unless it is told.  Filtering without these two
 * hooks moved 3.6% of the pinned frame (by more than 2/255): the wrong texture
 * on a handful of batches, exactly where a loader had recycled a name. */
void b3r_gl_gen_textures(int n, unsigned* names) {
    glGenTextures(n, names);
    for (int i = 0; i < n; i++)
        for (unsigned u = 0; u < sizeof g_tex_bound / sizeof g_tex_bound[0]; u++)
            if (g_tex_bound[u] == names[i]) g_tex_bound[u] = (unsigned)-1;
    if (g_tex_bound[0] == (unsigned)-1) g_c.tex = (unsigned)-1;
}

void b3r_gl_delete_textures(int n, const unsigned* names) {
    for (int i = 0; i < n; i++)
        for (unsigned u = 0; u < sizeof g_tex_bound / sizeof g_tex_bound[0]; u++)
            if (g_tex_bound[u] == names[i]) g_tex_bound[u] = 0;   /* GL unbinds */
    if (g_tex_bound[0] == 0) g_c.tex = 0;
    glDeleteTextures(n, names);
}

/* THE BOUND PROGRAM, in one place.  Three modules bind programs through their
 * own GetProcAddress'd pointers -- this renderer, the recovered carfx pair and
 * the postfx chain -- and between them they were issuing ~150 glUseProgram a
 * frame for about a dozen distinct transitions, because each one bound
 * unconditionally at every begin and unbound at every end.  Uniform state
 * belongs to the PROGRAM OBJECT, not to the binding, so skipping a redundant
 * bind cannot lose a uniform. */
static unsigned g_prog_bound;
static void (*g_prog_use)(unsigned);

void b3r_use_program_via(void (*use)(unsigned), unsigned prog) {
    if (g_prog_bound == prog && g_prog_use == use && !nofilter()) return;
    g_prog_bound = prog;
    g_prog_use   = use;
    use(prog);
}

void b3r_gl_blend_func(unsigned src, unsigned dst) {
    audit_push();
    if (g_gl.blend_src == src && g_gl.blend_dst == dst && !nofilter()) return;
    g_gl.blend_src = src; g_gl.blend_dst = dst;
    glBlendFunc((GLenum)src, (GLenum)dst);
}

void b3r_gl_blend_equation(unsigned mode) {
    audit_push();
    if (g_gl.blend_eq == mode && !nofilter()) return;
    g_gl.blend_eq = mode;
    glBlendEquation((GLenum)mode);
}

void b3r_gl_depth_mask(unsigned char on) {
    audit_push();
    on = on ? 1 : 0;
    if (g_gl.depth_mask == on && !nofilter()) return;
    g_gl.depth_mask = on;
    glDepthMask(on ? GL_TRUE : GL_FALSE);
}

void b3r_gl_depth_func(unsigned func) {
    audit_push();
    if (g_gl.depth_func == func && !nofilter()) return;
    g_gl.depth_func = func;
    glDepthFunc((GLenum)func);
}

void b3r_gl_front_face(unsigned dir) {
    audit_push();
    if (g_gl.front_face == dir && !nofilter()) return;
    g_gl.front_face = dir;
    glFrontFace((GLenum)dir);
}

void b3r_gl_color_mask(unsigned char r, unsigned char g,
                       unsigned char b, unsigned char a) {
    r = r ? 1 : 0; g = g ? 1 : 0; b = b ? 1 : 0; a = a ? 1 : 0;
    if (g_gl.cmask[0] == r && g_gl.cmask[1] == g &&
        g_gl.cmask[2] == b && g_gl.cmask[3] == a && !nofilter()) return;
    g_gl.cmask[0] = r; g_gl.cmask[1] = g; g_gl.cmask[2] = b; g_gl.cmask[3] = a;
    glColorMask(r, g, b, a);
}

/* ---- mipmap generation, in whichever spelling the context has ---------- *
 *
 * GL has two, and no target has both:
 *
 *   GL 1.4 / GLES1 : glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP,
 *                    GL_TRUE) BEFORE the upload -- the driver builds the
 *                    chain as part of glTexImage2D;
 *   GL 3.0 / GLES2 : glGenerateMipmap(GL_TEXTURE_2D) AFTER it.
 *
 * A desktop compatibility context has the first (and reaches the second only
 * through GetProcAddress); GLES2, and therefore WebGL, has ONLY the second --
 * GL_GENERATE_MIPMAP there is INVALID_ENUM, the chain never gets built, and a
 * GL_LINEAR_MIPMAP_LINEAR minifier on a texture with no chain is
 * MIPMAP-INCOMPLETE, which samples black.
 *
 * THE TWO ARE NOT PIXEL-EQUIVALENT, and that was measured, not assumed:
 * moving this harness' seven upload sites to glGenerateMipmap on the DESKTOP
 * moved 19.8% of the pinned frame (3.7% by more than 2/255).  The difference
 * is entirely in minification -- the bottom band of the frame, which is
 * magnified, moved 0.06%, while the mid-distance bands moved ~6% and came out
 * brighter -- i.e. the two paths average the cut-out texels (these textures
 * are alpha-keyed; the loader probes for it) differently.  Both are legal.
 * Neither is retail.  So the context is ASKED which one it has rather than
 * being handed the one that happens to be newer, and the desktop keeps the
 * chain every pinned reference in this tree was captured against. */
static int g_mip_param = -1;      /* 1 = GL 1.4 parameter, 0 = glGenerateMipmap */

static void (*p_glGenerateMipmap)(unsigned);

/* Call with the target texture BOUND, before glTexImage2D. */
void b3r_tex_mipmap_pre(void) {
    if (g_mip_param < 0) {
        /* ASK THE VERSION STRING, not the driver's error queue.  This used to
         * probe by setting GL_GENERATE_MIPMAP and reading glGetError, which
         * works but DELIBERATELY RAISES an error on every GLES target -- and a
         * browser's error log is a shared, rate-limited resource: Chrome stops
         * reporting for a context after enough of them, which is how the one
         * message that mattered got lost behind 255 that did not.  A renderer
         * has no business spending that budget on a question the version
         * string already answers. */
        const char* ver = (const char*)glGetString(GL_VERSION);
        int es = ver && (strstr(ver, "OpenGL ES") == ver
                         || strstr(ver, "WebGL") != NULL);
        g_mip_param = es ? 0 : 1;
        if (!g_mip_param) {
            *(void**)(&p_glGenerateMipmap) =
                SDL_GL_GetProcAddress("glGenerateMipmap");
            if (!p_glGenerateMipmap)
                fprintf(stderr, "[b3r] this context has NEITHER "
                        "GL_GENERATE_MIPMAP nor glGenerateMipmap -- minified "
                        "textures will be mipmap-incomplete\n");
        }
    }
    if (g_mip_param)
        glTexParameteri(GL_TEXTURE_2D, 0x8191, GL_TRUE);
}

/* Call with the same texture bound, after glTexImage2D. */
void b3r_gen_mipmap(void) {
    if (g_mip_param == 1) return;             /* built during the upload */
    if (p_glGenerateMipmap) p_glGenerateMipmap(GL_TEXTURE_2D);
}

/* B3_STATE_DUMP=1 -- print the shadow once per named site.  The shadow is
 * proven exact against the driver (see audit_push), so this is a readback-free
 * way to ask "what state is this pass actually running under" on a target where
 * a real glGet costs a round trip.  Added to compare the car body pass between
 * the desktop and the web, which is the comparison every earlier web gate was
 * missing. */
void b3r_state_dump(const char* where) {
    static const char* seen[16];
    static int nseen;
    const char* e = getenv("B3_STATE_DUMP");
    if (!e || *e == '0') return;
    for (int i = 0; i < nseen; i++) if (seen[i] == where) return;
    if (nseen < 16) seen[nseen++] = where;
    fprintf(stderr, "[b3r state] %-14s blend %d (src 0x%X dst 0x%X eq 0x%X) "
            "depth test %d write %d func 0x%X  cull %d front 0x%X  tex %u\n",
            where, g_gl.blend, g_gl.blend_src, g_gl.blend_dst, g_gl.blend_eq,
            g_gl.depth_test, g_gl.depth_mask, g_gl.depth_func,
            g_gl.cull, g_gl.front_face, g_tex_bound[0]);
    fflush(stderr);
}

void b3r_state_push(void) {
    audit_push();
    /* The texture binding is GL_TEXTURE_BIT, which one caller (the car shadow
     * pass) also asked for; b3r's own cache is the authority on it. */
    g_gl.tex2d = g_c.tex;
    if (g_gl_sp < (int)(sizeof g_gl_stack / sizeof g_gl_stack[0]))
        g_gl_stack[g_gl_sp] = g_gl;
    g_gl_sp++;
}

void b3r_state_pop(void) {
    B3RGLShadow w;
    if (g_gl_sp <= 0) return;
    g_gl_sp--;
    if (g_gl_sp >= (int)(sizeof g_gl_stack / sizeof g_gl_stack[0])) return;
    w = g_gl_stack[g_gl_sp];

    /* Restore through the shims, so each field costs a GL call only if it
     * actually moved -- the same filter glPopAttrib never had. */
    if (w.blend) b3r_gl_enable(GL_BLEND); else b3r_gl_disable(GL_BLEND);
    if (w.depth_test) b3r_gl_enable(GL_DEPTH_TEST);
    else              b3r_gl_disable(GL_DEPTH_TEST);
    if (w.cull) b3r_gl_enable(GL_CULL_FACE); else b3r_gl_disable(GL_CULL_FACE);
    b3r_gl_blend_func(w.blend_src, w.blend_dst);
    b3r_gl_blend_equation(w.blend_eq);
    b3r_gl_depth_mask(w.depth_mask);
    b3r_gl_depth_func(w.depth_func);
    b3r_gl_front_face(w.front_face);
    b3r_gl_color_mask(w.cmask[0], w.cmask[1], w.cmask[2], w.cmask[3]);
    b3r_gl_bind_texture(GL_TEXTURE_2D, w.tex2d);
    /* b3r_state()'s own cache tracked these same fields independently and has
     * just been contradicted, so point it back at the truth. */
    g_c.blend      = -1;
    g_c.depth_mask = w.depth_mask;
    g_c.depth_test = w.depth_test;
    g_c.depth_func = (int)w.depth_func;
    g_c.cull       = w.cull;
}

/* The shims exist now, so the REST of this file joins the other modules and
 * routes its own state calls through them -- b3r_state() and b3r_end() move
 * exactly the fields the shadow tracks, and a shadow its own owner bypassed
 * would be worthless. */
#define glEnable(c)             b3r_gl_enable((unsigned)(c))
#define glDisable(c)            b3r_gl_disable((unsigned)(c))
#define glBlendFunc(s, d)       b3r_gl_blend_func((unsigned)(s), (unsigned)(d))
#define glBlendEquation(m)      b3r_gl_blend_equation((unsigned)(m))
#define glDepthMask(m)          b3r_gl_depth_mask((unsigned char)((m) != 0))
#define glDepthFunc(f)          b3r_gl_depth_func((unsigned)(f))
#define glFrontFace(d)          b3r_gl_front_face((unsigned)(d))
#define glBindTexture(t, x)     b3r_gl_bind_texture((unsigned)(t), (unsigned)(x))
#define glActiveTexture(u)      b3r_gl_active_texture((unsigned)(u))

/* ---- begin / end ------------------------------------------------------- */
int b3r_world_cull(void) {
    if (g_world_cull < 0) g_world_cull = getenv("B3_TRACK_NOCULL") ? 0 : 1;
    return g_world_cull;
}

void b3r_begin(void) {
    if (g_state != 1 && !b3r_init()) return;
    if (g_depth++ > 0) return;
    b3r_use_program_via(p_glUseProgram, g_prog);
    /* The fixed-function features this shader replaces MUST be off, on the web
     * as much as on the desktop: gl4es tests the fixed-function state against
     * a bound user program (fpe_ReleventState / fpe_IsEmpty, src/gl/fpe.c:1090)
     * and, if any of it is live, COMPILES A VARIANT of the shader and switches
     * to it.  With all of them off the test is empty, one program stays bound
     * for the whole pass and no uniform re-sync happens.  On the desktop they
     * are simply ignored while a program is bound; this costs four calls a
     * frame either way. */
    /* The vertex data reaches the shader through GENERIC attributes now, at
     * the shared B3R_ATTR_* locations.  b3r_arrays_fmt() enables exactly the
     * ones the buffer actually carries. */
    /* Nothing about the cached state survives a legacy pass, so re-assert it
     * from scratch rather than trust it.  The uniform cache is safe to keep --
     * uniforms live in the program object -- but the GL enables are not. */
    g_c.tex = (unsigned)-1;
    g_c.blend = -1; g_c.depth_mask = -1; g_c.depth_test = -1;
    g_c.depth_func = -1; g_c.cull = -1; g_c.split = -1;
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;
    g_fmt_valid = 0;
}

void b3r_end(void) {
    if (g_state != 1) return;
    if (--g_depth > 0) return;
    g_depth = 0;
    /* an object transform must never outlive the pass that set it */
    b3r_model(NULL);
    b3r_arrays_none();
    b3r_use_program_via(p_glUseProgram, 0);
    /* LEAVE GL EXACTLY WHERE THE LEGACY WORLD PASS LEFT IT.  Every line below
     * is one the legacy path executed, and every one of them is load-bearing
     * for the passes that follow -- the cars and the HUD read this state and
     * do not set all of it themselves:
     *   depth mask / blend / texture / alpha test : the display list's own
     *     tail, burnout3_full.c:9228-9231;
     *   the alpha FUNC: the head of that same list, :9143.  It is a global,
     *     and the retained path never needs it (the test is in the shader),
     *     but leaving it where the legacy path left it costs one call and
     *     removes a whole class of "why does the car look different" question;
     *   the texture env: b3_props_draw / b3_scenery_draw set MODULATE and do
     *     not restore it (burnout3_props.c:1374, burnout3_scenery.c:335);
     *   CULL_FACE: those two turn it off and put it back from a glIsEnabled
     *     snapshot, so the legacy pass ends with the world's own setting.
     *     Getting this one wrong drew every car double-sided -- it was worth
     *     0.33% of the pinned frame before it was found. */
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (b3r_world_cull()) glEnable(GL_CULL_FACE);
    else                  glDisable(GL_CULL_FACE);
}


/* ---- state ------------------------------------------------------------- */
void b3r_state(const B3RState* st) {
    if (g_state != 1) return;
    audit_push();   /* B3_STATE_AUDIT: prove the shadow on EVERY state call */
    if (g_c.tex != st->tex) {
        g_c.tex = st->tex;
        glBindTexture(GL_TEXTURE_2D, st->tex);
    }
    u_1i(g_u.mode, &g_c.mode, st->mode);
    u_1f(g_u.aref, &g_c.aref, st->alpha_ref);
    /* A NEGATIVE field means "leave that GL state alone".  The postfx passes
     * need it: their blend, depth and viewport state is a recovered sequence
     * of raw calls interleaved with the draws, and the batcher only has to
     * supply the shader's own knobs -- the texture, the sampling mode and the
     * alpha reference.
     *
     * THERE IS EXACTLY ONE CACHE FOR THESE FIVE FIELDS, and it is the shim's.
     * This function used to keep a SECOND one and skip the GL call when its
     * own copy already matched -- which was wrong the moment any pass set the
     * same state with a raw call, because that call did not go through here.
     * The car glass does precisely that (glDepthMask(GL_FALSE) before
     * b3_carfx_glass_begin), and the stale copy then swallowed the next
     * b3r_state() that asked for writes back ON.
     *
     * That defect was INVISIBLE while the passes bracketed themselves with
     * glPushAttrib/glPopAttrib: the pop reset the real driver state behind
     * both caches, and the cache happened to be right again by luck.  Taking
     * the attribute stack away is what exposed it -- it cost 0.5-0.8% of every
     * pinned frame, uniformly, until the two caches became one. */
    if (st->blend >= 0) {
        if (st->blend == B3R_BLEND_NONE) {
            b3r_gl_disable(GL_BLEND);
        } else {
            b3r_gl_enable(GL_BLEND);
            if (st->blend == B3R_BLEND_ADD)
                b3r_gl_blend_func(GL_ONE, GL_ONE);
            else if (st->blend == B3R_BLEND_SA_ONE)
                b3r_gl_blend_func(GL_SRC_ALPHA, GL_ONE);
            else if (st->blend == B3R_BLEND_DST_ONE)
                b3r_gl_blend_func(GL_DST_COLOR, GL_ONE);
            else
                b3r_gl_blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        g_c.blend = st->blend;
    }
    if (st->depth_mask >= 0) b3r_gl_depth_mask((unsigned char)st->depth_mask);
    if (st->depth_test >= 0) {
        if (st->depth_test) b3r_gl_enable(GL_DEPTH_TEST);
        else                b3r_gl_disable(GL_DEPTH_TEST);
    }
    if (st->depth_func) b3r_gl_depth_func((unsigned)st->depth_func);
    if (st->cull >= 0) {
        if (st->cull) b3r_gl_enable(GL_CULL_FACE);
        else          b3r_gl_disable(GL_CULL_FACE);
    }
}

void b3r_color(float r, float g, float b, float a) {
    if (g_state != 1) return;
    u_4f(g_u.color, g_c.color, r, g, b, a);
}

void b3r_uv_offset(float u, float v) {
    if (g_state != 1) return;
    u_2f(g_u.uvoff, g_c.uvoff, u, v);
}

void b3r_model(const float m[16]) {
    if (g_state != 1) return;
    /* b3r_begin() has already loaded the camera into the MODELVIEW stack, so
     * world-space geometry -- which is everything the retained path bakes --
     * needs NO matrix work at all.  Only an object still carrying its own
     * transform pushes one, and there are at most B3P_MAX_LIVE = 16 of those
     * in a frame.
     *
     * THIS USED TO PUSH THE FIXED-FUNCTION STACK, because the shader reached
     * the transform through ftransform().  It does not any more: uMVP is
     * composed here, from THIS stack, so the push has to land here or a
     * knocked prop draws at the world origin.  The pinned pixel gates never
     * caught that -- a prop is only off its baked transform after a car has
     * hit it, and no gate frame has one.  b3_props_draw() (B3P_REST vs
     * KNOCKED/SETTLED) is the path that does. */
    int mode = g_ms_which;
    g_ms_which = B3R_MAT_MODELVIEW;
    if (g_model_pushed) {
        b3r_pop();
        g_model_pushed = 0;
    }
    if (m) {
        b3r_push();
        ms_apply(m);
        g_model_pushed = 1;
    }
    g_ms_which = mode;
}

// The world fog, as FUN_00038D10 programs it (see TrackScene for the chain).
//
//   D3DRS_FOGENABLE     1                        0x00038F47
//   D3DRS_FOGTABLEMODE  3 = D3DFOG_LINEAR        0x00038F23
//   D3DRS_FOGSTART      fog_far * 0.05           0x00038ECB
//   D3DRS_FOGEND        (fog_far-start)/div+start 0x00038EFC
//   D3DRS_FOGCOLOR      authored colour * 127.5  0x00038E0E (NOT *255)
//
// and the fog COORDINATE is not the raw depth: every world vertex program
// ends with `min oFog, r12.z, c[120].z` with c[120].z = fog_far, so the
// coordinate is clamped at fog_far and the linear factor never falls below
//     f_min = (fog_end - fog_far) / (fog_end - fog_start)
// (0.75 on US_C3_V1: far 1000, start 50, end 3850).
//
// THE FOG-COORDINATE CLAMP, and how it is reproduced here.
//
// `c[120]` is uploaded by exactly ONE call site in the whole executable --
// `MOV ECX,0x78 / CALL SetVertexShaderConstant` at 0x00038F59, inside this
// same world setup (a whole-image scan for `MOV ECX,0x78` feeding either
// uploader finds one hit; scratchpad cscan.py). Its float4 is built on the
// stack at ESP+0x40..0x4C and the .z lane is written at 0x00038DD8 from XMM1,
// which 0x00038D8D loaded as `[ESI + 0x10]` -- the SAME field the very next
// block turns into D3DRS_FOGSTART and D3DRS_FOGEND:
//
//   00038d87  ADD   ESI,0x60e100            ; fog record = 0x0060E100 + env*0x40
//   00038d8d  MOVSS XMM1,[ESI + 0x10]       ; fog_far
//   00038dd8  MOVSS [ESP + 0x48],XMM1       ; c[120].z := fog_far
//   00038f59  MOV   ECX,0x78 / CALL 0x0034f840
//   00038e09  MOVSS XMM0,[ESI + 0x10]       ; fog_far
//   00038e10  MULSS XMM0,[0x003a69bc]       ;   * 0.05          -> FOGSTART
//   00038e23  SUBSS XMM1,XMM0 / DIVSS [ESI+0x14] / ADDSS XMM0   -> FOGEND
//
// So `c[120].z` IS `fog_far`, in the same units as FOGSTART/FOGEND, and the
// floor is a property of the track's own three numbers -- nothing is assumed.
// (0.75 on US_C3_V1, 0.80 on AS_C1_V1/AS_M1_V1, 0.70 on AS_C2_V1, 0.90 on
// EU_C3_V1, 0.75 on US_C1_V1.)                                          [C]
//
// MECHANISM. Fixed-function GL could not express the clamp: its linear
// factor is affine in the fragment's eye distance and clamps to 0, and no
// choice of GL_FOG_START/GL_FOG_END/GL_FOG_COLOR can make an affine ramp hold
// still at f_min while still matching the true ramp inside [start, fog_far]
// (matching the ramp fixes both endpoints, and then the clamp is 0 by
// construction). GL_FOG_COORD would express it exactly but has to be supplied
// per vertex, and the world is one baked display list.
//
// The retained world shader writes it directly: one line in the vertex
// stage, `min(|z_eye|, uFogFar)` -- the microcode's
// `MIN oFog, r12.z, c[120].z` verbatim -- and one mix() in the fragment
// stage for the ramp itself, which is the whole of GL's linear fog table.
//
// The GLUE in it is (a) GLSL instead of NV2A microcode and (b) reading the
// microcode's `r12.z` as the GL eye-space depth |z_eye|. Both fog ramps are
// programmed from the same recovered fog_start/fog_end, so inside [start,
// fog_far] this changes nothing; past fog_far it stops the factor falling.
// B3_TRACK_NOFOGFLOOR=1 turns the clamp off (`clamp` = 0 here), which is the
// unclamped ramp this port drew before the constant was recovered.
void b3r_fog(const TrackScene* sc, int black, int clamp) {
    if (g_state != 1) return;
    /* 1e30 is the identity for `min(|z|, uFogFar)`: no eye-space depth this
     * harness can produce reaches it, so the clamp does nothing. */
    u_1f(g_u.fogfar, &g_c.fogfar,
         (clamp && sc && sc->valid) ? sc->fog_far : 1e30f);
    if (!sc || !sc->valid || !sc->fog_enabled) {
        u_1i(g_u.fogon, &g_c.fogon, 0);
        return;
    }
    u_1i(g_u.fogon, &g_c.fogon, 1);
    /* GL's linear fog: f = (end - z) / (end - start), clamped to [0,1], then
     * colour = f*C + (1-f)*fogColour.  uFogSE carries (end, 1/(end-start)). */
    float range = sc->fog_end - sc->fog_start;
    u_2f(g_u.fogse, g_c.fogse, sc->fog_end,
         (range > 1e-6f || range < -1e-6f) ? 1.0f / range : 0.0f);
    if (black)
        u_3f(g_u.fogcol, g_c.fogcol, 0.0f, 0.0f, 0.0f);
    else
        u_3f(g_u.fogcol, g_c.fogcol,
             sc->fog_rgb[0], sc->fog_rgb[1], sc->fog_rgb[2]);
}

/* ---- buffers ----------------------------------------------------------- */
static unsigned vbo_make(const float* data, long bytes, unsigned usage) {
    unsigned b = 0;
    p_glGenBuffers(1, &b);
    if (!b) return 0;
    p_glBindBuffer(GL_ARRAY_BUFFER, b);
    p_glBufferData(GL_ARRAY_BUFFER, bytes, data, usage);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;   /* the binding just moved */
    return b;
}

unsigned b3r_vbo_static(const float* data, int nverts) {
    if (g_state != 1 || nverts <= 0) return 0;
    return vbo_make(data, (long)nverts * 9 * (long)sizeof(float),
                    GL_STATIC_DRAW);
}
unsigned b3r_vbo_static5(const float* data, int nverts) {
    if (g_state != 1 || nverts <= 0) return 0;
    return vbo_make(data, (long)nverts * 5 * (long)sizeof(float),
                    GL_STATIC_DRAW);
}
unsigned b3r_vbo_dynamic3(int nverts) {
    if (g_state != 1 || nverts <= 0) return 0;
    return vbo_make(NULL, (long)nverts * 3 * (long)sizeof(float),
                    GL_DYNAMIC_DRAW);
}

/* ORPHANING THE DYNAMIC BUFFERS.
 *
 * Both dynamic buffers here are written at OFFSET 0 of the SAME buffer object
 * that a draw issued moments ago is still reading: the shine colours once a
 * frame (the GPU is a frame behind at best), and the 2D batcher's vertices
 * seven or eight times WITHIN one frame, each flush overwriting the region the
 * previous flush's glDrawArrays sourced.  That is a write-after-read hazard,
 * and the GL specification's answer to it is that the driver must make the
 * earlier draw see the earlier data -- by stalling the write until the read has
 * retired, or by renaming the storage underneath.  Which one you get is the
 * driver's business, and on a browser it is ANGLE's, two abstraction layers
 * away from anything this port can see.
 *
 * glBufferData(target, size, NULL, usage) says the old contents are dead.
 * There is nothing left to wait for, so the rename is the only option left and
 * the stall cannot happen.  It costs one extra GL call per write.
 *
 * WHAT IT IS WORTH, MEASURED, and the honest answer is "nothing here".  RTX
 * 3090 through ANGLE/Vulkan, headless, eight 30-frame windows of one pinned
 * autodrive per configuration: mask 0 -> 35.6 fps, mask 1 -> 35.5, mask 3 ->
 * 37.0, against a spread INSIDE any one column of 30.6 - 46.7.  None of that
 * is signal.  Bit 0 ships on the narrow grounds that it is correct by
 * construction, costs one call a frame, and this repo cannot test the ANGLE GL
 * backend a reporting user was actually on -- not because it was measured to
 * help, because it was not.
 *
 * A BITMASK, not a flag: bit 0 is the shine colour buffer, bit 1 is the 2D
 * batcher.  Splitting them was not tidiness -- it is what caught the finding
 * recorded in b3r2d_flush(), which is why bit 1 is NOT on by default.
 *
 * B3_VBO_ORPHAN=<mask>: 0 off, 1 shine (the default), 2 the 2D batcher,
 * 3 both. */
enum { B3R_ORPHAN_SHINE = 1, B3R_ORPHAN_2D = 2 };

static int b3r_orphan_on(int which) {
    static int mask = -1;
    if (mask < 0) {
        const char* e = getenv("B3_VBO_ORPHAN");
        mask = (e && *e) ? atoi(e) : B3R_ORPHAN_SHINE;
        if (mask < 0) mask = 0;
    }
    return (mask & which) != 0;
}

void b3r_vbo_update3(unsigned vbo, const float* data, int nverts) {
    if (g_state != 1 || !vbo || nverts <= 0) return;
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (b3r_orphan_on(B3R_ORPHAN_SHINE))
        p_glBufferData(GL_ARRAY_BUFFER, (long)nverts * 3 * (long)sizeof(float),
                       NULL, GL_DYNAMIC_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0,
                      (long)nverts * 3 * (long)sizeof(float), data);
    /* the ARRAY_BUFFER binding moved under the attribute pointers */
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;
}

void b3r_vbo_free(unsigned vbo) {
    if (g_state != 1 || !vbo) return;
    p_glDeleteBuffers(1, &vbo);
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;
}

/* The world layout: pos3 uv2 col4, one buffer. */
void b3r_arrays(unsigned vbo9) {
    B3RVtxFmt f;
    f.vbo = vbo9; f.stride = 9;
    f.off_uv = 3; f.off_col = 5; f.n_col = 4; f.off_nrm = -1;
    b3r_arrays_fmt(&f);
}

/* The shine pass' split pair: pos3 uv2 static, col3 rewritten per frame.
 * SIZE 3 on the colour, because the pass it replaced used a glColor3f and the
 * component count is part of the format. */
void b3r_arrays_split(unsigned vbo5, unsigned vbo3) {
    if (g_state != 1) return;
    if (g_c.split == 1 && g_c.vbo_a == vbo5 && g_c.vbo_b == vbo3) return;
    g_c.split = 1; g_c.vbo_a = vbo5; g_c.vbo_b = vbo3;
    g_fmt_valid = 0;
    const int s5 = 5 * (int)sizeof(float);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo5);
    p_glEnableVertexAttribArray(B3R_ATTR_POS);
    p_glVertexAttribPointer(B3R_ATTR_POS, 3, GL_FLOAT, 0, s5, (const void*)0);
    p_glEnableVertexAttribArray(B3R_ATTR_UV);
    p_glVertexAttribPointer(B3R_ATTR_UV, 2, GL_FLOAT, 0, s5,
                            (const void*)(3 * sizeof(float)));
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo3);
    p_glEnableVertexAttribArray(B3R_ATTR_COL);
    p_glVertexAttribPointer(B3R_ATTR_COL, 3, GL_FLOAT, 0,
                            3 * (int)sizeof(float), (const void*)0);
    p_glDisableVertexAttribArray(B3R_ATTR_NRM);
}

/* Hand the vertex attributes back the way a caller that draws through some
 * other program expects to find them: nothing enabled and NO buffer bound.
 * That last part is not optional -- with an ARRAY_BUFFER still bound, a
 * pointer given as a CPU address is reinterpreted as a byte OFFSET into that
 * buffer. */
void b3r_arrays_none(void) {
    if (g_state != 1) return;
    p_glDisableVertexAttribArray(B3R_ATTR_POS);
    p_glDisableVertexAttribArray(B3R_ATTR_UV);
    p_glDisableVertexAttribArray(B3R_ATTR_COL);
    p_glDisableVertexAttribArray(B3R_ATTR_NRM);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;
    g_c.split = -1;
    g_fmt_valid = 0;
}

void b3r_arrays_fmt(const B3RVtxFmt* f) {
    if (g_state != 1 || !f) return;
    if (g_fmt_valid && g_c.split == 2 && !nofilter()
        && memcmp(&g_fmt_cur, f, sizeof *f) == 0) return;
    g_fmt_cur = *f;
    g_fmt_valid = 1;
    g_c.split = 2;
    g_c.vbo_a = f->vbo; g_c.vbo_b = 0;
    p_glBindBuffer(GL_ARRAY_BUFFER, f->vbo);
    const int st = f->stride * (int)sizeof(float);
    p_glEnableVertexAttribArray(B3R_ATTR_POS);
    p_glVertexAttribPointer(B3R_ATTR_POS, 3, GL_FLOAT, 0, st, (const void*)0);
    if (f->off_uv >= 0) {
        p_glEnableVertexAttribArray(B3R_ATTR_UV);
        p_glVertexAttribPointer(B3R_ATTR_UV, 2, GL_FLOAT, 0, st,
                        (const void*)((size_t)f->off_uv * sizeof(float)));
    } else {
        p_glDisableVertexAttribArray(B3R_ATTR_UV);
    }
    if (f->off_col >= 0) {
        p_glEnableVertexAttribArray(B3R_ATTR_COL);
        /* The component COUNT is part of the vertex format, not a detail: a
         * 3-component colour where the source had three is a different
         * transform path from a 4-component one, and GL fills the missing w
         * with 1.0 -- which is glColor3f's implied alpha. */
        p_glVertexAttribPointer(B3R_ATTR_COL, f->n_col ? f->n_col : 4,
                        GL_FLOAT, 0, st,
                        (const void*)((size_t)f->off_col * sizeof(float)));
    } else {
        /* NO COLOUR ARRAY.  Under fixed function that meant "use the current
         * glColor", and the current colour was white unless a caller said
         * otherwise.  A disabled GENERIC attribute does NOT default to white
         * -- it defaults to (0,0,0,1) -- so a pass that relied on the old
         * behaviour would come out BLACK.  Pin it to white here and let
         * b3r_color()'s uniform carry the tint, which is what every such pass
         * (the car glass, every postfx quad) now does. */
        p_glDisableVertexAttribArray(B3R_ATTR_COL);
        p_glVertexAttrib4f(B3R_ATTR_COL, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (f->off_nrm >= 0) {
        p_glEnableVertexAttribArray(B3R_ATTR_NRM);
        p_glVertexAttribPointer(B3R_ATTR_NRM, 3, GL_FLOAT, 0, st,
                        (const void*)((size_t)f->off_nrm * sizeof(float)));
    } else {
        p_glDisableVertexAttribArray(B3R_ATTR_NRM);
    }
}

unsigned b3r_vbo_upload(const float* data, long nfloats, int dynamic) {
    if (g_state != 1 || nfloats <= 0) return 0;
    return vbo_make(data, nfloats * (long)sizeof(float),
                    dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

void b3r_draw(int first, int count) {
    if (g_state != 1 || count <= 0) return;
    b3r_sync_matrices();
    glDrawArrays(GL_TRIANGLES, first, count);
}

void b3r_stat_set(int slot, int n) {
    if (slot >= 0 && slot < B3R_STAT_COUNT) g_stats[slot] = n;
}

/* B3_RENDER_STATS=<n>: one line every n frames with the DRAW COUNT this
 * renderer issued, per pass.  This is the number the web port's WebGL call
 * count is a small multiple of, and unlike that count it is available on
 * every target -- so a regression in the merge shows up on the desktop
 * without a browser. */
void b3r_stats_frame(void) {
    static int every = -1, n;
    if (every < 0) {
        const char* e = getenv("B3_RENDER_STATS");
        every = (e && *e) ? atoi(e) : 0;
        if (every < 0) every = 0;
    }
    if (!every) return;
    if (++n < every) return;
    /* The world passes report the LAST frame's batch count; the 2D batcher
     * accumulates (it is flushed many times a frame, from many call sites),
     * so its total is divided by the interval. */
    int hud = (b3r2d_batches() + every / 2) / every;
    n = 0;
    printf("[b3r] draws/frame: track %d + scroll %d + shine %d + scenery %d "
           "+ props %d + hud %d = %d\n",
           g_stats[B3R_STAT_TRACK], g_stats[B3R_STAT_SCROLL],
           g_stats[B3R_STAT_SHINE], g_stats[B3R_STAT_SCENERY],
           g_stats[B3R_STAT_PROPS], hud,
           g_stats[B3R_STAT_TRACK] + g_stats[B3R_STAT_SCROLL]
           + g_stats[B3R_STAT_SHINE] + g_stats[B3R_STAT_SCENERY]
           + g_stats[B3R_STAT_PROPS] + hud);
    b3r2d_reset_batches();
}

/* ====================================================================== *
 *  THE TRACK
 *
 *  THE MERGE, AND THE ORDER IT MAY NOT BREAK.
 *
 *  US_C3_V1's world is 90 246 triangles in 990 material groups over 122
 *  textures, and the legacy path draws one batch per group because a display
 *  list replays the state changes it recorded.  Measured on the shipped data:
 *  NO TWO ADJACENT GROUPS SHARE A (texture, state) PAIR, which is exactly why
 *  gl4es' own batcher could not touch them.
 *
 *  Merging them by texture is an 8x cut on the biggest single item in the
 *  frame -- but the draw ORDER of this world is load-bearing in two places:
 *
 *    * the DECAL groups (material flag bit 0x400 -> D3DRS_ZWRITEENABLE := 0,
 *      FUN_000393C0 @0x00039AF5) are road markings and blob shadows lying a
 *      hundredth of a unit above the road.  They have no depth bias to lean
 *      on; what keeps them on top is that they are drawn AFTER, with depth
 *      writes off.  trackmesh_decals_last() already sorted them to the tail.
 *    * the ALPHA-BLENDED groups compose in order with whatever is behind
 *      them, and an opaque group moved across one changes the result.
 *
 *  So the merge runs inside MAXIMAL RUNS of consecutive groups of the same
 *  class, where "class" is (writes depth AND does not blend) vs anything
 *  else.  A run of the first kind is sorted by (texture, state) -- STABLY, on
 *  the original group index, so two groups that tie keep their relative order
 *  -- and merged; a run of the second kind is left in document order and only
 *  ADJACENT groups with identical state are merged.  Nothing ever crosses a
 *  run boundary.  B3_RENDER_TRACKSORT=0 turns the in-run sort off and leaves
 *  only the adjacent merge, so the two can be compared at a pinned frame.
 *
 *  Measured on US_C3_V1 the partition is exactly two runs: groups [0, 934)
 *  are all opaque and depth-writing over 112 textures, and groups [934, 990)
 *  are the 4 blended plus the 52 decal groups over the other 10 textures --
 *  the two texture sets are disjoint.  So the head collapses 934 draws to
 *  ~112 and the tail keeps every one of its 56.
 *
 *  Opaque geometry inside one run composes order-independently under the
 *  ambient LESSEQUAL depth test, which is why the game's own material record
 *  carries no sort key (see trackmesh_group_material()'s "COPLANAR DRAW
 *  PRIORITY" note).  The residual risk is two coplanar opaque surfaces with
 *  DIFFERENT textures swapping their z-fight winner, and the pinned-frame
 *  gate is the instrument for it.
 *
 *  ANIMATED GROUPS DO NOT SPLIT A RUN.  The legacy bake `continue`s past them
 *  (burnout3_full.c:9148) and draws them in a separate later pass, so they are
 *  simply absent from the base pass' order and the groups on either side of
 *  one are already adjacent there.
 *
 *  THE BUFFER IS DE-INDEXED.  glDrawArrays over a flat triangle list needs no
 *  index buffer at all, which sidesteps the 16-bit index ceiling WebGL 1
 *  imposes without OES_element_index_uint, and costs 9.7 MB for this track.
 *  The measurement that matters says the port is draw-bound and not
 *  vertex-bound (docs/web/webprof_sweep.md section 6), so trading vertex
 *  reuse for call count is the right way round.
 * ====================================================================== */

typedef struct {
    unsigned tex;
    int      mode;
    int      blend;
    float    aref;
    int      depth_mask;
    int      first, count;      /* vertices into the pass' VBO */
} B3RBatch;

static struct {
    unsigned  vbo_base;
    B3RBatch* base;
    int       nbase;

    unsigned  vbo_scroll;
    int*      scroll_group;     /* group index, in group order       */
    int*      scroll_first;
    int*      scroll_count;
    int       nscroll;

    unsigned  vbo_shine_pos;    /* 5f static  */
    unsigned  vbo_shine_col;    /* 3f dynamic */
    float*    shine_col;        /* CPU mirror, 3 floats a vertex     */
    int       shine_verts;
    int*      shine_group;      /* group index per span              */
    int*      shine_sfirst;
    int*      shine_scount;
    int       nshine_span;
} g_t;

/* The state one group draws with.  trackmesh_group_material() is the single
 * decode of the game's material flag word -- it carries the citations -- and
 * this turns its answer into a comparable BATCH KEY. */
static void group_key(const TrackMesh* m, int g, unsigned tex, int cutout,
                      B3RBatch* out) {
    int decal = 0, blend = 0, test = 0;
    trackmesh_group_material(m, g, tex, cutout, &decal, &blend, &test);
    out->tex        = tex;
    out->mode       = tex ? B3R_TEX_MODULATE : B3R_TEX_NONE;
    out->blend      = blend ? B3R_BLEND_ALPHA : B3R_BLEND_NONE;
    /* test == -1 is B3_TRACK_ONLYMAT's "nothing passes" sentinel: GREATER
     * against a reference of 1.0, which no fragment alpha can beat. */
    out->aref       = test < 0 ? 1.0f : (test ? TRACKMESH_ALPHA_REF : -1.0f);
    out->depth_mask = decal ? 0 : 1;
    out->first = out->count = 0;
}

static int batch_same(const B3RBatch* a, const B3RBatch* b) {
    return a->tex == b->tex && a->mode == b->mode && a->blend == b->blend
        && a->aref == b->aref && a->depth_mask == b->depth_mask;
}

/* The flat-shade fallback the legacy bake used for a group with no texture: a
 * face normal turned into a grey, so an unresolved group stays legible.  Dead
 * on every shipped track (all 990 US_C3_V1 groups resolve a texture), kept
 * because a rebuilt build/ tree need not. */
static void flat_shade(const TrackMesh* m, int t, float out[3]) {
    const unsigned* idx = m->indices + (size_t)t * 3;
    const float* p0 = m->positions + (size_t)idx[0] * 3;
    const float* p1 = m->positions + (size_t)idx[1] * 3;
    const float* p2 = m->positions + (size_t)idx[2] * 3;
    float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
    float vx = p2[0]-p0[0], vy = p2[1]-p0[1], vz = p2[2]-p0[2];
    float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
    float len = sqrtf(nx*nx + ny*ny + nz*nz);
    float shade = len > 1e-6f ? (0.35f + 0.65f * fabsf(ny / len)) : 0.5f;
    out[0] = shade * 0.75f; out[1] = shade * 0.78f; out[2] = shade * 0.82f;
}

/* Write one group's triangles into an interleaved 9-float cursor. */
static int emit_group9(const TrackMesh* m, int g, float* dst, int textured) {
    const TrackMeshGroup* grp = &m->groups[g];
    const unsigned* idx = m->indices + (size_t)grp->first_triangle * 3;
    int n = grp->triangle_count * 3;
    for (int i = 0; i < n; i++) {
        unsigned v = idx[i];
        float* o = dst + (size_t)i * 9;
        const float* P = m->positions + (size_t)v * 3;
        o[0] = P[0]; o[1] = P[1]; o[2] = P[2];
        if (textured) {
            float col[4];
            trackmesh_group_vertex_color(m, g, v, col);
            o[3] = m->uvs ? m->uvs[(size_t)v * 2]     : 0.0f;
            o[4] = m->uvs ? m->uvs[(size_t)v * 2 + 1] : 0.0f;
            o[5] = col[0]; o[6] = col[1]; o[7] = col[2]; o[8] = col[3];
        } else {
            float sh[3];
            flat_shade(m, grp->first_triangle + i / 3, sh);
            o[3] = o[4] = 0.0f;
            o[5] = sh[0]; o[6] = sh[1]; o[7] = sh[2]; o[8] = 1.0f;
        }
    }
    return n;
}

void b3r_track_free(void) {
    if (g_state == 1) {
        if (g_t.vbo_base)      b3r_vbo_free(g_t.vbo_base);
        if (g_t.vbo_scroll)    b3r_vbo_free(g_t.vbo_scroll);
        if (g_t.vbo_shine_pos) b3r_vbo_free(g_t.vbo_shine_pos);
        if (g_t.vbo_shine_col) b3r_vbo_free(g_t.vbo_shine_col);
    }
    free(g_t.base);
    free(g_t.scroll_group); free(g_t.scroll_first); free(g_t.scroll_count);
    free(g_t.shine_col);
    free(g_t.shine_group); free(g_t.shine_sfirst); free(g_t.shine_scount);
    memset(&g_t, 0, sizeof g_t);
}

/* The in-run order: texture first (that is what a batch is), then the rest of
 * the state (so one texture drawn with two different alpha tests still ends
 * up as two adjacent batches rather than many), then the original group index
 * -- which is what makes the sort STABLE. */
typedef struct {
    unsigned tex;
    int      blend, dmask;
    float    aref;
    int      idx;
} B3RSortKey;

static int sort_key_cmp(const void* a, const void* b) {
    const B3RSortKey* x = (const B3RSortKey*)a;
    const B3RSortKey* y = (const B3RSortKey*)b;
    if (x->tex   != y->tex)   return x->tex   < y->tex   ? -1 : 1;
    if (x->blend != y->blend) return x->blend < y->blend ? -1 : 1;
    if (x->dmask != y->dmask) return x->dmask < y->dmask ? -1 : 1;
    if (x->aref  != y->aref)  return x->aref  < y->aref  ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

int b3r_track_build(TrackMesh* m, const unsigned* tex,
                    const unsigned char* cutout) {
    if (g_state != 1 || !m) return 0;
    b3r_track_free();
    int ng = m->group_count;
    if (ng <= 0) return 0;

    int do_sort = 1;
    {   const char* e = getenv("B3_RENDER_TRACKSORT");
        if (e && strcmp(e, "0") == 0) do_sort = 0;
    }

    /* ---- classify ----------------------------------------------------- */
    B3RBatch* key  = (B3RBatch*)calloc((size_t)ng, sizeof(B3RBatch));
    char*     anim = (char*)calloc((size_t)ng, 1);
    int*      ord  = (int*)malloc((size_t)ng * sizeof(int));  /* base order */
    if (!key || !anim || !ord) { free(key); free(anim); free(ord); return 0; }
    long nbase_v = 0, nscroll_v = 0, nshine_v = 0;
    int nord = 0, nscroll = 0, nshine_span = 0;
    for (int g = 0; g < ng; g++) {
        const TrackMeshGroup* grp = &m->groups[g];
        if (grp->triangle_count <= 0) continue;
        group_key(m, g, tex ? tex[g] : 0, cutout ? cutout[g] : 0, &key[g]);
        anim[g] = (char)trackmesh_group_animated(m, g);
        if (anim[g]) { nscroll_v += grp->triangle_count * 3; nscroll++; }
        else { nbase_v += grp->triangle_count * 3; ord[nord++] = g; }
        if (grp->shine_strength > 0.0f && grp->gl_texture) {
            nshine_v += grp->triangle_count * 3;
            nshine_span++;
        }
    }

    /* ---- the base pass: runs, sorted and merged ----------------------- */
    float*      buf     = (float*)malloc((size_t)nbase_v * 9 * sizeof(float));
    B3RBatch*   batches = (B3RBatch*)malloc((size_t)(nord ? nord : 1)
                                            * sizeof(B3RBatch));
    B3RSortKey* run     = (B3RSortKey*)malloc((size_t)(nord ? nord : 1)
                                              * sizeof(B3RSortKey));
    if (!buf || !batches || !run) {
        free(buf); free(batches); free(run); free(key); free(anim); free(ord);
        return 0;
    }
    int nb = 0;
    long cursor = 0;
    int i0 = 0;
    while (i0 < nord) {
        int opaque = key[ord[i0]].blend == B3R_BLEND_NONE
                  && key[ord[i0]].depth_mask;
        int i1 = i0;
        while (i1 < nord) {
            int o = key[ord[i1]].blend == B3R_BLEND_NONE
                 && key[ord[i1]].depth_mask;
            if (o != opaque) break;
            i1++;
        }
        int n = i1 - i0;
        for (int i = 0; i < n; i++) {
            int g = ord[i0 + i];
            run[i].tex   = key[g].tex;
            run[i].blend = key[g].blend;
            run[i].dmask = key[g].depth_mask;
            run[i].aref  = key[g].aref;
            run[i].idx   = g;
        }
        if (opaque && do_sort && n > 1)
            qsort(run, (size_t)n, sizeof(B3RSortKey), sort_key_cmp);
        for (int i = 0; i < n; i++) {
            int g = run[i].idx;
            int nv = emit_group9(m, g, buf + cursor * 9, key[g].tex != 0);
            if (nb > 0 && batch_same(&batches[nb - 1], &key[g])
                && batches[nb - 1].first + batches[nb - 1].count
                   == (int)cursor) {
                batches[nb - 1].count += nv;
            } else {
                batches[nb] = key[g];
                batches[nb].first = (int)cursor;
                batches[nb].count = nv;
                nb++;
            }
            cursor += nv;
        }
        i0 = i1;
    }
    g_t.vbo_base = b3r_vbo_static(buf, (int)cursor);
    free(buf);
    free(run);
    g_t.base  = batches;
    g_t.nbase = nb;

    /* ---- the animated groups, in group order -------------------------- */
    if (nscroll > 0) {
        float* sb = (float*)malloc((size_t)nscroll_v * 9 * sizeof(float));
        g_t.scroll_group = (int*)malloc((size_t)nscroll * sizeof(int));
        g_t.scroll_first = (int*)malloc((size_t)nscroll * sizeof(int));
        g_t.scroll_count = (int*)malloc((size_t)nscroll * sizeof(int));
        if (sb && g_t.scroll_group && g_t.scroll_first && g_t.scroll_count) {
            long c = 0;
            int k = 0;
            for (int g = 0; g < ng; g++) {
                if (!anim[g] || m->groups[g].triangle_count <= 0) continue;
                int nv = emit_group9(m, g, sb + c * 9, key[g].tex != 0);
                g_t.scroll_group[k] = g;
                g_t.scroll_first[k] = (int)c;
                g_t.scroll_count[k] = nv;
                k++;
                c += nv;
            }
            g_t.nscroll = k;
            g_t.vbo_scroll = b3r_vbo_static(sb, (int)c);
        }
        free(sb);
    }

    /* ---- the shine pass ------------------------------------------------ *
     * Its colour is recomputed against the live camera every frame, so the
     * position/uv half is static and only the colour half moves.  Sorting is
     * unconditional here: the pass is additive with depth writes off, which
     * composes order-independently by construction. */
    if (nshine_span > 0) {
        float*      sp = (float*)malloc((size_t)nshine_v * 5 * sizeof(float));
        B3RSortKey* sk = (B3RSortKey*)malloc((size_t)nshine_span
                                             * sizeof(B3RSortKey));
        g_t.shine_group  = (int*)malloc((size_t)nshine_span * sizeof(int));
        g_t.shine_sfirst = (int*)malloc((size_t)nshine_span * sizeof(int));
        g_t.shine_scount = (int*)malloc((size_t)nshine_span * sizeof(int));
        g_t.shine_col    = (float*)malloc((size_t)nshine_v * 3 * sizeof(float));
        if (sp && sk && g_t.shine_group && g_t.shine_sfirst
            && g_t.shine_scount && g_t.shine_col) {
            int k = 0;
            for (int g = 0; g < ng; g++) {
                const TrackMeshGroup* grp = &m->groups[g];
                if (grp->triangle_count <= 0) continue;
                if (grp->shine_strength <= 0.0f || !grp->gl_texture) continue;
                sk[k].tex = grp->gl_texture;
                sk[k].blend = 0; sk[k].dmask = 0; sk[k].aref = 0.0f;
                sk[k].idx = g;
                k++;
            }
            if (do_sort && k > 1)
                qsort(sk, (size_t)k, sizeof(B3RSortKey), sort_key_cmp);
            long c = 0;
            for (int i = 0; i < k; i++) {
                int g = sk[i].idx;
                const TrackMeshGroup* grp = &m->groups[g];
                const unsigned* idx = m->indices
                                    + (size_t)grp->first_triangle * 3;
                int nv = grp->triangle_count * 3;
                for (int j = 0; j < nv; j++) {
                    unsigned v = idx[j];
                    float* o = sp + (c + j) * 5;
                    const float* P = m->positions + (size_t)v * 3;
                    o[0] = P[0]; o[1] = P[1]; o[2] = P[2];
                    o[3] = m->uvs[(size_t)v * 2];
                    o[4] = m->uvs[(size_t)v * 2 + 1];
                }
                g_t.shine_group[i]  = g;
                g_t.shine_sfirst[i] = (int)c;
                g_t.shine_scount[i] = nv;
                c += nv;
            }
            g_t.nshine_span   = k;
            g_t.shine_verts   = (int)c;
            g_t.vbo_shine_pos = b3r_vbo_static5(sp, (int)c);
            g_t.vbo_shine_col = b3r_vbo_dynamic3((int)c);
        }
        free(sp);
        free(sk);
    }

    free(key);
    free(anim);
    free(ord);
    printf("[b3r] track: %d groups -> %d base batches (%ld verts), "
           "%d animated, %d shine spans (%d verts)\n",
           ng, g_t.nbase, cursor, g_t.nscroll, g_t.nshine_span,
           g_t.shine_verts);
    return g_t.vbo_base != 0;
}

void b3r_track_draw(const TrackMesh* m) {
    (void)m;
    if (g_state != 1 || !g_t.vbo_base) return;
    b3r_arrays(g_t.vbo_base);
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    b3r_uv_offset(0.0f, 0.0f);
    B3RState st;
    st.depth_test = 1;
    st.depth_func = GL_LEQUAL;
    st.cull       = b3r_world_cull();
                            /* trackmesh_gl_single_sided(); the front face is
                             * CCW here because the projection carries the
                             * display mirror (burnout3_full.c:14569-14570) */
    for (int i = 0; i < g_t.nbase; i++) {
        const B3RBatch* b = &g_t.base[i];
        st.tex        = b->tex;
        st.mode       = b->mode;
        st.blend      = b->blend;
        st.alpha_ref  = b->aref;
        st.depth_mask = b->depth_mask;
        b3r_state(&st);
        b3r_draw(b->first, b->count);
    }
    b3r_stat_set(B3R_STAT_TRACK, g_t.nbase);
}

int b3r_track_draw_scroll(const TrackMesh* m) {
    if (g_state != 1 || !g_t.vbo_scroll || g_t.nscroll <= 0) return 0;
    b3r_arrays(g_t.vbo_scroll);
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    B3RState st;
    st.depth_test = 1;
    st.depth_func = GL_LEQUAL;
    st.cull       = b3r_world_cull();
    int drawn = 0, batches = 0;
    for (int k = 0; k < g_t.nscroll; k++) {
        int g = g_t.scroll_group[k];
        const TrackMeshGroup* grp = &m->groups[g];
        B3RBatch key;
        /* A frame-cycling group binds THIS FRAME's texture; a scrolling one
         * binds its only one and moves the UVs instead. */
        group_key(m, g, trackmesh_group_texture(m, g), 0, &key);
        st.tex        = key.tex;
        st.mode       = key.mode;
        st.blend      = key.blend;
        st.alpha_ref  = key.aref;
        st.depth_mask = key.depth_mask;
        b3r_state(&st);
        b3r_uv_offset(grp->uv_offset[0], grp->uv_offset[1]);
        b3r_draw(g_t.scroll_first[k], g_t.scroll_count[k]);
        drawn += grp->triangle_count;
        batches++;
    }
    b3r_uv_offset(0.0f, 0.0f);
    b3r_stat_set(B3R_STAT_SCROLL, batches);
    return drawn;
}

/* x^p for x in [0,1], p > 0, to ~2% -- the same exp2/log2 bit trick
 * the fixed-function shine pass used, and the SAME arithmetic in the same
 * order, so
 * the retained pass produces the same colours as the legacy one bit for bit. */
static float b3r_fast_powf(float x, float p) {
    union { float f; unsigned u; } v;
    if (x <= 0.0f) return 0.0f;
    v.f = x;
    float lg = (float)v.u * (1.0f / 8388608.0f) - 127.0f;
    float frac = lg - (float)(int)lg;
    if (frac < 0.0f) frac += 1.0f;
    lg -= (frac - frac * frac) * 0.346607f;
    float e = lg * p;
    if (e < -60.0f) return 0.0f;
    if (e > 0.0f) return 1.0f;
    float ef = e - (float)(int)e;
    if (ef < 0.0f) ef += 1.0f;
    e += (ef - ef * ef) * 0.346607f;
    v.u = (unsigned)((e + 127.0f) * 8388608.0f);
    return v.f;
}

// The class-1/7/10 additive term, as its own pass.  Lifted verbatim from
// trackmesh_draw_shine() when the renderer became retained -- the
// arithmetic below is the same arithmetic, drawn from a static position
// buffer with a per-frame colour buffer instead of from a CPU scratch.
//
// THE MECHANISM (tools/extract_track.py's "Shader classes" section carries the
// byte-level citations). The retail world pixel shaders are single-stage
// register-combiner programs; the class-1 one (D3DPIXELSHADERDEF at
// 0x003E8EE8, bound from slot 0x004D6578 by FUN_000393C0) has TWO stages:
//
//     stage 0: R0.rgb = 2 * (T0.rgb * V0.rgb)    R0.a = T0.a * V0.a
//     stage 1: R0.rgb = R0.a * C0.rgb + R0.rgb
//
// so the surface is its texture modulated by the vertex colour, PLUS an
// additive term masked per texel by the texture's alpha channel. V0 comes from
// the class-1 vertex program at 0x003E88C0, which is the class-0 program plus
// a Phong specular: it builds R = 2*N*(N.L) - L from the NORMPACKED3 normal at
// vertex +0x0C and the light direction in constant c[1], normalises the view
// vector V = position - c[0], and evaluates LIT(R.V) with the exponent taken
// from constant 0x62.w = the material's +0x08. oD0.rgb is the plain vertex
// colour; only oD0.w carries the specular, and material flag bit 0x40 selects
// the variant that multiplies it by the artist-painted vertex alpha.
// C0.rgb is the scene light colour (DAT_0060E0A0..AC) times material +0x04.
//
// So: colour = 2*tex.rgb*vcol.rgb + tex.a * gate * pow(max(R.V,0),power)
//                                         * light.rgb * strength.
//
// The first term is the ordinary modulated base pass; this function draws the
// second. In GL that is an additive pass (GL_ONE/GL_ONE, depth test LEQUAL,
// depth writes off) whose fragment colour is tex.alpha * primary colour, which
// GL_COMBINE expresses exactly as MODULATE(TEXTURE.alpha, PRIMARY.rgb) -- and
// the per-vertex primary colour is light*strength*gate*specular, computed here
// because the specular is view dependent and cannot live in a display list.
//
// NO LONGER GLUE. The light direction is vertex-shader constant c[0x61], which
// FUN_00038D10 fills at 0x00038D62..0x00038D92 by negating the float4 at
// +0x00 of the active light record (DAT_0060E0F0 + DAT_0060E170*0x40) -- and
// that float4 is enviro.dat +0x80, normalised, copied there by FUN_001888F0's
// 2-iteration loop at 0x00188B40. The scene light colour is DAT_0060E0A0 =
// enviro.dat +0x60. tools/extract_track.py's parse_enviro() reads both out of
// the track's own enviro.dat and writes them into the MTL header, so the
// values below are the game's, per track. B3_TRACK_SUN still overrides.
int b3r_track_draw_shine(TrackMesh* m, const float eye[3],
                         const float light_dir[3]) {
    if (g_state != 1 || !g_t.vbo_shine_pos || g_t.nshine_span <= 0) return 0;
    if (!m->colors || !m->normals) return 0;
    if (getenv("B3_TRACK_NOSHINE")) return 0;

    /* The light, exactly as trackmesh_draw_shine() resolves it. */
    float L[3] = { 0.900f, -0.350f, -0.250f };
    float LC[3] = { 1.0f, 1.0f, 1.0f };
    int have_light = 1;
    if (m->scene.valid) {
        L[0] = m->scene.light_dir[0]; L[1] = m->scene.light_dir[1];
        L[2] = m->scene.light_dir[2];
        LC[0] = m->scene.light_rgb[0]; LC[1] = m->scene.light_rgb[1];
        LC[2] = m->scene.light_rgb[2];
    }
    if (light_dir) { L[0] = light_dir[0]; L[1] = light_dir[1];
                     L[2] = light_dir[2]; }
    {   const char* e = getenv("B3_TRACK_SUN");
        if (e && strcmp(e, "none") == 0) have_light = 0;
        else if (e && sscanf(e, "%f,%f,%f", &L[0], &L[1], &L[2]) == 3)
            have_light = 1;
    }
    if (have_light) {
        float n = sqrtf(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]);
        if (n < 1e-6f) return 0;
        L[0] /= n; L[1] /= n; L[2] /= n;
    }
    /* c[96] is the camera position with 5.0 added to .y (0x00038F71). */
    float E[3] = { eye[0], eye[1] + 5.0f, eye[2] };

    int drawn = 0;
    for (int s = 0; s < g_t.nshine_span; s++) {
        int g = g_t.shine_group[s];
        const TrackMeshGroup* grp = &m->groups[g];
        const unsigned* idx = m->indices + (size_t)grp->first_triangle * 3;
        int base = g_t.shine_sfirst[s];
        int nv   = g_t.shine_scount[s];
        for (int i = 0; i < nv; i++) {
            unsigned v = idx[i];
            float* o = g_t.shine_col + (size_t)(base + i) * 3;
            const float* P = m->positions + (size_t)v * 3;
            const float* N = m->normals   + (size_t)v * 3;
            const float* C = m->colors    + (size_t)v * 4;
            float gate = grp->shine_gate ? C[3] : 1.0f;
            float sv = 0.0f;
            if (gate > 0.0f) {
                if (grp->shine_power <= 0.0f || !have_light) {
                    sv = 1.0f;
                } else {
                    float ndl = N[0]*L[0] + N[1]*L[1] + N[2]*L[2];
                    float R[3] = { 2.0f*N[0]*ndl - L[0],
                                   2.0f*N[1]*ndl - L[1],
                                   2.0f*N[2]*ndl - L[2] };
                    float V[3] = { P[0]-E[0], P[1]-E[1], P[2]-E[2] };
                    float vl = sqrtf(V[0]*V[0] + V[1]*V[1] + V[2]*V[2]);
                    if (vl > 1e-6f) {
                        float rv = (R[0]*V[0] + R[1]*V[1] + R[2]*V[2]) / vl;
                        if (rv > 0.0f) sv = b3r_fast_powf(rv, grp->shine_power);
                    }
                }
            }
            sv *= gate * grp->shine_strength;
            o[0] = sv * LC[0]; o[1] = sv * LC[1]; o[2] = sv * LC[2];
        }
        drawn += grp->triangle_count;
    }
    /* B3_TRACK_SHINE_STATS=1: what the specular term actually evaluates to
     * this frame.  Carried over from the fixed-function pass it replaced --
     * losing a diagnostic in a migration is a regression like any other -- and
     * extended with a CHECKSUM of the colour buffer, which is the cheapest way
     * to tell an input difference (the eye moved) from a downstream one. */
    if (getenv("B3_TRACK_SHINE_STATS")) {
        double sum = 0.0;
        float  mx = 0.0f;
        unsigned long long ck = 1469598103934665603ULL;   /* FNV-1a */
        const unsigned char* raw = (const unsigned char*)g_t.shine_col;
        long nb = (long)g_t.shine_verts * 3 * (long)sizeof(float);
        for (long i = 0; i < (long)g_t.shine_verts * 3; i++) {
            sum += g_t.shine_col[i];
            if (g_t.shine_col[i] > mx) mx = g_t.shine_col[i];
        }
        for (long i = 0; i < nb; i++) {
            ck ^= raw[i];
            ck *= 1099511628211ULL;
        }
        fprintf(stderr, "[shine] eye=(%.6f %.6f %.6f) L=(%.6f %.6f %.6f) "
                "verts=%d mean=%.8f max=%.8f fnv=%016llx\n",
                (double)E[0], (double)E[1], (double)E[2],
                (double)L[0], (double)L[1], (double)L[2],
                g_t.shine_verts,
                g_t.shine_verts ? sum / (double)(g_t.shine_verts * 3) : 0.0,
                (double)mx, ck);
    }
    b3r_vbo_update3(g_t.vbo_shine_col, g_t.shine_col, g_t.shine_verts);
    b3r_arrays_split(g_t.vbo_shine_pos, g_t.vbo_shine_col);
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    b3r_uv_offset(0.0f, 0.0f);
    /* FOG ON AN ADDITIVE PASS.  In retail this term is inside the same pixel
     * shader result the final combiner fogs; split into two GL passes, this
     * one must contribute f*spec and add NO fog colour, which is the same ramp
     * against black.  NO COORDINATE CLAMP: the legacy pass runs AFTER
     * trackmesh_fog_end() has unbound the fog vertex program
     * (burnout3_full.c:14630 then :14636), so its fog coordinate is plain
     * fixed-function eye distance.  Reproducing that exactly is what keeps the
     * pinned frame identical; closing the gap belongs with the fidelity work,
     * not with the migration. */
    b3r_fog(&m->scene, 1, 0);

    B3RState st;
    st.depth_test = 1;
    st.depth_func = GL_LEQUAL;
    st.cull       = b3r_world_cull();
    st.mode       = B3R_TEX_SHINE;
    st.blend      = B3R_BLEND_ADD;
    st.alpha_ref  = -1.0f;
    st.depth_mask = 0;
    /* A frame-cycling material's alpha mask changes with the frame, so bind
     * the frame the base pass just drew rather than frame 0's texture.  That
     * can split a merged batch, so the runs are re-merged here, per frame. */
    int batches = 0, s = 0;
    while (s < g_t.nshine_span) {
        unsigned t = trackmesh_group_texture(m, g_t.shine_group[s]);
        int first = g_t.shine_sfirst[s];
        int count = g_t.shine_scount[s];
        int e = s + 1;
        while (e < g_t.nshine_span
               && trackmesh_group_texture(m, g_t.shine_group[e]) == t
               && g_t.shine_sfirst[e] == first + count) {
            count += g_t.shine_scount[e];
            e++;
        }
        st.tex = t;
        b3r_state(&st);
        b3r_draw(first, count);
        batches++;
        s = e;
    }
    /* Put the world fog back the way the base pass wants it, so a caller that
     * draws more world geometry after the shine does not inherit black fog. */
    b3r_fog(&m->scene, 0, 1);
    b3r_stat_set(B3R_STAT_SHINE, batches);
    return drawn;
}

/* ====================================================================== *
 *  INSTANCED STATIC MODELS  (scenery, and the resting props)
 *
 *  523 scenery draws a frame is 22% of the port's WebGL traffic, and every
 *  one of them is `glPushMatrix / glMultMatrixf / glCallList / glPopMatrix`
 *  around ~110 triangles.  The transforms never change, so they do not belong
 *  in the frame at all: bake them.
 *
 *  ORDER.  The instances are laid out model-major, so one texture and one
 *  material state cover a whole span, and the BLENDED models are moved to the
 *  very end -- which is where they belong anyway (they write depth, so an
 *  opaque instance drawn after a blended one that covers it would be
 *  depth-rejected instead of composed).  Inside one model the instances are
 *  ordered by a Z-ORDER CURVE on their world (x, z): the per-instance LOD cull
 *  is a sphere about the eye, and a spatially sorted list turns that sphere
 *  into a handful of contiguous runs instead of 500 scattered singletons.
 *  That is the difference between ~40 draws and ~500.
 * ====================================================================== */

typedef struct {
    int      model;
    int      first, count;      /* vertices in the baked buffer */
    float    pos[3];
    float    cull2;             /* squared cull distance, 0 = never */
    int      src;               /* original instance index          */
} B3RInst;

struct B3RInstSet {
    unsigned  vbo_world;        /* every instance, baked, 9f            */
    unsigned  vbo_model;        /* the models in model space, 9f        */
    int*      model_first;      /* vertex span of each model in vbo_model */
    int*      model_count;
    unsigned* model_tex;
    unsigned* model_flags;
    int       nmodels;
    B3RInst*  inst;             /* in DRAW order                        */
    int       ninst;
    int*      by_src;           /* original index -> draw slot          */
};

/* Interleave 16 bits of x and 16 of y -- the classic Morton code. */
static unsigned morton2(unsigned x, unsigned y) {
    unsigned r = 0;
    for (int i = 0; i < 16; i++)
        r |= ((x >> i) & 1u) << (2 * i) | ((y >> i) & 1u) << (2 * i + 1);
    return r;
}

typedef struct { int model, blended; unsigned key; int src; } B3RInstSort;
static int inst_cmp(const void* a, const void* b) {
    const B3RInstSort* x = (const B3RInstSort*)a;
    const B3RInstSort* y = (const B3RInstSort*)b;
    if (x->blended != y->blended) return x->blended < y->blended ? -1 : 1;
    if (x->model   != y->model)   return x->model   < y->model   ? -1 : 1;
    if (x->key     != y->key)     return x->key     < y->key     ? -1 : 1;
    return x->src < y->src ? -1 : (x->src > y->src);
}

B3RInstSet* b3r_inst_build(const B3RMeshSrc* mesh,
                           const B3RModelSrc* models, int nmodels,
                           const B3RInstSrc* inst, int ninst) {
    if (g_state != 1 || !mesh || !models || nmodels <= 0) return NULL;
    B3RInstSet* s = (B3RInstSet*)calloc(1, sizeof(B3RInstSet));
    if (!s) return NULL;
    s->nmodels = nmodels;
    s->model_first = (int*)calloc((size_t)nmodels, sizeof(int));
    s->model_count = (int*)calloc((size_t)nmodels, sizeof(int));
    s->model_tex   = (unsigned*)calloc((size_t)nmodels, sizeof(unsigned));
    s->model_flags = (unsigned*)calloc((size_t)nmodels, sizeof(unsigned));
    if (!s->model_first || !s->model_count || !s->model_tex
        || !s->model_flags) { b3r_inst_free(s); return NULL; }

    /* ---- the model-space buffer --------------------------------------- */
    long mv = 0;
    for (int i = 0; i < nmodels; i++) mv += models[i].n_index;
    float* mb = (float*)malloc((size_t)(mv ? mv : 1) * 9 * sizeof(float));
    if (!mb) { b3r_inst_free(s); return NULL; }
    long c = 0;
    for (int i = 0; i < nmodels; i++) {
        const B3RModelSrc* md = &models[i];
        s->model_tex[i]   = md->tex;
        s->model_flags[i] = md->mat_flags;
        s->model_first[i] = (int)c;
        for (unsigned k = 0; k < md->n_index; k++) {
            unsigned vi = md->first_vertex + mesh->idx[md->first_index + k];
            float* o = mb + c * 9;
            if (vi >= mesh->nvtx) { o[0]=o[1]=o[2]=o[3]=o[4]=0.0f;
                                    o[5]=o[6]=o[7]=o[8]=1.0f; c++; continue; }
            const float* v = mesh->vtx + (size_t)vi * mesh->stride;
            o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
            o[3] = v[mesh->uv_off]; o[4] = v[mesh->uv_off + 1];
            o[5] = o[6] = o[7] = o[8] = 1.0f;
            c++;
        }
        s->model_count[i] = (int)c - s->model_first[i];
    }
    s->vbo_model = b3r_vbo_static(mb, (int)c);
    free(mb);

    if (ninst <= 0 || !inst) return s;

    /* ---- the instance draw order -------------------------------------- */
    B3RInstSort* srt = (B3RInstSort*)malloc((size_t)ninst
                                            * sizeof(B3RInstSort));
    s->inst   = (B3RInst*)calloc((size_t)ninst, sizeof(B3RInst));
    s->by_src = (int*)malloc((size_t)ninst * sizeof(int));
    if (!srt || !s->inst || !s->by_src) {
        free(srt); b3r_inst_free(s); return NULL;
    }
    /* the world bound, so the Morton quantisation has a scale */
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (int i = 0; i < ninst; i++) {
        float x = inst[i].m[12], z = inst[i].m[14];
        if (x < lo[0]) lo[0] = x;
        if (x > hi[0]) hi[0] = x;
        if (z < lo[1]) lo[1] = z;
        if (z > hi[1]) hi[1] = z;
    }
    float sx = (hi[0] - lo[0]) > 1e-3f ? 65535.0f / (hi[0] - lo[0]) : 0.0f;
    float sz = (hi[1] - lo[1]) > 1e-3f ? 65535.0f / (hi[1] - lo[1]) : 0.0f;
    for (int i = 0; i < ninst; i++) {
        unsigned mo = inst[i].model < (unsigned)nmodels ? inst[i].model : 0u;
        unsigned qx = (unsigned)((inst[i].m[12] - lo[0]) * sx);
        unsigned qz = (unsigned)((inst[i].m[14] - lo[1]) * sz);
        srt[i].model   = (int)mo;
        srt[i].blended = (models[mo].mat_flags & 0x001u) ? 1 : 0;
        srt[i].key     = morton2(qx > 65535u ? 65535u : qx,
                                 qz > 65535u ? 65535u : qz);
        srt[i].src     = i;
    }
    qsort(srt, (size_t)ninst, sizeof(B3RInstSort), inst_cmp);

    /* ---- bake ---------------------------------------------------------- */
    long wv = 0;
    for (int i = 0; i < ninst; i++) wv += models[srt[i].model].n_index;
    float* wb = (float*)malloc((size_t)(wv ? wv : 1) * 9 * sizeof(float));
    if (!wb) { free(srt); b3r_inst_free(s); return NULL; }
    c = 0;
    for (int i = 0; i < ninst; i++) {
        int mo  = srt[i].model;
        int src = srt[i].src;
        const B3RInstSrc* in = &inst[src];
        const B3RModelSrc* md = &models[mo];
        s->inst[i].model = mo;
        s->inst[i].first = (int)c;
        s->inst[i].src   = src;
        s->inst[i].pos[0] = in->m[12];
        s->inst[i].pos[1] = in->m[13];
        s->inst[i].pos[2] = in->m[14];
        s->inst[i].cull2  = in->cull_far > 0.0f
                          ? in->cull_far * in->cull_far : 0.0f;
        s->by_src[src] = i;
        for (unsigned k = 0; k < md->n_index; k++) {
            unsigned vi = md->first_vertex + mesh->idx[md->first_index + k];
            float* o = wb + c * 9;
            c++;
            if (vi >= mesh->nvtx) { o[0]=o[1]=o[2]=o[3]=o[4]=0.0f;
                                    o[5]=o[6]=o[7]=o[8]=1.0f; continue; }
            const float* v = mesh->vtx + (size_t)vi * mesh->stride;
            /* the instance transform, applied once and for all */
            o[0] = in->m[0]*v[0] + in->m[4]*v[1] + in->m[8]*v[2]  + in->m[12];
            o[1] = in->m[1]*v[0] + in->m[5]*v[1] + in->m[9]*v[2]  + in->m[13];
            o[2] = in->m[2]*v[0] + in->m[6]*v[1] + in->m[10]*v[2] + in->m[14];
            o[3] = v[mesh->uv_off]; o[4] = v[mesh->uv_off + 1];
            /* the per-instance half-range colour, baked into the vertex --
             * the legacy path spent one glColor4f a frame on it */
            o[5] = in->tint[0]; o[6] = in->tint[1]; o[7] = in->tint[2];
            o[8] = 1.0f;
        }
        s->inst[i].count = (int)c - s->inst[i].first;
    }
    s->ninst = ninst;
    s->vbo_world = b3r_vbo_static(wb, (int)c);
    free(wb);
    free(srt);
    return s;
}

void b3r_inst_free(B3RInstSet* s) {
    if (!s) return;
    if (g_state == 1) {
        if (s->vbo_world) b3r_vbo_free(s->vbo_world);
        if (s->vbo_model) b3r_vbo_free(s->vbo_model);
    }
    free(s->model_first); free(s->model_count);
    free(s->model_tex);   free(s->model_flags);
    free(s->inst);        free(s->by_src);
    free(s);
}

/* material +0x24: bit 0x010 = D3DRS_ALPHATESTENABLE with the world's fixed
 * GREATER 64/255, bit 0x001 = D3DRS_ALPHABLENDENABLE (extract_track.py's
 * "FLAG BITS"). */
static void inst_state(unsigned tex, unsigned flags, B3RState* st) {
    st->tex        = tex;
    st->mode       = B3R_TEX_MODULATE;
    st->blend      = (flags & 0x001u) ? B3R_BLEND_ALPHA : B3R_BLEND_NONE;
    st->alpha_ref  = (flags & 0x010u) ? (64.0f / 255.0f) : -1.0f;
    st->depth_mask = 1;
    st->depth_test = 1;
    st->depth_func = GL_LEQUAL;
    /* Culling stays off: the Z reflection flips every winding, and these are
     * cut-out cards anyway (material bit 0x020 = D3DCULL_NONE on the
     * tree/foliage materials). */
    st->cull       = 0;
}

int b3r_inst_draw(B3RInstSet* s, const float eye[3],
                  const unsigned char* skip) {
    if (g_state != 1 || !s || !s->vbo_world || s->ninst <= 0) return 0;
    b3r_arrays(s->vbo_world);
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    b3r_uv_offset(0.0f, 0.0f);
    B3RState st;
    int batches = 0;
    int i = 0;
    while (i < s->ninst) {
        const B3RInst* p = &s->inst[i];
        int live = 1;
        if (skip && skip[p->src]) live = 0;
        if (live && eye && p->cull2 > 0.0f) {
            float dx = p->pos[0] - eye[0];
            float dy = p->pos[1] - eye[1];
            float dz = p->pos[2] - eye[2];
            if (dx*dx + dy*dy + dz*dz > p->cull2) live = 0;
        }
        if (!live) { i++; continue; }
        /* extend the run over every following instance of the same model that
         * is also visible and immediately adjacent in the baked buffer */
        int first = p->first, count = p->count, model = p->model;
        int j = i + 1;
        while (j < s->ninst) {
            const B3RInst* q = &s->inst[j];
            if (q->model != model || q->first != first + count) break;
            if (skip && skip[q->src]) break;
            if (eye && q->cull2 > 0.0f) {
                float dx = q->pos[0] - eye[0];
                float dy = q->pos[1] - eye[1];
                float dz = q->pos[2] - eye[2];
                if (dx*dx + dy*dy + dz*dz > q->cull2) break;
            }
            count += q->count;
            j++;
        }
        inst_state(s->model_tex[model], s->model_flags[model], &st);
        b3r_state(&st);
        b3r_draw(first, count);
        batches++;
        i = j;
    }
    return batches;
}

void b3r_inst_draw_model(B3RInstSet* s, int model, const float m[16],
                         const float tint[3]) {
    if (g_state != 1 || !s || !s->vbo_model) return;
    if (model < 0 || model >= s->nmodels || s->model_count[model] <= 0) return;
    b3r_arrays(s->vbo_model);
    b3r_model(m);
    if (tint) b3r_color(tint[0], tint[1], tint[2], 1.0f);
    else      b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    b3r_uv_offset(0.0f, 0.0f);
    B3RState st;
    inst_state(s->model_tex[model], s->model_flags[model], &st);
    b3r_state(&st);
    b3r_draw(s->model_first[model], s->model_count[model]);
    /* leave the world transform where the merged pass expects it */
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
}

/* ====================================================================== *
 *  THE 2D BATCHER  (HUD, menus, load screen)
 *
 *  See burnout3_render.h for the contract.  The state that matters here is
 *  small -- a texture and a blend preset -- because the HUD pass establishes
 *  everything else once (depth test off, cull off, alpha test off, blend
 *  equation ADD, colour mask 0x010101, both matrices identity) and only three
 *  helpers ever move it.  So a flush is needed exactly when the texture or
 *  the blend changes, or when the caller is about to touch raw GL itself.
 * ====================================================================== */
#define B3R2D_STRIDE 9

/* One corner of the primitive under construction: its position and the
 * colour/uv the caller had current AT THAT CORNER, which is what makes a
 * gradient quad come out right -- the colour moves between glVertex calls. */
typedef struct { float x, y, z, u, v, c[4]; } B3R2DCorner;

static struct {
    float*   cpu;
    int      cap;             /* vertices the CPU buffer holds  */
    int      n;               /* vertices buffered              */
    unsigned vbo;
    int      vbo_cap;
    B3RState st;              /* the whole tuple a flush applies */
    int      kind;            /* the primitive being assembled  */
    int      prim_n;          /* vertices seen inside it        */
    float    col[4];
    float    uv[2];
    B3R2DCorner corner[4];
    int      batches;
    int      depth;           /* begin/end nesting, as b3r_begin counts it */
    int      raw;             /* geometry only: the caller owns program+state */
    int      col_used;        /* did anything set a vertex colour this batch? */
    unsigned gl_mode;         /* GL_TRIANGLES, or GL_LINES for a line loop   */
    B3R2DCorner loop_first;   /* the vertex a LINE_LOOP closes back onto     */
} g_2d;

static void r2d_emit(const B3R2DCorner* c) {
    if (g_2d.n >= g_2d.cap) {
        int cap = g_2d.cap ? g_2d.cap * 2 : 8192;
        float* p = (float*)realloc(g_2d.cpu,
                                   (size_t)cap * B3R2D_STRIDE * sizeof(float));
        if (!p) return;
        g_2d.cpu = p;
        g_2d.cap = cap;
    }
    float* o = g_2d.cpu + (size_t)g_2d.n * B3R2D_STRIDE;
    o[0] = c->x; o[1] = c->y; o[2] = c->z;
    o[3] = c->u; o[4] = c->v;
    o[5] = c->c[0]; o[6] = c->c[1]; o[7] = c->c[2]; o[8] = c->c[3];
    g_2d.n++;
}

void b3r2d_flush(void) {
    if (g_state != 1 || g_2d.n <= 0) return;
    if (g_2d.vbo_cap < g_2d.n) {
        if (g_2d.vbo) b3r_vbo_free(g_2d.vbo);
        g_2d.vbo_cap = g_2d.n < 8192 ? 8192 : g_2d.n * 2;
        g_2d.vbo = vbo_make(NULL,
                            (long)g_2d.vbo_cap * B3R2D_STRIDE
                            * (long)sizeof(float), GL_DYNAMIC_DRAW);
    }
    if (!g_2d.vbo) { g_2d.n = 0; return; }
    p_glBindBuffer(GL_ARRAY_BUFFER, g_2d.vbo);
    /* ORPHAN FIRST -- see b3r_vbo_update3().  On paper this is the sharper of
     * the two cases: b3r2d_flush() runs 7-8 times in one frame, and every call
     * after the first overwrites offset 0 while the previous flush's
     * glDrawArrays is still sourcing it.
     *
     * IT IS OFF BY DEFAULT, AND WHY IS A FINDING, NOT A PREFERENCE.  Orphaning
     * here MOVES PIXELS: measured on the desktop build, offscreen, US_C3_V1
     * frame 240 with the sim pinned, B3_VBO_ORPHAN=2 against 0 differs on
     * 5 px of 307 200 (0.0016%), max delta 3/255, all inside one 30x6 box.
     * Each configuration is bit-identical to itself across runs, so it is not
     * jitter; orphaning bit 0 alone (the shine buffer) is bit-identical to the
     * baseline, so it is this site specifically.
     *
     * Orphaning cannot change what a correct draw reads: the same bytes reach
     * the same offsets before the same glDrawArrays.  What it DOES change is
     * what lies beyond g_2d.n -- stale previous-batch vertices without it,
     * undefined storage with it.  So a difference here says something in this
     * path is sensitive to vertices past its own count, and THAT is a defect
     * worth finding on its own terms rather than papering over with a buffer
     * hint.  Until it is found, the shipped default is the behaviour the
     * pinned frame was pinned against.
     *
     * B3_VBO_ORPHAN=3 turns it on for anybody measuring it. */
    if (b3r_orphan_on(B3R_ORPHAN_2D))
        p_glBufferData(GL_ARRAY_BUFFER,
                       (long)g_2d.vbo_cap * B3R2D_STRIDE * (long)sizeof(float),
                       NULL, GL_DYNAMIC_DRAW);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0,
                      (long)g_2d.n * B3R2D_STRIDE * (long)sizeof(float),
                      g_2d.cpu);
    g_c.vbo_a = g_c.vbo_b = (unsigned)-1;
    /* THE COLOUR ARRAY IS CONDITIONAL, and that is load-bearing for the
     * geometry-only callers.  An ENABLED colour array overrides the current
     * glColor -- so a postfx quad that never sets a per-vertex colour, and
     * whose whole tint is one glColor4f the caller set (the sky gain, the
     * cloud combiner constant C0, the blur tap weight), must be drawn with
     * the array OFF or it comes out white. */
    {   B3RVtxFmt f;
        f.vbo = g_2d.vbo; f.stride = B3R2D_STRIDE;
        f.off_uv = 3; f.off_nrm = -1;
        f.off_col = g_2d.col_used ? 5 : -1;
        f.n_col = 4;
        b3r_arrays_fmt(&f);
    }
    if (!g_2d.raw) b3r_state(&g_2d.st);
    if (!g_2d.raw) b3r_sync_matrices();
    glDrawArrays(g_2d.gl_mode ? g_2d.gl_mode : GL_TRIANGLES, 0, g_2d.n);
    g_2d.batches++;
    g_2d.n = 0;
    g_2d.col_used = 0;
}

/* GEOMETRY ONLY.  The postfx passes bind their OWN recovered programs (the
 * gamma composite, the present blend) and run a recovered sequence of raw
 * blend / depth / texture-enable calls between draws.  All they want from the
 * batcher is somewhere to put vertices that is not glBegin -- so in raw mode
 * it touches neither the program nor any GL state, and only uploads, points
 * the client arrays and draws. */
void b3r2d_begin_raw(void) {
    if (g_state != 1 && !b3r_init()) return;
    if (g_2d.depth++ > 0) return;
    g_2d.raw = 1;
    g_2d.col_used = 0;
    g_2d.gl_mode = GL_TRIANGLES;
    g_2d.n = 0;
    g_2d.kind = -1;
    g_2d.prim_n = 0;
    g_2d.col[0] = g_2d.col[1] = g_2d.col[2] = g_2d.col[3] = 1.0f;
    g_2d.uv[0] = g_2d.uv[1] = 0.0f;
}

void b3r2d_begin(void) {
    if (g_state != 1 && !b3r_init()) return;
    /* Counted, so a nested pair cannot end the outer one's batch early and
     * drop whatever it had buffered.  No shipped caller nests today; this is
     * what stops the next one from being a silent missing element. */
    if (g_2d.depth++ > 0) return;
    g_2d.raw = 0;
    g_2d.col_used = 1;   /* the shader path always wants the vertex colour */
    g_2d.gl_mode = GL_TRIANGLES;
    b3r_begin();
    /* The HUD pass owns the depth / blend-equation / colour-mask state; b3r
     * only has to stop asserting the world's fog and hand the shader an
     * untextured, untested default. */
    b3r_fog(NULL, 0, 0);
    b3r_model(NULL);
    b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
    b3r_uv_offset(0.0f, 0.0f);
    g_2d.n = 0;
    /* the ambient 2D tuple: no depth, no cull, no alpha test, plain alpha
     * blending -- exactly what state_begin() establishes around it */
    g_2d.st.tex = 0;
    g_2d.st.mode = B3R_TEX_NONE;
    g_2d.st.blend = B3R_BLEND_ALPHA;
    g_2d.st.alpha_ref = -1.0f;
    g_2d.st.depth_mask = 0;
    g_2d.st.depth_test = 0;
    g_2d.st.depth_func = 0;
    g_2d.st.cull = 0;
    g_2d.col[0] = g_2d.col[1] = g_2d.col[2] = g_2d.col[3] = 1.0f;
    g_2d.uv[0] = g_2d.uv[1] = 0.0f;
    g_2d.kind = -1;
    g_2d.prim_n = 0;
}

void b3r2d_end(void) {
    if (g_state != 1 || g_2d.depth <= 0) return;
    if (--g_2d.depth > 0) return;
    b3r2d_flush();
    if (g_2d.raw) { b3r_arrays_none(); g_2d.raw = 0; }
    else          b3r_end();
}

/* Switch the batcher between "the retained program draws this" and "the
 * caller's own program draws this", mid-pass.  postfx needs it: within one 2D
 * pass it draws some quads under its recovered blur/gamma programs and others
 * under no program at all, and the two cannot share a batch. */
void b3r2d_set_raw(int on) {
    if (g_state != 1) return;
    on = on ? 1 : 0;
    if (g_2d.raw == on) return;
    b3r2d_flush();
    g_2d.raw = on;
    if (!on) {
        b3r_use_program_via(p_glUseProgram, g_prog);
        /* the program's uniform state is not what the cache remembers */
        g_c.mode = -1; g_c.fogon = -1; g_c.aref = -2.0f;
        g_c.fogfar = -1.0f;
        memset(g_c.mvp, 0, sizeof g_c.mvp);
        memset(g_c.mv, 0, sizeof g_c.mv);
        memset(g_c.color, 0, sizeof g_c.color);
        memset(g_c.uvoff, 0, sizeof g_c.uvoff);
        g_ms_dirty = 1;
        b3r_color(1.0f, 1.0f, 1.0f, 1.0f);
        b3r_uv_offset(0.0f, 0.0f);
        b3r_fog(NULL, 0, 0);
    }
}

/* Bind the retained program for a plain textured draw that is NOT going
 * through the batcher -- the car wheels, the traffic bodies, anything whose
 * geometry comes straight out of a VBO under its own matrix.
 *
 * Under fixed function these needed no program at all, which is exactly why
 * they broke silently when the compatibility profile went: with no program
 * bound the draw falls through to the fixed-function stage, where generic
 * attribute 0 aliases gl_Vertex and everything else is ignored -- untextured,
 * unlit geometry, no error, no clue. */
void b3r_use_flat(unsigned tex, float r, float g, float b, float a) {
    B3RState st;
    if (g_state != 1 && !b3r_init()) return;
    b3r_use_program_via(p_glUseProgram, g_prog);
    g_c.mode = -1; g_c.fogon = -1; g_c.aref = -2.0f; g_c.fogfar = -1.0f;
    g_c.tex = (unsigned)-1;
    memset(g_c.mvp, 0, sizeof g_c.mvp);
    memset(g_c.mv, 0, sizeof g_c.mv);
    memset(g_c.color, 0, sizeof g_c.color);
    memset(g_c.uvoff, 0, sizeof g_c.uvoff);
    g_ms_dirty = 1;
    st.tex        = tex;
    st.mode       = tex ? B3R_TEX_MODULATE : B3R_TEX_NONE;
    st.blend      = -1;
    st.alpha_ref  = -1.0f;
    st.depth_mask = -1;
    st.depth_test = -1;
    st.depth_func = 0;
    st.cull       = -1;
    b3r_state(&st);
    b3r_color(r, g, b, a);
    b3r_uv_offset(0.0f, 0.0f);
    b3r_fog(NULL, 0, 0);
}

void b3r_unuse(void) {
    if (g_state != 1) return;
    b3r_use_program_via(p_glUseProgram, 0);
}

/* The matrices, for a caller that draws with glDrawArrays directly rather
 * than through b3r_draw(). */
void b3r_sync(void) { b3r_sync_matrices(); }

void b3r2d_texture(unsigned tex) {
    if (g_state != 1 || g_2d.st.tex == tex) return;
    b3r2d_flush();
    g_2d.st.tex  = tex;
    g_2d.st.mode = tex ? B3R_TEX_MODULATE : B3R_TEX_NONE;
}

void b3r2d_blend(int preset) {
    if (g_state != 1 || g_2d.st.blend == preset) return;
    b3r2d_flush();
    g_2d.st.blend = preset;
}

void b3r_batch_state(const B3RState* st) {
    if (g_state != 1 || !st) return;
    if (memcmp(&g_2d.st, st, sizeof *st) == 0) return;
    b3r2d_flush();
    g_2d.st = *st;
}

void b3r2d_prim(int kind) {
    /* One batch is one GL primitive mode, so a change of mode has to draw what
     * is already buffered before it can be applied. */
    unsigned want = (kind == B3R2D_LINE_LOOP) ? GL_LINES : GL_TRIANGLES;
    if (g_2d.gl_mode && g_2d.gl_mode != want) b3r2d_flush();
    g_2d.gl_mode = want;
    g_2d.kind = kind;
    g_2d.prim_n = 0;
}

void b3r2d_prim_end(void) {
    if (g_2d.kind == B3R2D_LINE_LOOP && g_2d.prim_n >= 2) {
        r2d_emit(&g_2d.corner[0]);      /* the last vertex ... */
        r2d_emit(&g_2d.loop_first);     /* ... back to the first */
    }
    g_2d.kind = -1;
    g_2d.prim_n = 0;
}

void b3r2d_color(float r, float g, float b, float a) {
    g_2d.col[0] = r; g_2d.col[1] = g; g_2d.col[2] = b; g_2d.col[3] = a;
    g_2d.col_used = 1;
}

void b3r2d_uv(float u, float v) {
    g_2d.uv[0] = u; g_2d.uv[1] = v;
}

void b3r2d_vertex(float x, float y) { b3r2d_vertex3(x, y, 0.0f); }

void b3r2d_vertex3(float x, float y, float z) {
    if (g_state != 1 || g_2d.kind < 0) return;
    B3R2DCorner c;
    c.x = x; c.y = y; c.z = z; c.u = g_2d.uv[0]; c.v = g_2d.uv[1];
    c.c[0] = g_2d.col[0]; c.c[1] = g_2d.col[1];
    c.c[2] = g_2d.col[2]; c.c[3] = g_2d.col[3];

    switch (g_2d.kind) {
    case B3R2D_TRIANGLES:
        g_2d.corner[g_2d.prim_n % 3] = c;
        if (++g_2d.prim_n % 3 == 0) {
            r2d_emit(&g_2d.corner[0]);
            r2d_emit(&g_2d.corner[1]);
            r2d_emit(&g_2d.corner[2]);
        }
        break;
    case B3R2D_QUADS:
        g_2d.corner[g_2d.prim_n % 4] = c;
        if (++g_2d.prim_n % 4 == 0) {
            /* the quad rule: (v0,v1,v2) then (v0,v2,v3) */
            r2d_emit(&g_2d.corner[0]);
            r2d_emit(&g_2d.corner[1]);
            r2d_emit(&g_2d.corner[2]);
            r2d_emit(&g_2d.corner[0]);
            r2d_emit(&g_2d.corner[2]);
            r2d_emit(&g_2d.corner[3]);
        }
        break;
    case B3R2D_QUAD_STRIP:
        /* corner[] is a 4-deep shift register of the last four vertices:
         * [0]=v(n), [1]=v(n-1), [2]=v(n-2), [3]=v(n-3).  A quad closes on
         * every EVEN arrival past the first pair, and GL_QUAD_STRIP's
         * winding for it is (n-3, n-2, n, n-1). */
        g_2d.corner[3] = g_2d.corner[2];
        g_2d.corner[2] = g_2d.corner[1];
        g_2d.corner[1] = g_2d.corner[0];
        g_2d.corner[0] = c;
        g_2d.prim_n++;
        if (g_2d.prim_n >= 4 && (g_2d.prim_n & 1) == 0) {
            r2d_emit(&g_2d.corner[3]);   /* v(n-3) */
            r2d_emit(&g_2d.corner[2]);   /* v(n-2) */
            r2d_emit(&g_2d.corner[0]);   /* v(n)   */
            r2d_emit(&g_2d.corner[3]);
            r2d_emit(&g_2d.corner[0]);
            r2d_emit(&g_2d.corner[1]);   /* v(n-1) */
        }
        break;
    case B3R2D_LINE_LOOP:
        /* v0 is kept so the loop can close; every vertex after it emits the
         * segment from the previous one, and prim_end emits the last. */
        if (g_2d.prim_n == 0) {
            g_2d.loop_first = c;
        } else {
            r2d_emit(&g_2d.corner[0]);
            r2d_emit(&c);
        }
        g_2d.corner[0] = c;
        g_2d.prim_n++;
        break;
    case B3R2D_TRI_STRIP:
        g_2d.corner[2] = g_2d.corner[1];
        g_2d.corner[1] = g_2d.corner[0];
        g_2d.corner[0] = c;
        g_2d.prim_n++;
        if (g_2d.prim_n >= 3) {
            /* GL_TRIANGLE_STRIP alternates winding; reproduce it exactly, or
             * a back-face cull that the caller happens to leave on would eat
             * every other triangle. */
            if ((g_2d.prim_n & 1) == 1) {
                r2d_emit(&g_2d.corner[2]);
                r2d_emit(&g_2d.corner[1]);
                r2d_emit(&g_2d.corner[0]);
            } else {
                r2d_emit(&g_2d.corner[1]);
                r2d_emit(&g_2d.corner[2]);
                r2d_emit(&g_2d.corner[0]);
            }
        }
        break;
    default:
        break;
    }
}

int  b3r2d_batches(void)       { return g_2d.batches; }
void b3r2d_reset_batches(void) { g_2d.batches = 0; }
