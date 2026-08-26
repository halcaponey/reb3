// curb_traj.c -- the RE side of the CURB-STRIKE differential.
//
// tools/dump_traj.c already runs b3_vehicle_step_full against a supplied
// polygon soup, but its ground is a hard-coded flat plane at y = 0
// (dump_traj.c:31-40), so the suspension never sees anything the soup adds.
// Retail does not work that way: FUN_00123790 ray-tests THE SAME frozen set
// FUN_0011AEF0 walks -- `mov eax, dword ptr [esi + 0x200]` @0x001237E5 [C].
// This driver therefore binds `soup_ground_ray` to the very soup it hands
// `soup_freeze`, which is retail's supply exactly, so a divergence measured
// against tools/b3_emu_server-style emulation is a SOLVER divergence and not
// a geometry one.
//
// Usage (same state-file protocol as dump_traj --state):
//   curb_traj --state <file> <frames> <thr> <brk> <steer> <boost>
//             --soup <file> [--authority f] [--flags1353 n]
//             [--wheelgate 0|1] [--tracewheels 0|1]
//
// `--wheelgate 1` enables retail's per-poly WHEEL-RAY SURFACE GATE, the arm
// the port does not have:
//
//   [C] FUN_00123790 @0x00123799..0x0012383E (burnout3.elf)
//       cVar6 = veh+0x215;  bVar5 = (cVar6 == 1 || == 2 || == 3)
//       bVar2 = surface_flags[i] & 0xFF
//       if (bVar5 && veh+0x210 == 0 && bVar2 != 0x26 && 0xb < bVar2
//           && (bVar2 < 0xc || 0x14 < bVar2))   -> the poly is NOT ray-tested
//       (0xb < bVar2 && bVar2 < 0xc is empty, so the live rule is
//        `skip iff low byte > 0x14 and != 0x26`)
//
// Output: one JSON object per frame on stdout.
//
// Soup file format is dump_traj's, one polygon per line, GAME space:
//     p0x p0y p0z  p1x p1y p1z  p2x p2y p2z  nx ny nz  surface_u16

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "burnout3_vehicle_sim.h"
#include "burnout3_crash.h"
/* The per-car VDB tuning is a RUNTIME asset now: burnout3_car_physics.h is
 * gone and this loads build/cars/car_physics.bin at first use instead.  Same
 * numbers -- the header's "%.9g" round-tripped a float, so the asset carries
 * exactly the bits its literals compiled to (proof: the purge-2 parity dump). */
#include "burnout3_car_physics_runtime.h"

#define SOUP_MAX 256
static B3CrashPoly g_soup_poly[SOUP_MAX];
static unsigned short g_soup_flag[SOUP_MAX];
static int g_soup_n = 0;
/* The CHASSIS set, when it differs from the wheel set.  Retail hands both
 * queries one frozen soup (veh+0x200), so by default this aliases the set
 * above.  `--chassis-soup` splits them, which is what models the LIVE port:
 * src/burnout3_full.c's harness_soup_freeze runs b3_collision_filter_walls
 * over the sphere gather (dropping every |n.y| > 0.70 face) before handing
 * the result to the contact resolve, while the wheels keep the unfiltered
 * box gather. */
static B3CrashPoly g_chassis_poly[SOUP_MAX];
static unsigned short g_chassis_flag[SOUP_MAX];
static int g_chassis_n = -1;      /* -1 = alias the wheel set */
static int g_wheelgate = 0;      // retail FUN_00123790 surface gate on/off
static unsigned char g_class215 = 3;   // veh+0x215 (racecar pools 2/3)

// ---------------------------------------------------------------------------
// FUN_001B2230 [C] -- the one-sided Moller-Trumbore the wheel ray uses, with
// the game's compiled-in constants (mirrored from src/burnout3_collision.c,
// which is itself the verified 1:1 port).  Returns parametric t or -1.
// ---------------------------------------------------------------------------
#define B3C_DET_EPS 9.99999993922529e-09f
#define B3C_LO_K   -9.999999747378752e-06f
#define B3C_HI_K    1.0000100135803223f

