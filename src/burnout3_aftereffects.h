#ifndef BURNOUT3_AFTEREFFECTS_H
#define BURNOUT3_AFTEREFFECTS_H

/* AFTEREFFECTS — the in-race screen-space effects chain.
 * =====================================================
 *
 * This module owns the post-processing chain: the scene render target, the
 * downsample prefilter, the radial speed blur, the highlight bloom, the
 * present composite and the output gamma ramp.  It replaces the grab-based
 * chain in burnout3_postfx.c (`b3_postfx_blur` + `b3_postfx_gamma`), which
 * stays compiled as the fallback for contexts that cannot give us FBOs.
 *
 *
 * EVIDENCE MARKS.  This file follows the project convention, with one
 * addition the 2026-08-24 mandate introduced:
 *
 *   [C]        confirmed against the retail image at the cited address
 *   [S]        strongly supported, not proven
 *   [?]        unknown
 *   GLUE       harness-side invention standing in for something recovered
 *   INSPIRED   deliberately NOT retail: a modern construction chosen because
 *              it looks better, guided by what retail was reaching for.  An
 *              INSPIRED line is not a claim about the game and must never be
 *              cited as one.
 *
 * The split, honestly:
 *
 *   RECOVERED [C]  the render-to-texture architecture (a 640x480 scene
 *                  surface reduced 2x twice), the present pass's x2
 *                  SHIFTLEFTBY1, the alpha law C0.a = min(s,2)*0.5, and the
 *                  output gamma ramp round((i/255)^0.95*255).
 *   GLUE           the speed -> s strength ramp. Retail's producer of
 *                  blurState+0x54 has never been found; this ramp is fitted
 *                  to the xemu captures.
 *   INSPIRED       the RADIAL shape of the blur, the BLEND OPERATOR of the
 *                  present composite, the bloom, and the crash treatment.
 *
 * THE BLEND OPERATOR MOVED FROM THE FIRST LIST TO THE THIRD ON 2026-08-24,
 * and this file used to claim otherwise. Retail's composite is an ADD --
 * out = 2*(scene + C0.a*blur) [C] -- and an add cannot blur: the sharp scene
 * is still composited at unit weight underneath, so raising C0.a only adds
 * light. That was measured, not argued: on a pinned 90 mph frame, driving
 * C0.a to its recovered ceiling of 1.0 moved the frame's mean radial gradient
 * energy from 1.000 to 1.005 (i.e. nothing), and 60 -> 150 mph + boost was
 * six visually identical frames. It shipped that way and a user reported it
 * as "I don't see any blur effects on PC".
 *
 * The port now CROSS-FADES instead:  out = 2 * mix(scene, blur, C0.a*mask).
 * Both recovered endpoints survive -- C0.a is untouched, the x2 is untouched,
 * and at C0.a == 0 the expression is the recovered 2*scene bit for bit -- but
 * the operator between them is a modern choice and is labelled as one here,
 * over AFX_FS_COMPOSITE in the .c, and in docs/RE_POSTFX.md 4d.
 * B3_AFX_PRESENT_ADD=1 restores the literal recovered add so the [C] equation
 * stays runnable and checkable, not merely written down.
 *
 * That third line matters and is a 2026-08-24 CORRECTION to what this port
 * used to believe.  Retail's own screen blur is **not radial**: its third
 * reduction pass is a 3-tap HORIZONTAL blur (weights 0.4/0.4/0.4, offsets
 * -/+ 4/3 in x, all three samplers on the same 160x120 surface), the
 * `radialblurmask` texture is never bound by it, and the 0.99 / 0.9999 "zoom
 * layers" the blur state carries are written by the constructor and read by
 * nothing.  The radial construction here is a deliberate modern choice, not a
 * recovered one -- see the long note over AFX_FS_RADIAL in the .c, and
 * docs/RE_POSTFX.md section 4d.  Do not cite it as game behaviour.
 *
 *
 * WHY THIS EXISTS — the architecture, not the effects
 * ---------------------------------------------------
 * The old chain drew straight to the back buffer and then COPIED the finished
 * canvas into a texture (glCopyTexSubImage2D) twice per frame: once for the
 * blur composite and once again for the gamma pass.  On the web that copy was
 * measured as the single largest cost in the frame -- gl4es resolves it to a
 * synchronous glReadPixels, which drains the pipeline (docs/web/webprof_sweep
 * .md, and the 4da0bef commit).  It is also why the web build shipped with
 * the blur switched off.
 *
 * Here the scene is rendered INTO a texture-backed FBO from the start, so
 * there is nothing to copy: every pass is an ordinary shader draw that reads
 * one texture and writes another.  Two full-canvas copies per frame become
 * zero, which is why the chain can carry MORE effects than the old one and
 * still cost less.
 *
 * The frame is therefore:
 *
 *     b3_afx_frame_begin()    scene FBO bound, cleared          <- hook 1
 *        ... the whole 3D world draws, exactly as before ...
 *     b3_afx_scene_done()     downsample -> blur -> bloom       <- hook 2
 *                             -> composite into the UI target,
 *                             which is left bound
 *        ... the HUD draws, exactly as before ...
 *     b3_afx_frame_end()      gamma ramp -> the default FBO     <- hook 3
 *
 * Three hooks in render_frame(), nothing else in the engine moves.  The HUD
 * lands after the composite and before the ramp, which is retail's order:
 * FUN_0003DA90 presents, the HUD draws at 0x001AE620 onward, and the gamma
 * ramp is a scanout transform over the lot. [C]
 *
 *
 * ONE CODE PATH ON BOTH TARGETS.  There is no #ifdef in any pass.  Desktop
 * and web run the same shaders over the same targets in the same order; the
 * only platform-conditional code in the file is how the GL entry points are
 * looked up, and the depth format the driver will accept.
 */

