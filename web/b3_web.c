/* b3_web.c -- the web port's platform seam.  See b3_web.h.
 *
 * ================================================= WHY gl4es IS NO LONGER HERE
 * It used to be, and the reason it had to be is worth keeping: this harness
 * drew through the GL 2.1 COMPATIBILITY surface -- 42 glBegin/glEnd blocks,
 * GL_QUADS, display lists, the matrix stack, glPushAttrib, glAlphaFunc,
 * glTexEnvi(GL_COMBINE), fog -- and none of that exists in WebGL.  gl4es
 * re-implemented the whole surface on GLES2, the same build the Android port
 * runs, and that is what made a web port possible at all.
 *
 * The retained-renderer wave removed the reason.  There is no glBegin, no
 * display list, no matrix stack, no glTexEnv, no alpha test and no fog state
 * left in this engine: the passes emit static VBOs through generic vertex
 * attributes and do the rest in GLSL, and burnout3_render.h's state shadow
 * owns the handful of enables that remain.  That is a strict subset of GLES
 * 2.0 under the same entry-point names, so Emscripten's WebGL bindings satisfy
 * the link directly -- <GL/gl.h> now resolves to web/GL/gl.h, which is GLES2.
 *
 * Taking the layer out is not just dead weight.  gl4es sat between every draw
 * and WebGL, re-pointing one scratch VBO per attribute per batch and
 * re-checking fixed-function state against the bound program to decide whether
 * to compile a shader variant (fpe_ReleventState / fpe_CustomShader,
 * src/gl/fpe.c:1090).  With no fixed-function state left to check, all of it
 * was overhead on the exact axis this port is bound by -- WebGL calls a frame.
 *
 * The name-collision problem it used to have went with it: gl4es mangled its
 * exports to gl4es_gl* for __EMSCRIPTEN__ (USE_MGL_NAMESPACE) precisely
 * BECAUSE Emscripten's libGL.js already defines glBindTexture and friends.
 * Those definitions are now the ones the engine links against.
 */
#define B3_WEB_IMPL   1      /* keep the SDL_GL_* shims out of this file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/atomic.h>
#include <emscripten/threading.h>   /* emscripten_thread_sleep, the audio pump */
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <SDL2/SDL.h>          /* before b3_web.h: it gates the GL shims on SDL */
#include <GL/gl.h>          /* -> web/GL/gl.h, i.e. GLES2 */

#include "b3_web.h"
/* b3_afx_status() -- the resize-sweep test hook uses it as its start gun; it
 * returns NULL exactly while the effects chain is up. */
#include "burnout3_aftereffects.h"

/* ======================================================= THE GL SHIMS ====
 * See b3_web.h for why SDL cannot make this context.  "#canvas" is the
 * selector Emscripten resolves the VISIBLE canvas element by -- the target of
 * the present, and of the proxied fallback's context -- so the shell's canvas
 * element must carry id="canvas".  web/pre.js adopts the shell's canvas into
 * that id rather than demanding it be named that way. */
#define B3_WEB_CANVAS "#canvas"

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_web_ctx;

/* implemented in web/b3_web_lib.js; runs on the calling (game) thread */
extern void b3_web_gl_diag(char *dst, int cap);

/* ================================================== WHICH GPU, REALLY ====
 * b3_web_gpu_here() asks the context THIS THREAD DRAWS THROUGH; _main() asks
 * the page's.  They can differ, and when they do that difference is the whole
 * bug: a browser may back a worker's OffscreenCanvas with software while the
 * main thread's context is on the GPU, and nothing the port printed before
 * this said which one the player had.  See b3_web_lib.js. */
extern void b3_web_gpu_here(char *dst, int cap);
extern void b3_web_gpu_main(char *dst, int cap);

/* ==================================================== THE HARDWARE PROFILE
 * B3_WEB_HWPROF=<n>.  The counter block, sixteen int32s shared with whichever
 * thread the context object is on.  The order is fixed and b3_web_lib.js
 * writes it; changing one end without the other silently misreports. */
enum { B3_HW_TEX = 0, B3_HW_TEX_KB, B3_HW_COPY, B3_HW_COPY_KPX,
       B3_HW_BUF, B3_HW_BUF_KB, B3_HW_RDPX, B3_HW_RDPX_KPX,
       B3_HW_GETERR, B3_HW_GETPARAM, B3_HW_FLUSH, B3_HW_FBO,
       B3_HW_XFER_US, B3_HW_POST_US, B3_HW_GPU_US, B3_HW_GPU_N, B3_HW_N };
extern int  b3_web_hwprof_arm(int ctl);
extern int  b3_web_hwprof_arm_main(int ctl);
extern void b3_web_hwprof_dump(int frames);
extern void b3_web_hwprof_dump_main(int frames);
static int32_t *g_hw;
static int      g_hw_every;
static int      g_hw_sync;      /* B3_WEB_HWPROF_SYNC: a timed glFinish */
static double   g_hw_finish_ms; /* what that glFinish waited for            */

/* ================================================== THE DIRECT-CONTEXT GLUE
 * All four are implemented in web/b3_web_lib.js; see the long comment there
 * and the one at b3_web_gl_create_context() below for why they exist. */
extern int  b3_web_present_probe(void);                 /* main thread */
extern int  b3_web_present_init(int tid, int ctl, int w, int h); /* main */
extern int  b3_web_worker_ctx(int w, int h, int ctl,
                              int depth, int stencil, int antialias,
                              int major);
extern void b3_web_present_frame(void);                 /* game thread */

/* WHICH WEBGL THE GAME THREAD ACTUALLY GOT -- 1 or 2, or 0 before the context
 * exists.  Read back from the live context object rather than remembered from
 * the request, because the request is allowed to be downgraded and a port that
 * believes its own request is how a feature goes missing quietly. */
extern int  b3_web_ctx_version(void);

/* The present control block, four int32s shared with the main thread:
 *   [0] IN FLIGHT   bitmaps handed over and not yet painted or dropped
 *   [1] SENT        frames posted
 *   [2] DROP_SRC    frames not snapshotted because the cap was reached
 *   [3] DROP_STALE  frames superseded before the page painted them
 * Deliberately never freed: the main thread's message listener holds this
 * address for the life of the page. */
enum { B3_PRES_INFLIGHT = 0, B3_PRES_SENT, B3_PRES_DROP_SRC,
       B3_PRES_DROP_STALE, B3_PRES_N };
static int32_t *g_pres;
static int      g_direct;      /* 1 when the game thread owns its context */

/* ==================================================== THE GL CALL COUNTER
 * B3_WEB_GLCOUNT=<n>.  The buckets must match $b3glc_bucket in b3_web_lib.js. */
enum { B3_GLC_TOTAL = 0, B3_GLC_SYNC, B3_GLC_DRAW, B3_GLC_VERTEX,
       B3_GLC_TEXTURE, B3_GLC_UNIFORM, B3_GLC_PROGRAM, B3_GLC_FBO,
       B3_GLC_QUERY, B3_GLC_STATE, B3_GLC_OTHER, B3_GLC_N };
extern int b3_web_glcount_here(int ctl);
extern int b3_web_glcount_main(int ctl);
extern void b3_web_glcount_dump_here(int frames);
extern void b3_web_glcount_dump_main(int frames);
static int32_t *g_glc;
static int      g_glc_every;

/* =================================================== THE texParameter AUDIT
 * B3_WEB_TEXAUDIT=<n>: every n presents, report the texParameter calls made
 * with NOTHING BOUND -- the calls WebGL answers with
 * "INVALID_OPERATION: texParameter: no texture bound to target" -- and the
 * wasm stack of each distinct site.  See b3_web_lib.js for the mechanism and
 * for why the idiom is a genuine defect rather than a browser quirk. */
extern int  b3_web_texaudit_here(int max_sites);
extern int  b3_web_texaudit_main(int max_sites);
extern void b3_web_texaudit_dump_here(int frames);
extern void b3_web_texaudit_dump_main(int frames);
static int      g_tex_every;

static void b3_web_gl_bench(const char *when);   /* below, past gl4es' bring-up */

/* The zone counter.  See b3_web.h for what it is and why it is exact only on
 * the direct path. */
static const char *const g_zone_name[B3_ZONE_N] = {
    "other", "track", "sky", "props", "scenery", "cars", "traffic", "fx",
    "hud", "postfx"
};
static int32_t g_zone_calls[B3_ZONE_N], g_zone_draws[B3_ZONE_N];
static int     g_zone_cur;
static int32_t g_zone_c0, g_zone_d0;

void b3_web_glc_zone(int zone)
{
    if (!g_glc) return;
    if (zone < 0 || zone >= B3_ZONE_N) zone = B3_ZONE_NONE;
    g_zone_calls[g_zone_cur] += g_glc[B3_GLC_TOTAL] - g_zone_c0;
    g_zone_draws[g_zone_cur] += g_glc[B3_GLC_DRAW]  - g_zone_d0;
    g_zone_cur = zone;
    g_zone_c0  = g_glc[B3_GLC_TOTAL];
    g_zone_d0  = g_glc[B3_GLC_DRAW];
}

/* ============================================= THE INTERNAL RENDER RESOLUTION
 *
 * THE PORT RENDERS AT THE VIEWPORT'S OWN RESOLUTION.  The canvas backing store
 * is the element's displayed CSS size times the device pixel ratio, so a pixel
 * the engine draws is a pixel the display shows, and it follows the element
 * when the page resizes or goes fullscreen.
 *
 * IT USED TO BE PINNED TO 640x480, and that was a call-bound-era mitigation
 * rather than a fidelity decision.  The reasoning was: retail's render target
 * is 640x480, every one of the web's pixels was drawn by gl4es through a WebGL
 * context on the MAIN thread, and fill was expensive in a way it is not
 * natively -- on a 4K display in fullscreen the port was pushing 3840x2160
 * through that path, which was a large part of "FPS is very poor".
 *
 * Every clause of that is now false.  gl4es is gone (the engine talks to WebGL
 * directly), the context is worker-local rather than proxied, and the frame is
 * finished in ~9.7 ms of a 16.7 ms budget on real hardware.  What the pin buys
 * today is a soft, upscaled image -- which is what it looked like, and what
 * was reported.
 *
 * The 4:3-inside-16:9 letterbox goes with it: web/style.css sizes #canvas to
 * fill a 16:9 #frame, the backing store now matches that element exactly, and
 * `object-fit: contain` becomes a no-op rather than a pillarbox.  The ASPECT is
 * the engine's own business and always was -- render_frame() reads
 * SDL_GetWindowSize() every frame for its viewport, its projection aspect and
 * its HUD scale, and the HUD projects through its authored 640x480 virtual
 * space regardless (burnout3_full.c:19789).
 *
 * B3_RES=WxH still overrides, the same knob the desktop window takes, so
 * resolution measurements come out of one binary.  B3_RES=pin is the old
 * behaviour kept as a documented fallback: 640x480, letterboxed by CSS, for
 * anyone on hardware where the viewport-resolution frame does not hold 60. */
