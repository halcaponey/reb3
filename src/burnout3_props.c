/* =========================================================================
 * burnout3_props.c -- destructible track props (cones, barrier boards,
 * marker posts, signposts, boxes).  See burnout3_props.h for the recovery of
 * the placement data and the knock chain, with addresses.
 *
 * Everything data-driven comes out of build/tracks/<ID>/props.bin, written by
 * tools/extract_props.py straight from static.dat.  Nothing about a particular
 * track is compiled in.
 * ====================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>

#include "burnout3_props.h"
#include "burnout3_collision.h"
/* RETAINED RENDERER: the resting props draw from one baked world-space buffer
 * instead of a glPushMatrix/glCallList pair per instance.  See
 * src/burnout3_render.h. */
#include "burnout3_render.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191
#endif

/* ---- props.bin ------------------------------------------------------- */
#define B3P_MAGIC   0x50503342u        /* 'B3PP' little-endian            */
#define B3P_MODEL   0x60
#define B3P_INST    0x50

/* Retail hands a knocked prop one of exactly 0x10 class-6 rigid bodies
 * (FUN_00114730: the free-slot bitmask scan ends `CMP EDX,0x10` @0x0011476D
 * and the recycle scan walks the same 16 @0x001147C3, stride 0x780
 * @0x001147BD from the pool base gameworld+0xC4380 @0x001147E4).        [C] */
#define B3P_MAX_LIVE        16
/* FUN_0011A020 @0x0011A19E..0x0011A1B4: body+0x224 = DAT_0060EA20 +
 * [0x003A7F34] (= 10.0).  Recycle-order key only -- FUN_00114730's scan is its
 * ONLY reader and nothing compares it against the clock.                [C] */
#define B3P_LRU_OFFSET      10.0f
/* mph conversion FUN_00112E70 @0x001132B4 reads from [0x0038994C].      [C] */
#define B3P_MS_TO_MPH       2.2369363f

/* ---- the recovered constants of the knock chain ------------------------- */
/* FUN_0011A020 mass law @0x0011A137..0x0011A191.                        [C] */
#define B3P_MASS_MIN        100.0f     /* [0x003A2928] */
#define B3P_MASS_PER_M2     200.0f     /* [0x003A292C] */
/* FUN_00113960 @0x00113F16: the contact normal is bent toward the relative
 * velocity by this much before the impulse.  [0x0041A4C0]               [C] */
#define B3P_NORM_BLEND      (-0.9f)
/* FUN_00113960 @0x00113F5C: the restitution handed to FUN_0010F8D0 is the
 * global DAT_004A1D98, which is 0.0 in the image (the same number the car
 * agent recovered as B3_CARCOL_WRECK_RESTITUTION).                      [C] */
#define B3P_RESTITUTION     0.0f
/* FUN_0003B060's "is this vector zero" epsilon, [0x003B191C] = 2^-32.   [C] */
#define B3P_EPS2            2.3283064e-10f
/* FUN_0011A330's two drag coefficients: linear [0x003B16C0] = -1.0 on the
 * squared speed, angular [0x003B17F8] = -2.0 on |omega|.                [C] */
#define B3P_LIN_DRAG        (-1.0f)
#define B3P_ANG_DRAG        (-2.0f)
/* FUN_0011A020's launch draw @0x0011A1DD..0x0011A2F3.                   [C] */
#define B3P_LAUNCH_HALF     0.5f       /* [0x003B1684] */
#define B3P_LAUNCH_ONE      1.0f       /* [0x003B168C] */
#define B3P_LAUNCH_SCALE    5.0f       /* [0x003B1694] */

/* --- The knocked prop's WORLD contact.  RECOVERED, no GLUE left. ----------
 * The earlier ledger entry (PH-23) claimed retail had no ground pass for a
 * class-6 body because FUN_0011A330 -- the class-6 vtable's slot +0x00 -- is
 * only two drag terms and FUN_00109560.  That was a misread of the vtable:
 * the class-6 vtable at 0x003B1120 has a SECOND per-frame slot, +0x10 =
 * FUN_0011A490, which the collision manager drives once per frame per
 * allocated body.  FUN_0011A490 gathers the local polygon soup
 * (FUN_00109D20 @0x0011A5FB, into the staging list at 0x005A3AA0) and then
 * calls FUN_00109EA0 @0x0011A706 -- THE shared body-vs-world contact
 * resolve -- guarded by "the body is inside a loaded streaming unit"
 * (+0x216 != 0xFF, and when it is 0xFF the accumulators +0xF0/+0x100/
 * +0x110/+0x120/+0x130 are all cleared instead, @0x0011A6D5).
 * So retail DOES resolve a knocked prop against the world, BEFORE the body's
 * own update integrates -- and the whole chain is now ported:
 *     narrow phase  FUN_00107950  -> b3_rigid_body_obb_plane_contact()
 *     resolve       FUN_00109EA0  -> b3_rigid_body_world_contact()
 * The restitution is the rigid-body ctor's +0x1F8 = [0x003A69C4] = 0.1
 * (FUN_00109270 @0x001094C5); nothing in FUN_0011A020 overrides it.
 * B3_PROP_BALLISTIC=1 still skips the whole pass. */
#define B3P_WORLD_RESTITUTION  0.1f    /* [0x003A69C4] @0x001094C5      [C] */

/* The one harness guard on the knock gate, m/s.  Retail ships nothing here --
 * FUN_001084E0 @0x00113901 is purely geometric -- so this is GLUE, and it is
 * deliberately a SPEED and not a normal component: see the note at the gate in
 * b3p_collide().  A car doing anything at all clears it; a parked one does
 * not, which is the only case it exists for. */
#define B3P_KNOCK_MIN_SPEED    0.05f   /* GLUE */

/* How far below its own authored ground a knocked prop has to be, with no
 * collision soup and no surface under it, before it counts as having left the
 * world and is retired.  GLUE -- see the note at its use.  Generous enough
 * that a real drop off a raised section (the tallest on the shipped tracks is
 * under 20 m) is a FLIGHT and not a retirement. */
#define B3P_LOST_BELOW         25.0f   /* GLUE */

typedef struct {
    float bb_min[3], bb_max[3];
    unsigned first_vertex, n_vertex, first_index, n_index;
    unsigned prop_class;
    float mass, radius, lod_near, lod_far;
    unsigned mat_flags;
    char texture[32];
    /* runtime */
    unsigned tex;
    unsigned list;
    /* FUN_001084E0 @0x001084EF/@0x001085B4: the gate's two derived numbers,
     * the bbox CENTRE and the bbox HALF EXTENTS, both in model space.  Held
     * per model because the gate rebuilds them from the same bbox every call
     * and nothing about them is per instance.                            [C] */
    float bb_c[3], bb_h[3];
} B3PropModel;

/* World-slot type, exactly the retail one: 5 = static prop, 6 = knocked
 * (has a body), 8 = body taken away, dropped by the dispatcher @0x00111D0B. */
enum { B3P_REST = 0, B3P_KNOCKED = 1, B3P_SETTLED = 2 };

typedef struct {
    float base[16];        /* authored transform, HARNESS space, w row fixed */
    float cur[16];         /* live transform, HARNESS space                  */
    float tint[3];         /* doubled half-range instance colour             */
    unsigned model;
    unsigned prop_class;
    unsigned unit;
    float ground_y;        /* surface height under the authored position     */
    float bound_r;         /* broad-phase radius: |ax_k| . h_k summed, which
                            * bounds the instance's OBB whatever its rotation
                            * (the instance matrices carry scale -- row
                            * lengths run 1.000..1.516 on US_C1_V1 -- and
                            * b3p_orthonormalize only ever shrinks them, so
                            * the load-time value stays conservative).       */
    short body;            /* class-6 body slot, -1 = none                   */
    unsigned char state;
    unsigned char has_ground;
    unsigned char aud_swept;   /* B3_PROP_AUDIT: the car's SWEPT box overlapped
                                * this instance at some point in the run     */
    unsigned char aud_hit;     /* B3_PROP_AUDIT: a contact was admitted      */
    unsigned char aud_rest;    /* B3_PROP_AUDIT: reached rest -- frozen by the
                                * world resolve, or retired to B3P_SETTLED   */
    float aud_disp;            /* B3_PROP_AUDIT: furthest the instance has
                                * travelled from its authored transform, m   */
    float aud_turn;            /* B3_PROP_AUDIT: 1 - (live up . authored up),
                                * 0 = upright, 1 = on its side, 2 = inverted */
} B3PropInst;

/* One of the 16 class-6 rigid bodies (gameworld+0xC4380, stride 0x780).
 * The named fields are the body offsets FUN_0011A020 fills in. */
typedef struct {
    B3RigidBody rb;        /* +0x10/+0x40 inertia, +0xB0.. dynamics */
    float rb_frame_store[4][4];   /* the 4x4 is not inline any more.
                                   * Bound by b3p_bind_body_frames(). */
    float mass;            /* +0x1F0 */
    float com_height;      /* +0x1F4 */
    float radius;          /* +0x1CC */
    float lru_key;         /* +0x224 */
    float bbmax[3];        /* +0x1D0, copied from the model bbox @0x0011A0A8 */
    float bbmin[3];        /* +0x1E0, ditto @0x0011A0AF                      */
    int   owner;           /* +0x220 as a prop instance index, -1 = free     */
    unsigned char frozen;  /* +0x20E, the settle latch FUN_00109EA0 raises   */
    unsigned char hit_211; /* +0x211, "a rigid PAIR contact touched me this
                            * frame" -- FUN_00113960 sets it on both bodies
                            * (@0x00113B4x); FUN_00109560 @0x00109592 reads it
                            * and only then clears the sleep latch, so being
                            * hit by a car is what WAKES a settled prop.  [C] */
} B3PropBody;

static B3PropModel* g_model;
static B3PropInst*  g_inst;
static B3PropBody   g_body[B3P_MAX_LIVE];

/* Which of FUN_001084E0's three contact-point arms decided each contact:
 * [0] one of A's face normals, [1] one of B's, [2] an edge cross.  Telemetry
 * only -- the edge arm is the one still standing on GLUE, so its share is
 * worth being able to quote. */
static unsigned long g_obb_arm[3];

/* Regression telemetry for the two defects section 12 fixed, so neither can
 * come back silently:
 *   g_lost  -- props retired for having left the world entirely.  A prop
 *              falling through the road looks exactly like a prop flying away
 *              if all you read is "max travel", which is how the old single
 *              plane-per-polygon narrow phase hid for a whole wave.
 *   g_creep -- the fastest an UNDISTURBED frozen body is still moving.
 *              "Undisturbed" is the whole point: the freeze deliberately does
 *              not clear the impulse accumulator +0x110 (retail rejoins the
 *              normal path @0x00109728 so a car can still knock a settled
 *              prop), so a frozen body that took a world or pair contact this
 *              frame legitimately carries it -- sampling those measures the
 *              knock, not the creep, and reads tens of m/s.  Sampled only when
 *              the body is frozen and NOTHING touched it, which is exactly the
 *              state the old un-acted-on latch left drifting at 0.15-0.31 m/s
 *              for the rest of the race. */
static unsigned long g_lost;
static float g_creep;

/* Point every pooled body at its own frame storage.
 *
 * Idempotent and called from every entry point that can reach the pool, not
 * just from b3_props_load: the collision path runs before any track's props
 * are loaded, and while the matrix was inline that harmlessly read zeros.
 * With the matrix behind a pointer it would read through NULL. */
static void b3p_bind_body_frames(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    for (int i = 0; i < B3P_MAX_LIVE; i++)
        b3_rigid_body_bind_frame(&g_body[i].rb, g_body[i].rb_frame_store);
}
static float g_clock;      /* our mirror of DAT_0060EA20                     */
static int g_nmodel, g_ninst;
static int g_live;
static int g_ready;
static float* g_vtx;       /* 8 floats per vertex, HARNESS space            */
static unsigned short* g_idx;
static unsigned g_nvtx, g_nidx;
static char g_dir[512];
static B3RInstSet*   g_retained;
static unsigned char* g_retained_skip;   /* one byte per instance */

/* ---- B3_PROP_AUDIT: contact-admission telemetry, off by default ---------
 * Resolved once and cached, so the OFF cost is one `if (g_audit)` per collide
 * call.  See the sweep block in b3p_collide(). */
#define B3P_AUDIT_CARS 8
static int   g_audit = -1;
static float g_aud_prev[B3P_AUDIT_CARS][3];
static float g_aud_prev_ax[B3P_AUDIT_CARS][3][3];
static unsigned char g_aud_have[B3P_AUDIT_CARS];
static float g_aud_next_report;

static void b3p_audit_init(void) {
    if (g_audit >= 0) return;
    const char* e = getenv("B3_PROP_AUDIT");
    g_audit = (e && *e && *e != '0') ? 1 : 0;
}

/* One cumulative line per second.  The LAST one in a run is the verdict the
 * validator reads; the format is append-only, like the wreck log's. */
