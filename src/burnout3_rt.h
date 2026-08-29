#ifndef BURNOUT3_RT_H
#define BURNOUT3_RT_H

/* RAY-TRACED SUN SHADOWS — the option, the world, and the reference trace.
 * ======================================================================
 *
 * *** NOTHING IN THIS FILE IS A CLAIM ABOUT BURNOUT 3. ***
 *
 * INSPIRED, in the sense the EVIDENCE MARKS block at the top of
 * src/burnout3_aftereffects.h defines: a modern construction that does NOT
 * exist in the retail image and must never be cited as game behaviour.  The
 * Xbox had no depth texture to read, no acceleration structure, and no shader
 * budget for a per-pixel ray; a sweep of the renderer range
 * 0x00028000..0x00045000 finds nothing of the kind.  This is
 * docs/PHOTOREALISM.md tier 4r, and it is OFF by default.
 *
 * ------------------------------------------------------------------ WHY
 * Tier 4's depth-map pass is one 2048^2 cascade fitted to a 260 m box that
 * follows the camera, and both halves of that sentence are limits a ray does
 * not have.  A ray answers the question at the pixel's own scale, from the
 * pixel's own position, for as far as the world goes:
 *
 *   * CONTACT HARDNESS.  A shadow map has one texel size everywhere, so a
 *     kerb's shadow and a tower's shadow have the same edge.  A cone of rays
 *     the angular width of the sun gives a penumbra that grows with the
 *     occluder's distance, for free, because that is what the geometry says.
 *   * DISTANCE.  Past 260 m the cascade has nothing to say and fades to lit.
 *     A ray does not care how far the occluder is; the bridge four hundred
 *     metres down the freeway shades the road under it.
 *   * NO ACNE, NO PETER-PANNING, NO NORMAL OFFSET.  The whole bias apparatus
 *     tier 4c exists for is a sampling artefact of a MAP.  A ray starting a
 *     hair off the surface has no texel to fall behind.
 *
 * What it costs is a BVH (tools/cextract/cx_bvh.c) and the fill.
 *
 * --------------------------------------------------------------- THE CARS
 * CARS ARE IN IT TOO, through a second and much smaller tree per car MODEL
 * (tools/cextract/cx_car_bvh.c -> build/cars/carbvh.bin) walked by a TWO-LEVEL
 * trace.  The static world's tree cannot hold a car -- a BVH is built once and
 * a car moves every frame -- but a car is RIGID, so its tree is built ONCE in
 * MODEL space and the whole per-frame cost is one 3x4 matrix per instance.  A
 * shadow ray walks the static world as before, then for each car instance
 * transforms ITSELF into that car's space and walks that car's tree.  Nothing
 * is rebuilt, ever, and the top level is a linear scan behind a bounding-sphere
 * reject because with a handful of instances that is cheaper than anything
 * that would index them.
 *
 * When it is live, retail's own blob shadow (FUN_0019A7C0 / FUN_00043570, [C])
 * is SUPPRESSED for exactly the cars in the traced set: a blob under a car
 * that is already casting a real shadow is the same darkness applied twice.
 * Anything outside the set keeps its blob, and with ray tracing off every car
 * does -- see b3_rt_cars_mode() for the knob and for what "the set" is.
 *
 * What is still out is the KNOCKED PROPS, and the DAMAGE STATES: a wreck is
 * traced with its intact hull, because the wrecked body is an aperture shell
 * plus however many panels have not detached yet -- a tree whose CONTENTS
 * change while the car is being driven, which is the one thing this whole
 * construction exists to avoid.  cx_car_bvh.c's THE DAMAGE DECISION carries
 * the argument and docs/PHOTOREALISM.md carries it where a player will find
 * it, rather than hiding it.
 *
 * ------------------------------------------------------------- THE SWITCH
 * DEFAULT OFF, and the off position is a GATE rather than a preference: with
 * ray tracing off the deferred shader is assembled from exactly the strings
 * it was assembled from before this feature existed, so the frame is
 * BIT-IDENTICAL to a build without it.  That is what lets every
 * recovered-pixel suite go on pinning B3_PHOTO=0 and go on measuring
 * recovered behaviour.
 *
 *     the pause menu    SETTINGS -> RAY TRACING: ON / OFF, persisted to
 *                       build/settings.cfg next to build/mixer.cfg
 *     B3_RT=0 / 1       overrides the file outright, for harnesses
 *
 * It also requires the photorealism layer AND its shadow tier: the ray
 * REPLACES the map's term rather than adding to it, so with B3_PHOTO=0 or
 * B3_PHOTO_SHADOW=0 there is no term for it to replace and it stands down.
 *
 * Everything below is GL-FREE, so tools/validate_photo.py can compile this
 * file on its own and EXECUTE the switch and the trace as a probe -- the same
 * construction validate_postfx.py section F and validate_photo.py section 1
 * already use on b3_photo_on(). */

