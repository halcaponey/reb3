/* cx_bvh.c -- the STATIC WORLD's ray-tracing acceleration structure
 * -> <out_dir>/bvh.bin ('B3BV' v1).
 *
 * ======================================================= NO PYTHON ORACLE ==
 * THIS STAGE HAS NO PYTHON ORACLE, and it has no retail counterpart either.
 * Every other member of CX_STAGE_LIST is a byte-identical port of an archived
 * tools/py_extract_archive/extract_*.py; cx_scenery.c is a NEW RECOVERY of a
 * shipped table.  This one is neither: it is a DERIVED artefact, built out of
 * artefacts three earlier stages already wrote, and it exists to serve an
 * INSPIRED renderer feature (docs/PHOTOREALISM.md tier 4r).  So
 * tools/cextract/verify_cextract.py's oracle diff cannot cover bvh.bin -- it
 * is asserted by tools/validate_bvh.py instead, which re-derives the geometry
 * from track.obj / props.bin / scenery.bin and re-traces rays against it with
 * a brute-force reference.
 *
 * *** NOTHING IN THIS FILE IS A CLAIM ABOUT BURNOUT 3. ***  The Xbox had no
 * ray tracing, no BVH and no shader budget for either; a sweep of the renderer
 * range 0x00028000..0x00045000 finds no acceleration structure of any kind.
 * This is a modern construction, marked INSPIRED, and must never be cited as
 * game behaviour.  See the EVIDENCE MARKS block in
 * src/burnout3_aftereffects.h.
 *
 * ============================================================== THE INPUTS
 * Everything here is read back out of `out_dir`, i.e. out of what the TRACK,
 * PROPS and SCENERY stages have already written.  That is the same shape
 * nav_edges has against bgd_paths, and it is why this stage sits LAST in
 * CX_STAGE_LIST:
 *
 *     track.obj + track.mtl   the render geometry and its material flags
 *     props.bin  ('B3PP')     the destructible props, models + placements
 *     scenery.bin ('B3SC')    the instanced scenery, models + placements
 *     textures/<name>.png     only for a CUT-OUT material -- see OPACITY
 *
 * Reading the artefacts rather than static.dat is deliberate: this stage must
 * see EXACTLY the triangles the renderer draws, and the renderer draws these
 * files.  A second, independent decode of static.dat would be a second chance
 * to disagree with the picture, which is the one thing a shadow may not do.
 *
 * ================================================================ THE SPACE
 * RAW GAME SPACE, exactly as track.obj, props.bin and scenery.bin carry it.
 * The Z flip into the harness' GL frame is the LOADER's job here as it is for
 * every other geometry artefact (src/burnout3_trackmesh.c:552-568,
 * src/burnout3_scenery.c:585, src/burnout3_props.c:646).  An asset that
 * carried a renderer's handedness would be an asset that had picked a
 * renderer.
 *
 * Instance placements ARE baked, because an instance transform is data and not
 * a convention: each placement's 4x4 is applied to its model's vertices with
 * the same column-major apply b3r_inst_build() bakes the draw with
 * (src/burnout3_render.c:2736-2738), before the loaders' Z conjugation --
 * i.e. in the artefacts' own space, which is what makes the result loadable
 * with one uniform reflection at the end.
 *
 * ========================================================== WHAT IS IN IT
 * The STATIC world, and only the static world:
 *
 *   IN   every opaque track submesh, every prop placement at its authored
 *        rest transform, every scenery placement.
 *   OUT  the DECAL layer (material flag 0x400 -- road markings, the painted
 *        shadow sheets): they are coplanar with the road by construction, so
 *        a ray leaving the road hits them at t ~ 0 and every road pixel
 *        shadows itself.  Retail draws them with the depth write off for the
 *        same reason.
 *   OUT  the BLENDED pass (D3DRS_ALPHABLENDENABLE, flag 0x001, and the
 *        submeshes at or above a streamed unit's alpha cut): glass, the
 *        coplanar facade detail layer, water.  A transparent surface that
 *        casts a solid shadow is worse than one that casts none.
 *   OUT  cars, traffic and knocked props -- everything DYNAMIC.  They keep
 *        retail's own recovered blob shadow (FUN_0019A7C0 / FUN_00043570,
 *        [C]), which is the same reason the depth-map cascade excludes them.
 *        This is a real limitation and it is written down in
 *        docs/PHOTOREALISM.md rather than hidden.
 *   OUT  the reverse-wound duplicate every D3DCULL_NONE submesh carries
 *        (tools/extract_track.py emits each two-sided triangle twice).  A ray
 *        test is double-sided, so the duplicate is pure cost; the FIRST
 *        occurrence of a vertex-set wins, which keeps the drop deterministic.
 *
 * ================================================================== OPACITY
 * A CUT-OUT material (D3DRS_ALPHATESTENABLE, flag 0x010) is a chain-link
 * fence or a foliage card: a rectangle whose texture is mostly holes.  A ray
 * that treats it as solid casts a solid rectangle, which is the single
 * loudest artefact this feature can produce -- the depth-map pass fights the
 * same thing with a real alpha test and could not ship without one.
 *
 * A ray cannot afford a texture fetch per hit, so the coverage is measured
 * ONCE HERE, per material, and stored per triangle: the fraction of the
 * material's texels whose alpha passes the game's OWN cut-out threshold --
 * GREATER than 64/255, which the world setup FUN_00038D10 writes as
 * D3DRS_ALPHAFUNC := D3DCMP_GREATER (@0x0003901B) and D3DRS_ALPHAREF := 0x40
 * (@0x00038FEE) and nothing in either material apply changes.  [C]
 *
 * The traversal then treats a hit as ATTENUATION rather than as a block:
 * transmittance *= (1 - opacity).  A 30%-covered fence dapples, two of them
 * overlapping reach 51%, and a solid material at 1.0 blocks outright and is
 * bit-for-bit the ordinary case.  It is an average rather than a texture
 * lookup, and it is marked INSPIRED for exactly that reason -- but it is an
 * average of the game's own art, measured with the game's own threshold,
 * rather than a number somebody picked.
 *
 * ================================================================== THE BVH
 * A binned-SAH BVH2, flattened DEPTH-FIRST with ESCAPE INDICES so the shader
 * can walk it with NO STACK.  That is not a preference: the target dialect is
 * ESSL 1.00 (src/burnout3_aftereffects.c:417-432 -- one dialect for desktop
 * GL, GLES2, WebGL 1 and WebGL 2), and ESSL 1.00 permits an array index only
 * where it is a constant-index-expression, which a traversal stack pointer
 * never is.  An escape index removes the stack entirely: a node that misses
 * jumps to its escape, a node that hits descends to index+1, and a leaf tests
 * its triangles and then jumps to its escape.
 *
 * The build is SINGLE-THREADED and every tie is broken by index, so the same
 * inputs give the same bytes -- gated by the double-run diff in
 * tools/validate_bvh.py section 1.  (It does not need the pool: the worst
 * shipped track, AS_M1_V1 at ~950k triangles, builds in about a second.)
 *
 * ================================================================ bvh.bin
 * Little-endian, RAW GAME SPACE (the Z flip to the harness' GL frame is the
 * loader's job, exactly as for props.bin and scenery.bin).
 *
 *     +0x00 'B3BV'   +0x04 u32 version=1
 *     +0x08 u32 node_count       +0x0C u32 tri_count
 *     +0x10 u32 off_nodes        +0x14 u32 off_tris
 *     +0x18 f32[3] world_min     +0x24 f32[3] world_max
 *     +0x30 u32 tris_track       +0x34 u32 tris_props
 *     +0x38 u32 tris_scenery     +0x3C u32 max_depth
 *     +0x40 u32 max_leaf         (the build's leaf-size target, <= 8)
 *     +0x44 u32 leaf_count       +0x48 u32 dropped_dup
 *     +0x4C u32 reserved = 0
 *   node 0x30, in DEPTH-FIRST order (node 0 is the root):
 *     +0x00 f32[3] bb_min        +0x0C f32[3] bb_max
 *     +0x18 u32 escape       the node to visit when this one misses; the
 *                            root's escape is node_count, i.e. "done"
 *     +0x1C u32 first_tri    leaf only, else 0
 *     +0x20 u32 tri_count    0 = INTERIOR (its left child is index + 1)
 *     +0x24 u32 depth        root 0; the validator's depth bound
 *     +0x28 u32 reserved0 = 0    +0x2C u32 reserved1 = 0
 *   triangle 0x30, leaf-contiguous:
 *     +0x00 f32[3] v0        +0x0C f32[3] v1        +0x18 f32[3] v2
 *     +0x24 f32 opacity      1.0 solid; see OPACITY above
 *     +0x28 u32 source       0 = track, 1 = prop, 2 = scenery
 *     +0x2C u32 ref          track: the OBJ group index
 *                            prop / scenery: the instance index
 *
 * THE LEAF PACKING IS A CONTRACT WITH THE SHADER.  ESSL 1.00 has no bitwise
 * operators and no floatBitsToInt, so a node's two spare float slots have to
 * carry `escape` and BOTH halves of the leaf.  The runtime encodes
 * `first_tri * 8 + (tri_count - 1)` into one float, which is exact while
 * first_tri < 2^21 -- so tri_count is capped at 8 here and the stage REFUSES a
 * track with more than 2,097,151 triangles rather than emitting a file the
 * shader would silently mis-read.  The worst shipped track is under half that.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "cx_bvh_build.h"
#include "cx_common_c.h"
#include "cx_extract.h"

/* THE SHAPE, THE BUILD AND THE PACKING all live in cx_bvh_build.c now --
 * carbvh.bin ('B3CV', the per-car model BVHs) walks the same flattened
 * tree in the same shader, and two files cannot be kept agreeing about a
 * format by review.  See cx_bvh_build.h. */

