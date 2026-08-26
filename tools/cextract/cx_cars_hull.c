/* cx_cars_hull.c -- the C-only port of tools/emulate_carcol.py's
 * `--extract-hulls`: the per-vehicle CONVEX COLLISION HULL record, written to
 * <out_root>/build/cars/<CLS>_<CarN>.hull for every .bgv player car and every
 * .btv traffic car in the dump.
 *
 * ========================================================= WHAT THE HULL IS
 * The hull is not computed at runtime and it is not computed here: it is a
 * finished 0x600-byte convex-polyhedron record that ships INSIDE each vehicle
 * container at a fixed offset, and the game's only "construction" step is to
 * memcpy it into the live vehicle and rebase five section pointers.
 *
 * The retail chain, corrected ELF mapping (build/burnout3.elf):
 *
 *   FUN_00122830  the vehicle bring-up.  At [C] 0x001229EE it forms
 *                     src = *(int *)(param_1 + 0x40) + 0x1060
 *                 -- the loaded model image plus 0x1060 -- and
 *                     dst = ESI + 0x220
 *                 -- the live vehicle record plus 0x220 -- and calls
 *                 FUN_00122C20.  Two more callers form the SAME pair:
 *                 FUN_0012E4D0 @[C] 0x0012EB54 and FUN_00125BF0 @[C]
 *                 0x00125C65, so container +0x1060 -> veh +0x220 is universal
 *                 and is not a per-caller special case.
 *
 *                 The same `*(param_1 + 0x40)` model base is where FUN_00122830
 *                 takes the collision extents from at [C] 0x00122946..0x0012298A
 *                 (+0xE80 -> veh+0x1D0 bbmax, +0xE90 -> veh+0x1E0 bbmin), the
 *                 two fields tools/emulate_carcol.py's bbox() already reads at
 *                 those FILE offsets -- i.e. the container is mapped flat, so
 *                 model_base + X is file offset X.
 *
 *   FUN_00122C20  the copy itself.  It is a plain section-by-section move with
 *                 NO arithmetic of any kind -- six rep-style loops whose trip
 *                 counts give the record its layout and its total size:
 *
 *                   7 dwords            -> dst +0x0000 .. +0x001C   header
 *                   0x28 x (u16 + u8)   -> dst +0x001C .. +0x0094   40 x 3 B
 *                   (0x0094 .. 0x00A0 is NOT copied -- 12 B of alignment
 *                    padding ahead of the float4 array; it still ships in the
 *                    file, and this extractor preserves it verbatim)
 *                   0x28 x 4 dwords     -> dst +0x00A0 .. +0x0320   40 x f32[4]
 *                   0x16 x 4 dwords     -> dst +0x0320 .. +0x0480   22 x f32[4]
 *                   0x3C x u16          -> dst +0x0480 .. +0x04F8   60 x u16
 *                   0x16 x 3 dwords     -> dst +0x04F8 .. +0x0600   22 x 12 B
 *
 *                 The last span ends EXACTLY at +0x0600, which is where the
 *                 0x600 record size comes from [C], and the trip counts
 *                 0x16 / 0x28 / 0x3C are where the 22 vertex / 40 plane / 60
 *                 edge capacities come from [C] (src/burnout3_carcol.h's
 *                 B3_HULL_MAX_VERTS / _PLANES / _EDGES).
 *
 *   FUN_00122830  the rebase, [C] 0x001229F9..0x00122A0B, immediately after
 *                 the copy: the five dwords at dst+0x00..+0x10 are overwritten
 *                 with the absolute addresses of the five sections,
 *                     veh+0x23C  veh+0x2C0  veh+0x540  veh+0x6A0  veh+0x718
 *                 = record +0x1C, +0xA0, +0x320, +0x480, +0x4F8.  Then
 *                 veh+0x208 = veh+0x220 (the hull pointer the narrow phase
 *                 FUN_0010A9D0 reads) and veh+0x20C = 2.
 *
 *                 IN THE FILE those same five dwords hold the section offsets
 *                 as RECORD-RELATIVE values -- (0x1C, 0xA0, 0x320, 0x480,
 *                 0x4F8), identical in all 107 shipped vehicles [C: read back
 *                 from the dump] -- so the on-disk table is a self-description
 *                 that retail replaces wholesale.  Nothing reads it before the
 *                 rebase, and this extractor ships it unchanged: the .hull file
 *                 is the container's bytes, and src/burnout3_carcol.c's
 *                 b3_carcol_hull_from_record() memcpy's all 0x600 of them for
 *                 exactly that reason.
 *
 * ============================================================ RECORD LAYOUT
 *   +0x0000  u32[5]   section table (record-relative in the file, rebased to
 *                     absolute by FUN_00122830)
 *   +0x0014  u32      flag word; 0 or 2 across the dump [?]
 *   +0x0018  u8       nverts   (<= 22)
 *   +0x0019  u8       nplanes  (<= 40)
 *   +0x001A  u8       nedges   (<= 60)
 *   +0x001B  u8       0 in every shipped vehicle [?]
 *   +0x001C  40 x 3 B per-plane vertex-index triple (three verts of the face);
 *                     rows >= nplanes are stale [?]
 *   +0x0094  12 B     alignment padding, not copied by FUN_00122C20
 *   +0x00A0  40 x f32[4]  planes {n.xyz, d}; inside iff dot(n,p) <= d
 *   +0x0320  22 x f32[4]  vertices, model space
 *   +0x0480  60 x u16     edges, lo byte = v0, hi byte = v1
 *   +0x04F8  22 x 12 B    per-vertex record: leading f32 then 8 B of index /
 *                     flag data; not decoded [?].  Copied and shipped whole.
 *   +0x0600  end
 *
 * ===================================================== FLOAT DETERMINISM: NONE
 * There is no floating-point arithmetic anywhere on this path -- not in
 * FUN_00122C20, not in the python spec, not here.  The planes and vertices are
 * f32 data that is MOVED, never evaluated, so there is no x87-vs-SSE question,
 * no operation-order question and no -ffp-contract sensitivity: the output is
 * the input's bytes.  This module accordingly never interprets the payload; it
 * writes the 0x600-byte window through as opaque bytes, which is the only
 * representation that is bit-exact by construction.  (The house rule about
 * explicit-width little-endian writes has nothing to bite on: no field in the
 * output is synthesised.)
 *
 * ================================================================ THE SPEC
 * tools/emulate_carcol.py::extract_hulls() is the oracle, and it is PURE
 * PYTHON -- the Unicorn machinery in that file drives the collision RESPONSE
 * functions and is not on the hull path at all.  It does, in order:
 *
 *   for ext in ("bgv", "btv"):                      # bgv fleet, then btv
 *       for path in sorted(glob(GAME + "/pveh/<any>/<any>.<ext>")):
 *           data = read(path)
 *           if len(data) < 0x1060 + 0x600: continue          # short-file gate
 *           rec = data[0x1060 : 0x1060+0x600]
 *           nv, np, ne = rec[0x18], rec[0x19], rec[0x1A]
 *           if not (0 < nv <= 22 and 0 < np <= 40 and 0 < ne <= 60):
 *               print skip; continue                         # sanity gate
 *           write(build/cars/<basename(dirname(path))>_<stem>.hull, rec)
 *
 * Both gates are reproduced exactly.  The capacities 22/40/60 are [C] (the
 * FUN_00122C20 trip counts); using them as an upper bound to REJECT a file is
 * the python's own sanity check, not something retail does, so it is [S] --
 * and it is inert on the shipped dump, where all 107 vehicles pass and the
 * observed ranges are nverts 8..22, nplanes 12..40, nedges 18..60, i.e. right
 * up to the capacities and never past them.
 *
 * The two-pass extension order is preserved even though it cannot matter on
 * this dump: no (class, stem) pair occurs as both a .bgv and a .btv, so no
 * output is ever written twice [C: verified over the dump].
 *
 * ONE DELIBERATE DIVERGENCE, and it is unreachable on any real dump: python's
 * glob does not stat its matches, so a DIRECTORY named "<x>.bgv" under pveh
 * would be globbed and then raise IsADirectoryError; this module requires a
 * regular file and skips it.  Verified both ways against the python on a
 * synthetic dump; no such entry exists in the shipped game, where the only
 * non-vehicle name under pveh/ is the file vlist.bin (skipped by both, since
 * the glob needs one directory level).  Every reachable case -- the short-file
 * gate, the counts gate, and its exact skip message -- was checked against the
 * python on that same synthetic dump and matches line for line.
 */