/* Everything the chain needs to know about the frame it is dressing.  All of
 * it is read-only: this module never influences the simulation, and in
 * particular the time-dilation `divisor` is CONSUMED here and never written.
 * Gameplay timing is not a visual style. */
typedef struct B3AfxInputs {
    float speed_mph;    /* the car's speed. Drives the GLUE strength ramp.   */
    float boost_ramp;   /* racecar+0x11AC, 0..2 -- the quantity the recovered
                         * FOV 90->110 law consumes (RE_TAKEDOWN_FX 9.2).
                         * The old call site passed a hard 0 here, so the
                         * boost coupling was dead; it is wired now.         */
    int   divisor;      /* the time-dilation divisor: 1 normal, 3/4/5/6 the
                         * crash + aftertouch ladder (RE_TAKEDOWN_FX 1.2).
                         * READ ONLY -- drives the INSPIRED crash treatment. */
} B3AfxInputs;

/* ---------------------------------------------------------------- the hooks */

/* Bind the scene target and clear it.  `w` x `h` is the drawable size.
 * Returns 1 when the chain is live and the caller must pair this with
 * b3_afx_scene_done() + b3_afx_frame_end(); returns 0 when the chain could
 * not be built (no FBOs, no GLSL, or B3_AFX=0), in which case the caller
 * renders to the default framebuffer and uses the old postfx path. */
int  b3_afx_frame_begin(int w, int h);

/* Run the chain over the finished 3D scene and leave the UI target bound so
 * the HUD draws into it.  Only legal between a b3_afx_frame_begin() that
 * returned 1 and the matching b3_afx_frame_end(). */
void b3_afx_scene_done(int w, int h, const B3AfxInputs *in);

/* Resolve the UI target to the default framebuffer through the retail gamma
 * ramp.  Returns 1 when the ramp was applied in GL (so the capture paths must
 * NOT apply the software table as well), 0 otherwise. */
int  b3_afx_frame_end(int w, int h);

/* Drop every GL object.  Safe to call without a context. */
void b3_afx_shutdown(void);

/* WHY the chain is not running, as one short phrase, or NULL while it is.
 *
 * Added 2026-08-24 after "I don't see any blur effects on PC": the chain
 * degrades to the older postfx path on any of eight different conditions, and
 * before this there was no way to ask which one without reading a log.  The
 * caller prints it on its single unconditional post-path verdict line, so the
 * first thing the game says about post-processing also says why. */
const char *b3_afx_status(void);

/* ------------------------------------------------- the laws, GL-free for
 * tools/validate_postfx.py, which compiles and EXECUTES them as a probe. */

/* The crash/aftertouch weight the INSPIRED treatment runs on: 0 at divisor 1,
 * then 1 - 1/divisor, i.e. 0.667 / 0.75 / 0.8 / 0.833 for the retail ladder
 * 3 / 4 / 5 / 6.  Monotone in the divisor and 0 exactly when time is not
 * dilated. */
float b3_afx_crash_weight(int divisor);

/* The blur strength `s` handed to the recovered composite alpha law.  This is
 * b3_postfx_blur_strength()'s GLUE speed/boost ramp plus the INSPIRED crash
 * term; the s -> C0.a conversion afterwards is the recovered
 * b3_postfx_present_alpha(). */
float b3_afx_blur_s(float speed_mph, float boost_ramp, int divisor);

/* The radial mask's inner radius for a given crash weight: the sharp centre
 * pulls in as time dilates, so a wreck's slow-motion smear reaches further
 * into frame than a speed smear ever does. INSPIRED. */
float b3_afx_mask_r0(float crash_weight);

/* --------------------------------------------------------------- constants */

/* INSPIRED, and explicitly NOT retail.  A 2026-08-24 sweep established a
 * clean negative: retail changes NO post-processing during a crash or during
 * time dilation.  The dilation globals DAT_0060EA18 / DAT_0060EA24 and the
 * audio rate scale DAT_003EBFD0 have ZERO reads anywhere in the renderer /
 * postfx range 0x00028000..0x00045000 or in the world draw
 * 0x001AE340..0x001AEB00; FUN_0003DA90 and FUN_0003E520 have exactly two
 * callers each, both unconditional, both handed the same DAT_004D6524+0x50;
 * and the crash entry FUN_00025CC0 / impact machine FUN_00026050 /
 * aftertouch shaper FUN_00118410 touch nothing on the blur or present path.
 * No flash, no desaturation, no colour matrix either.
 *
 * So the drama below is a deliberate addition, asked for and labelled.  It
 * reads the divisor and never writes it: the aftertouch and crash-cinematic
 * MECHANICS are gameplay and are not this module's business.
 *
 * The three magnitudes, at full crash weight: */
