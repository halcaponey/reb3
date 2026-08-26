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

/* Same two-place texture search src/burnout3_props.c uses. */
static unsigned load_tex(const char* dir, const char* name)
{
    char path[768];
    SDL_Surface* s;
    SDL_Surface* c;
    unsigned id = 0;

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

void b3_scenery_shutdown(void)
{
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

    for (int i = 0; i < g_nmodel; i++)
        g_model[i].tex = load_tex(g_dir, g_model[i].texture);

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
