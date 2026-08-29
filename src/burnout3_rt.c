/* burnout3_rt.c -- the ray-traced sun shadow's switch, world and reference
 * trace.  See burnout3_rt.h for the design and for why every line of it is
 * INSPIRED rather than recovered.
 *
 * GL-FREE, deliberately and completely: tools/validate_photo.py compiles this
 * file on its own and calls b3_rt_want() and b3_rt_transmittance() with real
 * environments, which is the only way a switch and a traversal can be gated
 * as LAWS rather than as source text.  The GL side -- the two textures, the
 * uniforms and the GLSL walk -- lives in src/burnout3_aftereffects.c, exactly
 * as the shadow map's GL side lives in src/burnout3_render.c. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "burnout3_rt.h"

/* ============================================================ THE SWITCH */

/* The config file, and it is the SAME shape build/mixer.cfg has: a literal
 * "build/..." path handed to the ordinary fopen(), which
 * src/burnout3_isoshim.h rewrites to b3_iso_fopen() and resolve_write() then
 * routes to the real build/ tree whenever there is one.  No path arithmetic
 * and no env for the path: the two config files must live together or the
 * user has two places to look. */
#define B3_RT_CFG "build/settings.cfg"

static int g_want = -1;        /* -1 = not yet read                        */
static int g_forced;           /* $B3_RT decided it                        */

static void rt_read(void)
{
    FILE *f;
    const char *e;

    if (g_want >= 0) return;
    g_want = 0;                              /* DEFAULT OFF -- see the .h */

    f = fopen(B3_RT_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (fscanf(f, "%31s %f", k, &v) == 2) {
            if (!strcmp(k, "raytracing")) g_want = (v != 0.0f);
        }
        fclose(f);
    }
    /* The env wins outright, and says so: a harness that pins B3_RT must not
     * be quietly overruled by whatever the last person to open the menu
     * left in the file. */
    e = getenv("B3_RT");
    if (e && *e) { g_want = (*e != '0'); g_forced = 1; }
}

int b3_rt_want(void)
{
    rt_read();
    return g_want;
}

void b3_rt_set(int on)
{
    rt_read();
    g_want = on ? 1 : 0;
}

int b3_rt_env_forced(void)
{
    rt_read();
    return g_forced;
}

/* THE PLATFORM GATE, and it is deliberately ONE LINE of policy.
 *
 * Desktop: yes, and docs/PHOTOREALISM.md carries the numbers.
 * Web:     decided by measurement, not by taste -- see the WEB VERDICT in
 *          that document.  Hiding the ROW never disables the FEATURE, so
 *          B3_RT=1 still turns it on wherever it can run and the
 *          measurement stays repeatable; B3_RT_SHOW=1 puts the row back. */
int b3_rt_option_visible(void)
{
    const char *e = getenv("B3_RT_SHOW");
    if (e && *e) return *e != '0';
#ifdef __EMSCRIPTEN__
    return B3_RT_WEB_OPTION;
#else
    return 1;
#endif
}

/* Rewrite the file, keeping every key it already had that is not ours.  A
 * settings file that lost the caller's other entries every time one of them
 * changed would be a settings file nobody could add to. */
void b3_rt_save(void)
{
    char  keys[16][32];
    float vals[16];
    int   n = 0, i;
    FILE *f;

    rt_read();

    f = fopen(B3_RT_CFG, "r");
    if (f) {
        char  k[32];
        float v;
        while (n < 16 && fscanf(f, "%31s %f", k, &v) == 2) {
            if (!strcmp(k, "raytracing")) continue;
            snprintf(keys[n], sizeof keys[n], "%s", k);
            vals[n] = v;
            n++;
        }
        fclose(f);
    }

    f = fopen(B3_RT_CFG, "w");
    if (!f) return;
    fprintf(f, "raytracing %d\n", g_want ? 1 : 0);
    for (i = 0; i < n; i++) fprintf(f, "%s %.4f\n", keys[i], vals[i]);
    fclose(f);
}

/* ------------------------------------------------------------- the knobs */

static float rt_envf(const char *name, float def)
{
    const char *e = getenv(name);
    return (e && *e) ? (float)atof(e) : def;
}

/* A CLAMP THAT SAYS SO, once.
 *
 * b3_rt_knobs() runs every frame, so a clamp that printed every time would be
 * a scrolling wall; one that printed never is how a user came to set
 * B3_RT_RAYS=128, watch the frame rate not move, and conclude the knob was
 * dead.  It was not dead, it was silently 16.  One line per knob per process
 * is the whole of the fix: `seen` is a bit per call site.
 *
 * The message names the value the user ASKED FOR, the value they GOT and the
 * reason -- because "clamped to 16" without the reason invites the next
 * question rather than answering it. */
