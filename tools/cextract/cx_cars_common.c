/* cx_cars_common.c -- the shared half of the CAR/VEHICLE extractor family.
 *
 * Everything the .bgv/.btv readers have in common, ported from
 * tools/extract_bgv.py (parse_part / parse_section / read_verts /
 * unpack_normal / write_obj / read_wheels) and
 * tools/extract_bgv_textures.py (extract_textures / unswizzle8).
 *
 * FORMAT PROVENANCE, condensed from tools/extract_bgv.py's docstring -- the
 * layout is the game's own pointer-relocation pass FUN_000310F0 + per-LOD
 * FUN_00031010, reached from the .bgv load completion FUN_0018D0E0
 * (docs/BGV_EXTRACTION.md):
 *
 *   header  +0x00 u32 version 0x17 .. 0x25 accepted
 *           +0x0C u8 numBodyParts, +0x0D u8 numWheels  [C: FUN_00023DE0 /
 *             FUN_000303D0]
 *           +0x18 f32 wheel radius
 *           +0x4C u32[5] LOD section offsets, +0x60 u32 material dir offset
 *           +0xAC4 i32[numBodyParts] panel kind ids  [C: FUN_00023DE0]
 *           +0xADC u8[numBodyParts] hinge axis       [C: FUN_001069C0]
 *           +0xB80 4x4 f32 x6 WHEEL matrices, stride 0x40, rows
 *             Right/Up/At/Pos                        [C: FUN_0012FEE0]
 *           +0xD00 4x4 f32 x numBodyParts PANEL PLACEMENT matrices, stride
 *             0x40                                   [C: FUN_0012FEE0 ->
 *             ctx+0x180, composed by FUN_000303D0's panel loop]
 *           +0xE80 f32[4] half extents, +0xE90 f32[4] centre  [C: FUN_00122830]
 *           +0xEA0 f32[8] x numBodyParts pivot-local AABB {max,min}
 *                                                    [C: FUN_001069C0]
 *   section +0x00 u32[18] part-object offsets rel section (0 aperture body,
 *             1..6 damage panels, 7/8/9 wheel slow/blur/blur, 10..17 debris)
 *           +0x48 D3D vertex-buffer resource header; Data at +0x4C is the
 *             vertex-pool offset rel section, bound as STREAM 0 stride 0x18
 *                                                    [C: FUN_000315C0 ->
 *             FUN_0034EDB0(0, S+0x48, 0x18)]
 *           +0x60 the embedded ONE-PIECE INTACT car part object
 *   part    s8 record count at +0, u32 record-array offset rel part at +4
 *   record  0x1C bytes: +0x0C u32 index offset rel record, +0x10 u16 INDEX
 *             COUNT (NOT a byte size -- MOVZX word ptr [rec+0x10] at
 *             0x00031DF3 feeds the indexed draw), +0x18 u16 mask,
 *             +0x1A u8 texture slot (an index into the DRAW CONTEXT's
 *             five-entry table at ctx+0x334, not into the file: 0 = the
 *             car's own paint page, 1 = the shared "VehicleUnderside" page,
 *             2/3/4 = intact/cracked/shattered glass)  [C: FUN_00031AB0
 *             @0x00031AC5, FUN_000303D0 @0x00030546, FUN_000315C0
 *             @0x000317C0..E3]
 *   vertex  0x18 bytes: f32[3] pos, D3DVSDT_NORMPACKED3 normal at +0x0C
 *             (x = bits 0..10 signed / 1023, y = bits 11..21 signed / 1023,
 *             z = bits 22..31 signed / 511), f32[2] uv at +0x10  [C: the car
 *             vertex declaration at 0x00387558, created by the car shader
 *             factory FUN_0003C8A0 @0x0003CB78]
 *   indices u16 triangle strip, degenerate restarts, alternate winding
 *
 * V-ORIGIN: `vt` is written straight through, no 1-v flip -- the repo-wide
 * rule (tools/extract_textures.py).
 */
