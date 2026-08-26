/* cx_common_a.c -- agent A's private helpers: see cx_common_a.h.
 *
 * A 1:1 C port of the structural reader and geometry recovery in
 * tools/extract_bgd_paths.py (the byte-level spec) plus the tlist decode in
 * tools/extract_tlist.py.  Provenance markers [C] = executed retail code,
 * [S] = structural inference, carried over from those tools.
 *
 * Numerics: every intermediate is a C double, exactly as CPython's floats
 * are, and the f32 rounding happens only in the writers.  Operation ORDER
 * is preserved (including the reference tools' habit of summing an already
 * SORTED step list), because a reordered sum is a different double.
 */
#include "cx_common_a.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char CXA_B40_CHARSET[41] = " -/0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";

/* ---------------------------------------------------------------- basics */

unsigned char *cxa_slurp(const char *path, size_t *out_len)
{
    FILE *f = cx_vfs_fopen(path, "rb");
    unsigned char *buf;
    long len;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (len && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[len] = 0;
    if (out_len)
        *out_len = (size_t)len;
    return buf;
}

int cxa_file_exists(const char *path)
{
    FILE *f = cx_vfs_fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

char *cxa_join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    if (!p)
        return NULL;
    memcpy(p, dir, a);
    if (a && dir[a - 1] != '/')
        p[a++] = '/';
    memcpy(p + a, name, b + 1);
    return p;
}

/* FUN_001AECC0: base-40 decode, LSB char first, then reversed, stripped. [C] */
void cxa_b40(uint64_t v, char out[13])
{
    char tmp[13];
    int i, lo = 0, hi;
    for (i = 0; i < 12; i++) {
        tmp[i] = CXA_B40_CHARSET[v % 40];
        v /= 40;
    }
    /* reversed() then .strip() -- the charset's only whitespace is ' ' */
    for (i = 0; i < 12; i++)
        out[i] = tmp[11 - i];
    out[12] = 0;
    hi = 12;
    while (lo < hi && out[lo] == ' ')
        lo++;
    while (hi > lo && out[hi - 1] == ' ')
        hi--;
    memmove(out, out + lo, (size_t)(hi - lo));
    out[hi - lo] = 0;
}

uint32_t cxa_u32(const unsigned char *d, size_t o)
{
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8)
         | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

uint64_t cxa_u64(const unsigned char *d, size_t o)
{
    return (uint64_t)cxa_u32(d, o) | ((uint64_t)cxa_u32(d, o + 4) << 32);
}

uint16_t cxa_u16(const unsigned char *d, size_t o)
{
    return (uint16_t)((uint16_t)d[o] | ((uint16_t)d[o + 1] << 8));
}

double cxa_f32(const unsigned char *d, size_t o)
{
    uint32_t bits = cxa_u32(d, o);
    float v;
    memcpy(&v, &bits, 4);
    return (double)v;
}

cxa_v3 cxa_f3(const unsigned char *d, size_t o)
{
    cxa_v3 v;
    v.x = cxa_f32(d, o);
    v.y = cxa_f32(d, o + 4);
    v.z = cxa_f32(d, o + 8);
    return v;
}

void cxa_w_bytes(FILE *f, const void *p, size_t n) { fwrite(p, 1, n, f); }

void cxa_w_u8(FILE *f, unsigned v)
{
    unsigned char b = (unsigned char)(v & 0xff);
    fwrite(&b, 1, 1, f);
}

void cxa_w_u16(FILE *f, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    fwrite(b, 1, 2, f);
}

void cxa_w_u32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xff);
    b[1] = (unsigned char)((v >> 8) & 0xff);
    b[2] = (unsigned char)((v >> 16) & 0xff);
    b[3] = (unsigned char)((v >> 24) & 0xff);
    fwrite(b, 1, 4, f);
}

void cxa_w_i32(FILE *f, int32_t v) { cxa_w_u32(f, (uint32_t)v); }

void cxa_w_f32(FILE *f, double v)
{
    float s = (float)v;
    uint32_t bits;
    memcpy(&bits, &s, 4);
    cxa_w_u32(f, bits);
}

/* Game space -> harness GL space: one uniform z reflection (RE_NOTES 12). */
void cxa_w_v3_gl(FILE *f, cxa_v3 p)
{
    cxa_w_f32(f, p.x);
    cxa_w_f32(f, p.y);
    cxa_w_f32(f, -p.z);
}

/* python: s.encode('ascii')[:n-1] padded with NULs to n */
void cxa_w_pad(FILE *f, const char *s, size_t n)
{
    size_t len = strlen(s), i;
    if (len > n - 1)
        len = n - 1;
    fwrite(s, 1, len, f);
    for (i = len; i < n; i++)
        cxa_w_u8(f, 0);
}

/* dist/dist2 are XZ-ONLY in the reference tools */
double cxa_dist2(cxa_v3 a, cxa_v3 b)
{
    return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
}

double cxa_dist(cxa_v3 a, cxa_v3 b) { return sqrt(cxa_dist2(a, b)); }