static unsigned g_clamp_said;

static float rt_clamp_say(const char *name, float want, float lo, float hi,
                          unsigned bit, const char *why)
{
    float got = want < lo ? lo : (want > hi ? hi : want);
    if (got != want && !(g_clamp_said & bit)) {
        g_clamp_said |= bit;
        printf("[rt] %s=%g clamped to %g (%s)\n", name, (double)want,
               (double)got, why);
        fflush(stdout);
    }
    return got;
}

void b3_rt_knobs(B3RtKnobs *out)
{
    float rays, steps;
    if (!out) return;
    rays  = rt_envf("B3_RT_RAYS",  (float)B3_RT_RAYS);
    steps = rt_envf("B3_RT_STEPS", (float)B3_RT_STEPS);
    out->rays  = (int)rt_clamp_say("B3_RT_RAYS", rays, 1.0f,
                                   (float)B3_RT_RAYS_MAX, 1u,
                                   "the spliced loop bound's ceiling"
#ifdef __EMSCRIPTEN__
                                   " on the web"
#endif
                                   );
    out->steps = (int)rt_clamp_say("B3_RT_STEPS", steps, 8.0f,
                                   (float)B3_RT_STEPS_MAX, 2u,
                                   "the traversal budget's ceiling");
    out->sun_deg  = rt_clamp_say("B3_RT_SUN_DEG",
                                 rt_envf("B3_RT_SUN_DEG", B3_RT_SUN_DEG),
                                 0.0f, 90.0f, 4u, "an angular radius");
    out->range    = rt_clamp_say("B3_RT_RANGE",
                                 rt_envf("B3_RT_RANGE", B3_RT_RANGE),
                                 1.0f, 1.0e6f, 8u, "metres");
    out->origin   = rt_envf("B3_RT_ORIGIN",   B3_RT_ORIGIN);
    out->strength = rt_envf("B3_RT_STRENGTH", B3_RT_STRENGTH);
    /* THE FAR FIELD's three; the .h carries the whole argument and
     * tools/rt_shimmer.py carries the measurement that chose them. */
    out->dist_bias = rt_envf("B3_RT_DIST_BIAS", B3_RT_DIST_BIAS);
    out->slope     = rt_clamp_say("B3_RT_SLOPE",
                                  rt_envf("B3_RT_SLOPE", B3_RT_SLOPE),
                                  0.0f, 8.0f, 16u, "a blend toward 1/N.L");
    out->graze     = rt_clamp_say("B3_RT_GRAZE",
                                  rt_envf("B3_RT_GRAZE", B3_RT_GRAZE),
                                  0.0f, 8.0f, 32u, "a blend toward 1/|N.V|");
    if (out->dist_bias < 0.0f) out->dist_bias = 0.0f;
}

/* ============================================================= THE WORLD */

#define RT_HDR       0x50
#define RT_NODE_REC  0x30
#define RT_TRI_REC   0x30

static float   *g_node;          /* 8 floats a node, 2 texels             */
static float   *g_tri;           /* 12 floats a triangle, 3 texels        */
static int      g_nnode, g_ntri, g_depth;
static float    g_lo[3], g_hi[3];
static int      g_tex_w = 2048;
static unsigned g_gen;           /* bumped by every successful load       */
static const char *g_status = "no world loaded";

static unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static float rd_f32(const unsigned char *p)
{
    unsigned u = rd_u32(p);
    float    f;
    memcpy(&f, &u, sizeof f);
    return f;
}

void b3_rt_tex_set_width(int w)
{
    int v = 64;
    if (w < 64) w = 64;
    while (v * 2 <= w) v *= 2;      /* the next power of two at or below w */
    g_tex_w = v;
}

int b3_rt_tex_w(void)      { return g_tex_w; }
int b3_rt_node_count(void) { return g_nnode; }
int b3_rt_tri_count(void)  { return g_ntri; }
int b3_rt_max_depth(void)  { return g_depth; }
int b3_rt_node_texels(void){ return g_nnode * 2; }
int b3_rt_tri_texels(void) { return g_ntri * 3; }
const float *b3_rt_node_data(void) { return g_node; }
const float *b3_rt_tri_data(void)  { return g_tri; }
int b3_rt_world_ready(void) { return g_node && g_tri && g_nnode && g_ntri; }
unsigned b3_rt_world_gen(void) { return g_gen; }
const char *b3_rt_status(void) { return b3_rt_world_ready() ? NULL : g_status; }

int b3_rt_node_rows(void)
{
    return (b3_rt_node_texels() + g_tex_w - 1) / g_tex_w;
}

