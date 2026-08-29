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

/* ==========================================================================
 * THE PHOTOREALISM LAYER — seven effects, ALL OF THEM INSPIRED
 * ==========================================================================
 *
 * *** NOTHING IN THIS SECTION IS A CLAIM ABOUT BURNOUT 3. ***
 *
 * THE MANDATE, in one line: retail's look is the FLOOR, not the target.  The
 * default this game boots into is meant to go BEYOND the 2004 image, not to
 * approximate it more politely — so the numbers below are tuned for impact
 * and not for deniability.  The grade has a point of view (an arcade racer at
 * golden hour, and it is allowed to say so); the occlusion is meant to be
 * seen grounding the world; the shadows are a feature rather than a whisper.
 * The judgement that survives that is: no clipped highlights, no crushed
 * gameplay-relevant detail, and the HUD is sacred and is never touched by any
 * of it (it is drawn AFTER the layer, into the UI target — see the hook order
 * below).  The retail-faithful look stays exactly one env away, and the
 * recovered-law suites go on pinning it.
 *
 * Every one of the seven is a modern construction chosen because it makes the
 * port look like a photograph, and NONE of them exists in the retail image.
 * That is not a gap in the reverse engineering; it is the point.  The Xbox
 * hardware had no depth texture to read, no second geometry pass to spare and
 * no shader budget for any of this, and a sweep of the renderer range
 * 0x00028000..0x00045000 finds no ambient-occlusion, shadow-map, reflection
 * or light-shaft machinery of any kind.  An INSPIRED line must never be cited
 * as game behaviour — see the EVIDENCE MARKS block above.
 *
 * WHAT IS RETAIL, AND IS NOT TOUCHED.  The x2 present composite, the alpha law
 * C0.a = min(s,2)*0.5, the output gamma ramp round((i/255)^0.95*255) and the
 * world program's own linear fog all stay exactly where they were.  The
 * photorealism layer sits BETWEEN the finished scene and that recovered
 * ending: it changes what the composite is handed, never what the composite
 * is.  The one exception is spelled out under B3_PHOTO_TONEMAP below.
 *
 *
 * WHY IT IS ALL DEFERRED, and why the world shader is untouched
 * -------------------------------------------------------------
 * Six of the seven read the scene's DEPTH.  Once the depth is a texture, the
 * cheapest place to put a light — and the only place that treats the track,
 * the scenery, the props, the cars and the traffic IDENTICALLY — is a
 * screen-space pass over the finished frame.  So the layer adds exactly one
 * new geometry pass to the engine (the sun's shadow map, in
 * burnout3_render.c) and does everything else here, reading a depth texture
 * and writing a colour one.
 *
 * That is worth stating as a property rather than as an implementation note:
 * burnout3_render.c's ONE world program (B3R_VS / B3R_FS) is not modified by
 * this wave at all.  It compiles the same source it compiled before, so the
 * "B3_PHOTO=0 is bit-identical" gate below is not a hope about floating point
 * — the shader that draws the world is the same shader, byte for byte.
 *
 *
 * THE SWITCHES.  One master, seven individual, and the master's OFF position is
 * a gate rather than a preference:
 *
 *     B3_PHOTO=0        every effect off, every new pass skipped, every new
 *                       target unallocated, the depth attachment back to a
 *                       renderbuffer, and — the part that is checked — the
 *                       assembled shader source byte-identical to what this
 *                       file compiled before the wave.  The frame is then
 *                       BIT-IDENTICAL to the pre-wave build, which is what
 *                       lets the recovered-pixel suites pin it and keep
 *                       verifying recovered behaviour.
 *     B3_PHOTO=1        the default: the seven below, each at its own default.
 *
 *     B3_PHOTO_TONEMAP  1   filmic curve + authored grade   (tier 1)
 *     B3_PHOTO_SSAO     1   depth-derived ambient occlusion (tier 2)
 *     B3_PHOTO_ATMOS    1   aerial perspective + height fog (tier 3)
 *     B3_PHOTO_SHADOW   1   sun shadow map                  (tier 4)
 *     B3_PHOTO_SSR      1   screen-space reflections        (tier 5)
 *     B3_PHOTO_GODRAY   1   sun shafts                      (tier 6)
 *     B3_PHOTO_LIGHTS   1   per-source lights               (tier 7)
 *
 * An individual switch set while B3_PHOTO=0 does NOT turn its effect on: the
 * master is a hard gate, so the bit-identity leg cannot be defeated by a
 * stray env in a caller's environment.  Every magnitude below is overridable
 * by an env of the same name (see afx_photo_read_knobs) — the defaults are
 * gathered here so the whole look is one block of numbers.
 */

/* Is the photorealism layer live at all, and is effect <n> live?  GL-free, so
 * the validator probe build and the callers outside this module (the shadow
 * pass in burnout3_render.c) can both ask. */
int b3_photo_on(void);

enum {
    B3_PHOTO_FX_TONEMAP = 0,
    B3_PHOTO_FX_SSAO,
    B3_PHOTO_FX_ATMOS,
    B3_PHOTO_FX_SHADOW,
    B3_PHOTO_FX_SSR,
    B3_PHOTO_FX_GODRAY,
    B3_PHOTO_FX_LIGHTS,
    B3_PHOTO_FX_COUNT
};
int b3_photo_fx(int which);

/* ---- tier 1: FILMIC TONEMAP + GRADE ------------------------------------ */
/* INSPIRED.  The recovered ending is `out = clamp(2 * scene)` followed by the
 * [C] gamma ramp, and the x2 is a SHIFTLEFTBY1 in a register combiner — an
 * operation with no shoulder, so every value above 0.5 in the render target
 * lands on 255 and stays there.  On the pinned US_C3_V1 frame that is 1.8% of
 * the frame with the blur off and 3.1% with it on: sky, chrome, brake lights.
 *
 * The curve below is the ACES filmic approximation (Krzysztof Narkowicz's fit
 * of the RRT+ODT, the one every modern engine ships).  It is applied to the
 * SAME 2*scene the recovered path produced, so the exposure LAW is untouched —
 * a value that used to land at 0.5*2 = 1.0 now lands at 0.80 instead of
 * clipping, and mid-grey moves by under a level.  What changes is that the top
 * two stops exist at all.
 *
 * B3_PHOTO_TONEMAP=0 restores the raw clamp, so the recovered ending stays
 * RUNNABLE rather than merely written down (the same spirit as
 * B3_AFX_PRESENT_ADD=1). */
#define B3_PHOTO_TM_SHOULDER   1.00f  /* scales the curve's white point       */
/* THE LINEAR-SPACE EXPOSURE, and it is MEASURED rather than chosen.
 *
 * The curve is applied in LINEAR space (the scene target is display-referred,
 * so it is squared on the way in and square-rooted on the way out — see the
 * long note over AFX_COMPOSITE_TONEMAP, and the +37 levels of frame mean that
 * note exists because of).  This is the scale applied to the linearised value,
 * and 0.724 is the number that puts the pinned frame's mid-tone back exactly
 * where the recovered clamp had it: solve ACES(0.1296 * K) = 0.5184 for the
 * frame's own mid-grey and K comes out 2.896, which is 4 * 0.724 — the 4 being
 * the recovered x2 expressed in linear space.  The shipped value is 0.800
 * rather than that 0.724: the GRADE below also costs the frame mean about two
 * levels (an S-curve about 0.5 darkens a picture whose mean is under 0.5, and
 * this one's is), so the two are trimmed together to land the whole tier-1
 * leg within a couple of levels of the recovered frame rather than each half
 * being neutral on its own.  MEASURED on the pinned frame, both ways.
 *
 * So the exposure LAW is unchanged and the frame reads at the same brightness.
 * What it gains is the top: a render-target 0.5 used to land on 255 and so did
 * a 0.8, and they now land on 217 and 243. */
#define B3_PHOTO_TM_EXPOSURE   0.800f
/* The authored GRADE, as a 16x16x16 3-D lookup table this module GENERATES at
 * build-all time (afx_photo_lut_build) and uploads as one 256x16 texture.  It
 * is a derived asset of our own making — no download, nothing shipped, and it
 * is reproducible from these six numbers alone.
 *
 * IT HAS A POINT OF VIEW, and the point of view is golden hour on a Californian
 * strip at 150 mph: the shadows go cool and slightly blue, the highlights go
 * warm, the midtones gain contrast and saturation.  That is a look, chosen,
 * not a neutral transfer — and it is exactly what the "beyond retail" mandate
 * asks for.  It is still a GRADE and not a filter: the lift and gain are small
 * enough that white stays white and the HUD's yellow stays the HUD's yellow,
 * because a grade that recolours the interface is a bug however pretty it is.
 * (The HUD is drawn after this pass and never passes through it — but the
 * results screens and the menus do, so the restraint is not academic.) */
#define B3_PHOTO_GRADE_LIFT_R  (-0.016f) /* shadow tint, added at black      */
#define B3_PHOTO_GRADE_LIFT_G  (-0.004f)
#define B3_PHOTO_GRADE_LIFT_B  ( 0.030f)
#define B3_PHOTO_GRADE_GAIN_R  ( 1.045f) /* highlight tint, multiplied at 1  */
#define B3_PHOTO_GRADE_GAIN_G  ( 1.008f)
#define B3_PHOTO_GRADE_GAIN_B  ( 0.958f)
#define B3_PHOTO_GRADE_SAT       1.140f  /* about the luma the composite uses */
#define B3_PHOTO_GRADE_CONTRAST  1.110f  /* S-curve strength about 0.5        */
#define B3_PHOTO_LUT_N         16        /* the cube's edge; 256x16 as a 2-D  */