static void b3p_audit_report(void) {
    if (!g_audit || !g_ready) return;
    if (g_clock < g_aud_next_report) return;
    g_aud_next_report = g_clock + 1.0f;
    int swept = 0, hit = 0, miss = 0, hit_only = 0;
    int swept_cone = 0, hit_cone = 0, swept_sign = 0, hit_sign = 0;
    for (int i = 0; i < g_ninst; i++) {
        const B3PropInst* p = &g_inst[i];
        int cls = (int)p->prop_class;
        if (p->aud_swept) {
            swept++;
            if (cls == 1) swept_cone++;
            if (cls == 6) swept_sign++;
            if (p->aud_hit) {
                hit++;
                if (cls == 1) hit_cone++;
                if (cls == 6) hit_sign++;
            } else {
                miss++;
            }
        } else if (p->aud_hit) {
            hit_only++;     /* contacted without a swept sample: a teleport */
        }
    }
    printf("[propaudit] t=%.1f swept %d admitted %d missed %d "
           "(cones %d/%d, signposts %d/%d) untracked %d\n",
           g_clock, swept, hit, miss, hit_cone, swept_cone,
           hit_sign, swept_sign, hit_only);
    /* Did the knock actually DO anything?  A gate that admits a contact but
     * leaves the prop standing on its authored transform is no better than
     * one that never fires, so the flight is measured on the same instances.
     * `flew` counts an instance that left its transform by more than half a
     * metre, `tumbled` one whose up axis turned past 60 degrees
     * (1 - cos 60 = 0.5), `atrest` one the world resolve froze or the pool
     * retired -- retail's two ways for a knocked prop to stop. */
    int knocked = 0, flew = 0, tumbled = 0, atrest = 0;
    int sflew = 0, sknock = 0;
    float maxdisp = 0.0f;
    for (int i = 0; i < g_ninst; i++) {
        const B3PropInst* p = &g_inst[i];
        if (!p->aud_hit) continue;
        knocked++;
        if (p->aud_disp > 0.5f) flew++;
        if (p->aud_turn > 0.5f) tumbled++;
        if (p->aud_rest) atrest++;
        if (p->aud_disp > maxdisp) maxdisp = p->aud_disp;
        if ((int)p->prop_class == 6) {
            sknock++;
            if (p->aud_disp > 0.5f) sflew++;
        }
    }
    printf("[propflight] t=%.1f knocked %d flew %d tumbled %d atrest %d "
           "maxdisp %.2f signposts %d/%d flew\n",
           g_clock, knocked, flew, tumbled, atrest, maxdisp, sflew, sknock);
    printf("[proparm] t=%.1f aface %lu bface %lu edge %lu lost %lu "
           "creep %.4f\n", g_clock, g_obb_arm[0], g_obb_arm[1], g_obb_arm[2],
           g_lost, g_creep);
    if (getenv("B3_PROP_TRACE"))
        for (int i = 0; i < g_ninst; i++) {
            const B3PropInst* p = &g_inst[i];
            if (p->aud_disp > 30.0f)
                printf("[propfar] inst %d class %d disp %.1f state %d "
                       "body %d\n", i, (int)p->prop_class, p->aud_disp,
                       p->state, p->body);
        }
    fflush(stdout);
}

/* ---- small helpers ---------------------------------------------------- */
static unsigned rd_u32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static float rd_f32(const unsigned char* p) {
    unsigned v = rd_u32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static float v_len(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}
static float v_dot3(const float a[4], const float b[4]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The GL column-major instance matrix and B3RigidBody's row frame are the same
 * 16 floats: GL column c holds the object's c-th axis in world space, which is
 * exactly frame row c.  cur[r*4+c] == frame[r][c]. */
static void mat_to_frame(const float m[16], float f[4][4]) {
    memcpy(f, m, 16 * sizeof(float));
}
static void frame_to_mat(const float f[4][4], float m[16]) {
    memcpy(m, f, 16 * sizeof(float));
}

/* ---- the game PRNG, FUN_0011A020 @0x0011A1DD..0x0011A2C0 ---------------- *
 * state = state*0x10000 + (int)state>>16 + inc;  inc += state;  u = state*2^-32
 * ([0x0054F46C]).  Retail draws from the SHARED global pair DAT_0064ACE8 /
 * DAT_0064ACEC, which every other system also advances, so the exact retail
 * sequence is not reproducible from outside the game -- the LAW is [C], the
 * seed is [?].  Same generator as b3_rand01 in burnout3_vehicle_sim.c. */
static unsigned g_rng_state = 0xFD462907u;   /* FUN_001214A0's seeds          */
static unsigned g_rng_inc   = 0x02B9D6F8u;

void b3_props_seed(unsigned state, unsigned inc) {
    g_rng_state = state;
    g_rng_inc = inc;
}
static double b3p_rand01(void) {
    int s = (int)g_rng_state;
    unsigned n = (unsigned)s * 0x10000u + (unsigned)(s >> 16) + g_rng_inc;
    g_rng_inc = n + g_rng_inc;
    g_rng_state = n;
    return (double)n * 2.3283064365386963e-10;
}

/* ---- FUN_000FF270 [C], the frame re-orthonormaliser FUN_00109560 calls ---
 * @0x000FF27D..0x000FF544.  All three rows are normalised first (FUN_0002C0D0
 * returns the PRE-normalisation length); then the function keeps the pair of
 * rows that is already the most orthogonal and rebuilds the other two:
 *
 *   a = |r2 . r1|   b = |r0 . r2|   c = |r1 . r0|      (FUN_00013C60, then the
 *                                                       ABS at FUN_000FF090)
 *   b > a && c > a  ->  A: r0 = ^(r1 x r2), r2 = ^(r0 x r1)   @0x000FF332
 *   b > a && c <= a ->  C: r2 = ^(r0 x r1), r1 = ^(r2 x r0)   @0x000FF37C
 *   b <= a && c <= b->  C                                     @0x000FF3D3
 *   b <= a && c > b ->  B: r1 = ^(r2 x r0), r0 = ^(r1 x r2)   @0x000FF3D5
 * and the three degenerate entries take the same three shapes:
 *   L0 <= 0 -> A @0x000FF4E1,  L1 <= 0 -> B @0x000FF47D,  L2 <= 0 -> C
 *   @0x000FF41B.
 *
 * NOTE: b3_mat_orthonormalize() in burnout3_vehicle_sim.c implements ONLY
 * branch B (which is what a racecar's frame takes, hence its green suite).  A
 * knocked prop tumbles fast enough to take A and C every few frames, so this
 * module carries the whole function -- see docs/PHYSICS_GLUE_LEDGER.md PH-01. */
static void b3p_norm4(float v[4]) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
    if (l == 0.0f) return;
    for (int k = 0; k < 4; k++) v[k] /= l;
}
static float b3p_len4(const float v[4]) {
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
}
static void b3p_cross4(const float a[4], const float b[4], float o[4]) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
    o[3] = a[3]*b[3] - a[3]*b[3];      /* the real code's self-cancelling lane */
}
static void b3p_orthonormalize(float m[4][4]) {
    float L0 = b3p_len4(m[0]), L1 = b3p_len4(m[1]), L2 = b3p_len4(m[2]);
    b3p_norm4(m[0]); b3p_norm4(m[1]); b3p_norm4(m[2]);
    int br;                                   /* 0 = A, 1 = B, 2 = C */
    if (L0 <= 0.0f)      br = 0;
    else if (L1 <= 0.0f) br = 1;
    else if (L2 <= 0.0f) br = 2;
    else {
        float a = fabsf(v_dot3(m[2], m[1]));
        float b = fabsf(v_dot3(m[0], m[2]));
        float c = fabsf(v_dot3(m[1], m[0]));
        if (b > a) br = (c > a) ? 0 : 2;
        else       br = (c <= b) ? 2 : 1;
    }
    float t[4];
    if (br == 0) {
        b3p_cross4(m[1], m[2], t); memcpy(m[0], t, sizeof t); b3p_norm4(m[0]);
        b3p_cross4(m[0], m[1], t); memcpy(m[2], t, sizeof t); b3p_norm4(m[2]);
    } else if (br == 1) {
        b3p_cross4(m[2], m[0], t); memcpy(m[1], t, sizeof t); b3p_norm4(m[1]);
        b3p_cross4(m[1], m[2], t); memcpy(m[0], t, sizeof t); b3p_norm4(m[0]);
    } else {
        b3p_cross4(m[0], m[1], t); memcpy(m[2], t, sizeof t); b3p_norm4(m[2]);
        b3p_cross4(m[2], m[0], t); memcpy(m[1], t, sizeof t); b3p_norm4(m[1]);
    }
}

/* ---- FUN_00109560 [C], the shared rigid-body integrator ------------------
 * Line for line the same function burnout3_vehicle_sim.c ported and verified
 * (tools/validate_port.py, integrator section); repeated here ONLY so the prop
 * path can use the complete FUN_000FF270 above.  Prop bodies always take the
 * state-6 branch @0x00109606, so gravity is applied at pos + up*com_height. */
static void b3p_mat_mul3(const float A[3][4], const float B[3][4],
                         float out[3][4]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = B[i][0]*A[0][j] + B[i][1]*A[1][j] + B[i][2]*A[2][j];
}
/* FUN_00040AE0: +0x70 = the inverse frame (rotation transposed, translation
 * back-rotated and negated).  Split out of the integrator because the
 * narrow phase FUN_00107950 reads it and, on the frame a body is promoted,
 * the integrator has not run yet. */
static void b3p_build_inv_frame(B3RigidBody* rb) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) rb->inv_frame[i][j] = rb->frame[i][j];
    float t;
    t = rb->inv_frame[0][1]; rb->inv_frame[0][1] = rb->inv_frame[1][0];
    rb->inv_frame[1][0] = t;
    t = rb->inv_frame[0][2]; rb->inv_frame[0][2] = rb->inv_frame[2][0];
    rb->inv_frame[2][0] = t;
    t = rb->inv_frame[1][2]; rb->inv_frame[1][2] = rb->inv_frame[2][1];
    rb->inv_frame[2][1] = t;
    float p[4];
    for (int j = 0; j < 4; j++)
        p[j] = rb->inv_frame[3][0]*rb->inv_frame[0][j]
             + rb->inv_frame[3][1]*rb->inv_frame[1][j]
             + rb->inv_frame[3][2]*rb->inv_frame[2][j];
    for (int j = 0; j < 4; j++) rb->inv_frame[3][j] = -p[j];
}

static void b3p_integrate(B3RigidBody* rb, float mass, float com, float dt,
                          int frozen) {
    /* DAT_0040A8A0 = (0, -20, 0, 0), scaled by (1, mass, 1, -) */
    const float g[4] = { 0.0f, -20.0f * mass, 0.0f, 0.0f };
    float pt[4], r[3];
    for (int i = 0; i < 4; i++) {
        pt[i] = rb->frame[1][i] * com + rb->frame[3][i];
        rb->force_acc[i] += g[i];
    }
    for (int i = 0; i < 3; i++) r[i] = pt[i] - rb->frame[3][i];
    rb->torque_acc[0] += r[1]*g[2] - r[2]*g[1];
    rb->torque_acc[1] += r[2]*g[0] - r[0]*g[2];
    rb->torque_acc[2] += r[0]*g[1] - r[1]*g[0];

    /* THE SLEEP LATCH, @0x00109692..0x001096EC [C].  After the gravity
     * accumulation and before the accumulators are consumed, a body whose
     * +0x20E is still raised has its whole dynamic state zeroed: omega
     * (+0xD0), angular momentum (+0xE0), velocity and speed (+0xB0..+0xBC),
     * the travel direction reset to matrix row 2 (+0xC0 = [[+0x204]+0x20]),
     * and BOTH accumulators +0xF0/+0x100 cleared -- which cancels the gravity
     * just added.  Execution then rejoins the normal path @0x00109728, so the
     * IMPULSE accumulator +0x110 is deliberately NOT cleared: a car that hits
     * a settled prop still knocks it in the same frame.
     *
     * Without this a prop the world resolve had settled kept integrating with
     * whatever residue it had left, and a cone at rest crept upward at about
     * 0.02 m/s for the rest of the race. */
    if (frozen) {
        for (int i = 0; i < 4; i++) {
            rb->vel[i] = 0.0f;
            rb->omega[i] = 0.0f;
            rb->angmom[i] = 0.0f;
            rb->force_acc[i] = 0.0f;
            rb->torque_acc[i] = 0.0f;
            rb->dir[i] = rb->frame[2][i];
        }
    }

    const float dtv[4] = { dt, dt, dt, 0.0f };
    float vel4[4] = { rb->vel[0], rb->vel[1], rb->vel[2], rb->vel[3] };
    for (int i = 0; i < 4; i++) {
        rb->imp_force[i] += rb->force_acc[i] * dtv[i];
        rb->imp_torque[i] += rb->torque_acc[i] * dtv[i];
        rb->force_acc[i] = 0.0f;
        rb->torque_acc[i] = 0.0f;
    }
    for (int i = 0; i < 4; i++) {
        vel4[i] += rb->imp_force[i] / mass;
        rb->imp_force[i] = 0.0f;
    }
    if (vel4[1] > 120.0f) vel4[1] = 120.0f;
    for (int i = 0; i < 4; i++) {
        rb->angmom[i] += rb->imp_torque[i];
        rb->imp_torque[i] = 0.0f;
    }
    for (int j = 0; j < 4; j++)
        rb->omega[j] = rb->angmom[0] * rb->inv_inertia_world[0][j]
                     + rb->angmom[1] * rb->inv_inertia_world[1][j]
                     + rb->angmom[2] * rb->inv_inertia_world[2][j];
    const float w2 = v_dot3(rb->omega, rb->omega);
    if (w2 > 10000.0f) {
        b3p_norm4(rb->omega);
        const float s = (1.0f / sqrtf(w2)) * 100.0f;
        for (int i = 0; i < 4; i++) rb->omega[i] *= s;
        for (int i = 0; i < 4; i++) rb->angmom[i] *= 0.95f;
    }
    float rr[4];
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += vel4[i] * dtv[i];
        rr[i] = rb->omega[i] * dtv[i];
    }
    for (int row = 2; row >= 0; row--) {
        float c[4];
        b3p_cross4(rb->frame[row], rr, c);
        for (int i = 0; i < 4; i++) rb->frame[row][i] -= c[i];
    }
    b3p_orthonormalize(rb->frame);
    for (int i = 0; i < 4; i++) {
        rb->frame[3][i] += rb->deflection[i];
        rb->deflection[i] = 0.0f;
    }
    float Rt[3][4], tmp[3][4];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            Rt[i][j] = rb->frame[j][i];
    for (int i = 0; i < 3; i++) Rt[i][3] = 0.0f;
    b3p_mat_mul3(rb->inv_inertia_body, Rt, tmp);
    b3p_mat_mul3(rb->frame, tmp, rb->inv_inertia_world);
    b3p_build_inv_frame(rb);
    /* FUN_000FFC80: refresh +0xBC speed and +0xC0 unit travel direction */
    for (int i = 0; i < 3; i++) rb->vel[i] = vel4[i];
    rb->vel[3] = vel4[3];
    const float ls = v_dot3(vel4, vel4);
    if (ls < B3P_EPS2) {
        rb->vel[3] = 0.0f;
        for (int i = 0; i < 4; i++) rb->dir[i] = rb->frame[2][i];
    } else {
        float l = sqrtf(ls);
        rb->vel[3] = l;
        for (int i = 0; i < 3; i++) rb->dir[i] = vel4[i] / l;
        rb->dir[3] = 0.0f;
    }
}