static int g_pin_w, g_pin_h;
static int g_res_follow = 1;   /* 0 once B3_RES fixes the size */
/* Two device pixels per CSS pixel, 3840 on the long edge, and a PIXEL BUDGET.
 *
 * The budget is the one that actually binds, and it is measured rather than
 * chosen.  On an RTX 3090 through ANGLE/Vulkan headless, racing US_C1_V1 with
 * the aftereffects chain on:
 *
 *     640x480      16.66 ms   60.0 fps   render_frame  8.86 ms
 *     1920x1080    17.52 ms   57.1 fps   render_frame  9.73 ms
 *     3840x2160    63.61 ms   15.7 fps   render_frame 51.51 ms
 *
 * 1080p costs +0.87 ms of render_frame for 6.75x the pixels -- this port is
 * not fill-bound at that size, which is why the old 640x480 pin bought so
 * little and cost so much in sharpness.  4K is a different regime entirely.
 *
 * So the default renders at the viewport up to a 1080p-class pixel COUNT and
 * scales down past it, preserving aspect: a 1440p or 4K display gets a
 * 1080p-class buffer scaled up by the compositor instead of a frame at 16 fps.
 * B3_RES=WxH overrides the budget for anyone who wants to spend it. */
#define B3_WEB_DPR_MAX  2.0
#define B3_WEB_MAX_EDGE 3840
#define B3_WEB_MAX_PX   (1920 * 1080)

/* ================================================ THE ONE-PIXEL VIEWPORT WOBBLE
 *
 * THE BUG THIS EXISTS FOR.  A user held 60.0 fps for 37 s of racing and then
 * got this, mid-race, with nothing resized and no window touched:
 *
 *     [Burnout3] web: render resolution 1921x1080 (viewport changed)
 *
 * -- ONE PIXEL wider.  The chain rebuilt at the new size, the rebuild failed
 * (see afx_scene_depth_detach() in src/burnout3_aftereffects.c), and the rest
 * of the race ran on the legacy postfx path at ~20 fps.
 *
 * WHERE THE PIXEL COMES FROM.  The measurement above is
 * `(int)(css_px * dpr + 0.5)`, and NEITHER input is an integer.
 * emscripten_get_element_css_size() is getBoundingClientRect(), which reports
 * the FRACTIONAL used size; web/style.css builds #canvas out of `aspect-ratio:
 * 16 / 9` inside a container-query box whose height is itself derived from a
 * flex row (#hud) that reflows when its chips change text -- so the box is
 * 1920.4-something CSS px, not 1920.  devicePixelRatio is fractional too under
 * browser zoom and during a fullscreen transition.  Any sub-pixel nudge that
 * carries the sum across x.5 changes the ROUNDED answer by one, and every
 * downstream consumer treats that as a resolution change: SDL_SetWindowSize,
 * a canvas backing-store realloc, and a full effects-chain rebuild.
 *
 * TWO DEFENCES, because either alone still has a boundary:
 *
 *   QUANTUM.  The measured size is snapped DOWN to a multiple of 4.  A 1-2 px
 *   wobble can no longer produce a different number at all, and the chain's
 *   half- and quarter-resolution targets divide exactly, which they did not at
 *   an odd width (1921 -> 960 -> 480 loses a column at each halving).
 *
 *   DEAD BAND.  Snapping alone just moves the boundary: a box hovering on a
 *   multiple of 4 would flip by 4 instead of by 1.  So the every-30-presents
 *   follow additionally REFUSES to act until the measurement has moved at
 *   least 8 device pixels on some axis from what is currently applied.  A real
 *   resize -- a dragged window, a fullscreen toggle, an OS zoom step -- clears
 *   that by an order of magnitude; layout noise never does.
 *
 * Both are deliberately invisible to the pinned frame: web/smoke.html sizes its
 * canvas 640x480, and 640 and 480 are both multiples of 4. */
#define B3_WEB_RES_QUANTUM  4
#define B3_WEB_RES_DEADBAND 8

/* The pushed-viewport slot: the main thread writes, the game worker reads,
 * nobody blocks.  See b3_web_viewport_watch() in web/b3_web_lib.js for the
 * why and for the seqlock protocol; B3_VP_SEQ is odd while a write is in
 * flight.  Zero until the watcher is installed, which is what makes
 * b3_web_viewport_px() fall back to the old proxied query. */
enum { B3_VP_SEQ, B3_VP_CW_Q10, B3_VP_CH_Q10, B3_VP_DPR_Q20, B3_VP_N };
static int32_t *g_vp;
extern int b3_web_viewport_watch(int slot);       /* main thread */
static void b3_web_defend_pin(SDL_Window *win);   /* below, next to the query */

/* What the viewport is RIGHT NOW, in device pixels: the canvas element's
 * displayed CSS box times the device pixel ratio.
 *
 * Returns 0 if the browser cannot answer (the element is not laid out yet,
 * which happens if this runs before the shell reveals the stage), and the
 * caller keeps whatever it had. */
static int b3_web_viewport_px(int *out_w, int *out_h)
{
    double cw = 0, ch = 0, dpr;

    /* THE PUSHED SLOT FIRST, and it is why this is not a stall.  Both of the
     * calls in the fallback below are `__proxy: 'sync'` in Emscripten, so the
     * old path parked the GAME LOOP on the main thread twice a second; see
     * b3_web_viewport_watch() in web/b3_web_lib.js.  Here the main thread has
     * already written the answer and this is four loads out of our own heap.
     *
     * The seqlock: retry while the counter is odd (a write is in flight) or
     * changed under us.  Bounded, because the writer is a ResizeObserver
     * callback and cannot be preempted by us. */
    if (g_vp && emscripten_atomic_load_u32(&g_vp[B3_VP_SEQ])) {
        int tries;
        for (tries = 0; tries < 64; tries++) {
            uint32_t s0 = emscripten_atomic_load_u32(&g_vp[B3_VP_SEQ]);
            int32_t  qw, qh, qd;
            if (s0 & 1u) continue;
            qw = (int32_t)emscripten_atomic_load_u32(&g_vp[B3_VP_CW_Q10]);
            qh = (int32_t)emscripten_atomic_load_u32(&g_vp[B3_VP_CH_Q10]);
            qd = (int32_t)emscripten_atomic_load_u32(&g_vp[B3_VP_DPR_Q20]);
            if (emscripten_atomic_load_u32(&g_vp[B3_VP_SEQ]) != s0) continue;
            cw  = (double)qw / 1024.0;
            ch  = (double)qh / 1024.0;
            dpr = (double)qd / 1048576.0;
            break;
        }
        if (tries >= 64) return 0;          /* absurd contention: skip a poll */
        if (cw < 1.0 || ch < 1.0) return 0;
    } else {
        /* No watcher (no ResizeObserver, or it has not written yet).  The old
         * proxied path, kept whole so a browser without it still follows. */
        if (emscripten_get_element_css_size(B3_WEB_CANVAS, &cw, &ch)
                != EMSCRIPTEN_RESULT_SUCCESS)
            return 0;
        if (cw < 1.0 || ch < 1.0) return 0;
        dpr = emscripten_get_device_pixel_ratio();
    }
    if (!(dpr > 0.0)) dpr = 1.0;
    /* DPR IS CLAMPED, deliberately.  A phone at dpr 3 inside a 16:9 stage asks
     * for a backing store with nine times the pixels of the CSS box, and the
     * fill is real even though the frame is no longer call-bound.  Two device
     * pixels per CSS pixel is where the sharpness gain stops being visible. */
    if (dpr > B3_WEB_DPR_MAX) dpr = B3_WEB_DPR_MAX;

    *out_w = (int)(cw * dpr + 0.5);
    *out_h = (int)(ch * dpr + 0.5);
    /* A 4K display in fullscreen at dpr 2 would ask for 7680x4320.  Cap the
     * long edge: past this the cost is quadratic and the gain is nil. */
    if (*out_w > B3_WEB_MAX_EDGE) {
        *out_h = (int)((double)*out_h * B3_WEB_MAX_EDGE / (double)*out_w + 0.5);
        *out_w = B3_WEB_MAX_EDGE;
    }
    if (*out_h > B3_WEB_MAX_EDGE) {
        *out_w = (int)((double)*out_w * B3_WEB_MAX_EDGE / (double)*out_h + 0.5);
        *out_h = B3_WEB_MAX_EDGE;
    }
    /* THE PIXEL BUDGET.  Scale the whole rectangle down, so the aspect the
     * engine projects through is still the viewport's own. */
    {
        double px = (double)*out_w * (double)*out_h;
        if (px > (double)B3_WEB_MAX_PX) {
            double k = sqrt((double)B3_WEB_MAX_PX / px);
            *out_w = (int)((double)*out_w * k + 0.5);
            *out_h = (int)((double)*out_h * k + 0.5);
        }
    }
    /* SNAP TO THE QUANTUM.  See B3_WEB_RES_QUANTUM: this is what stops a
     * sub-pixel layout wobble from ever becoming a new render resolution. */
    *out_w -= *out_w % B3_WEB_RES_QUANTUM;
    *out_h -= *out_h % B3_WEB_RES_QUANTUM;
    if (*out_w < 320) *out_w = 320;
    if (*out_h < 240) *out_h = 240;
    return 1;
}

/* Set the canvas backing store AND SDL's idea of the window to the same
 * numbers.
 *
 * THEY HAVE TO AGREE, and getting this wrong is not subtle.  SDL2's Emscripten
 * video driver (SDL_emscriptenvideo.c Emscripten_CreateWindow) probes whether
 * CSS is sizing the canvas -- web/style.css gives #canvas width/height 100% --
 * decides `external_size`, and then sets the backing store to the CSS size and
 * fires SDL_WINDOWEVENT_RESIZED with it.  render_frame() takes its viewport,
 * its projection aspect, its HUD scale and the effects chain's FBO size from
 * SDL_GetWindowSize(), and SDL scales mouse events by window->w / css_w -- so
 * setting the canvas WITHOUT telling SDL leaves the whole frame drawn through a
 * viewport that does not match the framebuffer.  SDL_SetWindowSize() is the one
 * call that moves both together: it updates window->w/h and its driver hook
 * re-sets the canvas to window->w times the device pixel ratio, which SDL
 * treats as 1 here (SDL_WINDOW_ALLOW_HIGHDPI is not requested) -- which is why
 * the explicit emscripten_set_canvas_element_size() follows it rather than
 * being redundant with it. */
