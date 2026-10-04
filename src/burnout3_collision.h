// The GAME'S OWN collision world, loaded from build/collision.bin
// (tools/extract_collision.py: the per-unit kd-tree poly soups inside
// streamed.dat's unit LOD blocks -- format and query chain in
// docs/RE_NOTES.md section 15, execution-verified against FUN_001aff70 /
// FUN_00123790 in tools/validate_gameplay.py's 'collision' section).
//
// Queries run in the harness's GL space (game coordinates with Z negated,
// the same reflection trackmesh_load applies); the loader mirrors each
// triangle and swaps v1/v2 so the game's one-sided ray test keeps its
// orientation.

#ifndef BURNOUT3_COLLISION_H
#define BURNOUT3_COLLISION_H

// Loads build/collision.bin. Returns triangle count, 0 on failure.
int b3_collision_load(const char* path);
int b3_collision_ready(void);

// Raw triangle access (GL space, post-mirror) so the harness can derive
// structures (e.g. the AI's 2D barrier grid) from the real collision data.
int b3_collision_tri_count(void);
// Fills any non-NULL out; returns 0 if i is out of range.
int b3_collision_tri_get(int i, float* v0, float* v1, float* v2,
                         float* normal, unsigned short* type, int* excluded);

// Downward ray probe, the harness equivalent of the game's under-body ray in
// FUN_001239C0 (frame pos, 30 units straight down, nearest parametric hit;
// ray core FUN_001b2230 with the game's exact epsilon constants).
// Casts from (x, y + 2, z) down to (x, y - 28, z).  On hit fills
// *out_height (surface y) and out_normal (unit), and returns the winning
// triangle's u16 surface type (>= 0); returns -1 on no ground.  This is the
// contract burnout3_vehicle_sim.h declares for its wheel pipeline.
// (Surface-type note: low byte 0x26 is the low-grip wreck surface the game
// stamps on crashed-car planes -- FUN_00123790 drops grip to 0.2 on it;
// the static track polys carry low bytes 0x01..0x25.)
int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]);

// Same wheel/body ground query, additionally returning the streamed.dat unit
// that owned the winning triangle.  `out_unit` is 0xff when no ground was
// found.  Vehicle class-2 updates store this identity at body+0x216.
int b3_ground_probe_unit(float x, float y, float z,
                         float* out_height, float out_normal[3],
                         unsigned char* out_unit);

// RESIDENCY, retail's own -- the query that fills body+0x216 and, through
// FUN_00120F30, the body+0x242C "run the base update" latch.  FUN_0011BC60
// @0x0011BD55 takes it from FUN_001AD4A0, which asks FUN_0019D7F0
// @0x0019D7F0 whether the position lies inside a unit's FOUR XZ HALF-PLANES:
//
//     for i in 0..3:  0.0 <= a[i]*x + b[i]*z - c[i]
//
// The body's Y is never read and no ray is cast.  This is NOT
// b3_ground_probe_unit(): a body over a hole, a missing deck or a bridged
// gap is still resident in retail, and the port's ray-hit stand-in was the
// reason ~21% of traffic-path samples reported "no unit" and their cars were
// drawn but skipped by carcol_pass().  Returns 1 and fills *out_unit when
// the point is inside a unit's footprint, 0 (and 0xff) outside every one.
// See burnout3_collision.c for the AABB-vs-quad approximation note.
int b3_collision_unit_at_xz(float x, float z, unsigned char* out_unit);

// How many distinct streamed.dat units collision.bin carried (diagnostics).
int b3_collision_unit_count(void);

// Sphere sweep from 'from' to 'to' (float[3] each) against the collision
// triangles, for body-vs-wall contact.  ONE-SIDED like the game's own
// det>0 ray test: a triangle only blocks a sphere centre on its front
// (winding) side -- the data depends on this (fences are pairs of offset
// one-sided faces; the tall course-boundary walls are single one-sided
// quads passable from outside).  On hit fills hit_pos (the CONTACT POINT
// on the winning triangle), hit_normal (unit push direction, oriented
// toward the sphere centre) and returns 1; from==to degenerates to an
// overlap test at that position.
// Only "wall-like" triangles block (|normal.y| < wall_ny_max); pass 1.1 to
// sweep against everything.  Triangles the game's gather callback excludes
// (surface low byte 0x22/0x23, or type bit 0x1000) never block.
int b3_sweep_sphere(const float* from, const float* to, float radius,
                    float wall_ny_max, float* hit_pos, float* hit_normal);