#include "cx_cars.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ====================================================================== env */
const char *cxd_game_dir(void)
{
    /* $B3_ISO names the Xbox image; cx_src/cx_vfs serve it behind the same
     * "%s/pveh/..." joins an expanded dump takes. */
    const char *e = getenv(CX_SRC_ENV_ISO);
    if (!e || !*e)
        e = getenv("B3_GAME_DIR");
    if (!e || !*e)
        e = getenv("B3_GAME_ROOT");
    return (e && *e) ? e : CXD_GAME_DIR_DEFAULT;
}

const char *cxd_repo_root(void)
{
    /* $B3_REPO_DIR is what cx_main.c's --repo sets for the whole pipeline;
     * $B3_REPO_ROOT is accepted as well so this family can be driven stand-
     * alone without the shared driver. */
    const char *e = getenv("B3_REPO_DIR");
    if (!e || !*e)
        e = getenv("B3_REPO_ROOT");
    /* Last resort is the working directory: every driver here is meant to be
     * run from the checkout root.  No absolute path is compiled in. */
    return (e && *e) ? e : ".";
}

/* ==================================================================== blobs */
int cxd_read_file(const char *path, cxd_blob *out)
{
    FILE *f;
    long sz;

    out->d = NULL;
    out->n = 0;
    f = cx_vfs_fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    out->d = (unsigned char *)malloc((size_t)sz + 1u);
    if (!out->d) { fclose(f); return -1; }
    if (sz > 0 && fread(out->d, 1, (size_t)sz, f) != (size_t)sz) {
        free(out->d); out->d = NULL; fclose(f); return -1;
    }
    out->n = (size_t)sz;
    fclose(f);
    return 0;
}

void cxd_blob_free(cxd_blob *b)
{
    free(b->d);
    b->d = NULL;
    b->n = 0;
}

int cxd_mkdir_p(const char *path)
{
    char tmp[4096];
    size_t i, n;
    struct stat st;

    n = strlen(path);
    if (n == 0 || n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = 0;
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0777) != 0 && stat(tmp, &st) != 0)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && stat(tmp, &st) != 0)
        return -1;
    return 0;
}

void cxd_path(char *dst, size_t cap, const char *first, ...)
{
    va_list ap;
    const char *s = first;
    size_t n = 0;

    if (cap == 0)
        return;
    va_start(ap, first);
    while (s) {
        size_t l = strlen(s);
        if (n + l >= cap)
            l = cap - n - 1;
        memcpy(dst + n, s, l);
        n += l;
        if (n + 1 >= cap)
            break;
        s = va_arg(ap, const char *);
    }
    va_end(ap);
    dst[n] = 0;
}

uint8_t cxd_u8(const cxd_blob *b, size_t o) { return b->d[o]; }
int8_t  cxd_i8(const cxd_blob *b, size_t o) { return (int8_t)b->d[o]; }

uint16_t cxd_u16(const cxd_blob *b, size_t o)
{
    return (uint16_t)(b->d[o] | ((uint16_t)b->d[o + 1] << 8));
}

uint32_t cxd_u32(const cxd_blob *b, size_t o)
{
    return (uint32_t)b->d[o] | ((uint32_t)b->d[o + 1] << 8)
         | ((uint32_t)b->d[o + 2] << 16) | ((uint32_t)b->d[o + 3] << 24);
}

uint64_t cxd_u64(const cxd_blob *b, size_t o)
{
    return (uint64_t)cxd_u32(b, o) | ((uint64_t)cxd_u32(b, o + 4) << 32);
}