static void b3_web_apply_resolution(SDL_Window *win, int w, int h,
                                    const char *why)
{
    if (w < 1 || h < 1) return;
    if (w == g_pin_w && h == g_pin_h) return;
    g_pin_w = w; g_pin_h = h;
    if (win) SDL_SetWindowSize(win, w, h);
    emscripten_set_canvas_element_size(B3_WEB_CANVAS, w, h);
    printf("[Burnout3] web: render resolution %dx%d (%s)\n", w, h, why);
    fflush(stdout);
}

static void b3_web_pin_resolution(SDL_Window *win)
{
    const char *e = getenv("B3_RES");
    int rw, rh, w = 0, h = 0;

    /* Install the viewport watcher before the first measurement, so that even
     * that one is a heap read rather than a blocking round trip.  ONE sync
     * proxy at startup buys every later poll for free; if it declines (no
     * ResizeObserver) g_vp stays quiet and b3_web_viewport_px() keeps asking
     * the old way. */
    if (!g_vp && !getenv("B3_WEB_VP_POLL")) {
        /* B3_WEB_VP_POLL=1 forces the OLD blocking path out of the same
         * binary, which is how the before/after for the stall is measured --
         * same build, same session, one env apart.  See B3_WEB_VPPROF. */
        g_vp = (int32_t *)calloc(B3_VP_N, sizeof *g_vp);
        if (g_vp && !b3_web_viewport_watch((int)(intptr_t)g_vp)) {
            free(g_vp);
            g_vp = NULL;
            printf("[Burnout3] web: no ResizeObserver -- the viewport is "
                   "polled the slow way (a blocking main-thread round trip "
                   "every 30 presents)\n");
            fflush(stdout);
        }
    }

    if (e && *e) {
        /* B3_RES=pin -- the pre-viewport default, kept as a fallback knob for
         * hardware that cannot hold 60 at the display's own resolution. */
        if (!strcmp(e, "pin") || !strcmp(e, "PIN")) {
            g_res_follow = 0;
            g_pin_w = g_pin_h = 0;
            b3_web_apply_resolution(win, 640, 480,
                                    "B3_RES=pin, retail's 640x480 target; "
                                    "CSS letterboxes it");
            return;
        }
        if (sscanf(e, "%dx%d", &rw, &rh) == 2
            && rw >= 320 && rh >= 240 && rw <= 8192 && rh <= 8192) {
            g_res_follow = 0;
            g_pin_w = g_pin_h = 0;
            b3_web_apply_resolution(win, rw, rh, "B3_RES override");
            return;
        }
    }

    /* THE DEFAULT: follow the viewport. */
    g_res_follow = 1;
    if (b3_web_viewport_px(&w, &h)) {
        b3_web_apply_resolution(win, w, h,
                                "viewport: the canvas' CSS box x devicePixelRatio");
    } else {
        /* Not laid out yet.  Take retail's target for the first frames; the
         * every-30-presents follow below picks the real size up as soon as the
         * shell has revealed the stage. */
        b3_web_apply_resolution(win, 640, 480,
                                "viewport not measurable yet -- following");
    }
}

SDL_GLContext b3_web_gl_create_context(SDL_Window *win)
{
    EmscriptenWebGLContextAttributes a;
    const char *e;
    int want_major;

    /* ============================================ THE CANVAS' OWN ANTIALIAS
     *
     * OFF, and it is not the game's MSAA.  `antialias` applies to the REAL
     * canvas backbuffer, and the only thing this port ever draws into that is
     * the aftereffects chain's final full-screen gamma quad -- one triangle,
     * no interior edges, which multisampling cannot improve.  Switching it on
     * buys a multisample resolve over the whole canvas every frame and changes
     * not one pixel.
     *
     * THE SCENE'S MSAA IS ELSEWHERE and is real: src/burnout3_aftereffects.c
     * draws the world into a multisampled renderbuffer pair and resolves it
     * into the chain's scene texture.  That is what B3_MSAA now means on both
     * targets (see B3_AFX_MSAA_DEF), which is why it no longer steers the flag
     * below -- setting B3_MSAA=4 used to pay for BOTH, and only one of them
     * does anything.  B3_WEB_CANVAS_AA=1 keeps the old A/B runnable. */
    int canvas_aa = 0;

    b3_web_pin_resolution(win);
    emscripten_webgl_init_context_attributes(&a);

    /* ================================================== WEBGL 2, WITH A FALL
     *
     * WHY THE VERSION MOVED.  It was pinned to 1 because gl4es was underneath
     * and a GLES 3 context would have changed the path under it for no gain.
     * gl4es is gone -- the engine talks to WebGL directly -- and three things
     * this port wants are WebGL 2-only, all of them measured rather than
     * hoped for:
     *
     *   * glBlitFramebuffer + glRenderbufferStorageMultisample.  Without them
     *     the aftereffects chain runs single-sampled and the web frame has
     *     jagged track edges the desktop does not.  They are core in WebGL 2
     *     and do not exist as extensions in WebGL 1.
     *   * DEPTH_COMPONENT24 as a RENDERBUFFER format.  WebGL 1 guarantees only
     *     DEPTH_COMPONENT16, and the road decals sit ~0.011 units above the
     *     road, which 16 bits stops resolving at about 11 m
     *     (src/burnout3_full.c, by the SDL_GL_DEPTH_SIZE request).  The web
     *     build has been shipping `[afx] chain ready ... depth 16-bit`.
     *   * EXT_disjoint_timer_query_webgl2, the only timer query Chrome offers
     *     on a WebGL 2 context.  This one did NOT pay off and is recorded as
     *     such: the extension is exposed and the profiler now uses the right
     *     entry points, but Chrome still services no TIME_ELAPSED result.  It
     *     is listed here because it was a reason to try, not because it was a
     *     reason that held -- see b3_web_gl_swap() below.
     *
     * NOTHING ELSE HAD TO CHANGE FOR IT.  The GLSL is untouched: every shader
     * in this port is ESSL 1.00 (no `#version`, `attribute`/`varying`/
     * `texture2D`), and WebGL 2 accepts ESSL 1.00 shaders unchanged -- it adds
     * ESSL 3.00, it does not withdraw 1.00.  The Makefile's -sFULL_ES2 went
     * with gl4es and is not needed here either: the retained renderer draws
     * from static VBOs, never client arrays.  The link needs exactly one flag,
     * -sMAX_WEBGL_VERSION=2, which is what makes Emscripten's own
     * emscripten_webgl2_get_proc_address() answer for the two entry points
     * above (system/lib/gl/gl.c gates them on it).
     *
     * THE FALLBACK IS REAL AND KEPT.  -sMIN_WEBGL_VERSION stays 1, so the
     * WebGL 1 bindings are still linked, and a browser without WebGL 2 gets
     * today's behaviour: single-sampled scene target, 16-bit depth, no timer
     * query, everything else identical.  B3_WEBGL=1 forces that path out of
     * the same binary, which is how the before/after here is measured.
     *
     * The version the game thread ENDED UP WITH is read back off the live
     * context and printed below; it is never assumed from the request. */
    want_major = 2;
    if ((e = getenv("B3_WEBGL")) && *e) {
        want_major = atoi(e);
        if (want_major < 1) want_major = 1;
        if (want_major > 2) want_major = 2;
    }
    a.majorVersion = want_major;
    a.minorVersion = 0;
    a.alpha        = 0;
    a.depth        = 1;      /* 24-bit where the browser offers it */
    a.stencil      = 0;
    a.premultipliedAlpha = 0;
    a.preserveDrawingBuffer = 0;
    a.failIfMajorPerformanceCaveat = 0;

    if ((e = getenv("B3_WEB_CANVAS_AA")) && *e) canvas_aa = atoi(e);
    a.antialias = canvas_aa != 0;

    /* ==================================== WHY THE CONTEXT IS NOT PROXIED ANY MORE
     *
     * THE MEASUREMENT THAT FORCED THIS.  Dropping the render target from
     * 1280x720 to 320x240 -- twelve times fewer pixels -- bought 14% of the
     * frame, and the scene+hud stage did not move at all.  The port was never
     * fill-bound.  It was bound by the THREAD BOUNDARY: the context lived on
     * the main thread, so every gl* call gl4es made was dispatched across it,
     * and 68 of Emscripten's 140 GL entry points dispatch SYNCHRONOUSLY
     * (system/lib/gl/webgl1.c) -- glDrawElements and glVertexAttribPointer
     * among them, i.e. the two calls gl4es makes on every single batch.  A
     * frame was tens of thousands of dispatches and thousands of blocking
     * round trips.  See docs/web/webprof_sweep.md.
     *
     * WHY IT USED TO BE PROXIED, and what changed.  Presenting from a worker
     * was the blocker, and the reasoning still holds as far as it went:
     *
     *   - OffscreenCanvas.commit() was REMOVED from browsers.  Measured on the
     *     live context: "GLctx.commit=ABSENT".
     *   - emscripten_webgl_commit_frame() refuses unless the context asked for
     *     explicitSwapControl, and even then the body is a documented no-op:
     *     "swap is implicit".
     *   - Implicit swap means the frame is pushed WHEN THE THREAD RETURNS TO
     *     ITS EVENT LOOP.  This game is a blocking while() loop on a worker.
     *     It never returns.  So a TRANSFERRED canvas can never be presented.
     *
     * What that argument misses is that a canvas does not have to be presented
     * to be read.  A WORKER-LOCAL `new OffscreenCanvas` is never composited by
     * anybody -- it is snapshotted with transferToImageBitmap(), and the
     * bitmap is postMessage'd to the main thread, which drops it into a
     * `bitmaprenderer` context on the visible canvas inside a rAF.  ONE
     * message a frame, transferred rather than copied, replaces every one of
     * those dispatches.
     *
     * And the context created on this thread is a context this thread OWNS,
     * which is the whole point: webgl1.c decides direct-vs-proxied per call
     * from a TLS flag that emscripten_webgl_make_context_current() sets when
     * the handle's owning thread is the calling thread.  Nothing in gl4es or
     * in the engine changes; every call simply stops crossing.
     *
     * THE FALLBACK IS KEPT.  A browser with no OffscreenCanvas, or no
     * bitmaprenderer context, still gets the proxied path below -- slow, but
     * it draws.  B3_WEB_PRESENT=proxy forces it, which is also how the
     * before/after in docs/web/webprof_sweep.md is measured out of one binary. */
    e = getenv("B3_WEB_PRESENT");
    if (!(e && !strcmp(e, "proxy"))) {
        g_pres = (int32_t *)calloc(B3_PRES_N, sizeof *g_pres);
        /* Probe BEFORE creating anything: getContext() is one-shot per canvas
         * element, so a failed direct attempt must not have spent the visible
         * canvas that the proxied fallback needs. */
        if (g_pres && b3_web_present_probe()) {
            /* b3_web_worker_ctx() does the 2-then-1 walk itself, on a FRESH
             * OffscreenCanvas per attempt -- see the note there for why the
             * canvas cannot be reused across a failed getContext(). */
            EMSCRIPTEN_WEBGL_CONTEXT_HANDLE h = (EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)
                (intptr_t)b3_web_worker_ctx(g_pin_w, g_pin_h,
                                            (int)(intptr_t)g_pres,
                                            a.depth, a.stencil, a.antialias,
                                            want_major);
            if (h && emscripten_webgl_make_context_current(h)
                     == EMSCRIPTEN_RESULT_SUCCESS
                  && b3_web_present_init((int)(intptr_t)pthread_self(),
                                         (int)(intptr_t)g_pres,
                                         g_pin_w, g_pin_h)) {
                g_web_ctx = h;
                g_direct  = 1;
            }
        }
    }

    if (!g_direct) {
        a.renderViaOffscreenBackBuffer = 1;
        a.proxyContextToMainThread     = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;

        g_web_ctx = emscripten_webgl_create_context(B3_WEB_CANVAS, &a);
        /* THE SAME 2-THEN-1 WALK, on the visible canvas.  A getContext() that
         * RETURNS NULL does not settle the canvas' context mode -- only a
         * successful one does -- so asking for 'webgl2' and being refused
         * leaves 'webgl' available, which is the standard way every WebGL 2
         * page with a fallback is written.  (The one-shot rule this file warns
         * about elsewhere is about spending a SUCCESSFUL context, which is why
         * b3_web_present_probe() still runs before anything is created.) */
        if (!g_web_ctx && a.majorVersion > 1) {
            a.majorVersion = 1;
            g_web_ctx = emscripten_webgl_create_context(B3_WEB_CANVAS, &a);
        }
        if (!g_web_ctx) {
            fprintf(stderr, "[Burnout3] web: no WebGL context on %s -- is the "
                            "shell's canvas id=\"canvas\"?\n", B3_WEB_CANVAS);
            return NULL;
        }
        if (emscripten_webgl_make_context_current(g_web_ctx)
            != EMSCRIPTEN_RESULT_SUCCESS) {
            fprintf(stderr, "[Burnout3] web: could not make the WebGL context "
                            "current\n");
            return NULL;
        }
    }

    {   /* WHAT WE ASKED FOR AND WHAT WE GOT, on one line, unconditionally.
         * A WebGL 1 fallback is a QUIETER frame, not a broken one -- no MSAA,
         * 16-bit depth -- so it has to announce itself or the next person
         * measuring a jagged web frame has nothing to grep for. */
        int got = b3_web_ctx_version();
        if (got == 2) {
            printf("[Burnout3] web: WebGL 2 (asked for %d) -- the MSAA resolve "
                   "and DEPTH_COMPONENT24 the aftereffects chain wants are "
                   "available\n", want_major);
        } else {
            printf("[Burnout3] web: WEBGL 1 FALLBACK (asked for %d, got %d)%s "
                   "-- no glBlitFramebuffer, so the aftereffects scene target "
                   "runs SINGLE-SAMPLED, and depth falls back to 16-bit\n",
                   want_major, got,
                   want_major == 1 ? " [B3_WEBGL=1]" : "");
        }
        fflush(stdout);
    }

    {   /* say out loud what kind of context this is -- see b3_web_lib.js */
        char diag[512];
        int dw = -1, dh = -1;
        b3_web_gl_diag(diag, (int)sizeof diag);
        emscripten_webgl_get_drawing_buffer_size(g_web_ctx, &dw, &dh);
        printf("[Burnout3] web: GL %s drawingBuffer=%dx%d present=%s\n",
               diag, dw, dh,
               g_direct ? "DIRECT (worker-local OffscreenCanvas -> ImageBitmap)"
                        : "proxied (main-thread context + offscreen blit)");
        fflush(stdout);
    }

    {   /* AND WHICH GPU.  Unconditional, because it is one getExtension at
         * boot and it is the first thing anybody looking at a frame-rate
         * report needs: the WORKER line is the context the game draws
         * through, and it is free to be software while the page's is not. */
        char gpu[384];
        b3_web_gpu_here(gpu, (int)sizeof gpu);
        printf("[Burnout3] web: gpu = %s   <- THE GAME THREAD'S CONTEXT\n", gpu);
        b3_web_gpu_main(gpu, (int)sizeof gpu);
        printf("[Burnout3] web: gpu = %s   (the page's own, for contrast)\n",
               gpu);
        fflush(stdout);
    }

    {   /* arm the call counter, on whichever thread the context object is on */
        const char *g = getenv("B3_WEB_GLCOUNT");
        g_glc_every = (g && *g) ? atoi(g) : 0;
        if (g_glc_every > 0) {
            g_glc = (int32_t *)calloc(B3_GLC_N, sizeof *g_glc);
            if (g_glc) {
                int ok = g_direct ? b3_web_glcount_here((int)(intptr_t)g_glc)
                                  : b3_web_glcount_main((int)(intptr_t)g_glc);
                printf("[Burnout3] web: GL call counter %s\n",
                       ok ? "armed" : "COULD NOT ARM");
                fflush(stdout);
                if (!ok) g_glc_every = 0;
            }
        }
    }

    {   /* and the hardware profile, on the same thread, the same way */
        const char *h = getenv("B3_WEB_HWPROF");
        const char *s = getenv("B3_WEB_HWPROF_SYNC");
        g_hw_every = (h && *h) ? atoi(h) : 0;
        if (g_hw_every > 0) {
            g_hw = (int32_t *)calloc(B3_HW_N, sizeof *g_hw);
            if (g_hw) {
                int ok = g_direct ? b3_web_hwprof_arm((int)(intptr_t)g_hw)
                                  : b3_web_hwprof_arm_main((int)(intptr_t)g_hw);
                g_hw_sync = (s && *s) ? atoi(s) : 0;
                printf("[Burnout3] web: hardware profile %s%s\n",
                       ok ? "armed" : "COULD NOT ARM",
                       (ok && g_hw_sync)
                           ? " (+ a timed glFinish before each present -- this "
                             "is a REAL pipeline sync and perturbs what it "
                             "measures)" : "");
                fflush(stdout);
                if (!ok) { g_hw_every = 0; g_hw = NULL; }
            }
        }
    }

    {   /* and the texParameter audit, on the same thread, the same way */
        const char *t = getenv("B3_WEB_TEXAUDIT");
        g_tex_every = (t && *t) ? atoi(t) : 0;
        if (g_tex_every > 0) {
            int ok = g_direct ? b3_web_texaudit_here(24)
                              : b3_web_texaudit_main(24);
            printf("[Burnout3] web: texParameter audit %s\n",
                   ok ? "armed" : "COULD NOT ARM");
            fflush(stdout);
            if (!ok) g_tex_every = 0;
        }
    }

    /* SDL only ever compares this against NULL. */
    return (SDL_GLContext)(intptr_t)g_web_ctx;
}