int b3_rt_tri_rows(void)
{
    return (b3_rt_tri_texels() + g_tex_w - 1) / g_tex_w;
}

void b3_rt_world_bounds(float lo[3], float hi[3])
{
    int k;
    for (k = 0; k < 3; k++) { lo[k] = g_lo[k]; hi[k] = g_hi[k]; }
}

void b3_rt_world_free(void)
{
    free(g_node); g_node = NULL;
    free(g_tri);  g_tri  = NULL;
    g_nnode = g_ntri = g_depth = 0;
}

int b3_rt_world_load(const char *track_dir)
{
    char           path[1024];
    FILE          *f = NULL;
    unsigned char *d = NULL;
    long           n = 0;
    unsigned       nnode, ntri, off_nodes, off_tris, ver;
    unsigned       i;
    int            k;

    b3_rt_world_free();
    g_status = "no world loaded";
    if (!track_dir || !*track_dir) return 0;

    snprintf(path, sizeof path, "%s/bvh.bin", track_dir);
    f = fopen(path, "rb");
    if (!f) { g_status = "no bvh.bin for this track"; return 0; }
    if (fseek(f, 0, SEEK_END) != 0) goto bad;
    n = ftell(f);
    if (n < RT_HDR) goto bad;
    if (fseek(f, 0, SEEK_SET) != 0) goto bad;
    d = (unsigned char *)malloc((size_t)n);
    if (!d) { g_status = "out of memory"; goto bad; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) goto bad;
    fclose(f); f = NULL;

    if (memcmp(d, "B3BV", 4) != 0) { g_status = "bvh.bin: bad magic"; goto bad; }
    ver = rd_u32(d + 4);
    if (ver != 1u) { g_status = "bvh.bin: wrong version"; goto bad; }
    nnode     = rd_u32(d + 0x08);
    ntri      = rd_u32(d + 0x0C);
    off_nodes = rd_u32(d + 0x10);
    off_tris  = rd_u32(d + 0x14);
    g_depth   = (int)rd_u32(d + 0x3C);
    if (!nnode || !ntri) { g_status = "bvh.bin: empty"; goto bad; }
    if ((size_t)off_nodes + (size_t)nnode * RT_NODE_REC > (size_t)n ||
        (size_t)off_tris  + (size_t)ntri  * RT_TRI_REC  > (size_t)n) {
        g_status = "bvh.bin: truncated";
        goto bad;
    }

    g_node = (float *)malloc((size_t)nnode * 8 * sizeof *g_node);
    g_tri  = (float *)malloc((size_t)ntri * 12 * sizeof *g_tri);
    if (!g_node || !g_tri) { g_status = "out of memory"; goto bad; }

    /* THE ONE REFLECTION.  Every geometry artefact in this port is in raw
     * game space and every loader negates Z on the way in
     * (src/burnout3_trackmesh.c:552-568 and the note there about why the
     * game's left-handed data through a right-handed camera needs exactly
     * one).  A BOX needs its Z ENDS SWAPPED as well as negated -- the old
     * max becomes the new min -- which is the only place the reflection is
     * more than a sign, and getting it wrong makes every box empty on one
     * axis and every ray miss the world. */
    for (i = 0; i < nnode; i++) {
        const unsigned char *r = d + off_nodes + (size_t)i * RT_NODE_REC;
        float *o = g_node + (size_t)i * 8;
        unsigned escape = rd_u32(r + 0x18);
        unsigned first  = rd_u32(r + 0x1C);
        unsigned count  = rd_u32(r + 0x20);
        float lo[3], hi[3];
        for (k = 0; k < 3; k++) lo[k] = rd_f32(r + k * 4);
        for (k = 0; k < 3; k++) hi[k] = rd_f32(r + 12 + k * 4);
        o[0] = lo[0]; o[1] = lo[1]; o[2] = -hi[2];
        o[3] = (float)escape;
        o[4] = hi[0]; o[5] = hi[1]; o[6] = -lo[2];
        /* THE LEAF PACKING, and it is a contract with the shader: ESSL 1.00
         * has no bitwise operators and no floatBitsToInt, so both halves of
         * a leaf ride in one float and come back out with mod() and a
         * divide.  cx_bvh.c caps the count at 8 and refuses a track whose
         * first index would leave a highp float's 24 exact bits. */
        if (count == 0u) {
            o[7] = -1.0f;
        } else {
            if (count > 8u) count = 8u;
            o[7] = (float)first * 8.0f + (float)(count - 1u);
        }
    }

    for (i = 0; i < ntri; i++) {
        const unsigned char *r = d + off_tris + (size_t)i * RT_TRI_REC;
        float *o = g_tri + (size_t)i * 12;
        for (k = 0; k < 3; k++) {
            o[k * 4 + 0] =  rd_f32(r + k * 12 + 0);
            o[k * 4 + 1] =  rd_f32(r + k * 12 + 4);
            o[k * 4 + 2] = -rd_f32(r + k * 12 + 8);
            o[k * 4 + 3] = 0.0f;
        }
        o[3] = rd_f32(r + 0x24);              /* opacity, on v0's w slot   */
    }

    g_nnode = (int)nnode;
    g_ntri  = (int)ntri;
    g_gen++;
    for (k = 0; k < 3; k++) { g_lo[k] = g_node[k]; g_hi[k] = g_node[4 + k]; }
    g_status = NULL;
    free(d);
    return 1;

bad:
    if (f) fclose(f);
    free(d);
    b3_rt_world_free();
    return 0;
}