/* ---- tier 2: SSAO ------------------------------------------------------- */
/* INSPIRED.  Half-resolution, twelve samples over a hemisphere oriented by a
 * normal reconstructed from the depth buffer, then a depth-aware (bilateral)
 * separable blur and a bilateral upsample in the deferred pass.
 *
 * MEANT TO BE SEEN.  The brief for this effect is GROUNDING — the contact
 * darkening under a car, a kerb, a lamp post base, the eave of a shop front —
 * and under the "beyond retail" mandate that grounding is supposed to be
 * legible at a glance rather than deniable in a difference image.  What it is
 * still NOT is the dirt-in-every-corner look that makes a scene read as a 2009
 * tech demo: the radius stays a couple of metres so the occlusion stays a
 * CONTACT cue and does not creep up walls, and the whole term is scaled by how
 * lit the pixel already is (B3_PHOTO_AO_LITBIAS), because the art already
 * carries the artists' own baked occlusion in its vertex colours and two
 * occlusions multiplied are a hole. */
#define B3_PHOTO_AO_RADIUS     4.00f  /* metres, world space                  */
#define B3_PHOTO_AO_STRENGTH   0.85f  /* 0 = none, 1 = full occlusion is black */
#define B3_PHOTO_AO_BIAS       0.045f /* metres; kills self-occlusion acne     */
#define B3_PHOTO_AO_POWER      1.10f  /* contrast, applied AFTER the gain      */
/* THE GAIN.  The kernel's mean occlusion for a pixel a human would call "in a
 * corner" is about 0.15, so the raw average is not a usable signal — see the
 * long note in AFX_FS_AO.  This maps what the geometry produces onto what the
 * eye wants, and it is the knob to turn when the occlusion is too shy or too
 * heavy; the strength above then scales the finished term. */
#define B3_PHOTO_AO_GAIN       3.60f
/* The ceiling on the tap spiral's SCREEN radius, in uv.  See the note at its
 * use site: without it a pixel two metres from the camera spirals its taps
 * over most of the frame, which is meaningless and slow at the same time. */
#define B3_PHOTO_AO_SRCAP      0.14f
/* THE ANGLE BIAS, and it is what stops this pass shading an open road.
 *
 * The per-sample occlusion is the SINE of the angle a sample sits above the
 * tangent plane, and `max(0, ...)` on a signal whose true value is zero and
 * whose noise is a couple of degrees of reconstructed-normal error does not
 * return zero -- it returns the positive half.  Times B3_PHOTO_AO_GAIN that DC
 * is a uniform multiply over every flat surface in the near field, which is a
 * player-visible blanket, not ambient occlusion.
 *
 * MEASURED on US_C1_V1 frame 655 at 2048x1536, the size the game boots into:
 * with no angle bias, open flat road -42.62 levels and a real wheel-to-road
 * contact -42.82 -- the same number, i.e. no discrimination at all.  0.22 is
 * about 13 degrees, comfortably above the reconstruction's noise and far below
 * the 45-90 degrees a genuine corner presents; what survives is rescaled back
 * to 0..1 so the gain keeps its meaning. */
#define B3_PHOTO_AO_ANGBIAS    0.22f
#define B3_PHOTO_AO_MAXDIST    180.0f /* metres; AO fades out past this        */
#define B3_PHOTO_AO_TAPS       12     /* samples per pixel, at half res        */
#define B3_PHOTO_AO_LITBIAS    0.45f  /* the floor of the "already dark" scale */

/* ---- tier 3: DEPTH ATMOSPHERICS ---------------------------------------- */
/* INSPIRED, and deliberately ADDITIVE TO the recovered fog rather than a
 * replacement for it.  burnout3_render.c's world program computes retail's own
 * `min(|z_eye|, fog_far)` ramp against enviro.dat's fog colour and distances,
 * and that is [C] — it is not this wave's to overwrite.  What retail has no
 * concept of is AERIAL PERSPECTIVE: the wavelength-dependent scatter that
 * makes a mountain eight kilometres away read blue-grey and slightly LIGHTER
 * than the shopfront in front of it, and the warm inscatter that gathers
 * around the sun's side of the sky.
 *
 * So this pass adds, per pixel, a scatter term whose colour is the TRACK'S OWN
 * SUN COLOUR (enviro.dat +0x60, [C] as a field, read through the same sidecar
 * carfx reads) leaned toward the sun by the view direction, over an
 * exponential in distance and a second exponential in HEIGHT.  It is applied
 * in the deferred pass, so the track, the scenery, the props, the cars and the
 * traffic all receive exactly the same treatment from exactly the same code —
 * which is the property the legacy per-pass fog could not have, since the cars
 * were never in it.
 *
 * The sky is excluded by depth: at the far plane the scatter would be the only
 * thing left and the dome would turn into a flat plate. */
#define B3_PHOTO_ATM_DENSITY   0.00075f /* per metre; 1/e at about 1.3 km     */
#define B3_PHOTO_ATM_HEIGHT    240.0f   /* metres; the scale height           */
#define B3_PHOTO_ATM_STRENGTH  0.74f    /* ceiling on the scatter fraction    */
#define B3_PHOTO_ATM_SUNGAIN   0.95f    /* extra scatter looking INTO the sun */
#define B3_PHOTO_ATM_SUNPOW    5.0f     /* how tight that forward lobe is     */
#define B3_PHOTO_ATM_NEAR      30.0f    /* metres of nothing, so the car and
                                         * the road under it stay untinted    */
/* The scatter's own colour, as a lean from the sun's HUE toward the sky's own
 * colour in the view direction.  0 is "the sun's hue exactly", 1 is "the sky
 * exactly"; the useful range is the middle, because aerial perspective on a
 * clear day is neither — it is the sun's light scattered by air, which is why
 * a distant ridge goes blue-grey and a distant ridge WITH the sun behind it
 * goes gold. */
#define B3_PHOTO_ATM_SKYLEAN   0.55f
/* THE SKY THE SCATTER FADES TO IS READ FROM THE DOME, not from here: see
 * b3_sky_horizon_band() in burnout3_postfx.h and the note over
 * AFX_LIGHT_ATMOS.  These three are the FALLBACK the layer keeps when the
 * track ships no gradient sheet (four do not), and what
 * B3_PHOTO_ATM_SKYSRC=0 pins for an A/B.
 *
 * They are a bright daylight blue, and they used to be the target on EVERY
 * track — which is what made US_M1's distant city read as flat light-grey
 * cut-outs against a near-black storm: a far band at 152 levels against a sky
 * at 72.  A fade target that is not the sky it silhouettes against does not
 * look like distance, it looks like a hole. */
#define B3_PHOTO_ATM_SKY_R     0.50f
#define B3_PHOTO_ATM_SKY_G     0.64f
#define B3_PHOTO_ATM_SKY_B     0.86f

/* ---- tier 4: SUN SHADOW MAPS ------------------------------------------- */
/* INSPIRED.  One depth-only pass over the retained track / scenery / props /
 * car buffers from the track's own sun DIRECTION (enviro.dat +0x80, [C] as a
 * field), rendered orthographically into a square depth texture, and sampled
 * in the deferred pass with a 3x3 PCF kernel.
 *
 * ONE CASCADE, FOLLOWING THE CAMERA, and that is a measured decision rather
 * than a shortcut — the report carries the numbers.  Two cascades cost a
 * second full geometry pass, and the geometry pass is the whole bill here; the
 * fill is nothing.  A single 2048 map fitted to a 260 m box around the camera
 * puts about 8 texels on a metre, which is enough for a lamp post's shadow to
 * read as a lamp post at the distance a racing camera ever looks.
 *
 * STABLE, i.e. NO SHIMMER.  The box's centre is SNAPPED to a whole shadow
 * texel in light space every frame (afx_photo_shadow_snap), so driving forward
 * slides the map by whole texels and a shadow edge stops crawling along a
 * kerb.  Without the snap the edge boils at every speed and it is the first
 * thing a viewer notices.
 *
 * SCALE, DO NOT DOUBLE-DARKEN.  The track's vertex colours already carry the
 * artists' baked lighting, including their own painted shade under the eaves.
 * A shadow term that simply multiplies turns those into holes.  The deferred
 * pass therefore darkens by a fraction that FALLS as the pixel gets darker —
 * see B3_PHOTO_SH_LITBIAS — so a shadow crossing an already-shaded wall
 * changes it very little and a shadow crossing bright tarmac changes it a lot,
 * which is also what the physics says should happen. */
#define B3_PHOTO_SH_SIZE       2048   /* the map's edge, in texels            */
#define B3_PHOTO_SH_EXTENT     260.0f /* metres; the fitted box's half-width  */
#define B3_PHOTO_SH_AHEAD      90.0f  /* metres the box leads the camera      */
#define B3_PHOTO_SH_DEPTH      900.0f /* metres along the light, front to back */
/* HOW DARK A SHADOW GETS, and it is a FLOOR as much as a strength: a fully
 * shadowed lit pixel keeps (1 - this) of its brightness.  0.50 means open
 * shade at half the sunlit value, which is roughly what a photograph of a
 * Californian street at golden hour actually shows.
 *
 * IT USED TO BE 0.72 AND A PLAYER CALLED IT "too dark", and they were right:
 * losing the sun on a real surface does not leave it black, it leaves the
 * SKY's light on it, and the sky is not nothing.  The shadowed pixel is also
 * pulled toward the cool tint below rather than merely scaled, which is the
 * other half of making shade read as shade instead of as a hole. */
