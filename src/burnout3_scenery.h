/* burnout3_scenery.h -- INSTANCED TRACK SCENERY (palms, hero trees, lamp
 * posts, signage, benches, moored boats, parked vehicles).
 *
 * The source table is static.dat's FIRST 0x70-record table (hdr +0x34 count /
 * +0x38 table) -- the one tools/py_extract_archive/extract_track.py:23 marks
 * `(not extracted, [S])` and :719-727 argues away as a harmless LOD choice.
 * It is not: on US_C1_V1 it is 36 models placed 1370 times, including 232
 * palm trees, and none of them were on screen.  The tell was that the world
 * mesh DOES draw the `WF_Palm_shadow` decal, so the port painted palm shadows
 * on the road with no palm above them (user capture debug_dump_082).
 *
 * tools/cextract/cx_scenery.c bakes them to build/tracks/<ID>/scenery.bin
 * ('B3SC'); that file carries the full [C] provenance for every offset.  This
 * module is the consumer and is deliberately a near-copy of
 * src/burnout3_props.c's draw half -- same 0x70 model record, same 0x40
 * instance matrix, same shader-class 8/9 vertex, same alpha rules -- minus
 * the physics, because these instances are scenery and retail never
 * registers them as world objects (FUN_00110420 walks the +0x3C table only).
 */
#ifndef BURNOUT3_SCENERY_H
#define BURNOUT3_SCENERY_H

/* Load build/tracks/<ID>/scenery.bin out of `dir`.  Missing file = a quiet
 * no-op, exactly as a track with no +0x34 table would be. */
int  b3_scenery_load(const char* dir);
void b3_scenery_shutdown(void);
int  b3_scenery_ready(void);

/* Instance/model counts, for the validator's log-line assertions. */
int  b3_scenery_instances(void);
int  b3_scenery_models(void);

/* Draw every instance whose model passes the distance cull, inside the
 * world's fog bracket.  `eye` is the camera position in HARNESS (GL) space;
 * pass NULL to skip the cull. */
void b3_scenery_draw(const float eye[3]);

/* ---- THE DERIVED LIGHT FIELD (photorealism tier 7, INSPIRED) -------------
 *
 * There is no light table on the disc -- see the long note over
 * B3_PHOTO_LIGHT_* in burnout3_aftereffects.h for the sweep that establishes
 * that, and for why this is derived per MODEL rather than authored per track.
 *
 * What this module can see that nothing else can: the scenery models, their
 * textures, and 1300-odd placements of them.  A lamp post's bulb is painted
 * into its model's texture, so the bulb is FINDABLE -- bright, opaque texels
 * sitting well above the texture's own mean -- and the model's vertices whose
 * uv lands on them say where it is in model space.  One scan per model at
 * load; every placement of that model then carries the same light.
 *
 * Positions and colours are therefore the ART's, not this port's: a sodium
 * street lamp comes out orange and a fluorescent shopfront comes out white
 * because that is what the artists painted, and no colour for any of it is
 * written down anywhere in this tree.
 *
 * `rgb` is the mean of the bulb's own texels; `radius` is the model's own
 * size times B3_PHOTO_LIGHT_REACH, clamped; `power` is how much of the model
 * lights up, normalised, so a big neon sign outshines a single bulb. */
typedef struct B3ScLight {
    float    pos[3];     /* GL world space, ready to hand to the deferred pass */
    float    rgb[3];     /* 0..1, the source's own texels                      */
    float    radius;     /* metres                                             */
    float    power;      /* 0..1                                               */
    unsigned model;      /* which scenery model it came from                   */
} B3ScLight;

/* The whole track's derived lights.  Returns the count and, through `out`,
 * the array (owned here, valid until b3_scenery_shutdown).  Zero on a track
 * whose scenery has no emissive art at all, which is a legitimate answer. */
int  b3_scenery_lights(const B3ScLight** out);

#endif /* BURNOUT3_SCENERY_H */