/* ---- GAME <-> HARNESS mirror -------------------------------------------
 * The vehicle pipeline's B3RigidBody lives in GAME space; this module (like
 * the collision world and the renderer) lives in HARNESS/GL space, which is
 * game space reflected through S = diag(1,1,-1) (RE_NOTES 12; the same
 * reflection burnout3_collision.c's loader and harness_ground_probe apply).
 * Under an improper map a true vector transforms as S*v while a PSEUDO-vector
 * (omega, torque) transforms as -S*w, because S(a x b) = -(Sa x Sb); the
 * inertia tensor transforms as S I S, i.e. the entries with exactly one z
 * index flip sign. */
static void b3p_mirror_rb(const B3RigidBody* in, B3RigidBody* out) {
    /* `out` is the caller's body and owns its frame storage; carry the
     * binding across the zeroing or the writes below go through NULL. */
    float (*keep_frame)[4] = out->frame;
    memset(out, 0, sizeof *out);
    out->frame = keep_frame;
    for (int r = 0; r < 3; r++) {
        out->frame[r][0] =  in->frame[r][0];
        out->frame[r][1] =  in->frame[r][1];
        out->frame[r][2] = -in->frame[r][2];
    }
    out->frame[3][0] =  in->frame[3][0];
    out->frame[3][1] =  in->frame[3][1];
    out->frame[3][2] = -in->frame[3][2];
    out->frame[3][3] = 1.0f;
    out->vel[0] =  in->vel[0]; out->vel[1] =  in->vel[1];
    out->vel[2] = -in->vel[2]; out->vel[3] =  in->vel[3];
    out->dir[0] =  in->dir[0]; out->dir[1] =  in->dir[1];
    out->dir[2] = -in->dir[2]; out->dir[3] =  in->dir[3];
    out->omega[0] = -in->omega[0];
    out->omega[1] = -in->omega[1];
    out->omega[2] =  in->omega[2];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out->inv_inertia_world[r][c] =
                ((r == 2) != (c == 2)) ? -in->inv_inertia_world[r][c]
                                       :  in->inv_inertia_world[r][c];
}

/* FUN_00106500 [C]: an impulse at a world point.  +0x110 += J and
 * +0x120 += (P - pos) x J. */
static void b3p_impulse_at(B3RigidBody* rb, const float imp[4],
                           const float pt[4]) {
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = pt[k] - rb->frame[3][k];
    rb->imp_force[0] += imp[0];
    rb->imp_force[1] += imp[1];
    rb->imp_force[2] += imp[2];
    rb->imp_force[3] += imp[3];
    rb->imp_torque[0] += r[1] * imp[2] - r[2] * imp[1];
    rb->imp_torque[1] += r[2] * imp[0] - r[0] * imp[2];
    rb->imp_torque[2] += r[0] * imp[1] - r[1] * imp[0];
}

/* FUN_001066A0 [C]: point velocity = omega x (pt - pos) + vel. */
static void b3p_point_vel(const B3RigidBody* rb, const float pt[4],
                          float out[4]) {
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = pt[k] - rb->frame[3][k];
    out[0] = rb->omega[1] * r[2] - rb->omega[2] * r[1] + rb->vel[0];
    out[1] = rb->omega[2] * r[0] - rb->omega[0] * r[2] + rb->vel[1];
    out[2] = rb->omega[0] * r[1] - rb->omega[1] * r[0] + rb->vel[2];
    out[3] = rb->vel[3];
}

/* FUN_0010F8D0 [C]: the two-body contact impulse.  Identical to the car
 * agent's b3_carcol_mutual_impulse (docs/RE_CARCOL.md); kept local so this
 * module does not depend on the car-collision translation unit.  Writes
 * n * -|j| and returns |j|. */
static float b3p_mutual_impulse(const B3RigidBody* rb1, float m1,
                                const B3RigidBody* rb3, float m3,
                                const float pt3[4], const float pt1[4],
                                const float vrel[4], const float n[4],
                                float restitution, float out[4]) {
    float r3[3], r1[3], c3[3], c1[3], a3[3], a1[3];
    for (int k = 0; k < 3; k++) {
        r3[k] = pt3[k] - rb3->frame[3][k];
        r1[k] = pt1[k] - rb1->frame[3][k];
    }
    c3[0] = r3[1]*n[2] - r3[2]*n[1];
    c3[1] = r3[2]*n[0] - r3[0]*n[2];
    c3[2] = r3[0]*n[1] - r3[1]*n[0];
    c1[0] = r1[1]*n[2] - r1[2]*n[1];
    c1[1] = r1[2]*n[0] - r1[0]*n[2];
    c1[2] = r1[0]*n[1] - r1[1]*n[0];
    for (int k = 0; k < 3; k++) {
        a3[k] = rb3->inv_inertia_world[0][k]*c3[0]
              + rb3->inv_inertia_world[1][k]*c3[1]
              + rb3->inv_inertia_world[2][k]*c3[2];
        a1[k] = rb1->inv_inertia_world[0][k]*c1[0]
              + rb1->inv_inertia_world[1][k]*c1[1]
              + rb1->inv_inertia_world[2][k]*c1[2];
    }
    float dx = (a3[1]*r3[2] - a3[2]*r3[1]) + (a1[1]*r1[2] - a1[2]*r1[1]);
    float dy = (a3[2]*r3[0] - a3[0]*r3[2]) + (a1[2]*r1[0] - a1[0]*r1[2]);
    float dz = (a3[0]*r3[1] - a3[1]*r3[0]) + (a1[0]*r1[1] - a1[1]*r1[0]);
    float den = 1.0f/m1 + 1.0f/m3 + dx*n[0] + dy*n[1] + dz*n[2];
    float num = -(restitution + 1.0f) * (n[0]*vrel[0] + n[1]*vrel[1]
                                       + n[2]*vrel[2]);
    float j = fabsf(num / den);
    for (int k = 0; k < 4; k++) out[k] = n[k] * (-j);
    return j;
}

/* ---- texture ---------------------------------------------------------- */
static unsigned load_tex(const char* dir, const char* name) {
    if (!name || !name[0]) return 0;
    char path[768];
    SDL_Surface* s = NULL;
    snprintf(path, sizeof path, "%s/textures/%s.png", dir, name);
    s = IMG_Load(path);
    if (!s) {
        snprintf(path, sizeof path, "build/textures/%s.png", name);
        s = IMG_Load(path);
    }
    if (!s) return 0;
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c) return 0;
    unsigned id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    b3r_tex_mipmap_pre();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c->w, c->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, c->pixels);
    b3r_gen_mipmap();
    /* GL_GENERATE_MIPMAP is GL 1.4 / GLES1: it does not exist in GLES2, and
     * therefore not in WebGL either.  Asking for it there is INVALID_ENUM,
     * the chain never gets built, and a GL_LINEAR_MIPMAP_LINEAR minifier on
     * a texture with no chain is MIPMAP-INCOMPLETE -- which samples black.
     * glGenerateMipmap() after the upload is the GL 3.0 / GLES2 spelling of
     * the same thing, and it is what every target here uses now. */
    SDL_FreeSurface(c);
    return id;
}

/* ---- load ------------------------------------------------------------- */
void b3_props_shutdown(void) {
    if (g_retained) { b3r_inst_free(g_retained); g_retained = NULL; }
    free(g_retained_skip); g_retained_skip = NULL;
    if (g_model) {
        for (int i = 0; i < g_nmodel; i++)
            if (g_model[i].tex) glDeleteTextures(1, &g_model[i].tex);
    }
    free(g_model); free(g_inst); free(g_vtx); free(g_idx);
    g_model = NULL; g_inst = NULL; g_vtx = NULL; g_idx = NULL;
    g_nmodel = g_ninst = g_live = g_ready = 0;
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        /* the memset wiped rb.frame; re-point it at this slot's storage */
        b3_rigid_body_bind_frame(&g_body[i].rb, g_body[i].rb_frame_store);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_nvtx = g_nidx = 0;
}

int b3_props_load(const char* track_dir) {
    b3p_bind_body_frames();   /* the 4x4 is not inline any more */
    b3p_audit_init();
    memset(g_aud_have, 0, sizeof g_aud_have);
    g_aud_next_report = 0.0f;
    b3_props_shutdown();
    if (!track_dir || !track_dir[0]) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", track_dir);

    char path[768];
    snprintf(path, sizeof path, "%s/props.bin", track_dir);
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("props: no %s (props disabled)\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x30) { fclose(f); return 0; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return 0; }
    fclose(f);

    if (rd_u32(d) != B3P_MAGIC || rd_u32(d + 4) != 1) {
        printf("props: %s is not a version-1 B3PP file\n", path);
        free(d);
        return 0;
    }
    g_nmodel   = (int)rd_u32(d + 0x08);
    g_ninst    = (int)rd_u32(d + 0x0C);
    g_nvtx     = rd_u32(d + 0x10);
    g_nidx     = rd_u32(d + 0x14);
    unsigned om = rd_u32(d + 0x18), oi = rd_u32(d + 0x1C);
    unsigned ov = rd_u32(d + 0x20), ox = rd_u32(d + 0x24);
    if (g_nmodel <= 0 || g_ninst <= 0) { free(d); return 0; }

    g_model = (B3PropModel*)calloc((size_t)g_nmodel, sizeof *g_model);
    g_inst  = (B3PropInst*)calloc((size_t)g_ninst, sizeof *g_inst);
    g_vtx   = (float*)calloc(g_nvtx ? g_nvtx * 8 : 1, sizeof(float));
    g_idx   = (unsigned short*)calloc(g_nidx ? g_nidx : 1, sizeof(unsigned short));
    if (!g_model || !g_inst || !g_vtx || !g_idx) { free(d); b3_props_shutdown(); return 0; }

    for (int i = 0; i < g_nmodel; i++) {
        const unsigned char* r = d + om + (size_t)i * B3P_MODEL;
        B3PropModel* m = &g_model[i];
        for (int k = 0; k < 3; k++) {
            m->bb_min[k] = rd_f32(r + k * 4);
            m->bb_max[k] = rd_f32(r + 0x0C + k * 4);
        }
        /* game -> harness: the Z axis is reflected (RE_NOTES 12), so the
         * bbox's z bounds swap sign and swap roles. */
        float zn = -m->bb_max[2], zx = -m->bb_min[2];
        m->bb_min[2] = zn; m->bb_max[2] = zx;
        m->first_vertex = rd_u32(r + 0x18);
        m->n_vertex     = rd_u32(r + 0x1C);
        m->first_index  = rd_u32(r + 0x20);
        m->n_index      = rd_u32(r + 0x24);
        m->prop_class   = rd_u32(r + 0x28);
        m->mass         = rd_f32(r + 0x2C);
        m->radius       = rd_f32(r + 0x30);
        m->lod_near     = rd_f32(r + 0x34);
        m->lod_far      = rd_f32(r + 0x38);
        m->mat_flags    = rd_u32(r + 0x3C);
        memcpy(m->texture, r + 0x40, 31);
        m->texture[31] = 0;
        /* FUN_001084E0 @0x0010851B/@0x001085B4 -- the gate's two derived
         * numbers.  Done once here, in HARNESS space (the z reflection above
         * has already swapped the z bounds back into min/max order). */
        for (int k = 0; k < 3; k++) {
            m->bb_c[k] = (m->bb_max[k] + m->bb_min[k]) * 0.5f;
            m->bb_h[k] = (m->bb_max[k] - m->bb_min[k]) * 0.5f;
            if (m->bb_h[k] < 0.0f) m->bb_h[k] = -m->bb_h[k];
        }
    }

    for (unsigned i = 0; i < g_nvtx; i++) {
        const unsigned char* v = d + ov + (size_t)i * 0x20;
        float* o = g_vtx + i * 8;
        o[0] = rd_f32(v + 0);
        o[1] = rd_f32(v + 4);
        o[2] = -rd_f32(v + 8);        /* game -> harness Z reflection */
        o[3] = rd_f32(v + 12);
        o[4] = rd_f32(v + 16);
        o[5] = -rd_f32(v + 20);
        o[6] = rd_f32(v + 24);
        o[7] = rd_f32(v + 28);
    }
    for (unsigned i = 0; i < g_nidx; i++)
        g_idx[i] = (unsigned short)(d[ox + i * 2] | (d[ox + i * 2 + 1] << 8));

    for (int i = 0; i < g_ninst; i++) {
        const unsigned char* r = d + oi + (size_t)i * B3P_INST;
        B3PropInst* p = &g_inst[i];
        float m[16];
        for (int k = 0; k < 16; k++) m[k] = rd_f32(r + k * 4);
        /* The four `w` slots are the instance's baked half-range colour, not
         * transform (FUN_0011A020 saves/restores them at 0x0011A03A). Lift
         * them out and make the matrix affine. */
        p->tint[0] = m[3] * 2.0f;
        p->tint[1] = m[7] * 2.0f;
        p->tint[2] = m[11] * 2.0f;
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;
        /* game -> harness Z reflection of the whole transform, conjugated so
         * it composes with the reflected local vertices: negate the elements
         * with exactly one z index. */
        m[2] = -m[2]; m[6] = -m[6]; m[8] = -m[8]; m[9] = -m[9]; m[14] = -m[14];
        memcpy(p->base, m, sizeof m);
        memcpy(p->cur, m, sizeof m);
        p->model      = rd_u32(r + 0x40);
        p->prop_class = rd_u32(r + 0x44);
        p->unit       = rd_u32(r + 0x48);
        if ((int)p->model >= g_nmodel) p->model = 0;
        p->state = B3P_REST;
        p->body = -1;
        /* Broad-phase radius: sum_k |axis_k| * half_k bounds the OBB from its
         * centre whatever the rotation, and the instance matrices carry scale
         * (row lengths 1.000..1.516 on US_C1_V1), so it is measured off the
         * authored matrix rather than assumed to be 1.  A knocked prop's frame
         * is re-orthonormalised every step, which makes the axes unit -- so
         * the bound is also floored at sum(half) and stays valid however the
         * authored scale falls out. */
        const B3PropModel* mm = &g_model[p->model];
        float unit_r = 0.0f;
        p->bound_r = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float* ax = m + k * 4;
            p->bound_r += sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2])
                        * mm->bb_h[k];
            unit_r += mm->bb_h[k];
        }
        if (p->bound_r < unit_r) p->bound_r = unit_r;
    }
    free(d);

    for (int i = 0; i < g_nmodel; i++)
        g_model[i].tex = load_tex(g_dir, g_model[i].texture);

    /* RETAINED: the same baked world-space buffer the scenery pass uses.  No
     * distance cull is applied (the legacy pass has none either), so
     * `cull_far` is left at 0 = never culled. */
    b3r_init();
    if (b3r_active()) {
        B3RMeshSrc mesh;
        mesh.vtx = g_vtx; mesh.stride = 8; mesh.uv_off = 6;
        mesh.nvtx = g_nvtx; mesh.idx = g_idx; mesh.nidx = g_nidx;
        B3RModelSrc* ms = (B3RModelSrc*)calloc((size_t)g_nmodel,
                                               sizeof(B3RModelSrc));
        B3RInstSrc*  is = (B3RInstSrc*)calloc((size_t)g_ninst,
                                              sizeof(B3RInstSrc));
        g_retained_skip = (unsigned char*)calloc((size_t)g_ninst, 1);
        if (ms && is && g_retained_skip) {
            for (int i = 0; i < g_nmodel; i++) {
                ms[i].first_vertex = g_model[i].first_vertex;
                ms[i].n_vertex     = g_model[i].n_vertex;
                ms[i].first_index  = g_model[i].first_index;
                ms[i].n_index      = g_model[i].n_index;
                ms[i].tex          = g_model[i].tex;
                ms[i].mat_flags    = g_model[i].mat_flags;
            }
            for (int i = 0; i < g_ninst; i++) {
                is[i].m        = g_inst[i].base;
                is[i].tint     = g_inst[i].tint;
                is[i].model    = g_inst[i].model;
                is[i].cull_far = 0.0f;
            }
            g_retained = b3r_inst_build(&mesh, ms, g_nmodel, is, g_ninst);
        }
        free(ms);
        free(is);
    }

    /* Ground height under every prop, once -- the props sit on authored
     * ground and a knocked one has to come back down to it. */
    if (b3_collision_ready()) {
        for (int i = 0; i < g_ninst; i++) {
            B3PropInst* p = &g_inst[i];
            float h, n[3];
            if (b3_ground_probe(p->base[12], p->base[13] + 1.0f, p->base[14],
                                &h, n) >= 0) {
                p->ground_y = h;
                p->has_ground = 1;
            } else {
                p->ground_y = p->base[13];
            }
        }
    } else {
        for (int i = 0; i < g_ninst; i++) g_inst[i].ground_y = g_inst[i].base[13];
    }

    /* FUN_00119F40 @0x00119F83/0x00119F90: every pool body starts with
     * +0x220 = 0 (no owner) and +0x224 = -1.0 -- an EMPTY slot. */
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        /* the memset wiped rb.frame; re-point it at this slot's storage */
        b3_rigid_body_bind_frame(&g_body[i].rb, g_body[i].rb_frame_store);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_live = 0;
    g_clock = 0.0f;

    g_ready = 1;
    int cones = 0;
    for (int i = 0; i < g_ninst; i++)
        if (g_inst[i].prop_class == 1) cones++;
    int grounded = 0;
    for (int i = 0; i < g_ninst; i++) grounded += g_inst[i].has_ground;
    printf("props: %s -> %d models, %d instances (%d cones, %d on ground)\n",
           path, g_nmodel, g_ninst, cones, grounded);
    return g_ninst;
}