#define B3_PHOTO_SH_STRENGTH   0.50f
/* The COLOUR a shadow goes, rather than just "darker".  Sunlight removed from
 * a surface leaves the sky's light on it, and the sky is blue — so a shadow on
 * grey tarmac at golden hour is not grey, it is cool.  The shadowed pixel is
 * pulled this far toward its own luminance times this tint.  INSPIRED, and it
 * is most of what makes the shadows read as photographic rather than as a
 * multiply. */
#define B3_PHOTO_SH_COOL       0.42f
#define B3_PHOTO_SH_COOL_R     0.72f
#define B3_PHOTO_SH_COOL_G     0.84f
#define B3_PHOTO_SH_COOL_B     1.12f
#define B3_PHOTO_SH_BIAS       0.0016f/* constant depth bias, light-clip units */
#define B3_PHOTO_SH_SLOPE      0.0055f/* slope-scaled part of the same         */
#define B3_PHOTO_SH_FADE       0.82f  /* fraction of the box where fade starts */
#define B3_PHOTO_SH_LITBIAS    0.45f  /* see "scale, do not double-darken"     */
#define B3_PHOTO_SH_PCF        1      /* kernel half-width: 1 -> 3x3 taps      */
/* THE NORMAL OFFSET, in shadow TEXELS (the pass multiplies by the map's own
 * world texel size, so this number does not have to be re-tuned when the map
 * size or the box extent changes).  It is what removes the acne a depth bias
 * alone can only trade against peter-panning -- see the long note at its use
 * site in AFX_LIGHT_SHADOW. */
#define B3_PHOTO_SH_NORMOFF    2.2f

/* ---- tier 4b: THE DIRECTIONAL TERM ------------------------------------ */
/* INSPIRED.  A shadow map answers "is the sun blocked" and says nothing at all
 * about "is this surface FACING the sun" -- so shadows alone leave a wall in
 * full sun and a wall turned eighty degrees away at the same brightness, and
 * the geometry reads flat and uniformly lifted.  A player said exactly that
 * ("light should be more directional") and this block is the answer.
 *
 * It is ENERGY-PRESERVING, not a second light.  The art already carries the
 * artists' baked lighting; adding a lambert on top would light everything
 * twice.  The gain is exactly 1.0 at B3_PHOTO_SUN_REF -- the N.L a typical
 * surface in these scenes already sits at -- and leans up or down from there,
 * so a sun-facing wall gains about as much as a turned-away wall loses.
 *
 * The N.L comes from a normal reconstructed from the depth buffer, and that
 * normal is only trustworthy near the camera: see B3_PHOTO_SUN_NCONF. */
#define B3_PHOTO_SUN_DIRECT    0.55f  /* how far the relight leans            */
/* THE N.L AT WHICH THE GAIN IS EXACTLY 1.0, and it is MEASURED rather than
 * chosen: it has to be the average N.L of the surfaces actually on screen, or
 * the term stops being a redistribution and becomes an exposure change.  On
 * these street scenes -- road at N.L 0.50, shopfronts spread across 0 to 0.85
 * -- that average is near 0.38.  The first cut used 0.55 and cost the pinned
 * frame TEN LEVELS of mean brightness, which is a dimmer switch wearing a
 * directional-light costume. */
#define B3_PHOTO_SUN_REF       0.38f
#define B3_PHOTO_SUN_LO        0.75f  /* clamps, so a bad normal cannot make  */
#define B3_PHOTO_SUN_HI        1.45f  /* a hole or a blowout                  */
/* HOW FAR A DEPTH-DERIVED NORMAL IS WORTH TRUSTING, in metres, and the width
 * of the fade to not trusting it.  Past a couple of hundred metres down a road
 * at a grazing angle the depth difference between adjacent pixels collapses
 * and the reconstructed normal is mostly noise -- which SHIMMERS frame to
 * frame as the camera moves.  That shimmer is invisible on a moving road and
 * glaring next to a HUD element that is not moving, which is why it gets
 * reported as "flashing when text is on screen" rather than as noise.  Both
 * N.L-driven terms fade out over this; the shadow MAP does not, because a
 * depth comparison needs no normal. */
#define B3_PHOTO_SUN_NCONF     220.0f
#define B3_PHOTO_SUN_NBAND     110.0f

/* ---- tier 5: SSR ON THE SHINE SPANS ------------------------------------ */
/* INSPIRED.  A screen-space depth march, run ONLY where the track's own
 * material flags already declare a shiny surface — the class-1/7/10 additive
 * specular groups burnout3_render.c gathers into its shine spans.  Those spans
 * are the game's own statement about which surfaces reflect, so the port does
 * not have to invent a gloss channel: it re-draws the shine geometry once,
 * flat, into a quarter-resolution MASK (one draw call, because the spans are
 * contiguous in one VBO) and marches only under that mask.
 *
 * DRAMATIC WHERE THE TAGS SAY SO.  The shine tags are sparse — they are the
 * game's wet-tarmac, glass and polished-panel groups — so this is one of the
 * few places where a strong effect costs nothing anywhere else on the screen.
 * Where the mask is hot the reflection is allowed to be a real reflection.
 * The default and the measured reason for it are in the wave report; the two
 * knobs that matter are the strength and the edge fade, and a miss always
 * falls back to the existing env-map look rather than to black. */
#define B3_PHOTO_SSR_STEPS     24     /* march steps, half res                */
#define B3_PHOTO_SSR_STRIDE    0.9f   /* metres per step at the near plane    */
#define B3_PHOTO_SSR_THICK     1.6f   /* metres; a hit thicker than this is a
                                       * miss, not a hit behind a wall        */
#define B3_PHOTO_SSR_STRENGTH  0.85f  /* ceiling on the reflected fraction    */
#define B3_PHOTO_SSR_EDGE      0.14f  /* screen fraction the fade occupies    */
#define B3_PHOTO_SSR_FRESNEL   2.5f   /* grazing-angle exponent               */

/* ---- tier 6: GOD RAYS --------------------------------------------------- */
/* INSPIRED.  The ordinary screen-space radial scatter: build a quarter-res
 * buffer holding only what the SKY contributes (depth at the far plane), then
 * smear it toward the sun's projected screen position with a decaying tap
 * train, and ADD it.  Gated three ways — by the sun being on or near the
 * screen at all, by how far off-axis the camera is looking, and by depth at
 * the SOURCE, so the shafts are made of sky and only of sky.
 *
 * Not subtle, under the mandate: when the camera swings into the sun coming
 * out of a corner this is supposed to bloom the whole frame open for a beat.
 * What keeps that from being a white-out is that the term is ADDED before the
 * filmic curve, so the shoulder catches it — turning the shafts up makes the
 * frame glow rather than clip, which is the entire argument for doing tier 1
 * first. */
#define B3_PHOTO_GR_TAPS       28
#define B3_PHOTO_GR_DENSITY    0.80f  /* how far along the ray the taps reach */
#define B3_PHOTO_GR_DECAY      0.962f /* per-tap falloff                      */
#define B3_PHOTO_GR_WEIGHT     0.48f  /* per-tap gain                         */
#define B3_PHOTO_GR_STRENGTH   0.85f  /* the whole term's ceiling             */
#define B3_PHOTO_GR_MARGIN     0.60f  /* how far off-screen the sun may be    */
#define B3_PHOTO_GR_RADIUS     0.85f  /* the source's falloff about the sun   */

/* ---- tier 7: PER-SOURCE LIGHTS ------------------------------------------ */
/* INSPIRED, and the derivation is the interesting half.
 *
 * WHAT RETAIL HAS, AND DOES NOT.  There is NO light table in this game.  A
 * sweep of the recovered formats finds no record type for a light, a corona, a
 * flare or a lamp anywhere a track could carry one: docs/RE_BGD.md's chunk
 * list is events, sections, traffic and the road network and nothing else;
 * the props and scenery tables (tools/cextract/cx_props.c,
 * tools/cextract/cx_scenery.c) are a 0x70 model record and a 4x4 placement
 * with a class field that says CONE or BARRIER, never LAMP; the light probes
 * (tools/cextract/cx_light_probes.c) are a 9-coefficient SH bake of the light
 * a car RECEIVES at a point on the collision mesh, not a light anything
 * emits; and the only corona machinery in the binary is per-CAR — the .bgv
 * model's own light table at model+0x1664/+0x16AC, 0x30-byte records in MODEL
 * space (docs/RE_CARFX.md 534-549, [C]) — plus one global sun/sky glow.
 *
 * So there is nothing to read, and hand-authoring a light list per track is
 * exactly the baked per-track data this port does not do.  What there IS, is
 * the ART: every lamp post, traffic-light head, neon sign and lit shopfront in
 * the scenery table carries its own bulb, painted, in the model's own texture.
 *
 * THE DERIVATION, therefore, is per MODEL and happens once at load:
 *   1. scan the model's texture for EMISSIVE texels -- bright, opaque, and
 *      sitting well above the texture's own mean, which is what separates a
 *      bulb on a dark pole from a white van;
 *   2. find the model's VERTICES whose uv lands on those texels, and take
 *      their centroid: that is where the bulb is, in model space;
 *   3. take the mean colour of those same texels: sodium orange, fluorescent
 *      white and neon come out of the art for free, per model, with no
 *      colour written down anywhere in this port;
 *   4. instance it through every placement of that model.
 * It is retail's own shape -- a light table that lives in the model and is
 * instanced -- arrived at from the art rather than from a table, because the
 * table is the one thing the disc does not have.
 *
 * The rules below are the thresholds step 1 uses; burnout3_scenery.c asks for
 * them through b3_photo_light_rules() so that every number in the look still
 * lives in this one header. */
