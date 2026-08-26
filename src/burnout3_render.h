/* burnout3_render.h -- THE RETAINED, SHADER-BASED DRAW PATH.
 *
 * WHAT THIS IS, AND WHY IT EXISTS
 * ==============================================================
 * Every world pass in this harness used to emit its geometry per frame through
 * the GL 1.x fixed-function surface: one baked display list for the track, one
 * `glCallList` per scenery/prop instance, `glBegin`/`glEnd` per HUD element.
 * On the desktop that is merely wasteful.  In a browser it is the ceiling:
 * gl4es keeps ONE SCRATCH VBO PER ATTRIBUTE and re-uploads and re-points all
 * three every batch, so the measured cost was 12.2 real WebGL calls per draw
 * and ~26 500 calls a frame (docs/web/webprof_sweep.md section 6).  At
 * Chromium's ~0.2-0.5 us per WebGL call that is 5-13 ms of a 16.7 ms budget
 * before the GPU does anything.
 *
 * The fix is not a driver knob -- `LIBGL_BATCH` was measured and dropped,
 * because gl4es can only merge draws whose state is already identical AND
 * consecutive, and every one of the track's 990 material groups rebinds a
 * texture.  The fix is to stop re-describing static geometry every frame:
 *
 *   * ONE static interleaved VBO per body of geometry, uploaded once at load;
 *   * ONE program, whose vertex format never changes, so gl4es' own
 *     vertex-attrib cache (`realize_glenv`, src/gl/fpe.c:1435-1510) re-issues
 *     glVertexAttribPointer ZERO times between draws;
 *   * draws merged by texture, so the track's 934 opaque groups become one
 *     draw per texture and its 972 scenery instances become one draw per run;
 *   * the fixed-function state the passes used (texture modulate, the world's
 *     GREATER 64/255 alpha test, the linear fog with its recovered
 *     `min(|z|, fog_far)` coordinate clamp) reproduced IN THE SHADER, so
 *     GL_TEXTURE_2D / GL_ALPHA_TEST / GL_FOG stay OFF -- which also keeps
 *     gl4es out of its shader-customisation path (`fpe_ReleventState` +
 *     `fpe_CustomShader`, src/gl/fpe.c:1090) and leaves exactly one compiled
 *     program alive for the whole frame.
 *
 * ONE CODE PATH, TWO TARGETS.  Everything here is plain GL 2.0: VBOs, generic
 * vertex attributes and GLSL 1.10.  That is what a desktop compatibility
 * context offers natively and what gl4es re-implements on GLES2, so the
 * desktop binary and the web build run the SAME source with no #ifdef.
 *
 * ONE ROUTE.  There is no fixed-function fall-back to switch to: the passes
 * migrated here had their glBegin/glEnd, display-list and glTexEnv code
 * DELETED as each one's pinned-frame gate went green, so the branch's history
 * is the reference and the shipped tree has a single renderer.  b3r_init()
 * failing therefore means the context cannot run this program at all, which
 * on any target this harness supports (a desktop 2.1 compatibility context,
 * or gl4es on GLES2) does not happen -- carfx and postfx already required the
 * same GL 2.0 surface before this module existed.
 */
#ifndef BURNOUT3_RENDER_H
#define BURNOUT3_RENDER_H

#include "burnout3_trackmesh.h"

/* ---- lifecycle -------------------------------------------------------- */

/* 1 once the program is compiled and usable.  Safe to call before
 * b3r_init() (returns 0). */
int  b3r_active(void);

/* Compile the program and load the GL 2.0 entry points.  Needs a current GL
 * context.  Returns 1 on success.  Idempotent. */
int  b3r_init(void);

/* Release every VBO and the program.  Safe with no context. */
void b3r_shutdown(void);

/* ---- the frame -------------------------------------------------------- */

/* The camera, ONCE a frame, as two column-major 4x4s in the exact layout
 * glLoadMatrixf consumes.  `proj` must already carry the harness' display
 * mirror (the legacy path's glScalef(-1,1,1) on the projection).  Every MVP
 * this module uploads is computed on the CPU from these, which is what kills
 * the per-object glGetFloatv(GL_MODELVIEW_MATRIX) readbacks. */
void b3r_set_camera(const float proj[16], const float view[16]);
const float* b3r_view(void);
const float* b3r_proj(void);