int b3_props_ready(void) { return g_ready; }
int b3_props_count(void) { return g_ninst; }
int b3_props_live(void) { return g_live; }

int b3_props_class_of(int i) {
    if (!g_ready || i < 0 || i >= g_ninst) return -1;
    return (int)g_inst[i].prop_class;
}
float b3_props_mass_of(int i) {
    if (!g_ready || i < 0 || i >= g_ninst) return 0.0f;
    return g_model[g_inst[i].model].mass;
}

/* The object class FUN_00112E70's port sees.  RECOVERED [C]: a static prop's
 * collision handle is type 5 (FUN_00110420 @0x00110A19) and a knocked one is
 * type 6 (FUN_00114730 @0x0011478B); FUN_0010FBC0's jump table @0x0010FC04
 * sends both to the class-6 arm @0x0010FBFC, and DAT_0039AE50 row 6 is all
 * zeros -- no prop of any size crashes any car.  That is the law, and it is
 * why you can plough a whole cone field without a scratch.
 *
 * GLUE (default OFF): B3_PROP_CRASH_KG=<kg> promotes props at or above that
 * recovered mass to class 2, i.e. makes them behave like a retail type-3 prop
 * ENTITY, whose row [2][0] = 1 does crash a racecar.  Retail ships no such
 * promotion for static.dat props; the knob exists so a future entity-backed
 * prop family can be switched on and measured. */
int b3_props_object_class(int i)
{
    static float heavy_kg = -1.0f;
    float m;
    if (heavy_kg < 0.0f) {
        const char* e = getenv("B3_PROP_CRASH_KG");
        heavy_kg = (e && *e) ? (float)atof(e) : 0.0f;
    }
    if (!g_ready || i < 0 || i >= g_ninst) return 6;
    m = g_model[g_inst[i].model].mass;
    if (heavy_kg > 0.0f && m >= heavy_kg) return 2;   /* GLUE: type-3 entity */
    return 6;                                        /* [C] type 5/6 -> 6   */
}

void b3_props_reset(void) {
    if (!g_ready) return;
    for (int i = 0; i < g_ninst; i++) {
        B3PropInst* p = &g_inst[i];
        memcpy(p->cur, p->base, sizeof p->cur);
        p->state = B3P_REST;
        p->body = -1;
        p->aud_swept = p->aud_hit = p->aud_rest = 0;
        p->aud_disp = p->aud_turn = 0.0f;
    }
    memset(g_aud_have, 0, sizeof g_aud_have);
    g_aud_next_report = 0.0f;
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        memset(&g_body[i], 0, sizeof g_body[i]);
        /* the memset wiped rb.frame; re-point it at this slot's storage */
        b3_rigid_body_bind_frame(&g_body[i].rb, g_body[i].rb_frame_store);
        g_body[i].owner = -1;
        g_body[i].lru_key = -1.0f;
    }
    g_live = 0;
    g_clock = 0.0f;
    g_obb_arm[0] = g_obb_arm[1] = g_obb_arm[2] = 0;
    g_lost = 0;
    g_creep = 0.0f;
}

/* ---- knock ------------------------------------------------------------ */
/* FUN_00109BB0 @0x00109BB9..0x00109CC8 -> FUN_00109190 [C]: the body inverse
 * inertia is the diagonal built from the bbox half-extents and the mass. */
static void b3p_set_inertia(B3RigidBody* rb, const float bbmax[4],
                            const float bbmin[4], float mass) {
    float e[3];
    for (int k = 0; k < 3; k++) {
        float a = bbmax[k], b = -bbmin[k];
        e[k] = a > b ? a : b;
    }
    float d[3];
    d[0] = 1.0f / ((e[1]*e[1] + e[2]*e[2]) * mass * 0.5f);
    d[1] = 1.0f / ((e[0]*e[0] + e[2]*e[2]) * mass * 0.5f);
    d[2] = 1.0f / ((e[0]*e[0] + e[1]*e[1]) * mass * 0.5f);
    memset(rb->inv_inertia_body, 0, sizeof rb->inv_inertia_body);
    for (int k = 0; k < 3; k++) rb->inv_inertia_body[k][k] = d[k];
    /* FUN_00109190's tail: the WORLD inverse inertia is R^t . I0 . R, built
     * immediately so the contact resolved in the same frame already has it
     * (FUN_00113890 promotes @0x0011393B then resolves @0x0011394E). */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            rb->inv_inertia_world[i][j] =
                rb->frame[0][i] * d[0] * rb->frame[0][j]
              + rb->frame[1][i] * d[1] * rb->frame[1][j]
              + rb->frame[2][i] * d[2] * rb->frame[2][j];
    for (int i = 0; i < 3; i++) rb->inv_inertia_world[i][3] = 0.0f;
}

/* FUN_00114730 [C]: first a free slot in the 16-body pool (the bitmask scan
 * @0x00114750..0x0011476D), otherwise recycle the body with the SMALLEST
 * +0x224 (@0x001147A0..0x001147C6) -- the oldest.  The recycled prop's world
 * slot is retyped to 8 @0x0011480C, and the dispatcher drops type-8 pairs
 * @0x00111D0B, so it stops colliding: B3P_SETTLED here. */
static int b3p_acquire(int self)
{
    for (int i = 0; i < B3P_MAX_LIVE; i++)
        if (g_body[i].owner < 0) { g_live++; return i; }
    int victim = -1;
    float best = 999999.0f;                    /* FUN_00114730's seed value */
    for (int i = 0; i < B3P_MAX_LIVE; i++) {
        if (g_body[i].owner == self) continue;
        if (g_body[i].lru_key < best) { best = g_body[i].lru_key; victim = i; }
    }
    if (victim < 0) return -1;
    int prev = g_body[victim].owner;
    if (prev >= 0 && prev < g_ninst) {
        g_inst[prev].state = B3P_SETTLED;      /* world slot -> type 8 */
        g_inst[prev].body = -1;
        g_inst[prev].aud_rest = 1;             /* retired: retail's other stop */
    }
    g_body[victim].owner = -1;
    return victim;
}

/* FUN_0011A020 @0x0011A0A5..0x0011A317 [C]: everything the body setup derives
 * from the model bbox, over an already-posed frame. */
static void b3p_body_setup(B3RigidBody* rb, const float bbmax[4],
                           const float bbmin[4], float* out_mass,
                           float* out_com, float* out_radius)
{
    /* +0x1CC = |bbmax|  @0x0011A0DE */
    float radius = sqrtf(bbmax[0]*bbmax[0] + bbmax[1]*bbmax[1]
                       + bbmax[2]*bbmax[2]);
    /* +0x1F0 = max(100, (bbmax.x-bbmin.x)*(bbmax.z-bbmin.z)*200) @0x0011A137
     * -- MAXSS @0x0011A17D, so the 100 wins on a NaN/tie the same way. */
    float dx = bbmax[0] - bbmin[0];
    float dz = bbmax[2] - bbmin[2];
    float area = dz * dx * B3P_MASS_PER_M2;
    float mass = area > B3P_MASS_MIN ? area : B3P_MASS_MIN;

    b3p_set_inertia(rb, bbmax, bbmin, mass);
    /* +0x1F4 = bbmax.y - (bbmax.y - bbmin.y)*0.5  @0x0011A2F8..0x0011A317 */
    float com = bbmax[1] - (bbmax[1] - bbmin[1]) * 0.5f;

    /* THE LAUNCH, @0x0011A1DD..0x0011A2F3 -> FUN_000FFC80.  Four PRNG draws;
     * the y term is (u*0.5), never negative, so a knocked prop always pops up.
     * FUN_000FFC80 SETS the velocity (it does not add), then refreshes +0xBC
     * speed and +0xC0 unit direction. */
    float r0 = (float)(b3p_rand01() - B3P_LAUNCH_HALF);
    float r1 = (float)(b3p_rand01() * B3P_LAUNCH_HALF);
    float r2 = (float)(b3p_rand01() - B3P_LAUNCH_HALF);
    float s  = (float)((b3p_rand01() + B3P_LAUNCH_ONE) * B3P_LAUNCH_SCALE);
    rb->vel[0] = r0 * s;
    rb->vel[1] = r1 * s;
    rb->vel[2] = r2 * s;
    float l2 = v_dot3(rb->vel, rb->vel);
    if (l2 < B3P_EPS2) {
        rb->vel[3] = 0.0f;
        for (int k = 0; k < 4; k++) rb->dir[k] = rb->frame[2][k];
    } else {
        float l = sqrtf(l2);
        rb->vel[3] = l;
        for (int k = 0; k < 3; k++) rb->dir[k] = rb->vel[k] / l;
        rb->dir[3] = 0.0f;
    }
    if (out_mass) *out_mass = mass;
    if (out_com) *out_com = com;
    if (out_radius) *out_radius = radius;
}

/* FUN_0011A330 [C] minus the harness ground stop: the two quadratic drag
 * terms, FUN_00109560, and the matrix-w restore @0x0011A434. */
static void b3p_body_step(B3RigidBody* rb, float mass, float com, float dt,
                          int frozen)
{
    /* force += dir * -(speed^2)          @0x0011A370, [0x003B16C0] */
    float f = B3P_LIN_DRAG * rb->vel[3] * rb->vel[3];
    for (int k = 0; k < 4; k++) rb->force_acc[k] += rb->dir[k] * f;
    /* torque += omega * (|omega| * -2)   @0x0011A3B4, [0x003B17F8] */
    float wl = sqrtf(v_dot3(rb->omega, rb->omega)) * B3P_ANG_DRAG;
    for (int k = 0; k < 4; k++) rb->torque_acc[k] += rb->omega[k] * wl;
    /* in_race = body+0x210 = 0, state6 = (body+0x215 == 6) = 1, so gravity is
     * applied at pos + up*com_height and therefore tumbles the prop. */
    b3p_integrate(rb, mass, com, dt, frozen);
    rb->frame[0][3] = rb->frame[1][3] = rb->frame[2][3] = 0.0f;
    rb->frame[3][3] = 1.0f;
}