/* ===================================================== THE FRAME PROFILER ====
 * See b3_web.h for what each slot means and why SWAP is the interesting one. */
static double g_prof_acc[B3_WEB_PROF_SLOTS];
static double g_hw_acc[B3_WEB_PROF_SLOTS];   /* the hardware profile's own */
static int    g_prof_every = -1;
static long   g_prof_n;

double b3_web_now_ms(void) { return emscripten_get_now(); }

/* TWO ACCUMULATORS, ONE FEED.  B3_WEB_PROF and B3_WEB_HWPROF report different
 * things on different periods, and either must be usable alone -- so each owns
 * an array and clears only its own.  One shared array would have whichever
 * line printed first steal the other's frames. */
void b3_web_prof(int slot, double ms)
{
    if (g_prof_every < 0) {
        const char *e = getenv("B3_WEB_PROF");
        g_prof_every = (e && *e) ? atoi(e) : 0;
        if (g_prof_every < 0) g_prof_every = 0;
    }
    if (slot < 0 || slot >= B3_WEB_PROF_SLOTS) return;
    if (g_prof_every) g_prof_acc[slot] += ms;
    if (g_hw_every)   g_hw_acc[slot]  += ms;
}

/* Called once per present, after FRAME has been accumulated. */
static void b3_web_prof_tick(void)
{
    int i;

    if (g_prof_every <= 0) return;
    if (++g_prof_n < g_prof_every) return;

    {
        double n = (double)g_prof_n;
        double frame = g_prof_acc[B3_WEB_PROF_FRAME] / n;
        double blur  = g_prof_acc[B3_WEB_PROF_BLUR]  / n;
        double gamma = g_prof_acc[B3_WEB_PROF_GAMMA] / n;
        double swap  = g_prof_acc[B3_WEB_PROF_SWAP]  / n;
        double rest  = frame - blur - gamma - swap;
        if (rest < 0.0) rest = 0.0;
        printf("[Burnout3] [webprof] frame %.2f ms (%.1f fps) = scene+hud %.2f"
               " + blur %.2f + gamma %.2f + present %.2f\n",
               frame, frame > 0.0 ? 1000.0 / frame : 0.0,
               rest, blur, gamma, swap);
        fflush(stdout);
    }
    for (i = 0; i < B3_WEB_PROF_SLOTS; i++) g_prof_acc[i] = 0.0;
    g_prof_n = 0;
}

/* The call counter's own tick.  Separate from the frame profiler's so that
 * either can be run alone, and so the per-frame divisor is the number of
 * PRESENTS -- the same denominator the frame time uses.
 *
 * The counters are CUMULATIVE and this prints deltas, rather than zeroing
 * them: under proxying the incrementing happens on the MAIN thread, and a
 * plain store from here would race that and silently lose counts. */
