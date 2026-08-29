/* burnout3_scenery.c -- see burnout3_scenery.h for what this is and why.
 *
 * scenery.bin ('B3SC' v1), written by tools/cextract/cx_scenery.c:
 *   header 0x30: magic, version, model_count, instance_count, vertex_count,
 *                index_count, off_models, off_instances, off_vertices,
 *                off_indices, unit_count, reserved
 *   model 0x60 : f32[3] bb_min, f32[3] bb_max, u32 first_vertex, vertex_count,
 *                first_index, index_count (MODEL-LOCAL index values),
 *                u32 record, f32 lod_near, lod_far, u32 mat_flags,
 *                u32 shader_class, u32 lod_flags, char[32] texture
 *   inst  0x50 : f32[16] 4x4 row-major (rows right/up/at, row3 position),
 *                u32 model, u32 unit, u32 record, u32 reserved
 *   vertex 0x20: f32 pos[3], normal[3], uv[2]      index: u16
 *
 * Everything is RAW GAME SPACE; the Z reflection into this harness' GL frame
 * is done here, byte for byte the way src/burnout3_props.c does it (vertices
 * negate z; the instance matrix negates the elements with exactly one z
 * index, which conjugates the transform so it composes with the reflected
 * local vertices).
 *
 * The four `w` slots of the matrix are NOT transform: they carry the
 * instance's baked HALF-RANGE colour, the same convention props.bin uses
 * (FUN_0011A020 saves/restores them at 0x0011A03A) -- measured 0.21..0.39
 * across US_C1_V1's 1370 instances, i.e. 0.42..0.79 doubled.  Doubled here
 * for the same reason the world's stage-0 combiner doubles a vertex colour.
 *
 * DISTANCE CULL, GLUE.  Retail's own draw of this table is FUN_001ADA40's
 * LOD2/impostor pass and its gate is `dist2 <= ctx+0xD4`, fed from the
 * record's +0x68 far distance (FUN_0003A840 @0x0003A87E..0x0003A8B0 [C]); the
 * exact scaling FUN_0003a740 applies to it does not decompile unambiguously,
 * so this module culls at the record's raw +0x68 far distance and says so.
 * B3_SCENERY_FAR overrides it (metres), B3_SCENERY=0 disables the pass.
 */
#include "burnout3_scenery.h"
/* RETAINED RENDERER: 523 of this pass' draws a frame were 22% of the whole
 * port's WebGL traffic (docs/web/webprof_sweep.md section 6).  The instance
 * transforms never move, so b3r bakes them into world space once and merges
 * the survivors of the LOD cull into a handful of glDrawArrays calls. */
#include "burnout3_render.h"
/* TIER 7's thresholds and its switch.  The derivation happens HERE because
 * this is the only module that can see a scenery model's texture and its
 * vertices at the same moment; the numbers it derives with live over there
 * with the rest of the look.  See b3_scenery_lights in the header. */
#include "burnout3_aftereffects.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
/* ANDROID / WEB PORT: gl4es supplies the desktop GL surface this file draws
 * through.  SDL_opengl.h declares GL 1.1 ITSELF rather than including
 * <GL/gl.h>, so on those targets it would introduce a second, UNMANGLED set
 * of gl* names -- on the web those bind to Emscripten's own WebGL symbols,
 * which know nothing of gl4es' display lists or fixed-function state.  Take
 * the same <GL/gl.h> every other renderer here takes. */
#include <GL/gl.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B3S_HDR   0x30
#define B3S_MODEL 0x60
#define B3S_INST  0x50
#define B3S_VTX   0x20

typedef struct {
    float    bb_min[3], bb_max[3];
    unsigned first_vertex, n_vertex, first_index, n_index;
    unsigned record;
    float    lod_near, lod_far;
    unsigned mat_flags, cls, lod_flags;
    char     texture[32];
    unsigned tex;
    float    radius;               /* |bbox| about the model origin */
} B3ScModel;

typedef struct {
    float    m[16];                /* GL-ready, already Z-reflected */
    float    tint[3];
    float    pos[3];               /* m[12..14], hoisted for the cull */
    unsigned model, unit, record;
} B3ScInst;