#define B3_AFX_CRASH_S        0.55f  /* extra `s` -- on top of the speed ramp */
#define B3_AFX_CRASH_DESAT    0.30f  /* pull toward luma                      */
#define B3_AFX_CRASH_VIGNETTE 0.22f  /* corner darkening                      */
#define B3_AFX_CRASH_MASK_R0  0.05f  /* the mask's inner radius at full crash */

/* INSPIRED.  The highlight bloom.
 *
 * The threshold is in RENDER-TARGET space, i.e. BEFORE the recovered x2, and
 * that is the whole reason it is where it is.  The composite doubles, so a
 * render-target value of 0.5 is exactly what lands on 255 -- everything above
 * 0.5 RT is detail the present pass was going to throw away as a flat white
 * plate.  Thresholding just UNDER that, at 0.40, means the bloom picks up
 * precisely the highlights that are about to clip and hands them back as glow.
 *
 * MEASURED, and it is why this is 0.40 and not the 0.60 first written here:
 * the scene render target is 8-bit, so it cannot hold anything above 1.0, and
 * on US_C3_V1 at 96 mph its mean sits near 0.20 with even the sunlit cloud
 * tops only reaching ~0.45.  A 0.60 threshold selected NOTHING -- the bloom
 * pass ran every frame and the composite added exactly zero, bit for bit
 * (B3_AFX_BLOOM=0 came out pixel-identical to bloom on).  A dead effect that
 * still costs its passes is worse than no effect. */
#define B3_AFX_BLOOM_THRESHOLD 0.40f
#define B3_AFX_BLOOM_GAIN      0.55f

/* INSPIRED.  Extra bloom while boosting, as a multiplier on the gain.  The
 * ramp's SHAPE is the recovered one: 1 - (racecar+0x11AC - 1)^2, the same
 * curve the [C] FOV 90->110 law consumes at 0x0015ED5C.  Reusing it here is a
 * modelling choice, not a recovered coupling -- but it means the screen heats
 * up on exactly the curve the camera widens on, which is what makes a boost
 * read as one event rather than two. */
#define B3_AFX_BLOOM_BOOST     0.80f

/* INSPIRED.  Tap count for the radial pass.  Retail's own count is 3 [C] --
 * but retail's pass is a horizontal smear, not a radial one, so the number is
 * not transferable.  16 is what the old chain used and it survives here
 * because the taps now run at QUARTER resolution, where sixteen of them cost
 * a sixteenth of what they used to.
 *
 * MEASURED 2026-08-24, and it is why this stayed at 16 during the retune: the
 * taps are a zoom trail, so the count sets the trail's LENGTH (0.99^n), and a
 * sweep of 16 / 24 / 32 / 48 on a pinned 120 mph frame moved the frame's
 * radial gradient energy by 0.004 in total -- 0.763 to 0.759 -- for three
 * times the samples.  The trail length is NOT the lever on how blurred the
 * frame looks; the mix weight is, because the surface being smeared is
 * already a quarter-resolution image and its high frequencies are gone before
 * the first tap.  B3_AFX_TAPS=<n> overrides it for anyone re-measuring. */
#define B3_AFX_TAPS           16

/* MSAA ON THE SCENE TARGET.  4x, everywhere, by default.
 *
 * WHY THIS CONSTANT EXISTS AT ALL.  It used to be read off the GL context --
 * `SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES)` -- and that was a defect,
 * not a shortcut.  SDL2 answers that attribute with `glGetIntegerv(GL_SAMPLES)`
 * on the CURRENTLY BOUND DRAW FRAMEBUFFER, and the chain asks the question from
 * inside afx_resize(), one call after it has bound its own single-sampled scene
 * FBO.  So the answer was always 0, on every platform, and the desktop shipped
 * `MSAA: requested 4x, got 4x` next to `[afx] chain ready ... msaa 0`: the
 * window really did have a 4x drawable, and the only thing drawn into it was
 * the chain's final full-screen gamma quad, which has no interior edges.
 *
 * The scene target's sample count is a PROPERTY OF THE CHAIN, so it is decided
 * here and not asked of a driver.  The resolution order is
 *
 *     B3_AFX_MSAA=<n>   this chain only, wins outright
 *     B3_MSAA=<n>       the platform-wide knob burnout3_full.c and
 *                       web/b3_web.c already read -- one env, one meaning
 *     B3_AFX_MSAA_DEF   otherwise
 *
 * so `B3_MSAA=0` switches multisampling off from the default framebuffer to
 * the scene FBO in one move (which is what the headless suites, the Android
 * port and tools/web_smoke.py all rely on), and nothing has to know that the
 * scene is drawn into an FBO to turn it off. */
#define B3_AFX_MSAA_DEF       4

#endif
