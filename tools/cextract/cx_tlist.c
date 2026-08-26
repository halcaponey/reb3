/* cx_tlist.c -- Tracks/tlist.bin decode + track resolution.
 * Port of tools/extract_tlist.py (the spec).
 *
 * ================================================================ tlist.bin
 * The same container shape as pveh/vlist.bin: a raw dump of the tool's
 * fixed-size C arrays, of which only the first `count` entries are real and
 * the rest is uninitialised tool memory.  4096 bytes, little-endian.  [S]
 *
 *     +0x000  u32      version  = 4
 *     +0x004  u32      count    = 36
 *     +0x008  u32[128] flagsA
 *     +0x208  u32[128] flagsB
 *     +0x408  u64[128] track ids -- base-40 packed 12-char ids ("US_C3_V1")
 *     +0x808  u32[36]  zero
 *
 * The packed ids decode with the game's own base-40 decoder FUN_001AECC0
 * (charset " -/0-9A-Z_"), the same one used for vehicle ids and .bgd event
 * ids.                                                                  [C]
 *
 * =========================================================== id -> directory
 * FUN_001574F0 builds the track directory from the packed id [C]:
 *     strncpy(buf,"tracks/"); FUN_001aecc0(id) -> "US_C3_V1";
 *     +id[0:2] + "/" + id[3:5] + "_" + id[6:8] + "/"
 * so id <REG>_<Cn>_<Vn> maps to Tracks/<REG>/<Cn>_<Vn>/.
 */
#include "cx_extract.h"
#include "cx_common_a.h"
#include "cx_src.h"   /* the dump may be a directory OR an ISO */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CXA_TLIST_MAX 128

typedef struct {
    int  count;
    char id[CXA_TLIST_MAX][13];
} cxa_tlist;

static int tlist_read(const char *game_dir, cxa_tlist *out)
{
    char *path = cxa_join(game_dir, "Tracks/tlist.bin");
    unsigned char *d;
    size_t len = 0;
    uint32_t version, count, i;

    if (!path)
        return -1;
    d = cxa_slurp(path, &len);
    free(path);
    if (!d)
        return -1;
    if (len < 0x808) { free(d); return -1; }
    version = cxa_u32(d, 0);
    count   = cxa_u32(d, 4);
    if (version != 4 || count == 0 || count > CXA_TLIST_MAX) {
        free(d);
        return -1;                          /* tlist.bin version/count */
    }
    out->count = (int)count;
    for (i = 0; i < count; i++) {
        char *id = out->id[i];
        cxa_b40(cxa_u64(d, 0x408 + (size_t)i * 8), id);
        if (strlen(id) != 8 || id[2] != '_' || id[5] != '_') {
            free(d);
            return -1;                      /* implausible track id */
        }
    }
    free(d);
    return 0;
}

/* Tracks/<REG>/<Cn>_<Vn> for a US_C3_V1-style id (FUN_001574F0). [C] */
static int track_dir_of(const char *game_dir, const char *id,
                        char *out, size_t cap)
{
    char rel[32];
    char *joined;
    size_t n;
    rel[0] = 'T'; rel[1] = 'r'; rel[2] = 'a'; rel[3] = 'c';
    rel[4] = 'k'; rel[5] = 's'; rel[6] = '/';
    rel[7] = id[0]; rel[8] = id[1]; rel[9] = '/';
    rel[10] = id[3]; rel[11] = id[4]; rel[12] = '_';
    rel[13] = id[6]; rel[14] = id[7]; rel[15] = 0;
    joined = cxa_join(game_dir, rel);
    if (!joined)
        return -1;
    n = strlen(joined);
    if (n + 1 > cap) { free(joined); return -1; }
    memcpy(out, joined, n + 1);
    free(joined);
    return 0;
}

static int is_dir(const char *p)
{
    return p && *p && cx_vfs_is_dir(p);
}

