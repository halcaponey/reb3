/* cx_cars_vdb.c -- port of tools/extract_car_vdb.py's `generate` mode.
 *
 * Every car's real physics tuning, recovered from the retail ValueDB and
 * emitted as <out_root>/gen/burnout3_car_physics.h.  (The python writes
 * src/burnout3_car_physics.h; this pipeline never writes into src/, so the
 * CONTENT is byte-identical and installing it is the caller's decision.)
 *
 * ================================================================ EVIDENCE
 * Carried over verbatim from the python, whose chain was verified stage by
 * stage before the next was trusted:
 *
 *   1. The per-car physics VDB is NOT embedded in each .bgv.  It is the
 *      single retail file Data/vdb.xml (binary despite the extension),
 *      byte-identical to the community dump vdb_xbox_bo3_release.xml.  A
 *      full scan of every shipped file for the recovered hash dwords finds
 *      them ONLY there.
 *   2. The registration key, captured from the real code by emulating
 *      FUN_00132D10 (64 params) and FUN_00134AC0 (the 9 traffic params)
 *      under Unicorn with hooks at 0x001AEE20 / 0x001AF250 / 0x001AF27F, is
 *
 *          "<param name><group>/../Export/ValueDB/VehiclePhysics/<ID>.cfg"
 *
 *      -- param name FIRST.  Earlier direct-CRC attempts composed
 *      cfg-path-first and matched nothing.
 *   3. The hash core at 0x001AF250 is a table-CRC over the 256 dwords at
 *      0x003F7700 with an ARITHMETIC shift (SAR, not SHR) and no final
 *      inversion.  A pure mirror of it reproduced every emulated hash
 *      (64/64 on three different car names), and the community's
 *      bo3_vdb_definitions.yaml independently lists the same hash for
 *      COMPCAR1 "Mass (Kg)" -- 1803659936 == 0x6B81AAA0.
 *   4. Values are the raw dwords of the 8-byte {u32 value, i32 hash} default
 *      records reinterpreted as f32 (RwReal; all 64 physics params are reals).
 *
 * The car name is the base-40 decode (FUN_001AECC0) of the packed 8-byte
 * vehicle ID in pveh/vlist.bin.
 *
 * ============================================================== DEVIATION
 * The python re-runs the Unicorn registrar emulation as a GATE before
 * writing, so the header can never be regenerated from an unverified hash
 * pipeline.  There is no CPU emulator in this pipeline, so that gate is
 * replaced by the cheapest equivalent standing check with the same power to
 * catch a broken table or a drifted key: the module recomputes COMPCAR1's
 * "Mass (Kg)" key and refuses to write unless it hashes to 0x6B81AAA0, the
 * value BOTH the emulation and the community definitions file report [C].
 * Nothing about the OUTPUT differs -- the emulation never fed a byte of it.
 *
 * INPUTS OUTSIDE game_dir (both read-only, both what the python reads):
 *   <repo>/src/burnout3_physics_params.h   the 64 (offset, group, name) rows
 *   <repo>/build/burnout3.elf              the CRC table at VA 0x003F7700
 * <repo> is the driver's --repo / $B3_REPO_DIR (or $B3_REPO_ROOT standalone),
 * else the checkout this file was built from; $B3_ELF, which the driver sets
 * when it has a mapped retail image, overrides the ELF path outright.
 *
 * ============================================================ PYTHON QUIRKS
 *  Q18 sorted(vals) orders the emitted rows by struct offset, but the
 *      per-car ARRAY order in B3_CAR_PHYSICS is vlist order, not sorted.
 *  Q19 "%.9g" is the value format, with ".0" appended only when the result
 *      contains neither '.' nor 'e' -- so 800 becomes "800.0f" while
 *      0.109999999 is already legal.
 *  Q20 the sanity gate EXCLUDES a car from B3_CAR_PHYSICS entirely rather
 *      than emitting a suspect row, and lists the reason as a trailing
 *      `// EXCLUDED <id>: <reasons>` comment in vlist order.
 *  Q21 every one of the 107 vlist ids is looked up against ALL 64 registered
 *      params, drivable or not: a .btv car simply finds only the 9 the
 *      reduced registrar FUN_00134AC0 registers, because those 9 keys are
 *      the SAME strings.  There is no separate traffic table.
 */