/* THE BUDGET HOLDS BOTH KINDS.  Twelve was a street's worth of lamps and
 * nothing else; six racers' headlights then filled it exactly and the
 * streetlights vanished.  Eighteen leaves half for the beams (the cap in the
 * pick) and half for the scenery, and the cost curve says the difference is
 * 0.02 ms at 1080p -- see docs/PHOTOREALISM.md section 4. */
#define B3_PHOTO_LIGHT_N        18    /* nearest-N accumulated per frame      */
/* MEASURED, on US_P1_V1 -- the dusk end of the roster -- at frame 520 under
 * the elevated section, against the same frame with the tier off.  The first
 * cut shipped 1.30 and it was a white-out: +62 levels of frame mean, the
 * underpass filled in and the sunset behind it gone.  0.30 lifts the pillars,
 * the pavement and the roof the lamps are actually under by +4.6 levels and
 * leaves the sky, the distance and the clipping (0.10%) exactly where they
 * were.  On a daylight track the day multiplier takes the same number down to
 * an effective 0.11, which is the restraint half of the mandate. */
#define B3_PHOTO_LIGHT_GAIN     0.30f /* the whole term's ceiling             */
#define B3_PHOTO_LIGHT_DAY      0.22f /* ...times this on a high, white sun   */
/* THE DUSK FACTOR, and it is derived rather than authored.  There is no
 * night flag in enviro.dat and, measured across all 36 circuits, no night
 * TRACK either: every sun in the game sits 15-45 degrees above the horizon.
 * What the roster does have is a dusk END -- US_P1 (sun 1.00/0.65/0.55, 25
 * degrees) and EU_M2 (15 degrees, the lowest sun in the game) against
 * AS_C3/EU_C1 (a pure white sun at 30).  Both numbers are enviro.dat's own
 * (+0x60 colour, +0x80 vector, [C] as fields), so the lamps read the track's
 * own hour instead of a list of track names.  A value >= 0 overrides it. */
#define B3_PHOTO_LIGHT_DUSK    (-1.0f)
#define B3_PHOTO_LIGHT_ELEV     35.0f /* degrees; the sun is "low" below this */
#define B3_PHOTO_LIGHT_ELEV_W   25.0f /* ...over this many degrees of ramp    */
#define B3_PHOTO_LIGHT_WARM     0.95f /* sun luma that is "full day"          */
#define B3_PHOTO_LIGHT_WARM_W   0.25f /* ...over this much luma of ramp       */
#define B3_PHOTO_LIGHT_FAR      170.0f/* metres; a light past this is cut     */
#define B3_PHOTO_LIGHT_AHEAD    25.0f /* metres the pick point leads the eye  */
#define B3_PHOTO_LIGHT_LITBIAS  0.55f /* "scale, do not double-brighten"      */
/* the reach: a lamp's radius is its MODEL's own size times this, clamped */
/* A LAMP LIGHTS A POOL, NOT A STREET.  3.4 model radii put a street lamp's
 * reach at the 28 m ceiling and twelve of those overlap into a flat ambient
 * lift with no pools in it at all -- the tell was that the picture got
 * brighter without getting more lit.  2.0 with a 15 m ceiling is a pool the
 * size a street lamp actually throws. */
#define B3_PHOTO_LIGHT_REACH    2.00f
#define B3_PHOTO_LIGHT_RMIN     7.0f
#define B3_PHOTO_LIGHT_RMAX     15.0f
/* what counts as a bulb, in the model's texture */
#define B3_PHOTO_LIGHT_EMIS     0.78f /* texel luminance, 0..1                */
#define B3_PHOTO_LIGHT_EMIS_DK  0.58f /* the texture MEAN must be below this  */
#define B3_PHOTO_LIGHT_EMIS_DL  0.26f /* ...and the texel this far above it   */
/* A BULB HAS TO BE A FEATURE OF THE ART.  The floor was 0.12% and it let a
 * palm tree in on AS_M1_V1 -- three vertices' worth of sky-lit frond edge at
 * 0.55% of its sheet.  One per cent is the first number that is a painted
 * thing rather than a highlight, and it keeps every real source measured on
 * both tracks (a street lamp is 4.1%, a traffic-light head 13.3%, the
 * market's electrical poles 15.5%, the street clutter 1.7%). */
#define B3_PHOTO_LIGHT_EMIS_MIN 0.0100f  /* fraction of the texture, floor    */
#define B3_PHOTO_LIGHT_EMIS_MAX 0.2200f  /* ...and ceiling: a bright WALL is
                                          * not a bulb                       */
/* HOW FAR THE EMISSIVE TEXELS SPREAD, in the model's own radii.  A light
 * source is a SMALL bright part of a bigger object -- a lamp head is a third
 * of a metre on a ten-metre post -- and this is what rejects a bright
 * SURFACE.  The threshold sits in the gap the two tracks it was measured on
 * actually leave: the loosest thing accepted is a lit phone box at 0.29 and
 * the tightest thing rejected is a sign arrow at 0.34, with a traffic-light
 * head at 0.04 and a street lamp at 0.15 far below both.  Fruit crates
 * (0.47) and a plastic stool (0.63) are not close. */
#define B3_PHOTO_LIGHT_TIGHT    0.32f

/* ---- tier 7b: THE CARS' OWN HEADLIGHTS ----------------------------------
 * The racers light the road ahead of them, from the car's OWN lamp table --
 * the .bgv records at model+0x1664 / +0x16AC, which carry a position AND a
 * NORMAL per lamp in model space (docs/RE_CARFX.md 534-549, [C]).  A position
 * and a direction is a spotlight, so nothing here had to be invented except
 * the cone and the reach: the beam's origin, its aim and the fact that the
 * headlamps are ON are all retail's (the corona pass has always drawn type 0
 * with B3_CARFX_LIGHT_HEAD set, on every car, in daylight).
 *
 * EVERY RACER'S BEAM IS RESERVED, never sorted away, and the reservation now
 * sits BESIDE the streetlights' budget rather than inside it: the array the
 * shader declares is B3_PHOTO_LIGHT_N lamps PLUS one slot per racer, so a
 * beam cannot lose a slot to a lamp and a lamp cannot lose one to a beam.
 * The old spelling carved the beams out of N and capped them at half of it,
 * which was correct at the shipped N=18 (measured: 6 beams admitted on 6 of 6
 * racers in every one of 7745 frames across both test tracks) and silently
 * wrong for anyone who lowered N -- at N=8 two racers would have gone dark.
 * A guarantee that holds only at the default is not a guarantee. */
#define B3_PHOTO_HEAD_ON        1     /* 0 retires tier 7b alone             */
/* THE REACH IS THE VISIBLE WINDOW'S, not the lamp's.  34 m is about right for
 * a real low beam and was wrong here for a reason that is pure camera: the
 * chase view sits some 8 m behind the car and 2.5 m up, so the car's own body
 * hides the road from its nose out to roughly 10 m.  The near half of a 34 m
 * pool -- the bright half, the half the windowed falloff spends most of its
 * energy on -- was therefore behind the very car that cast it, and what the
 * driver could actually see was the crushed tail: att at 25 m is 0.21 of a
 * 34 m beam and 0.63 of a 60 m one.  A longer window is not a brighter beam,
 * it is the same beam with its falloff spread across the part of the road the
 * player is looking at. */
#define B3_PHOTO_HEAD_RANGE     60.0f /* metres the beam reaches             */
/* THE BEAM IS AN ELLIPSE, and the two angles are not interchangeable: a
 * headlight is WIDE across the road and SHALLOW in elevation, and it is the
 * shallow half that gives the pool the far cutoff line that reads, from a
 * chase camera, as "that car has its lights on".  A round cone does not read
 * at all from behind the car casting it -- you are standing inside it -- which
 * is what the first two cuts of this fix ran into.  Degrees, half-angles off
 * the beam axis.  See the shader's own note for the worked geometry. */
#define B3_PHOTO_HEAD_SPREAD    30.0f /* degrees ACROSS the road             */
#define B3_PHOTO_HEAD_CUTOFF     3.2f /* degrees in ELEVATION                */
#define B3_PHOTO_HEAD_HOT        0.35f/* ellipse radius^2 held at full power */
/* THE BEAMS ARE ALWAYS ON, and B3_PHOTO_HEAD_DAY is the floor that says so.
 *
 * This used to ride `dusk` SQUARED, on the argument that a headlight at noon
 * is doing nothing at all.  That is true of a headlight pointed at sunlit
 * tarmac and false of every other surface a racing car drives past.  The
 * roster's dusk factor only spans 0.14 (US_C1_V1, a bright afternoon) to 0.61
 * (US_P1's sunset), so squaring left US_C1 at 0.02 -- two per cent, i.e. off
 * -- on a track that spends a third of its lap under a stadium deck.  A car
 * whose lamps are visibly lit and which throws nothing into an underpass is
 * the defect, not the fix.
 *
 * So the mix is `day + (1 - day) * dusk`, always at least `day`.  The floor is
 * not a brightness in itself: the deferred pass' b3lit ramp
 * (B3_PHOTO_LIGHT_LITBIAS) hands a light most of its authority on a dark pixel
 * and little on a bright one, so one number reads as a pool in shade and stays
 * quiet in the sun. */
#define B3_PHOTO_HEAD_DAY       0.30f /* the floor: the beams never go out   */