/* trackmesh_gl_single_sided()'s answer -- 1 unless B3_TRACK_NOCULL.  The
 * world passes cull with it and b3r_end() restores it, because the legacy
 * scenery/props passes put it back from a glIsEnabled snapshot and the car
 * pass that follows inherits it. */
int  b3r_world_cull(void);

/* ---- the shared vertex attribute layout -------------------------------- *
 * Every program that draws this harness' vertex buffers binds these four
 * names to these four locations, so one buffer pointed once feeds any of
 * them -- the retained world/HUD program and the recovered carfx pair alike.
 * Call b3r_attr_bind() BEFORE glLinkProgram. */
#define B3R_ATTR_POS 0
#define B3R_ATTR_UV  1
#define B3R_ATTR_COL 2
#define B3R_ATTR_NRM 3
void b3r_attr_bind(unsigned prog);

/* projection*modelview and modelview, for a caller with its own program. */
const float* b3r_mvp(void);
const float* b3r_mv(void);

/* ---- the matrix stack -------------------------------------------------- *
 * A CPU mirror of the fixed-function one, with the SAME semantics -- GL's own
 * column-major layout, GL's own glRotatef definition, GL's own
 * "multiply applies before what is already there" association.
 *
 * The retained path takes its transform from HERE, not from ftransform().
 * That is the change that lets the compatibility profile go: 150 call sites
 * across this harness push, rotate and ortho their way through a frame, and
 * WebGL has none of it.  Every operation below still issues the
 * fixed-function call as well, because the passes that have not been converted
 * yet read it -- when the last of them is, that half is deleted in ONE place
 * rather than at 150 call sites.  See the block comment in the .c. */
#define B3R_MAT_MODELVIEW  0
#define B3R_MAT_PROJECTION 1
void b3r_matrix_mode(int which);
void b3r_push(void);
void b3r_pop(void);
void b3r_identity(void);
void b3r_load(const float m[16]);
void b3r_mult(const float m[16]);
void b3r_translate(float x, float y, float z);
void b3r_rotate(float deg, float x, float y, float z);
void b3r_scale(float x, float y, float z);
void b3r_ortho(float l, float r, float b, float t, float n, float f);
void b3r_frustum(float l, float r, float b, float t, float n, float f);
/* The current matrix of either stack, column-major, as glLoadMatrixf takes. */
const float* b3r_mat(int which);

/* Bind the program and the attribute arrays.  Between begin and end, the
 * fixed-function texture / alpha-test / fog / lighting enables are OFF and
 * must stay off; the shader does that work.  Nested calls are counted. */
void b3r_begin(void);
void b3r_end(void);

/* ---- state ------------------------------------------------------------ */

/* Fragment source.  MODULATE is the world's `tex * primary`, the GL spelling
 * of every world pixel shader's stage 0 (the x2 is already baked into the
 * stored vertex colour by trackmesh_load).  SHINE is the class-1/7/10
 * additive term's `tex.a * primary.rgb`, which the legacy path expressed as
 * GL_COMBINE(MODULATE, TEXTURE.alpha, PRIMARY.rgb). */
#define B3R_TEX_NONE     0
#define B3R_TEX_MODULATE 1
#define B3R_TEX_SHINE    2

/* Blend presets.  ALPHA is the ambient world/HUD preset (SRC_ALPHA,
 * ONE_MINUS_SRC_ALPHA, FUNC_ADD); ADD is the shine pass' (ONE, ONE). */
#define B3R_BLEND_NONE  0
#define B3R_BLEND_ALPHA 1
#define B3R_BLEND_ADD   2
/* SRC_ALPHA / ONE -- the HUD's additive preset, used by the boost bar's fire
 * layers and the aftertouch arrow's gloss pass (burnout3_hud.c state_fire). */
#define B3R_BLEND_SA_ONE 3
/* DST_COLOR / ONE -- the DOUBLING blend.  dst = dst*src + dst = 2*dst for a
 * white source, which is retail's step 3 present composite: the car-select
 * screen authors its 3D block at half range (B3_MENU_RT) and this is what
 * brings it back, exactly as the race frame's present does. */
#define B3R_BLEND_DST_ONE 4

/* A NEGATIVE blend / depth_mask / depth_test / cull means LEAVE THAT GL STATE
 * ALONE.  The postfx passes are a recovered sequence of raw state calls
 * interleaved with draws, and there the batcher only supplies the shader's own
 * knobs: the texture, the sampling mode and the alpha reference. */