#include "cx_cars.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_TABLE_VA 0x003F7700u    /* 256 dwords used by the CRC core */
#define VDB_TYPE      2

/* ------------------------------------------------------ base-40 (FUN_001AECC0) */
static void b40_decode(uint64_t v, char *out, size_t cap)
{
    char buf[16];
    int i, n = 0, end;

    for (i = 0; i < 12; i++) {
        unsigned r = (unsigned)(v % 40u);
        v /= 40u;
        if (r == 0)       buf[n] = ' ';
        else if (r == 1)  buf[n] = '-';
        else if (r == 2)  buf[n] = '/';
        else if (r == 39) buf[n] = '_';
        else if (r >= 3 && r <= 12)  buf[n] = (char)(r + 0x2D);   /* '0'..'9' */
        else if (r >= 13 && r <= 38) buf[n] = (char)(r + 0x34);   /* 'A'..'Z' */
        else              buf[n] = '?';
        n++;
    }
    /* reversed, then rstrip(' ') */
    end = 0;
    for (i = n - 1; i >= 0; i--)
        if ((size_t)end + 1 < cap)
            out[end++] = buf[i];
    while (end > 0 && out[end - 1] == ' ')
        end--;
    out[end] = 0;
}

/* --------------------------------------------------------------- vlist.bin */
typedef struct {
    char     name[24];
    uint64_t pid;
    int      drivable;
} cxd_vlent;

static int read_vlist(const char *game_dir, cxd_vlent **out, int *n_out)
{
    char p[4096];
    cxd_blob d = { NULL, 0 };
    uint32_t version, count, i;
    cxd_vlent *v;

    cxd_path(p, sizeof p, game_dir, "/pveh/vlist.bin", NULL);
    if (cxd_read_file(p, &d) != 0)
        return -1;
    if (d.n < 8) { cxd_blob_free(&d); return -1; }
    version = cxd_u32(&d, 0);
    count = cxd_u32(&d, 4);
    if (version != 6) {                          /* the python asserts this */
        fprintf(stderr, "[cx_cars_vdb] vlist.bin version %u, expected 6\n",
                (unsigned)version);
        cxd_blob_free(&d);
        return -1;
    }
    if (d.n < (size_t)0x408 + (size_t)count * 8u
        || d.n < 8u + (size_t)count * 4u) {
        cxd_blob_free(&d);
        return -1;
    }
    v = (cxd_vlent *)calloc(count ? count : 1, sizeof *v);
    if (!v) { cxd_blob_free(&d); return -1; }
    for (i = 0; i < count; i++) {
        v[i].drivable = (int)cxd_u32(&d, 8u + (size_t)i * 4u);
        v[i].pid = cxd_u64(&d, 0x408u + (size_t)i * 8u);
        b40_decode(v[i].pid, v[i].name, sizeof v[i].name);
    }
    cxd_blob_free(&d);
    *out = v;
    *n_out = (int)count;
    return 0;
}

/* -------------------------------------------- src/burnout3_physics_params.h */
/* known_param_offsets(): the python's
 *     re.match(r'\s*\{ "([^"]+)", "([^"]+)", (0x[0-9A-Fa-f]+)u,', line)
 * over the generated header, hand-coded. */
typedef struct {
    unsigned offset;
    char     group[96];
    char     name[96];
} cxd_pparam;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int match_param_line(const char *s, cxd_pparam *out)
{
    const char *q;
    size_t l;
    unsigned v = 0;
    int nd = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'
           || *s == '\f' || *s == '\v')
        s++;
    if (strncmp(s, "{ \"", 3) != 0)
        return 0;
    s += 3;
    q = strchr(s, '"');
    if (!q || q == s)                            /* [^"]+ requires >= 1 */
        return 0;
    l = (size_t)(q - s);
    if (l >= sizeof out->group)
        return 0;
    memcpy(out->group, s, l);
    out->group[l] = 0;
    s = q + 1;
    if (strncmp(s, ", \"", 3) != 0)
        return 0;
    s += 3;
    q = strchr(s, '"');
    if (!q || q == s)
        return 0;
    l = (size_t)(q - s);
    if (l >= sizeof out->name)
        return 0;
    memcpy(out->name, s, l);
    out->name[l] = 0;
    s = q + 1;
    if (strncmp(s, ", 0x", 4) != 0)
        return 0;
    s += 4;
    while (hexval((unsigned char)*s) >= 0) {
        v = v * 16u + (unsigned)hexval((unsigned char)*s);
        s++;
        nd++;
    }
    if (nd == 0 || strncmp(s, "u,", 2) != 0)
        return 0;
    out->offset = v;
    return 1;
}