#include "cx_extract.h"
#include "cx_src.h"   /* the dump may be a directory OR an ISO */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* --- the record, from FUN_00122830 / FUN_00122C20 ----------------------- */
#define CXH_HULL_OFF     0x1060u   /* [C] 0x001229EE  model_base + 0x1060 */
#define CXH_HULL_SZ      0x600u    /* [C] FUN_00122C20's spans end at +0x600 */
#define CXH_OFF_NVERTS   0x18u
#define CXH_OFF_NPLANES  0x19u
#define CXH_OFF_NEDGES   0x1Au
#define CXH_MAX_VERTS    22        /* [C] FUN_00122C20 trip count 0x16 */
#define CXH_MAX_PLANES   40        /* [C] FUN_00122C20 trip count 0x28 */
#define CXH_MAX_EDGES    60        /* [C] FUN_00122C20 trip count 0x3C */

#define CXH_PATH_MAX     4096

/* One glob hit, carrying the pieces the output name is built from so the
 * sort can run over the same string python's sorted(glob(...)) sorts: the
 * path below <game_dir>/pveh/, which is "<CLS>/<file>". */
typedef struct {
    char rel[512];      /* "<CLS>/<file>"      -- the sort key */
    char cls[256];      /* basename(dirname(path)) */
    char stem[256];     /* splitext(basename(path))[0] */
} cxh_hit;