static float ray_tri(const float* A, const float* B,
                     const float* v0, const float* v1, const float* v2) {
    float d[3]  = {B[0]-A[0], B[1]-A[1], B[2]-A[2]};
    float e1[3] = {v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2]};
    float e2[3] = {v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2]};
    float P[3]  = {d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2],
                   d[0]*e2[1]-d[1]*e2[0]};
    float det = e1[0]*P[0] + e1[1]*P[1] + e1[2]*P[2];
    if (!(det > B3C_DET_EPS)) return -1.0f;
    float lo = det * B3C_LO_K, hi = det * B3C_HI_K;
    float T[3] = {A[0]-v0[0], A[1]-v0[1], A[2]-v0[2]};
    float u = T[0]*P[0] + T[1]*P[1] + T[2]*P[2];
    if (!(u > lo && u <= hi)) return -1.0f;
    float Q[3] = {T[1]*e1[2]-T[2]*e1[1], T[2]*e1[0]-T[0]*e1[2],
                  T[0]*e1[1]-T[1]*e1[0]};
    float v = d[0]*Q[0] + d[1]*Q[1] + d[2]*Q[2];
    if (!(v > lo)) return -1.0f;
    if (u + v > hi) return -1.0f;
    float t = e2[0]*Q[0] + e2[1]*Q[1] + e2[2]*Q[2];
    if (!(t > lo && t <= hi)) return -1.0f;
    return t / det;
}

// FUN_00123790's per-poly testability gate, [C] @0x00123799..0x0012383E.
// Pure: whether the gate RUNS is the caller's decision (see soup_ray).
static int poly_testable(unsigned short flag) {
    const int cls_in_123 = (g_class215 == 1 || g_class215 == 2
                            || g_class215 == 3);
    const unsigned lo = flag & 0xFFu;
    /* veh+0x210 == 0 on the racing path this driver models */
    if (cls_in_123 && lo != 0x26u && lo > 0x14u) return 0;
    return 1;
}

// The winner rule is FUN_00123790's: the minimum PARAMETRIC t over the soup.
//
// TWO BUILDS.  Against today's tree the `soup_ground_ray` hook has no gate
// argument, so `--wheelgate` decides here and the driver MODELS the proposed
// fix.  Built with -DB3_SOUP_GROUND_RAY_HAS_GATE against the patched tree the
// hook carries retail's own wheel/under-body distinction, and the port -- not
// this driver -- decides.  Running both and getting the same numbers is what
// proves the patch implements the law this file transcribes.
#ifdef B3_SOUP_GROUND_RAY_HAS_GATE
static int soup_ray(void* user, const float start[3], const float end[3],
                    float* hit_t, float normal[3], int wheel_gate) {
    (void)user;
    const int gate_here = wheel_gate;
#else
static int soup_ray(void* user, const float start[3], const float end[3],
                    float* hit_t, float normal[3]) {
    (void)user;
    const int gate_here = g_wheelgate;
#endif
    float best = 999.0f;
    int best_i = -1;
    for (int i = 0; i < g_soup_n; i++) {
        if (gate_here && !poly_testable(g_soup_flag[i])) continue;
        const B3CrashPoly* p = &g_soup_poly[i];
        float t = ray_tri(start, end, p->p0, p->p1, p->p2);
        if (t >= 0.0f && t < best) { best = t; best_i = i; }
    }
    if (best_i < 0) return -1;
    if (hit_t) *hit_t = best;
    if (normal) {
        normal[0] = g_soup_poly[best_i].n[0];
        normal[1] = g_soup_poly[best_i].n[1];
        normal[2] = g_soup_poly[best_i].n[2];
    }
    return (int)g_soup_flag[best_i];
}

// The vehicle_sim link-time fallback; unused once soup_ground_ray is bound,
// but the translation unit references it.
int b3_ground_probe(float x, float y, float z,
                    float* out_height, float out_normal[3]) {
    (void)x; (void)y; (void)z;
    *out_height = 0.0f;
    out_normal[0] = 0.0f; out_normal[1] = 1.0f; out_normal[2] = 0.0f;
    return 0;
}

static int soup_freeze(void* user, B3VehicleFull* v) {
    (void)user;
    if (g_chassis_n >= 0) {
        v->soup.count = g_chassis_n;
        v->soup.polys = g_chassis_poly;
        v->soup.flags = g_chassis_flag;
        return g_chassis_n;
    }
    v->soup.count = g_soup_n;
    v->soup.polys = g_soup_poly;
    v->soup.flags = g_soup_flag;
    return g_soup_n;
}

static int load_soup_into(const char* path, B3CrashPoly* poly,
                          unsigned short* flag) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "soup: cannot open %s\n", path); exit(1); }
    double q[13];
    int n = 0;
    while (n < SOUP_MAX) {
        int got = 0;
        for (; got < 13; got++)
            if (fscanf(f, "%lf", &q[got]) != 1) break;
        if (got < 13) break;
        B3CrashPoly* p = &poly[n];
        for (int k = 0; k < 3; k++) {
            p->p0[k] = (float)q[k];
            p->p1[k] = (float)q[3 + k];
            p->p2[k] = (float)q[6 + k];
            p->n[k]  = (float)q[9 + k];
        }
        p->p0[3] = p->p1[3] = p->p2[3] = p->n[3] = 0.0f;
        flag[n] = (unsigned short)q[12];
        n++;
    }
    fclose(f);
    return n;
}