/* =================================================== THE REFERENCE TRACE
 *
 * The same walk AFX_LIGHT_SHADOW_RT runs, in C.  It is deliberately written
 * against the SAME buffers the textures are uploaded from -- not against the
 * file, and not against a second decode -- so a disagreement between this and
 * the shader is a shader bug and nothing else. */

float b3_rt_transmittance(const float origin[3], const float dir[3],
                          float tmax)
{
    float inv[3], trans = 1.0f;
    int   i = 0, k, guard = 0;

    if (!b3_rt_world_ready() || !origin || !dir) return 1.0f;

    for (k = 0; k < 3; k++) {
        float v = dir[k];
        if (v > -1e-9f && v < 1e-9f) v = (v < 0.0f) ? -1e-9f : 1e-9f;
        inv[k] = 1.0f / v;
    }

    while (i < g_nnode) {
        const float *A = g_node + (size_t)i * 8;
        float tn = 0.0f, tf = tmax;
        int   hit;

        if (++guard > 4000000) break;
        for (k = 0; k < 3; k++) {
            float a = (A[k] - origin[k]) * inv[k];
            float b = (A[4 + k] - origin[k]) * inv[k];
            float lo = a < b ? a : b, hi = a < b ? b : a;
            if (lo > tn) tn = lo;
            if (hi < tf) tf = hi;
        }
        hit = (tn <= tf);
        if (!hit) { i = (int)A[3]; continue; }
        if (A[7] < 0.0f) { i++; continue; }
        {
            float c = A[7] - 8.0f * (float)((int)(A[7] / 8.0f));
            int   count = (int)c + 1;
            int   first = (int)((A[7] - c) / 8.0f);
            int   j;
            for (j = 0; j < count; j++) {
                const float *T = g_tri + (size_t)(first + j) * 12;
                float e1[3], e2[3], pv[3], tv[3], qv[3];
                float det, idet, u, v, t;
                for (k = 0; k < 3; k++) {
                    e1[k] = T[4 + k] - T[k];
                    e2[k] = T[8 + k] - T[k];
                }
                pv[0] = dir[1] * e2[2] - dir[2] * e2[1];
                pv[1] = dir[2] * e2[0] - dir[0] * e2[2];
                pv[2] = dir[0] * e2[1] - dir[1] * e2[0];
                det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (det > -1e-8f && det < 1e-8f) continue;
                idet = 1.0f / det;
                for (k = 0; k < 3; k++) tv[k] = origin[k] - T[k];
                u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * idet;
                if (u < 0.0f || u > 1.0f) continue;
                qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
                qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
                qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
                v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * idet;
                if (v < 0.0f || u + v > 1.0f) continue;
                t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * idet;
                if (t <= 1e-4f || t >= tmax) continue;
                trans *= 1.0f - T[3];
                if (trans < 0.004f) return 0.0f;
            }
        }
        i = (int)A[3];
    }
    return trans;
}

/* =============================================================== THE CARS
 *
 * The fleet file and the PACKED subset.  See burnout3_rt.h for the design;
 * the short version is that build/cars/carbvh.bin holds a model-space BVH for
 * all 106 vehicles and a race needs about a dozen of them, so the file is kept
 * whole in memory and a SELECTION is packed for the GPU.
 *
 * GL-FREE like everything else here: this produces a float buffer and some
 * indices, and src/burnout3_aftereffects.c decides what to do with them. */

#define RTC_HDR       0x40
#define RTC_MODEL_REC 0x50
#define RTC_NAME      32

typedef struct {
    char     name[RTC_NAME + 1];
    unsigned node_first, node_count, tri_first, tri_count;
    float    lo[3], hi[3], radius;
    unsigned flags;
} RtCarModel;