/* The game's own cut-out threshold: D3DRS_ALPHAFUNC := D3DCMP_GREATER,
 * D3DRS_ALPHAREF := 0x40, both written by FUN_00038D10 and changed by
 * nothing.  [C]  (src/burnout3_trackmesh.h's TRACKMESH_ALPHA_REF is the same
 * number expressed as a float; this stage compares raw bytes.) */
#define CXV_ALPHA_REF   64

/* bvh.bin's OWN header size.  It stays here rather than in the shared
 * builder: the node and triangle records are the shader's contract and are
 * shared, but what a file says about itself in front of them is the stage's. */
#define CXV_HDR         0x50

/* --------------------------------------------------------- the PNG reader
 *
 * Only what the TEXTURES stage writes: 8-bit, non-interlaced, colour type 6
 * (RGBA) or 2 (RGB).  Every shipped track texture is type 6 -- they are
 * prebaked DXT1/DXT5 surfaces and cx_textures.c decodes both to RGBA -- and a
 * type-2 sheet has no alpha channel at all, so its coverage is 1.0 by
 * definition and the pixels never have to be walked.  Anything else (a
 * palette, 16 bits, Adam7) returns "unknown" and the caller keeps 1.0, which
 * is the conservative answer: a solid shadow is wrong, and so is no shadow,
 * but only one of them is a REGRESSION against the depth-map pass.
 *
 * There is no PNG DECODER in this pipeline -- cx_png.c writes only -- and
 * pulling in libpng for one statistic is not worth a link dependency, so this
 * is inflate() plus the five filter types.  It reads the file the pipeline
 * itself produced, so the format space it has to cover is exactly one. */