/* ---- THE THREE NUMBERS THAT MAKE THE POOL READ --------------------------
 *
 * The floor above says the beams are ON in daylight.  It did not make them
 * VISIBLE in daylight, and a third round of the same user report is what
 * established the difference.  The measurement, on a 10-second drive rendered
 * twice at the user's own 2048x1536 (B3_PHOTO_HEAD_ON 1 against 0, differenced
 * over the road ahead of the chase car, 99th percentile of the box):
 *
 *     US_C1_V1 day    open road   0.06 / 1.67 / 2.33 levels
 *     US_P1_V1 dusk   open road   2.00 / 3.33 / 5.00 levels
 *     ...and the frames that DID pool: 84 / 88 / 154 levels -- every one of
 *        them a tunnel wall or a barrier, i.e. a VERTICAL surface.
 *
 * Under one level of 8-bit quantisation is not a subtle look, it is off, and
 * the cause is N dot L.  The recovered type-0 lamp sits a MEASURED 0.615 m
 * above the road (b3_ground_probe under the beam origin), so the road at 10 m
 * takes 0.0613 of the beam and at 30 m takes 0.0205, while the tunnel wall in
 * the same beam takes 1.0.  The pass was delivering two to six per cent of a
 * headlight to the only surface the feature exists to light.
 *
 * B3_PHOTO_HEAD_WRAP is the fix and it is Valve's half-Lambert, applied to
 * the beams alone: mix(N.L, 1, wrap).  The physical excuse is the honest one
 * -- a sealed-beam lamp is a 15 cm emitter behind a fluted lens, not a point,
 * and asphalt scatters what grazes it -- and the arithmetic is that at 0.85
 * the road at 10 m goes 0.061 -> 0.859, the fourteen-fold the measurement says
 * is missing.  A wall is mix(1, 1, w) = 1, unchanged, so the tunnel case that
 * already worked is left exactly where it was.
 *
 * B3_PHOTO_HEAD_LIT is the beams' own b3lit bias -- their own knob, separate
 * from the lamps' B3_PHOTO_LIGHT_LITBIAS, and MEASURED to the same value,
 * which is a result rather than a coincidence.  It was set near 1 first, on
 * the argument that a headlight the player has asked three times to be able
 * to see should not be damped on sunlit tarmac the way a streetlight at noon
 * deserves to be.  Sweeping it says that argument was answered by WRAP and
 * not by this: at 0.90 the road pool on US_C1_V1 measures 118 levels and at
 * 0.55 it measures 111 -- seven levels of a hundred and ten -- while the
 * B3_PHOTO_LIGHT_DUSK=1 stress leg goes from 4.01% of the frame CLIPPED to
 * 0.06%.  The beams never needed an exemption from the bright-pixel ramp;
 * they needed the grazing-incidence fix, and once they had it the ramp is
 * doing what it was written to do.
 *
 * B3_PHOTO_HEAD_GAIN is now ABSOLUTE.  It used to be "relative to a scenery
 * lamp" and was multiplied by B3_PHOTO_LIGHT_GAIN downstream, which is what
 * forced the caller to smuggle a ratio through the per-light COLOUR to cancel
 * a day curve the beams did not want.  0.48 is the old 1.60 x 0.30 to the
 * digit, so this rename moved no pixels by itself; everything the wave gained
 * is in WRAP and LIT.  MEASURED after, same drive, same box, same percentile:
 * see docs/PHOTOREALISM.md. */
/* ...AND THEN THE PLAYTEST SAID "TOO BRIGHT", which is the fourth round of
 * this report and the first one in the other direction.  The interesting part
 * is WHICH knob answers it, because two of them would move the number and only
 * one of them moves the right pixels.
 *
 * GAIN scales every surface the beam touches by the same factor, so halving it
 * halves the tunnel wall along with the blown road -- and the tunnel is the
 * case this whole tier was built for.  WRAP is `mix(N.L, 1, w)` and it is, by
 * construction, a GRAZING-INCIDENCE term: the road at 10 m has N.L = 0.061 and
 * is almost entirely made of wrap, while a wall has N.L = 1 and is mix(1,1,w)
 * = 1 whatever w is.  So wrap is the only knob that can dim the road pool and
 * leave the wall alone, and the measurement says so -- validate_photo's own
 * section-12 drive, 2048x1536, six moments, road-band delta at the 99th
 * percentile, WRAP swept with GAIN held at 0.70:
 *
 *     wrap    US_C1_V1 (day, open road)     US_P1_V1 (dusk, tunnels)
 *             worst        best             worst        best
 *     0.85    111.9        137.4             86.1        229.0   <- was
 *     0.60     84.4        115.4             59.7        206.8
 *     0.45     64.3         97.8             43.8        185.0
 *     0.40     57.4         91.0             38.7        176.1   <- is
 *     0.35     50.8         83.7             33.6        164.7
 *
 * 0.40 is HALF the worst-frame road delta to the level (111.9 -> 57.4) and the
 * tunnel keeps 77% of its punch (229.0 -> 176.1).  Read the two columns
 * against each other and that is the wrap term doing exactly what the physics
 * above says it does: the open road loses 49% and the frames that pool lose
 * 23%.  Halving GAIN instead would have taken both down by half.
 *
 * The floor this must clear is still BEAM_FLOOR = 8 levels, and 38.7 is five
 * times it.  Anyone who liked the old look has the HEADLIGHTS row in the pause
 * menu (HIGH is 0.85 to the digit) and $B3_PHOTO_HEAD_WRAP underneath it. */
#define B3_PHOTO_HEAD_GAIN      0.70f /* absolute, NOT a multiple of the
                                       * lamps' B3_PHOTO_LIGHT_GAIN         */
#define B3_PHOTO_HEAD_WRAP      0.40f /* half-Lambert on the beams only      */
/* The pause menu's HEADLIGHTS row, in wrap terms.  LOW is the shipped default
 * and HIGH is the pre-playtest look, kept because the report that produced it
 * was a real one and taste is not a regression. */
#define B3_PHOTO_HEAD_WRAP_LOW  0.25f
#define B3_PHOTO_HEAD_WRAP_MED  0.40f
#define B3_PHOTO_HEAD_WRAP_HIGH 0.85f
#define B3_PHOTO_HEAD_LIT       0.55f /* the beams' own b3lit bias           */
/* THE AIM AND THE CUTOFF PLACE THE POOL, and they are chosen together against
 * the CAMERA, not against a parked car.  With the lamp a measured 0.615 m up,
 * the road at distance d sits atan(0.615/d) below the lamp: 3.5 degrees at
 * 10 m, 1.8 at 20, 1.2 at 30.  Those angles are all SMALL and they converge,
 * so a beam aimed steeply down (this shipped at 0.16, i.e. 9.1 degrees) puts
 * its hot core at 4 m -- under the car, behind the bodywork, where the chase
 * camera cannot see it -- and shows the driver only the outskirts.
 *
 * Aimed at 0.062 (3.55 degrees) with a 3.2-degree cutoff the same beam holds
 * FULL power from 6.5 m to 21 m and fades out through the range window: the
 * pool now sits exactly where the car stops hiding the road.  SWEPT rather
 * than reasoned into place -- six aim/cutoff pairs rendered as difference maps
 * on the same pinned frame -- and the ones aimed lower measured a third of the
 * road coverage of this one.
 *
 * Raising the aim pulls both ends in.  A cutoff at or above the aim removes
 * the far end entirely and the beam goes back to washing the horizon. */
#define B3_PHOTO_HEAD_AIM      0.062f /* how far the beam is aimed DOWN, so
                                       * it lands on the road rather than on
                                       * the horizon                        */
/* THE COLOUR IS THE OTHER HALF OF READING IN DAYLIGHT, and it is the half no
 * amount of gain can buy.  Sunlit asphalt is a neutral grey; a pool that is
 * merely BRIGHTER grey is an exposure change, and the eye forgives exposure
 * changes without noticing them.  A pool that is a different COLOUR is a
 * second light source, and the eye cannot help noticing that.  These are a
 * halogen sealed beam's own numbers -- about 3200 K, which is where the R:B
 * ratio below lands -- rather than the near-white this shipped with. */
#define B3_PHOTO_HEAD_R         1.00f /* ~3200 K: the colour a halogen
                                       * sealed-beam lamp of the era threw   */
#define B3_PHOTO_HEAD_G         0.89f
#define B3_PHOTO_HEAD_B         0.68f
#define B3_PHOTO_HEAD_CARS      6     /* racers to light; 0 is none          */