// ---------------------------------------------------------------------------
// state file (emulate_pipeline.Pipeline.export_state), same reader as
// tools/dump_traj.c
// ---------------------------------------------------------------------------
static float kv(const char* buf, const char* key, float dflt) {
    const char* p = buf;
    size_t kl = strlen(key);
    while (p) {
        if (!strncmp(p, key, kl) && p[kl] == ' ')
            return (float)atof(p + kl + 1);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return dflt;
}

static void load_state(B3VehicleFull* v, const char* path) {
    static char buf[65536];
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "state: cannot open %s\n", path); exit(1); }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    char key[64];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            snprintf(key, sizeof key, "frame_%d_%d", r, c);
            v->rb.frame[r][c] = kv(buf, key, v->rb.frame[r][c]);
        }
    v->rb.vel[0] = kv(buf, "vel_x", 0);
    v->rb.vel[1] = kv(buf, "vel_y", 0);
    v->rb.vel[2] = kv(buf, "vel_z", 0);
    v->rb.vel[3] = kv(buf, "speed", 0);
    for (int i = 0; i < 3; i++) {
        snprintf(key, sizeof key, "dir_%d", i);
        v->rb.dir[i] = kv(buf, key, 0);
        snprintf(key, sizeof key, "omega_%d", i);
        v->rb.omega[i] = kv(buf, key, 0);
        snprintf(key, sizeof key, "angmom_%d", i);
        v->rb.angmom[i] = kv(buf, key, 0);
        snprintf(key, sizeof key, "defl_%d", i);
        v->rb.deflection[i] = kv(buf, key, 0);
    }
    for (int i = 0; i < 4; i++) {
        B3WheelSim* w = &v->wheel[i];
        static const char* nm[4] = {"wp", "cp", "pp", "pc"};
        float* dst[4] = {w->world_pos, w->contact_pt, w->prev_pos,
                         w->prev_contact};
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 3; k++) {
                snprintf(key, sizeof key, "w%d_%s_%d", i, nm[j], k);
                dst[j][k] = kv(buf, key, 0);
            }
        for (int k = 0; k < 3; k++) {
            snprintf(key, sizeof key, "w%d_n_%d", i, k);
            w->normal[k] = kv(buf, key, k == 1 ? 1.0f : 0.0f);
        }
        snprintf(key, sizeof key, "w%d_torque", i);
        w->torque = kv(buf, key, 0);
        snprintf(key, sizeof key, "w%d_spin", i);
        w->spin = kv(buf, key, 0);
        snprintf(key, sizeof key, "w%d_omega", i);
        w->omega = kv(buf, key, 0);
        snprintf(key, sizeof key, "w%d_prev", i);
        w->prev_len = kv(buf, key, w->prev_len);
        snprintf(key, sizeof key, "w%d_cur", i);
        w->cur_len = kv(buf, key, w->cur_len);
        snprintf(key, sizeof key, "w%d_contact", i);
        w->contact = (unsigned char)kv(buf, key, 0);
        v->wheel_frame_y[i] = w->prev_len;
    }
    B3EngineTransmission* t = &v->trans;
    t->omega = kv(buf, "t_omega", t->omega);
    t->shift_timer = kv(buf, "t_timer", 0);
    t->shifting = (int)kv(buf, "t_shifting", 0);
    t->no_upshift = (int)kv(buf, "t_noupshift", 0);
    t->rev_limit_rpm = kv(buf, "t_limit", t->rev_limit_rpm);
    {
        const char* a = strstr(buf, "t_rng_a ");
        const char* c = strstr(buf, "t_rng_c ");
        if (a) t->rng_state = (unsigned)(double)atof(a + 8);
        if (c) t->rng_inc = (unsigned)(double)atof(c + 8);
    }
    t->gear = (int)kv(buf, "t_gear", 0);
    t->upshift_block = kv(buf, "t_upblk", 0);
    t->downshift_block = kv(buf, "t_dnblk", 0);
    v->thr_prev_141C = kv(buf, "thr_prev", 0);
    v->brake_prev_1420 = kv(buf, "brake_prev", 0);
    v->steer_prev_1424 = kv(buf, "steer_prev", 0);
    v->drift_time_142C = kv(buf, "drift_time", 0);
    v->slide_prev_1430 = kv(buf, "slide_prev", v->slide_prev_1430);
    v->drift_timer_1438 = kv(buf, "drift_timer", 0);
    v->airtime_143C = kv(buf, "airtime", 0);
    v->slide_1440 = kv(buf, "slide", v->slide_1440);
    v->drift_state_1524 = (int)kv(buf, "drift_state", 0);
    v->f1168 = (unsigned char)kv(buf, "f1168", 0);
    v->timer_152C = kv(buf, "timer_152C", -1.0f);
    v->clock = kv(buf, "clock", 0);
    v->grip_scalar = kv(buf, "grip", 1.2f);
    v->class_215 = (unsigned char)kv(buf, "class_215", v->class_215);
    v->contact_212 = (unsigned char)kv(buf, "contact_212", 0);
    v->hit_side_153C = (unsigned char)kv(buf, "hit_side_153C", 0);
    v->flag_b_1446 = (unsigned char)kv(buf, "flag_b_1446", 0);
    v->ooc_slam_1598 = kv(buf, "ooc_slam_1598", -1.0f);
    v->ooc_wall_1690 = kv(buf, "ooc_wall_1690", -1.0f);
    v->launch_time_1350 = kv(buf, "launch_1350", -100.0f);
    g_class215 = v->class_215;
    {   // rebuild the derived matrices (inverse frame + world inverse inertia)
        float(*m)[4] = v->rb.frame;
        float(*inv)[4] = v->rb.inv_frame;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) inv[r][c] = m[r][c];
        float t2;
        t2 = inv[0][1]; inv[0][1] = inv[1][0]; inv[1][0] = t2;
        t2 = inv[0][2]; inv[0][2] = inv[2][0]; inv[2][0] = t2;
        t2 = inv[1][2]; inv[1][2] = inv[2][1]; inv[2][1] = t2;
        float p[4];
        for (int j = 0; j < 4; j++)
            p[j] = m[3][0] * inv[0][j] + m[3][1] * inv[1][j]
                 + m[3][2] * inv[2][j];
        for (int j = 0; j < 4; j++) inv[3][j] = -p[j];
        float Rt[3][4], tmp[3][4];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) Rt[r][c] = m[c][r];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                float s = 0.0f;
                for (int k = 0; k < 3; k++)
                    s += Rt[r][k] * v->rb.inv_inertia_body[k][c];
                tmp[r][c] = s;
            }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                float s = 0.0f;
                for (int k = 0; k < 3; k++) s += tmp[r][k] * m[k][c];
                v->rb.inv_inertia_world[r][c] = s;
            }
    }
}