static B3ScModel* g_model;
static B3ScInst*  g_inst;
static float*     g_vtx;           /* n_vertex * 8: pos, normal, uv */
static unsigned short* g_idx;
static int        g_nmodel, g_ninst;
static unsigned   g_nvtx, g_nidx;
static int        g_ready;
static float      g_far_override = -1.0f;
static char       g_dir[256];
static B3RInstSet* g_retained;
static B3ScLight* g_light;         /* tier 7's derived field, see the header */
static int        g_nlight;

static float rd_f32(const unsigned char* p)
{
    float f;
    memcpy(&f, p, 4);
    return f;
}
static unsigned rd_u32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8)
         | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* ---- THE EMISSIVE SCAN (photorealism tier 7) ---------------------------
 *
 * Built while the texture is on the CPU anyway, because that is the only
 * moment its texels are reachable: after the upload it is a GL name, and
 * glGetTexImage does not exist on GLES or in a browser.
 *
 * The mask is deliberately COARSE -- 64x64 regardless of the texture's own
 * size.  What it is used for is "did this vertex's uv land on the bulb", and
 * a vertex is a corner of a triangle several texels wide; a full-resolution
 * mask would answer a more precise question than the geometry can ask.
 */
#define B3S_EM 64
typedef struct {
    int   found;
    float rgb[3];                  /* mean colour of the emissive texels    */
    float lum;                     /* ...its luminance, BEFORE normalising  */
    float mean;                    /* the sheet's own opaque mean           */
    float frac;                    /* fraction of the texture they are      */
    unsigned char m[B3S_EM * B3S_EM];
} B3ScEmis;

static void scenery_lights_build(const B3ScEmis* em);

/* WHAT COUNTS AS A BULB, and the third test is the one that matters.
 *
 * "Bright and opaque" alone calls a white van a light.  What separates a lamp
 * from a white object is CONTRAST WITHIN THE TEXTURE: a bulb is a bright patch
 * on a dark thing.  So a model qualifies only if its texture is dark on
 * average (B3_PHOTO_LIGHT_EMIS_DK) and the emissive texels sit a good way
 * above that mean (B3_PHOTO_LIGHT_EMIS_DL) as well as above an absolute floor
 * (B3_PHOTO_LIGHT_EMIS).  Two more bounds catch the ends: a handful of stray
 * bright texels is compression noise, and a texture that is bright over a
 * fifth of its area is a wall.
 *
 * ALPHA MATTERS.  These sheets are cut-outs -- a chain-link fence, a foliage
 * card -- and a transparent texel is not painted, it is absent.  The mean is
 * taken over the OPAQUE texels only, or every cut-out reads as "a bright thing
 * on a black background" and the whole tree lights up.
 */
static void scenery_scan_emissive(SDL_Surface* c, B3ScEmis* em)
{
    B3PhotoLightRules r;
    const unsigned char* px = (const unsigned char*)c->pixels;
    double sum = 0.0, esum[3] = { 0.0, 0.0, 0.0 };
    long   opaque = 0, hot = 0;
    int    x, y, pass;
    float  mean, cut;

    b3_photo_light_rules(&r);
    if (c->w <= 0 || c->h <= 0) return;

    for (y = 0; y < c->h; y++) {
        const unsigned char* row = px + (size_t)y * (size_t)c->pitch;
        for (x = 0; x < c->w; x++) {
            const unsigned char* p = row + x * 4;   /* ABGR8888: r,g,b,a */
            if (p[3] < 128) continue;
            sum += (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
            opaque++;
        }
    }
    if (opaque < 64) return;
    mean = (float)(sum / (double)opaque);
    if (mean >= r.emis_dark) return;          /* a bright OBJECT, not a bulb */
    cut = mean + r.emis_lift;
    if (cut < r.emis) cut = r.emis;
    if (cut >= 1.0f) return;

    /* two passes: count first so the fraction test can reject before the mask
     * is written, then write the mask and accumulate the colour */
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            float frac = (float)hot / (float)opaque;
            if (frac < r.emis_min || frac > r.emis_max) return;
            em->frac = frac;
        }
        for (y = 0; y < c->h; y++) {
            const unsigned char* row = px + (size_t)y * (size_t)c->pitch;
            for (x = 0; x < c->w; x++) {
                const unsigned char* p = row + x * 4;
                float l;
                if (p[3] < 128) continue;
                l = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f;
                if (l < cut) continue;
                if (pass == 0) { hot++; continue; }
                esum[0] += p[0]; esum[1] += p[1]; esum[2] += p[2];
                em->m[(y * B3S_EM / c->h) * B3S_EM + (x * B3S_EM / c->w)] = 1;
            }
        }
        if (pass == 0 && hot == 0) return;
    }
    em->rgb[0] = (float)(esum[0] / (double)hot / 255.0);
    em->rgb[1] = (float)(esum[1] / (double)hot / 255.0);
    em->rgb[2] = (float)(esum[2] / (double)hot / 255.0);
    em->mean   = mean;
    em->lum    = 0.299f * em->rgb[0] + 0.587f * em->rgb[1]
               + 0.114f * em->rgb[2];
    /* NORMALISE THE HUE, keep the colour.  A sodium lamp's texels are
     * (1.0, 0.72, 0.35) and a fluorescent's are (0.95, 0.97, 1.0); what the
     * light should carry is the RATIO, with the brightness coming from the
     * area and the tier's own gain.  Dividing by the peak channel keeps
     * "orange" and "white" apart and stops a dim bulb being a dim light. */
    {   float mx = em->rgb[0];
        if (em->rgb[1] > mx) mx = em->rgb[1];
        if (em->rgb[2] > mx) mx = em->rgb[2];
        if (mx > 1e-3f) { em->rgb[0] /= mx; em->rgb[1] /= mx; em->rgb[2] /= mx; }
    }
    em->found = 1;
}