/* the FILE, kept whole: re-reading 24 MB on a car swap is not worth saving */
static RtCarModel  *g_car_model;
static int          g_car_nmodel;
static float       *g_car_fnode;      /* 8 floats a node, file order   */
static float       *g_car_ftri;       /* 12 floats a triangle          */
static int          g_car_fnnode, g_car_fntri;
static const char  *g_car_status = "no fleet loaded";

/* the PACKED selection, laid out for one texture */
#define RTC_SEL_MAX 32
static B3RtCarModel g_car_sel[RTC_SEL_MAX];
static int          g_car_sel_src[RTC_SEL_MAX];   /* -> g_car_model index */
static int          g_car_sel_n;
static float       *g_car_pack;       /* nodes then triangles, texel-major */
static int          g_car_pnode, g_car_ptri;
static unsigned     g_car_gen;

int b3_rt_cars_ready(void) { return g_car_model && g_car_nmodel > 0; }
int b3_rt_car_selected(void) { return g_car_sel_n; }
int b3_rt_car_fleet_count(void) { return g_car_nmodel; }
unsigned b3_rt_car_gen(void) { return g_car_gen; }
int b3_rt_car_node_count(void) { return g_car_pnode; }
int b3_rt_car_tri_count(void) { return g_car_ptri; }
int b3_rt_car_tri_base(void) { return g_car_pnode * 2; }
int b3_rt_car_texels(void) { return g_car_pnode * 2 + g_car_ptri * 3; }
const float *b3_rt_car_data(void) { return g_car_pack; }

int b3_rt_car_rows(void)
{
    return (b3_rt_car_texels() + g_tex_w - 1) / g_tex_w;
}

const char *b3_rt_cars_status(void)
{
    return b3_rt_cars_ready() ? NULL : g_car_status;
}

/* WHICH CARS, as one word.  Latched, so a harness cannot get two answers in
 * one frame -- the same rule b3_rt_want() follows.  A word this does not know
 * is REPORTED rather than silently taken as the default: an operator who
 * typed "traffic" expecting "all" would otherwise measure the default and
 * believe it was measuring traffic. */
int b3_rt_cars_mode(void)
{
    static int mode = -1;
    const char *e;
    if (mode >= 0) return mode;
    mode = B3_RT_CARS_RACERS;            /* the shipped default */
    e = getenv("B3_RT_CARS");
    if (e && *e) {
        if      (!strcmp(e, "off")  || !strcmp(e, "0")) mode = B3_RT_CARS_OFF;
        else if (!strcmp(e, "all"))                     mode = B3_RT_CARS_ALL;
        else if (!strcmp(e, "racers") || !strcmp(e, "1"))
                                                        mode = B3_RT_CARS_RACERS;
        else {
            printf("[rt] B3_RT_CARS=%s is not one of off/racers/all -- "
                   "keeping racers\n", e);
            fflush(stdout);
        }
    }
    return mode;
}

void b3_rt_cars_free(void)
{
    free(g_car_model); g_car_model = NULL;
    free(g_car_fnode); g_car_fnode = NULL;
    free(g_car_ftri);  g_car_ftri  = NULL;
    free(g_car_pack);  g_car_pack  = NULL;
    g_car_nmodel = g_car_fnnode = g_car_fntri = 0;
    g_car_sel_n = g_car_pnode = g_car_ptri = 0;
}