static int read_known(const char *repo, cxd_pparam **out, int *n_out)
{
    char p[4096], line[1024];
    FILE *f;
    cxd_pparam *v = NULL;
    int n = 0, cap = 0;

    cxd_path(p, sizeof p, repo, "/src/burnout3_physics_params.h", NULL);
    f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "[cx_cars_vdb] cannot read %s (set B3_REPO_ROOT)\n", p);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        cxd_pparam pp;
        int i, dup = -1;
        if (!match_param_line(line, &pp))
            continue;
        /* python builds a dict keyed by offset: a repeat overwrites */
        for (i = 0; i < n; i++)
            if (v[i].offset == pp.offset) { dup = i; break; }
        if (dup >= 0) {
            v[dup] = pp;
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            v = (cxd_pparam *)realloc(v, (size_t)cap * sizeof *v);
            if (!v) { fclose(f); return -1; }
        }
        v[n++] = pp;
    }
    fclose(f);
    *out = v;
    *n_out = n;
    return n ? 0 : -1;
}

/* ------------------------------------------ the CRC table out of the ELF */
static int load_hash_table(const char *repo, uint32_t table[256])
{
    char p[4096];
    cxd_blob d = { NULL, 0 };
    uint32_t ph_off;
    uint16_t ph_num;
    int i, found = 0;

    /* the driver points $B3_ELF at the mapped retail image when it has one;
     * otherwise the python's own path, <repo>/build/burnout3.elf */
    {
        const char *e = getenv("B3_ELF");
        if (e && *e)
            cxd_path(p, sizeof p, e, NULL);
        else
            cxd_path(p, sizeof p, repo, "/build/burnout3.elf", NULL);
    }
    if (cxd_read_file(p, &d) != 0) {
        fprintf(stderr, "[cx_cars_vdb] cannot read %s (set B3_REPO_ROOT)\n", p);
        return -1;
    }
    if (d.n < 0x34) { cxd_blob_free(&d); return -1; }
    ph_off = cxd_u32(&d, 0x1C);
    ph_num = cxd_u16(&d, 0x2C);
    for (i = 0; i < (int)ph_num; i++) {
        size_t o = (size_t)ph_off + (size_t)i * 32u;
        uint32_t p_type, off, va, fsz;
        if (o + 32u > d.n)
            break;
        p_type = cxd_u32(&d, o);
        off    = cxd_u32(&d, o + 4);
        va     = cxd_u32(&d, o + 8);
        fsz    = cxd_u32(&d, o + 16);
        if (p_type == 1 && va <= HASH_TABLE_VA
            && HASH_TABLE_VA < (uint64_t)va + fsz) {
            size_t base = (size_t)off + (size_t)(HASH_TABLE_VA - va);
            int k;
            if (base + 0x400u > d.n)
                break;
            for (k = 0; k < 256; k++)
                table[k] = cxd_u32(&d, base + (size_t)k * 4u);
            found = 1;
            break;
        }
    }
    cxd_blob_free(&d);
    return found ? 0 : -1;
}

/* The hash core at 0x001AF250:
 *   or eax,-1 ; for each byte: idx = (eax & 0xFF) ^ sext(byte)
 *                              eax = (eax SAR 8) ^ table[idx]
 * SAR (arithmetic), not SHR, and no final inversion.  The key bytes are
 * ASCII < 0x80, so MOVSX never sets high bits and idx stays in-table. */