#include <stddef.h>

/* ------------------------------------------------------------ the switch */

/* The user's setting: build/settings.cfg, then $B3_RT on top of it.  Latched
 * on the first call so a harness cannot get two different answers in one
 * frame; b3_rt_set() is how the menu changes it. */
int  b3_rt_want(void);
void b3_rt_set(int on);
/* Persist the current setting.  Follows build/mixer.cfg exactly: a literal
 * "build/..." path through the ordinary fopen, which the ISO shim's write
 * rule routes to the real build/ tree (src/burnout3_isodata.c
 * resolve_write()).  Unknown keys in an existing file are preserved. */
void b3_rt_save(void);
/* 1 when $B3_RT was set, i.e. the file's value is being overridden.  The
 * menu greys the row out rather than lying about what is live. */
int  b3_rt_env_forced(void);

/* SHOULD THE PAUSE MENU OFFER THE OPTION AT ALL ON THIS PLATFORM?
 *
 * The PERF GATE, and it is one line by design.  On the desktop the answer is
 * yes and the numbers are in docs/PHOTOREALISM.md; the web is measured
 * separately and the option is hidden there if it cannot hold the frame,
 * because an option a player can turn on and then watch the game crawl is
 * worse than an option that is not offered.  $B3_RT_SHOW=1 forces the row
 * back on wherever it is hidden, so the measurement stays repeatable.
 *
 * Hiding the ROW does not disable the FEATURE: $B3_RT=1 still turns it on
 * anywhere it can run, which is what keeps the web measurement honest. */
int  b3_rt_option_visible(void);

/* ------------------------------------------------------------- the world */

/* Load <track_dir>/bvh.bin and prepare the two GPU-ready float buffers.
 * Returns 1 on success.  Calling it again frees the previous world first, so
 * a track change is one call.  A missing or malformed file is not an error:
 * it returns 0, says why through b3_rt_status(), and the option stands down.
 *
 * THE GL FRAME.  The artefact is in RAW GAME SPACE (the same space
 * track.obj, props.bin and scenery.bin are in).  This loader applies the
 * SAME single reflection every other geometry loader applies -- negate Z --
 * so what comes out is in the frame the renderer draws and the deferred pass
 * reconstructs.  A box needs its Z ends swapped as well as negated, which is
 * the one place the reflection is not just a sign. */
int  b3_rt_world_load(const char *track_dir);
void b3_rt_world_free(void);
int  b3_rt_world_ready(void);

/* WHICH world is loaded, as a number that only ever goes up.  The GL side
 * uploads two large textures and must re-upload them when the track changes;
 * it used to notice by comparing the buffer POINTER, which is a bug waiting
 * for malloc to hand back the address it just freed -- and on a track change
 * that is exactly what malloc is most likely to do.  A counter cannot do
 * that. */
unsigned b3_rt_world_gen(void);