typedef struct {
    unsigned tex;        /* GL texture name; 0 with mode NONE                */
    int      mode;       /* B3R_TEX_*                                        */
    int      blend;      /* B3R_BLEND_*, or < 0 to leave it alone            */
    float    alpha_ref;  /* GREATER test reference; < 0 disables the test    */
    int      depth_mask; /* 1 write, 0 not, < 0 leave alone                  */
    int      depth_test; /* 0/1, < 0 leave alone                             */
    int      depth_func; /* GL enum, 0 = leave alone                         */
    int      cull;       /* 0 off, 1 on, < 0 leave alone                     */
} B3RState;

/* Apply a state, diffing against what is already set: a redundant field costs
 * nothing.  Callers build a B3RState per batch and hand it over. */
void b3r_state(const B3RState* st);

/* The constant colour every vertex colour is multiplied by (the per-instance
 * scenery/prop tint, or white).  Cached. */
void b3r_color(float r, float g, float b, float a);

/* The UV offset added to every texcoord -- the animated-material scroll's
 * vertex-shader constant 0x63.  Cached. */
void b3r_uv_offset(float u, float v);

/* The model transform.  NULL means identity (world-space geometry).  The MVP
 * and the fog's eye-Z row are computed here on the CPU. */
void b3r_model(const float m[16]);

/* The world fog, as the world setup FUN_00038D10 programs D3DRS_FOG*.  `sc` NULL or disabled turns it off.  `black` runs the same ramp
 * against a BLACK fog colour, which is what an additive pass needs so it
 * contributes f*spec and adds no fog colour of its own.  `clamp` applies the
 * recovered `min(|z_eye|, fog_far)` coordinate clamp -- on for the world pass
 * (the recovered `min(|z_eye|, fog_far)` from the world vertex programs), off
 * for the shine pass, which the fixed-function path drew with an UNCLAMPED
 * ramp because its fog vertex program had already been unbound -- and matching
 * that exactly is what keeps the pinned frame identical. */
void b3r_fog(const TrackScene* sc, int black, int clamp);

/* ---- buffers ---------------------------------------------------------- */

/* An interleaved static buffer, 9 floats a vertex: x y z  u v  r g b a. */
unsigned b3r_vbo_static(const float* data, int nverts);
/* A 5-float (x y z u v) static buffer and its 3-float (r g b) dynamic mate,
 * for the passes whose colour is recomputed per frame against the camera. */
unsigned b3r_vbo_static5(const float* data, int nverts);
unsigned b3r_vbo_dynamic3(int nverts);
void     b3r_vbo_update3(unsigned vbo, const float* data, int nverts);
void     b3r_vbo_free(unsigned vbo);

/* Point the three attributes at one interleaved 9-float buffer, or at the
 * 5+3 split pair.  Cached: re-binding the same layout costs nothing. */
void b3r_arrays(unsigned vbo9);
void b3r_arrays_split(unsigned vbo5, unsigned vbo3);

/* ---- arbitrary interleaved layouts ------------------------------------- *
 * The world buffers are all (pos, uv, colour) and b3r_arrays() covers them.
 * The CAR meshes are not: the recovered carfx program reads gl_Normal, and
 * the traffic bodies carry no colour at all.  A layout descriptor keeps one
 * buffer type for every one of them instead of a helper per shape.
 *
 * Offsets are in FLOATS from the start of the vertex; < 0 means the channel
 * is absent, and then its client array is DISABLED.  For colour that is not a
 * detail: with no array, gl_Color is the CURRENT colour, which is exactly what
 * the car glass pass and every postfx quad rely on -- they set one glColor4f
 * and draw a whole mesh with it.  The caller owns it; b3r_begin() leaves it
 * white for everyone who does not. */
typedef struct {
    unsigned vbo;
    int      stride;     /* floats per vertex          */
    int      off_uv;
    int      off_col;    /* 3- or 4-component, see n_col */
    int      n_col;      /* 3 or 4; the component count is part of the format */
    int      off_nrm;
} B3RVtxFmt;

void     b3r_arrays_fmt(const B3RVtxFmt* f);
/* Hand the client arrays back to a fixed-function caller: nothing enabled and
 * NO buffer bound.  A pass that still draws from CLIENT MEMORY must see this
 * first, or its glVertexPointer is read as an offset into whatever VBO was
 * left bound. */
void     b3r_arrays_none(void);
/* A static buffer of `nfloats` floats.  `dynamic` picks GL_DYNAMIC_DRAW. */
unsigned b3r_vbo_upload(const float* data, long nfloats, int dynamic);