/* FUN_00113960's arm for (car A, prop B) at a resolved contact [C].  Returns
 * |j|; `car_rb` is only written when the car is crashed (role != 2). */
static float b3p_contact(B3RigidBody* prb, float pmass,
                         B3RigidBody* car_rb, float cmass,
                         const float cp[4], const float n_in[4],
                         int car_crashed, float out_n[4], float out_imp[4])
{
    /* @0x00113B57: an un-crashed car (handle type 0/1/2) is FORCED to role 2
     * -- immovable -- so it takes no reaction at all. */
    int kindA = car_crashed ? 0 : 2;

    /* point velocities and v_rel = v_prop - v_car   @0x00113E9C */
    float vpa[4], vpb[4], vrel[4], n[4];
    b3p_point_vel(car_rb, cp, vpa);
    b3p_point_vel(prb, cp, vpb);
    for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];
    for (int k = 0; k < 4; k++) n[k] = n_in[k];
    n[3] = 0.0f;
    /* @0x00113EFA..0x00113F49: bend the normal toward v_rel by -0.9 and
     * re-normalise, skipped when |v_rel|^2 < 2^-32 (FUN_0003B060). */
    if (v_dot3(vrel, vrel) >= B3P_EPS2) {
        float u[4] = { vrel[0], vrel[1], vrel[2], vrel[3] };
        float ul = sqrtf(v_dot3(u, u));
        for (int k = 0; k < 3; k++) u[k] /= ul;
        for (int k = 0; k < 3; k++) n[k] += u[k] * B3P_NORM_BLEND;
        float nl = sqrtf(v_dot3(n, n));
        if (nl < 1e-20f) { if (out_n) memcpy(out_n, n, sizeof n); return 0.0f; }
        for (int k = 0; k < 3; k++) n[k] /= nl;
    }
    if (out_n) memcpy(out_n, n, 4 * sizeof(float));

    /* @0x00113F78 FUN_0010F8D0 -- both masses and both inertias are in the
     * denominator even though only the prop moves. */
    float imp[4];
    float j = b3p_mutual_impulse(prb, pmass, car_rb, cmass,
                                 cp, cp, vrel, n, B3P_RESTITUTION, imp);
    if (out_imp) memcpy(out_imp, imp, sizeof imp);
    /* @0x00113F91: nothing is applied unless j > 0 */
    if (j > 0.0f) {
        float neg[4] = { -imp[0], -imp[1], -imp[2], -imp[3] };
        /* @0x00113FAD kindA == 2: the prop takes -J and the car nothing */
        b3p_impulse_at(prb, neg, cp);
        if (kindA != 2) b3p_impulse_at(car_rb, imp, cp);
    }
    return j;
}

/* FUN_0011A020 [C]: hand a fresh class-6 body to prop `inst`. */
static int b3p_promote(int inst)
{
    B3PropInst* p = &g_inst[inst];
    const B3PropModel* m = &g_model[p->model];
    int slot = b3p_acquire(inst);
    if (slot < 0) return -1;
    B3PropBody* b = &g_body[slot];
    memset(b, 0, sizeof *b);
    b3_rigid_body_bind_frame(&b->rb, b->rb_frame_store);
    b->owner = inst;

    /* +0x204 -> the instance matrix itself (@0x0011A062) */
    mat_to_frame(p->cur, b->rb.frame);
    b->rb.frame[0][3] = b->rb.frame[1][3] = b->rb.frame[2][3] = 0.0f;
    b->rb.frame[3][3] = 1.0f;

    b3p_build_inv_frame(&b->rb);

    float bbmax[4] = { m->bb_max[0], m->bb_max[1], m->bb_max[2], 0.0f };
    float bbmin[4] = { m->bb_min[0], m->bb_min[1], m->bb_min[2], 0.0f };
    /* +0x1D0 = bbmax, +0x1E0 = bbmin  @0x0011A0A8/@0x0011A0AF -- the OBB the
     * narrow phase FUN_00107950 clips the surface polygons against. */
    for (int k = 0; k < 3; k++) { b->bbmax[k] = bbmax[k]; b->bbmin[k] = bbmin[k]; }
    b3p_body_setup(&b->rb, bbmax, bbmin, &b->mass, &b->com_height, &b->radius);
    /* +0x224 = clock + 10  @0x0011A19E */
    b->lru_key = g_clock + B3P_LRU_OFFSET;

    p->body = (short)slot;
    p->state = B3P_KNOCKED;
    if (getenv("B3_PROP_TRACE"))
        printf("[propknock] t=%.2f inst %d -> body %d mass %.0f com %.2f "
               "launch (%.2f %.2f %.2f) live %d\n", g_clock, inst, slot,
               b->mass, b->com_height, b->rb.vel[0], b->rb.vel[1],
               b->rb.vel[2], g_live);
    return slot;
}

/* ---- differential test surface (see burnout3_props.h) ------------------- */
void b3_props_test_body_setup(const float frame[4][4], const float bbmax[4],
                              const float bbmin[4], unsigned rng_state,
                              unsigned rng_inc, B3RigidBody* out_rb,
                              float* out_mass, float* out_com_height,
                              float* out_radius) {
    {   /* the caller owns this body's frame storage; the memset would drop
     * the binding, so carry the pointer across it */
    float (*keep_frame)[4] = out_rb->frame;
    memset(out_rb, 0, sizeof *out_rb);
    out_rb->frame = keep_frame; }
    memcpy(out_rb->frame, frame, 16 * sizeof(float));
    unsigned ss = g_rng_state, si = g_rng_inc;
    b3_props_seed(rng_state, rng_inc);
    b3p_body_setup(out_rb, bbmax, bbmin, out_mass, out_com_height, out_radius);
    b3_props_seed(ss, si);
}
void b3_props_test_body_step(B3RigidBody* rb, float mass, float com_height,
                             float dt) {
    /* the differential cases all seed a body with the latch DOWN, which is
     * the arm FUN_0011A330 takes for a body in flight */
    b3p_body_step(rb, mass, com_height, dt, 0);
}
float b3_props_test_contact(B3RigidBody* prop_rb, float prop_mass,
                            const B3RigidBody* car_rb, float car_mass,
                            const float point[4], const float normal[4],
                            float out_normal[4], float out_imp[4]) {
    /* By-value copy: with the frame no longer inline, `car.frame` would alias
     * the caller's matrix and b3p_contact could write through it. Give the
     * copy its own storage and copy the rows. */
    B3RigidBody car = *car_rb;
    float car__frame_store[4][4];
    /* bind BEFORE the copy: binding zeroes the storage, so filling it first
     * would be undone. */
    b3_rigid_body_bind_frame(&car, car__frame_store);
    memcpy(car__frame_store, car_rb->frame, 16 * sizeof(float));
    return b3p_contact(prop_rb, prop_mass, &car, car_mass, point, normal, 0,
                       out_normal, out_imp);
}

/* FUN_0011A330 [C], the class-6 body vtable's slot +0x00 (vtable 0x003B1120),
 * driven once per allocated body per frame by the collision manager
 * FUN_00110AF0 @0x00110FB4 with the frame dt.  The WHOLE update. */
void b3_props_update(float dt) {
    b3p_bind_body_frames();
    if (!g_ready || dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    g_clock += dt;
    b3p_audit_report();

    static int ballistic = -1;
    if (ballistic < 0) {
        const char* e = getenv("B3_PROP_BALLISTIC");
        ballistic = (e && *e && *e != '0') ? 1 : 0;
    }

    for (int s = 0; s < B3P_MAX_LIVE; s++) {
        B3PropBody* b = &g_body[s];
        if (b->owner < 0) continue;
        B3PropInst* p = &g_inst[b->owner];
        B3RigidBody* rb = &b->rb;

        /* ---- vtable slot +0x10, FUN_0011A490 -> FUN_00109EA0 -------------
         * Retail's collision manager runs this BEFORE the class's own update
         * slot, so the contact impulse lands in +0x110/+0x120 and the
         * push-out in +0x130 and the SAME frame's FUN_00109560 consumes all
         * three.  Doing it the other way round (what this file used to do)
         * costs a frame of lag and lets the body integrate through the
         * surface first, which is exactly the reported clipping. The same
         * gathered collision soup feeds live cars and wrecks: a knocked prop
         * must not be reduced to a single downward ground plane. */
        /* "did anything touch this body this frame" -- a pair contact from
         * last frame's collide pass, or a world contact from the pass below */
        int touched = b->hit_211 ? 1 : 0;
        if (!ballistic) {
            int surf = -1;
            int probed = -1;       /* the raw ground-probe verdict */
            int hit = 0;
            int slept = 0;
            int nsoup = 0;
            float h = 0.0f, nn[3] = {0.0f, 1.0f, 0.0f};
            if (b3_collision_ready()) {
                /* 96, the same cap the chassis gather uses.  At 32 this
                 * TRUNCATED on real track soup -- a barrier board crossing a
                 * dense patch at 41 m/s saw a full 32-entry list that did not
                 * happen to contain the floor under it, passed through the
                 * road in one frame, and was never caught again (the next
                 * frame's y-band no longer reached the surface, soup went to
                 * 0, and it fell to the drag terminal velocity of 61.7 m/s
                 * until the 16-body pool recycled it 20 s later). */
                B3CollisionPoly soup[96];
                float half[3];
                float velocity[3] = {rb->vel[0], rb->vel[1], rb->vel[2]};
                /* ONE FRAME OF TRAVEL, retail's own broad-phase margin:
                 * FUN_0011BC60 sizes its query as |box| + speed * dt
                 * (@0x0011BC7A the speed*dt, @0x0011BCD9/@0x0011BD17 the
                 * norm) rather than the box alone, which is what stops a fast
                 * body tunnelling between two discrete frames.  Without it a
                 * prop moving 0.68 m per tick could step clean through a
                 * surface its resting box would have overlapped. */
                const float travel = rb->vel[3] * dt;
                for (int axis = 0; axis < 3; axis++) {
                    float lo = fabsf(b->bbmin[axis]);
                    float hi = fabsf(b->bbmax[axis]);
                    half[axis] = (lo > hi ? lo : hi) + 0.5f + travel;
                }
                nsoup = b3_collision_gather_walls(rb->frame[3], half,
                                                   velocity, 1.1f, soup,
                                                   (int)(sizeof(soup)
                                                         / sizeof(soup[0])));
                /* ONE narrow phase over the WHOLE soup, ONE resolve -- which
                 * is what retail does: FUN_0011A490 calls FUN_00109EA0 exactly
                 * once @0x0011A706, over the single soup FUN_00109D20 gathered
                 * @0x0011A5FB.  This loop used to call the SINGLE-PLANE form
                 * once per polygon with `soup[poly].v0` as the plane point --
                 * the very pair of defects burnout3_full.c's wreck path had
                 * already found and fixed (see the note at its
                 * b3_wreck_world_contact_soup call):
                 *
                 *  (a) the plane form has no polygon, so it fabricates a
                 *      square of half-size |box dims| + 1 centred on the point
                 *      it is handed.  A triangle's FIRST VERTEX is not under
                 *      the prop, so a large road face produced NO CONTACT AT
                 *      ALL -- a cone at rest 0.15 m above the road was given
                 *      19 candidate polygons and zero contacts, fell through
                 *      the surface, and once below it the gather's own y-band
                 *      no longer reached the road (soup went to 0), so nothing
                 *      could ever catch it again: it accelerated to the
                 *      terminal velocity of the -1.0 quadratic drag, 44.7 m/s,
                 *      and the audit's "max travel" was 900 m of FALL.
                 *  (b) resolving per polygon applies N impulses and N
                 *      push-outs in a frame where retail applies one, which
                 *      levitated resting props 0.5..1.3 m into the air and
                 *      kept them jittering there instead of settling.
                 *
                 * b3_rigid_body_obb_soup_contact is FUN_00107950 as written:
                 * it clips the real triangles, sums the clipping faces'
                 * normals and averages their centroids into one contact. */
                B3WorldPoly wp[(int)(sizeof(soup) / sizeof(soup[0]))];
                for (int poly = 0; poly < nsoup; poly++) {
                    memcpy(wp[poly].v[0], soup[poly].v0, sizeof wp[poly].v[0]);
                    memcpy(wp[poly].v[1], soup[poly].v1, sizeof wp[poly].v[1]);
                    memcpy(wp[poly].v[2], soup[poly].v2, sizeof wp[poly].v[2]);
                    memcpy(wp[poly].n,    soup[poly].normal, sizeof wp[poly].n);
                }
                B3WorldContact ct;
                B3WorldContactResult res;
                if (nsoup > 0
                    && b3_rigid_body_obb_soup_contact(rb, b->bbmin, b->bbmax,
                                                      wp, nsoup, &ct)) {
                    b3_rigid_body_world_contact(rb, b->mass, 6, 0,
                                                B3P_WORLD_RESTITUTION, &ct,
                                                &res);
                    hit = 1;
                    if (res.sleep) slept = 1;
                }
                // Telemetry only; collision response above already used the
                // complete gathered soup.
                surf = b3_ground_probe(rb->frame[3][0],
                                       rb->frame[3][1] + b->radius + 1.0f,
                                       rb->frame[3][2], &h, nn);
                probed = surf;      /* before the fallback below rewrites it */
            }
            /* GLUE anti-tunnelling net.  Retail has only the soup; this is
             * the harness's floor of last resort, and it fires whenever the
             * soup produced NO contact -- not, as it did before, only when
             * the ground PROBE also failed.  That guard was backwards: the
             * case it locked itself out of is precisely the one that matters,
             * a prop over known ground that the (then broken) narrow phase
             * refused, which is how a resting cone fell through the road.
             * The PROBED height is preferred over the authored one because
             * `ground_y` is sampled under the prop's AUTHORED position and is
             * meaningless once it has been knocked anywhere. */
            if (!hit) {
                float gy = 0.0f;
                int have = 0;
                float gn[3] = {0.0f, 1.0f, 0.0f};
                if (surf >= 0) {
                    gy = h; have = 1;
                    gn[0] = nn[0]; gn[1] = nn[1]; gn[2] = nn[2];
                } else if (p->has_ground) {
                    gy = p->ground_y; have = 1;
                }
                if (have) {
                    const float ppt[3] = {rb->frame[3][0], gy,
                                           rb->frame[3][2]};
                    B3WorldContact ct;
                    B3WorldContactResult res;
                    if (b3_rigid_body_obb_plane_contact(rb, b->bbmin,
                                                         b->bbmax, ppt, gn,
                                                         &ct)) {
                        b3_rigid_body_world_contact(rb, b->mass, 6, 0,
                                                    B3P_WORLD_RESTITUTION,
                                                    &ct, &res);
                        hit = 1;
                        slept = res.sleep;
                    }
                    h = gy;
                    surf = 0;
                }
            }
            /* @0x00109592..0x001095C1: the sleep latch is cleared ONLY when
             * +0x211 says a rigid PAIR contact touched this body -- so a car
             * ploughing into a settled prop wakes it, and nothing else does. */
            if (b->hit_211) b->frozen = 0;
            if (slept) b->frozen = 1;
            if (hit) touched = 1;

            /* OUT OF THE WORLD.  FUN_0011A490 runs its whole gather/resolve
             * only while the body is inside a LOADED STREAMING UNIT
             * (+0x216 != 0xFF); when it is not, retail clears the body's
             * accumulators instead (@0x0011A6D5) and the body stops being
             * driven by contacts at all.  The harness has no streaming units,
             * so the equivalent test is "no soup and no surface anywhere near
             * it": a prop that has left the track through a seam the narrow
             * phase missed.  Retiring it hands its slot straight back to the
             * 16-body pool, which is where retail's own recycle
             * (FUN_00114730 @0x0011480C, world slot -> type 8) would have put
             * it anyway -- and it is the difference between a cone that is
             * simply gone and one that accelerates to the drag terminal
             * velocity for twenty seconds while holding a body hostage.
             * GLUE, and the only thing left standing between a soup miss and
             * an unbounded fall. */
            if (nsoup == 0 && probed < 0 && p->has_ground
                && rb->frame[3][1] < p->ground_y - B3P_LOST_BELOW) {
                p->state = B3P_SETTLED;
                p->body = -1;
                p->aud_rest = 1;
                b->owner = -1;
                g_lost++;
                if (g_live > 0) g_live--;
                if (getenv("B3_PROP_TRACE"))
                    printf("[proplost] t=%.2f inst %d fell %.1f m below its "
                           "ground -- body %d retired\n", g_clock, (int)(p - g_inst),
                           p->ground_y - rb->frame[3][1], s);
                continue;
            }
            if (getenv("B3_PROP_TRACE")) {
                static float next_at[B3P_MAX_LIVE];
                /* B3_PROP_TRACE_NEAR: every frame while the body is within
                 * a few metres of the surface, which is the window a
                 * tunnelling defect has to be caught in. */
                int near = getenv("B3_PROP_TRACE_NEAR") && surf >= 0
                        && fabsf(rb->frame[3][1] - h) < 3.0f;
                if (near || g_clock >= next_at[s]) {
                    if (!near) next_at[s] = g_clock + 1.0f;
                    printf("[propgnd] t=%.1f body %d inst %d pos=(%.2f %.3f "
                           "%.2f) ground=%.3f dy=%+.3f soup=%d hit=%d "
                           "|v|=%.2f frozen=%d\n",
                           g_clock, s, b->owner, rb->frame[3][0],
                           rb->frame[3][1], rb->frame[3][2],
                           surf >= 0 ? h : -999.0f,
                           surf >= 0 ? rb->frame[3][1] - h : 0.0f,
                           nsoup, hit, rb->vel[3], b->frozen);
                }
            }
        }

        b3p_body_step(rb, b->mass, b->com_height, dt, b->frozen);
        /* after the step, so this reads what the latch actually left behind,
         * and only for a body NOTHING touched this frame -- see the note on
         * g_creep. */
        if (b->frozen && !touched && rb->vel[3] > g_creep)
            g_creep = rb->vel[3];
        b->hit_211 = 0;        /* consumed, like retail's per-frame scratch */

        /* @0x0011A434..0x0011A45B: the four matrix w slots are restored after
         * the integrator -- they carry the instance colour, not transform. */
        rb->frame[0][3] = rb->frame[1][3] = rb->frame[2][3] = 0.0f;
        rb->frame[3][3] = 1.0f;
        frame_to_mat(rb->frame, p->cur);

        if (g_audit) {
            float dx = p->cur[12] - p->base[12];
            float dy = p->cur[13] - p->base[13];
            float dz = p->cur[14] - p->base[14];
            float dd = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dd > p->aud_disp) p->aud_disp = dd;
            /* the authored up axis is row 1 of the authored matrix; both are
             * unit after b3p_orthonormalize, so the dot is the cosine */
            float t = 1.0f - (p->cur[4]*p->base[4] + p->cur[5]*p->base[5]
                            + p->cur[6]*p->base[6]);
            if (t > p->aud_turn) p->aud_turn = t;
            if (b->frozen) p->aud_rest = 1;
        }
    }
}