// ---------------------------------------------------------------------------
// THE RACING GATHER'S TWO RUNTIME FILTERS -- FUN_0011BBE0 [C-disasm]
//
// FUN_0011BBE0 is the per-frame kd-walk callback FUN_0011BE50 hands to
// FUN_001AFF70 (`PUSH 0x11BBE0` @0x0011BD90), i.e. the RACING soup builder.
// Its whole predicate, with EDI = the candidate record (normal at +0x10,
// prim ptr at +0x60, surface u16 at prim+4; layout proved by the appender
// FUN_0010A8E0) and ESI = the vehicle:
//
//   low = type & 0xFF
//   0x0011BBFE  low == 0x23                                  -> skip
//   0x0011BC03  low == 0x22                                  -> skip
//   0x0011BC08  type & 0x1000                                -> skip
//   0x0011BC0D  0x15 <= low   \  the STRUCTURE band
//   0x0011BC15  low <= 0x20   /
//   0x0011BC2D     j = dot(normal, *(vec3*)(veh + 0xB0))     (FUN_00013C60)
//   0x0011BC32     j > 0.5   [0x003B1684]                    -> skip
//   0x0011BC43  -0.7 [0x0039B264] > normal.y                 -> skip
//   0x0011BC4B  FUN_0010A8E0 -- append to the soup
//
// veh+0xB0 is the car's OWN linear velocity vector (RE_NOTES 14: "+0xB0 is
// the true velocity vector, +0xBC its magnitude"), not a relative velocity,
// so (a) reads "drop the structure faces I am already separating from" --
// which is what stops a car snagging on the back plate of a paired one-sided
// armco.  (b) drops downward-facing faces outright, whatever their type.
//
// The first three tests are baked into the loader's `excl` byte; the two
// below need runtime state and are applied here.  `vel` is the car's world
// velocity in the SAME space as from/to (the harness GL space -- both
// filters are invariant under the loader's z mirror, see burnout3_collision.c),
// or NULL to run without (a).  `hit_type` receives the winning triangle's u16
// surface type.  b3_sweep_sphere == this with vel = NULL, hit_type = NULL.
// ---------------------------------------------------------------------------
#define B3_COL_GATHER_VDOT_MAX   0.5f    /* 0x003B1684 @0x0011BC32 */
#define B3_COL_GATHER_NY_MIN   (-0.7f)   /* 0x0039B264 @0x0011BC43 */
/* "no upper bound on the face normal's y" -- FUN_0011BBE0 tests only
 * n.y < -0.7, so a gather that wants retail's set passes this as
 * `wall_ny_max` rather than a real limit.  1.01 clears a normalised n.y of
 * 1.0 with room for the rounding in the loader's normalise. */
#define B3_COL_NO_NY_MAX         1.01f
#define B3_COL_STRUCT_LO         0x15    /* 0x0011BC0D */
#define B3_COL_STRUCT_HI         0x20    /* 0x0011BC15 */

int b3_sweep_sphere_ex(const float* from, const float* to, float radius,
                       float wall_ny_max, const float* vel,
                       float* hit_pos, float* hit_normal,
                       unsigned short* hit_type);

// Same sweep, additionally reporting the WINNING TRIANGLE's index so the
// caller can re-test it with the game's own rules (b3_collision_tri_get, or
// b3_crash_poly_admits for FUN_0011AC30's contact admission).  `hit_tri` may
// be NULL, in which case this is b3_sweep_sphere_ex exactly.
int b3_sweep_sphere_tri(const float* from, const float* to, float radius,
                        float wall_ny_max, const float* vel,
                        float* hit_pos, float* hit_normal,
                        unsigned short* hit_type, int* hit_tri);

// ...and the same sweep with a caller-supplied per-triangle ADMISSION test,
// so the winner is chosen only from triangles the caller accepts.  This is
// not a post-filter: rejecting the winner afterwards would let a face the
// caller does not want MASK a real one behind it, because the sweep returns
// only its single best hit.  `admit` may be NULL (then this is
// b3_sweep_sphere_tri exactly).  The harness uses it to hold the body
// push-out to the geometry retail's chassis box can actually touch.
int b3_sweep_sphere_admit(const float* from, const float* to, float radius,
                          float wall_ny_max, const float* vel,
                          float* hit_pos, float* hit_normal,
                          unsigned short* hit_type, int* hit_tri,
                          int (*admit)(void* user, int tri), void* user);

/* DIAGNOSTIC (not a game path): the signed distance from `p` to the nearest
 * near-vertical face within `radius`, WITHOUT the one-sided front-face test
 * b3_sweep_sphere_ex applies.  Negative means the point is on the BACK side of
 * that face -- the state a car ends in when it pops through a wall, and the
 * one the push-out can never recover from, because the front-face test then
 * rejects the very contact that would push it back out.  Returns 0 if no face
 * is in range. */
int b3_nearest_wall_signed(const float* p, float radius, float wall_ny_max,
                           float* out_signed, float* out_normal);

/* DIAGNOSTIC (not a game path): does the segment a->b pass THROUGH the front
 * face of a near-vertical triangle?  This is the true pop-through test -- it
 * is immune to the false positive that "the nearest face is back-facing",
 * which is the normal state when driving alongside a fence, since fences are
 * PAIRS of offset one-sided faces.  Returns 1 and fills the hit if so. */
int b3_segment_crosses_wall(const float* a, const float* b, float wall_ny_max,
                            float* out_hit, float* out_normal);

typedef struct {
    float v0[3], v1[3], v2[3];
    float normal[3];
    unsigned short type;
} B3CollisionPoly;