static uint32_t cxv_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int cxv_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* Returns 0 and writes `*out` on success; non-zero when the sheet is a shape
 * this reader does not cover (and then `*out` is untouched). */
static int cxv_png_alpha_cover(const char *path, float *out)
{
    cxc_blob   b;
    uint8_t   *idat = NULL, *raw = NULL;
    size_t     nidat = 0, cidat = 0;
    uint32_t   w, h, i, y, x;
    int        bd, ct, il, bpp, stride;
    uLongf     rawlen;
    size_t     off;
    long       pass = 0, total = 0;
    int        rc = -1;

    if (cxc_blob_load(&b, path) != 0) return -1;
    if (b.n < 33 || memcmp(b.d, "\211PNG\r\n\032\n", 8) != 0) goto done;
    if (memcmp(b.d + 12, "IHDR", 4) != 0) goto done;
    w  = cxv_be32(b.d + 16);
    h  = cxv_be32(b.d + 20);
    bd = b.d[24];
    ct = b.d[25];
    il = b.d[28];
    if (bd != 8 || il != 0) goto done;
    if (ct == 2) { *out = 1.0f; rc = 0; goto done; }   /* no alpha at all */
    if (ct != 6) goto done;
    if (!w || !h || w > 8192u || h > 8192u) goto done;

    bpp    = 4;
    stride = (int)w * bpp;

    /* gather the IDAT chain */
    off = 8;
    while (off + 8 <= b.n) {
        uint32_t len = cxv_be32(b.d + off);
        const uint8_t *ty = b.d + off + 4;
        if (len > b.n || off + 12 + len > b.n) break;
        if (memcmp(ty, "IDAT", 4) == 0) {
            if (nidat + len > cidat) {
                size_t cap = cidat ? cidat * 2 : 65536;
                uint8_t *n;
                while (cap < nidat + len) cap *= 2;
                n = (uint8_t *)realloc(idat, cap);
                if (!n) goto done;
                idat = n; cidat = cap;
            }
            memcpy(idat + nidat, b.d + off + 8, len);
            nidat += len;
        } else if (memcmp(ty, "IEND", 4) == 0) {
            break;
        }
        off += 12 + len;
    }
    if (!nidat) goto done;

    rawlen = (uLongf)((size_t)h * (size_t)(stride + 1));
    raw = (uint8_t *)malloc(rawlen ? rawlen : 1);
    if (!raw) goto done;
    if (uncompress(raw, &rawlen, idat, (uLong)nidat) != Z_OK) goto done;
    if (rawlen != (uLongf)((size_t)h * (size_t)(stride + 1))) goto done;

    /* unfilter in place, row by row, then count */
    for (y = 0; y < h; y++) {
        uint8_t *row  = raw + (size_t)y * (size_t)(stride + 1);
        int      f    = row[0];
        uint8_t *cur  = row + 1;
        uint8_t *prev = y ? raw + (size_t)(y - 1) * (size_t)(stride + 1) + 1
                          : NULL;
        for (i = 0; i < (uint32_t)stride; i++) {
            int a = (i >= (uint32_t)bpp) ? cur[i - bpp] : 0;
            int p = prev ? prev[i] : 0;
            int c = (prev && i >= (uint32_t)bpp) ? prev[i - bpp] : 0;
            int v = cur[i];
            switch (f) {
            case 0: break;
            case 1: v += a;               break;
            case 2: v += p;               break;
            case 3: v += (a + p) >> 1;    break;
            case 4: v += cxv_paeth(a, p, c); break;
            default: goto done;
            }
            cur[i] = (uint8_t)v;
        }
        for (x = 0; x < w; x++) {
            total++;
            if (cur[x * 4 + 3] > CXV_ALPHA_REF) pass++;
        }
    }
    if (total <= 0) goto done;
    *out = (float)((double)pass / (double)total);
    rc = 0;

done:
    free(idat);
    free(raw);
    cxc_blob_free(&b);
    return rc;
}