/* The buffers, laid out for a 2-D RGBA32F texture of width `b3_rt_tex_w()`.
 *
 *   nodes: 2 texels each, in DEPTH-FIRST order, node 0 the root
 *     texel 0   xyz = box min          w = escape index, as a float
 *     texel 1   xyz = box max          w = -1.0 for an INTERIOR node, else
 *                                          first_tri * 8 + (count - 1)
 *   tris:  3 texels each
 *     texel 0   xyz = v0               w = opacity (see cx_bvh.c's OPACITY)
 *     texel 1   xyz = v1               w = 0
 *     texel 2   xyz = v2               w = 0
 *
 * The leaf packing is arithmetic rather than bit twiddling because the target
 * dialect is ESSL 1.00, which has neither bitwise operators nor
 * floatBitsToInt.  `first * 8 + (count - 1)` is exact in a highp float while
 * first < 2^21, which cx_bvh.c enforces at write time. */
const float *b3_rt_node_data(void);
const float *b3_rt_tri_data(void);
int b3_rt_node_texels(void);     /* 2 * node count                         */
int b3_rt_tri_texels(void);      /* 3 * triangle count                     */
int b3_rt_node_count(void);
int b3_rt_tri_count(void);
int b3_rt_max_depth(void);
/* The world's bounds in the GL frame, for the ray's own far clamp. */
void b3_rt_world_bounds(float lo[3], float hi[3]);

/* The texture WIDTH both buffers are laid out for.  A power of two, so the
 * shader's `idx / w` is exact; set by b3_rt_tex_set_width() before the load
 * when the context's GL_MAX_TEXTURE_SIZE demands something smaller. */
int  b3_rt_tex_w(void);
void b3_rt_tex_set_width(int w);
int  b3_rt_node_rows(void);
int  b3_rt_tri_rows(void);

/* One short phrase for the verdict line, or NULL when the world is loaded. */
const char *b3_rt_status(void);

/* ============================================================== THE CARS ==
 *
 * The FLEET FILE, build/cars/carbvh.bin ('B3CV' v1, tools/cextract/cx_car_bvh.c):
 * one model-space BVH per vehicle, both fleets, in one artefact.  Loaded once;
 * a track change does not touch it, because the fleet does not vary by track.
 *
 * THE GL FRAME, as everywhere else: the artefact is RAW GAME SPACE (+Z is the
 * NOSE) and this loader applies the SAME single reflection every other geometry
 * loader applies -- negate Z, and swap a box's Z ENDS as well as negating them.
 * What comes out is the frame src/burnout3_full.c's draw matrix poses, so an
 * instance's transform is literally the one car_lamp_pose() hands the corona
 * pass and the headlights. */
int  b3_rt_cars_load(const char *cars_dir);
void b3_rt_cars_free(void);
int  b3_rt_cars_ready(void);
const char *b3_rt_cars_status(void);
int  b3_rt_car_fleet_count(void);        /* models in the FILE               */

/* WHICH CARS THE RAY MAY TRACE, as one word, from $B3_RT_CARS:
 *
 *     racers   the player and the AI -- the DEFAULT, and the shipped answer
 *     all      ...plus the traffic models the track loaded
 *     off      none; every car keeps its blob, exactly as before this landed
 *
 * It is a knob rather than a setting because its cost is per INSTANCE and the
 * budget is measured: docs/PHOTOREALISM.md carries the curve. */
#define B3_RT_CARS_OFF    0
#define B3_RT_CARS_RACERS 1
#define B3_RT_CARS_ALL    2
int  b3_rt_cars_mode(void);

/* CHOOSE THE MODELS THIS RACE WILL DRAW, and pack only those for the GPU.
 *
 * The fleet file holds all 106 vehicles -- 24 MB of float texture -- and a
 * race puts six racers and at most a dozen traffic models on the road.
 * Uploading the other ninety would be most of the bandwidth and all of the
 * memory for nothing, so the buffer below is packed from a SELECTION.
 *
 * `names[i]` is "<CLASS>_<CarN>", the same key build/cars/<name>.obj uses.
 * Returns the number of DISTINCT models packed; `out_slot[i]` is the packed
 * slot for names[i], or -1 when the fleet file has no such model (a vehicle
 * the extractor refused -- see cx_car_bvh.c's plausibility gate -- which is
 * not an error and simply means that car keeps its blob).  Duplicates collapse
 * onto one slot, which is the common case: six racers are often four models. */
int  b3_rt_car_select(const char *const *names, int n, int *out_slot);
/* How many models the last selection packed.  Read-only, unlike calling
 * b3_rt_car_select() with an empty list, which CLEARS the selection. */