/* ------------------------------------------------------------ id -> name
 * FUN_00158680(track_index) returns the Globalus string index of the
 * track's display name [C]:
 *     if (-1 < i) return *(undefined4 *)(&DAT_0039EE00 + i * 4);
 *     return 0x4ea;                    // "UNKNOWN TRACK"
 * DAT_0039EE00 is `count` u32 string indices indexed by the tlist index.
 * Globalus.bin: u32 count at +0x08, u32 offset table at +0x10, UTF-16LE
 * payload.  The image is read SECTION-AWARE through the program headers --
 * a flat load silently reads the wrong bytes (HANDOFF.md section 2).
 *
 * The python tool finds the image at <repo>/build/burnout3.elf; this ABI is
 * given no repo root, so the probe order is $B3_ELF then build/burnout3.elf
 * at ./, ../ and ../../.  When none of them is readable the python tool
 * itself falls back to the literal names "TRACK <n>", and so does this. */
#define CXA_NAME_TABLE_VA 0x0039EE00u

static const unsigned char *elf_read(const unsigned char *raw, size_t len,
                                     uint32_t va, size_t n)
{
    uint32_t phoff, i;
    uint16_t entsz, num;
    if (len < 0x34)
        return NULL;
    phoff = cxa_u32(raw, 0x1C);
    entsz = cxa_u16(raw, 0x2A);
    num   = cxa_u16(raw, 0x2C);
    for (i = 0; i < num; i++) {
        size_t p = (size_t)phoff + (size_t)i * entsz;
        uint32_t typ, off, base, fsz;
        if (p + 24 > len)
            break;
        typ  = cxa_u32(raw, p);
        off  = cxa_u32(raw, p + 4);
        base = cxa_u32(raw, p + 8);
        fsz  = cxa_u32(raw, p + 16);
        if (typ != 1)
            continue;
        if (base <= va && va < base + fsz) {
            uint32_t k = va - base;
            if (k + n <= fsz && (size_t)off + k + n <= len)
                return raw + off + k;
            return NULL;
        }
    }
    return NULL;
}

/* one Globalus string as ASCII; returns 0 on success */
static int globalus_ascii(const unsigned char *g, size_t glen, uint32_t idx,
                          char *out, size_t cap)
{
    uint32_t count, off;
    size_t k = 0;
    if (glen < 0x14)
        return -1;
    count = cxa_u32(g, 8);
    if (idx >= count || 0x10 + (size_t)idx * 4 + 4 > glen)
        return -1;
    off = cxa_u32(g, 0x10 + (size_t)idx * 4);
    while ((size_t)off + 1 < glen && !(g[off] == 0 && g[off + 1] == 0)) {
        uint16_t u = cxa_u16(g, off);
        if (u >= 0x80 || k + 1 >= cap)
            return -1;                  /* non-ASCII: not a name we match */
        out[k++] = (char)u;
        off += 2;
    }
    out[k] = 0;
    return 0;
}

/* fill names[i] for every tlist row, or return -1 when the image is absent */
static int track_names(const char *game_dir, int count, char (*names)[64])
{
    static const char *probes[4] = { NULL, "build/burnout3.elf",
                                     "../build/burnout3.elf",
                                     "../../build/burnout3.elf" };
    unsigned char *raw = NULL, *glob = NULL;
    size_t len = 0, glen = 0;
    const unsigned char *tbl;
    char *gp;
    int i, rc = -1;

    probes[0] = getenv("B3_ELF");
    for (i = 0; i < 4 && !raw; i++)
        if (probes[i] && *probes[i])
            raw = cxa_slurp(probes[i], &len);
    if (!raw)
        return -1;
    tbl = elf_read(raw, len, CXA_NAME_TABLE_VA, (size_t)count * 4);
    if (!tbl) { free(raw); return -1; }
    gp = cxa_join(game_dir, "Data/Globalus.bin");
    if (gp) {
        glob = cxa_slurp(gp, &glen);
        free(gp);
    }
    if (glob) {
        rc = 0;
        for (i = 0; i < count; i++)
            if (globalus_ascii(glob, glen, cxa_u32(tbl, (size_t)i * 4),
                               names[i], 64))
                names[i][0] = 0;        /* unreadable -> never matches */
    }
    free(raw);
    free(glob);
    return rc;
}