/* ---- tier 7c: THE CARS' OWN TAIL AND BRAKE LAMPS -------------------------
 *
 * The same table, two rows further down.  model+0x1664 / +0x16AC carries a
 * record per lamp TYPE, and the corona pass has always read four of them:
 * type 0 is the headlamp (tier 7b's beam), type 1 the tail lamp, type 2 the
 * brake lamp, type 8 the tailpipe (tier 7d below).  So nothing about WHERE a
 * tail lamp is, or what colour it is, is invented here -- b3_carfx_car_lamps()
 * is type-generic and B3FX_CORONAS carries the recovered colours (1.1, 0, 0)
 * for the tail and (1.4, 0, 0) for the brake, read from 0x004161D0 and
 * 0x004161C0.
 *
 * *** THE BRAKE BIT IS TESTED BEFORE THE TAIL BIT, AND THAT IS RETAIL'S OWN
 * RULE ***, not a simplification: FUN_00187C70 walks its corona table in the
 * order 0x02, 0x04, 0x10, 0x08, ... and the tail row carries an explicit
 * `if (light_byte & BRAKE) continue`.  A braking car therefore draws the brake
 * corona INSTEAD of the tail corona rather than on top of it -- and, because
 * the skip is keyed on the BYTE and not on whether the model owns any type-2
 * records, a braking car whose model has no brake lamps draws NEITHER.  The
 * light this tier adds mirrors that exactly, including the last part, because
 * a "helpful" fallback to the tail would be the one place the pool and the
 * sprite disagreed about what a car is doing.
 *
 * WHY THE SPOT BRANCH AND NOT THE OMNI ONE.  A tail lamp looks like a scenery
 * lamp -- a point, a colour, no cone -- and putting it down the shader's omni
 * path would have been one line shorter.  It would also have been wrong twice
 * over, and both errors are the ones tier 7b already paid for:
 *
 *   * GRAZING INCIDENCE.  The recovered tail lamps sit 0.42 m above the road
 *     (COMP_Car10: y = 0.417).  Two metres behind the car that is N.L = 0.21,
 *     and eight metres behind it is N.L = 0.05 -- so a plain cosine throws
 *     away 80-95% of the pool the moment it leaves the bumper.  `mix(nl, 1,
 *     uHeadK.z)` is a grazing-incidence term and it is the whole reason the
 *     beams read on tarmac; a lamp mounted lower than a headlight needs it
 *     more, not less.
 *   * THE DAY FLOOR.  uLightK's curve takes the streetlights down to 0.22 on
 *     a high white sun, which is right for a streetlight and wrong for a brake
 *     light: the one moment a player must be able to read a brake lamp is the
 *     moment the car in front of them stands on the middle pedal, and that
 *     moment is not reserved for dusk.  uHeadK's floor (B3_PHOTO_HEAD_DAY) is
 *     the curve a lamp CARRIED BY A CAR wants.
 *
 * A spot with a wide cone aimed down the lamp's own recovered normal is both
 * of those for free and costs no shader text at all -- and it buys a third
 * thing that is not a workaround: a tail lamp aimed backwards puts NO light on
 * the road in front of the car, which an omni point at the same place would,
 * and which would have been visible as a red wash under every bonnet.
 *
 * ONE LIGHT PER CAR, at the midpoint of the pair, for tier 7b's arithmetic:
 * the two lamps sit 1.24 m apart and this pool reaches seven metres, so they
 * overlap over all but the first half-metre. */
/* *** TWO KINDS OF OFF, AND THEY ARE NOT THE SAME ACT. ***
 *
 * B3_PHOTO_TAIL_ON=0 retires the LIGHTS: the caller stops filling the slots
 * and every one of them is uploaded as a zero.  It does NOT shorten the
 * shader's uniform array, deliberately, for the reason b3_photo_head_slots()
 * spells out at length -- an array whose length depends on the frame's
 * contents makes the two legs of every A/B compile different programs.
 *
 * B3_PHOTO_TAIL_CARS=0 retires the RESERVATION: the budget drops back by six,
 * the assembled shader is the shorter text, and the frame is the pre-tier
 * frame BYTE FOR BYTE (validate_photo section 14.1c executes exactly that
 * against a reference binary).
 *
 * The difference is measurable and was measured, because the gate was first
 * written with the wrong one and failed on a correct tree.  With the switch
 * off but the reservation standing, the street spills into the spare slots
 * ("whatever it does not use falls to the lamps") -- and even on a frame where
 * it does not, because too few lamps are in range to fill either budget, the
 * picture still moves by 75 pixels and up to 17 levels.  The loop bound is a
 * compile-time constant, and thirty-two additions of zero do not associate the
 * way twenty-four do.  A zeroed slot is free in arithmetic and not in floating
 * point.  Anyone bisecting a pixel difference against a pre-7c build wants
 * _CARS, not _ON. */
#define B3_PHOTO_TAIL_ON        1     /* 0 retires tier 7c's LIGHTS          */
#define B3_PHOTO_TAIL_CARS      6     /* racers whose tails light the road;
                                       * 0 retires the RESERVATION too       */
/* SEVEN METRES, and the number is the LAMP's rather than the camera's -- which
 * is the opposite of B3_PHOTO_HEAD_RANGE's argument and for the opposite
 * reason.  A headlight's problem was that the chase camera hides the first ten
 * metres of its own pool; a tail lamp's pool is BEHIND the car, i.e. between
 * the car and the camera, so every metre of it is on screen and the reach can
 * be the physical one.  A 5 W bulb behind a red lens throws a readable pool
 * about two car-lengths back on dark tarmac, and that is seven metres. */
#define B3_PHOTO_TAIL_RANGE     7.0f  /* metres the tail pool reaches        */
/* THE BRAKE LAMP IS BIGGER AS WELL AS BRIGHTER, and both ratios are retail's.
 * B3FX_CORONAS gives the brake row a size multiplier of 0.75 against the tail
 * row's 0.50 (0x003895BC / 0x003A55F8) and a colour of 1.4 against 1.1, so
 * retail's own brake lamp is 1.27x the tail's brightness at 1.50x its extent.
 * The colours are used verbatim; carrying the SIZE ratio over to the light's
 * RANGE is [S] -- a sprite's half-extent is not a lamp's reach -- but it is
 * the ratio the artists chose for the same two lamps, and the alternative was
 * to invent a second one. */
#define B3_PHOTO_BRAKE_REACH    1.50f /* x TAIL_RANGE; 0.75/0.50 [C]         */
/* THE COLOURS ARE THE CORONA TABLE'S, unscaled.  0x004161D0 and 0x004161C0. */
#define B3_PHOTO_TAIL_R         1.10f
#define B3_PHOTO_TAIL_G         0.00f
#define B3_PHOTO_TAIL_B         0.00f
#define B3_PHOTO_BRAKE_R        1.40f
#define B3_PHOTO_BRAKE_G        0.00f
#define B3_PHOTO_BRAKE_B        0.00f
/* THE GAIN, and it has both ends pinned before anyone complains about either.
 *
 * MEASURED the way tier 7b's wrap was, and the sweep is the reason the number
 * is what it is rather than the round 0.5 it started at.  validate_photo
 * section 14 renders an ordinary drive twice, tails on against tails off, and
 * takes the 99th percentile of the RED-channel delta over the band of road
 * behind the player -- US_C1_V1, six moments a hundred frames apart, 1024x768:
 *
 *     gain    worst frame    best frame
 *     0.20        13             39
 *     0.35        26             66
 *     0.55        44            100      <- started here
 *
 * 0.55 was chosen on the road and looked at afterwards on the ARMCO, which is
 * where it fell over: a barrier beside the car is a vertical surface, so it
 * takes mix(N.L, 1, wrap) at N.L = 1 while the road beside it takes it at
 * N.L = 0.05, and at 0.55 the whole left-hand crash barrier went red.  That is
 * the same wall-versus-road split the beams' WRAP note works through, seen
 * from the other end, and the fix is the same shape: pick the gain against the
 * surface that is CLOSEST to the lamp, not the one the effect was built for.
 *
 * 0.22 puts a readable red wash on the tarmac two car-lengths back and leaves
 * the barrier looking like a barrier with a red lamp near it.  The bounds are
 * TAIL_FLOOR = 5 levels (a tail lamp is a 5 W bulb; it must read, not shout)
 * and TAIL_CEIL = 55, which is where the pool stops being a lamp and starts
 * being a red filter over the tarmac -- both ends pinned, from the start, so
 * that the "too bright" round the beams had to go through four times does not
 * have to happen again here.
 *
 * A MULTIPLE OF THE BEAMS' uHeadK gain and not an absolute one, so the pause
 * menu's HEADLIGHTS row moves the tails with the beams: they are the same
 * class of light and a player who has said "these car lamps are too strong"
 * has said it about both. */
#define B3_PHOTO_TAIL_GAIN      0.22f /* x the beams' uHeadK.x               */
/* WIDE, BUT NOT A HEMISPHERE.  A real tail lamp is close to hemispherical, and
 * a hemisphere here would light the kerb, the barrier and the car's own flanks
 * as strongly as the road it is meant to mark.  70 degrees across and 55 in
 * elevation keeps the pool on the carriageway behind the car; against the
 * beams' 30/3.2 that is "a lamp" where the headlight is "a beam", which is the
 * distinction the two cone pairs exist to make. */
#define B3_PHOTO_TAIL_SPREAD   70.0f  /* degrees ACROSS                      */
#define B3_PHOTO_TAIL_CUTOFF   55.0f  /* degrees in ELEVATION                */