static void b3_web_glc_tick(void)
{
    static int32_t prev[B3_GLC_N];
    static long n;
    int32_t d[B3_GLC_N];
    int i;

    if (g_glc_every <= 0 || !g_glc) return;
    if (++n < g_glc_every) return;

    for (i = 0; i < B3_GLC_N; i++) { d[i] = g_glc[i] - prev[i]; prev[i] += d[i]; }
#define G(x) (d[x])
    printf("[Burnout3] [glcount] per frame: TOTAL %.0f (of which SYNC round "
           "trips %.0f) = draw %.0f + vertex %.0f + texture %.0f + uniform "
           "%.0f + program %.0f + fbo %.0f + query %.0f + state %.0f + other "
           "%.0f   [%s]\n",
           (double)G(B3_GLC_TOTAL)   / (double)n,
           (double)G(B3_GLC_SYNC)    / (double)n,
           (double)G(B3_GLC_DRAW)    / (double)n,
           (double)G(B3_GLC_VERTEX)  / (double)n,
           (double)G(B3_GLC_TEXTURE) / (double)n,
           (double)G(B3_GLC_UNIFORM) / (double)n,
           (double)G(B3_GLC_PROGRAM) / (double)n,
           (double)G(B3_GLC_FBO)     / (double)n,
           (double)G(B3_GLC_QUERY)   / (double)n,
           (double)G(B3_GLC_STATE)   / (double)n,
           (double)G(B3_GLC_OTHER)   / (double)n,
           g_direct ? "direct: none of these crossed a thread"
                    : "PROXIED: every one of these crossed a thread");
#undef G
    {   /* the same frame, split by which pass made the calls */
        char line[512];
        int  len = 0, z;
        for (z = 1; z < B3_ZONE_N; z++) {
            int32_t c = g_zone_calls[z], d = g_zone_draws[z];
            len += snprintf(line + len, sizeof line - (size_t)len,
                            "%s%s %.0f/%.0f", z > 1 ? ", " : "",
                            g_zone_name[z], (double)c / (double)n,
                            (double)d / (double)n);
            g_zone_calls[z] = 0; g_zone_draws[z] = 0;
            if (len >= (int)sizeof line - 32) break;
        }
        g_zone_calls[B3_ZONE_NONE] = 0; g_zone_draws[B3_ZONE_NONE] = 0;
        printf("[Burnout3] [glzone] per frame, calls/draws by pass%s: %s\n",
               g_direct ? "" : " -- MEANINGLESS while proxied, the calls "
                               "execute on another thread", line);
    }
    if (g_direct) b3_web_glcount_dump_here((int)n);
    else          b3_web_glcount_dump_main((int)n);
    if (g_pres)
        printf("[Burnout3] [present] sent %d, dropped-at-source %d, "
               "dropped-stale %d, in flight %d\n",
               g_pres[B3_PRES_SENT], g_pres[B3_PRES_DROP_SRC],
               g_pres[B3_PRES_DROP_STALE], g_pres[B3_PRES_INFLIGHT]);
    fflush(stdout);
    n = 0;
}

/* ===================================================== THE HARDWARE PROFILE
 * Two lines every n presents.  The first is where the frame went in WALL time;
 * the second is what crossed to the driver, which is where a frame that is
 * nowhere in the first line has gone.
 *
 * Deltas, not totals, and for the same reason b3_web_glc_tick() differences
 * rather than zeroes: under proxying the JS side increments on another thread
 * and a store from here would race it. */
static void b3_web_hw_tick(void)
{
    static int32_t prev[B3_HW_N];
    static long n;
    int32_t d[B3_HW_N];
    int i;
    double fn;

    if (g_hw_every <= 0 || !g_hw) return;
    if (++n < g_hw_every) return;
    for (i = 0; i < B3_HW_N; i++) { d[i] = g_hw[i] - prev[i]; prev[i] += d[i]; }
    fn = (double)n;

    {
        double frame = g_hw_acc[B3_WEB_PROF_FRAME] / fn;
        double sim   = g_hw_acc[B3_WEB_PROF_SIM]   / fn;
        double scene = g_hw_acc[B3_WEB_PROF_SCENE] / fn;
        double grab  = g_hw_acc[B3_WEB_PROF_GRAB]  / fn;
        double swap  = g_hw_acc[B3_WEB_PROF_SWAP]  / fn;
        double xfer  = (double)d[B3_HW_XFER_US] / fn / 1000.0;
        double post  = (double)d[B3_HW_POST_US] / fn / 1000.0;
        double rest  = frame - sim - scene - swap;
        char gpu[128];

        /* SCENE already contains the postfx passes and therefore the grab;
         * the grab is printed INSIDE it rather than added, so the parts still
         * sum to the frame. */
        if (rest < 0.0) rest = 0.0;
        if (d[B3_HW_GPU_N] > 0)
            snprintf(gpu, sizeof gpu, "%.2f ms (timer query)",
                     (double)d[B3_HW_GPU_US] / (double)d[B3_HW_GPU_N] / 1000.0);
        else if (g_hw_sync)
            snprintf(gpu, sizeof gpu, "%.2f ms (glFinish backlog, inside "
                     "present)", g_hw_finish_ms / fn);
        else
            snprintf(gpu, sizeof gpu, "n/a (no EXT_disjoint_timer_query; "
                     "B3_WEB_HWPROF_SYNC=1 substitutes a timed glFinish)");
        printf("[Burnout3] [hwprof] frame %.2f ms (%.1f fps) = sim %.2f + "
               "render_frame %.2f (of which the postfx grab %.2f) + present "
               "%.2f (bitmap %.2f + postMessage %.2f) + loop %.2f | gpu %s\n",
               frame, frame > 0.0 ? 1000.0 / frame : 0.0,
               sim, scene, grab, swap, xfer, post, rest, gpu);
    }
    printf("[Burnout3] [hwsync] per frame: readPixels %.2f (%.0f kpx)"
           " | copyTex %.2f (%.0f kpx) | getError %.2f | getParameter %.2f"
           " | finish/flush %.2f | bindFramebuffer %.2f\n",
           (double)d[B3_HW_RDPX] / fn, (double)d[B3_HW_RDPX_KPX] / fn,
           (double)d[B3_HW_COPY] / fn, (double)d[B3_HW_COPY_KPX] / fn,
           (double)d[B3_HW_GETERR] / fn, (double)d[B3_HW_GETPARAM] / fn,
           (double)d[B3_HW_FLUSH] / fn, (double)d[B3_HW_FBO] / fn);
    printf("[Burnout3] [hwupload] per frame: texture %.2f uploads (%.0f KiB)"
           " | buffer %.2f writes (%.0f KiB)\n",
           (double)d[B3_HW_TEX] / fn, (double)d[B3_HW_TEX_KB] / fn,
           (double)d[B3_HW_BUF] / fn, (double)d[B3_HW_BUF_KB] / fn);
    if (d[B3_HW_RDPX] > 0)
        printf("[Burnout3] [hwsync] ^ readPixels IS BEING CALLED IN THE FRAME. "
               "The engine only calls it from capture paths, so this is gl4es "
               "taking glCopyTexSubImage2D's readback fallback "
               "(texture_read.c:159) -- a full GPU->CPU stall every frame. "
               "See postfx_web_grab_mask() in src/burnout3_postfx.c; the "
               "colour-mask guard that is supposed to prevent this is not "
               "holding on this browser.\n");
    if (g_direct) b3_web_hwprof_dump((int)n);
    else          b3_web_hwprof_dump_main((int)n);
    fflush(stdout);
    for (i = 0; i < B3_WEB_PROF_SLOTS; i++) g_hw_acc[i] = 0.0;
    g_hw_finish_ms = 0.0;
    n = 0;
}

int b3_web_hwprof_on(void) { return g_hw_every > 0; }

/* The audit's own tick, on the same denominator as the two above. */
static void b3_web_tex_tick(void)
{
    static long n;

    if (g_tex_every <= 0) return;
    if (++n < g_tex_every) return;
    if (g_direct) b3_web_texaudit_dump_here((int)n);
    else          b3_web_texaudit_dump_main((int)n);
    n = 0;
}

void b3_web_gl_swap(SDL_Window *win)
{
    static int complained;
    static double t_prev_end;
    double t0 = b3_web_now_ms();
    EMSCRIPTEN_RESULT r = EMSCRIPTEN_RESULT_SUCCESS;

    b3_web_defend_pin(win);
    /* B3_WEB_HWPROF_SYNC=1: block until the GPU has actually retired this
     * frame, so the wall time attributed to the present is the GPU's backlog
     * rather than the queue's.  This is a REAL pipeline sync -- it removes the
     * CPU/GPU overlap the frame depends on and makes the total slower -- so it
     * is a diagnostic and never a default.  It is the stand-in for
     * EXT_disjoint_timer_query, and moving to WebGL 2 did NOT retire it.
     * MEASURED 2026-08-25: the WebGL 2 context does expose
     * EXT_disjoint_timer_query_webgl2, and the profiler now takes the core
     * beginQuery/endQuery path with it -- and Chrome still never makes a
     * TIME_ELAPSED result available (241 queries, none landed, on SwiftShader
     * and on ANGLE/Vulkan over an RTX 3090, both headless).  So this glFinish
     * remains the only GPU-time number this harness can report.  See the
     * b3hw_dump note in web/b3_web_lib.js. */
    if (g_hw_sync) {
        double f0 = b3_web_now_ms();
        glFinish();
        g_hw_finish_ms += b3_web_now_ms() - f0;
    }
    if (g_direct) {
        /* One transferToImageBitmap() + one postMessage.  The bitmap is
         * TRANSFERRED, not copied, and the main thread's rAF hands it to the
         * canvas's bitmaprenderer context, which takes ownership of it -- so
         * the only bitmap either side has to close by hand is a dropped one.
         * See b3_web_lib.js for the in-flight cap that bounds how many can
         * exist at once. */
        b3_web_present_frame();
    } else {
        /* With renderViaOffscreenBackBuffer this blits the back buffer onto
         * the real canvas (libwebgl.js blitOffscreenFramebuffer) and the main
         * thread presents it.  The return is CHECKED: the first version of
         * this port ignored it, and it had been quietly returning
         * INVALID_TARGET on every single frame -- a black screen that every
         * log-based test called a pass. */
        r = emscripten_webgl_commit_frame();
    }
    {   /* FRAME spans present-return to present-return, so it is the whole
         * frame including this present; SWAP is the present alone. */
        double t1 = b3_web_now_ms();
        b3_web_prof(B3_WEB_PROF_SWAP, t1 - t0);
        if (t_prev_end > 0.0) b3_web_prof(B3_WEB_PROF_FRAME, t1 - t_prev_end);
        t_prev_end = t1;
        b3_web_prof_tick();
        b3_web_hw_tick();
        b3_web_glc_tick();
        b3_web_tex_tick();
    }
    {   /* the second bench: mid-race, with the queue already saturated */
        static long nf;
        if (++nf == 300) b3_web_gl_bench("mid-race, queue saturated");
    }
    if (r != EMSCRIPTEN_RESULT_SUCCESS && !complained) {
        complained = 1;
        fprintf(stderr, "[Burnout3] web: PRESENT FAILED -- "
                        "emscripten_webgl_commit_frame() returned %d; "
                        "nothing will reach the canvas\n", (int)r);
        fflush(stderr);
    }
}

/* The audio bridge lives at the END of this file, past the image bridge, so
 * that it stays out of the GL sections above. */