int b3_rt_cars_load(const char *cars_dir)
{
    char           path[1024];
    FILE          *f = NULL;
    unsigned char *d = NULL;
    long           n = 0;
    unsigned       nm, nn, nt, om, on, ot, ver;
    unsigned       i;
    int            k;

    b3_rt_cars_free();
    g_car_status = "no fleet loaded";
    if (!cars_dir || !*cars_dir) return 0;

    snprintf(path, sizeof path, "%s/carbvh.bin", cars_dir);
    f = fopen(path, "rb");
    if (!f) { g_car_status = "no carbvh.bin"; return 0; }
    if (fseek(f, 0, SEEK_END) != 0) goto bad;
    n = ftell(f);
    if (n < RTC_HDR) goto bad;
    if (fseek(f, 0, SEEK_SET) != 0) goto bad;
    d = (unsigned char *)malloc((size_t)n);
    if (!d) { g_car_status = "out of memory"; goto bad; }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) goto bad;
    fclose(f); f = NULL;

    if (memcmp(d, "B3CV", 4) != 0) {
        g_car_status = "carbvh.bin: bad magic"; goto bad;
    }
    ver = rd_u32(d + 4);
    if (ver != 1u) { g_car_status = "carbvh.bin: wrong version"; goto bad; }
    nm = rd_u32(d + 0x08); nn = rd_u32(d + 0x0C); nt = rd_u32(d + 0x10);
    om = rd_u32(d + 0x14); on = rd_u32(d + 0x18); ot = rd_u32(d + 0x1C);
    if (!nm || !nn || !nt) { g_car_status = "carbvh.bin: empty"; goto bad; }
    if ((size_t)om + (size_t)nm * RTC_MODEL_REC > (size_t)n ||
        (size_t)on + (size_t)nn * RT_NODE_REC   > (size_t)n ||
        (size_t)ot + (size_t)nt * RT_TRI_REC    > (size_t)n) {
        g_car_status = "carbvh.bin: truncated";
        goto bad;
    }

    g_car_model = (RtCarModel *)calloc(nm, sizeof *g_car_model);
    g_car_fnode = (float *)malloc((size_t)nn * 8 * sizeof *g_car_fnode);
    g_car_ftri  = (float *)malloc((size_t)nt * 12 * sizeof *g_car_ftri);
    if (!g_car_model || !g_car_fnode || !g_car_ftri) {
        g_car_status = "out of memory"; goto bad;
    }

    /* THE ONE REFLECTION, exactly as b3_rt_world_load() applies it and for
     * exactly the same reason: the artefact is raw game space (+Z the nose)
     * and every geometry loader in this port negates Z on the way in.  A BOX
     * needs its Z ENDS SWAPPED as well as negated -- the old max becomes the
     * new min -- which is the one place the reflection is more than a sign. */
    for (i = 0; i < nm; i++) {
        const unsigned char *r = d + om + (size_t)i * RTC_MODEL_REC;
        RtCarModel *m = &g_car_model[i];
        float lo[3], hi[3];
        memcpy(m->name, r, RTC_NAME);
        m->name[RTC_NAME] = 0;
        m->node_first = rd_u32(r + 0x20);
        m->node_count = rd_u32(r + 0x24);
        m->tri_first  = rd_u32(r + 0x28);
        m->tri_count  = rd_u32(r + 0x2C);
        for (k = 0; k < 3; k++) lo[k] = rd_f32(r + 0x30 + k * 4);
        for (k = 0; k < 3; k++) hi[k] = rd_f32(r + 0x3C + k * 4);
        m->lo[0] = lo[0]; m->lo[1] = lo[1]; m->lo[2] = -hi[2];
        m->hi[0] = hi[0]; m->hi[1] = hi[1]; m->hi[2] = -lo[2];
        m->radius = rd_f32(r + 0x48);
        m->flags  = rd_u32(r + 0x4C);
        if (m->node_first + m->node_count > nn ||
            m->tri_first  + m->tri_count  > nt) {
            g_car_status = "carbvh.bin: a model points outside the file";
            goto bad;
        }
    }

    for (i = 0; i < nn; i++) {
        const unsigned char *r = d + on + (size_t)i * RT_NODE_REC;
        float *o = g_car_fnode + (size_t)i * 8;
        unsigned escape = rd_u32(r + 0x18);
        unsigned first  = rd_u32(r + 0x1C);
        unsigned count  = rd_u32(r + 0x20);
        float lo[3], hi[3];
        for (k = 0; k < 3; k++) lo[k] = rd_f32(r + k * 4);
        for (k = 0; k < 3; k++) hi[k] = rd_f32(r + 12 + k * 4);
        o[0] = lo[0]; o[1] = lo[1]; o[2] = -hi[2];
        o[3] = (float)escape;
        o[4] = hi[0]; o[5] = hi[1]; o[6] = -lo[2];
        /* THE LEAF PACKING is left in FILE indices here.  b3_rt_car_select()
         * rewrites both it and the escape onto the PACKED arrays, because
         * that is the only point at which the packed indices exist. */
        if (count == 0u) {
            o[7] = -1.0f;
        } else {
            if (count > 8u) count = 8u;
            o[7] = (float)first * 8.0f + (float)(count - 1u);
        }
    }

    for (i = 0; i < nt; i++) {
        const unsigned char *r = d + ot + (size_t)i * RT_TRI_REC;
        float *o = g_car_ftri + (size_t)i * 12;
        for (k = 0; k < 3; k++) {
            o[k * 4 + 0] =  rd_f32(r + k * 12 + 0);
            o[k * 4 + 1] =  rd_f32(r + k * 12 + 4);
            o[k * 4 + 2] = -rd_f32(r + k * 12 + 8);
            o[k * 4 + 3] = 0.0f;
        }
        o[3] = rd_f32(r + 0x24);              /* opacity, on v0's w slot   */
    }

    g_car_nmodel = (int)nm;
    g_car_fnnode = (int)nn;
    g_car_fntri  = (int)nt;
    g_car_status = NULL;
    free(d);
    return 1;