/* Same two-place texture search src/burnout3_props.c uses. */
static unsigned load_tex(const char* dir, const char* name, B3ScEmis* em)
{
    char path[768];
    SDL_Surface* s;
    SDL_Surface* c;
    unsigned id = 0;

    if (em) memset(em, 0, sizeof *em);
    if (!name || !name[0]) return 0;
    snprintf(path, sizeof path, "%s/textures/%s.png", dir, name);
    s = IMG_Load(path);
    if (!s) {
        snprintf(path, sizeof path, "build/textures/%s.png", name);
        s = IMG_Load(path);
    }
    if (!s) return 0;
    c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c) return 0;
    if (em) scenery_scan_emissive(c, em);
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
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

/* ---- FROM A MASK TO A LIGHT FIELD --------------------------------------
 *
 * Per model: find the vertices whose uv lands on the bulb and take their
 * centroid -- that is the bulb, in model space.  Then instance it.
 *
 * WHY THE VERTICES AND NOT THE BOUNDING BOX.  A lamp post's bbox centre is
 * halfway down the pole and its top is the top of the pole, neither of which
 * is where the light is; a traffic-light gantry's bbox centre is out over the
 * road with nothing there at all.  The vertices that carry the bulb's texels
 * ARE the bulb -- it is the only construction that is right for a lamp, a
 * neon sign, a lit shopfront window and a traffic-light head at the same
 * time, without a single word about which model is which.
 *
 * A model whose emissive texels attract fewer than three vertices is dropped.
 * Two vertices are an edge, and an edge is what a compression artefact along
 * a bright seam looks like; three is the first number that is a piece of a
 * surface.
 */