static int cmp_double(const void *pa, const void *pb)
{
    double a = *(const double *)pa, b = *(const double *)pb;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

/* median(v) == sorted(v)[len//2]; sorts a private copy, v is left alone */
double cxa_median(double *v, size_t n)
{
    double *copy, out;
    if (!n)
        return 0.0;
    copy = (double *)malloc(n * sizeof(double));
    if (!copy)
        return 0.0;
    memcpy(copy, v, n * sizeof(double));
    qsort(copy, n, sizeof(double), cmp_double);
    out = copy[n / 2];
    free(copy);
    return out;
}

/* ------------------------------------------------------------------- bgd */

int cxa_bgd_open(cxa_bgd *b, const char *path)
{
    uint32_t i;
    memset(b, 0, sizeof *b);
    b->d = cxa_slurp(path, &b->n);
    if (!b->d)
        return -1;
    if (b->n < 0x264 || cxa_u32(b->d, 0) != 9)     /* Gamedata.bgd version 9 */
        return -1;
    b->count = cxa_u32(b->d, 0x260);               /* [C] 0x0018B37A */
    if (!(b->count > 0 && b->count <= 64))
        return -1;
    b->events = (cxa_event *)calloc(b->count, sizeof(cxa_event));
    if (!b->events)
        return -1;
    for (i = 0; i < b->count; i++) {
        uint32_t off = cxa_u32(b->d, 0x198 + i * 4);   /* [C] 0x0018B3C5 */
        cxa_event *e;
        if (!(off > 0 && (size_t)off < b->n - 0x800))
            continue;
        e = &b->events[b->nevents++];
        e->index = (int)i;
        cxa_b40(cxa_u64(b->d, 0x08 + i * 8), e->id);   /* [C] 0x0018B398 */
        e->param        = off;
        e->laps         = cxa_u32(b->d, off + 0x3B8);
        e->spatial_size = cxa_u32(b->d, off + 0x3BC);
        e->spatial_off  = cxa_u32(b->d, off + 0x3C0);
        e->tdesc_size   = cxa_u32(b->d, off + 0x3C4);
        e->tdesc_off    = cxa_u32(b->d, off + 0x3C8);
        e->ridx_size    = cxa_u32(b->d, off + 0x3CC);
        e->ridx_off     = cxa_u32(b->d, off + 0x3D0);
        e->net_size     = cxa_u32(b->d, off + 0x3D4);
        e->net_off      = cxa_u32(b->d, off + 0x3D8);
    }
    return 0;
}

void cxa_bgd_close(cxa_bgd *b)
{
    if (!b)
        return;
    free(b->d);
    free(b->events);
    memset(b, 0, sizeof *b);
}

static int bgd_sane(const cxa_bgd *b, const cxa_event *e)
{
    return e->net_size
        && (size_t)e->net_off + e->net_size == b->n
        && (size_t)e->ridx_off + e->ridx_size == e->net_off
        && e->spatial_size == 0x800
        && e->spatial_off > 0 && (size_t)e->spatial_off < b->n - 0x800;
}

cxa_event *cxa_bgd_event(cxa_bgd *b, const char *name)
{
    int i;
    if (name) {
        for (i = 0; i < b->nevents; i++)
            if (!strcmp(b->events[i].id, name) && bgd_sane(b, &b->events[i]))
                return &b->events[i];
    }
    for (i = 0; i < b->nevents; i++)
        if (bgd_sane(b, &b->events[i]))
            return &b->events[i];
    return NULL;
}

int cxa_network(cxa_bgd *b, const cxa_event *ev, cxa_net *out)
{
    uint32_t n = ev->net_off, i;
    memset(out, 0, sizeof *out);
    out->base  = n;
    out->rows  = cxa_u32(b->d, n + 0x04);
    out->nodes = cxa_u32(b->d, n + 0x08);
    out->nsec  = cxa_u32(b->d, n + 0x10);
    out->idx   = n + cxa_u32(b->d, n + 0x14);
    /* route-section records, stride 0x10 {u32 start_node, f32 length_m, f32,
     * f32} -- the first record's dword shares N+0x20 with the relocated
     * pointer.  Lap length = sum of the lengths. */
    out->secs = (cxa_secrec *)calloc(out->nsec ? out->nsec : 1,
                                     sizeof(cxa_secrec));
    if (!out->secs)
        return -1;
    out->lap = 0.0;
    for (i = 0; i < out->nsec; i++) {
        size_t o = n + 0x20 + i * 0x10;
        out->secs[i].start_node = cxa_u32(b->d, o);
        out->secs[i].length     = cxa_f32(b->d, o + 4);
        out->secs[i].f2         = cxa_f32(b->d, o + 8);
        out->secs[i].f3         = cxa_f32(b->d, o + 12);
        out->lap += out->secs[i].length;
    }
    out->ptr20 = n + cxa_u32(b->d, n + 0x20);
    return 0;
}

void cxa_net_free(cxa_net *n)
{
    if (n) { free(n->secs); n->secs = NULL; }
}

/* The retail section/node graph used by FUN_00175B10.  The 0x10 index rows
 * are relocated ROW-RELATIVE by FUN_00158DE0:
 *   {pair_rel, edge_rel, link_rel, flags:u16|node_count:u16}          [C] */
int cxa_nav_graph(cxa_bgd *b, const cxa_event *ev, cxa_graph *out)
{
    cxa_net net;
    uint32_t s;
    if (cxa_network(b, ev, &net))
        return -1;
    memset(out, 0, sizeof *out);
    out->nrows = net.rows;
    out->rows  = (cxa_navrow *)calloc(net.rows ? net.rows : 1,
                                      sizeof(cxa_navrow));
    if (!out->rows) { cxa_net_free(&net); return -1; }
    for (s = 0; s < net.rows; s++) {
        uint32_t row = net.idx + s * 0x10;
        uint32_t pair_rel = cxa_u32(b->d, row);
        uint32_t edge_rel = cxa_u32(b->d, row + 4);
        uint32_t link_rel = cxa_u32(b->d, row + 8);
        uint32_t cf       = cxa_u32(b->d, row + 12);
        out->rows[s].section    = s;
        out->rows[s].node_count = cf & 0xffff;
        out->rows[s].flags      = cf >> 16;
        out->rows[s].pairs      = row + pair_rel;
        out->rows[s].edges      = row + edge_rel;
        out->rows[s].links      = row + link_rel;
    }
    out->points      = net.ptr20;
    out->point_count = (net.idx - net.ptr20) / 16;
    cxa_net_free(&net);
    return 0;
}

void cxa_graph_free(cxa_graph *g)
{
    if (g) { free(g->rows); g->rows = NULL; }
}

/* FUN_001772A0's 12-byte look-ahead planning records. [C] */
int cxa_nav_plans(cxa_bgd *b, const cxa_event *ev,
                  cxa_navplan **out, uint32_t *out_count)
{
    cxa_net net;
    uint32_t count, table, i;
    cxa_navplan *p;
    if (cxa_network(b, ev, &net))
        return -1;
    count = cxa_u32(b->d, net.base + 0x0c);
    table = net.base + cxa_u32(b->d, net.base + 0x1c);
    cxa_net_free(&net);
    p = (cxa_navplan *)calloc(count ? count : 1, sizeof(cxa_navplan));
    if (!p)
        return -1;
    for (i = 0; i < count; i++) {
        size_t o = table + i * 12;
        p[i].node_a  = cxa_u16(b->d, o);
        p[i].node_b  = cxa_u16(b->d, o + 2);
        p[i].node_c  = cxa_u16(b->d, o + 4);
        p[i].speed   = cxa_u16(b->d, o + 6);
        p[i].section = b->d[o + 8];
        p[i].byte9   = b->d[o + 9];
        p[i].flags   = b->d[o + 10];
        p[i].byte11  = b->d[o + 11];
    }
    *out = p;
    *out_count = count;
    return 0;
}

/* The 6 start-grid slots in RAW game space, 0x50 bytes each:
 *   +0x00 right  +0x10 up  +0x20 at (forward)  +0x30 position
 *   +0x40 u32 road-network node index                                  [C] */
int cxa_grid(cxa_bgd *b, const cxa_event *ev, cxa_gridslot out[6])
{
    uint32_t so = ev->spatial_off;
    int k;
    for (k = 0; k < 6; k++) {
        size_t o = so + (size_t)k * 0x50;
        double n2;
        out[k].right = cxa_f3(b->d, o);
        out[k].up    = cxa_f3(b->d, o + 0x10);
        out[k].at    = cxa_f3(b->d, o + 0x20);
        out[k].pos   = cxa_f3(b->d, o + 0x30);
        out[k].node  = cxa_u32(b->d, o + 0x40);
        n2 = out[k].at.x * out[k].at.x + out[k].at.y * out[k].at.y
           + out[k].at.z * out[k].at.z;
        if (!(sqrt(n2) > 0.99 && sqrt(n2) < 1.01))
            return -1;              /* grid slot forward is not a unit vector */
    }
    return 0;
}

/* ------------------------------------------------------- traffic (RIDX) [C]
 * The active event's RIDX image begins with the header relocated by
 * FUN_00158CC0: {u32 descriptor_rows, u32 point_base, u32 path_count}; each
 * 0x14-byte descriptor is {pair_rows, distances, aux, point_base, count}.
 * FUN_0019FFA0 reads the pair rows and the shared 16-byte point pool,
 * FUN_0019F1C0 the cumulative-distance rows.                              */
int cxa_traffic_paths(cxa_bgd *b, const cxa_event *ev, cxa_tpaths *out)
{
    uint32_t size = ev->ridx_size, base = ev->ridx_off;
    uint32_t rows_rel, points_rel, count, i;
    memset(out, 0, sizeof *out);
    if (!(size >= 12 && base > 0 && (size_t)base <= b->n - size))
        return -1;
    rows_rel   = cxa_u32(b->d, base);
    points_rel = cxa_u32(b->d, base + 4);
    count      = cxa_u32(b->d, base + 8);
    if (!(count > 0 && count <= 255 && rows_rel < size && points_rel < size
          && rows_rel + count * 0x14 <= size))
        return -1;
    out->base        = base;
    out->size        = size;
    out->points      = base + points_rel;
    out->point_count = (size - points_rel) / 16;
    out->npaths      = count;
    out->paths       = (cxa_tpath *)calloc(count, sizeof(cxa_tpath));
    if (!out->paths)
        return -1;
    for (i = 0; i < count; i++) {
        uint32_t row = base + rows_rel + i * 0x14;
        uint32_t pairs_rel = cxa_u32(b->d, row);
        uint32_t dist_rel  = cxa_u32(b->d, row + 4);
        uint32_t aux_rel   = cxa_u32(b->d, row + 8);
        uint32_t path_count = cxa_u32(b->d, row + 16);
        uint32_t node;
        if (!(path_count >= 2 && path_count <= out->point_count
              && pairs_rel + path_count * 4 <= size
              && dist_rel + path_count * 8 <= size
              && aux_rel < size))
            return -1;
        if ((size_t)base + aux_rel + path_count * 0x12 > b->n)
            return -1;
        for (node = 0; node < path_count; node++) {
            uint32_t pa = cxa_u16(b->d, base + pairs_rel + node * 4);
            uint32_t pb = cxa_u16(b->d, base + pairs_rel + node * 4 + 2);
            if (pa >= out->point_count || pb >= out->point_count)
                return -1;
        }
        out->paths[i].index     = i;
        out->paths[i].row_count = path_count;
        out->paths[i].pairs_off = base + pairs_rel;
        out->paths[i].dist_off  = base + dist_rel;
        out->paths[i].aux_off   = base + aux_rel;
    }
    return 0;
}

void cxa_tpaths_free(cxa_tpaths *t)
{
    if (t) { free(t->paths); t->paths = NULL; }
}

/* ----------------------------------------------------- TDESC (mode block) */

/* FUN_001A5E30's jump table @0x001A5F10 maps the runtime CLASS code onto the
 * six list offsets (index = class - 1): 1 -> +0x54, 2 -> +0x60, 3 -> +0x78,
 * 4 -> +0x84, 5 -> +0x6C, 0xB -> +0x90, so the extractor's slot order is NOT
 * the class order.                                                      [C] */
static const uint32_t CXA_TRAFFIC_LISTS[6] = { 0x54, 0x60, 0x6C, 0x78, 0x84,
                                               0x90 };
static const uint32_t CXA_LIST_CLASS[6]    = { 1, 2, 5, 3, 4, 0x0B };

static uint32_t umin3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t m = a < b ? a : b;
    return m < c ? m : c;
}