typedef struct {
    cxh_hit *v;
    size_t   n, cap;
} cxh_list;

static int cxh_push(cxh_list *l, const cxh_hit *h)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        cxh_hit *v = (cxh_hit *)realloc(l->v, cap * sizeof *v);
        if (!v)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = *h;
    return 0;
}

/* python's sorted() orders str by code point; every path in the dump is
 * 7-bit ASCII, so an unsigned-char comparison is the same order.  strcmp is
 * not guaranteed unsigned, so compare explicitly. */
static int cxh_cmp(const void *a, const void *b)
{
    const unsigned char *x = (const unsigned char *)((const cxh_hit *)a)->rel;
    const unsigned char *y = (const unsigned char *)((const cxh_hit *)b)->rel;

    while (*x && *x == *y) {
        x++;
        y++;
    }
    return (int)*x - (int)*y;
}

/* mkdir -p, self-contained so this module links with any subset of the
 * pipeline present (build.sh globs *.c and the agents land separately). */
static int cxh_mkdir_p(const char *path)
{
    char tmp[CXH_PATH_MAX];
    size_t n = strlen(path);
    size_t i;

    if (n == 0 || n >= sizeof tmp)
        return -1;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return -1;
        if (i < n)
            tmp[i] = '/';
    }
    return 0;
}

static int cxh_is_dir(const char *path)
{
    return cx_vfs_is_dir(path);
}

/* python `os.path.splitext(name)[0]`: strip the LAST dot-extension, and only
 * when the dot is not the leading character. */
static void cxh_stem(const char *name, char *out, size_t cap)
{
    const char *dot = strrchr(name, '.');
    size_t n = (dot && dot != name) ? (size_t)(dot - name) : strlen(name);

    if (n >= cap)
        n = cap - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

/* The glob "<pveh>/<any>/<any>.<ext>": exactly one directory level below
 * pveh, names that do not begin with '.', regular files whose name ends in
 * ".<ext>". */
static int cxh_glob(const char *pveh, const char *ext, cxh_list *out)
{
    CxSrcList top;
    size_t extlen = strlen(ext);
    int rc = 0, i;

    if (cx_vfs_listdir(pveh, &top) != 0)
        return -1;
    for (i = 0; i < top.n; i++) {
        char sub[CXH_PATH_MAX];
        CxSrcList in;
        int j;

        if (top.name[i][0] == '.')
            continue;
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", pveh, top.name[i])
            >= sizeof sub)
            continue;
        if (!top.is_dir[i])
            continue;                       /* pveh/vlist.bin, and the like */
        if (cx_vfs_listdir(sub, &in) != 0)
            continue;
        for (j = 0; j < in.n; j++) {
            char full[CXH_PATH_MAX];
            size_t nl = strlen(in.name[j]);
            cxh_hit h;

            if (in.name[j][0] == '.')
                continue;
            if (nl < extlen + 1 || in.name[j][nl - extlen - 1] != '.')
                continue;
            if (memcmp(in.name[j] + nl - extlen, ext, extlen) != 0)
                continue;
            if ((size_t)snprintf(full, sizeof full, "%s/%s", sub, in.name[j])
                >= sizeof full)
                continue;
            if (in.is_dir[j] || !cx_vfs_is_file(full))
                continue;
            if ((size_t)snprintf(h.rel, sizeof h.rel, "%s/%s",
                                 top.name[i], in.name[j]) >= sizeof h.rel)
                continue;
            snprintf(h.cls, sizeof h.cls, "%s", top.name[i]);
            cxh_stem(in.name[j], h.stem, sizeof h.stem);
            if (cxh_push(out, &h) != 0) {
                rc = -1;
                break;
            }
        }
        cx_src_list_free(&in);
        if (rc != 0)
            break;
    }
    cx_src_list_free(&top);
    return rc;
}

/* Read exactly CXH_HULL_SZ bytes at CXH_HULL_OFF.  Returns 1 on success, 0
 * when the file is shorter than 0x1060+0x600 (the python's `continue`), -1 on
 * an I/O error (the python would raise). */