/* glDrawArrays(GL_TRIANGLES, first, count) through the cached state. */
void b3r_draw(int first, int count);

/* ---- the track -------------------------------------------------------- */

/* Build the retained track: one static VBO for the baked world, one for the
 * animated (scroll / frame-cycling) groups, and the pos/uv + colour pair the
 * shine pass draws from.  `tex` and `cutout` are the renderer's per-group
 * texture names and its texel cut-out fallback, exactly as the legacy
 * display-list bake consumed them.  Returns 1 on success. */
int  b3r_track_build(TrackMesh* m, const unsigned* tex,
                     const unsigned char* cutout);
void b3r_track_free(void);

/* The three track passes: the baked world, the animated (scroll and
 * frame-cycling) groups, and the class-1/7/10 additive specular. */
void b3r_track_draw(const TrackMesh* m);
int  b3r_track_draw_scroll(const TrackMesh* m);
int  b3r_track_draw_shine(TrackMesh* m, const float eye[3],
                          const float light_dir[3]);

/* ---- instanced static models (scenery, props) -------------------------- */
/*
 * Both of those passes are the same shape: a small table of models, a large
 * table of 4x4 instance transforms with a baked per-instance tint, and one
 * `glCallList` per instance under a glPushMatrix/glMultMatrixf pair.  523
 * scenery draws a frame is 22% of the port's whole WebGL traffic.
 *
 * The retained answer bakes every instance into WORLD SPACE once, so the
 * transform is not a per-draw uniform at all, and orders the baked geometry
 * model-major (one texture and one material state per model) with a Z-order
 * curve inside each model, so that the per-instance distance cull keeps
 * long contiguous runs and the surviving instances collapse into a handful
 * of glDrawArrays calls.
 */
typedef struct B3RInstSet B3RInstSet;

typedef struct {
    const float*          vtx;      /* nvtx * stride floats                 */
    int                   stride;   /* floats per vertex                    */
    int                   uv_off;   /* float index of the uv pair           */
    unsigned              nvtx;
    const unsigned short* idx;      /* MODEL-LOCAL index values             */
    unsigned              nidx;
} B3RMeshSrc;

typedef struct {
    unsigned first_vertex, n_vertex, first_index, n_index;
    unsigned tex;
    unsigned mat_flags;             /* bit 0x001 blend, 0x010 alpha test    */
} B3RModelSrc;

typedef struct {
    const float* m;                 /* 16 floats, column-major, GL-ready    */
    const float* tint;              /* 3 floats                             */
    unsigned     model;
    float        cull_far;          /* <= 0 = never culled                  */
} B3RInstSrc;

/* Bake.  Returns NULL if the retained path is off or the data is unusable. */
B3RInstSet* b3r_inst_build(const B3RMeshSrc* mesh,
                           const B3RModelSrc* models, int nmodels,
                           const B3RInstSrc* inst, int ninst);
void b3r_inst_free(B3RInstSet* s);

/* Draw every instance the eye can see.  `skip` (may be NULL) is one byte per
 * ORIGINAL instance index: non-zero leaves that instance out, so a caller
 * whose instance has moved off its baked transform can draw it itself.
 * Returns the number of glDrawArrays calls issued. */
int  b3r_inst_draw(B3RInstSet* s, const float eye[3],
                   const unsigned char* skip);

/* One model, in MODEL space, under an explicit transform -- for the
 * instances `skip` left out. */
void b3r_inst_draw_model(B3RInstSet* s, int model, const float m[16],
                         const float tint[3]);

/* ---- the 2D batcher (HUD, menus, load screen) -------------------------- *
 *
 * The HUD's problem is not vertices, it is BATCHES.  draw_text() draws every
 * string NINE times -- eight black shadow offsets plus the fill
 * (burnout3_hud.c:455) -- each as its own glBegin/glEnd, and gl4es turns each
 * one into a fresh draw with a fresh set of scratch-VBO uploads.  ~95 draws
 * and ~1250 WebGL calls a frame for a few hundred quads.
 *
 * This is a drop-in for the glBegin/glColor/glTexCoord/glVertex quartet those
 * sites use.  It accumulates into one dynamic VBO and flushes only when the
 * STATE changes -- so the nine passes of a string, drawn with one texture and
 * one blend mode, become one glDrawArrays.
 *
 * GL_QUADS becomes two triangles in the (v0,v1,v2) + (v0,v2,v3) order, which
 * is the decomposition the spec's quad rule and every desktop driver use, so
 * a gradient quad interpolates identically.  GL_QUAD_STRIP and
 * GL_TRIANGLE_STRIP unroll the same way.
 *
 * The caller keeps owning the pass-level state it already set (the blend
 * equation, the colour mask, the identity matrices) -- b3r2d only needs to be
 * told to flush before any of it moves. */