/* FOLLOW THE VIEWPORT, or defend the override -- the same every-30-presents
 * slot does both, because they are the same question asked twice.
 *
 * SDL2's Emscripten video driver registers a window resize handler
 * (SDL_emscriptenevents.c Emscripten_HandleResize) that, for a
 * SDL_WINDOW_RESIZABLE window -- which this one is -- overwrites the canvas
 * backing store with the CSS size times the device pixel ratio on every browser
 * resize.  With B3_RES set that is a stomp to undo.  On the default path it is
 * very nearly what we want anyway, except that SDL applies its own idea of the
 * pixel ratio and we apply a clamped one, so the size is recomputed here rather
 * than trusted.
 *
 * WHY EVERY 30 PRESENTS AND NOT A RESIZE EVENT.  Measuring the element costs a
 * round trip to the main thread from the worker the game loop runs on, so it
 * cannot be per-frame; and the browser's resize event does not fire for every
 * way the box can change (a CSS layout shift, the shell revealing the stage,
 * an OS zoom change).  Polling the answer covers all of them for one query
 * every half second, and it is the same slot that already defended the pin.
 * It also drives b3_web_resize_sweep() below, which is a test hook. */

/* THE SCRIPTED RESIZE SWEEP -- B3_WEB_RESIZE_SWEEP=WxH,WxH,...
 *
 * A test hook, and it earns its place: the two defences above mean the
 * VIEWPORT path can no longer be talked into an odd or a 1-px-off size, which
 * is exactly what the rebuild path most needs to be proven against.  This
 * drives b3_web_apply_resolution() directly, so the sweep exercises the real
 * SDL_SetWindowSize + canvas realloc + afx_resize() chain at sizes the
 * measurement would now refuse to produce -- 1921x1080, 637x479 -- and
 * tools/web_resize_sweep.py gates on a `[afx] chain ready` line at each step.
 *
 * One step every B3_WEB_RESIZE_SWEEP_EVERY presents (default 120, i.e. ~2 s),
 * which leaves the follow slot below plenty of room to have run in between.
 * Off, and inert, unless the env is set.
 *
 * IT WAITS FOR THE CHAIN.  Presents start during the load, long before
 * render_frame() first calls b3_afx_frame_begin(), so an unguarded sweep spent
 * its early steps resizing a chain that did not exist yet and proved nothing
 * about them -- measured: steps 1-3 of a five-step sweep produced no
 * `[afx] chain ready` at all.  b3_afx_status() returns NULL exactly while the
 * chain is up (src/burnout3_aftereffects.c sets g_why = NULL on a successful
 * build), so that is the start gun, and it is self-synchronising rather than a
 * guessed delay. */