int  b3_rt_car_selected(void);

/* The packed buffer, laid out for ONE 2-D RGBA32F texture of width
 * b3_rt_tex_w().  Nodes first (2 texels each, exactly bvh.bin's layout), then
 * triangles (3 texels each) starting at b3_rt_car_tri_base().
 *
 * ONE texture and not two, unlike the world's pair, and that is a hard
 * constraint rather than tidiness: ESSL 1.00's guaranteed minimum is EIGHT
 * fragment texture image units, and the deferred pass already binds scene,
 * depth, AO, shadow, SSR and the world's two.  The car tree is the eighth and
 * last, so it does not get to be two. */
const float *b3_rt_car_data(void);
int b3_rt_car_texels(void);
int b3_rt_car_rows(void);
int b3_rt_car_tri_base(void);        /* texel index of packed triangle 0     */
int b3_rt_car_node_count(void);
int b3_rt_car_tri_count(void);
unsigned b3_rt_car_gen(void);        /* bumped by every successful select    */

/* One packed model, in the packed buffer's own indices -- what the uploader
 * turns into two of the six per-instance vec4s. */
typedef struct B3RtCarModel {
    float lo[3], hi[3];     /* the model-space box, ALREADY Z-reflected      */
    float radius;           /* about the box centre; the instance reject     */
    float root, end;        /* node indices, as floats, as the shader wants  */
    int   tri_count;
    /* 33, not 32: the artefact's name field is a 32-byte NUL-PADDED window
     * rather than a C string, so a name that fills it needs a terminator of
     * its own here. */
    char  name[33];
} B3RtCarModel;
int b3_rt_car_packed(int slot, B3RtCarModel *out);

/* ---------------------------------------------------- the reference trace
 *
 * The SAME stackless escape walk the shader runs, in C, over the same
 * buffers.  It exists for two reasons and neither of them is the renderer:
 * tools/validate_bvh.py needs a second implementation to disagree with, and
 * tools/validate_photo.py's ray-tracing leg needs to be able to ask "is this
 * point in shadow" without a GL context.
 *
 * Returns TRANSMITTANCE along the segment: 1.0 for a clear path, 0.0 for a
 * fully blocked one, and the product of (1 - opacity) over every cut-out it
 * passed through in between.  `dir` need not be normalised; `tmax` is in the
 * same units as `dir`'s length. */
float b3_rt_transmittance(const float origin[3], const float dir[3],
                          float tmax);

/* The same walk over ONE PACKED CAR MODEL, in that model's own space.  Same
 * contract, same return.  tools/validate_car_bvh.py traces rays through it and
 * against a brute-force test over the model's triangles; tools/validate_photo.py's
 * car leg uses it to assert that a car casts where the frame says it does
 * without needing a GL context to ask. */
float b3_rt_car_transmittance(int slot, const float origin[3],
                              const float dir[3], float tmax);

/* ------------------------------------------------------------ the knobs */

/* The look, gathered here so the whole of it is one block of numbers, and
 * every one overridable by an env of the same name (b3_rt_knobs()). */
#define B3_RT_RAYS      4      /* rays per pixel across the sun's disc     */
/* THE TRAVERSAL BUDGET, and it is MEASURED rather than picked.
 *
 * A stackless escape walk cannot order its children, so an ANY-HIT shadow
 * ray finds its blocker in depth-first order rather than in nearest-first
 * order -- which means the budget is not "how deep is the tree" but "how
 * many boxes does this ray cross before it meets something".  A ray that
 * runs out comes back with whatever transmittance it had, i.e. LIT, so a
 * budget set too low does not slow anything down: it silently DELETES
 * shadows, which is much harder to notice and much worse.
 *
 * Measured over 700 ground rays toward each track's own sun, counting the
 * visits a BLOCKED ray needed (the only ones a cap can cost anything):
 *
 *                       US_C3_V1        AS_M1_V1 (the heaviest, 853k tris)
 *     cap  96          19.8% lost          28.5% lost
 *     cap 128           8.4% lost          11.9% lost
 *     cap 192           0.0% lost           1.2% lost
 *     cap 256           0.0% lost           0.0% lost
 *
 * and the COST stops growing at about the same place, because past ~200 the
 * cap stops binding at all and the walk is ending on its own: 96 -> 8.78 ms,
 * 128 -> 9.55, 192 -> 10.61, 256 -> 10.34 (the last two are one run apart,
 * i.e. the same number).  So 256 is the first value that costs nothing and
 * loses nothing on any shipped track. */