/* ------------------------------------------------------------ the MTL/OBJ */

#define CXV_MAT_NAME 64

typedef struct {
    char  name[CXV_MAT_NAME];
    char  texture[128];      /* basename, no directory and no .png          */
    int   alpha_test, alpha_blend, decal, alpha_pass, two_sided;
    float opacity;
    int   opacity_done;
} CxvMat;

/* Truncating copy.  snprintf("%s") would do it, and would also earn a
 * -Wformat-truncation warning on every one of these, because the source is a
 * 1 KB line buffer and the destinations are not. */
static void cxv_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void cxv_strip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t')) s[--n] = 0;
}

/* `# key <int>` on the current material.  Returns 1 when the line matched. */
static int cxv_mtl_flag(const char *l, const char *key, int *dst)
{
    size_t k = strlen(key);
    if (strncmp(l, "# ", 2) != 0) return 0;
    if (strncmp(l + 2, key, k) != 0 || l[2 + k] != ' ') return 0;
    *dst = atoi(l + 3 + k);
    return 1;
}

static CxvMat *cxv_mtl_load(const char *out_dir, size_t *count)
{
    char     path[1024], line[1024];
    FILE    *f;
    CxvMat  *m = NULL;
    size_t   n = 0, cap = 0;

    snprintf(path, sizeof path, "%s/track.mtl", out_dir);
    f = fopen(path, "r");
    if (!f) { *count = 0; return NULL; }

    while (fgets(line, sizeof line, f)) {
        cxv_strip(line);
        if (strncmp(line, "newmtl ", 7) == 0) {
            if (n == cap) {
                size_t c = cap ? cap * 2 : 128;
                CxvMat *nm = (CxvMat *)realloc(m, c * sizeof *nm);
                if (!nm) { free(m); fclose(f); *count = 0; return NULL; }
                m = nm; cap = c;
            }
            memset(&m[n], 0, sizeof m[n]);
            cxv_copy(m[n].name, sizeof m[n].name, line + 7);
            m[n].opacity = 1.0f;
            n++;
            continue;
        }
        if (!n) continue;
        if (strncmp(line, "map_Kd ", 7) == 0) {
            const char *s = line + 7, *slash = strrchr(s, '/');
            char       *dot;
            if (slash) s = slash + 1;
            cxv_copy(m[n - 1].texture, sizeof m[n - 1].texture, s);
            dot = strrchr(m[n - 1].texture, '.');
            if (dot) *dot = 0;
            continue;
        }
        if (cxv_mtl_flag(line, "alpha_test",  &m[n - 1].alpha_test))  continue;
        if (cxv_mtl_flag(line, "alpha_blend", &m[n - 1].alpha_blend)) continue;
        if (cxv_mtl_flag(line, "decal",       &m[n - 1].decal))       continue;
        if (cxv_mtl_flag(line, "two_sided",   &m[n - 1].two_sided))   continue;
        /* "# alpha 1" -- the transparent PASS.  Tested after alpha_test and
         * alpha_blend so their longer keys win their own lines. */
        if (cxv_mtl_flag(line, "alpha",       &m[n - 1].alpha_pass))  continue;
    }
    fclose(f);
    *count = n;
    return m;
}