static uint32_t gt_hash(const uint32_t table[256], const char *s, size_t n)
{
    uint32_t h = 0xFFFFFFFFu;
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t idx = (h & 0xFFu) ^ (uint32_t)(unsigned char)s[i];
        uint32_t sh = h >> 8;
        if (h & 0x80000000u)
            sh |= 0xFF000000u;
        h = sh ^ table[idx];
    }
    return h;
}

/* compose(): "<param name><group>/../Export/ValueDB/VehiclePhysics/<ID>.cfg" */
static size_t compose(char *out, size_t cap, const char *name,
                      const char *group, const char *param)
{
    int n = snprintf(out, cap,
                     "%s%s/../Export/ValueDB/VehiclePhysics/%s.cfg",
                     param, group, name);
    return (n < 0 || (size_t)n >= cap) ? 0u : (size_t)n;
}

/* ----------------------------------------------------------- Data/vdb.xml */
/* Header {i32 type=2, i32 defaultCount, i32 unk, i32 fileDefCount,
 *         i32 fileDefOffset}; then defaultCount x {u32 rawValue, i32 hash};
 * fileDefs are {u32 active, i32 pathHash}.  Layout per burnout-data-tool's
 * VDBParser.cs, confirmed against the file itself. */
typedef struct {
    uint32_t *hash;
    uint32_t *raw;
    uint32_t  n;
    uint32_t *fd;
    uint32_t  nfd;
} cxd_vdb;

static int read_vdb(const char *game_dir, cxd_vdb *out)
{
    char p[4096];
    cxd_blob d = { NULL, 0 };
    uint32_t t, dvc, fdc, fdo, i;

    memset(out, 0, sizeof *out);
    cxd_path(p, sizeof p, game_dir, "/Data/vdb.xml", NULL);
    if (cxd_read_file(p, &d) != 0) {
        fprintf(stderr, "[cx_cars_vdb] cannot read %s\n", p);
        return -1;
    }
    if (d.n < 20) { cxd_blob_free(&d); return -1; }
    t   = cxd_u32(&d, 0);
    dvc = cxd_u32(&d, 4);
    fdc = cxd_u32(&d, 12);
    fdo = cxd_u32(&d, 16);
    if (t != VDB_TYPE) {
        fprintf(stderr, "[cx_cars_vdb] vdb type %u, expected 2\n", (unsigned)t);
        cxd_blob_free(&d);
        return -1;
    }
    if (d.n < 20u + (size_t)dvc * 8u
        || d.n < (size_t)fdo + (size_t)fdc * 8u) {
        cxd_blob_free(&d);
        return -1;
    }
    out->hash = (uint32_t *)malloc((size_t)(dvc ? dvc : 1) * sizeof *out->hash);
    out->raw  = (uint32_t *)malloc((size_t)(dvc ? dvc : 1) * sizeof *out->raw);
    out->fd   = (uint32_t *)malloc((size_t)(fdc ? fdc : 1) * sizeof *out->fd);
    if (!out->hash || !out->raw || !out->fd) { cxd_blob_free(&d); return -1; }
    for (i = 0; i < dvc; i++) {
        out->raw[i]  = cxd_u32(&d, 20u + (size_t)i * 8u);
        out->hash[i] = cxd_u32(&d, 24u + (size_t)i * 8u);
    }
    out->n = dvc;
    for (i = 0; i < fdc; i++)
        out->fd[i] = cxd_u32(&d, (size_t)fdo + (size_t)i * 8u + 4u);
    out->nfd = fdc;
    cxd_blob_free(&d);
    return 0;
}

static void vdb_free(cxd_vdb *v)
{
    free(v->hash); free(v->raw); free(v->fd);
    memset(v, 0, sizeof *v);
}

/* The python keeps a {hash: raw} dict (and asserts the hashes are unique);
 * this is the same lookup over a sorted index -- 107 cars x 64 params against
 * the whole default table is too many probes for a linear scan. */
static uint32_t *g_vdb_order;
static const cxd_vdb *g_vdb_sort;

static int cmp_vdb_idx(const void *a, const void *b)
{
    uint32_t ha = g_vdb_sort->hash[*(const uint32_t *)a];
    uint32_t hb = g_vdb_sort->hash[*(const uint32_t *)b];
    return (ha > hb) - (ha < hb);
}