float cxd_f32(const cxd_blob *b, size_t o)
{
    uint32_t v = cxd_u32(b, o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* ======================================================= directory listing */
void cxd_names_free(cxd_names *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static void names_push(cxd_names *l, const char *s)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **v = (char **)realloc(l->v, (size_t)cap * sizeof *v);
        if (!v)
            return;
        l->v = v;
        l->cap = cap;
    }
    /* not strdup(): -std=c11 hides the POSIX declaration */
    {
        size_t sz = strlen(s) + 1u;
        char *c = (char *)malloc(sz);
        if (!c)
            return;
        memcpy(c, s, sz);
        l->v[l->n++] = c;
    }
}

static int cmp_bytes(const void *a, const void *b)
{
    /* python sorts str by code point; these names are ASCII, so byte order.
     * strcmp's sign is implementation-defined in magnitude only. */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    size_t i;
    if (lf > ls)
        return 0;
    for (i = 0; i < lf; i++) {
        int a = tolower((unsigned char)s[ls - lf + i]);
        int b = tolower((unsigned char)suf[i]);
        if (a != b)
            return 0;
    }
    return 1;
}

int cxd_list_dir(const char *dir, const char *suffix, int dirs_only,
                 cxd_names *out)
{
    CxSrcList l;
    int i;

    out->v = NULL;
    out->n = out->cap = 0;
    /* cx_vfs_listdir() drops "." and ".." itself and resolves is_dir the
     * same way the stat() here used to (following symlinks). */
    if (cx_vfs_listdir(dir, &l) != 0)
        return -1;
    for (i = 0; i < l.n; i++) {
        if (suffix && !ends_with_ci(l.name[i], suffix))
            continue;
        if (dirs_only && !l.is_dir[i])
            continue;
        names_push(out, l.name[i]);
    }
    cx_src_list_free(&l);
    if (out->n > 1)
        qsort(out->v, (size_t)out->n, sizeof *out->v, cmp_bytes);
    return 0;
}

/* tools/extract_vehicles.py car_sort_key(): every digit of the stem
 * concatenated and read as one integer, then the stem as tie-break. */
static long car_key_num(const char *fn)
{
    const char *dot = strrchr(fn, '.');
    size_t stop = dot ? (size_t)(dot - fn) : strlen(fn);
    long v = 0;
    int any = 0;
    size_t i;
    for (i = 0; i < stop; i++) {
        if (fn[i] >= '0' && fn[i] <= '9') {
            v = v * 10 + (fn[i] - '0');
            any = 1;
        }
    }
    return any ? v : 0;
}

static int cmp_car_key(const void *pa, const void *pb)
{
    const char *a = *(const char *const *)pa;
    const char *b = *(const char *const *)pb;
    long ka = car_key_num(a), kb = car_key_num(b);
    char sa[512], sb[512];
    const char *da, *db;

    if (ka != kb)
        return ka < kb ? -1 : 1;
    /* tie-break on the stem, exactly as the python tuple does */
    da = strrchr(a, '.');
    db = strrchr(b, '.');
    snprintf(sa, sizeof sa, "%.*s", da ? (int)(da - a) : (int)strlen(a), a);
    snprintf(sb, sizeof sb, "%.*s", db ? (int)(db - b) : (int)strlen(b), b);
    return strcmp(sa, sb);
}

void cxd_sort_car_key(cxd_names *l)
{
    if (l->n > 1)
        qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_car_key);
}

/* =================================================== .bgv part / section */
void cxd_part_free(cxd_part *p)
{
    int i;
    for (i = 0; i < p->n; i++)
        free(p->rec[i].tris);
    free(p->rec);
    p->rec = NULL;
    p->n = 0;
    p->valid = 0;
}

void cxd_section_free(cxd_section *s)
{
    int i;
    cxd_part_free(&s->body);
    for (i = 0; i < 10; i++)
        cxd_part_free(&s->slots[i]);
    s->valid = 0;
}