int cxa_read_tdesc(cxa_bgd *b, const cxa_event *ev, cxa_tdesc *out)
{
    const unsigned char *d = b->d;
    uint32_t size = ev->tdesc_size, base = ev->tdesc_off;
    uint32_t i, k, rel, cnt;
    uint8_t gate;

    memset(out, 0, sizeof *out);
    out->base = base;
    out->size = size;

    for (i = 0; i < 6; i++) {
        uint32_t p = CXA_TRAFFIC_LISTS[i];
        rel = cxa_u32(d, base + p);
        cnt = cxa_u32(d, base + p + 4);
        out->lists[i].off   = p;
        out->lists[i].cls   = CXA_LIST_CLASS[i];
        out->lists[i].total = cxa_u32(d, base + p + 8);
        if (cnt && rel > 0 && rel < size) {
            out->lists[i].records =
                (cxa_listrec *)calloc(cnt, sizeof(cxa_listrec));
            if (!out->lists[i].records)
                return -1;
            out->lists[i].nrecords = cnt;
            for (k = 0; k < cnt; k++) {
                size_t o = base + rel + (size_t)k * 0x18;
                cxa_listrec *r = &out->lists[i].records[k];
                cxa_b40(cxa_u64(d, o), r->id);
                /* FUN_001A5F90 reads eight per-record paint percentages at
                 * +0x08..+0x0F; FUN_001A5E30 walks the u32 spawn weight at
                 * +0x10 against the list total at listoff+0x08. [C] */
                memcpy(r->colours, d + o + 8, 8);
                r->weight = cxa_u32(d, o + 0x10);
                r->tail   = cxa_u32(d, o + 0x14);
            }
        }
    }

    /* five inline u64 special ids at TDESC+0x00.., gated by TDESC+0xB5 [C] */
    gate = d[base + 0xB5];
    for (k = 0; k < 5; k++)
        if (gate & (1u << k))
            cxa_b40(cxa_u64(d, base + k * 8), out->specials[out->nspecials++]);

    /* spawn/entry table {ptr TDESC+0xAC, count TDESC+0xB0}, stride 0x20,
     * {pos[3], w, dir[3], w} -- location [C], semantics [S] */
    rel = cxa_u32(d, base + 0xAC);
    cnt = cxa_u32(d, base + 0xB0);
    if (cnt && rel > 0 && rel < size) {
        out->spawns = (double *)calloc((size_t)cnt * 6, sizeof(double));
        if (!out->spawns)
            return -1;
        out->nspawns = cnt;
        for (k = 0; k < cnt; k++) {
            size_t o = base + rel + (size_t)k * 0x20;
            cxa_v3 p = cxa_f3(d, o), q = cxa_f3(d, o + 0x10);
            out->spawns[k * 6 + 0] = p.x;
            out->spawns[k * 6 + 1] = p.y;
            out->spawns[k * 6 + 2] = p.z;
            out->spawns[k * 6 + 3] = q.x;
            out->spawns[k * 6 + 4] = q.y;
            out->spawns[k * 6 + 5] = q.z;
        }
    }

    /* {ptr TDESC+0x4C, count +0x50} 0x58-stride traffic schedules; row 0 is
     * the pre-race state (trigger +0x50 == 0).  Stage 2 installs a float
     * cruise speed in MPH, stage 3 a 0x1C-byte per-class rate record. [C] */
    {
        uint32_t schedule_rel = cxa_u32(d, base + 0x4C);
        uint32_t schedule_count = cxa_u32(d, base + 0x50);
        if (schedule_count && schedule_rel > 0 && schedule_rel < size) {
            out->schedules =
                (cxa_schedule *)calloc(schedule_count, sizeof(cxa_schedule));
            if (!out->schedules)
                return -1;
            for (k = 0; k < schedule_count; k++) {
                size_t o = base + schedule_rel + (size_t)k * 0x58;
                cxa_schedule *s;
                uint32_t source_rel, manager_rel, field_rel, stage2_count;
                uint32_t rate_rel, rate_mgr, rate_fld, rate_count, j, n;
                if (o + 0x58 > (size_t)base + size)
                    break;
                s = &out->schedules[out->nschedules++];
                s->off = (uint32_t)o;
                source_rel   = cxa_u32(d, o + 0x04);
                manager_rel  = cxa_u32(d, o + 0x08);
                field_rel    = cxa_u32(d, o + 0x0C);
                stage2_count = cxa_u32(d, o + 0x40);
                if (stage2_count && source_rel > 0 && source_rel < size
                    && manager_rel > 0 && manager_rel < size
                    && field_rel > 0 && field_rel < size) {
                    n = umin3(stage2_count, size - manager_rel,
                              size - field_rel);
                    s->speeds = (cxa_speed *)calloc(n ? n : 1,
                                                    sizeof(cxa_speed));
                    if (!s->speeds)
                        return -1;
                    for (j = 0; j < n; j++) {
                        s->speeds[j].record = d[base + manager_rel + j];
                        s->speeds[j].slot   = d[base + field_rel + j];
                        s->speeds[j].mph    = cxa_f32(d, base + source_rel
                                                         + (size_t)j * 4);
                    }
                    s->nspeeds = n;
                }
                rate_rel   = cxa_u32(d, o + 0x10);
                rate_mgr   = cxa_u32(d, o + 0x14);
                rate_fld   = cxa_u32(d, o + 0x18);
                rate_count = cxa_u32(d, o + 0x44);
                if (rate_count && rate_rel > 0 && rate_rel < size
                    && rate_mgr > 0 && rate_mgr < size
                    && rate_fld > 0 && rate_fld < size) {
                    n = umin3(rate_count, size - rate_mgr, size - rate_fld);
                    s->rates = (cxa_rate *)calloc(n ? n : 1, sizeof(cxa_rate));
                    if (!s->rates)
                        return -1;
                    for (j = 0; j < n; j++) {
                        size_t q = base + rate_rel + (size_t)j * 0x1C;
                        int t;
                        if (q + 0x1C > (size_t)base + size)
                            break;
                        s->rates[s->nrates].record = d[base + rate_mgr + j];
                        s->rates[s->nrates].slot   = d[base + rate_fld + j];
                        for (t = 0; t < 7; t++)
                            s->rates[s->nrates].rate[t] =
                                cxa_f32(d, q + (size_t)t * 4);
                        s->nrates++;
                    }
                }
                s->trigger    = cxa_u16(d, o + 0x50);
                s->trigger_hi = d[o + 0x52];
            }
        }
    }

    /* FUN_001A28B0's TDESC pool windows {ptr +0xA4, count +0xA8}, 0x18 each */
    {
        uint32_t wrel = cxa_u32(d, base + 0xA4);
        uint32_t wcnt = cxa_u32(d, base + 0xA8);
        if (wcnt && wrel > 0 && wrel < size) {
            uint32_t n = (size - wrel) / 0x18;
            if (wcnt < n)
                n = wcnt;
            out->pool_windows = (cxa_poolwin *)calloc(n ? n : 1,
                                                      sizeof(cxa_poolwin));
            if (!out->pool_windows)
                return -1;
            out->npool_windows = n;
            for (k = 0; k < n; k++) {
                size_t o = base + wrel + (size_t)k * 0x18;
                cxa_poolwin *w = &out->pool_windows[k];
                uint32_t request_rel = cxa_u32(d, o);
                w->first_progress = cxa_u32(d, o + 4);
                w->last_progress  = cxa_u32(d, o + 8);
                w->request_count  = d[o + 0x14];
                w->refresh_count  = d[o + 0x15];
                if (w->request_count && request_rel > 0 && request_rel < size) {
                    uint32_t nr = (size - request_rel) / 6, j;
                    if (w->request_count < nr)
                        nr = w->request_count;
                    w->requests = (cxa_poolreq *)calloc(nr ? nr : 1,
                                                        sizeof(cxa_poolreq));
                    if (!w->requests)
                        return -1;
                    w->nrequests = nr;
                    for (j = 0; j < nr; j++) {
                        size_t q = base + request_rel + (size_t)j * 6;
                        w->requests[j].first_row = cxa_u16(d, q);
                        w->requests[j].last_row  = cxa_u16(d, q + 2);
                        w->requests[j].path_id   = d[q + 4];
                        w->requests[j].direction = d[q + 5];
                    }
                }
            }
        }
    }

    /* Manager-record table: FUN_001A13F0 @0x001A1BE0 reads the count from
     * TDESC+0x40 and hands each 0x10-byte row to FUN_001A5680.  Row =
     * {u32 path_ids_rel, u32 start_rows_rel, u32 slot_count, u8 flags}. [C] */
    {
        uint32_t record_rel = cxa_u32(d, base + 0x3C);
        uint32_t record_count = d[base + 0x40];
        if (record_count && record_rel > 0 && record_rel < size) {
            out->records = (cxa_mrecord *)calloc(record_count,
                                                 sizeof(cxa_mrecord));
            if (!out->records)
                return -1;
            for (k = 0; k < record_count; k++) {
                size_t o = base + record_rel + (size_t)k * 0x10;
                cxa_mrecord *r;
                uint32_t paths_rel, rows_rel, nslot, s;
                if (o + 0x10 > (size_t)base + size)
                    break;
                r = &out->records[out->nrecords++];
                paths_rel = cxa_u32(d, o);
                rows_rel  = cxa_u32(d, o + 4);
                nslot     = cxa_u32(d, o + 8);
                if (nslot > 0 && nslot <= 8 && paths_rel > 0 && paths_rel < size
                    && rows_rel > 0 && rows_rel < size) {
                    for (s = 0; s < nslot; s++) {
                        r->slots[s].path_id =
                            cxa_u32(d, base + paths_rel + (size_t)s * 4);
                        r->slots[s].start_row =
                            cxa_u32(d, base + rows_rel + (size_t)s * 4);
                    }
                    r->nslots = nslot;
                }
                r->flags = d[o + 0xC];
            }
        }
    }
    return 0;
}