#define B3_RT_STEPS     256    /* traversal iterations before giving up    */
/* THE SUN IS NOT A POINT.  Its angular radius from Earth is 0.265 degrees,
 * and that number -- not a blur radius -- is what makes a ray-traced shadow
 * read as a photograph: the penumbra widens with the occluder's distance on
 * its own, so a kerb is sharp and a rooftop four hundred metres away is soft.
 * It is scaled up here because 0.265 degrees over four rays is a penumbra
 * only a still frame can see, and the mandate is beyond-retail rather than
 * physically-metered.  B3_RT_SUN_DEG=0.265 restores the real sun. */
#define B3_RT_SUN_DEG   0.75f
#define B3_RT_RANGE     900.0f /* metres a shadow ray reaches              */
/* The ray leaves the surface along its own normal by this much, in metres.
 * It is NOT the map's normal offset -- there is no texel to fall behind, and
 * nothing about it scales with a map size.  It is the smallest step that
 * clears a depth-reconstructed position's own error at the distances this
 * camera looks. */
#define B3_RT_ORIGIN    0.05f
/* The ray's own strength, as a multiplier on B3_PHOTO_SH_STRENGTH.  1.0
 * means "exactly as dark as the cascade would have made it", which is the
 * default because the bounded-difference gate in tools/validate_photo.py
 * measures the two against each other on open road. */
#define B3_RT_STRENGTH  1.0f

/* THE WEB VERDICT, as one number, and it is MEASURED rather than chosen.
 *
 * The brief for the web was "only if it comes free": ONE shader that compiles
 * on both, no web-specific optimisation, and the option appears there only if
 * it holds the frame.  It came free.  The ESSL 1.00 traversal compiles under
 * WebGL 2 unchanged -- there is no #ifdef in it, exactly as there is none
 * anywhere else in the chain -- and MEASURED through tools/web_smoke.py
 * --gl hw (headless Chromium on ANGLE/Vulkan, the same RTX 3090), 17 windows
 * of 120 frames each at 1920x1080 with MSAA 4x and the whole photo stack on:
 *
 *     ray tracing OFF   60.0 fps,  render_frame  8.43 ms
 *     ray tracing ON    60.0 fps,  render_frame 10.90 ms
 *
 * i.e. +2.5 ms, the same cost it has on desktop GL, and 5.8 ms of the
 * 16.67 ms budget still unspent.  So the row is OFFERED on the web.
 *
 * 0 would hide it; the FEATURE would still run under $B3_RT=1 either way, so
 * the measurement above stays repeatable whichever way this lands. */
#define B3_RT_WEB_OPTION 1

/* THE RAY BUDGET'S CEILING, and it is a CLAMP rather than a compiled limit.
 *
 * The loop bound is SPLICED into the shader with %d, so raising this costs
 * nothing but the unroll -- there is no fixed 16 anywhere in the traversal.
 * The ceiling exists so that a typo cannot hand the driver a 10,000-iteration
 * unroll and hang the compile, not because 16 is a hardware fact.
 *
 * A user set B3_RT_RAYS=128, saw no frame-rate change at all, and concluded
 * the knob did nothing -- because the clamp was SILENT.  It is not silent any
 * more (b3_rt_knobs announces every clamp once), and the desktop ceiling is
 * lifted to 32, which is two more doublings of the penumbra's sample count
 * for anyone who wants to spend them on a still frame.  The web keeps 16: its
 * shader compiler is the slowest link in that chain and an unroll it refuses
 * costs the whole feature rather than some frame rate. */