/* strip_to_triangles(): degenerate restarts dropped, winding alternates. */
static uint16_t *strip_to_triangles(const uint16_t *idx, int n, int *ntris)
{
    uint16_t *out;
    int i, k = 0;

    *ntris = 0;
    if (n < 3)
        return NULL;
    out = (uint16_t *)malloc((size_t)(n - 2) * 3u * sizeof *out);
    if (!out)
        return NULL;
    for (i = 0; i < n - 2; i++) {
        uint16_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == b || b == c || a == c)
            continue;
        out[k * 3 + 0] = a;
        if ((i & 1) == 0) {
            out[k * 3 + 1] = b;
            out[k * 3 + 2] = c;
        } else {
            out[k * 3 + 1] = c;
            out[k * 3 + 2] = b;
        }
        k++;
    }
    *ntris = k;
    return out;
}

static void part_push(cxd_part *p, uint16_t mask, uint8_t tex,
                      uint16_t *tris, int ntris)
{
    cxd_rec *r = (cxd_rec *)realloc(p->rec, (size_t)(p->n + 1) * sizeof *r);
    if (!r) {
        free(tris);
        return;
    }
    p->rec = r;
    p->rec[p->n].mask = mask;
    p->rec[p->n].tex = tex;
    p->rec[p->n].tris = tris;
    p->rec[p->n].ntris = ntris;
    p->n++;
}

void cxd_parse_part(const cxd_blob *d, int64_t P, cxd_part *out)
{
    int64_t recs;
    int cnt, i;
    int64_t len = (int64_t)d->n;

    memset(out, 0, sizeof *out);
    if (!(P > 0 && P < len - 8))
        return;                                  /* python: None */
    cnt = cxd_i8(d, (size_t)P);
    if (!(cnt > 0 && cnt < 64))
        return;                                  /* python: None */
    recs = P + (int64_t)cxd_u32(d, (size_t)P + 4);
    out->valid = 1;
    for (i = 0; i < cnt; i++) {
        int64_t r = recs + (int64_t)i * 0x1C;
        int64_t ioff, payload;
        uint32_t isize;
        uint16_t mask;
        uint8_t tex;
        int n, j, ntris, over = 0;
        uint16_t *idx, *tris;

        if (r < 0 || r + 0x1C > len) {
            cxd_part_free(out);                  /* python: None */
            return;
        }
        ioff = (int64_t)cxd_u32(d, (size_t)r + 0x0C);
        isize = cxd_u32(d, (size_t)r + 0x10);
        mask = cxd_u16(d, (size_t)r + 0x18);
        tex = cxd_u8(d, (size_t)r + 0x1A);
        payload = r + ioff;
        /* [C deep-traced]: +0x10 is the index COUNT (low u16), not a size. */
        n = (int)(isize & 0xFFFFu);
        if (n < 3 || payload + 2 * (int64_t)n > len)
            continue;
        idx = (uint16_t *)malloc((size_t)n * sizeof *idx);
        if (!idx) {
            cxd_part_free(out);
            return;
        }
        for (j = 0; j < n; j++) {
            idx[j] = cxd_u16(d, (size_t)payload + (size_t)j * 2u);
            if (idx[j] > 0x8000)
                over = 1;
        }
        if (over) {
            free(idx);
            cxd_part_free(out);                  /* python: None */
            return;
        }
        tris = strip_to_triangles(idx, n, &ntris);
        free(idx);
        part_push(out, mask, tex, tris, ntris);
    }
}