static CxvMat *cxv_mtl_find(CxvMat *m, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) if (strcmp(m[i].name, name) == 0) return &m[i];
    return NULL;
}

/* The coverage of a cut-out material's sheet, measured once and cached. */
static float cxv_opacity_of(const char *out_dir, CxvMat *mat)
{
    char  path[1024];
    float cover;

    if (!mat) return 1.0f;
    if (mat->opacity_done) return mat->opacity;
    mat->opacity_done = 1;
    if (!mat->alpha_test || !mat->texture[0]) return mat->opacity;
    snprintf(path, sizeof path, "%s/textures/%s.png", out_dir, mat->texture);
    if (cxv_png_alpha_cover(path, &cover) == 0) {
        /* A sheet that is 2% opaque is a wire fence, not a wall; a sheet
         * that measures 0 would make the surface invisible to every ray,
         * which is a hole in the world rather than a hole in a fence.  The
         * floor is the smallest coverage that still reads as a thing. */
        if (cover < 0.02f) cover = 0.02f;
        if (cover > 1.0f)  cover = 1.0f;
        mat->opacity = cover;
    }
    return mat->opacity;
}

/* ----------------------------------------------------------- the two-sided
 * duplicate drop.
 *
 * tools/extract_track.py emits a reverse-wound copy of every triangle in a
 * D3DCULL_NONE submesh, because a GL renderer with culling on needs both
 * windings.  A ray test does not: cxv_hit() is double-sided.  So within a
 * two_sided group the SECOND occurrence of a vertex triple is dropped.
 *
 * The key is the three OBJ vertex indices SORTED, so winding does not enter
 * it, and the table is a plain open-addressed hash over the group's own
 * triangles -- rebuilt per group, so a coincidental triple shared with
 * another group is never dropped.  First occurrence wins, which makes the
 * result a function of file order alone.
 *
 * The table GROWS rather than filling: an open-addressed probe over a full
 * table does not slow down, it hangs, and a two-sided submesh is not bounded
 * by anything this stage gets to see in advance. */
typedef struct { uint32_t k[3]; int used; } CxvDupSlot;

typedef struct {
    CxvDupSlot *tab;
    uint32_t    mask, used;
} CxvDup;

static uint32_t cxv_dup_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = 2166136261u;
    h = (h ^ a) * 16777619u;
    h = (h ^ b) * 16777619u;
    h = (h ^ c) * 16777619u;
    return h;
}

static void cxv_dup_free(CxvDup *d)
{
    free(d->tab);
    d->tab = NULL;
    d->mask = d->used = 0u;
}

/* 0 on success, -1 on allocation failure (the caller then stops deduping). */
static int cxv_dup_grow(CxvDup *d, uint32_t want)
{
    CxvDupSlot *nt;
    uint32_t    cap = 1024u, i;

    while (cap < want) cap <<= 1;
    nt = (CxvDupSlot *)calloc(cap, sizeof *nt);
    if (!nt) return -1;
    for (i = 0; d->tab && i <= d->mask; i++) {
        uint32_t h;
        if (!d->tab[i].used) continue;
        h = cxv_dup_hash(d->tab[i].k[0], d->tab[i].k[1], d->tab[i].k[2])
            & (cap - 1u);
        while (nt[h].used) h = (h + 1u) & (cap - 1u);
        nt[h] = d->tab[i];
    }
    free(d->tab);
    d->tab  = nt;
    d->mask = cap - 1u;
    return 0;
}

/* 1 when this triple has been seen in this group before; 0 when it is new
 * (and it is then recorded).  A table that cannot grow stops deduping, which
 * only ever costs memory. */