static int vdb_index(cxd_vdb *v)
{
    uint32_t i;
    g_vdb_order = (uint32_t *)malloc((size_t)(v->n ? v->n : 1) * sizeof(uint32_t));
    if (!g_vdb_order)
        return -1;
    for (i = 0; i < v->n; i++)
        g_vdb_order[i] = i;
    g_vdb_sort = v;
    if (v->n > 1)
        qsort(g_vdb_order, v->n, sizeof(uint32_t), cmp_vdb_idx);
    return 0;
}

static int vdb_lookup(const cxd_vdb *v, uint32_t h, uint32_t *raw_out)
{
    long lo = 0, hi = (long)v->n - 1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        uint32_t hm = v->hash[g_vdb_order[mid]];
        if (hm == h) { *raw_out = v->raw[g_vdb_order[mid]]; return 1; }
        if (hm < h) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ------------------------------------------------------------ formatting */
static float raw_to_f32(uint32_t raw)
{
    float f;
    memcpy(&f, &raw, 4);
    return f;
}

/* "%.9g", then ".0" when the result carries neither '.' nor 'e' (Q19). */
static void value_literal(double v, char *out, size_t cap)
{
    snprintf(out, cap, "%.9g", v);
    if (!strchr(out, '.') && !strchr(out, 'e')) {
        size_t l = strlen(out);
        if (l + 3 < cap)
            memcpy(out + l, ".0", 3);
    }
}

/* ------------------------------------------------------------ sanity gate */
typedef struct {
    unsigned off[128];
    double   val[128];
    int      n;
} cxd_carvals;

static const double *carval(const cxd_carvals *c, unsigned off)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (c->off[i] == off)
            return &c->val[i];
    return NULL;
}

/* sane(): the python's problem list, joined with "; ". */
static void sane(const cxd_carvals *c, char *out, size_t cap)
{
    char buf[1024];
    const double *m, *fin, *tq, *rev;
    const double *gears[6];
    int i, nfwd = 0;
    double fwd[6];

    out[0] = 0;
    if (c->n == 0) {
        snprintf(out, cap, "no VDB overrides");
        return;
    }
#define ADD(...) do {                                              \
        snprintf(buf, sizeof buf, __VA_ARGS__);                    \
        if (out[0])                                                \
            cxd_path(out + strlen(out), cap - strlen(out), "; ",    \
                     buf, NULL);                                    \
        else                                                       \
            cxd_path(out, cap, buf, NULL);                          \
    } while (0)

    m = carval(c, 0x0B8);
    if (m && !(*m >= 400.0 && *m <= 20000.0))
        ADD("mass %.0f out of range", *m);

    for (i = 0; i < 6; i++) {
        gears[i] = carval(c, 0x0E8u + (unsigned)i * 4u);
        if (gears[i] && *gears[i] > 0.0)
            fwd[nfwd++] = *gears[i];
    }
    for (i = 0; i + 1 < nfwd; i++) {
        if (fwd[i + 1] >= fwd[i]) {
            /* python: "gear ratios not descending: %r" % (gears,) -- a repr
             * of the six-slot list, None for a missing gear */
            char lst[256] = "[", one[48];
            int j;
            for (j = 0; j < 6; j++) {
                if (j)
                    strncat(lst, ", ", sizeof lst - strlen(lst) - 1);
                if (gears[j]) {
                    cxd_py_repr_double(*gears[j], one, sizeof one);
                    strncat(lst, one, sizeof lst - strlen(lst) - 1);
                } else {
                    strncat(lst, "None", sizeof lst - strlen(lst) - 1);
                }
            }
            strncat(lst, "]", sizeof lst - strlen(lst) - 1);
            ADD("gear ratios not descending: %s", lst);
            break;
        }
    }
    fin = carval(c, 0x100);
    if (fin && !(*fin >= 1.0 && *fin <= 8.0))
        ADD("final drive %.2f out of range", *fin);
    tq = carval(c, 0x11C);
    if (tq && !(*tq >= 50.0 && *tq <= 5000.0))
        ADD("torque %.0f out of range", *tq);
    rev = carval(c, 0x0E0);
    if (rev && *rev >= 0.0)
        ADD("reverse ratio %.2f not negative", *rev);