void cxd_parse_section(const cxd_blob *d, int64_t S, cxd_section *out)
{
    int64_t len = (int64_t)d->n;
    int64_t pool;
    int slot, i, r;
    int maxidx = -1;

    memset(out, 0, sizeof *out);
    if (!(S > 0 && S < len - 0x70))
        return;
    /* relinker's pool rule: offset += S, masked unless (flags&0x70000)==0x20000 */
    pool = (int64_t)cxd_u32(d, (size_t)S + 0x4C);
    if ((cxd_u32(d, (size_t)S + 0x48) & 0x70000u) != 0x20000u)
        pool &= 0xFFFFFFF;
    pool += S;
    cxd_parse_part(d, S + 0x60, &out->body);
    if (!out->body.valid || !(pool > 0 && pool < len)) {
        cxd_part_free(&out->body);
        return;
    }
    for (slot = 0; slot < 10; slot++) {
        uint32_t off = cxd_u32(d, (size_t)S + (size_t)slot * 4u);
        if (off) {
            out->slot_present[slot] = 1;
            cxd_parse_part(d, S + (int64_t)off, &out->slots[slot]);
        }
    }
    for (r = -1; r < 10; r++) {
        const cxd_part *p;
        if (r < 0) {
            p = &out->body;
        } else {
            /* python iterates slots.values() filtered by truthiness */
            if (!out->slot_present[r] || !out->slots[r].valid
                || out->slots[r].n == 0)
                continue;
            p = &out->slots[r];
        }
        for (i = 0; i < p->n; i++) {
            int t;
            for (t = 0; t < p->rec[i].ntris * 3; t++)
                if ((int)p->rec[i].tris[t] > maxidx)
                    maxidx = (int)p->rec[i].tris[t];
        }
    }
    if (maxidx < 2 || pool + (int64_t)(maxidx + 1) * 0x18 > len) {
        cxd_section_free(out);
        return;
    }
    out->pool = pool;
    out->maxidx = maxidx;
    out->valid = 1;
}

const cxd_part *cxd_slot(const cxd_section *s, int k)
{
    if (k < 0 || k > 9 || !s->slot_present[k])
        return NULL;
    if (!s->slots[k].valid || s->slots[k].n == 0)
        return NULL;                             /* None, or a falsy [] */
    return &s->slots[k];
}

/* D3DVSDT_NORMPACKED3 [C: declaration 0x00387558]. */
static void unpack_normal(uint32_t w, double *nx, double *ny, double *nz)
{
    int x = (int)(w & 0x7FFu);
    int y = (int)((w >> 11) & 0x7FFu);
    int z = (int)((w >> 22) & 0x3FFu);
    if (x & 0x400) x -= 0x800;
    if (y & 0x400) y -= 0x800;
    if (z & 0x200) z -= 0x400;
    *nx = x / 1023.0;
    *ny = y / 1023.0;
    *nz = z / 511.0;
}

cxd_vert *cxd_read_verts(const cxd_blob *d, int64_t pool, int count)
{
    cxd_vert *v = (cxd_vert *)malloc((size_t)count * sizeof *v);
    int i;

    if (!v)
        return NULL;
    for (i = 0; i < count; i++) {
        size_t o = (size_t)(pool + (int64_t)i * 0x18);
        double nx, ny, nz, l;
        v[i].x = cxd_f32(d, o);
        v[i].y = cxd_f32(d, o + 4);
        v[i].z = cxd_f32(d, o + 8);
        unpack_normal(cxd_u32(d, o + 0x0C), &nx, &ny, &nz);
        v[i].u = cxd_f32(d, o + 0x10);
        v[i].v = cxd_f32(d, o + 0x14);
        l = sqrt(nx * nx + ny * ny + nz * nz);
        /* renormalise so a degenerate word can never emit a zero-length vn */
        if (l < 1e-6) { nx = 0.0; ny = 1.0; nz = 0.0; l = 1.0; }
        v[i].nx = nx / l;
        v[i].ny = ny / l;
        v[i].nz = nz / l;
    }
    return v;
}

int cxd_best_section(const cxd_blob *d, cxd_section *out)
{
    int li, best = -1;
    cxd_section cur;

    memset(out, 0, sizeof *out);
    for (li = 0; li < 5; li++) {
        int ntris = 0, i;
        cxd_parse_section(d, (int64_t)cxd_u32(d, 0x4C + (size_t)li * 4u), &cur);
        if (!cur.valid)
            continue;
        for (i = 0; i < cur.body.n; i++)
            ntris += cur.body.rec[i].ntris;
        if (best < 0 || ntris > best) {          /* strictly greater: first wins */
            best = ntris;
            cxd_section_free(out);
            *out = cur;
        } else {
            cxd_section_free(&cur);
        }
    }
    return out->valid ? 0 : -1;
}