static int cxv_dup_seen(CxvDup *d, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t t, h;

    if (!d->tab || d->used * 2u >= d->mask) {
        if (cxv_dup_grow(d, (d->used + 1u) * 4u) != 0) return 0;
    }
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    h = cxv_dup_hash(a, b, c) & d->mask;
    for (;;) {
        if (!d->tab[h].used) {
            d->tab[h].used = 1;
            d->tab[h].k[0] = a; d->tab[h].k[1] = b; d->tab[h].k[2] = c;
            d->used++;
            return 0;
        }
        if (d->tab[h].k[0] == a && d->tab[h].k[1] == b &&
            d->tab[h].k[2] == c) return 1;
        h = (h + 1u) & d->mask;
    }
}

/* -------------------------------------------------------------- track.obj */

static int cxv_load_track(CxvBuild *B, const char *out_dir,
                          CxvMat *mats, size_t nmat,
                          uint32_t *out_dropped)
{
    char        path[1024], line[1024];
    FILE       *f;
    float      *pos = NULL;
    size_t      npos = 0, cappos = 0;
    CxvMat     *cur = NULL;
    int         skip = 0, dedup = 0;
    uint32_t    group = 0;
    CxvDup      dup;
    float       opacity = 1.0f;
    uint32_t    dropped = 0;
    int         rc = -1;

    memset(&dup, 0, sizeof dup);
    snprintf(path, sizeof path, "%s/track.obj", out_dir);
    f = fopen(path, "r");
    if (!f) return 0;                 /* no render mesh: not an error here  */

    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float p[3];
            if (sscanf(line + 2, "%f %f %f", &p[0], &p[1], &p[2]) != 3)
                continue;
            if (npos == cappos) {
                size_t c = cappos ? cappos * 2 : (1u << 16);
                float *n = (float *)realloc(pos, c * 3 * sizeof *n);
                if (!n) goto done;
                pos = n; cappos = c;
            }
            memcpy(pos + npos * 3, p, sizeof p);
            npos++;
            continue;
        }
        if (strncmp(line, "usemtl ", 7) == 0) {
            char name[CXV_MAT_NAME];
            cxv_strip(line);
            cxv_copy(name, sizeof name, line + 7);
            cur   = cxv_mtl_find(mats, nmat, name);
            group++;
            /* THE EXCLUSIONS, and each of them has a reason above. */
            skip = cur && (cur->decal || cur->alpha_blend || cur->alpha_pass);
            opacity = cxv_opacity_of(out_dir, cur);
            cxv_dup_free(&dup);
            dedup = cur && cur->two_sided && !skip;
            continue;
        }
        if (line[0] == 'f' && line[1] == ' ') {
            long a, b, c;
            if (skip) continue;
            if (sscanf(line + 2, "%ld/%*d/%*d %ld/%*d/%*d %ld/%*d/%*d",
                       &a, &b, &c) != 3 &&
                sscanf(line + 2, "%ld %ld %ld", &a, &b, &c) != 3) continue;
            if (a < 1 || b < 1 || c < 1) continue;
            if ((size_t)a > npos || (size_t)b > npos || (size_t)c > npos)
                continue;
            if (dedup && cxv_dup_seen(&dup, (uint32_t)a, (uint32_t)b,
                                      (uint32_t)c)) { dropped++; continue; }
            if (cxv_tri_push(B, pos + (a - 1) * 3, pos + (b - 1) * 3,
                             pos + (c - 1) * 3, opacity, 0u, group - 1u) != 0)
                goto done;
            continue;
        }
    }
    rc = 0;

done:
    cxv_dup_free(&dup);
    free(pos);
    fclose(f);
    *out_dropped += dropped;
    return rc;
}

/* --------------------------------------------------- props.bin/scenery.bin
 *
 * Both files carry the SAME shape for the four things this stage needs (the
 * header counts/offsets, a 0x60 model record whose +0x18..+0x24 window is the
 * vertex/index range, a 0x50 instance record whose first 64 bytes are the
 * 4x4 and whose +0x40 is the model, a 0x20 vertex whose first 12 bytes are
 * the position, and u16 indices), so one reader serves both.  The differences
 * -- prop class and mass against LOD distances and shader class -- are in
 * fields this stage does not read. */