#undef ADD
}

/* ------------------------------------------- the RUNTIME asset, car_physics.bin
 *
 * The same table the header above carries, as a file the port loads at boot
 * instead of compiling in (cx_extract.h, "purge 2"; the loader is
 * src/burnout3_car_physics_runtime.h).  Format spec in that block.
 *
 * No quantisation step is needed here: the header's own "%.9g" round-trips a
 * float exactly, so writing the f32 bits IS the compiled constant. */
static void bin_u32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
    b[2] = (unsigned char)((v >> 16) & 0xFFu);
    b[3] = (unsigned char)((v >> 24) & 0xFFu);
    fwrite(b, 1, 4, f);
}

static void bin_u16(FILE *f, unsigned v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
    fwrite(b, 1, 2, f);
}

static void bin_f32(FILE *f, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    bin_u32(f, u);
}

/* NUL-padded fixed field -- char arrays, not pointers, for the same reason
 * B3TrafficCarId uses them: the loader must not own a string table. */
static void bin_str(FILE *f, const char *s, size_t cap)
{
    char buf[64];
    size_t n = cap < sizeof buf ? cap : sizeof buf, l = s ? strlen(s) : 0;
    memset(buf, 0, n);
    if (l > n - 1) {
        /* a silently truncated key would make the runtime table miss its
         * roster row -- exactly the class of bug this purge exists to kill */
        fprintf(stderr, "[cextract] field overflow: %s does not fit in %d "
                "bytes\n", s, (int)n);
        l = n - 1;
    }
    memcpy(buf, s ? s : "", l);
    fwrite(buf, 1, n, f);
}

static int emit_car_physics_bin(const char *out_root, const cxd_vlent *vl,
                                int nvl, const cxd_carvals *cars,
                                const char (*excl)[256])
{
    char dir[4096], out[4096];
    FILE *f;
    int i, k, ncar = 0;
    uint32_t nparam = 0;

    cxd_path(dir, sizeof dir, out_root, "/build/cars", NULL);
    cxd_path(out, sizeof out, dir, "/car_physics.bin", NULL);
    if (cxd_mkdir_p(dir) != 0) {
        fprintf(stderr, "[cx_cars_vdb] cannot create %s\n", dir);
        return 1;
    }
    for (i = 0; i < nvl; i++) {
        if (excl[i][0])
            continue;
        ncar++;
        nparam += (uint32_t)cars[i].n;
    }
    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "[cx_cars_vdb] cannot write %s\n", out);
        return 1;
    }
    fwrite("B3CP", 1, 4, f);
    bin_u32(f, 1);
    bin_u32(f, (uint32_t)ncar);
    bin_u32(f, nparam);
    for (i = 0; i < nvl; i++) {
        char cls[8], file[32];
        const char *nm = vl[i].name;
        if (excl[i][0])
            continue;
        /* vehicle_file(): COMPCAR10 -> ("COMP", "Car10.bgv"), the header's
         * own derivation, so the two tables key identically */
        snprintf(cls, sizeof cls, "%.4s", nm);
        snprintf(file, sizeof file, "Car%s.%s", nm + 7,
                 vl[i].drivable ? "bgv" : "btv");
        bin_str(f, nm, 16);
        bin_str(f, cls, 8);
        bin_str(f, file, 12);
        bin_u32(f, (uint32_t)cars[i].n);
    }
    for (i = 0; i < nvl; i++) {
        if (excl[i][0])
            continue;
        for (k = 0; k < cars[i].n; k++) {
            bin_u16(f, cars[i].off[k] & 0xFFFFu);
            bin_u16(f, 0);
            bin_f32(f, (float)cars[i].val[k]);
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "[cx_cars_vdb] short write on %s\n", out);
        return 1;
    }
    printf("wrote      : %s (%d cars, %u params)\n", out, ncar,
           (unsigned)nparam);
    return 0;
}