static float g_authority = 1.0f;
static unsigned char g_flags1353 = 0;

static void emit(const B3VehicleFull* v) {
    const float(*m)[4] = v->rb.frame;
    printf("{\"pos\":[%.9g,%.9g,%.9g],\"vel\":[%.9g,%.9g,%.9g],"
           "\"speed\":%.9g,\"omega\":[%.9g,%.9g,%.9g],"
           "\"angmom\":[%.9g,%.9g,%.9g],"
           "\"right\":[%.9g,%.9g,%.9g],\"up\":[%.9g,%.9g,%.9g],"
           "\"at\":[%.9g,%.9g,%.9g],"
           "\"defl\":[%.9g,%.9g,%.9g],"
           "\"gear\":%d,\"drift\":%d,\"c212\":%d,\"cstate\":%d,"
           "\"impact\":%.9g,\"cfire\":%d,\"wheels\":[",
           m[3][0], m[3][1], m[3][2],
           v->rb.vel[0], v->rb.vel[1], v->rb.vel[2], v->rb.vel[3],
           v->rb.omega[0], v->rb.omega[1], v->rb.omega[2],
           v->rb.angmom[0], v->rb.angmom[1], v->rb.angmom[2],
           m[0][0], m[0][1], m[0][2],
           m[1][0], m[1][1], m[1][2],
           m[2][0], m[2][1], m[2][2],
           v->rb.deflection[0], v->rb.deflection[1], v->rb.deflection[2],
           v->trans.gear, v->drift_state_1524,
           v->contact_212, v->contact_state_198, v->impact_194,
           v->crash_fired);
    for (int i = 0; i < 4; i++) {
        const B3WheelSim* w = &v->wheel[i];
        printf("%s{\"cur\":%.9g,\"prev\":%.9g,\"contact\":%d,\"bump\":%d,"
               "\"fflag\":%d,\"surf\":%u,\"n\":[%.9g,%.9g,%.9g],"
               "\"wp\":[%.9g,%.9g,%.9g]}",
               i ? "," : "", w->cur_len, w->prev_len, w->contact, w->bump,
               w->force_flag, (unsigned)w->surface,
               w->normal[0], w->normal[1], w->normal[2],
               w->world_pos[0], w->world_pos[1], w->world_pos[2]);
    }
    printf("]}\n");
}

