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

#endif /* BURNOUT3_SCENERY_H */