static void b3_web_resize_sweep(SDL_Window *win)
{
    static const char *list = NULL;
    static int         armed = -1, every = 120, countdown, step;
    const char        *p;
    int                i, w, h;

    if (armed < 0) {
        const char *e;
        list  = getenv("B3_WEB_RESIZE_SWEEP");
        armed = (list && *list) ? 1 : 0;
        if ((e = getenv("B3_WEB_RESIZE_SWEEP_EVERY")) && *e) {
            every = atoi(e);
            if (every < 1) every = 1;
        }
        countdown = every;
        if (armed) {
            /* The viewport follow would undo every step at the next poll --
             * the sweep IS the resolution authority while it runs.  The
             * pin-defence branch below then simply re-asserts whatever step is
             * current, which is what we want. */
            g_res_follow = 0;
            printf("[Burnout3] web: resize sweep armed -- %s (a step every %d "
                   "presents); the viewport follow is off for this run\n",
                   list, every);
            fflush(stdout);
        }
    }
    if (!armed) return;
    /* Nothing to resize until the effects chain has been built once. */
    if (b3_afx_status() != NULL) return;
    if (--countdown > 0) return;
    countdown = every;

    /* walk to the step'th WxH entry; stop (and say so) past the end */
    p = list;
    for (i = 0; i < step && p; i++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    if (!p || !*p) {
        if (armed == 1) {
            armed = 2;
            printf("[Burnout3] web: resize sweep complete (%d steps)\n", step);
            fflush(stdout);
        }
        return;
    }
    step++;
    if (sscanf(p, "%dx%d", &w, &h) != 2 || w < 1 || h < 1) return;
    printf("[Burnout3] web: resize sweep step %d -> %dx%d\n", step, w, h);
    fflush(stdout);
    /* g_pin_w/h are what apply_resolution compares against, and an entry that
     * repeats the current size must still count as a step -- so force it. */
    g_pin_w = g_pin_h = 0;
    b3_web_apply_resolution(win, w, h, "resize sweep");
}

static void b3_web_defend_pin(SDL_Window *win)
{
    static int countdown;
    int w = 0, h = 0;

    if (!win) return;
    b3_web_resize_sweep(win);
    if (--countdown > 0) return;
    countdown = 30;

    if (g_res_follow) {
        /* B3_WEB_VPPROF=1 -- WHAT THE MEASUREMENT ITSELF COSTS THE FRAME LOOP.
         * The whole point of the pushed slot is that this number stops being a
         * main-thread queue latency, so it is worth being able to read it.
         * MAX is the interesting one: the mean was never the complaint. */
        static int   prof = -1;
        static int   nprof;
        static double sum, worst;
        double t0 = 0;
        if (prof < 0) prof = getenv("B3_WEB_VPPROF") != NULL;
        if (prof) t0 = b3_web_now_ms();
        if (!b3_web_viewport_px(&w, &h)) return;
        if (prof) {
            double dt = b3_web_now_ms() - t0;
            sum += dt;
            if (dt > worst) worst = dt;
            /* 10 polls, not 60: a poll is 30 presents, and a headless run
             * under SwiftShader does not reach 1 800 presents of racing. */
            if (++nprof >= 10) {
                printf("[Burnout3] [vpprof] viewport measure x%d: mean %.4f ms,"
                       " MAX %.4f ms (%s)\n", nprof, sum / nprof, worst,
                       g_vp ? "pushed slot" : "blocking main-thread poll");
                fflush(stdout);
                nprof = 0; sum = 0; worst = 0;
            }
        }
        /* THE DEAD BAND.  See B3_WEB_RES_DEADBAND: a measurement that has not
         * moved meaningfully is not a resize, and acting on it costs a canvas
         * realloc and a full effects-chain rebuild for nothing. */
        if (g_pin_w > 0
            && (w - g_pin_w) < B3_WEB_RES_DEADBAND
            && (g_pin_w - w) < B3_WEB_RES_DEADBAND
            && (h - g_pin_h) < B3_WEB_RES_DEADBAND
            && (g_pin_h - h) < B3_WEB_RES_DEADBAND)
            return;
        b3_web_apply_resolution(win, w, h, "viewport changed");
        return;
    }

    if (g_pin_w <= 0) return;
    SDL_GetWindowSize(win, &w, &h);
    if (w == g_pin_w && h == g_pin_h) return;
    SDL_SetWindowSize(win, g_pin_w, g_pin_h);
    if (!g_direct) emscripten_set_canvas_element_size(B3_WEB_CANVAS,
                                                      g_pin_w, g_pin_h);
}

void b3_web_gl_drawable_size(SDL_Window *win, int *out_w, int *out_h)
{
    int w = 0, h = 0;

    (void)win;
    /* Once pinned, ANSWER FROM THE PIN.  b3_web_main_fb_size() routes here from
     * inside gl4es on every FBO unbind, and every query is a synchronous proxy
     * to the main thread; b3_web_defend_pin() is what notices a stomp. */
    if (g_pin_w > 0) {
        if (out_w) *out_w = g_pin_w;
        if (out_h) *out_h = g_pin_h;
        return;
    }
    if (g_web_ctx)
        emscripten_webgl_get_drawing_buffer_size(g_web_ctx, &w, &h);
    if (w <= 0 || h <= 0) { w = 640; h = 480; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* WAS gl4es' main-framebuffer-size hook: built NOEGL, it could not ask a
 * windowing system how big the default framebuffer was, and needed this to
 * restore the viewport on every FBO unbind (src/gl/framebuffers.c:1475) -- the
 * postfx chain binds and unbinds one every frame, and without it the composite
 * landed at the wrong size.  With the layer gone, the engine sets its own
 * viewport and nothing asks.  b3_web_gl_drawable_size() is still the port's
 * answer for anything that needs the size.  [removed with gl4es] */

/* ============================================== THE DISPATCH MICRO-BENCHMARK
 * B3_WEB_GLBENCH=1.  This exists because the headless harness cannot measure
 * the thing phase 1 actually fixes.
 *
 * Chromium --headless rasterises WebGL with SwiftShader, on the CPU, and
 * SwiftShader's per-call cost is so large that it swamps the cost of the
 * thread hop -- so the frame time barely moves whether the calls are proxied
 * or not, and would move enormously on a real GPU where the driver call is
 * nearly free and the hop is not.  Measuring the frame is therefore measuring
 * the wrong thing.
 *
 * So measure the hop DIRECTLY, on three calls chosen for what they do NOT do.
 * The entry points come from SDL_GL_GetProcAddress rather than through gl4es,
 * so what is timed is Emscripten's dispatch and the browser, nothing else:
 *
 *   ASYNC  glDisable(GL_DITHER)   dispatched fire-and-forget
 *                                 (ASYNC_GL_FUNCTION_1, webgl1.c).  Pure
 *                                 enqueue cost -- and ~20 500 of the ~26 500
 *                                 calls in a race frame are in this class, so
 *                                 THIS IS THE HEADLINE NUMBER.
 *   SYNC   glIsEnabled(GL_DITHER) dispatched SYNCHRONOUSLY (a blocking round
 *                                 trip when proxied), but answered inside
 *                                 Chromium's command-buffer client without
 *                                 reaching the GPU process -- so the gap
 *                                 between the two builds is the round trip
 *                                 and not the driver.
 *   FLUSH  glGetError()           also sync-dispatched, but a real pipeline
 *                                 sync in WebGL: it costs on the order of
 *                                 100 us with no proxying at all.  Reported so
 *                                 that cost cannot masquerade as proxy cost.
 *
 * The result is a per-call cost in microseconds that does not depend on the
 * GPU at all, and it multiplies straight through the measured calls per frame.
 *
 * IT IS RUN TWICE, and the two answers are different things.  At init the main
 * thread is parked in its event loop, so a synchronous proxied call has to WAKE
 * it -- that is the cost of a call the renderer makes while the queue is
 * empty, and it is the worst case.  Mid-race the main thread is already
 * spinning through the queue, so the round trip is only the queue latency.
 * Both are real; quoting either alone would be a half-truth. */
static void b3_web_gl_bench(const char *when)
{
    typedef unsigned      (*B3PfnGetError)(void);
    typedef void          (*B3PfnDisable)(unsigned);
    typedef unsigned char (*B3PfnIsEnabled)(unsigned);
    enum { N_FLUSH = 2000, N_CALL = 20000, GL_DITHER_ = 0x0BD0 };
    B3PfnGetError  ge;
    B3PfnDisable   dis;
    B3PfnIsEnabled ise;
    double t0, t_flush, t_sync, t_async;
    volatile unsigned sink = 0;
    int i;
    const char *e = getenv("B3_WEB_GLBENCH");

    if (!(e && *e && atoi(e))) return;
    ge  = (B3PfnGetError)SDL_GL_GetProcAddress("glGetError");
    dis = (B3PfnDisable)SDL_GL_GetProcAddress("glDisable");
    ise = (B3PfnIsEnabled)SDL_GL_GetProcAddress("glIsEnabled");
    if (!ge || !dis || !ise) {
        printf("[Burnout3] [glbench] no entry points -- skipped\n");
        return;
    }

    t0 = b3_web_now_ms();
    for (i = 0; i < N_CALL; i++) dis(GL_DITHER_);
    t_async = (b3_web_now_ms() - t0) * 1000.0 / (double)N_CALL;

    t0 = b3_web_now_ms();
    for (i = 0; i < N_CALL; i++) sink += ise(GL_DITHER_);
    t_sync = (b3_web_now_ms() - t0) * 1000.0 / (double)N_CALL;

    t0 = b3_web_now_ms();
    for (i = 0; i < N_FLUSH; i++) sink += ge();
    t_flush = (b3_web_now_ms() - t0) * 1000.0 / (double)N_FLUSH;

    printf("[Burnout3] [glbench] %s, %s: ASYNC call %.3f us, SYNC call %.3f us,"
           " glGetError (a real pipeline sync) %.3f us\n",
           g_direct ? "DIRECT" : "PROXIED", when, t_async, t_sync, t_flush);
    fflush(stdout);
}

void b3_web_gl_init(void)
{
    /* THERE IS NOTHING TO BRING UP ANY MORE.  This used to hand gl4es its
     * proc-address hook and its main-framebuffer-size hook and then run
     * initialize_gl4es() (it was built NO_INIT_CONSTRUCTOR, so calling any
     * entry point before this trapped the wasm).  The engine now talks to
     * WebGL directly, so the context created in b3_web_gl_setup() is already
     * the whole story.
     *
     * The function stays because it is the port's declared seam and because
     * the version print and the dispatch benchmark below are both wanted --
     * and because b3r_init() is deliberately sequenced AFTER it in
     * burnout3_full.c, which no longer matters for correctness but keeps the
     * bring-up order the Android port also uses. */
    printf("[Burnout3] web: direct WebGL, no compatibility layer -- %s / %s\n",
           glGetString(GL_VERSION)  ? (const char *)glGetString(GL_VERSION)  : "?",
           glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
    fflush(stdout);

    b3_web_gl_bench("main thread parked");
}

/* ==================================================== THE IMAGE BRIDGE ====
 * See b3_web.h for WHY this exists rather than a WORKERFS mount.
 *
 * THE CONTROL BLOCK, six int32s in the shared wasm heap:
 *
 *   [0] STATE   0 idle / 1 request / 2 done / 3 error
 *   [1] OFF_LO  byte offset, low  32 bits   (an Xbox image is bigger than a
 *   [2] OFF_HI  byte offset, high 32 bits    wasm32 pointer, so it is split)
 *   [3] LEN     bytes wanted
 *   [4] DST     where to put them, a wasm heap pointer
 *   [5] GOT     bytes the worker actually read
 *
 * The game thread stores REQUEST and blocks in memory.atomic.wait32; the
 * helper worker is blocked in Atomics.wait on the same address, wakes, does a
 * FileReaderSync read straight into the heap at DST, stores DONE and notifies.
 * Both sides look at the SAME SharedArrayBuffer, so no copy crosses a
 * postMessage boundary.
 */
enum { B3_CTL_STATE = 0, B3_CTL_OFF_LO, B3_CTL_OFF_HI,
       B3_CTL_LEN,       B3_CTL_DST,    B3_CTL_GOT,   B3_CTL_N };
enum { B3_ST_IDLE = 0, B3_ST_REQ = 1, B3_ST_DONE = 2, B3_ST_ERR = 3 };

/* THE CONTRACT PATH.  web/pre.js leaves a zero-byte placeholder here so the
 * existence checks in burnout3_isodata.c pass; the bytes come from the
 * bridge, not from that file. */
#define B3_WEB_ISO_PATH "/iso/game.xiso"

/* Implemented in web/b3_web_lib.js.  Proxied to the main browser thread,
 * which is where the helper worker and the File live.  Returns the image
 * size, or 0 if the shell handed us no file. */
extern double b3_web_iso_bridge_start(int ctl_ptr);

static int32_t           *g_ctl;
static unsigned long long g_iso_size;
static int                g_armed;      /* 0 untried, 1 up, -1 unavailable */

static void b3_web_iso_arm(void)
{
    double sz;

    if (g_armed) return;
    g_armed = -1;

    /* Deliberately never freed: the bridge lives as long as the process, and
     * the helper worker holds this address for its whole life. */
    g_ctl = (int32_t *)calloc(B3_CTL_N, sizeof *g_ctl);
    if (!g_ctl) return;

    sz = b3_web_iso_bridge_start((int)(intptr_t)g_ctl);
    if (!(sz > 0.0)) {
        fprintf(stderr, "[Burnout3] web: no image bridge -- the shell passed "
                        "no Module.b3IsoFile\n");
        return;
    }
    g_iso_size = (unsigned long long)sz;
    g_armed = 1;
    printf("[Burnout3] web: image bridge up, %llu bytes "
           "(helper worker + shared heap, no copy)\n", g_iso_size);
    fflush(stdout);
}

int b3_web_iso_claims(const char *path)
{
    if (!path || strcmp(path, B3_WEB_ISO_PATH) != 0) return 0;
    b3_web_iso_arm();
    return g_armed == 1;
}

unsigned long long b3_web_iso_size(void)
{
    return g_armed == 1 ? g_iso_size : 0ull;
}

long b3_web_iso_pread(void *dst, unsigned long long off, unsigned long len)
{
    uint32_t st;

    if (g_armed != 1 || !dst) return -1;
    if (len == 0) return 0;

    g_ctl[B3_CTL_OFF_LO] = (int32_t)(uint32_t)(off & 0xFFFFFFFFull);
    g_ctl[B3_CTL_OFF_HI] = (int32_t)(uint32_t)(off >> 32);
    g_ctl[B3_CTL_LEN]    = (int32_t)len;
    g_ctl[B3_CTL_DST]    = (int32_t)(intptr_t)dst;
    g_ctl[B3_CTL_GOT]    = 0;

    emscripten_atomic_store_u32(&g_ctl[B3_CTL_STATE], B3_ST_REQ);
    emscripten_atomic_notify(&g_ctl[B3_CTL_STATE], 1);

    /* Blocks the game thread exactly as pread() would.  Legal because this
     * thread is a worker -- the main browser thread may not block, which is
     * one more reason the image cannot be served from there. */
    while ((st = emscripten_atomic_load_u32(&g_ctl[B3_CTL_STATE])) == B3_ST_REQ)
        emscripten_atomic_wait_u32(&g_ctl[B3_CTL_STATE], B3_ST_REQ, -1);

    return st == B3_ST_DONE ? (long)g_ctl[B3_CTL_GOT] : -1;
}

/* ====================================================== THE AUDIO BRIDGE ====
 * See b3_web.h for WHY the sink moves and the mixer does not.  This is the
 * game-side half: the ring, the pump thread, and the four SDL entry points.
 *
 * THE CONTROL BLOCK, twelve int32s in the shared wasm heap.  The worklet
 * writes READ / UNDER / SILENCE / STATE / RATE / QUANTUM / RMS*, and the
 * library's main-thread half writes the two latency slots; this side writes
 * WRITE and reads the rest.  Every slot is a plain int32 touched with wasm
 * atomics, because the two ends are on different threads and one of them (the
 * audio rendering thread) MAY NOT BLOCK -- so there is no lock between them.
 *
 *   [0] WRITE     frames the pump has produced   (monotonic, wraps as int32)
 *   [1] READ      frames the worklet has consumed (ditto)
 *   [2] UNDER     quanta the worklet found short
 *   [3] SILENCE   frames of silence it substituted
 *   [4] STATE     0 until the worklet's first process(), then 1
 *   [5] RATE      the context's sample rate, as the worklet sees it
 *   [6] QUANTUM   its render quantum (128 everywhere, but measured not assumed)
 *   [7] RMS       the newest window's RMS x 32767
 *   [8] RMSMAX    the loudest window's RMS x 32767
 *   [9] RMSN      windows measured -- 0 means the worklet has produced nothing
 *  [10] BASELAT   the context's own baseLatency, in microseconds
 *  [11] OUTLAT    its outputLatency, in microseconds (0 until it is running)
 *
 * WRITE and READ are MONOTONIC counters, never masked.  Their difference is
 * taken in int32 arithmetic so it stays correct across the wrap (which is
 * 2^31 / 44100 = 13.5 hours of audio away); only the INDEX is masked.
 */
enum { B3_AUD_WRITE = 0, B3_AUD_READ,   B3_AUD_UNDER, B3_AUD_SILENCE,
       B3_AUD_STATE,     B3_AUD_RATE,   B3_AUD_QUANTUM,
       B3_AUD_RMS,       B3_AUD_RMSMAX, B3_AUD_RMSN,
       B3_AUD_BASELAT,   B3_AUD_OUTLAT, B3_AUD_CTL_N };

/* THE RING.  8192 frames at the engine's 44100 is 185.8 ms of capacity, and
 * the pump fills to B3_AUD_TARGET of it.  The target IS the latency: the
 * worklet reads roughly that far behind the mixer, plus its own quantum
 * (2.7 ms at 48 kHz) and whatever the context's output adds.
 *
 * 3072 frames = 69.7 ms.  Lower is tighter and costs underruns on the frames
 * this port actually has -- a materialise off the disc blocks for tens of
 * milliseconds and the pump can be scheduled behind it.  The 116 ms of
 * headroom above the target is what absorbs a pump that woke up late.
 * A power of two, so the index masks. */
#define B3_AUD_RING    8192
#define B3_AUD_MASK    (B3_AUD_RING - 1)
#define B3_AUD_TARGET  3072
#define B3_AUD_MIXRATE 44100          /* what audio_callback() produces */

static int32_t        *g_aud_ctl;
static float          *g_aud_ring;
static Sint16         *g_aud_tmp;     /* one callback's worth, S16 as SDL gives */
static SDL_AudioSpec   g_aud_spec;
static int             g_aud_frames;  /* spec.samples: frames per callback */
static int             g_aud_up;      /* 1 once the pump thread is running */
static volatile int    g_aud_paused = 1;   /* written by the game thread */
static volatile int    g_aud_quit;         /* set from atexit, see below */
static double          g_aud_ctx_rate;
static pthread_t       g_aud_thread;
static pthread_mutex_t g_aud_lock = PTHREAD_MUTEX_INITIALIZER;
static double          g_aud_stats_every = 10.0;   /* B3_WEB_AUDIO_STATS */

/* Implemented in web/b3_web_lib.js, proxied to the main browser thread, which
 * is the only place an AudioContext exists.  Returns the context's sample
 * rate, or 0 if the shell handed us no context.  The worklet module loads
 * ASYNCHRONOUSLY after this returns and there is nothing to wait for: the ring
 * is filled either way, and the worklet starts draining when it is ready. */
extern double b3_web_audio_start(int ctl_ptr, int ring_ptr, int ring_frames,
                                 int mix_rate);

/* THE STATS LINE.  It exists because "the port has no sound" is not a number,
 * and every part of this bridge can fail quietly: no context, a worklet that
 * never loaded, a pump starved by the game thread, or a mix that is genuinely
 * silent because nothing asked for a sound.  The line separates all four --
 * mixed/played say whether the ring is moving, underruns say whether it is
 * moving fast enough, and rms says whether what moved was audible.
 * B3_WEB_AUDIO_STATS=<seconds>, 0 to silence it. */
static void b3_web_audio_stats(void)
{
    static double t_next;
    static int32_t w0, r0, u0;
    double now = emscripten_get_now();
    int32_t w, r, u, s;

    if (g_aud_stats_every <= 0.0) return;
    if (t_next == 0.0) { t_next = now + g_aud_stats_every * 1000.0; return; }
    if (now < t_next) return;
    t_next = now + g_aud_stats_every * 1000.0;

    w = (int32_t)emscripten_atomic_load_u32(&g_aud_ctl[B3_AUD_WRITE]);
    r = (int32_t)emscripten_atomic_load_u32(&g_aud_ctl[B3_AUD_READ]);
    u = (int32_t)emscripten_atomic_load_u32(&g_aud_ctl[B3_AUD_UNDER]);
    s = (int32_t)emscripten_atomic_load_u32(&g_aud_ctl[B3_AUD_SILENCE]);

    printf("[Burnout3] [audio] ring %d/%d frames (%.1f ms) | mixed %d, played "
           "%d in %.0fs | underruns %d (+%d), silence %d frames | rms %.4f "
           "peak %.4f over %d windows | worklet %d Hz, quantum %d, live %d, "
           "out latency %.1f ms\n",
           (int)(w - r), B3_AUD_RING,
           (double)(w - r) * 1000.0 / B3_AUD_MIXRATE,
           (int)(w - w0), (int)(r - r0), g_aud_stats_every,
           (int)u, (int)(u - u0), (int)s,
           (double)g_aud_ctl[B3_AUD_RMS] / 32767.0,
           (double)g_aud_ctl[B3_AUD_RMSMAX] / 32767.0,
           (int)g_aud_ctl[B3_AUD_RMSN],
           (int)g_aud_ctl[B3_AUD_RATE], (int)g_aud_ctl[B3_AUD_QUANTUM],
           (int)g_aud_ctl[B3_AUD_STATE],
           (double)g_aud_ctl[B3_AUD_OUTLAT] / 1000.0);
    fflush(stdout);
    w0 = w; r0 = r; u0 = u;
}

/* THE SHUTDOWN GUARD.  main() returning does not stop the pump thread, and the
 * engine's exit path frees the very state audio_callback() reads -- so the
 * pump has to be told to stop calling it BEFORE that happens.  atexit is the
 * hook that runs first, and after it the pump only sleeps.  (The desktop gets
 * this for free: SDL closes the device.) */
static void b3_web_audio_atexit(void)
{
    g_aud_quit = 1;
    g_aud_paused = 1;
}

/* THE PUMP.  SDL's audio thread, in every respect the engine can observe: it
 * calls spec.callback with spec.samples frames, under the same lock fe_play()
 * takes, and it is the only caller.
 *
 * It TOPS UP rather than paces: sleep a little, then produce until the ring is
 * at its target.  That is what survives a game thread which has just spent
 * 200 ms materialising a track -- the pump is a separate thread and keeps its
 * own cadence, and the target's headroom covers the gap when it does not.
 *
 * The 4 ms sleep is far under the ring's 116 ms of headroom above the target,
 * so this normally produces one callback (23.2 ms of audio) every few wakes
 * and is asleep the rest of the time. */
static void *b3_web_audio_pump(void *arg)
{
    (void)arg;
    for (;;) {
        if (!g_aud_paused && !g_aud_quit) {
            for (;;) {
                int32_t w = (int32_t)emscripten_atomic_load_u32(
                                &g_aud_ctl[B3_AUD_WRITE]);
                int32_t r = (int32_t)emscripten_atomic_load_u32(
                                &g_aud_ctl[B3_AUD_READ]);
                int32_t fill = w - r;
                int i;

                if (fill >= B3_AUD_TARGET) break;
                if (B3_AUD_RING - fill < g_aud_frames) break;

                /* VERBATIM.  The engine's own callback, its own buffer size,
                 * and the same lock the desktop's SDL device would hold. */
                pthread_mutex_lock(&g_aud_lock);
                g_aud_spec.callback(g_aud_spec.userdata, (Uint8 *)g_aud_tmp,
                                    g_aud_frames * (int)sizeof *g_aud_tmp);
                pthread_mutex_unlock(&g_aud_lock);

                for (i = 0; i < g_aud_frames; i++)
                    g_aud_ring[(w + i) & B3_AUD_MASK] =
                        (float)g_aud_tmp[i] * (1.0f / 32768.0f);

                /* The store PUBLISHES the samples above, so it is the last
                 * thing that happens and it is atomic: the worklet may read
                 * WRITE at any instant and must never see a count that runs
                 * ahead of the frames behind it. */
                emscripten_atomic_store_u32(&g_aud_ctl[B3_AUD_WRITE],
                                            (uint32_t)(w + g_aud_frames));
            }
        }
        if (!g_aud_quit) b3_web_audio_stats();
        emscripten_thread_sleep(4.0);
    }
    return NULL;
}

/* THE DEVICE OPEN.  Answers with a fake device id -- the caller only ever
 * tests it for non-zero and hands it back to Lock/Unlock/Pause, all three of
 * which are ours.  0 still means "no sound", and it still means what it always
 * meant: the shell gave us no AudioContext. */
SDL_AudioDeviceID b3_web_open_audio_device(const SDL_AudioSpec *want)
{
    static int said;
    const char *e;

    if (g_aud_up) return 1;
    if (!want || !want->callback) return 0;

    /* Everything downstream of audio_callback() mixes at 44100 mono S16 and
     * the ring is built for exactly that.  Refuse rather than quietly play it
     * at the wrong pitch. */
    if (want->freq != B3_AUD_MIXRATE || want->channels != 1
        || want->format != AUDIO_S16SYS) {
        fprintf(stderr, "[Burnout3] web: the audio bridge carries 44100 mono "
                        "S16; the engine asked for %d Hz x%d fmt 0x%04x -- "
                        "silent\n", (int)want->freq, (int)want->channels,
                (unsigned)want->format);
        fflush(stderr);
        return 0;
    }

    if ((e = getenv("B3_WEB_AUDIO_STATS")) && *e) g_aud_stats_every = atof(e);

    /* Deliberately never freed: the worklet holds these two addresses for the
     * life of the page, exactly as the image bridge's helper worker does. */
    g_aud_ctl    = (int32_t *)calloc(B3_AUD_CTL_N, sizeof *g_aud_ctl);
    g_aud_ring   = (float *)calloc(B3_AUD_RING, sizeof *g_aud_ring);
    g_aud_spec   = *want;
    g_aud_frames = want->samples ? want->samples : 1024;
    g_aud_tmp    = (Sint16 *)calloc((size_t)g_aud_frames, sizeof *g_aud_tmp);
    if (!g_aud_ctl || !g_aud_ring || !g_aud_tmp) return 0;

    g_aud_ctx_rate = b3_web_audio_start((int)(intptr_t)g_aud_ctl,
                                        (int)(intptr_t)g_aud_ring,
                                        B3_AUD_RING, B3_AUD_MIXRATE);
    if (!(g_aud_ctx_rate > 0.0)) {
        if (!said) {
            said = 1;
            printf("[Burnout3] web: running SILENT -- the shell passed no "
                   "Module.audioContext, and an AudioContext cannot be made "
                   "on a worker thread\n");
            fflush(stdout);
        }
        return 0;
    }

    if (pthread_create(&g_aud_thread, NULL, b3_web_audio_pump, NULL) != 0) {
        fprintf(stderr, "[Burnout3] web: the audio pump thread would not "
                        "start -- silent\n");
        fflush(stderr);
        return 0;
    }
    g_aud_up = 1;
    atexit(b3_web_audio_atexit);
    {   /* THE LATENCY, all of it and named, because the ring is only the part
         * this port chose.  The context's own baseLatency is the browser's
         * share and nothing here can shrink it. */
        double ring_ms = (double)B3_AUD_TARGET * 1000.0 / B3_AUD_MIXRATE;
        double q_ms    = 128.0 * 1000.0 / g_aud_ctx_rate;  /* the spec's fixed
                                                            * render quantum */
        double base_ms = (double)g_aud_ctl[B3_AUD_BASELAT] / 1000.0;
        printf("[Burnout3] web: audio bridge up -- AudioWorklet on the main "
               "thread reading a %d-frame shared-heap ring; engine mixes %d Hz "
               "mono, context runs %.0f Hz (resampled in the worklet, %.4f in "
               "per out)\n"
               "           latency ~%.1f ms = ring target %.1f + quantum %.1f "
               "+ context baseLatency %.1f\n",
               B3_AUD_RING, B3_AUD_MIXRATE, g_aud_ctx_rate,
               (double)B3_AUD_MIXRATE / g_aud_ctx_rate,
               ring_ms + q_ms + base_ms, ring_ms, q_ms, base_ms);
    }
    fflush(stdout);
    return 1;
}

void b3_web_pause_audio_device(SDL_AudioDeviceID dev, int pause_on)
{
    (void)dev;
    g_aud_paused = pause_on ? 1 : 0;
}

/* fe_play() mutates the front-end voice table under these, and on the desktop
 * that stops SDL's audio thread mid-callback.  Here it stops the pump the same
 * way -- which is why the pump takes this lock and nothing else does. */
void b3_web_lock_audio_device(SDL_AudioDeviceID dev)
{
    (void)dev;
    if (g_aud_up) pthread_mutex_lock(&g_aud_lock);
}

void b3_web_unlock_audio_device(SDL_AudioDeviceID dev)
{
    (void)dev;
    if (g_aud_up) pthread_mutex_unlock(&g_aud_lock);
}