void b3r2d_begin(void);          /* bind the program; matrices are the caller's */
/* GEOMETRY ONLY: no program bind, no GL state, no b3r uniforms.  For a pass
 * that has its own recovered program and its own raw state sequence and just
 * needs somewhere to put vertices that is not glBegin. */
/* Bind the retained program for a draw that bypasses the batcher (the car
 * wheels, the traffic bodies).  With NO program bound such a draw falls
 * through to fixed function, where generic attribute 0 aliases gl_Vertex --
 * untextured geometry and no error. */
void b3r_use_flat(unsigned tex, float r, float g, float b, float a);
void b3r_unuse(void);
/* Push uMVP / uMV if the stack moved; for a caller that issues its own
 * glDrawArrays rather than going through b3r_draw(). */
void b3r_sync(void);

void b3r2d_begin_raw(void);
/* Flip that mode mid-pass; flushes what is buffered first. */
void b3r2d_set_raw(int on);
void b3r2d_end(void);            /* flush and unbind                            */
void b3r2d_flush(void);          /* emit what is buffered; call before raw GL    */

/* State.  Each of these flushes if it actually changes something. */
void b3r2d_texture(unsigned tex);            /* 0 = untextured               */
void b3r2d_blend(int preset);                /* B3R_BLEND_*                  */

/* The immediate-mode shape, so a call site changes by name and nothing else. */
#define B3R2D_QUADS      0
#define B3R2D_QUAD_STRIP 1
#define B3R2D_TRIANGLES  2
#define B3R2D_TRI_STRIP  3
/* GL_LINE_LOOP, unrolled into segments.  Only the B3_DEBUGWALLS collision
 * overlay uses it, but leaving one glBegin in the tree for a diagnostic is
 * exactly how a "there is one renderer" claim stops being true. */
#define B3R2D_LINE_LOOP  4
void b3r2d_prim(int kind);                   /* was glBegin                  */
void b3r2d_prim_end(void);                   /* was glEnd                    */
void b3r2d_color(float r, float g, float b, float a);
void b3r2d_uv(float u, float v);
void b3r2d_vertex(float x, float y);         /* clip space, as glVertex2f    */

/* THE SAME BATCHER IN 3D.  The particle, boost-flame, corona and car-shadow
 * passes build their quads in WORLD space on the CPU (they already have the
 * camera basis), so they are ordinary triangles that happen to be assembled
 * four corners at a time -- the only thing they need beyond the 2D case is a
 * z and the world's depth state.  b3r_batch_state() carries the full tuple;
 * it flushes when any of it changes. */
void b3r2d_vertex3(float x, float y, float z);
void b3r_batch_state(const B3RState* st);

/* How many glDrawArrays the 2D batcher issued since the last reset, for the
 * call-count ledger. */
int  b3r2d_batches(void);
void b3r2d_reset_batches(void);

/* ---- the ledger ------------------------------------------------------- */
/* How many merged batches each retained pass issued last frame.  This is the
 * number the call-count table is built from; B3_RENDER_STATS=1 prints it. */
void b3r_stat_set(int slot, int batches);
/* B3_RENDER_STATS=<n> prints the per-pass draw counts every n frames.  Call
 * once per rendered frame. */
void b3r_stats_frame(void);
#define B3R_STAT_TRACK   0
#define B3R_STAT_SCROLL  1
#define B3R_STAT_SHINE   2
#define B3R_STAT_SCENERY 3
#define B3R_STAT_PROPS   4
#define B3R_STAT_COUNT   5