static void scenery_lights_build(const B3ScEmis* em)
{
    B3PhotoLightRules r;
    float (*bulb)[3];
    float* rad;
    float* spread;
    int*   ok;
    int i, n = 0, verbose;

    free(g_light); g_light = NULL; g_nlight = 0;
    if (!b3_photo_fx(B3_PHOTO_FX_LIGHTS) || g_nmodel <= 0 || g_ninst <= 0)
        return;
    b3_photo_light_rules(&r);
    verbose = getenv("B3_PHOTO_VERBOSE") != NULL;

    bulb   = (float(*)[3])calloc((size_t)g_nmodel, sizeof *bulb);
    rad    = (float*)calloc((size_t)g_nmodel, sizeof *rad);
    spread = (float*)calloc((size_t)g_nmodel, sizeof *spread);
    ok     = (int*)calloc((size_t)g_nmodel, sizeof *ok);
    if (!bulb || !rad || !spread || !ok) {
        free(bulb); free(rad); free(spread); free(ok); return;
    }

    for (i = 0; i < g_nmodel; i++) {
        const B3ScModel* m = &g_model[i];
        double s[3] = { 0.0, 0.0, 0.0 };
        long   hit = 0;
        unsigned v;
        if (!em[i].found) continue;
        for (v = 0; v < m->n_vertex; v++) {
            const float* p = g_vtx + (size_t)(m->first_vertex + v) * 8;
            /* uv WRAPS -- these sheets are GL_REPEAT and a uv of 3.4 is a
             * texture tiled three times, so the fractional part is the texel.
             * fmodf on a negative gives a negative, hence the fold. */
            float u = fmodf(p[6], 1.0f), w = fmodf(p[7], 1.0f);
            int   ux, uy;
            if (u < 0.0f) u += 1.0f;
            if (w < 0.0f) w += 1.0f;
            ux = (int)(u * (float)B3S_EM);
            uy = (int)(w * (float)B3S_EM);
            if (ux < 0) ux = 0;
            if (ux >= B3S_EM) ux = B3S_EM - 1;
            if (uy < 0) uy = 0;
            if (uy >= B3S_EM) uy = B3S_EM - 1;
            if (!em[i].m[uy * B3S_EM + ux]) continue;
            s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
            hit++;
        }
        if (hit < 3) continue;
        bulb[i][0] = (float)(s[0] / (double)hit);
        bulb[i][1] = (float)(s[1] / (double)hit);
        bulb[i][2] = (float)(s[2] / (double)hit);
        /* ---- IS IT A LAMP, OR IS IT A WHITE BOAT?
         *
         * The first cut of this derivation found the street lamps, the
         * traffic-light heads and the lit phone boxes -- and also five models
         * of moored boat, whose white hulls are bright, opaque and sit well
         * above a dark sheet's mean, which is every test above passed.
         *
         * What separates them is not brightness, it is EXTENT: a light source
         * is a small bright part of a bigger object.  A lamp head is a
         * third-of-a-metre blob on a ten-metre post; a hull is the boat.  So
         * the emissive vertices' spread about their own centroid, measured
         * against the model's own radius, is the test -- and it needs no new
         * data, no model names and no per-track list. */
        {   double sp = 0.0;
            float  tight;
            for (v = 0; v < m->n_vertex; v++) {
                const float* p = g_vtx + (size_t)(m->first_vertex + v) * 8;
                float u = fmodf(p[6], 1.0f), w = fmodf(p[7], 1.0f);
                int   ux, uy;
                float dx, dy, dz;
                if (u < 0.0f) u += 1.0f;
                if (w < 0.0f) w += 1.0f;
                ux = (int)(u * (float)B3S_EM);
                uy = (int)(w * (float)B3S_EM);
                if (ux < 0) ux = 0;
                if (ux >= B3S_EM) ux = B3S_EM - 1;
                if (uy < 0) uy = 0;
                if (uy >= B3S_EM) uy = B3S_EM - 1;
                if (!em[i].m[uy * B3S_EM + ux]) continue;
                dx = p[0] - bulb[i][0];
                dy = p[1] - bulb[i][1];
                dz = p[2] - bulb[i][2];
                sp += (double)(dx * dx + dy * dy + dz * dz);
            }
            tight = m->radius > 1e-3f
                  ? (float)sqrt(sp / (double)hit) / m->radius : 1.0f;
            spread[i] = tight;
            if (tight > r.tight) {
                if (verbose)
                    printf("[photo] light model %2d '%s': rejected, its "
                           "emissive texels spread %.2f of the model's own "
                           "radius -- that is a bright SURFACE, not a bulb\n",
                           i, m->texture, tight);
                continue;
            }
        }
        rad[i] = m->radius * r.reach;
        if (rad[i] < r.rmin) rad[i] = r.rmin;
        if (rad[i] > r.rmax) rad[i] = r.rmax;
        ok[i] = 1;
        if (verbose) {
            int placed = 0, k;
            for (k = 0; k < g_ninst; k++)
                if ((int)g_inst[k].model == i) placed++;
            printf("[photo] light model %2d '%s': %ld emissive verts, "
                   "rgb %.2f %.2f %.2f, lum %.3f over a sheet mean of "
                   "%.3f, %.2f%% of the sheet, spread %.2f, r %.1f m, "
                   "%d placements\n",
                   i, m->texture, hit, em[i].rgb[0], em[i].rgb[1],
                   em[i].rgb[2], em[i].lum, em[i].mean,
                   em[i].frac * 100.0f, spread[i], rad[i], placed);
        }
    }

    for (i = 0; i < g_ninst; i++)
        if (ok[g_inst[i].model]) n++;
    if (n > 0) g_light = (B3ScLight*)calloc((size_t)n, sizeof(B3ScLight));
    if (!g_light) { free(bulb); free(rad); free(spread); free(ok); return; }

    for (i = 0; i < g_ninst; i++) {
        const B3ScInst* in = &g_inst[i];
        const float* b;
        B3ScLight* L;
        unsigned mo = in->model;
        if (!ok[mo]) continue;
        b = bulb[mo];
        L = &g_light[g_nlight++];
        /* the same column-major apply b3r_inst_build bakes the mesh with, so
         * the bulb lands exactly where its own texels are drawn */
        L->pos[0] = in->m[0]*b[0] + in->m[4]*b[1] + in->m[8]*b[2]  + in->m[12];
        L->pos[1] = in->m[1]*b[0] + in->m[5]*b[1] + in->m[9]*b[2]  + in->m[13];
        L->pos[2] = in->m[2]*b[0] + in->m[6]*b[1] + in->m[10]*b[2] + in->m[14];
        L->rgb[0] = em[mo].rgb[0];
        L->rgb[1] = em[mo].rgb[1];
        L->rgb[2] = em[mo].rgb[2];
        L->radius = rad[mo];
        /* AREA, SOFTENED.  A neon frontage is a bigger light than a bulb, but
         * not fifty times bigger: the fourth root keeps the ordering and
         * throws away the magnitude, which is what the tier's own gain is
         * for. */
        L->power  = r.emis_max > 1e-6f
                  ? sqrtf(sqrtf(em[mo].frac / r.emis_max)) : 1.0f;
        if (L->power > 1.0f) L->power = 1.0f;
        L->model  = mo;
    }
    if (g_nlight) {
        int lit = 0;
        for (i = 0; i < g_nmodel; i++) if (ok[i]) lit++;
        printf("[photo] %d per-source lights derived from %d of %d scenery "
               "models (emissive texels -> bulb vertices -> placements)\n",
               g_nlight, lit, g_nmodel);
        fflush(stdout);
    }
    free(bulb); free(rad); free(spread); free(ok);
}