int b3_collision_gather_walls(const float center[3], const float half[3],
                               const float* vel, float wall_ny_max,
                               B3CollisionPoly* out, int cap);

int b3_collision_gather(const float center[3], const float half[3],
                        B3CollisionPoly* out, int cap);

/* Retail's generic rigid-body polygon gather (FUN_00109D20 [C], callback
 * FUN_00109CE0 [C] @0x00109CE0..0x00109D12): gathers all collision geometry
 * in the AABB [center-half, center+half], skipping only surface types 0x20
 * (chevrons), 0x22 (cameras/triggers), 0x23 (reverb boundaries) and 0x24
 * (cull planes).  Applies NO velocity filter and NO face normal filter.
 * Used by class-6 props (FUN_0011A490 @0x0011A5FB) and wrecks
 * (FUN_00122D00 @0x00122D4A). */
int b3_collision_gather_rigid(const float center[3], const float half[3],
                              B3CollisionPoly* out, int cap);


/* Retail's own per-frame collision query, FUN_0011BC60: a SPHERE centred on
 * the frame translation ([[veh+0x204]+0x30]) whose radius is the vehicle's
 * half-extent magnitude plus one frame of travel --
 *   r = |veh+0x1D0.xyz| + veh[0xBC] * DAT_0060EA1C
 * [C] @0x0011BCD9 (the norm), @0x0011BD17 (SQRTSS), @0x0011BC7A (speed*dt),
 * handed to FUN_001AFF70 @0x0011BD9B whose leaf test is sphere-vs-AABB.
 * For COMPCAR1 that is 2.56 m at rest and 3.56 m at 60 m/s -- far tighter
 * than the harness's {5.5, 34, 5.5} box, which is why the port fed the wall
 * response twenty records where retail's median is zero.  Applies the same
 * two runtime filters as b3_collision_gather_walls. */
int b3_collision_gather_sphere(const float center[3], float radius,
                               const float* vel, B3CollisionPoly* out,
                               int cap);

// Apply FUN_0011BBE0's runtime wall predicate to an already-frozen raw soup.
// This lets chassis contact and wheel rays share one per-frame gather.
int b3_collision_filter_walls(const B3CollisionPoly* input, int input_count,
                              const float center[3], const float half[3],
                              const float* vel, float wall_ny_max,
                              B3CollisionPoly* out, int cap);

int b3_collision_ray_polys(const B3CollisionPoly* polys, int count,
                           const float start[3], const float end[3],
                           float* hit_t, float normal[3]);

int b3_collision_ray_polys_game_space(const B3CollisionPoly* polys,
                                      int count, const float start[3],
                                      const float end[3], float* hit_t,
                                      float normal[3]);

// THE WHEEL RAY'S SURFACE GATE -- FUN_00123790 @0x00123799..0x0012383E [C].
//
// Retail's per-wheel ground ray does NOT test every polygon in the frozen
// soup.  Before calling FUN_001B2230 it reads the poly's u16 surface flag,
// takes the LOW BYTE, and skips the polygon outright for a racing car:
//
//   cVar6 = veh+0x215;  bVar5 = (cVar6 == 1 || cVar6 == 2 || cVar6 == 3)
//   bVar2 = flags[i] & 0xFF
//   if (bVar5 && veh+0x210 == '\0' && bVar2 != 0x26 && 0xb < bVar2
//       && (bVar2 < 0xc || 0x14 < bVar2))     -> NOT ray-tested
//
// `0xb < bVar2 && bVar2 < 0xc` is empty, so the live rule is simply
//   testable  <=>  (type & 0xFF) <= 0x14  ||  (type & 0xFF) == 0x26
// which is exactly the band src/burnout3_sfx.c:1045-1062 names as driving
// surfaces (eCS ids 0..20) plus the 0x26 wreck plane; ids 0x15..0x25 are the
// game's WALL / non-driving surfaces and a wheel must roll straight through
// them.  0x210 != 0 is the crashed body, which does test everything; the
// racing path this port models is 0x210 == 0.
//
// `class_215` is the vehicle's veh+0x215 class byte.  Pass 0 to disable the
// gate (retail's own behaviour for a class outside {1,2,3}).
int b3_collision_wheel_surface_testable(unsigned short type,
                                        unsigned char class_215);

// b3_collision_ray_polys_game_space with that gate applied per polygon, i.e.
// FUN_00123790's walk rather than a bare nearest-hit query.  The winner is
// still the minimum PARAMETRIC t over the polygons that survive the gate.
int b3_collision_ray_polys_game_space_wheel(const B3CollisionPoly* polys,
                                            int count, const float start[3],
                                            const float end[3], float* hit_t,
                                            float normal[3],
                                            unsigned char class_215);

// 1 when the surface type's low byte lies in FUN_0011BBE0's structure band
// (0x15..0x20) -- the only classification of soup surface types the retail
// code itself makes.  The harness uses it as its OBJECT/PROP class for
// FUN_00112E70's crash trigger (burnout3_td_rules.h section 10).
int b3_collision_is_structure(unsigned short type);

#endif // BURNOUT3_COLLISION_H