void cxa_tdesc_free(cxa_tdesc *t)
{
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < 6; i++)
        free(t->lists[i].records);
    free(t->spawns);
    for (i = 0; i < t->nschedules; i++) {
        free(t->schedules[i].speeds);
        free(t->schedules[i].rates);
    }
    free(t->schedules);
    for (i = 0; i < t->npool_windows; i++)
        free(t->pool_windows[i].requests);
    free(t->pool_windows);
    free(t->records);
    memset(t, 0, sizeof *t);
}

/* ============================================================== geometry [S]
 * No code path exposes the boundary strip or the drive lines, so they are
 * recovered from the file's own geometry; none of the thresholds is a track
 * constant -- each is a multiple of a statistic measured on the track's own
 * data.  See tools/extract_bgd_paths.py's docstring.
 */

static void rev_v3(cxa_v3 *p, int n)
{
    int i;
    for (i = 0; i < n / 2; i++) {
        cxa_v3 t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

/* The strip is the one INTERLEAVED array in the network section: its records
 * alternate between the two road edges, so it ZIGZAGS across the road while
 * every other array is a smooth polyline.  The test is the turn angle
 * (scale-free): cos(angle) < -0.2 -> zigzag.  Longest such run, tolerating
 * up to 5 consecutive non-zigzag samples.                               [S] */
int cxa_corridor(cxa_bgd *b, const cxa_net *net, cxa_cor *out)
{
    const unsigned char *d = b->d;
    uint32_t lo = net->base + 0x20 + net->nsec * 0x10;
    uint32_t hi = net->idx;
    int n, i, s0, cnt, phase, pairs, b0, b1, start, bad;
    int best_s = 0, best_c = 0;
    cxa_v3 *P;
    double *cosang, *adv, *arc, ma, lo_len, hi_len, best_close = 0.0;
    int *jump;
    int have_close = 0;

    memset(out, 0, sizeof *out);
    lo = (lo + 15) & ~15u;
    if (hi <= lo)
        return -1;
    n = (int)((hi - lo) / 16);
    if (n < 3)
        return -1;
    P = (cxa_v3 *)malloc((size_t)n * sizeof(cxa_v3));
    cosang = (double *)malloc((size_t)(n - 2) * sizeof(double));
    if (!P || !cosang) { free(P); free(cosang); return -1; }
    for (i = 0; i < n; i++)
        P[i] = cxa_f3(d, lo + (size_t)i * 16);
    for (i = 0; i < n - 2; i++) {
        double ax = P[i + 1].x - P[i].x, az = P[i + 1].z - P[i].z;
        double bx = P[i + 2].x - P[i + 1].x, bz = P[i + 2].z - P[i + 1].z;
        double na = hypot(ax, az), nb = hypot(bx, bz);
        cosang[i] = (na > 1e-6 && nb > 1e-6)
                  ? (ax * bx + az * bz) / (na * nb) : 1.0;
    }
    start = 0;
    bad = 0;
    for (i = 0; i < n - 2; i++) {
        if (cosang[i] < -0.2) {
            bad = 0;
            if (i + 2 - start > best_c) {
                best_s = start;
                best_c = i + 2 - start;
            }
        } else {
            bad++;
            if (bad >= 6)
                start = i + 2;
        }
    }
    free(cosang);
    s0 = best_s;
    cnt = best_c;
    if (cnt < 64) { free(P); return -1; }   /* no interleaved strip found */

    /* the pair phase is the one whose median strand separation is smaller */
    {
        double m[2];
        int p;
        for (p = 0; p < 2; p++) {
            int kn = (cnt - p) / 2, k;
            double *sep = (double *)malloc((size_t)(kn ? kn : 1)
                                           * sizeof(double));
            if (!sep) { free(P); return -1; }
            for (k = 0; k < kn; k++)
                sep[k] = cxa_dist(P[s0 + p + 2 * k], P[s0 + p + 2 * k + 1]);
            m[p] = cxa_median(sep, (size_t)kn);
            free(sep);
        }
        phase = (m[0] <= m[1]) ? 0 : 1;     /* python min() keeps the first */
    }
    s0 += phase;
    pairs = (cnt - phase) / 2;
    out->A   = (cxa_v3 *)malloc((size_t)pairs * sizeof(cxa_v3));
    out->B   = (cxa_v3 *)malloc((size_t)pairs * sizeof(cxa_v3));
    out->mid = (cxa_v3 *)malloc((size_t)pairs * sizeof(cxa_v3));
    if (!out->A || !out->B || !out->mid) { free(P); return -1; }
    for (i = 0; i < pairs; i++) {
        out->A[i] = P[s0 + 2 * i];
        out->B[i] = P[s0 + 2 * i + 1];
        out->mid[i].x = (out->A[i].x + out->B[i].x) / 2;
        out->mid[i].y = (out->A[i].y + out->B[i].y) / 2;
        out->mid[i].z = (out->A[i].z + out->B[i].z) / 2;
    }
    free(P);

    /* The dropout tolerance can bridge into a small neighbouring array at
     * either end.  Trim to the sub-run that best CLOSES INTO A LAP: among
     * sub-runs covering 0.85..1.25 lap lengths and free of bridge jumps
     * (advance > 6 x median), take the one whose ends are closest. */
    adv  = (double *)malloc((size_t)(pairs > 1 ? pairs - 1 : 1)
                            * sizeof(double));
    arc  = (double *)malloc((size_t)pairs * sizeof(double));
    jump = (int *)malloc((size_t)pairs * sizeof(int));
    if (!adv || !arc || !jump) { free(adv); free(arc); free(jump); return -1; }
    for (i = 0; i < pairs - 1; i++)
        adv[i] = cxa_dist(out->mid[i], out->mid[i + 1]);
    ma = cxa_median(adv, (size_t)(pairs - 1));
    if (ma == 0.0)
        ma = 1.0;
    arc[0] = 0.0;
    jump[0] = 0;
    for (i = 0; i < pairs - 1; i++) {
        arc[i + 1] = arc[i] + adv[i];
        jump[i + 1] = jump[i] + (adv[i] > 6 * ma ? 1 : 0);
    }
    lo_len = 0.85 * net->lap;
    hi_len = 1.25 * net->lap;
    b0 = 0;
    b1 = pairs - 1;
    {
        int j = 0, k;
        for (i = 0; i < pairs; i++) {
            if (j < i)
                j = i;
            while (j + 1 < pairs && arc[j] - arc[i] < lo_len)
                j++;
            for (k = j; k < pairs && arc[k] - arc[i] <= hi_len; k++) {
                if (arc[k] - arc[i] >= lo_len && jump[k] == jump[i]) {
                    double c = cxa_dist(out->mid[i], out->mid[k]);
                    if (!have_close || c < best_close) {
                        have_close = 1;
                        best_close = c;
                        b0 = i;
                        b1 = k;
                    }
                }
            }
        }
    }
    free(arc);
    free(jump);
    if (b0 > 0) {
        memmove(out->A, out->A + b0, (size_t)(b1 - b0 + 1) * sizeof(cxa_v3));
        memmove(out->B, out->B + b0, (size_t)(b1 - b0 + 1) * sizeof(cxa_v3));
        memmove(out->mid, out->mid + b0,
                (size_t)(b1 - b0 + 1) * sizeof(cxa_v3));
    }
    pairs = b1 - b0 + 1;
    s0 += 2 * b0;
    out->pairs = pairs;
    {
        double *sep = (double *)malloc((size_t)pairs * sizeof(double));
        double sum = 0.0;
        if (!sep) { free(adv); return -1; }
        for (i = 0; i < pairs; i++)
            sep[i] = cxa_dist(out->A[i], out->B[i]);
        out->width = cxa_median(sep, (size_t)pairs);
        free(sep);
        for (i = 0; i < pairs - 1; i++)
            adv[i] = cxa_dist(out->mid[i], out->mid[i + 1]);
        out->step = cxa_median(adv, (size_t)(pairs - 1));
        for (i = 0; i < pairs - 1; i++)
            sum += adv[i];
        out->close  = cxa_dist(out->mid[0], out->mid[pairs - 1]);
        out->length = sum + out->close;
    }
    free(adv);
    out->off = lo + (uint32_t)s0 * 16;
    out->rel = out->off - net->base;
    out->end = lo + (uint32_t)(s0 + 2 * pairs) * 16;
    return 0;
}

void cxa_cor_free(cxa_cor *c)
{
    if (!c)
        return;
    free(c->A); free(c->B); free(c->mid);
    c->A = c->B = c->mid = NULL;
}

/* Closed lap-length point loops in one [lo,hi) region of the network
 * section.  Array boundaries: a step more than 6 x the region's own median
 * point spacing; a run longer than 1.5 laps whose point count is an exact
 * multiple of the node count is two node-paired arrays and is split back;
 * a run is a lap loop when its length is 0.7..1.5 laps AND it closes on
 * itself within 0.08 of a lap.                                          [S] */
static int loops_in(cxa_bgd *b, const cxa_net *net, uint32_t lo, uint32_t hi,
                    cxa_loop **out, int *nout, int *cap)
{
    const unsigned char *d = b->d;
    int n, i, nb = 0, nr = 0;
    cxa_v3 *pts;
    double *st, scale;
    int (*bounds)[2], (*runs)[2];

    if (hi <= lo)
        return 0;
    n = (int)((hi - lo) / 16);
    if (n < 32)
        return 0;
    pts = (cxa_v3 *)malloc((size_t)n * sizeof(cxa_v3));
    st  = (double *)malloc((size_t)(n - 1) * sizeof(double));
    bounds = malloc((size_t)(n + 1) * sizeof(*bounds));
    runs   = malloc((size_t)(2 * n + 2) * sizeof(*runs));
    if (!pts || !st || !bounds || !runs) {
        free(pts); free(st); free(bounds); free(runs);
        return -1;
    }
    for (i = 0; i < n; i++)
        pts[i] = cxa_f3(d, lo + (size_t)i * 16);
    for (i = 0; i < n - 1; i++)
        st[i] = cxa_dist(pts[i], pts[i + 1]);
    scale = cxa_median(st, (size_t)(n - 1));
    if (scale == 0.0)
        scale = 1.0;
    {
        int start = 0;
        for (i = 0; i < n - 1; i++) {
            if (st[i] > 6.0 * scale) {
                bounds[nb][0] = start;
                bounds[nb][1] = i + 1 - start;
                nb++;
                start = i + 1;
            }
        }
        bounds[nb][0] = start;
        bounds[nb][1] = n - start;
        nb++;
    }
    for (i = 0; i < nb; i++) {
        int s = bounds[i][0], c = bounds[i][1], k = 1, j;
        double plen = 0.0;
        for (j = s; j < s + c - 1; j++)
            plen += st[j];
        if (plen > 1.5 * net->lap && net->nodes && (uint32_t)c > net->nodes
            && (uint32_t)c % net->nodes == 0)
            k = c / (int)net->nodes;
        for (j = 0; j < k; j++) {
            runs[nr][0] = s + j * (c / k);
            runs[nr][1] = c / k;
            nr++;
        }
    }
    for (i = 0; i < nr; i++) {
        int s = runs[i][0], c = runs[i][1], j;
        double *steps, close, length = 0.0, area = 0.0, med;
        cxa_loop *lp;
        if (c < 32)
            continue;
        steps = (double *)malloc((size_t)(c - 1) * sizeof(double));
        if (!steps)
            continue;
        for (j = 0; j < c - 1; j++)
            steps[j] = cxa_dist(pts[s + j], pts[s + j + 1]);
        qsort(steps, (size_t)(c - 1), sizeof(double), cmp_double);
        med = steps[(c - 1) / 2];
        close = cxa_dist(pts[s], pts[s + c - 1]);
        /* python sums the SORTED step list -- keep that order */
        for (j = 0; j < c - 1; j++)
            length += steps[j];
        length += close;
        free(steps);
        if (!(0.7 * net->lap <= length && length <= 1.5 * net->lap))
            continue;
        if (close > 0.08 * net->lap)        /* an open sub-path, not a lap */
            continue;
        for (j = 0; j < c; j++) {
            cxa_v3 a = pts[s + j], q = pts[s + (j + 1) % c];
            area += a.x * q.z - q.x * a.z;
        }
        if (*nout == *cap) {
            int ncap = *cap ? *cap * 2 : 8;
            cxa_loop *tmp = (cxa_loop *)realloc(*out,
                                                (size_t)ncap * sizeof(cxa_loop));
            if (!tmp)
                break;
            *out = tmp;
            *cap = ncap;
        }
        lp = &(*out)[(*nout)++];
        memset(lp, 0, sizeof *lp);
        lp->off = lo + (uint32_t)s * 16;
        lp->count = c;
        lp->pts = (cxa_v3 *)malloc((size_t)c * sizeof(cxa_v3));
        if (!lp->pts)
            break;
        memcpy(lp->pts, pts + s, (size_t)c * sizeof(cxa_v3));
        lp->length = length;
        lp->close  = close;
        lp->med    = med;
        lp->area   = area / 2;
    }
    free(pts); free(st); free(bounds); free(runs);
    return 0;
}

int cxa_lap_loops(cxa_bgd *b, const cxa_net *net, const cxa_cor *cor,
                  cxa_loop **out, int *out_count)
{
    uint32_t base = (net->base + 0x20 + net->nsec * 0x10 + 15) & ~15u;
    int cap = 0;
    *out = NULL;
    *out_count = 0;
    if (loops_in(b, net, base, cor->off, out, out_count, &cap))
        return -1;
    if (loops_in(b, net, cor->end, net->idx, out, out_count, &cap))
        return -1;
    return 0;
}

void cxa_loops_free(cxa_loop *loops, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(loops[i].pts);
    free(loops);
}

/* ------------------- orientation / role assignment helpers (data-derived) */

/* signed lateral offset of q from the closed polyline ref */
static double polyline_lateral(const cxa_v3 *ref, int n, cxa_v3 q, int *out_j)
{
    int k, j = 0;
    double best = 0.0, tx, tz, m;
    cxa_v3 a, bb;
    for (k = 0; k < n; k++) {
        double d2 = cxa_dist2(ref[k], q);
        if (k == 0 || d2 < best) { best = d2; j = k; }
    }
    a = ref[(j + 1) % n];
    bb = ref[j];
    tx = a.x - bb.x;
    tz = a.z - bb.z;
    m = hypot(tx, tz);
    if (m == 0.0)
        m = 1.0;
    if (out_j)
        *out_j = j;
    return ((q.x - bb.x) * (-tz) + (q.z - bb.z) * tx) / m;
}

static double median_offset(const cxa_v3 *ref, int refn,
                            const cxa_v3 *pts, int n, int stride)
{
    int i, k = 0;
    double *v, out;
    v = (double *)malloc((size_t)(n / stride + 2) * sizeof(double));
    if (!v)
        return 0.0;
    for (i = 0; i < n; i += stride)
        v[k++] = fabs(polyline_lateral(ref, refn, pts[i], NULL));
    out = cxa_median(v, (size_t)k);
    free(v);
    return out;
}

/* dot of the polyline tangent nearest `pos` with `fwd` (xz only) */
static double tangent_dot(const cxa_v3 *pts, int n, cxa_v3 pos, cxa_v3 fwd,
                          int *out_j, double *out_dist)
{
    int k, j = 0;
    double best = 0.0, tx, tz, m;
    cxa_v3 a, b;
    for (k = 0; k < n; k++) {
        double d2 = cxa_dist2(pts[k], pos);
        if (k == 0 || d2 < best) { best = d2; j = k; }
    }
    a = pts[(j + 1) % n];
    b = pts[j];
    tx = a.x - b.x;
    tz = a.z - b.z;
    m = hypot(tx, tz);
    if (m == 0.0)
        m = 1.0;
    if (out_j)
        *out_j = j;
    if (out_dist)
        *out_dist = sqrt(cxa_dist2(pts[j], pos));
    return (tx * fwd.x + tz * fwd.z) / m;
}

/* two strands `half` metres either side of a polyline (left, right) */
static void offset_pair(const cxa_v3 *pts, int n, double half,
                        cxa_v3 *A, cxa_v3 *B)
{
    int i;
    for (i = 0; i < n; i++) {
        cxa_v3 p = pts[i];
        cxa_v3 a = pts[(i + 1) % n];
        cxa_v3 b = pts[((i - 1) % n + n) % n];
        double tx = a.x - b.x, tz = a.z - b.z;
        double m = hypot(tx, tz), nx, nz;
        if (m == 0.0)
            m = 1.0;
        nx = -tz / m;
        nz = tx / m;
        A[i].x = p.x + nx * half; A[i].y = p.y; A[i].z = p.z + nz * half;
        B[i].x = p.x - nx * half; B[i].y = p.y; B[i].z = p.z - nz * half;
    }
}

/* ------------------------------------------------------------ road support
 * build/tracks/<ID>/collision.bin ('B3CL', tri_count @+0x08, triangles from
 * +0x28 with stride 40) bucketed on a 20 m XZ grid; a triangle counts as
 * drivable when its unit normal's Y is >= 0.45.                            */

static int cmp_cell(const void *pa, const void *pb)
{
    const int32_t *a = (const int32_t *)pa, *b = (const int32_t *)pb;
    if (a[0] != b[0]) return a[0] < b[0] ? -1 : 1;
    if (a[1] != b[1]) return a[1] < b[1] ? -1 : 1;
    return a[2] < b[2] ? -1 : (a[2] > b[2] ? 1 : 0);
}

int cxa_road_load(cxa_road *r, const char *path)
{
    unsigned char *d;
    size_t len = 0, o;
    uint32_t n, i;
    int32_t *cells;
    int ncell = 0, cap;

    memset(r, 0, sizeof *r);
    r->cellsize = 20.0;
    d = cxa_slurp(path, &len);
    if (!d)
        return -1;
    if (len < 0x28 || memcmp(d, "B3CL", 4) != 0) { free(d); return -1; }
    n = cxa_u32(d, 8);
    if ((size_t)n * 40 + 0x28 > len) { free(d); return -1; }
    r->tri = (double *)malloc((size_t)n * 9 * sizeof(double));
    cap = 8;
    cells = (int32_t *)malloc((size_t)cap * 3 * sizeof(int32_t));
    if (!r->tri || !cells) { free(d); free(r->tri); free(cells); return -1; }
    o = 0x28;
    for (i = 0; i < n; i++, o += 40) {
        double v[9];
        double ux, uy, uz, vx, vy, vz, nx, ny, nz, L;
        int k;
        int32_t gx, gz, gx0, gx1, gz0, gz1;
        for (k = 0; k < 9; k++)
            v[k] = cxa_f32(d, o + (size_t)k * 4);
        ux = v[3] - v[0]; uy = v[4] - v[1]; uz = v[5] - v[2];
        vx = v[6] - v[0]; vy = v[7] - v[1]; vz = v[8] - v[2];
        nx = uy * vz - uz * vy;
        ny = uz * vx - ux * vz;
        nz = ux * vy - uy * vx;
        L = sqrt(nx * nx + ny * ny + nz * nz);
        if (L == 0.0)
            L = 1.0;
        if (ny / L < 0.45)                  /* not a drivable surface */
            continue;
        memcpy(r->tri + (size_t)r->ntri * 9, v, 9 * sizeof(double));
        gx0 = (int32_t)floor(fmin(fmin(v[0], v[3]), v[6]) / r->cellsize);
        gx1 = (int32_t)floor(fmax(fmax(v[0], v[3]), v[6]) / r->cellsize);
        gz0 = (int32_t)floor(fmin(fmin(v[2], v[5]), v[8]) / r->cellsize);
        gz1 = (int32_t)floor(fmax(fmax(v[2], v[5]), v[8]) / r->cellsize);
        for (gx = gx0; gx <= gx1; gx++) {
            for (gz = gz0; gz <= gz1; gz++) {
                if (ncell == cap) {
                    int32_t *tmp;
                    cap *= 2;
                    tmp = (int32_t *)realloc(cells,
                                             (size_t)cap * 3 * sizeof(int32_t));
                    if (!tmp) { free(d); free(cells); free(r->tri); return -1; }
                    cells = tmp;
                }
                cells[ncell * 3 + 0] = gx;
                cells[ncell * 3 + 1] = gz;
                cells[ncell * 3 + 2] = r->ntri;
                ncell++;
            }
        }
        r->ntri++;
    }
    free(d);
    qsort(cells, (size_t)ncell, 3 * sizeof(int32_t), cmp_cell);
    r->cell = cells;
    r->ncell = ncell;
    return 0;
}

void cxa_road_free(cxa_road *r)
{
    if (!r)
        return;
    free(r->tri);
    free(r->cell);
    memset(r, 0, sizeof *r);
}

/* how many of `pts` have NO road-like triangle within 6 m below them */
int cxa_road_support(const cxa_road *r, const cxa_v3 *pts, int n)
{
    const double tol = 6.0;
    int i, bad = 0;
    for (i = 0; i < n; i++) {
        int32_t gx = (int32_t)floor(pts[i].x / r->cellsize);
        int32_t gz = (int32_t)floor(pts[i].z / r->cellsize);
        int lo = 0, hi = r->ncell, hit = 0, k;
        while (lo < hi) {                    /* first entry >= (gx,gz,-inf) */
            int mid = (lo + hi) / 2;
            const int32_t *c = r->cell + mid * 3;
            if (c[0] < gx || (c[0] == gx && c[1] < gz))
                lo = mid + 1;
            else
                hi = mid;
        }
        for (k = lo; k < r->ncell; k++) {
            const int32_t *c = r->cell + k * 3;
            const double *t;
            double d1, d2, d3, ux, uy, uz, vx, vy, vz, nx, ny, nz, h;
            if (c[0] != gx || c[1] != gz)
                break;
            t = r->tri + (size_t)c[2] * 9;
            d1 = (t[3] - t[0]) * (pts[i].z - t[2])
               - (t[5] - t[2]) * (pts[i].x - t[0]);
            d2 = (t[6] - t[3]) * (pts[i].z - t[5])
               - (t[8] - t[5]) * (pts[i].x - t[3]);
            d3 = (t[0] - t[6]) * (pts[i].z - t[8])
               - (t[2] - t[8]) * (pts[i].x - t[6]);
            if (!((d1 >= 0 && d2 >= 0 && d3 >= 0)
                  || (d1 <= 0 && d2 <= 0 && d3 <= 0)))
                continue;
            ux = t[3] - t[0]; uy = t[4] - t[1]; uz = t[5] - t[2];
            vx = t[6] - t[0]; vy = t[7] - t[1]; vz = t[8] - t[2];
            ny = uz * vx - ux * vz;
            if (fabs(ny) < 1e-9)
                continue;
            nx = uy * vz - uz * vy;
            nz = ux * vy - uy * vx;
            h = t[1] + (nx * (t[0] - pts[i].x) + nz * (t[2] - pts[i].z)) / ny;
            if (fabs(h - pts[i].y) < tol) { hit = 1; break; }
        }
        if (!hit)
            bad++;
    }
    return bad;
}

/* =============================================================== analysis */

typedef struct {
    cxa_v3   *pts;
    int       n;
    cxa_loop *loop;             /* NULL for the corridor midline */
    double    grid_off, grid_spread;
    int       unsupported;      /* -1 = not measured */
} cxa_cand;

int cxa_analyse(cxa_analysis *a, const char *gamedata_bgd, const char *event,
                const char *out_dir)
{
    cxa_gridslot *g0;
    cxa_cand *cands;
    int ncand, i, k;
    cxa_road road;
    int have_road = 0;
    double lane;
    cxa_loop *oncoming = NULL, *raceline = NULL;
    cxa_cand *route_c;

    memset(a, 0, sizeof *a);
    a->bgd = (cxa_bgd *)calloc(1, sizeof(cxa_bgd));
    if (!a->bgd)
        return -1;
    if (cxa_bgd_open(a->bgd, gamedata_bgd))
        return -1;
    a->ev = cxa_bgd_event(a->bgd, event ? event : CXA_DEFAULT_EVENT);
    if (!a->ev)
        return -1;
    if (cxa_grid(a->bgd, a->ev, a->grid))
        return -1;
    if (cxa_network(a->bgd, a->ev, &a->net))
        return -1;
    if (cxa_corridor(a->bgd, &a->net, &a->cor))
        return -1;
    if (cxa_lap_loops(a->bgd, &a->net, &a->cor, &a->loops, &a->nloops))
        return -1;
    g0 = &a->grid[0];

    /* orientation: ascending index must run WITH the race direction */
    a->reversed_walls =
        tangent_dot(a->cor.mid, a->cor.pairs, g0->pos, g0->at, NULL, NULL) < 0;
    if (a->reversed_walls) {
        rev_v3(a->cor.A, a->cor.pairs);
        rev_v3(a->cor.B, a->cor.pairs);
        rev_v3(a->cor.mid, a->cor.pairs);
    }

    /* roles pass 1: nearest lap loop to the corridor is the oncoming lane */
    for (i = 0; i < a->nloops; i++) {
        cxa_loop *lp = &a->loops[i];
        lp->corridor_offset = median_offset(a->cor.mid, a->cor.pairs,
                                            lp->pts, lp->count, 7);
        lp->dot = tangent_dot(lp->pts, lp->count, g0->pos, g0->at, NULL,
                              &lp->grid_dist);
    }
    {
        int *rank = (int *)malloc((size_t)(a->nloops ? a->nloops : 1)
                                  * sizeof(int));
        if (!rank)
            return -1;
        for (i = 0; i < a->nloops; i++)
            rank[i] = i;
        for (i = 1; i < a->nloops; i++) {       /* stable insertion sort */
            int cur = rank[i], j = i - 1;
            while (j >= 0 && a->loops[rank[j]].corridor_offset
                             > a->loops[cur].corridor_offset) {
                rank[j + 1] = rank[j];
                j--;
            }
            rank[j + 1] = cur;
        }
        oncoming = a->nloops > 0 ? &a->loops[rank[0]] : NULL;
        raceline = a->nloops > 1 ? &a->loops[rank[1]] : NULL;
        free(rank);
    }
    for (k = 0; k < 2; k++) {
        cxa_loop *lp = k ? raceline : oncoming;
        double want = k ? 1.0 : -1.0;
        if (!lp)
            continue;
        if (lp->dot * want < 0) {
            rev_v3(lp->pts, lp->count);
            lp->reversed = 1;
        } else {
            lp->reversed = 0;
        }
    }

    /* WHICH LINE IS THE DRIVING ROUTE?  Measured, not assumed: score every
     * candidate by how far the START GRID sits off it and, when
     * collision.bin exists, by how many of its points have a road-like
     * triangle under them in the game's own collision world.            [S] */
    ncand = a->nloops + 1;
    cands = (cxa_cand *)calloc((size_t)ncand, sizeof(cxa_cand));
    if (!cands)
        return -1;
    cands[0].pts = a->cor.mid;
    cands[0].n   = a->cor.pairs;
    cands[0].loop = NULL;
    for (i = 0; i < a->nloops; i++) {
        cands[i + 1].pts  = a->loops[i].pts;
        cands[i + 1].n    = a->loops[i].count;
        cands[i + 1].loop = &a->loops[i];
    }
    if (out_dir) {
        char *cp = cxa_join(out_dir, "collision.bin");
        if (cp) {
            have_road = (cxa_road_load(&road, cp) == 0);
            free(cp);
        }
    }
    for (i = 0; i < ncand; i++) {
        double lat[6], sum = 0.0, mn, mx;
        for (k = 0; k < 6; k++)
            lat[k] = polyline_lateral(cands[i].pts, cands[i].n,
                                      a->grid[k].pos, NULL);
        mn = mx = lat[0];
        for (k = 0; k < 6; k++) {
            sum += fabs(lat[k]);
            if (lat[k] < mn) mn = lat[k];
            if (lat[k] > mx) mx = lat[k];
        }
        cands[i].grid_off    = sum / 6;
        cands[i].grid_spread = mx - mn;
        cands[i].unsupported = have_road
            ? cxa_road_support(&road, cands[i].pts, cands[i].n) : -1;
    }
    if (have_road)
        cxa_road_free(&road);
    for (i = 1; i < ncand; i++) {               /* stable insertion sort */
        cxa_cand cur = cands[i];
        int j = i - 1;
        while (j >= 0) {
            int greater;
            if (have_road)
                greater = (cands[j].unsupported > cur.unsupported)
                       || (cands[j].unsupported == cur.unsupported
                           && cands[j].grid_off > cur.grid_off);
            else
                greater = cands[j].grid_off > cur.grid_off;
            if (!greater)
                break;
            cands[j + 1] = cands[j];
            j--;
        }
        cands[j + 1] = cur;
    }
    route_c = &cands[0];
    a->route = route_c->pts;                    /* ALIASES cor.mid or a loop */
    a->route_n = route_c->n;
    a->route_is_loop = route_c->loop != NULL;

    /* roles, measured against the chosen route.  The oncoming carriageway is
     * the nearest lap loop at least one grid-column apart from the route. */
    lane = route_c->grid_spread > 1e-3 ? route_c->grid_spread : 1e-3;
    for (i = 0; i < a->nloops; i++) {
        cxa_loop *lp = &a->loops[i];
        lp->route_offset = median_offset(a->route, a->route_n,
                                         lp->pts, lp->count, 7);
        lp->dot = tangent_dot(lp->pts, lp->count, g0->pos, g0->at, NULL,
                              &lp->grid_dist);
    }
    oncoming = NULL;
    for (i = 0; i < a->nloops; i++) {           /* stable min over the filter */
        cxa_loop *lp = &a->loops[i];
        if (lp->route_offset > lane
            && (!oncoming || lp->route_offset < oncoming->route_offset))
            oncoming = lp;
    }
    if (route_c->loop) {
        raceline = route_c->loop;               /* route already IS the line */
    } else {
        raceline = NULL;
        for (i = 0; i < a->nloops; i++) {
            cxa_loop *lp = &a->loops[i];
            if (lp == oncoming)
                continue;
            if (!raceline || lp->route_offset < raceline->route_offset)
                raceline = lp;
        }
    }
    for (k = 0; k < 2; k++) {
        cxa_loop *lp = k ? raceline : oncoming;
        double want = k ? 1.0 : -1.0;
        if (!lp)
            continue;
        if (lp->dot * want < 0) {
            rev_v3(lp->pts, lp->count);
            lp->reversed = 1;
        } else {
            lp->reversed = 0;
        }
    }
    a->oncoming = oncoming;
    a->raceline = raceline;

    /* wall strands: the pair whose midpoint IS the route */
    if (!route_c->loop) {
        a->wall_a = a->cor.A;                   /* aliases the strip */
        a->wall_b = a->cor.B;
        a->wall_n = a->cor.pairs;
    } else {
        double half = oncoming ? oncoming->route_offset / 2.0
                               : a->cor.width / 2;
        a->wall_a = (cxa_v3 *)malloc((size_t)a->route_n * sizeof(cxa_v3));
        a->wall_b = (cxa_v3 *)malloc((size_t)a->route_n * sizeof(cxa_v3));
        if (!a->wall_a || !a->wall_b) { free(cands); return -1; }
        offset_pair(a->route, a->route_n, half, a->wall_a, a->wall_b);
        a->wall_n = a->route_n;
    }

    a->route_start = 0;
    {
        double best = 0.0;
        for (i = 0; i < a->route_n; i++) {
            double d2 = cxa_dist2(a->route[i], g0->pos);
            if (i == 0 || d2 < best) { best = d2; a->route_start = i; }
        }
    }
    free(cands);
    return 0;
}

void cxa_analysis_free(cxa_analysis *a)
{
    if (!a)
        return;
    if (a->route_is_loop) {                 /* else the walls alias cor.A/B */
        free(a->wall_a);
        free(a->wall_b);
    }
    a->wall_a = a->wall_b = NULL;
    cxa_loops_free(a->loops, a->nloops);
    a->loops = NULL;
    a->nloops = 0;
    cxa_cor_free(&a->cor);
    cxa_net_free(&a->net);
    if (a->bgd) {
        cxa_bgd_close(a->bgd);
        free(a->bgd);
        a->bgd = NULL;
    }
}