int b3_scenery_lights(const B3ScLight** out)
{
    if (out) *out = g_light;
    return g_nlight;
}

void b3_scenery_shutdown(void)
{
    free(g_light); g_light = NULL; g_nlight = 0;
    if (g_retained) { b3r_inst_free(g_retained); g_retained = NULL; }
    if (g_model) {
        for (int i = 0; i < g_nmodel; i++)
            if (g_model[i].tex) glDeleteTextures(1, &g_model[i].tex);
    }
    free(g_model);  g_model = NULL;
    free(g_inst);   g_inst = NULL;
    free(g_vtx);    g_vtx = NULL;
    free(g_idx);    g_idx = NULL;
    g_nmodel = g_ninst = 0;
    g_nvtx = g_nidx = 0;
    g_ready = 0;
}

int b3_scenery_ready(void)     { return g_ready; }
int b3_scenery_instances(void) { return g_ninst; }
int b3_scenery_models(void)    { return g_nmodel; }

int b3_scenery_load(const char* dir)
{
    char path[512];
    FILE* f;
    long  sz;
    unsigned char* d;
    unsigned ver, nm, ni, nv, nx, om, oi, ov, ox;

    b3_scenery_shutdown();
    {   const char* on = getenv("B3_SCENERY");
        if (on && atoi(on) == 0) {
            printf("[scenery] disabled (B3_SCENERY=0)\n");
            return 0;
        }
        const char* fo = getenv("B3_SCENERY_FAR");
        g_far_override = fo ? (float)atof(fo) : -1.0f;
    }
    if (!dir || !*dir) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(path, sizeof path, "%s/scenery.bin", dir);
    f = fopen(path, "rb");
    if (!f) {
        printf("[scenery] no %s -- run tools/cextract/build.sh && "
               "cxtract --track <ID> --only scenery --out %s\n", path, dir);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < B3S_HDR) { fclose(f); return 0; }
    d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d); fclose(f); return 0;
    }
    fclose(f);

    if (memcmp(d, "B3SC", 4) != 0) { free(d); return 0; }
    ver = rd_u32(d + 4);
    if (ver != 1) { free(d); return 0; }
    nm = rd_u32(d + 0x08);  ni = rd_u32(d + 0x0C);
    nv = rd_u32(d + 0x10);  nx = rd_u32(d + 0x14);
    om = rd_u32(d + 0x18);  oi = rd_u32(d + 0x1C);
    ov = rd_u32(d + 0x20);  ox = rd_u32(d + 0x24);
    if (!nm || (size_t)om + (size_t)nm * B3S_MODEL > (size_t)sz ||
        (size_t)oi + (size_t)ni * B3S_INST  > (size_t)sz ||
        (size_t)ov + (size_t)nv * B3S_VTX   > (size_t)sz ||
        (size_t)ox + (size_t)nx * 2         > (size_t)sz) {
        free(d);
        fprintf(stderr, "[scenery] %s: truncated\n", path);
        return 0;
    }

    g_model = (B3ScModel*)calloc(nm, sizeof(B3ScModel));
    g_inst  = ni ? (B3ScInst*)calloc(ni, sizeof(B3ScInst)) : NULL;
    g_vtx   = nv ? (float*)malloc((size_t)nv * 8 * sizeof(float)) : NULL;
    g_idx   = nx ? (unsigned short*)malloc((size_t)nx * 2) : NULL;
    if (!g_model || (ni && !g_inst) || (nv && !g_vtx) || (nx && !g_idx)) {
        free(d); b3_scenery_shutdown(); return 0;
    }
    g_nmodel = (int)nm;  g_ninst = (int)ni;
    g_nvtx = nv;         g_nidx = nx;

    for (unsigned i = 0; i < nm; i++) {
        const unsigned char* r = d + om + (size_t)i * B3S_MODEL;
        B3ScModel* m = &g_model[i];
        float ex = 0.0f;

        for (int k = 0; k < 3; k++) {
            m->bb_min[k] = rd_f32(r + k * 4);
            m->bb_max[k] = rd_f32(r + 12 + k * 4);
        }
        m->first_vertex = rd_u32(r + 0x18);
        m->n_vertex     = rd_u32(r + 0x1C);
        m->first_index  = rd_u32(r + 0x20);
        m->n_index      = rd_u32(r + 0x24);
        m->record       = rd_u32(r + 0x28);
        m->lod_near     = rd_f32(r + 0x2C);
        m->lod_far      = rd_f32(r + 0x30);
        m->mat_flags    = rd_u32(r + 0x34);
        m->cls          = rd_u32(r + 0x38);
        m->lod_flags    = rd_u32(r + 0x3C);
        memcpy(m->texture, r + 0x40, 31);
        m->texture[31] = 0;
        for (int k = 0; k < 3; k++) {
            float a = fabsf(m->bb_min[k]), b = fabsf(m->bb_max[k]);
            float v = a > b ? a : b;
            ex += v * v;
        }
        m->radius = sqrtf(ex);
    }

    for (unsigned i = 0; i < nv; i++) {
        const unsigned char* v = d + ov + (size_t)i * B3S_VTX;
        float* o = g_vtx + (size_t)i * 8;
        o[0] =  rd_f32(v + 0);
        o[1] =  rd_f32(v + 4);
        o[2] = -rd_f32(v + 8);          /* game -> harness Z reflection */
        o[3] =  rd_f32(v + 12);
        o[4] =  rd_f32(v + 16);
        o[5] = -rd_f32(v + 20);
        o[6] =  rd_f32(v + 24);
        o[7] =  rd_f32(v + 28);
    }
    for (unsigned i = 0; i < nx; i++)
        g_idx[i] = (unsigned short)(d[ox + i * 2] | (d[ox + i * 2 + 1] << 8));

    for (int i = 0; i < g_ninst; i++) {
        const unsigned char* r = d + oi + (size_t)i * B3S_INST;
        B3ScInst* p = &g_inst[i];
        float m[16];

        for (int k = 0; k < 16; k++) m[k] = rd_f32(r + k * 4);
        p->tint[0] = m[3]  * 2.0f;
        p->tint[1] = m[7]  * 2.0f;
        p->tint[2] = m[11] * 2.0f;
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;
        m[2] = -m[2]; m[6] = -m[6]; m[8] = -m[8]; m[9] = -m[9]; m[14] = -m[14];
        memcpy(p->m, m, sizeof m);
        p->pos[0] = m[12]; p->pos[1] = m[13]; p->pos[2] = m[14];
        p->model  = rd_u32(r + 0x40);
        p->unit   = rd_u32(r + 0x44);
        p->record = rd_u32(r + 0x48);
        if ((int)p->model >= g_nmodel) p->model = 0;
        /* A zero tint would black the instance out; the shipped files all
         * carry one, but a future file need not. */
        if (p->tint[0] + p->tint[1] + p->tint[2] <= 0.0f)
            p->tint[0] = p->tint[1] = p->tint[2] = 1.0f;
    }
    free(d);

    {   /* the texture upload and, in the same pass, the emissive scan that
         * tier 7's light field is derived from -- see scenery_lights_build.
         *
         * THE SCAN IS SKIPPED ENTIRELY WHEN THE TIER IS OFF, and that is not
         * only about the work.  Under B3_PHOTO=0 this module must load in the
         * same number of frames it loaded in before the tier existed, or the
         * pinned-frame suites that gate the whole port on B3_PHOTO=0 would be
         * photographing a different moment. */
        int want = b3_photo_fx(B3_PHOTO_FX_LIGHTS);
        B3ScEmis* em = want
            ? (B3ScEmis*)calloc((size_t)(g_nmodel ? g_nmodel : 1),
                                sizeof(B3ScEmis))
            : NULL;
        for (int i = 0; i < g_nmodel; i++)
            g_model[i].tex = load_tex(g_dir, g_model[i].texture,
                                      em ? &em[i] : NULL);
        if (em) { scenery_lights_build(em); free(em); }
    }

    /* RETAINED: one baked world-space buffer, model-major and Z-order sorted
     * inside each model, so the per-instance LOD cull below turns into a few
     * contiguous glDrawArrays runs instead of one draw per instance. */
    b3r_init();
    if (b3r_active()) {
        B3RMeshSrc mesh;
        mesh.vtx = g_vtx; mesh.stride = 8; mesh.uv_off = 6;
        mesh.nvtx = g_nvtx; mesh.idx = g_idx; mesh.nidx = g_nidx;
        B3RModelSrc* ms = (B3RModelSrc*)calloc((size_t)g_nmodel,
                                               sizeof(B3RModelSrc));
        B3RInstSrc*  is = g_ninst ? (B3RInstSrc*)calloc((size_t)g_ninst,
                                                        sizeof(B3RInstSrc))
                                  : NULL;
        if (ms && (is || !g_ninst)) {
            for (int i = 0; i < g_nmodel; i++) {
                ms[i].first_vertex = g_model[i].first_vertex;
                ms[i].n_vertex     = g_model[i].n_vertex;
                ms[i].first_index  = g_model[i].first_index;
                ms[i].n_index      = g_model[i].n_index;
                ms[i].tex          = g_model[i].tex;
                ms[i].mat_flags    = g_model[i].mat_flags;
            }
            for (int i = 0; i < g_ninst; i++) {
                const B3ScModel* mm = &g_model[g_inst[i].model];
                float far_ = g_far_override > 0.0f ? g_far_override
                                                   : mm->lod_far;
                is[i].m        = g_inst[i].m;
                is[i].tint     = g_inst[i].tint;
                is[i].model    = g_inst[i].model;
                is[i].cull_far = far_ > 0.0f ? far_ + mm->radius : 0.0f;
            }
            g_retained = b3r_inst_build(&mesh, ms, g_nmodel, is, g_ninst);
        }
        free(ms);
        free(is);
    }
    g_ready = 1;
    {   int notex = 0;
        for (int i = 0; i < g_nmodel; i++) if (!g_model[i].tex) notex++;
        printf("[scenery] %s: %d models, %d instances, %u verts, %u tris"
               "%s\n", path, g_nmodel, g_ninst, g_nvtx, g_nidx / 3,
               notex ? " (some textures missing)" : "");
        if (notex)
            for (int i = 0; i < g_nmodel; i++)
                if (!g_model[i].tex)
                    printf("[scenery]   no texture for model %d '%s'\n",
                           i, g_model[i].texture);
    }
    return g_ninst;
}

void b3_scenery_draw(const float eye[3])
{
    if (!g_ready || !g_retained) return;
    /* One glDrawArrays per contiguous run of visible instances of one model.
     * What this replaces was 523 draws a frame -- one glPushMatrix /
     * glMultMatrixf / glCallList / glPopMatrix per instance, plus FOUR
     * SYNCHRONOUS glIsEnabled readbacks to save and restore state the caller
     * already owns.  22% of the web port's entire WebGL traffic went here.
     *
     * The instance transforms are baked into the vertices (b3r_inst_build),
     * so the LOD cull is the only per-instance work left, and the Z-order
     * layout inside each model keeps its survivors contiguous. */
    b3r_stat_set(B3R_STAT_SCENERY, b3r_inst_draw(g_retained, eye, NULL));
}