#ifdef __EMSCRIPTEN__
#define B3_RT_RAYS_MAX  16
#else
#define B3_RT_RAYS_MAX  32
#endif
#define B3_RT_STEPS_MAX 512

typedef struct B3RtKnobs {
    int   rays, steps;
    float sun_deg, range, origin, strength;
    /* THE FAR FIELD's three.  See THE FAR FIELD below. */
    float dist_bias;    /* origin/tmin growth per (metre of view distance)^2 */
    float slope;        /* blend toward the SUN's grazing reciprocal         */
    float graze;        /* blend toward the VIEW's grazing reciprocal        */
} B3RtKnobs;
void b3_rt_knobs(B3RtKnobs *out);

/* THE FAR FIELD, and why these three knobs exist.
 *
 * A player reported "with ray tracing on, things far away have a shimmer /
 * flash".  Three causes were plausible; the measurement (tools/rt_shimmer.py,
 * and its numbers are in docs/PHOTOREALISM.md) says which one it was.
 *
 * WHAT IT IS.  The ray starts at a world position RECONSTRUCTED FROM DEPTH,
 * and that position carries two errors that both grow with the range:
 *
 *   the DEPTH QUANTUM   a depth buffer's world-space step goes as dist^2 /
 *                       near -- about 5 mm at 200 m with this camera's 0.5 m
 *                       near plane, 4 cm at 600 m, 10 cm at 900 m.
 *                       B3_RT_ORIGIN is a flat 5 cm, so past roughly half a
 *                       kilometre the error EXCEEDS the step meant to clear
 *                       it and the ray starts alternately just above and just
 *                       below its own triangle.
 *   the PIXEL FOOTPRINT down a road at a grazing angle one pixel spans TENS
 *                       OF METRES along the view ray, so one reconstructed
 *                       position stands for a surface that is nowhere near
 *                       flat under it.
 *
 * Both are shadow acne, and a ray having no texel to fall behind never meant
 * it had no position error to fall behind.  So the origin push and the
 * trace's own tmin grow as `origin + dist_bias * dist^2`, scaled by the two
 * grazing angles -- the SUN's (the ordinary slope scale, which is what
 * B3_PHOTO_SH_SLOPE does for the map) and the VIEW's (which the map does not
 * need, because a map's error lives in the light's frame and a
 * depth-reconstructed position's lives in the camera's).
 *
 * WHAT IT IS NOT, and this was the surprise.  The screen-locked golden-angle
 * spiral looked like the obvious culprit -- four samples of a wide penumbra,
 * re-rotated whenever sub-pixel motion moves a surface to a new pixel.
 * Measured, it is not: collapsing the cone to a single hard ray removed 4% of
 * the flicker and 4 -> 32 rays removed 1%, against 62% for the bias terms
 * alone.  So the cone is left exactly as it was, and the rotation stays
 * SCREEN-locked -- which is also what makes a pinned camera render the same
 * bytes twice, as tools/validate_photo.py section 6 measures.
 *
 * Every one of the three is a BLEND from the old behaviour, so setting them
 * to zero restores tier 4r exactly as it shipped and the defect can be
 * measured back into existence rather than argued about. */
#define B3_RT_DIST_BIAS 1.0e-6f   /* metres of push per metre^2 of range     */
#define B3_RT_SLOPE     1.0f      /* blend toward 1/max(N.L, 0.1)            */
#define B3_RT_GRAZE     1.0f      /* blend toward 1/max(|N.V|, 0.1)          */

/* THE ARMING LINE.  One line, once per process, ALWAYS -- on or off, and why.
 *
 * It exists because a user tuning B3_RT_* envs had no way to see whether the
 * feature they were tuning was even running: the option is off by default, it
 * needs the photorealism layer AND its shadow tier, and every one of those can
 * be down for a different reason.  A knob that does nothing because the
 * feature is off looks exactly like a knob that does nothing.
 *
 * `on` is whether the ray is actually answering; `why` is the one phrase for
 * when it is not (NULL when it is).  GL-free: the caller supplies the verdict,
 * this fills in the numbers. */
void b3_rt_announce(int on, const char *why);

#endif /* BURNOUT3_RT_H */