/* ---- tier 7d: THE BOOST FLAME'S OWN LIGHT --------------------------------
 *
 * Same table again, type 8: the tailpipes.  FUN_0017F730 emits the flame from
 * exactly these records (0x0017F73C) into sprite pools 1 and 2, and this tier
 * adds the light that flame has always been missing -- an additive billboard
 * lights nothing, so until now a car could run a metre of blue fire out of its
 * exhausts over pitch-dark tarmac and leave no mark on it.
 *
 * THE INTENSITY IS b3_boostfx_level(), UNTOUCHED.  It is 1.0 while the boost
 * burns, 2.0 for the ignition flare, and it decays at the two recovered rates
 * (FUN_0017A480's tail: 2.5/s down to a floor of 1.0 while lit, 2.0/s to zero
 * when released).  So the light flares when the flame flares and dies when it
 * dies, because it IS the flame's own number and not a second envelope written
 * alongside it.  A crashed car's level is zero, so a wreck carries no flame
 * light -- retail's own gate (carObj+0x18FA), for free.
 *
 * THE COLOUR IS THE SPRITE'S OWN, DERIVED.  Retail picks between two textures
 * by carObj+0x1901 (FUN_0018D0E0: the five Car10 specials burn orange, every
 * other car blue-white) and modulates each by its own recovered constant --
 * 0x00415CC0 (0.70, 0.72, 0.75) for pool 1 and 0x00415CD0 (0.80, 0.80, 0.80)
 * for pool 2.  What a flame EMITS is the whole sprite's energy, so the numbers
 * below are the alpha-weighted mean of each texture's texels times its own
 * modulation constant, normalised to a unit maximum channel:
 *
 *   coronaboost.png     mean (0.0378, 0.0621, 0.1338) x (0.70,0.72,0.75)
 *                         -> (0.264, 0.446, 1.000)      BLUE-WHITE
 *   coronaboostred.png  mean (0.2593, 0.0601, 0.0128) x (0.80,0.80,0.80)
 *                         -> (1.000, 0.232, 0.049)      ORANGE
 *
 * validate_photo section 14 re-derives both from the shipped PNGs rather than
 * reading them from here, so an art change moves the light with the sprite.
 *
 * *** THE DEFAULT FLAME IS BLUE, NOT WARM. ***  Worth saying out loud because
 * every description of this feature reaches for "a warm glow": only five car
 * models in the game burn orange, and the pool behind everybody else is a cold
 * blue-white one.  A gate written for warmth would have failed on 95% of the
 * roster and passed on the wrong five.
 *
 * THE FLICKER IS A HASH AND NOT A rand().  Retail's own flame flickers off a
 * per-frame rand() (the sprite cascade's jitter), and reusing that here would
 * have been the one change in this wave capable of breaking every pinned-frame
 * gate in the tree: two legs of an A/B rendered at the same frame would light
 * the road differently for a reason that has nothing to do with the leg.  So
 * the flicker is a bijective integer mix of (frame counter, car slot) -- no
 * wall clock, no PRNG state, no dependence on how many cars boosted before
 * this one.  Two runs of the same frame produce the same flame, which is what
 * section 14's determinism leg asserts by rendering one twice.
 *
 * *** THESE SLOTS ARE TRANSIENT, AND THAT IS THE CEILING TALKING. ***
 * AFX_LIGHT_MAX is 32 -- the length of the three vec4 uniform arrays the
 * deferred pass declares.  The shipped reservations already spend 18 on the
 * street and 6 on the beams; tier 7c takes 6 more for the tails, which leaves
 * exactly two.  Six boost lights would need 36 and there is no 36 available:
 * see THE CEILING in b3_photo_boost_slots().  So the two spare slots are not
 * assigned to cars at all -- they go to the two NEAREST cars that are actually
 * burning, recomputed every frame, and a frame in which nobody boosts hands
 * them straight to the streetlights.  Unlike the beams' and the tails', this
 * reservation is a POOL rather than a per-car guarantee, and it is honest
 * about it: b3_photo_boost_slots() is how many, not who. */
/* The same two kinds of off as tier 7c's -- see the note over
 * B3_PHOTO_TAIL_ON.  _ON retires the flames; _LIGHTS retires the two slots. */
#define B3_PHOTO_BOOST_ON       1     /* 0 retires tier 7d's LIGHTS          */
#define B3_PHOTO_BOOST_LIGHTS   2     /* the transient pool; 32 - 18 - 6 - 6.
                                       * 0 retires the RESERVATION too       */
/* EIGHT METRES.  Longer than the tail pool because the source is: the
 * recovered cascade throws its three sprites up to 1.4 m out of the pipe and
 * the flare doubles the level, so the thing casting this is a metre of fire
 * and not a bulb.  Still short enough that it reads as coming OUT of the car
 * rather than as a spotlight following it. */
#define B3_PHOTO_BOOST_RANGE    8.0f
/* Like the tails', a multiple of the beams' gain, and MEASURED the same way.
 *
 * THE MEASUREMENT NEEDED A KNOB THAT ALREADY EXISTED, and it is worth writing
 * down because the first three attempts at it measured nothing at all.  Under
 * B3_TESTDRIVE the player only holds the throttle -- it never boosts -- so the
 * only flames on a test drive belong to the AI, and the AI boosts on the far
 * side of the pack: the ledger's own flamedist column says the median burning
 * car is 116 m from the eye and the nearest is 14 m, i.e. usually behind the
 * camera or a dozen pixels wide.  A leg pinned to those frames photographs a
 * correct light and measures zero.  B3_TEST_PAD_BOOST=1 -- which was already
 * in the tree for validate_aftertouch's real-input leg -- holds the boost
 * button from the same local the pad writes, so the PLAYER burns, two metres
 * from the eye, and the pool is on screen.  Section 14's boost legs use it.
 *
 * Swept on US_C1_V1 at 1024x768, six frames of the player's own burn, 99th
 * percentile of the LUMINANCE delta over the road band behind the car (the
 * red-channel metric the tails use is the wrong one here -- the default flame
 * is blue and its red channel is a quarter of its blue):
 *
 *     gain    worst frame    best frame
 *     0.60        33             65
 *     1.20        63            138
 *     2.00        98            174
 *
 * 0.35 lands around 20-40, which is a pool the eye reads as the flame washing
 * the tarmac rather than as a blue spotlight bolted under the car.  Bounds:
 * BOOST_FLOOR = 8 levels and BOOST_CEIL = 90, both ends pinned from the start.
 *
 * The level itself already carries a factor of two between the burn and the
 * ignition flare, so this number is the BURN's and the flare gets double it
 * for free -- which is the recovered envelope doing the work rather than a
 * second one written here. */
#define B3_PHOTO_BOOST_GAIN     0.35f /* x the beams' uHeadK.x, x the level  */
/* +-18%, which is a flame and not a fault.  Swept by eye against the sprite
 * cascade it sits under: below about 10% the pool looks like a solid lamp and
 * above about 25% it reads as a rendering bug rather than as combustion. */
#define B3_PHOTO_BOOST_FLICKER  0.18f
/* NARROWER THAN THE TAILS', not wider, and the first cut had it the other way
 * round on the argument that a flame radiates in every direction.  It does --
 * and a 75/65 cone from a lamp mounted at the rear bumper reaches up over the
 * ROOF, which put a blue rim along the top of the car's own bodywork.  A flame
 * blasting backwards out of a tailpipe lights the tarmac behind it and the
 * diffuser above it, and 60 across by 45 in elevation is that patch. */
#define B3_PHOTO_BOOST_SPREAD  60.0f
#define B3_PHOTO_BOOST_CUTOFF  45.0f
/* THE TWO FLAME COLOURS, derived above from the shipped sprites. */
#define B3_PHOTO_FLAME_R        0.264f
#define B3_PHOTO_FLAME_G        0.446f
#define B3_PHOTO_FLAME_B        1.000f
#define B3_PHOTO_FLAMEHOT_R     1.000f
#define B3_PHOTO_FLAMEHOT_G     0.232f
#define B3_PHOTO_FLAMEHOT_B     0.049f

typedef struct B3PhotoLightRules {
    float emis, emis_dark, emis_lift, emis_min, emis_max, tight;
    float reach, rmin, rmax;
} B3PhotoLightRules;
/* GL-free.  burnout3_scenery.c asks for the thresholds it derives with. */
void b3_photo_light_rules(B3PhotoLightRules *out);

/* The lights the caller picked for this frame, in the GL world frame.
 * `pos[i]` is xyz world + w = 1/radius^2; `col[i]` is the source's own rgb
 * times its power, with w the cosine of the SPOT's hot inner cone; `dir[i]`
 * is the SPOT aim + w = the cosine of its outer spread, and w == 0 means the
 * light is omnidirectional (every scenery lamp) rather than a beam (every
 * headlight).  n == 0 (or no call) stands the term down for the frame.
 * `dusk` is 0 for a high white sun and 1 for the roster's dusk end. */
void b3_photo_set_lights(const float *pos4, const float *col4,
                         const float *dir4, int n, float dusk);
/* How many lights the assembled shader has room for this run -- the caller
 * sorts to this and no further.  0 when the tier is off.
 *
 * It is the SUM of the two reservations: B3_PHOTO_LIGHT_N scenery lamps plus
 * b3_photo_head_slots() beams.  Neither can spend the other's, which is the
 * whole point of there being two numbers. */
int  b3_photo_light_budget(void);

/* ...and the beams' half of it: one slot per racer that can carry a
 * headlight, 0 when tier 7b (or tier 7) is off.  The caller writes its beams
 * into the FRONT of the array and stops at this count; whatever it does not
 * use falls to the lamps, so a race with two cars in it does not waste four
 * slots on cars that are not there. */
int  b3_photo_head_slots(void);

/* ...and tier 7c's and tier 7d's, in the same currency and with the same
 * contract: the caller writes beams, then tails, then flames into the front of
 * the array, each stopping at its own count, and the scenery pick fills what
 * is left and may never evict any of them.
 *
 * THE TWO ARE NOT THE SAME KIND OF NUMBER, and the difference is the point.
 * b3_photo_tail_slots() is a PER-CAR guarantee, exactly as the beams' is: one
 * racer, one slot, never sorted away.  b3_photo_boost_slots() is a POOL -- how
 * many flames the frame can afford at once, which is two -- because the
 * ceiling has no room for a sixth-plus-sixth reservation and a flame, unlike a
 * tail lamp, is off far more often than it is on.  See THE CEILING over
 * b3_photo_boost_slots() in burnout3_aftereffects.c. */
int  b3_photo_tail_slots(void);
int  b3_photo_boost_slots(void);