/* ------------------------------------------------------------------ stage */
int cx_extract_car_tuning(const char *game_dir, const char *out_root)
{
    const char *repo = cxd_repo_root();
    char gen[4096], out[4096], key[512];
    uint32_t table[256];
    cxd_pparam *known = NULL;
    cxd_vlent *vl = NULL;
    cxd_vdb vdb;
    cxd_carvals *cars = NULL;
    char (*excl)[256] = NULL;
    int nknown = 0, nvl = 0, i, k, nemit = 0, nexcl = 0;
    FILE *f;

    memset(&vdb, 0, sizeof vdb);
    cxd_path(gen, sizeof gen, out_root, "/gen", NULL);
    cxd_path(out, sizeof out, gen, "/burnout3_car_physics.h", NULL);

    if (load_hash_table(repo, table) != 0)
        return 1;
    if (read_known(repo, &known, &nknown) != 0)
        return 1;
    if (read_vlist(game_dir, &vl, &nvl) != 0) { free(known); return 1; }
    if (read_vdb(game_dir, &vdb) != 0) { free(known); free(vl); return 1; }
    if (vdb_index(&vdb) != 0) { free(known); free(vl); vdb_free(&vdb); return 1; }

    /* The standing self-check that replaces the Unicorn gate: COMPCAR1's
     * "Mass (Kg)" key must hash to 0x6B81AAA0 -- the value the emulation
     * captured AND the community bo3_vdb_definitions.yaml lists [C]. */
    {
        size_t n = compose(key, sizeof key, "COMPCAR1", "Physics/Vehicle",
                           "Mass (Kg)");
        if (!n || gt_hash(table, key, n) != 0x6B81AAA0u) {
            fprintf(stderr, "[cx_cars_vdb] hash self-check FAILED "
                    "(COMPCAR1 Mass (Kg) -> %08X, expected 6B81AAA0) -- "
                    "refusing to write\n",
                    (unsigned)(n ? gt_hash(table, key, n) : 0u));
            goto fail;
        }
    }
    if (cxd_mkdir_p(gen) != 0) {
        fprintf(stderr, "[cx_cars_vdb] cannot create %s\n", gen);
        goto fail;
    }

    cars = (cxd_carvals *)calloc((size_t)(nvl ? nvl : 1), sizeof *cars);
    excl = malloc((size_t)(nvl ? nvl : 1) * sizeof *excl);
    if (!cars || !excl)
        goto fail;

    /* Q21: every vlist id is looked up against ALL 64 registered params. */
    for (i = 0; i < nvl; i++) {
        for (k = 0; k < nknown; k++) {
            size_t n = compose(key, sizeof key, vl[i].name, known[k].group,
                               known[k].name);
            uint32_t raw;
            if (!n)
                continue;
            if (vdb_lookup(&vdb, gt_hash(table, key, n), &raw)) {
                int s = cars[i].n;
                if (s < 512) {
                    cars[i].off[s] = known[k].offset;
                    cars[i].val[s] = raw_to_f32(raw);
                    cars[i].n = s + 1;
                }
            }
        }
        /* sorted(vals) -- by struct offset (Q18) */
        {
            int a, b;
            for (a = 1; a < cars[i].n; a++) {
                unsigned o = cars[i].off[a];
                double v = cars[i].val[a];
                for (b = a - 1; b >= 0 && cars[i].off[b] > o; b--) {
                    cars[i].off[b + 1] = cars[i].off[b];
                    cars[i].val[b + 1] = cars[i].val[b];
                }
                cars[i].off[b + 1] = o;
                cars[i].val[b + 1] = v;
            }
        }
        sane(&cars[i], excl[i], sizeof excl[0]);
        if (excl[i][0])
            nexcl++;
    }

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "[cx_cars_vdb] cannot write %s\n", out);
        goto fail;
    }
    fputs("// Generated by tools/extract_car_vdb.py -- do not edit by hand.\n"
          "//\n"
          "// Per-car physics overrides from the retail Data/vdb.xml, keyed by\n"
          "// hashes recovered by executing the game's own registrar FUN_00132D10\n"
          "// under Unicorn (key = \"<param><group>/../Export/ValueDB/\n"
          "// VehiclePhysics/<VLIST-ID>.cfg\", table-CRC at 0x001AF250 with SAR\n"
          "// semantics). Offsets are the same 0x1D0-struct offsets as\n"
          "// burnout3_physics_params.h; apply with b3_config_set_by_offset().\n"
          "// Drivable cars carry all 64 params; traffic (.btv) cars carry the\n"
          "// 9-param reduced set (FUN_00134AC0: mass + suspension). Cars with no\n"
          "// VDB overrides fall back to b3_physics_defaults().\n"
          "#ifndef BURNOUT3_CAR_PHYSICS_H\n"
          "#define BURNOUT3_CAR_PHYSICS_H\n"
          "\n"
          "typedef struct {\n"
          "    unsigned short offset;   // into the game's 0x1D0 physics struct\n"
          "    float          value;\n"
          "} B3CarParam;\n"
          "\n"
          "typedef struct {\n"
          "    const char*       id;         // vlist vehicle ID (base-40 decoded)\n"
          "    const char*       class_code; // pveh/<class>/\n"
          "    const char*       file;       // matches VehicleInfo.file\n"
          "    const B3CarParam* params;\n"
          "    int               n_params;   // 64 drivable / 9 traffic\n"
          "} B3CarPhysics;\n"
          "\n", f);

    for (i = 0; i < nvl; i++) {
        if (excl[i][0])
            continue;
        fprintf(f, "static const B3CarParam B3_CARPARAMS_%s[] = {\n",
                vl[i].name);
        for (k = 0; k < cars[i].n; k++) {
            char lit[64];
            const char *grp = "?", *prm = "?";
            int j;
            for (j = 0; j < nknown; j++)
                if (known[j].offset == cars[i].off[k]) {
                    grp = known[j].group;
                    prm = known[j].name;
                    break;
                }
            value_literal(cars[i].val[k], lit, sizeof lit);
            fprintf(f, "    { 0x%03Xu, %sf },  // %s/%s\n",
                    cars[i].off[k], lit, grp, prm);
        }
        fputs("};\n", f);
        nemit++;
    }
    fprintf(f, "\n#define B3_CAR_PHYSICS_COUNT %d\n\n", nemit);
    fputs("static const B3CarPhysics B3_CAR_PHYSICS[B3_CAR_PHYSICS_COUNT] = {\n",
          f);
    for (i = 0; i < nvl; i++) {
        char cls[8], file[32];
        const char *nm = vl[i].name;
        if (excl[i][0])
            continue;
        /* vehicle_file(): COMPCAR10 -> ("COMP", "Car10.bgv") */
        snprintf(cls, sizeof cls, "%.4s", nm);
        snprintf(file, sizeof file, "Car%s.%s", nm + 7,
                 vl[i].drivable ? "bgv" : "btv");
        fprintf(f, "    { \"%s\", \"%s\", \"%s\", B3_CARPARAMS_%s, %d },\n",
                nm, cls, file, nm, cars[i].n);
    }
    fputs("};\n\n", f);
    for (i = 0; i < nvl; i++)
        if (excl[i][0])
            fprintf(f, "// EXCLUDED %s: %s\n", vl[i].name, excl[i]);
    fputs("\n#endif // BURNOUT3_CAR_PHYSICS_H\n", f);
    fclose(f);

    /* and the same table as the RUNTIME asset the port actually loads */
    if (emit_car_physics_bin(out_root, vl, nvl, cars,
                             (const char (*)[256])excl) != 0)
        goto fail;

    printf("emitted %d cars to %s (excluded %d: ", nemit, out, nexcl);
    if (!nexcl) {
        printf("none");
    } else {
        int first = 1;
        for (i = 0; i < nvl; i++)
            if (excl[i][0]) {
                if (!first)
                    printf(", ");
                printf("%s", vl[i].name);
                first = 0;
            }
    }
    printf(")\n");

    free(known); free(vl); free(cars); free(excl); free(g_vdb_order);
    vdb_free(&vdb);
    return 0;

fail:
    free(known); free(vl); free(cars); free(excl); free(g_vdb_order);
    vdb_free(&vdb);
    return 1;
}