/* ====================================================================== *
 *  THE STATE SHADOW  --  what replaced glPushAttrib / glPopAttrib
 *
 *  Eight passes bracketed themselves with
 *      glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT
 *                   | GL_DEPTH_BUFFER_BIT | GL_CURRENT_BIT)
 *  ... glPopAttrib(), to hand the frame back the blend / depth / cull state
 *  they moved.  That pair is real work, not dead compatibility scaffolding --
 *  but it is also GL 1.1 only, and the whole point of this wave is that the
 *  web link stops going through a compatibility layer.  GLES2 has no
 *  attribute stack.
 *
 *  Rebuilding it out of glIsEnabled / glGetIntegerv would be correct and
 *  ruinous: on WebGL every one of those is a SYNCHRONOUS round trip to the GL
 *  thread (the sibling wave measured 28-30 getParameter calls a frame as a
 *  material cost), and eight pushes a frame of eight queries each would add
 *  ~64 more.
 *
 *  So the stack is kept on the CPU instead.  b3r already shadowed the fields
 *  IT set, in order to skip redundant calls; the shims below extend that
 *  shadow over the raw calls the passes make for themselves, so the shadow is
 *  authoritative and a push/pop costs no readback at all.  The macros are how
 *  110 existing call sites join in without being rewritten -- every renderer
 *  translation unit already includes this header.  burnout3_render.c defines
 *  B3R_NO_GL_SHADOW and calls the shims by name.
 * ====================================================================== */
void b3r_gl_enable(unsigned cap);
void b3r_gl_disable(unsigned cap);
void b3r_gl_blend_func(unsigned src, unsigned dst);
void b3r_gl_blend_equation(unsigned mode);
void b3r_gl_depth_mask(unsigned char on);
void b3r_gl_depth_func(unsigned func);
void b3r_gl_front_face(unsigned dir);
void b3r_gl_bind_texture(unsigned target, unsigned tex);
void b3r_gl_gen_textures(int n, unsigned* names);
void b3r_gl_delete_textures(int n, const unsigned* names);
/* Bind a program through the ONE cache, using the caller's own
 * GetProcAddress'd glUseProgram (carfx and postfx each have their own). */
void b3r_use_program_via(void (*use)(unsigned), unsigned prog);
void b3r_gl_active_texture(unsigned unit);
void b3r_gl_active_texture_via(void (*act)(unsigned), unsigned unit);
void b3r_gl_color_mask(unsigned char r, unsigned char g,
                       unsigned char b, unsigned char a);
/* Reads the SHADOW, not the driver -- this is what the three glIsEnabled
 * readbacks in the frame loop became. */
int  b3r_gl_is_enabled(unsigned cap);

/* The attribute stack, exactly as deep as the passes nest it (they do not). */
/* GL 3.0 / GLES2 mipmap generation, resolved through GetProcAddress.
 * Replaces the GL_GENERATE_MIPMAP texture parameter, which GLES2 and
 * therefore WebGL do not have. */
void b3r_tex_mipmap_pre(void);   /* before glTexImage2D */
void b3r_gen_mipmap(void);       /* after  glTexImage2D */

void b3r_state_dump(const char* where);
void b3r_state_push(void);
void b3r_state_pop(void);

#ifndef B3R_NO_GL_SHADOW
/* The GL prototypes must be seen BEFORE the names become macros, or the
 * macros expand inside <GL/gl.h>'s own declarations.  Pulling the header in
 * here rather than leaving it to each includer is what makes the shims
 * order-independent. */
#include <GL/gl.h>

#define glEnable(c)                b3r_gl_enable((unsigned)(c))
#define glDisable(c)               b3r_gl_disable((unsigned)(c))
#define glBlendFunc(s, d)          b3r_gl_blend_func((unsigned)(s), (unsigned)(d))
#define glBlendEquation(m)         b3r_gl_blend_equation((unsigned)(m))
#define glDepthMask(m)             b3r_gl_depth_mask((unsigned char)(m))
#define glDepthFunc(f)             b3r_gl_depth_func((unsigned)(f))
#define glFrontFace(d)             b3r_gl_front_face((unsigned)(d))
#define glColorMask(r, g, b, a)    b3r_gl_color_mask((unsigned char)(r), \
                                       (unsigned char)(g),              \
                                       (unsigned char)(b),              \
                                       (unsigned char)(a))
#define glIsEnabled(c)             b3r_gl_is_enabled((unsigned)(c))
#define glBindTexture(t, x)        b3r_gl_bind_texture((unsigned)(t), (unsigned)(x))
#define glGenTextures(n, p)        b3r_gl_gen_textures((int)(n), (p))
#define glDeleteTextures(n, p)     b3r_gl_delete_textures((int)(n), (p))
#define glActiveTexture(u)         b3r_gl_active_texture((unsigned)(u))
#endif

#endif /* BURNOUT3_RENDER_H */