/* THE CAMERA, handed in once a frame by the caller.
 *
 * Everything above works in WORLD SPACE, reconstructed from the depth buffer,
 * and that is deliberate: it is the one frame in which the shadow matrix, the
 * sun vector, the height fog and the AO radius are all expressible without a
 * second convention to get wrong.  `inv_vp` is the inverse of the SAME
 * view-projection the world was drawn with — including render_frame()'s
 * display mirror, which is why the caller reads it back off the renderer's own
 * matrix stack instead of rebuilding it — so uv + depth -> world needs no sign
 * conventions here at all.
 *
 * Call it every frame BEFORE b3_afx_scene_done(); with no call the layer's
 * depth-reading effects stand down and say so. */
typedef struct B3PhotoCamera {
    float inv_vp[16];   /* clip -> world, column-major as GL wants it        */
    float vp[16];       /* world -> clip, the same matrix forward            */
    float eye[3];       /* the camera position, world space                  */
    float near_z, far_z;
} B3PhotoCamera;
void b3_photo_set_camera(const B3PhotoCamera *cam);

/* The track's sun, in the GL world frame this renderer draws in (i.e. already
 * z-mirrored out of the game frame), pointing TOWARD the sun, plus the track's
 * own light colour.  burnout3_full.c publishes it from the same sidecar carfx
 * reads.  `have` is 0 when the track shipped no sun vector, which stands the
 * shadow pass and the god rays down. */
void b3_photo_set_sun(const float dir_toward[3], const float rgb[3], int have);

/* IS THE RAY ANSWERING THIS FRAME?  1 when the deferred pass has tier 4r's
 * traversal compiled into it AND its world is uploaded -- i.e. when the sun's
 * shadow is being TRACED rather than sampled out of a map.
 *
 * The one caller is burnout3_full.c, and what it does with the answer is SKIP
 * THE DEPTH-MAP PASS ENTIRELY: the ray replaces the map's term outright, so
 * rendering the map as well would be a second full geometry pass whose result
 * nothing reads.  That is where tier 4's own +0.25 ms comes back.
 *
 * Safe to call without a GL context (it answers 0).  INSPIRED; see
 * src/burnout3_rt.h. */
int b3_afx_rt_active(void);

/* ------------------------------------------- tier 4rc: THE CARS' OWN TREES
 *
 * INSPIRED, and the whole design is in src/burnout3_rt.h.  A car is rigid, so
 * its BVH is built once in model space and posed per frame; the shadow ray
 * walks the static world and then, for each instance, transforms itself into
 * that car's space and walks that car's tree.
 *
 * HOW MANY INSTANCES the assembled shader has room for.  0 when the tier is
 * not live at all -- no world, no fleet file, B3_RT_CARS=off, or the ray not
 * answering this frame.  The caller fills up to this many and no further,
 * exactly as it does with b3_photo_light_budget().
 *
 * IT IS FIXED FOR A GIVEN B3_RT_CARS MODE and does not shrink when the caller
 * has fewer cars, for the reason b3_photo_head_slots() spells out at length:
 * the array's length is the SHADER'S, so letting the frame's contents change
 * it makes two legs of a measurement compile different programs, and a program
 * that takes a different time to compile lands a pinned shot on a different
 * race moment.  An unfilled slot is uploaded with a zero radius and costs one
 * compare. */
int b3_afx_rt_car_budget(void);

/* ONE CAR INSTANCE, as the caller already has it.
 *
 * `pos` is the model ORIGIN in the GL world frame and `rot9` is the ROW-MAJOR
 * object->world rotation -- i.e. exactly what car_lamp_pose() hands the corona
 * pass and the headlights, and exactly what the body's own draw matrix is
 * built from.  There is deliberately no second way to say where a car is: a
 * shadow that disagreed with the bodywork about the car's pose would be worse
 * than no shadow.
 *
 * `model` is the packed slot from b3_rt_car_select(); a negative one drops the
 * instance (that vehicle has no tree, so it keeps its blob).
 *
 * `slot` is the caller's OWN index for the car -- the racer's grid slot -- and
 * the only thing this layer does with it is answer b3_afx_rt_car_traced()
 * later in the frame.  Pass -1 for anything that is not a racer.
 *
 * Call b3_afx_rt_cars_begin() once, then this per car, BEFORE the blob-shadow
 * pass, so the blob pass can ask what was taken. */
void b3_afx_rt_cars_begin(void);
void b3_afx_rt_car_add(int slot, int model, const float pos[3],
                       const float rot9[9]);

/* IS THIS RACER CASTING A TRACED SHADOW THIS FRAME?
 *
 * The one question the blob pass asks.  When the answer is yes the blob is
 * SUPPRESSED for that car, because a blobbyshadow quad under a car that is
 * already casting a real shadow is the same darkness applied twice -- and the
 * blob is a fixed ellipse, so it would sit visibly wrong under a shadow that
 * has the car's actual outline.
 *
 * Everything else keeps its blob: a car whose model the extractor refused, a
 * car past the instance budget, every frame with ray tracing off.  The
 * transition is clean in both directions because this is answered from what
 * was actually UPLOADED, not from what was wanted.
 *
 * ONE FRAME OF SLACK, stated rather than glossed: the caller publishes early
 * in the frame and the layer settles what the shader has in it LATER, inside
 * b3_afx_scene_done().  So on the single frame in which ray tracing is turned
 * off, the instances were already published (this answers yes, the blob is
 * skipped) and the deferred pass is rebuilt without the car walk before it
 * draws -- so that one frame has neither shadow.  It is one frame, it happens
 * only when a human presses a key, and the game is PAUSED behind the overlay
 * they pressed it in.  Closing it would mean settling the shader before the
 * scene draws, which is a larger reordering than the artefact is worth. */
int b3_afx_rt_car_traced(int slot);

/* ---------------------------------------------------- THE MSAA SETTING --
 *
 * The pause menu's second SETTINGS row, and it has the shape RAY TRACING has:
 * $B3_MSAA (or $B3_AFX_MSAA) wins outright and greys the row, otherwise the
 * value comes from build/settings.cfg and the menu writes it back.  The two
 * settings share that file, which works because both writers preserve the
 * keys they do not own -- the property b3_rt_save() was built with.
 *
 * `want` is the USER'S NUMBER and `live` is what the context actually gave:
 * MSAA is best-effort here and never fails the chain, so a driver that will
 * not complete a multisampled FBO leaves `live` at 0 with `why` saying so, and
 * the menu shows that rather than the number nobody got.
 *
 * b3_afx_msaa_set() does NOT rebuild the chain itself -- it arms a rebuild for
 * the next frame, through the same resize path a window drag takes.  It is
 * called from the pause overlay, which runs with the scene target bound and
 * half the chain live; tearing those down underneath the frame drawing into
 * them is the one way to make a settings toggle a crash. */
int  b3_afx_msaa_want(void);
void b3_afx_msaa_set(int n);
void b3_afx_msaa_save(void);
int  b3_afx_msaa_env_forced(void);
int  b3_afx_msaa_live(void);
const char *b3_afx_msaa_why(void);

/* ---------------------------------------------- THE HEADLIGHTS SETTING --
 *
 * The pause menu's third SETTINGS row, same idiom again -- an env that wins
 * outright and greys the row, a file underneath it -- and by far the cheapest
 * of the three: RAY TRACING rebuilds an assembled shader, MSAA rebuilds the
 * whole chain, and this moves a float in a uniform that goes up every frame
 * regardless.  b3_afx_head_set() drops the knob cache and that is all it does.
 *
 * IT DRIVES THE WRAP, NOT THE GAIN, and the long note over B3_PHOTO_HEAD_WRAP
 * has the measurement: gain scales the tunnel wall and the blown road by the
 * same factor, wrap is a grazing-incidence term and moves the road alone.
 * "Too bright" from a player is about the road pool, so this is what a
 * brightness row has to be wired to for the row to mean what it says. */
enum { B3_AFX_HEAD_OFF = 0, B3_AFX_HEAD_LOW, B3_AFX_HEAD_MED,
       B3_AFX_HEAD_HIGH };
#define B3_AFX_HEAD_DEF  B3_AFX_HEAD_MED
int   b3_afx_head_want(void);       /* one of the four stops above         */
int   b3_afx_head_on(void);         /* 0 only at OFF                       */
float b3_afx_head_wrap(void);       /* the stop's wrap, for the knob read  */
void  b3_afx_head_set(int n);
void  b3_afx_head_save(void);
int   b3_afx_head_env_forced(void);
const char *b3_afx_head_name(void); /* "OFF" / "LOW" / "MED" / "HIGH"      */

/* The shadow map the deferred pass samples, and the matrix that put it there.
 * burnout3_render.c owns the pass and calls this once it has rendered; passing
 * tex == 0 stands the shadow term down for the frame. */
void b3_photo_set_shadow(unsigned tex, const float light_vp[16], int size);

/* What the layer actually did this frame, as one line for the verdict print:
 * which effects are live, which stood down and why. */
const char *b3_photo_status(void);

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

/* Put the scene target back, exactly as b3_afx_frame_begin() left it, WITHOUT
 * clearing it.  The one caller is the sun shadow pass: it is a geometry pass
 * that has to own a framebuffer of its own for the length of one draw list,
 * and this is how it hands the frame back.  A no-op when the chain is not
 * running, so the caller does not need a second code path.
 *
 * It does not clear, and that is the whole contract -- b3_afx_frame_begin()
 * clears, this restores.  A rebind that cleared would silently throw the sky
 * away on any frame the shadow pass ran after it. */
void b3_afx_rebind_scene(int w, int h);

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