int main(int argc, char** argv) {
    const char* state_file = NULL;
    const char* soup_file = NULL;
    const char* chassis_file = NULL;
    int frames = 0;
    float th = 0, br = 0, st = 0;
    int bo = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--state") && i + 6 < argc) {
            state_file = argv[i + 1];
            frames = atoi(argv[i + 2]);
            th = (float)atof(argv[i + 3]);
            br = (float)atof(argv[i + 4]);
            st = (float)atof(argv[i + 5]);
            bo = atoi(argv[i + 6]);
            i += 6;
        } else if (!strcmp(argv[i], "--soup") && i + 1 < argc) {
            soup_file = argv[++i];
        } else if (!strcmp(argv[i], "--chassis-soup") && i + 1 < argc) {
            chassis_file = argv[++i];
        } else if (!strcmp(argv[i], "--authority") && i + 1 < argc) {
            g_authority = (float)atof(argv[++i]);
        } else if (!strcmp(argv[i], "--flags1353") && i + 1 < argc) {
            g_flags1353 = (unsigned char)strtoul(argv[++i], 0, 0);
        } else if (!strcmp(argv[i], "--wheelgate") && i + 1 < argc) {
            g_wheelgate = atoi(argv[++i]);
        }
    }
    if (!state_file) {
        fprintf(stderr, "usage: curb_traj --state <f> <n> <th> <br> <st> <bo>"
                        " --soup <f> [--chassis-soup <f>]"
                        " [--wheelgate 0|1]\n");
        return 2;
    }

    B3PhysicsConfig cfg;
    b3_physics_defaults(&cfg);
    {   /* COMPCAR1's 64 VDB overrides, out of build/cars/car_physics.bin */
        const B3CarPhysics* pc = b3_car_physics_find("COMPCAR1");
        if (!pc) {
            fprintf(stderr, "build/cars/car_physics.bin carries no COMPCAR1\n");
            return 1;
        }
        for (int i = 0; i < pc->n_params; i++)
            b3_config_set_by_offset(&cfg, pc->params[i].offset,
                                    pc->params[i].value);
    }

    // Car1.bgv real geometry -- the same values emulate_pipeline.py seeds
    static const float wheels_xz[4][2] = {
        {-0.7600f, 1.2379f}, {0.7600f, 1.2379f},
        {-0.7600f, -1.3067f}, {0.7600f, -1.3067f}};
    static const float half_ext[4] = {1.0157f, 1.1222f, 2.0636f, 0.0f};
    static const float center_off[4] = {-1.0157f, -0.1505f, -2.0866f,
                                        2.0636f};
    static const float inv_inertia[3] = {0.0008f, 0.0011f, 0.0013f};
    static const float pos[3] = {0.0f, 0.31f, 0.0f};

    B3VehicleFull v;
    b3_vehicle_full_init(&v, &cfg, wheels_xz, 0.3117f, half_ext,
                         center_off, inv_inertia, pos, 0.0f);
    if (soup_file)
        g_soup_n = load_soup_into(soup_file, g_soup_poly, g_soup_flag);
    if (chassis_file)
        g_chassis_n = load_soup_into(chassis_file, g_chassis_poly,
                                     g_chassis_flag);
    v.soup_freeze        = soup_freeze;
    v.soup_ground_user   = NULL;
    v.soup_ground_ray    = soup_ray;      // retail: the SAME veh+0x200 set
    v.chassis_resolve    = b3_vehicle_chassis_contact;
    v.surface_grip_13A8  = 1.0f;
    v.authority_1534     = g_authority;
    v.drift_dir_1434     = 0.0f;
    v.flags_1353         = g_flags1353;
    v.no_scrub_153E      = 0;
    v.landed_211         = 0;
    v.racecar_class_1920 = 0;
    v.is_class0          = 1;
    v.party_mode         = 0;

    load_state(&v, state_file);
    const float dt = 1.0f / 60.0f;
    for (int f = 0; f < frames; f++) {
        b3_vehicle_step_full(&v, th, br, st, bo, dt);
        emit(&v);
    }
    return 0;
}