bad:
    if (f) fclose(f);
    free(d);
    b3_rt_cars_free();
    return 0;
}

/* The model table is written sorted by name, so this is a bisection.  -1 when
 * the fleet file has no such model, which is not an error: cx_car_bvh.c
 * refuses a vehicle whose mesh fails its plausibility gate, and the renderer
 * draws a box for exactly the same ones. */
int b3_rt_car_model_find(const char *name)
{
    int lo = 0, hi = g_car_nmodel - 1;
    if (!g_car_model || !name) return -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(g_car_model[mid].name, name);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int b3_rt_car_packed(int slot, B3RtCarModel *out)
{
    if (!out || slot < 0 || slot >= g_car_sel_n) return 0;
    *out = g_car_sel[slot];
    return 1;
}

/* PACK THE SELECTION.
 *
 * Every chosen model's nodes and triangles are copied into one pair of runs,
 * and both halves of every index are REWRITTEN onto the packed arrays: a
 * node's escape (a global node index) and a leaf's `first` (a global triangle
 * index, riding inside the float packing).  That rewrite is the whole of what
 * makes a selection possible -- the file's indices are the file's, and the
 * shader only ever sees the packed ones.
 *
 * The leaf's packing is RE-DONE rather than adjusted, because
 * `first * 8 + (count - 1)` is not something an offset can be added to. */
int b3_rt_car_select(const char *const *names, int n, int *out_slot)
{
    int    i, s;
    size_t nnode = 0, ntri = 0;

    g_car_sel_n = g_car_pnode = g_car_ptri = 0;
    free(g_car_pack); g_car_pack = NULL;
    if (!b3_rt_cars_ready() || !names || n <= 0) {
        if (out_slot) for (i = 0; i < n; i++) out_slot[i] = -1;
        return 0;
    }

    for (i = 0; i < n; i++) {
        int src = names[i] ? b3_rt_car_model_find(names[i]) : -1;
        int hit = -1;
        if (src >= 0) {
            for (s = 0; s < g_car_sel_n; s++)
                if (g_car_sel_src[s] == src) { hit = s; break; }
            if (hit < 0 && g_car_sel_n < RTC_SEL_MAX) {
                hit = g_car_sel_n++;
                g_car_sel_src[hit] = src;
            }
        }
        if (out_slot) out_slot[i] = hit;
    }
    if (!g_car_sel_n) return 0;

    for (s = 0; s < g_car_sel_n; s++) {
        const RtCarModel *m = &g_car_model[g_car_sel_src[s]];
        nnode += m->node_count;
        ntri  += m->tri_count;
    }
    g_car_pack = (float *)malloc((nnode * 2 + ntri * 3) * 4
                                 * sizeof *g_car_pack);
    if (!g_car_pack) { g_car_sel_n = 0; return 0; }

    {
        size_t nout = 0, tout = 0;
        for (s = 0; s < g_car_sel_n; s++) {
            const RtCarModel *m = &g_car_model[g_car_sel_src[s]];
            size_t base_n = nout, base_t = tout, j;
            for (j = 0; j < m->node_count; j++) {
                const float *in = g_car_fnode
                                + (size_t)(m->node_first + j) * 8;
                float *o = g_car_pack + (base_n + j) * 8;
                memcpy(o, in, 8 * sizeof *o);
                o[3] = (float)(base_n + (size_t)in[3]
                               - (size_t)m->node_first);
                if (in[7] >= 0.0f) {
                    float    cc = in[7] - 8.0f * (float)((int)(in[7] / 8.0f));
                    unsigned first = (unsigned)((in[7] - cc) / 8.0f);
                    o[7] = (float)(base_t + first - m->tri_first) * 8.0f + cc;
                }
            }
            nout += m->node_count;
            memcpy(g_car_pack + nnode * 8 + base_t * 12,
                   g_car_ftri + (size_t)m->tri_first * 12,
                   (size_t)m->tri_count * 12 * sizeof *g_car_pack);
            tout += m->tri_count;

            memcpy(g_car_sel[s].lo, m->lo, sizeof m->lo);
            memcpy(g_car_sel[s].hi, m->hi, sizeof m->hi);
            g_car_sel[s].radius    = m->radius;
            g_car_sel[s].root      = (float)base_n;
            g_car_sel[s].end       = (float)(base_n + m->node_count);
            g_car_sel[s].tri_count = (int)m->tri_count;
            snprintf(g_car_sel[s].name, sizeof g_car_sel[s].name, "%s",
                     m->name);
        }
    }
    g_car_pnode = (int)nnode;
    g_car_ptri  = (int)ntri;
    g_car_gen++;
    return g_car_sel_n;
}

/* The reference trace over one PACKED model, in that model's own space.  The
 * same stackless escape walk b3_rt_transmittance() runs and the same walk the
 * shader runs, against the same buffer the texture is uploaded from -- so a
 * disagreement between this and the shader is a shader bug and nothing else. */
float b3_rt_car_transmittance(int slot, const float origin[3],
                              const float dir[3], float tmax)
{
    const float *NODE, *TRI;
    float inv[3], trans = 1.0f;
    int   i, end, k, guard = 0;

    if (slot < 0 || slot >= g_car_sel_n || !g_car_pack || !origin || !dir)
        return 1.0f;
    NODE = g_car_pack;
    TRI  = g_car_pack + (size_t)g_car_pnode * 8;
    i    = (int)g_car_sel[slot].root;
    end  = (int)g_car_sel[slot].end;

    for (k = 0; k < 3; k++) {
        float v = dir[k];
        if (v > -1e-9f && v < 1e-9f) v = (v < 0.0f) ? -1e-9f : 1e-9f;
        inv[k] = 1.0f / v;
    }

    while (i < end) {
        const float *A = NODE + (size_t)i * 8;
        float tn = 0.0f, tf = tmax;
        int   hit;

        if (++guard > 1000000) break;
        for (k = 0; k < 3; k++) {
            float a = (A[k] - origin[k]) * inv[k];
            float b = (A[4 + k] - origin[k]) * inv[k];
            float lo = a < b ? a : b, hi = a < b ? b : a;
            if (lo > tn) tn = lo;
            if (hi < tf) tf = hi;
        }
        hit = (tn <= tf);
        if (!hit) { i = (int)A[3]; continue; }
        if (A[7] < 0.0f) { i++; continue; }
        {
            float c = A[7] - 8.0f * (float)((int)(A[7] / 8.0f));
            int   count = (int)c + 1;
            int   first = (int)((A[7] - c) / 8.0f);
            int   j;
            for (j = 0; j < count; j++) {
                const float *T = TRI + (size_t)(first + j) * 12;
                float e1[3], e2[3], pv[3], tv[3], qv[3];
                float det, idet, u, v, t;
                for (k = 0; k < 3; k++) {
                    e1[k] = T[4 + k] - T[k];
                    e2[k] = T[8 + k] - T[k];
                }
                pv[0] = dir[1] * e2[2] - dir[2] * e2[1];
                pv[1] = dir[2] * e2[0] - dir[0] * e2[2];
                pv[2] = dir[0] * e2[1] - dir[1] * e2[0];
                det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (det > -1e-8f && det < 1e-8f) continue;
                idet = 1.0f / det;
                for (k = 0; k < 3; k++) tv[k] = origin[k] - T[k];
                u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * idet;
                if (u < 0.0f || u > 1.0f) continue;
                qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
                qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
                qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
                v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * idet;
                if (v < 0.0f || u + v > 1.0f) continue;
                t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * idet;
                if (t <= 1e-4f || t >= tmax) continue;
                trans *= 1.0f - T[3];
                if (trans < 0.004f) return 0.0f;
            }
        }
        i = (int)A[3];
    }
    return trans;
}

/* ---------------------------------------------------------- the arming line
 *
 * Once per process, whatever the answer.  See the .h for why it is
 * unconditional: a user tuning B3_RT_* had no way to tell a knob that does
 * nothing from a FEATURE that is not running, and those look identical from
 * the outside. */
void b3_rt_announce(int on, const char *why)
{
    static int said;
    B3RtKnobs  k;

    if (said) return;
    said = 1;
    b3_rt_knobs(&k);
    if (on) {
        printf("[rt] ray tracing ON -- %d ray%s, sun %.3g deg, world %d tris",
               k.rays, k.rays == 1 ? "" : "s", (double)k.sun_deg, g_ntri);
        if (b3_rt_cars_ready() && g_car_sel_n > 0)
            printf(", %d car model%s (%d tris)", g_car_sel_n,
                   g_car_sel_n == 1 ? "" : "s", g_car_ptri);
        else if (b3_rt_cars_mode() == B3_RT_CARS_OFF)
            printf(", cars OFF (B3_RT_CARS)");
        else
            printf(", no car trees (%s)", b3_rt_cars_status());
        printf("\n");
    } else {
        printf("[rt] ray tracing OFF (%s)\n",
               why && *why ? why : "the settings menu; B3_RT=1 overrides");
    }
    fflush(stdout);
}