static int cxh_read_record(const char *path, uint8_t *rec)
{
    FILE *f = cx_vfs_fopen(path, "rb");
    long end;
    size_t got;

    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    end = ftell(f);
    if (end < 0) {
        fclose(f);
        return -1;
    }
    /* python: `if len(data) < BGV_HULL_OFF + HULL_SZ: continue` */
    if ((unsigned long)end < (unsigned long)(CXH_HULL_OFF + CXH_HULL_SZ)) {
        fclose(f);
        return 0;
    }
    if (fseek(f, (long)CXH_HULL_OFF, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    got = fread(rec, 1, CXH_HULL_SZ, f);
    fclose(f);
    return got == CXH_HULL_SZ ? 1 : -1;
}

static int cxh_write_record(const char *path, const uint8_t *rec)
{
    FILE *f = fopen(path, "wb");
    size_t put;

    if (!f)
        return -1;
    put = fwrite(rec, 1, CXH_HULL_SZ, f);
    if (fclose(f) != 0 || put != CXH_HULL_SZ)
        return -1;
    return 0;
}

int cx_extract_hulls(const char *game_dir, const char *out_root)
{
    /* The two container extensions, in the python's order: the .bgv player
     * fleet first, then the .btv traffic fleet.  Both live under pveh/. */
    static const char *const EXT[2] = { "bgv", "btv" };
    char pveh[CXH_PATH_MAX];
    char outdir[CXH_PATH_MAX];
    uint8_t rec[CXH_HULL_SZ];
    int written = 0, skipped = 0, e;

    if (!game_dir || !out_root)
        return 1;
    if ((size_t)snprintf(pveh, sizeof pveh, "%s/pveh", game_dir) >= sizeof pveh)
        return 1;
    if (!cxh_is_dir(pveh)) {
        fprintf(stderr, "[hulls] no vehicle directory: %s\n", pveh);
        return 1;
    }
    if ((size_t)snprintf(outdir, sizeof outdir, "%s/build/cars", out_root)
        >= sizeof outdir)
        return 1;
    if (cxh_mkdir_p(outdir) != 0) {
        fprintf(stderr, "[hulls] cannot create %s\n", outdir);
        return 1;
    }

    for (e = 0; e < 2; e++) {
        cxh_list hits = { NULL, 0, 0 };
        size_t i;
        int rc = 0;

        if (cxh_glob(pveh, EXT[e], &hits) != 0) {
            free(hits.v);
            fprintf(stderr, "[hulls] scan failed under %s\n", pveh);
            return 1;
        }
        qsort(hits.v, hits.n, sizeof *hits.v, cxh_cmp);

        for (i = 0; i < hits.n; i++) {
            const cxh_hit *h = &hits.v[i];
            char src[CXH_PATH_MAX], dst[CXH_PATH_MAX];
            unsigned nv, np, ne;
            int got;

            if ((size_t)snprintf(src, sizeof src, "%s/%s", pveh, h->rel)
                >= sizeof src) {
                rc = 1;
                break;
            }
            got = cxh_read_record(src, rec);
            if (got < 0) {
                fprintf(stderr, "[hulls] cannot read %s\n", src);
                rc = 1;
                break;
            }
            if (got == 0)
                continue;                   /* short file: python `continue` */

            nv = rec[CXH_OFF_NVERTS];
            np = rec[CXH_OFF_NPLANES];
            ne = rec[CXH_OFF_NEDGES];
            /* [S] the python's sanity gate.  The bounds are retail's array
             * capacities [C]; rejecting on them is the extractor's own guard
             * against a container that has no hull at +0x1060. */
            if (!(nv > 0 && nv <= CXH_MAX_VERTS &&
                  np > 0 && np <= CXH_MAX_PLANES &&
                  ne > 0 && ne <= CXH_MAX_EDGES)) {
                printf("  skip (bad counts %u/%u/%u): %s\n", nv, np, ne, src);
                skipped++;
                continue;
            }

            if ((size_t)snprintf(dst, sizeof dst, "%s/%s_%s.hull",
                                 outdir, h->cls, h->stem) >= sizeof dst) {
                rc = 1;
                break;
            }
            if (cxh_write_record(dst, rec) != 0) {
                fprintf(stderr, "[hulls] cannot write %s\n", dst);
                rc = 1;
                break;
            }
            written++;
        }
        free(hits.v);
        if (rc != 0)
            return rc;
    }

    printf("[hulls] wrote %d hull records to %s", written, outdir);
    if (skipped)
        printf(" (%d skipped)", skipped);
    printf("\n");
    return written > 0 ? 0 : 1;
}