/* ======================================================== the OBJ writer */
/* python's sorted({...}) over the used vertex indices. */
static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int cxd_write_obj(const char *path, const cxd_vert *verts, int nverts,
                  const cxd_group *groups, int ngroups,
                  const char *header_note)
{
    unsigned char *seen;
    int *used, nused = 0;
    int *remap;
    int g, i;
    FILE *f;
    char *iobuf = NULL;

    seen = (unsigned char *)calloc((size_t)nverts, 1);
    if (!seen)
        return 0;
    for (g = 0; g < ngroups; g++) {
        const cxd_rec *r = groups[g].rec;
        int t;
        for (t = 0; t < r->ntris * 3; t++)
            if (r->tris[t] < nverts)
                seen[r->tris[t]] = 1;
    }
    used = (int *)malloc((size_t)nverts * sizeof *used);
    remap = (int *)malloc((size_t)nverts * sizeof *remap);
    if (!used || !remap) {
        free(seen); free(used); free(remap);
        return 0;
    }
    for (i = 0; i < nverts; i++)
        if (seen[i])
            used[nused++] = i;
    free(seen);
    if (nused == 0) {
        free(used); free(remap);
        return 0;                                /* python returns False */
    }
    qsort(used, (size_t)nused, sizeof *used, cmp_int);
    for (i = 0; i < nused; i++)
        remap[used[i]] = i + 1;

    f = fopen(path, "wb");
    if (!f) {
        free(used); free(remap);
        return 0;
    }
    /* ONE WRITE PER FILE, NOT ONE PER 4 KB.  This writes 909 OBJs totalling
     * 88 MB, a line at a time; on stdio's default st_blksize buffer that is
     * ~22 000 write() calls.  Natively they are cheap.  On the web they are
     * not: under -sPROXY_TO_PTHREAD every write() from the game worker is a
     * blocking round trip to the browser's main thread, which is what made
     * car_meshes the worst cold stage there (~19.8 s).  The same argument,
     * and the same remedy, as the read side in git cb7f8d2 -- and as
     * cx_track_mesh.c already applies to track.obj, which is where the
     * 1 << 22 there came from.  1 MB covers the largest car OBJ (491 KB) in
     * a single flush; a failed malloc simply leaves stdio's default. */
    iobuf = (char *)malloc(1u << 20);
    if (iobuf)
        setvbuf(f, iobuf, _IOFBF, 1u << 20);
    fputs("# Burnout 3 vehicle mesh, tools/extract_bgv.py\n", f);
    fputs("# layout from the game's relinker FUN_000310f0/FUN_00031010"
          " (BGV_EXTRACTION.md)\n", f);
    fprintf(f, "# %s\n", header_note);
    for (i = 0; i < nused; i++)
        fprintf(f, "v %.5f %.5f %.5f\n", verts[used[i]].x, verts[used[i]].y,
                verts[used[i]].z);
    for (i = 0; i < nused; i++)
        fprintf(f, "vt %.5f %.5f\n", verts[used[i]].u, verts[used[i]].v);
    for (i = 0; i < nused; i++)
        fprintf(f, "vn %.4f %.4f %.4f\n", verts[used[i]].nx,
                verts[used[i]].ny, verts[used[i]].nz);
    for (g = 0; g < ngroups; g++) {
        const cxd_rec *r = groups[g].rec;
        int t;
        fprintf(f, "o %s\n", groups[g].name);
        if (groups[g].tex >= 0)
            fprintf(f, "usemtl b3tex%d\n", groups[g].tex);
        for (t = 0; t < r->ntris; t++) {
            int a = remap[r->tris[t * 3 + 0]];
            int b = remap[r->tris[t * 3 + 1]];
            int c = remap[r->tris[t * 3 + 2]];
            fprintf(f, "f %d/%d/%d %d/%d/%d %d/%d/%d\n",
                    a, a, a, b, b, b, c, c, c);
        }
    }
    fclose(f);            /* flushes into iobuf's last block first */
    free(iobuf);
    free(used);
    free(remap);
    return 1;
}