static int cxv_load_inst(CxvBuild *B, const char *out_dir, const char *file,
                         const char *magic, uint32_t source,
                         const char *out_dir_tex, uint32_t *n_out)
{
    char      path[1024];
    cxc_blob  b;
    uint32_t  mc, ic, vc, idc, om, oi, ov, oidx, i, j;
    float    *mopacity = NULL;
    uint32_t  before = (uint32_t)B->ntri;
    int       rc = -1;

    snprintf(path, sizeof path, "%s/%s", out_dir, file);
    if (cxc_blob_load(&b, path) != 0) { *n_out = 0; return 0; }
    if (b.n < 0x30 || memcmp(b.d, magic, 4) != 0) goto done;
    if (cxc_u32(&b, 4) != 1u) goto done;

    mc   = cxc_u32(&b, 0x08); ic   = cxc_u32(&b, 0x0C);
    vc   = cxc_u32(&b, 0x10); idc  = cxc_u32(&b, 0x14);
    om   = cxc_u32(&b, 0x18); oi   = cxc_u32(&b, 0x1C);
    ov   = cxc_u32(&b, 0x20); oidx = cxc_u32(&b, 0x24);
    if (!mc || !ic) { rc = 0; goto done; }

    /* per-model opacity: the cut-out bit is mat_flags 0x010, the same bit
     * b3r_inst_build()'s inst_state() decodes into the alpha test. */
    mopacity = (float *)malloc(mc * sizeof *mopacity);
    if (!mopacity) goto done;
    for (i = 0; i < mc; i++) {
        int64_t  r = (int64_t)om + (int64_t)i * 0x60;
        uint32_t fl = cxc_u32(&b, r + 0x34);
        char     tex[64];
        float    cover;
        mopacity[i] = 1.0f;
        if (!(fl & 0x010u)) continue;
        memset(tex, 0, sizeof tex);
        if ((size_t)(r + 0x40 + 32) > b.n) continue;
        memcpy(tex, b.d + (size_t)r + 0x40, 32);
        tex[32] = 0;
        if (!tex[0]) continue;
        snprintf(path, sizeof path, "%s/textures/%s.png", out_dir_tex, tex);
        if (cxv_png_alpha_cover(path, &cover) == 0) {
            if (cover < 0.02f) cover = 0.02f;
            if (cover > 1.0f)  cover = 1.0f;
            mopacity[i] = cover;
        }
    }

    for (i = 0; i < ic; i++) {
        int64_t  r = (int64_t)oi + (int64_t)i * 0x50;
        float    m[16];
        uint32_t mi, fv, nv, fi, ni;
        int64_t  mr;

        for (j = 0; j < 16; j++) m[j] = cxc_f32(&b, r + (int64_t)j * 4);
        mi = cxc_u32(&b, r + 0x40);
        if (mi >= mc) continue;
        /* The loaders lift the tint out of the w column and clear it before
         * using the matrix as a transform (src/burnout3_scenery.c:580-584,
         * src/burnout3_props.c:640-645); the same three slots must not be
         * read as projection here. */
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;

        mr = (int64_t)om + (int64_t)mi * 0x60;
        fv = cxc_u32(&b, mr + 0x18); nv = cxc_u32(&b, mr + 0x1C);
        fi = cxc_u32(&b, mr + 0x20); ni = cxc_u32(&b, mr + 0x24);
        if (nv == 0 || ni < 3) continue;
        if (fv > vc || nv > vc - fv) continue;
        if (fi > idc || ni > idc - fi) continue;

        for (j = 0; j + 2 < ni; j += 3) {
            float w[3][3];
            int   k, e, bad = 0;
            for (k = 0; k < 3; k++) {
                uint32_t vi = cxc_u16(&b, (int64_t)oidx +
                                          ((int64_t)fi + j + k) * 2);
                int64_t  vr;
                float    v[3];
                if (vi >= nv) { bad = 1; break; }
                vr = (int64_t)ov + ((int64_t)fv + vi) * 0x20;
                for (e = 0; e < 3; e++) v[e] = cxc_f32(&b, vr + e * 4);
                /* The SAME column-major apply b3r_inst_build() bakes the
                 * draw with (src/burnout3_render.c:2736-2738). */
                w[k][0] = m[0]*v[0] + m[4]*v[1] + m[8] *v[2] + m[12];
                w[k][1] = m[1]*v[0] + m[5]*v[1] + m[9] *v[2] + m[13];
                w[k][2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14];
            }
            if (bad) continue;
            if (cxv_tri_push(B, w[0], w[1], w[2], mopacity[mi], source, i) != 0)
                goto done;
        }
    }
    rc = 0;

done:
    free(mopacity);
    *n_out = (uint32_t)B->ntri - before;
    if (cxc_oob(&b)) rc = -1;
    cxc_blob_free(&b);
    return rc;
}

/* ------------------------------------------------------------------ write */