static int ieq_upper(const char *a, const char *b)
{
    size_t i;
    for (i = 0; a[i] && b[i]; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 0;
    return a[i] == b[i];
}

int cx_resolve_track(const char *game_dir, const char *spec,
                     char *out_track_id, size_t id_cap,
                     char *out_track_dir, size_t dir_cap)
{
    cxa_tlist tl;
    char s[512], key[512];
    const char *env;
    size_t len, i;
    int all_digits = 1;

    if (!game_dir || !*game_dir)
        game_dir = getenv("B3_GAME_DIR");
    if (!game_dir || !*game_dir)
        game_dir = getenv("B3_GAME_ROOT");
    if (!game_dir || !*game_dir)
        game_dir = CXA_DEFAULT_GAME_DIR;

    /* spec = spec or $B3_TRACK or DEFAULT_TRACK */
    if (!spec || !*spec) {
        env = getenv("B3_TRACK");
        spec = (env && *env) ? env : CXA_DEFAULT_TRACK;
    }
    len = strlen(spec);
    if (len >= sizeof s)
        return -1;
    memcpy(s, spec, len + 1);
    /* .strip() then .rstrip('/') */
    {
        size_t a = 0, b = len;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        while (b > a && s[b - 1] == '/') b--;
        memmove(s, s + a, b - a);
        s[b - a] = 0;
        len = b - a;
    }
    for (i = 0; i < len; i++) {
        char c = s[i];
        key[i] = (c == '/' || c == '\\') ? '_' : (char)toupper((unsigned char)c);
        if (!isdigit((unsigned char)c))
            all_digits = 0;
    }
    key[len] = 0;
    if (!len)
        all_digits = 0;

    if (tlist_read(game_dir, &tl))
        return -1;

    for (i = 0; i < (size_t)tl.count; i++) {
        if (!strcmp(tl.id[i], key))
            goto found;
    }
    if (is_dir(s)) {
        /* os.path.normpath(s).split(sep)[-2:] -> "<REG>_<Cn_Vn>" */
        char *p2 = strrchr(s, '/');
        if (p2) {
            char tmp[512];
            char *p1;
            size_t n2;
            memcpy(tmp, s, len + 1);
            p2 = strrchr(tmp, '/');
            *p2 = 0;
            p1 = strrchr(tmp, '/');
            p1 = p1 ? p1 + 1 : tmp;
            n2 = strlen(p1);
            if (n2 + 1 + strlen(p2 + 1) < sizeof key) {
                size_t k;
                memcpy(key, p1, n2);
                key[n2] = '_';
                strcpy(key + n2 + 1, p2 + 1);
                for (k = 0; key[k]; k++)
                    key[k] = (char)toupper((unsigned char)key[k]);
                for (i = 0; i < (size_t)tl.count; i++)
                    if (!strcmp(tl.id[i], key))
                        goto found;
            }
        }
    }
    if (all_digits) {
        long v = strtol(s, NULL, 10);
        if (v >= 0 && v < tl.count) {
            i = (size_t)v;
            goto found;
        }
    }
    {   /* display name, e.g. "SILVER LAKE" (the V1 variant matches first) */
        char (*names)[64] = calloc((size_t)tl.count, 64);
        int hit = -1;
        if (names) {
            int j;
            if (track_names(game_dir, tl.count, names)) {
                for (j = 0; j < tl.count; j++)   /* python's own fallback */
                    snprintf(names[j], 64, "TRACK %d", j);
            }
            for (j = 0; j < tl.count && hit < 0; j++)
                if (names[j][0] && ieq_upper(names[j], s))
                    hit = j;
            free(names);
        }
        if (hit >= 0) {
            i = (size_t)hit;
            goto found;
        }
    }
    return -1;                              /* unknown track */

found:
    if (out_track_id) {
        if (strlen(tl.id[i]) + 1 > id_cap)
            return -1;
        strcpy(out_track_id, tl.id[i]);
    }
    if (out_track_dir) {
        if (track_dir_of(game_dir, tl.id[i], out_track_dir, dir_cap))
            return -1;
    }
    return 0;
}

/* Stage-ABI wrapper: extract_tlist.py writes no per-track artefact. */
int cx_extract_tlist(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir)
{
    char id[64], dir[1024];
    (void)out_dir;
    if (cx_resolve_track(game_dir, track_id, id, sizeof id, dir, sizeof dir))
        return -1;
    if (track_dir && *track_dir && strcmp(track_dir, dir))
        return -1;
    return 0;
}