/* =========================================================== .wheels data */
int cxd_read_wheels(const cxd_blob *d, double *radius_out,
                    cxd_wheel *out, int cap)
{
    int nw = cxd_u8(d, 0x0D);
    int n = 0, w;

    *radius_out = cxd_f32(d, 0x18);
    if (nw > 6)
        nw = 6;
    for (w = 0; w < nw && w < cap; w++) {
        size_t b = 0xB80 + (size_t)w * 0x40u;
        double right0;
        if (b + 0x40 > d->n)
            break;
        right0 = cxd_f32(d, b);
        out[n].pos[0] = cxd_f32(d, b + 0x30);
        out[n].pos[1] = cxd_f32(d, b + 0x34);
        out[n].pos[2] = cxd_f32(d, b + 0x38);
        out[n].mirror = right0 >= 0.0 ? 1 : -1;
        n++;
    }
    return n;
}

/* ======================================================== paint pages ==== */
/* tools/extract_bgv_textures.py.  Texture record at the absolute offset in
 * the .bgv header +0x60; layout (format credit EdnessP's boTexXbox, cross-
 * checked against the draw path, which picks the palette by the count byte
 * at record+0x69 and the pointer array at record+0x14):
 *   +0x04 u32 pixel-data offset rel record
 *   +0x14 u32[palCount] palette-record offsets rel record
 *   +0x34 u32 format (0xB = 8bpp paletted), +0x38 w, +0x3C h, +0x40 depth
 *   +0x48 char[] name, +0x69 u8 palette count
 * palette record: {u16 1, u16 3 or 0xC003}, u32 data offset rel TEXTURE
 * record; data = 256 x BGRA.  Pixels are Morton/Z-order swizzled. */
static void unswizzle8(const unsigned char *in, int w, int h,
                       unsigned char *out)
{
    int xbits = 0, ybits = 0, shared, b;
    long i, total = (long)w * (long)h;
    unsigned t;

    for (t = (unsigned)w; t > 1u; t >>= 1) xbits++;
    for (t = (unsigned)h; t > 1u; t >>= 1) ybits++;
    shared = xbits < ybits ? xbits : ybits;
    for (i = 0; i < total; i++) {
        unsigned x = 0, y = 0, v = (unsigned)i;
        for (b = 0; b < shared; b++) {
            x |= (v & 1u) << b; v >>= 1;
            y |= (v & 1u) << b; v >>= 1;
        }
        if (xbits > ybits)
            x |= v << shared;
        else
            y |= v << shared;
        out[(size_t)y * (size_t)w + x] = in[i];
    }
}

void cxd_free_paint(unsigned char **images, int count)
{
    int i;
    for (i = 0; i < count; i++)
        free(images[i]);
    free(images);
}