int cx_extract_bvh(const char *game_dir, const char *track_dir,
                   const char *track_id, const char *out_dir)
{
    CxvBuild  B;
    cxc_buf   out;
    CxvMat   *mats = NULL;
    size_t    nmat = 0;
    char      path[1024];
    uint32_t  n_track = 0, n_props = 0, n_scen = 0, dropped = 0;
    uint32_t  maxdepth = 0, leaves = 0;
    size_t    i;
    int       rc = -1;

    (void)game_dir; (void)track_dir;

    memset(&B, 0, sizeof B);
    memset(&out, 0, sizeof out);

    mats = cxv_mtl_load(out_dir, &nmat);

    if (cxv_load_track(&B, out_dir, mats, nmat, &dropped) != 0) goto done;
    n_track = (uint32_t)B.ntri;
    if (cxv_load_inst(&B, out_dir, "props.bin", "B3PP", 1u, out_dir,
                      &n_props) != 0) goto done;
    if (cxv_load_inst(&B, out_dir, "scenery.bin", "B3SC", 2u, out_dir,
                      &n_scen) != 0) goto done;

    if (B.ntri == 0) {
        printf("[cx bvh] %s: no static geometry -- nothing written\n",
               track_id ? track_id : "?");
        rc = 0;
        goto done;
    }
    if (B.ntri > CXV_TRI_MAX) {
        fprintf(stderr, "[cx bvh] %s: %zu triangles exceeds the leaf "
                        "packing's ceiling of %u -- refusing to write a file "
                        "the shader would mis-read\n",
                track_id ? track_id : "?", B.ntri, (unsigned)CXV_TRI_MAX);
        goto done;
    }

    B.ord = (uint32_t *)malloc(B.ntri * sizeof *B.ord);
    if (!B.ord) goto done;
    for (i = 0; i < B.ntri; i++) B.ord[i] = (uint32_t)i;

    if (cxv_build(&B, &maxdepth) != 0 || B.err) goto done;

    /* permute the triangles into leaf order, so a leaf is one contiguous run */
    {
        CxvTri *sorted = (CxvTri *)malloc(B.ntri * sizeof *sorted);
        if (!sorted) goto done;
        for (i = 0; i < B.ntri; i++) sorted[i] = B.tri[B.ord[i]];
        free(B.tri);
        B.tri = sorted;
    }
    for (i = 0; i < B.nnode; i++) if (B.node[i].count) leaves++;

    /* ------------------------------------------------------------- header */
    cxc_put_bytes(&out, "B3BV", 4);
    cxc_put_u32(&out, 1u);
    cxc_put_u32(&out, (uint32_t)B.nnode);
    cxc_put_u32(&out, (uint32_t)B.ntri);
    cxc_put_u32(&out, (uint32_t)CXV_HDR);
    cxc_put_u32(&out, (uint32_t)(CXV_HDR + B.nnode * CXV_NODE_REC));
    for (i = 0; i < 3; i++) cxc_put_f32(&out, B.node[0].lo[i]);
    for (i = 0; i < 3; i++) cxc_put_f32(&out, B.node[0].hi[i]);
    cxc_put_u32(&out, n_track);
    cxc_put_u32(&out, n_props);
    cxc_put_u32(&out, n_scen);
    cxc_put_u32(&out, maxdepth);
    cxc_put_u32(&out, (uint32_t)CXV_LEAF_MAX);
    cxc_put_u32(&out, leaves);
    cxc_put_u32(&out, dropped);
    cxc_put_u32(&out, 0u);
    cxc_pad_to(&out, CXV_HDR);

    for (i = 0; i < B.nnode; i++) cxv_put_node(&out, &B.node[i]);
    for (i = 0; i < B.ntri; i++) {
        const CxvTri *t = &B.tri[i];
        int k;
        for (k = 0; k < 9; k++) cxc_put_f32(&out, t->v[k]);
        cxc_put_f32(&out, t->opacity);
        cxc_put_u32(&out, t->source);
        cxc_put_u32(&out, t->ref);
    }
    if (out.err) goto done;

    snprintf(path, sizeof path, "%s/bvh.bin", out_dir);
    if (cxc_buf_write(&out, path) != 0) goto done;

    printf("[cx bvh] %-9s %7u tris (%u track + %u props + %u scenery, "
           "%u dup dropped)  %6u nodes  depth %2u  %5.1f MB\n",
           track_id ? track_id : "?", (unsigned)B.ntri, n_track, n_props,
           n_scen, dropped, (unsigned)B.nnode, maxdepth,
           (double)out.n / (1024.0 * 1024.0));
    rc = 0;

done:
    free(mats);
    cxv_build_free(&B);
    cxc_buf_free(&out);
    return rc;
}