int b3_props_body_state(int instance, B3RigidBody* out_rb, float* out_mass,
                        float* out_com_height, float* out_lru_key) {
    if (!g_ready || instance < 0 || instance >= g_ninst) return 0;
    int s = g_inst[instance].body;
    if (s < 0 || s >= B3P_MAX_LIVE || g_body[s].owner != instance) return 0;
    if (out_rb) *out_rb = g_body[s].rb;
    if (out_mass) *out_mass = g_body[s].mass;
    if (out_com_height) *out_com_height = g_body[s].com_height;
    if (out_lru_key) *out_lru_key = g_body[s].lru_key;
    return 1;
}

/* =========================================================================
 * FUN_001084E0 [C] -- retail's "may I knock it" gate, @0x001084EF..0x00108C71.
 *
 * FUN_00113890 (the dispatcher's prop arm) calls this at @0x00113901 and it is
 * the WHOLE admission test: no mass veto, no size veto and -- the part that
 * matters here -- NO VELOCITY TEST.  Re-read out of build/burnout3.elf with a
 * capstone sweep over the PT_LOAD segments (the Ghidra bridge was refusing
 * connections); the earlier record described it only as "reads both bbox rows
 * and halves their sum", which is the first four instructions of it.
 *
 * It is a fifteen-axis SEPARATING-AXIS test between two ORIENTED BOXES.
 * There is no sphere anywhere in it.
 *
 *   @0x001084EF  box A comes in as EDX -> {bbox MAX at +0x00, bbox MIN at
 *                +0x10} and EAX -> a 4x4 (axis rows +0x00/+0x10/+0x20,
 *                translation +0x30).
 *   @0x0010851B  centre  = (MAX + MIN) * [0x003B1684] (= 0.5)
 *   @0x00108541  the centre is pushed through the 4x4:
 *                world_c = row3 + row0*c.x + row1*c.y + row2*c.z
 *   @0x001085B4  half extents = (MAX - MIN) * 0.5
 *   @0x001085CE  the same two for box B (ECX -> its bbox pair)
 *   @0x0010869C..@0x00108AFA
 *                FIFTEEN calls to FUN_00107FD0, one per candidate axis, each
 *                followed by `TEST AL,AL / JNE 0x00108DEE`: A's three rows
 *                ([0x0040A5B0] = (1,0,0), [0x0040A5C0] = (0,1,0),
 *                [0x0040A5D0] = (0,0,1) mapped through A's frame), B's three
 *                rows, and the nine edge crosses built by FUN_000328F0.
 *                ANY separating axis -> no contact at all.
 *
 *   FUN_00107FD0 @0x00107FD0: projects both boxes with FUN_00107E90, returns
 *                AL = 1 @0x00108076 when either `a.lo > b.hi` @0x0010800F or
 *                `b.lo > a.hi` @0x00108020, otherwise writes
 *                min(a.hi,b.hi) - max(a.lo,b.lo) @0x0010805D and returns 0.
 *   FUN_00107E90 @0x00107E9A: interval = [c - r, c + r] with c = axis . centre
 *                and r = sum_k |axis . row_k| * half_k.  The axes are used
 *                UNNORMALISED -- prop instance matrices carry scale -- which
 *                is exactly why
 *   @0x00108B4E  each axis's overlap is divided by |axis| (FUN_0002C0D0)
 *                before the comparison, and the axis of MINIMUM normalised
 *                overlap wins (seed [0x003B172C] = FLT_MAX @0x00108B15,
 *                degenerate crosses skipped by FUN_0003B060's zero test
 *                @0x00108B32).  No axis chosen -> no contact @0x00108B7A.
 *   @0x00108BB7  the winning axis is scaled by [0x003B16C0] (= -1) when
 *                dot(axis, centreB - centreA) > [0x003B16E0] (= 0), so the
 *                normal handed back always points from B toward A.
 *   @0x00108BE3  contact point = the support point of the opposing box along
 *                -normal (FUN_00108080, whose per-axis eps is [0x003B16D0] =
 *                0.001 and which reads the half extents at +0x60/+0x64/+0x68),
 *                pulled back by the penetration @0x00108C39.  Outputs are the
 *                depth [ebp+0x08], the point [ebp+0x0C] and the normal
 *                [ebp+0x10], with AL = 1.
 *
 * DEVIATION, marked: retail hands back the winning axis UNNORMALISED
 * (FUN_000116A0 @0x00108C64 is a bare 16-byte copy, not a normalise) while the
 * depth it pairs with it IS normalised @0x00108B4E, so retail's own point
 * pull-back is off by |axis| (1.0..1.5 for a face axis, <= 1 for a cross).
 * FUN_00113960 re-normalises the normal anyway at its bend @0x00113F49, so
 * this port normalises here and keeps retail's depth -- the geometry the
 * separation push-out needs.
 * ====================================================================== */
typedef struct {
    float c[3];        /* world centre                          (desc +0x50) */
    float ax[3][3];    /* the three world axes, SCALE INCLUDED  (+0x20/30/40) */
    float h[3];        /* local half extents                    (desc +0x60) */
} B3POBB;

/* FUN_00107E90 [C]: project a box onto `n` (which need not be unit). */
static void b3p_obb_project(const B3POBB* b, const float n[3],
                            float* lo, float* hi)
{
    float c = n[0] * b->c[0] + n[1] * b->c[1] + n[2] * b->c[2];
    float r = 0.0f;
    for (int k = 0; k < 3; k++) {
        float d = n[0] * b->ax[k][0] + n[1] * b->ax[k][1] + n[2] * b->ax[k][2];
        r += fabsf(d) * b->h[k];
    }
    *lo = c - r;
    *hi = c + r;
}

/* FUN_00107FD0 [C]: 1 = this axis SEPARATES them, 0 = they overlap by
 * `*out_overlap` along it. */
static int b3p_axis_separates(const B3POBB* a, const B3POBB* b,
                              const float n[3], float* out_overlap)
{
    float alo, ahi, blo, bhi;
    b3p_obb_project(a, n, &alo, &ahi);
    b3p_obb_project(b, n, &blo, &bhi);
    if (alo > bhi) return 1;              /* @0x0010800F */
    if (blo > ahi) return 1;              /* @0x00108020 */
    *out_overlap = (ahi < bhi ? ahi : bhi) - (alo > blo ? alo : blo);
    return 0;
}

/* FUN_00108080 [C]: the support point of a box along `n`. */
static void b3p_obb_support(const B3POBB* b, const float n[3], float out[3])
{
    for (int i = 0; i < 3; i++) out[i] = b->c[i];
    for (int k = 0; k < 3; k++) {
        float d = n[0] * b->ax[k][0] + n[1] * b->ax[k][1] + n[2] * b->ax[k][2];
        float s = 0.0f;
        if (d > 0.001f) s =  b->h[k];     /* [0x003B16D0] */
        else if (d < -0.001f) s = -b->h[k];
        for (int i = 0; i < 3; i++) out[i] += b->ax[k][i] * s;
    }
}

/* FUN_00108240 [C]: edge-edge closest point between boxes A and B along
 * edge axes `eA` and `eB`.
 * Given edge directions a = A->ax[ia] and b = B->ax[ib], and contact normal `n`,
 * finds the point on the contacting edges.
 * If the edges are nearly parallel (|cross(a, b)| < 1e-4), falls back to the
 * support midpoint. Otherwise computes the segment-segment closest point. */
static void b3p_edge_edge_closest(const B3POBB* A, const B3POBB* B,
                                  int ia, int ib, const float n[3],
                                  float out_pt[3])
{
    float a[3], b_dir[3];
    for (int i = 0; i < 3; i++) {
        a[i] = A->ax[ia][i];
        b_dir[i] = B->ax[ib][i];
    }

    /* Support points along n on A and -n on B to select the contacting edge */
    float pA[3], pB[3], nneg[3];
    for (int i = 0; i < 3; i++) nneg[i] = -n[i];
    b3p_obb_support(A, n, pA);
    b3p_obb_support(B, nneg, pB);

    /* Check edge direction alignment: cross(a, b) */
    float u[3];
    u[0] = a[1]*b_dir[2] - a[2]*b_dir[1];
    u[1] = a[2]*b_dir[0] - a[0]*b_dir[2];
    u[2] = a[0]*b_dir[1] - a[1]*b_dir[0];
    float u_len = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (u_len < 1e-4f) {
        /* Parallel edges: midpoint of support points [0x00108DE7 fallback] */
        for (int i = 0; i < 3; i++) out_pt[i] = (pA[i] + pB[i]) * 0.5f;
        return;
    }

    /* Direction dot products */
    float da = a[0]*a[0] + a[1]*a[1] + a[2]*a[2];
    float db = b_dir[0]*b_dir[0] + b_dir[1]*b_dir[1] + b_dir[2]*b_dir[2];
    float dab = a[0]*b_dir[0] + a[1]*b_dir[1] + a[2]*b_dir[2];

    float r[3];
    for (int i = 0; i < 3; i++) r[i] = pA[i] - pB[i];
    float ra = r[0]*a[0] + r[1]*a[1] + r[2]*a[2];
    float rb = r[0]*b_dir[0] + r[1]*b_dir[1] + r[2]*b_dir[2];

    float denom = da * db - dab * dab;
    float s = 0.0f, t = 0.0f;
    if (fabsf(denom) > 1e-6f) {
        s = (dab * rb - db * ra) / denom;
        t = (da * rb - dab * ra) / denom;
    }

    /* Clamp s to edge A half-extent [-h[ia], h[ia]] */
    if (s < -A->h[ia]) s = -A->h[ia];
    if (s >  A->h[ia]) s =  A->h[ia];

    /* Clamp t to edge B half-extent [-h[ib], h[ib]] */
    if (t < -B->h[ib]) t = -B->h[ib];
    if (t >  B->h[ib]) t =  B->h[ib];

    /* Closest points on each segment, and resulting contact point */
    for (int i = 0; i < 3; i++) {
        float ptA = pA[i] + a[i] * s;
        float ptB = pB[i] + b_dir[i] * t;
        out_pt[i] = (ptA + ptB) * 0.5f;
    }
}