int cxd_extract_paint(const cxd_blob *d, char *name_out, size_t name_cap,
                      int *w_out, int *h_out,
                      unsigned char ***images_out, int *count_out,
                      const char **err)
{
    /* THREAD-LOCAL, because cx_cars_paint.c now calls this from a worker
     * pool: the buffer outlives the call by contract (*err points into it),
     * so one shared copy would let two failing cars overwrite each other's
     * message -- and the transcript is part of what the byte-identity gate
     * is protecting.  One 128-byte slot per worker is the whole cost. */
    static _Thread_local char msg[128];
    int64_t len = (int64_t)d->n;
    int64_t tex, bmp;
    uint32_t fmt, w, h, depth;
    int npal, k, n = 0;
    unsigned char *pixels = NULL, **imgs = NULL;
    size_t i;

    *images_out = NULL;
    *count_out = 0;
    *err = NULL;

    if (len < 4) { *err = "no texture record"; return -1; }
    tex = (int64_t)cxd_u32(d, 0x60);
    if (!(tex > 0 && tex < len - 0x70)) { *err = "no texture record"; return -1; }
    fmt   = cxd_u32(d, (size_t)tex + 0x34);
    w     = cxd_u32(d, (size_t)tex + 0x38);
    h     = cxd_u32(d, (size_t)tex + 0x3C);
    depth = cxd_u32(d, (size_t)tex + 0x40);
    if (fmt != 0xB || depth != 8) {
        snprintf(msg, sizeof msg, "unhandled fmt 0x%X depth %d",
                 (unsigned)fmt, (int)depth);
        *err = msg;
        return -1;
    }
    if (!(w > 0 && w <= 2048 && h > 0 && h <= 2048)) {
        snprintf(msg, sizeof msg, "bad dims %dx%d", (int)w, (int)h);
        *err = msg;
        return -1;
    }
    bmp = tex + (int64_t)cxd_u32(d, (size_t)tex + 0x4);
    npal = cxd_u8(d, (size_t)tex + 0x69);
    /* name: NUL-terminated at +0x48 */
    {
        size_t o = (size_t)tex + 0x48, e = o;
        while (e < d->n && d->d[e])
            e++;
        if (e - o >= name_cap)
            e = o + name_cap - 1;
        memcpy(name_out, d->d + o, e - o);
        name_out[e - o] = 0;
    }
    if (bmp < 0 || bmp + (int64_t)w * (int64_t)h > len) {
        *err = "pixels truncated";
        return -1;
    }
    pixels = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!pixels)
        return -1;
    unswizzle8(d->d + (size_t)bmp, (int)w, (int)h, pixels);

    imgs = (unsigned char **)malloc((size_t)(npal ? npal : 1) * sizeof *imgs);
    if (!imgs) { free(pixels); return -1; }
    for (k = 0; k < npal; k++) {
        int64_t prec = tex + (int64_t)cxd_u32(d, (size_t)tex + 0x14
                                              + (size_t)k * 4u);
        int64_t pdata;
        const unsigned char *pal, *m;
        unsigned char *img;

        if (prec < 0 || prec + 12 > len)
            continue;
        m = d->d + (size_t)prec;
        if (!(m[0] == 0x01 && m[1] == 0x00 && m[2] == 0x03
              && (m[3] == 0x00 || m[3] == 0xC0)))
            continue;
        pdata = tex + (int64_t)cxd_u32(d, (size_t)prec + 4);
        if (pdata < 0 || pdata + 1024 > len)
            continue;
        pal = d->d + (size_t)pdata;
        img = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
        if (!img)
            continue;
        for (i = 0; i < (size_t)w * (size_t)h; i++) {
            const unsigned char *e = pal + (size_t)pixels[i] * 4u;
            img[i * 4 + 0] = e[2];               /* B,G,R,A -> R,G,B,A */
            img[i * 4 + 1] = e[1];
            img[i * 4 + 2] = e[0];
            img[i * 4 + 3] = e[3];
        }
        imgs[n++] = img;
    }
    free(pixels);
    if (n == 0) {
        free(imgs);
        snprintf(msg, sizeof msg, "no valid palettes (%d declared)", npal);
        *err = msg;
        return -1;
    }
    *images_out = imgs;
    *count_out = n;
    *w_out = (int)w;
    *h_out = (int)h;
    return 0;
}

/* ============================================================ formatting */
void cxd_py_repr_double(double v, char *out, size_t cap)
{
    int p;
    /* python repr(): the shortest %.{p}g that round-trips ... */
    for (p = 1; p <= 17; p++) {
        double back;
        snprintf(out, cap, "%.*g", p, v);
        if (sscanf(out, "%lf", &back) == 1 && back == v)
            break;
        if (v != v)                              /* nan never round-trips == */
            break;
    }
    /* ... with a ".0" forced onto an integral value, which %g never writes. */
    if (!strpbrk(out, ".eE") && !strpbrk(out, "nia")) {
        size_t l = strlen(out);
        if (l + 3 < cap)
            memcpy(out + l, ".0", 3);
    }
}