/* FUN_001084E0 [C].  `out_n` is UNIT and oriented A -> B (retail returns
 * B -> A @0x00108BB7; the caller here wants "away from the car", which is the
 * direction the solver's own impulse `n * j` then pushes the prop). */
static int b3p_obb_contact(const B3POBB* A, const B3POBB* B,
                           float out_n[3], float* out_depth, float out_pt[3])
{
    float axes[15][3], ov[15];
    int na = 0;
    for (int k = 0; k < 3; k++, na++)
        for (int i = 0; i < 3; i++) axes[na][i] = A->ax[k][i];
    for (int k = 0; k < 3; k++, na++)
        for (int i = 0; i < 3; i++) axes[na][i] = B->ax[k][i];
    for (int p = 0; p < 3; p++)
        for (int q = 0; q < 3; q++, na++) {   /* FUN_000328F0, the 9 crosses */
            axes[na][0] = A->ax[p][1]*B->ax[q][2] - A->ax[p][2]*B->ax[q][1];
            axes[na][1] = A->ax[p][2]*B->ax[q][0] - A->ax[p][0]*B->ax[q][2];
            axes[na][2] = A->ax[p][0]*B->ax[q][1] - A->ax[p][1]*B->ax[q][0];
        }
    for (int i = 0; i < 15; i++) {
        ov[i] = 0.0f;
        if (b3p_axis_separates(A, B, axes[i], &ov[i])) return 0;
    }
    /* @0x00108B15..0x00108B75 */
    float best = 3.4028235e38f;                       /* [0x003B172C] */
    int bi = -1;
    for (int i = 0; i < 15; i++) {
        float l2 = axes[i][0]*axes[i][0] + axes[i][1]*axes[i][1]
                 + axes[i][2]*axes[i][2];
        if (l2 < B3P_EPS2) continue;                  /* FUN_0003B060 */
        float o = ov[i] / sqrtf(l2);
        if (best > o) { best = o; bi = i; }
    }
    if (bi < 0) return 0;                             /* @0x00108B7A */

    float l = sqrtf(axes[bi][0]*axes[bi][0] + axes[bi][1]*axes[bi][1]
                  + axes[bi][2]*axes[bi][2]);
    float n[3];
    for (int i = 0; i < 3; i++) n[i] = axes[bi][i] / l;
    /* @0x00108BB7 with the sign taken the other way round -- see the header */
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = B->c[i] - A->c[i];
    if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] < 0.0f)
        for (int i = 0; i < 3; i++) n[i] = -n[i];
    /* THE CONTACT POINT.  Retail has THREE arms, keyed on WHICH axis won
     * (`bi`), not one:
     *   @0x00108BDA  bi <  3  -- the axis is one of box A's face normals, so
     *                            the contact feature is a VERTEX OF B: take
     *                            B's support point and retract it along the
     *                            normal by the penetration (@0x00108BFD the
     *                            support, @0x00108C43 the SUBPS retract).
     *   @0x00108C72  bi <  6  -- the axis is one of box B's face normals, so
     *                            the feature is a vertex of A (@0x00108C8B,
     *                            @0x00108CD4 the ADDPS -- the other sign,
     *                            because retail's normal points B -> A and
     *                            ours points A -> B).
     *   @0x00108D00  bi >= 6  -- an edge-cross axis: retail decodes the axis
     *                            pair through the byte table at 0x0039A99C
     *                            (@0x00108D0F/@0x00108D1E) and runs the
     *                            edge-edge closest point FUN_00108240
     *                            @0x00108D3D.
     * This took box A's support point unconditionally, which is right only
     * for the middle arm -- so on every contact decided by one of the CAR's
     * own face normals the point sat on the car instead of on the prop, and
     * the lever arm r = point - prop_origin that the impulse denominator and
     * the torque are built from was wrong.  That is a FORCE defect, not a
     * bookkeeping one: it is what sets which way a clipped cone tumbles. */
    float pa[3], pb[3], nneg[3];
    for (int i = 0; i < 3; i++) nneg[i] = -n[i];
    if (bi < 3) {
        b3p_obb_support(B, nneg, pb);
        for (int i = 0; i < 3; i++) out_pt[i] = pb[i] + n[i] * best;
    } else if (bi < 6) {
        b3p_obb_support(A, n, pa);
        for (int i = 0; i < 3; i++) out_pt[i] = pa[i] - n[i] * best;
    } else {
        /* FUN_00108D00 / FUN_00108240 [C]: edge-edge closest point.
         * Axis pair decoded through the cross-index (bi - 6) -> (p, q). */
        int edge_idx = bi - 6;
        int ia = edge_idx / 3;
        int ib = edge_idx % 3;
        b3p_edge_edge_closest(A, B, ia, ib, n, out_pt);
    }
    g_obb_arm[bi < 3 ? 0 : (bi < 6 ? 1 : 2)]++;
    for (int i = 0; i < 3; i++) out_n[i] = n[i];
    *out_depth = best;
    return 1;
}

/* The prop instance's OBB: the model bbox under the live instance matrix.
 * `cur` is the GL column-major 4x4 and cur[r*4+c] == frame[r][c], so rows
 * 0/1/2 are the object's world axes and row 3 the translation. */
static void b3p_inst_obb(const B3PropInst* p, const B3PropModel* m, B3POBB* o)
{
    const float* M = p->cur;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) o->ax[k][i] = M[k * 4 + i];
    for (int i = 0; i < 3; i++)
        o->c[i] = M[12 + i] + o->ax[0][i] * m->bb_c[0]
                            + o->ax[1][i] * m->bb_c[1]
                            + o->ax[2][i] * m->bb_c[2];
    for (int k = 0; k < 3; k++) o->h[k] = m->bb_h[k];
}

static int b3p_collide(int car, B3RigidBody* car_rb, B3RigidBody* game_rb,
                       float car_mass, const float bbmax[3],
                       const float bbmin[3],
                       int car_crashed, B3PropHit* out, int max_out);

/* The car box FUN_001084E0 builds for box A, out of the bbox PAIR the vehicle
 * carries at +0x1D0/+0x1E0.  @0x001084EF the gate takes {MAX, MIN}; @0x0010851B
 * centre = (MAX + MIN) * [0x003B1684] (= 0.5); @0x00108541 that local centre is
 * carried into the world by the 4x4, `row3 + row0*c.x + row1*c.y + row2*c.z`;
 * @0x001085B4 half = (MAX - MIN) * 0.5.  `bbmin` NULL keeps the old symmetric
 * box so the pos/vel entry point still has something to use.            [C] */
static void b3p_car_obb(const float frame[4][4], const float bbmax[3],
                        const float bbmin[3], B3POBB* o)
{
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) o->ax[k][i] = frame[k][i];
    float c[3], h[3];
    for (int k = 0; k < 3; k++) {
        float mx = bbmax ? fabsf(bbmax[k]) : 0.0f;
        float mn = bbmin ? bbmin[k] : -mx;
        c[k] = (mx + mn) * 0.5f;
        h[k] = (mx - mn) * 0.5f;
    }
    /* Floors, GLUE, retained from the half-extent form: a car whose .bgv box
     * failed to load must still present something a prop can be knocked by. */
    if (h[0] < 0.4f) h[0] = 0.4f;
    if (h[1] < 0.5f) h[1] = 0.5f;
    if (h[2] < 0.8f) h[2] = 0.8f;
    for (int i = 0; i < 3; i++)
        o->c[i] = frame[3][i] + o->ax[0][i] * c[0] + o->ax[1][i] * c[1]
                              + o->ax[2][i] * c[2];
    for (int k = 0; k < 3; k++) o->h[k] = h[k];
}

void b3_props_test_car_obb(const float frame[4][4], const float bbmax[3],
                           const float bbmin[3], float out_c[3],
                           float out_h[3])
{
    B3POBB o;
    b3p_car_obb(frame, bbmax, bbmin, &o);
    for (int k = 0; k < 3; k++) { out_c[k] = o.c[k]; out_h[k] = o.h[k]; }
}

int b3_props_test_obb_contact(const float aframe[4][4], const float abbmax[3],
                              const float abbmin[3],
                              const float bframe[4][4], const float bbbmax[3],
                              const float bbbmin[3],
                              float* out_depth, float out_pt[3],
                              float out_n[3])
{
    B3POBB A, B;
    b3p_car_obb(aframe, abbmax, abbmin, &A);
    /* box B the same way but with NO floors -- a prop's box is whatever the
     * model bbox says, which is the pair FUN_0011A020 copies to +0x1D0/+0x1E0. */
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) B.ax[k][i] = bframe[k][i];
    float c[3];
    for (int k = 0; k < 3; k++) {
        c[k] = (bbbmax[k] + bbbmin[k]) * 0.5f;
        B.h[k] = (bbbmax[k] - bbbmin[k]) * 0.5f;
    }
    for (int i = 0; i < 3; i++)
        B.c[i] = bframe[3][i] + B.ax[0][i] * c[0] + B.ax[1][i] * c[1]
                              + B.ax[2][i] * c[2];
    return b3p_obb_contact(&A, &B, out_n, out_depth, out_pt);
}

int b3_props_collide_car(int car, const float pos[3], const float vel[3],
                         float yaw, const float bbmax[3], const float bbmin[3],
                         B3PropHit* out, int max_out)
{
    /* Compatibility entry: synthesise the car body FUN_00113960 would have
     * had.  Zero omega, unit inverse inertia and a nominal mass -- prefer
     * b3_props_collide_rb(), which gets the real numbers. */
    B3RigidBody rb; float rb__frame_store[4][4];
    memset(&rb, 0, sizeof rb);
    /* bind AFTER the memset -- it would zero the frame pointer */
    b3_rigid_body_bind_frame(&rb, rb__frame_store);
    if (!pos) return 0;
    float fw[3] = { sinf(yaw), 0.0f, -cosf(yaw) };
    rb.frame[0][0] = -fw[2]; rb.frame[0][2] = fw[0];
    rb.frame[1][1] = 1.0f;
    rb.frame[2][0] = fw[0];  rb.frame[2][2] = fw[2];
    rb.frame[3][0] = pos[0]; rb.frame[3][1] = pos[1]; rb.frame[3][2] = pos[2];
    rb.frame[3][3] = 1.0f;
    if (vel) {
        rb.vel[0] = vel[0]; rb.vel[1] = vel[1]; rb.vel[2] = vel[2];
        rb.vel[3] = v_len(vel);
    }
    for (int k = 0; k < 3; k++) rb.inv_inertia_world[k][k] = 1.0f / 1800.0f;
    /* the frame above is already HARNESS, so hand it over unmirrored */
    return b3p_collide(car, &rb, NULL, B3P_CAR_MASS_FALLBACK, bbmax, bbmin, 0,
                       out, max_out);
}

int b3_props_collide_rb(int car, B3RigidBody* car_rb, float car_mass,
                        const float bbmax[3], const float bbmin[3],
                        int car_crashed, B3PropHit* out, int max_out)
{
    b3p_bind_body_frames();
    /* car_rb is the pipeline's GAME-space body; mirror it into harness space,
     * run the solver there, and mirror any reaction back (only a CRASHED car
     * gets one -- an un-crashed one is role 2, @0x00113B57). */
    B3_RIGID_BODY_LOCAL(h);
    if (!car_rb) return 0;
    b3p_mirror_rb(car_rb, &h);
    return b3p_collide(car, &h, car_crashed ? car_rb : NULL, car_mass,
                       bbmax, bbmin, car_crashed, out, max_out);
}

/* `car_rb` HARNESS space; `game_rb` non-NULL means "mirror the reaction back
 * onto this game-space body when the solver moves the car". */
static int b3p_collide(int car, B3RigidBody* car_rb, B3RigidBody* game_rb,
                       float car_mass, const float bbmax[3],
                       const float bbmin[3],
                       int car_crashed, B3PropHit* out, int max_out)
{
    if (!g_ready || !car_rb) return 0;
    b3p_audit_init();
    int nout = 0;
    if (car_mass < 1.0f) car_mass = B3P_CAR_MASS_FALLBACK;
    float car_imp0[4], car_tor0[4];
    memcpy(car_imp0, car_rb->imp_force, sizeof car_imp0);
    memcpy(car_tor0, car_rb->imp_torque, sizeof car_tor0);

    const float* pos = car_rb->frame[3];
    /* B3_PROP_PROBE: once-a-second "where is the nearest prop" line, for
     * aiming the headless capture runs at a cone field. */
    if (getenv("B3_PROP_PROBE") && car == 0) {
        static float next = 0.0f;
        if (g_clock >= next) {
            next = g_clock + 1.0f;
            float dd = 0.0f;
            int ni = b3_props_nearest(pos, &dd);
            printf("[propprobe] t=%.1f car=(%.1f %.1f %.1f) nearest=%d d=%.2f "
                   "live=%d\n", g_clock, pos[0], pos[1], pos[2], ni, dd,
                   g_live);
            fflush(stdout);
        }
    }
    /* The CAR is retail's box A: the rigid-body frame rows are its world axes
     * (row 0 right, row 1 up, row 2 at) and its extent is the bbox PAIR at
     * +0x1D0/+0x1E0 (.bgv +0xE80/+0xE90).  Using the real frame rather than a
     * yaw-only basis is retail's own geometry -- a pitched or rolled car
     * presents a pitched box -- and using the PAIR rather than the max alone
     * is the rest of it: see the note on b3_props_collide_car(). */
    const float defmax[3] = { 0.95f, 0.75f, 2.20f };
    B3POBB carbox;
    b3p_car_obb((const float (*)[4])car_rb->frame,
                bbmax ? bbmax : defmax, bbmin, &carbox);
    /* Broad-phase radius about the box's OWN centre, which is no longer the
     * body origin; the offset has to be carried or a car whose box sits well
     * forward of its origin rejects props it really does overlap. */
    const float car_r = carbox.h[0] + carbox.h[1] + carbox.h[2];

    /* ---- B3_PROP_AUDIT: the independent geometry measurement -------------
     * Off by default and OFF costs one getenv-cached int test per call.  It
     * does NOT reuse the live gate's verdict -- that would make the answer
     * true by construction.  It sweeps the car's box from where it was last
     * frame to where it is now and asks the recovered SAT at each sample, so
     * it also catches the contact a single discrete test would TUNNEL past.
     * A prop it marks swept but the live path never marks hit is a MISS. */
    if (g_audit && car >= 0 && car < B3P_AUDIT_CARS) {
        float prev[3], prev_ax[3][3];
        int have_prev = g_aud_have[car];
        /* the sweep tracks the BOX CENTRE, which is offset from the body
         * origin by the bbox pair -- sweeping the origin would slide the box
         * along a line it never travelled. */
        for (int k = 0; k < 3; k++) {
            prev[k] = g_aud_prev[car][k];
            g_aud_prev[car][k] = carbox.c[k];
            for (int q = 0; q < 3; q++) {
                prev_ax[k][q] = g_aud_prev_ax[car][k][q];
                g_aud_prev_ax[car][k][q] = carbox.ax[k][q];
            }
        }
        g_aud_have[car] = 1;
        if (have_prev) {
            float step[3];
            float sl = 0.0f;
            for (int k = 0; k < 3; k++) {
                step[k] = carbox.c[k] - prev[k];
                sl += step[k] * step[k];
            }
            sl = sqrtf(sl);
            /* 0.25 m of travel per sample: fine enough that the smallest
             * shipped prop (a 0.25 m WF_Cone) cannot slip between two
             * samples, coarse enough to stay cheap.  A step past 5 m is a
             * SCENARIO PLACEMENT or a stuck-rescue teleport, not travel -- its
             * straight line crosses props the car never went near, so the
             * frame is dropped rather than swept. */
            int ns = (int)(sl / 0.25f) + 2;
            if (sl > 5.0f) ns = 0;
            if (ns > 1) {
                B3POBB sweep = carbox;
                for (int i = 0; i < g_ninst; i++) {
                    B3PropInst* p = &g_inst[i];
                    if (p->aud_swept || p->state == B3P_SETTLED) continue;
                    B3POBB pb;
                    b3p_inst_obb(p, &g_model[p->model], &pb);
                    float far2 = car_r + p->bound_r + sl;
                    float dd[3];
                    for (int k = 0; k < 3; k++) dd[k] = pb.c[k] - carbox.c[k];
                    if (dd[0]*dd[0] + dd[1]*dd[1] + dd[2]*dd[2]
                        > far2 * far2) continue;
                    for (int s = 0; s < ns; s++) {
                        float u = (float)s / (float)(ns - 1);
                        float nn[3], pp, qq[4];
                        for (int k = 0; k < 3; k++)
                            sweep.c[k] = prev[k] + step[k] * u;
                        /* The car TURNS between frames too.  Blending the
                         * axes and renormalising is not a slerp, but over one
                         * 16 ms frame the error is far below a cone's width
                         * -- and holding the current orientation over the
                         * previous POSITION, which is what this did first,
                         * invents overlaps a turning car never had. */
                        for (int k = 0; k < 3; k++) {
                            float l2 = 0.0f;
                            for (int q = 0; q < 3; q++) {
                                sweep.ax[k][q] = prev_ax[k][q]
                                    + (carbox.ax[k][q] - prev_ax[k][q]) * u;
                                l2 += sweep.ax[k][q] * sweep.ax[k][q];
                            }
                            if (l2 > 1e-12f) {
                                float inv = 1.0f / sqrtf(l2);
                                for (int q = 0; q < 3; q++)
                                    sweep.ax[k][q] *= inv;
                            }
                        }
                        if (b3p_obb_contact(&sweep, &pb, nn, &pp, qq)) {
                            p->aud_swept = 1;
                            break;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < g_ninst; i++) {
        B3PropInst* p = &g_inst[i];
        if (p->state == B3P_SETTLED) continue;
        const B3PropModel* m = &g_model[p->model];

        /* THE PROP'S COLLISION VOLUME IS ITS MODEL BOUNDING BOX under the live
         * instance transform -- the same two bbox rows retail's constructor
         * copies onto the body (+0x1D0 MAX @0x0011A0A8, +0x1E0 MIN
         * @0x0011A0AF) and the same pair FUN_001084E0 reads.
         *
         * THIS IS THE FIX FOR BOTH REPORTED DEFECTS.  What stood here was a
         * SPHERE at the model's mid height with radius max(halfX, halfZ),
         * which is wrong in three different directions on shipped data:
         *   WF_sign_prop  1.12 x 6.14 x 0.42  ->  a 0.56 m sphere floating
         *       3.07 m above the road.  A car box roughly 1.2 m tall could
         *       never reach it: every tall signpost in the game (class 6,
         *       2056 instances across the 37 tracks) was UN-HITTABLE, which is
         *       the reported "hitting signs should make them fly off".
         *   WF_Cone       0.59 x 1.04 x 0.59  ->  a 0.30 m sphere spanning
         *       0.22..0.82 m instead of the cone's real 0 .. 1.04 m, so a
         *       cone only entered the test through a narrow band and the
         *       degenerate-normal case below then threw most of those away.
         *   WF_prop_mktbarrier 3.43 x 1.48 x 0.44 -> a 1.72 m sphere around a
         *       0.44 m thick board, knocked from 1.7 m of clear air.       */
        B3POBB pb;
        b3p_inst_obb(p, m, &pb);

        float d[3] = { pb.c[0] - carbox.c[0], pb.c[1] - carbox.c[1],
                       pb.c[2] - carbox.c[2] };
        /* cheap reject (broad phase only; the gate below is the real test) */
        float far2 = car_r + p->bound_r;
        if (d[0]*d[0] + d[1]*d[1] + d[2]*d[2] > far2 * far2) continue;

        float nw[3], pen, cp[4];
        if (!b3p_obb_contact(&carbox, &pb, nw, &pen, cp)) continue;
        cp[3] = 0.0f;

        /* Pre-promotion gate.  Retail has NONE: FUN_001084E0 @0x00113901 is
         * purely geometric and FUN_00113890 promotes on any overlap.  This
         * guard exists only so a prop a PARKED car is resting against is not
         * promoted again every frame, and it is now the SPEED of the car at
         * the contact point -- the same |v_rel| the solver itself takes --
         * rather than that speed's component along the contact normal.
         *
         * The normal-component form was the other half of the cone defect:
         * once the car box had swallowed a cone the old sphere test reported
         * distance 0 and fell back to a hard-coded LATERAL normal, so a car
         * driving straight through a cone scored a normal component of ~0 and
         * the cone was refused a body on every single frame of the pass.  A
         * dead-centre cone was never knocked; only a glancing one was. */
        if (p->state == B3P_REST) {
            float vcp[4];
            b3p_point_vel(car_rb, cp, vcp);
            if (v_dot3(vcp, vcp) <= B3P_KNOCK_MIN_SPEED * B3P_KNOCK_MIN_SPEED)
                continue;
            if (b3p_promote(i) < 0) continue;
        }
        p->aud_hit = 1;
        B3PropBody* b = &g_body[p->body];
        B3RigidBody* prb = &b->rb;

        /* ---- FUN_00113960, the generic solver, for (car A, prop B) ------ */
        float nbent[4], imp[4], nin[4] = { nw[0], nw[1], nw[2], 0.0f };
        float j = b3p_contact(prb, b->mass, car_rb, car_mass, cp, nin,
                              car_crashed, nbent, imp);
        /* @0x00113B4x: the solver stamps +0x211 on BOTH bodies of the pair.
         * On the prop that is the wake -- see the latch note in b3p_integrate. */
        b->hit_211 = 1;
        b->lru_key = g_clock + B3P_LRU_OFFSET;   /* keep the freshest alive */
        float vpa[4], vpb[4], vrel[4];
        b3p_point_vel(car_rb, cp, vpa);
        b3p_point_vel(prb, cp, vpb);
        for (int k = 0; k < 4; k++) vrel[k] = vpb[k] - vpa[k];

        /* Separation.  FUN_00114F30's mass split degenerates to "the prop
         * takes all of it" when the car is role 2 (@0x00113BFF/@0x00113C05),
         * which is the un-crashed case; deflection is consumed by the next
         * FUN_00109560 (+0x130).  `pen` is FUN_001084E0's own output, the
         * minimum normalised overlap it hands back at [ebp+0x08]. */
        if (pen > 0.0f)
            for (int k = 0; k < 3; k++) prb->deflection[k] += nw[k] * pen;

        /* v_rel for the crash trigger is the OBJECT's point velocity minus
         * the CAR's -- FUN_00112E70 @0x00113311 -- which is exactly `vrel`. */
        float vnc = -v_dot3(vrel, nbent);

        if (out && nout < max_out) {
            B3PropHit* h = &out[nout++];
            h->instance = i;
            h->model = (int)p->model;
            h->prop_class = (int)p->prop_class;
            h->car = car;
            h->obj_class = b3_props_object_class(i);
            h->mass = b->mass;
            h->radius = b->radius;
            h->point[0] = cp[0]; h->point[1] = cp[1]; h->point[2] = cp[2];
            h->normal[0] = nbent[0]; h->normal[1] = nbent[1];
            h->normal[2] = nbent[2];
            h->vrel[0] = vrel[0]; h->vrel[1] = vrel[1]; h->vrel[2] = vrel[2];
            h->closing_mph = fabsf(vnc) * B3P_MS_TO_MPH;
            h->impulse = j;
        }
        (void)m;
    }
    /* Mirror the car's share back into game space: J is a true vector (S*J),
     * the torque is a pseudo-vector (-S*T). */
    if (game_rb) {
        float dj[4], dt[4];
        for (int k = 0; k < 4; k++) {
            dj[k] = car_rb->imp_force[k] - car_imp0[k];
            dt[k] = car_rb->imp_torque[k] - car_tor0[k];
        }
        game_rb->imp_force[0] += dj[0];
        game_rb->imp_force[1] += dj[1];
        game_rb->imp_force[2] -= dj[2];
        game_rb->imp_torque[0] -= dt[0];
        game_rb->imp_torque[1] -= dt[1];
        game_rb->imp_torque[2] += dt[2];
    }
    return nout;
}

/* Nearest STILL-STANDING prop of `prop_class` at least `min_dist` away, in
 * HARNESS space.  Aiming surface for the scripted prop drives -- the harness
 * equivalent of b3_props_nearest, restricted so a scenario can ask for "a cone"
 * or "a signpost" and never re-target the one it has just flattened. */
int b3_props_nearest_class(const float pos[3], int prop_class, float min_dist,
                           float* out_dist, float out_pos[3]) {
    if (!g_ready || !pos) return -1;
    int best = -1;
    float bd = 1e30f;
    const float md2 = min_dist * min_dist;
    for (int i = 0; i < g_ninst; i++) {
        if (g_inst[i].state != B3P_REST) continue;
        if (prop_class >= 0 && (int)g_inst[i].prop_class != prop_class) continue;
        float dx = g_inst[i].cur[12] - pos[0];
        float dy = g_inst[i].cur[13] - pos[1];
        float dz = g_inst[i].cur[14] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < md2 || d >= bd) continue;
        bd = d; best = i;
    }
    if (best < 0) return -1;
    if (out_dist) *out_dist = sqrtf(bd);
    if (out_pos) {
        out_pos[0] = g_inst[best].cur[12];
        out_pos[1] = g_inst[best].cur[13];
        out_pos[2] = g_inst[best].cur[14];
    }
    return best;
}

int b3_props_nearest(const float pos[3], float* out_dist) {
    if (!g_ready || !pos) return -1;
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < g_ninst; i++) {
        float dx = g_inst[i].cur[12] - pos[0];
        float dy = g_inst[i].cur[13] - pos[1];
        float dz = g_inst[i].cur[14] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    if (out_dist) *out_dist = best >= 0 ? sqrtf(bd) : 0.0f;
    return best;
}

/* ---- draw ------------------------------------------------------------- */
void b3_props_draw(void) {
    if (!g_ready || !g_retained) return;
    /* The props that are still on their authored transform -- which is all of
     * them until a car hits one -- come out of one baked world-space buffer,
     * merged into a handful of glDrawArrays.  A KNOCKED or SETTLED prop is no
     * longer at its baked transform, so it is skipped there and drawn on its
     * own from the model-space copy; at most B3P_MAX_LIVE = 16 of those exist
     * at once.
     *
     * What this replaces was one glPushMatrix / glMultMatrixf / glCallList /
     * glPopMatrix per instance plus four SYNCHRONOUS glIsEnabled readbacks. */
    unsigned char* skip = g_retained_skip;
    int live = 0;
    for (int i = 0; i < g_ninst; i++) {
        skip[i] = (unsigned char)(g_inst[i].state != B3P_REST);
        live += skip[i];
    }
    int batches = b3r_inst_draw(g_retained, NULL, live ? skip : NULL);
    for (int i = 0; live && i < g_ninst; i++) {
        if (!skip[i]) continue;
        b3r_inst_draw_model(g_retained, (int)g_inst[i].model,
                            g_inst[i].cur, g_inst[i].tint);
        batches++;
    }
    b3r_stat_set(B3R_STAT_PROPS, batches);
}
