/* cx_src.c -- see cx_src.h for the contract, the format provenance and the
 * loudly-stated deviations.  Two backends behind one handle:
 *
 *   CX_SRC_DIR   open/pread/mmap on an expanded dump.
 *   CX_SRC_XISO  ONE mmap of the whole image; every directory is flattened
 *                in-order on first touch and cached; every file is a
 *                contiguous extent, so map() is base + sector*2048.
 *
 * plus the cx_vfs_* path-prefix shim the pipeline's low-level primitives
 * call instead of libc.
 */
#define _POSIX_C_SOURCE 200809L

#include "cx_src.h"
#include "cx_pool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include "compat/win_posix_compat.h"
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CXS_SECTOR       2048u
/* [S] the volume descriptor sits at sector 32 OF THE PARTITION.  This dump
 * has the partition at byte 0, which is the plain-xiso case; the other
 * offsets are the redump/XGD layouts and cost one 20-byte compare each. */
#define CXS_VD_SECTOR    32u
static const uint64_t CXS_PART_OFF[] = {
    0x00000000ull,          /* plain .xiso / .iso  <- this dump */
    0x0000FD90ull * CXS_SECTOR,
    0x00018300ull * CXS_SECTOR,
    0x00020600ull * CXS_SECTOR,
};
#define CXS_NPART (sizeof CXS_PART_OFF / sizeof CXS_PART_OFF[0])

#define CXS_MAGIC "MICROSOFT*XBOX*MEDIA"
#define CXS_MAGIC_LEN 20u

#define CXS_NAME_MAX 256
#define CXS_PATH_MAX 4096

/* ============================================== NO-MMAP (wasm32) BACKEND ==
 * The XISO backend's whole read path is ONE mmap of the image and pointer
 * arithmetic off it.  That is exact and free on a 64-bit host; on wasm32 the
 * address space is 4 GB total and this dump is 2.4 GB, so the mapping cannot
 * exist -- and Emscripten's mmap of a file is a full read into the heap
 * anyway, which is the same 2.4 GB by another name.
 *
 * So under CX_SRC_NO_MMAP the image is opened but never mapped, and the three
 * sites that dereference the mapping go through cxs_img() instead: it pread()s
 * the extent into a malloc'd block and caches it for the life of the CxSrc,
 * which is exactly the lifetime cx_src_map() promises its callers.  Only the
 * extents actually touched are ever resident.
 *
 * THE NATIVE PATH IS UNTOUCHED: with the macro off, cxs_img() compiles to
 * `s->base + off` and every other line here is the code that shipped. */
#if defined(__EMSCRIPTEN__) && !defined(CX_SRC_NO_MMAP)
#  define CX_SRC_NO_MMAP 1
#endif

#ifdef __EMSCRIPTEN__
/* ...and on the web the image is not even a FILE.  Emscripten's filesystem
 * lives on the main browser thread, so a File cannot be read synchronously
 * through it (FileReaderSync is worker-only) -- web/b3_web.c serves the image
 * from a helper worker instead, over the shared wasm heap.  Same shape as
 * pread(), different plumbing; see web/b3_web.h. */
#include "b3_web.h"
#endif

/* ---------------------------------------------------------------- dirents */
typedef struct {
    char     name[CXS_NAME_MAX];
    uint32_t sector;
    uint32_t size;
    uint8_t  attr;                       /* 0x10 = directory */
} cxs_ent;

typedef struct {
    char     path[CXS_PATH_MAX];         /* normalised, '/'-relative */
    cxs_ent *v;
    int      n;
} cxs_dcache;

/* per-file mmap cache, directory backend only */
typedef struct {
    char        *real;                   /* the resolved on-disk path */
    void        *p;
    size_t       n;
} cxs_fmap;

#ifdef CX_SRC_NO_MMAP
/* one pread'd image extent, held for the life of the CxSrc */
typedef struct {
    uint64_t     off;                    /* absolute byte offset in the image */
    uint64_t     len;
    uint8_t     *p;
} cxs_xmap;
#endif

struct CxSrc {
    int          kind;
    char         root[CXS_PATH_MAX];     /* the spec as opened */

    /* --- CX_SRC_XISO --- */
    int          fd;
    const uint8_t *base;                 /* mmap of the whole image */
    uint64_t     maplen;
    uint64_t     part;                   /* partition byte offset */
    uint32_t     root_sector;
    uint32_t     root_size;
    cxs_dcache  *dc;
    int          ndc, capdc;

#ifdef CX_SRC_NO_MMAP
    /* --- CX_SRC_XISO, no-mmap: the pread'd extents standing in for `base` */
    cxs_xmap    *xm;
    int          nxm, capxm;
    uint64_t     xmbytes;                /* resident total, for the report */
#endif
#ifdef __EMSCRIPTEN__
    int          bridged;                /* image served by web/b3_web.c */
#endif

    /* --- CX_SRC_DIR --- */
    cxs_fmap    *fm;
    int          nfm, capfm;
};

/* THE ONE SEAM.  `len` bytes of the image at absolute offset `off`, valid
 * until cx_src_close().  Callers must already have range-checked against
 * s->maplen -- this reproduces the mmap's contract, not a bounds policy. */
static const uint8_t *cxs_img(CxSrc *s, uint64_t off, uint64_t len)
{
#ifndef CX_SRC_NO_MMAP
    (void)len;
    return s->base + off;                /* the mapping: no copy, as before */
#else
    uint8_t *p;
    uint64_t got = 0;
    int      i;

    for (i = 0; i < s->nxm; i++)         /* already resident? */
        if (s->xm[i].off == off && s->xm[i].len >= len)
            return s->xm[i].p;

    if (s->nxm == s->capxm) {
        int cap = s->capxm ? s->capxm * 2 : 32;
        cxs_xmap *nx = (cxs_xmap *)realloc(s->xm, (size_t)cap * sizeof *nx);
        if (!nx)
            return NULL;
        s->xm = nx;
        s->capxm = cap;
    }
    /* +1 so a zero-length extent still yields a unique non-NULL pointer, as
     * the mapping did. */
    p = (uint8_t *)malloc((size_t)len + 1u);
    if (!p)
        return NULL;
    while (got < len) {                  /* pread can legally come up short */
        long r;
#ifdef __EMSCRIPTEN__
        if (s->bridged)
            r = b3_web_iso_pread(p + got, off + got, (unsigned long)(len - got));
        else
#endif
        r = (long)pread(s->fd, p + got, (size_t)(len - got),
                        (off_t)(off + got));
        if (r <= 0) {
            free(p);
            return NULL;
        }
        got += (uint64_t)r;
    }
    p[len] = 0;
    s->xm[s->nxm].off = off;
    s->xm[s->nxm].len = len;
    s->xm[s->nxm].p   = p;
    s->nxm++;
    s->xmbytes += len;
    return p;
#endif
}

/* =========================================================== path helpers */

/* Normalise a caller's rel path: drop leading '/', collapse runs of '/',
 * drop "." components, keep case.  Returns 0 on success. */
static int cxs_norm(const char *rel, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = rel ? rel : "";

    while (*p) {
        const char *seg;
        size_t len;

        while (*p == '/')
            p++;
        seg = p;
        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        if (len == 0)
            break;
        if (len == 1 && seg[0] == '.')
            continue;
        if (o) {
            if (o + 1 >= cap)
                return -1;
            out[o++] = '/';
        }
        if (o + len >= cap)
            return -1;
        memcpy(out + o, seg, len);
        o += len;
    }
    out[o] = '\0';
    return 0;
}

/* =============================================================== the XISO */

/* One directory extent flattened IN-ORDER.  Explicit stack, not recursion:
 * a corrupt extent must not be able to blow the C stack.  See cx_src.h for
 * why this is a full flatten and not a tree descent. */
#define CXS_STACK_MAX 1024

/* A child offset is real only when it is neither of the two "no child"
 * encodings (0 is the extent's own root, 0xFFFF is the fill word) and the
 * dirent it names fits inside the extent. */
static int cxs_child_ok(uint32_t o, uint32_t dsz)
{
    return o != 0u && o != 0xFFFFu && o * 4u + 14u <= dsz;
}

static uint32_t cxs_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t cxs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int cxs_flatten(const uint8_t *d, uint32_t dsz, cxs_ent **out, int *nout)
{
    uint16_t stack[CXS_STACK_MAX];
    int      sp = 0;
    uint32_t cur = 0;
    int      cur_ok = (dsz >= 14u);
    cxs_ent *v = NULL;
    int      n = 0, cap = 0;
    /* Every node is visited exactly twice (push, pop) and a node costs at
     * least 14 bytes, so dsz/4 iterations is a generous ceiling and any
     * cycle in a corrupt extent hits it instead of spinning. */
    uint32_t guard = 0, guard_max = dsz / 4u + 16u;

    *out = NULL;
    *nout = 0;

    while ((cur_ok || sp > 0) && guard++ < guard_max) {
        uint32_t off, l, r;
        uint8_t  nl;

        if (cur_ok) {
            if (sp == CXS_STACK_MAX)
                break;                      /* pathological depth: stop */
            stack[sp++] = (uint16_t)cur;
            l = cxs_le16(d + cur * 4u);
            if (cxs_child_ok(l, dsz))
                cur = l;
            else
                cur_ok = 0;
            continue;
        }

        cur = stack[--sp];
        off = cur * 4u;
        l   = cxs_le16(d + off);
        r   = cxs_le16(d + off + 2u);
        nl  = d[off + 13];

        /* An all-0xFF slot is the extent's fill, not an entry.  (Only that
         * exact shape is rejected: a genuine leaf whose child words are
         * 0xFFFF still has a real name length and is kept.) */
        if (!(l == 0xFFFFu && r == 0xFFFFu && nl == 0xFFu)
            && nl != 0u && off + 14u + (uint32_t)nl <= dsz) {
            /* CXS_NAME_MAX is 256 and nl is a u8, so the name always fits
             * with room for the NUL; the assert keeps that true. */
            _Static_assert(CXS_NAME_MAX > 255, "name buffer too small");

            if (n == cap) {
                cxs_ent *nv;
                cap = cap ? cap * 2 : 64;
                nv = (cxs_ent *)realloc(v, (size_t)cap * sizeof *v);
                if (!nv) {
                    free(v);
                    return -1;
                }
                v = nv;
            }
            memset(&v[n], 0, sizeof v[n]);
            memcpy(v[n].name, d + off + 14u, nl);
            v[n].name[nl] = '\0';
            v[n].sector = cxs_le32(d + off + 4u);
            v[n].size   = cxs_le32(d + off + 8u);
            v[n].attr   = d[off + 12];
            n++;
        }

        if (cxs_child_ok(r, dsz)) {
            cur = r;
            cur_ok = 1;
        }
    }

    *out = v;
    *nout = n;
    return 0;
}

static const cxs_dcache *cxs_dc_find(const CxSrc *s, const char *norm)
{
    int i;

    for (i = 0; i < s->ndc; i++)
        if (strcasecmp(s->dc[i].path, norm) == 0)
            return &s->dc[i];
    return NULL;
}

/* forward */
static int cxs_iso_lookup(CxSrc *s, const char *norm, cxs_ent *out);

/* The flattened listing of directory `norm`, cached.  0 on success. */
static int cxs_iso_dir(CxSrc *s, const char *norm, const cxs_ent **v, int *n)
{
    const cxs_dcache *hit = cxs_dc_find(s, norm);
    uint32_t sector, size;
    uint64_t byte_off, extent;
    cxs_ent *ents = NULL;
    int      nents = 0;

    if (hit) {
        *v = hit->v;
        *n = hit->n;
        return 0;
    }

    if (!norm[0]) {
        sector = s->root_sector;
        size   = s->root_size;
    } else {
        cxs_ent e;
        if (cxs_iso_lookup(s, norm, &e) != 0)
            return -1;
        if (!(e.attr & 0x10u))
            return -1;
        sector = e.sector;
        size   = e.size;
    }

    /* An empty directory has size 0 (and, in some images, sector 0 too). */
    if (size == 0u) {
        nents = 0;
    } else {
        byte_off = s->part + (uint64_t)sector * CXS_SECTOR;
        extent   = ((uint64_t)size + CXS_SECTOR - 1u) / CXS_SECTOR * CXS_SECTOR;
        const uint8_t *dext;
        if (byte_off > s->maplen || s->maplen - byte_off < extent)
            return -1;
        dext = cxs_img(s, byte_off, extent);
        if (!dext)
            return -1;
        /* dirent offsets are bounded by the DECLARED size, not by the
         * sector-padded extent the range check above used. */
        if (cxs_flatten(dext, size, &ents, &nents) != 0)
            return -1;
    }

    if (s->ndc == s->capdc) {
        cxs_dcache *nd;
        int cap = s->capdc ? s->capdc * 2 : 32;
        nd = (cxs_dcache *)realloc(s->dc, (size_t)cap * sizeof *nd);
        if (!nd) {
            free(ents);
            return -1;
        }
        s->dc = nd;
        s->capdc = cap;
    }
    memset(&s->dc[s->ndc], 0, sizeof s->dc[s->ndc]);
    snprintf(s->dc[s->ndc].path, sizeof s->dc[s->ndc].path, "%s", norm);
    s->dc[s->ndc].v = ents;
    s->dc[s->ndc].n = nents;
    *v = ents;
    *n = nents;
    s->ndc++;
    return 0;
}

/* Resolve a normalised rel path to its dirent.  Component by component,
 * each step a LINEAR case-insensitive scan of a flattened listing. */
static int cxs_iso_lookup(CxSrc *s, const char *norm, cxs_ent *out)
{
    char        acc[CXS_PATH_MAX];
    const char *p = norm;
    size_t      accn = 0;
    cxs_ent     cur;

    if (!norm[0])
        return -1;                       /* the root is not a dirent */

    acc[0] = '\0';
    memset(&cur, 0, sizeof cur);

    while (*p) {
        const cxs_ent *v;
        int            n, i, found = -1;
        const char    *seg = p;
        size_t         len;

        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        while (*p == '/')
            p++;

        acc[accn] = '\0';
        if (cxs_iso_dir(s, acc, &v, &n) != 0)
            return -1;
        for (i = 0; i < n; i++) {
            if (strlen(v[i].name) == len
                && strncasecmp(v[i].name, seg, len) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return -1;
        cur = v[found];

        if (*p) {
            if (!(cur.attr & 0x10u))
                return -1;               /* a file cannot have children */
            if (accn) {
                if (accn + 1 >= sizeof acc)
                    return -1;
                acc[accn++] = '/';
            }
            if (accn + len >= sizeof acc)
                return -1;
            memcpy(acc + accn, seg, len);
            accn += len;
        }
    }
    *out = cur;
    return 0;
}

/* ============================================== the directory backend path */

/* Join root + rel.  On a miss, retry component by component with a
 * case-insensitive readdir scan, so the DIR backend honours the
 * case-insensitive promise the contract makes. */
static int cxs_dir_real(const CxSrc *s, const char *norm, char *out,
                        size_t cap)
{
    struct stat st;
    const char *p;
    size_t      o;

    if ((size_t)snprintf(out, cap, "%s%s%s", s->root, norm[0] ? "/" : "",
                         norm) >= cap)
        return -1;
    if (stat(out, &st) == 0)
        return 0;

    /* case-insensitive walk */
    o = strlen(s->root);
    if (o >= cap)
        return -1;
    memcpy(out, s->root, o + 1);
    p = norm;
    while (*p) {
        const char    *seg = p;
        size_t         len;
        DIR           *d;
        struct dirent *de;
        int            hit = 0;

        while (*p && *p != '/')
            p++;
        len = (size_t)(p - seg);
        while (*p == '/')
            p++;

        d = opendir(out);
        if (!d)
            return -1;
        while ((de = readdir(d)) != NULL) {
            if (strlen(de->d_name) != len
                || strncasecmp(de->d_name, seg, len) != 0)
                continue;
            if (o + 1 + len >= cap) {
                closedir(d);
                return -1;
            }
            out[o++] = '/';
            memcpy(out + o, de->d_name, len);
            o += len;
            out[o] = '\0';
            hit = 1;
            break;
        }
        closedir(d);
        if (!hit)
            return -1;
    }
    return stat(out, &st) == 0 ? 0 : -1;
}

static const void *cxs_dir_map(CxSrc *s, const char *real, size_t *len)
{
    struct stat st;
    int         fd, i;
    void       *p;

    for (i = 0; i < s->nfm; i++)
        if (strcmp(s->fm[i].real, real) == 0) {
            *len = s->fm[i].n;
            return s->fm[i].p;
        }
    fd = open(real, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    if (st.st_size == 0) {
        p = malloc(1);                   /* a valid, non-NULL, 0-length map */
        if (!p) {
            close(fd);
            return NULL;
        }
    } else {
        p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            return NULL;
        }
    }
    close(fd);

    if (s->nfm == s->capfm) {
        cxs_fmap *nf;
        int cap = s->capfm ? s->capfm * 2 : 16;
        nf = (cxs_fmap *)realloc(s->fm, (size_t)cap * sizeof *nf);
        if (!nf) {
            if (st.st_size)
                munmap(p, (size_t)st.st_size);
            else
                free(p);
            return NULL;
        }
        s->fm = nf;
        s->capfm = cap;
    }
    s->fm[s->nfm].real = strdup(real);
    if (!s->fm[s->nfm].real) {
        if (st.st_size)
            munmap(p, (size_t)st.st_size);
        else
            free(p);
        return NULL;
    }
    s->fm[s->nfm].p = p;
    s->fm[s->nfm].n = (size_t)st.st_size;
    s->nfm++;
    *len = (size_t)st.st_size;
    return p;
}

/* ================================================================== open  */

#ifndef CX_SRC_NO_MMAP
static int cxs_probe_iso(const uint8_t *base, uint64_t len, uint64_t *part,
                         uint32_t *root_sector, uint32_t *root_size)
{
    size_t k;

    for (k = 0; k < CXS_NPART; k++) {
        uint64_t vd = CXS_PART_OFF[k] + (uint64_t)CXS_VD_SECTOR * CXS_SECTOR;

        if (vd + CXS_SECTOR > len)
            continue;
        if (memcmp(base + vd, CXS_MAGIC, CXS_MAGIC_LEN) != 0)
            continue;
        *part = CXS_PART_OFF[k];
        *root_sector = cxs_le32(base + vd + 20);
        *root_size   = cxs_le32(base + vd + 24);
        return 0;
    }
    return -1;
}
#else
/* The same probe, one pread'd sector at a time -- the only thing the mmap was
 * doing here was making `base + vd` legal. */
static int cxs_probe_iso_pread(int fd, uint64_t len, uint64_t *part,
                               uint32_t *root_sector, uint32_t *root_size)
{
    uint8_t sec[CXS_SECTOR];
    size_t  k;

    for (k = 0; k < CXS_NPART; k++) {
        uint64_t vd = CXS_PART_OFF[k] + (uint64_t)CXS_VD_SECTOR * CXS_SECTOR;

        if (vd + CXS_SECTOR > len)
            continue;
        if (pread(fd, sec, sizeof sec, (off_t)vd) != (ssize_t)sizeof sec)
            continue;
        if (memcmp(sec, CXS_MAGIC, CXS_MAGIC_LEN) != 0)
            continue;
        *part = CXS_PART_OFF[k];
        *root_sector = cxs_le32(sec + 20);
        *root_size   = cxs_le32(sec + 24);
        return 0;
    }
    return -1;
}
#endif

#ifdef __EMSCRIPTEN__
/* The same probe again, over the image bridge rather than a file descriptor. */
static int cxs_probe_iso_bridge(uint64_t len, uint64_t *part,
                                uint32_t *root_sector, uint32_t *root_size)
{
    uint8_t sec[CXS_SECTOR];
    size_t  k;

    for (k = 0; k < CXS_NPART; k++) {
        uint64_t vd = CXS_PART_OFF[k] + (uint64_t)CXS_VD_SECTOR * CXS_SECTOR;

        if (vd + CXS_SECTOR > len)
            continue;
        if (b3_web_iso_pread(sec, vd, sizeof sec) != (long)sizeof sec)
            continue;
        if (memcmp(sec, CXS_MAGIC, CXS_MAGIC_LEN) != 0)
            continue;
        *part = CXS_PART_OFF[k];
        *root_sector = cxs_le32(sec + 20);
        *root_size   = cxs_le32(sec + 24);
        return 0;
    }
    return -1;
}
#endif

CxSrc *cx_src_open(const char *path_or_dir)
{
    struct stat st;
    CxSrc      *s;
    size_t      n;

    if (!path_or_dir || !*path_or_dir)
        return NULL;

#ifdef __EMSCRIPTEN__
    /* THE BRIDGED IMAGE, before any stat(): on the web the path names a
     * zero-byte placeholder (web/pre.js) and the real bytes come from the
     * helper worker, so stat() would measure the wrong thing. */
    if (b3_web_iso_claims(path_or_dir)) {
        s = (CxSrc *)calloc(1, sizeof *s);
        if (!s)
            return NULL;
        s->fd = -1;
        s->bridged = 1;
        snprintf(s->root, sizeof s->root, "%s", path_or_dir);
        s->maplen = b3_web_iso_size();
        if (cxs_probe_iso_bridge(s->maplen, &s->part, &s->root_sector,
                                 &s->root_size) != 0) {
            free(s);
            return NULL;
        }
        s->kind = CX_SRC_XISO;
        return s;
    }
#endif

    if (stat(path_or_dir, &st) != 0)
        return NULL;

    s = (CxSrc *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fd = -1;

    n = strlen(path_or_dir);
    while (n > 1 && path_or_dir[n - 1] == '/')
        n--;
    if (n >= sizeof s->root) {
        free(s);
        return NULL;
    }
    memcpy(s->root, path_or_dir, n);
    s->root[n] = '\0';

    if (S_ISDIR(st.st_mode)) {
        s->kind = CX_SRC_DIR;
        return s;
    }
    if (!S_ISREG(st.st_mode)) {
        free(s);
        return NULL;
    }

    s->fd = open(s->root, O_RDONLY);
    if (s->fd < 0) {
        free(s);
        return NULL;
    }
    if (fstat(s->fd, &st) != 0 || st.st_size <= 0) {
        close(s->fd);
        free(s);
        return NULL;
    }
    s->maplen = (uint64_t)st.st_size;
#ifndef CX_SRC_NO_MMAP
    s->base = (const uint8_t *)mmap(NULL, (size_t)s->maplen, PROT_READ,
                                    MAP_PRIVATE, s->fd, 0);
    if (s->base == MAP_FAILED) {
        s->base = NULL;
        close(s->fd);
        free(s);
        return NULL;
    }
    if (cxs_probe_iso(s->base, s->maplen, &s->part, &s->root_sector,
                      &s->root_size) != 0) {
        munmap((void *)s->base, (size_t)s->maplen);
        close(s->fd);
        free(s);
        return NULL;                     /* a regular file that is not a XISO */
    }
#else
    /* no mapping exists to probe: read the one candidate sector per layout */
    if (cxs_probe_iso_pread(s->fd, s->maplen, &s->part, &s->root_sector,
                            &s->root_size) != 0) {
        close(s->fd);
        free(s);
        return NULL;                     /* a regular file that is not a XISO */
    }
#endif
    s->kind = CX_SRC_XISO;
    return s;
}

void cx_src_close(CxSrc *s)
{
    int i;

    if (!s)
        return;
    for (i = 0; i < s->ndc; i++)
        free(s->dc[i].v);
    free(s->dc);
    for (i = 0; i < s->nfm; i++) {
        if (s->fm[i].n)
            munmap(s->fm[i].p, s->fm[i].n);
        else
            free(s->fm[i].p);
        free(s->fm[i].real);
    }
    free(s->fm);
#ifdef CX_SRC_NO_MMAP
    for (i = 0; i < s->nxm; i++)
        free(s->xm[i].p);
    free(s->xm);
#endif
    if (s->base)
        munmap((void *)s->base, (size_t)s->maplen);
    if (s->fd >= 0)
        close(s->fd);
    free(s);
}

int cx_src_kind(const CxSrc *s)
{
    return s ? s->kind : CX_SRC_DIR;
}

const char *cx_src_root(const CxSrc *s)
{
    return s ? s->root : "";
}

/* ============================================================== the five ==
 * THREAD SAFETY, and why it is a lock rather than a design.
 *
 * Every one of these grows a lazy cache: cxs_dcache of flattened directory
 * extents, cxs_fmap of per-file mmaps, cxs_xmap of pread'd image extents.
 * Two threads inside one of them at the same time realloc the same array and
 * lose entries -- or worse, hand back a pointer into freed storage.  The
 * extraction stages now run their per-item loops on a worker pool
 * (tools/cextract/cx_pool.h), and several of those items read the disc.
 *
 * So the whole source layer is serialised on cx_pool_lock(), a recursive
 * mutex, taken at the PUBLIC entry points only -- the `_u` bodies below are
 * the unchanged code and call each other freely.  Serialising the reads is
 * not a compromise: the read is microseconds and the decode-and-encode after
 * it is milliseconds, which is the whole reason the pool pays.  When
 * B3_JOBS=1, or in the standalone driver's single-threaded stages, the mutex
 * is uncontended and costs a pair of atomics per call. */

static int cxs_stat_u(CxSrc *s, const char *rel_path,
                      unsigned long long *out_size)
{
    char norm[CXS_PATH_MAX];

    if (!s || cxs_norm(rel_path, norm, sizeof norm) != 0)
        return -1;

    if (s->kind == CX_SRC_DIR) {
        char        real[CXS_PATH_MAX];
        struct stat st;

        if (cxs_dir_real(s, norm, real, sizeof real) != 0)
            return -1;
        if (stat(real, &st) != 0)
            return -1;
        if (out_size)
            *out_size = S_ISDIR(st.st_mode) ? 0ull
                                            : (unsigned long long)st.st_size;
        return 0;
    }

    if (!norm[0]) {                      /* the root directory */
        if (out_size)
            *out_size = 0ull;
        return 0;
    }
    {
        cxs_ent e;
        if (cxs_iso_lookup(s, norm, &e) != 0)
            return -1;
        if (out_size)
            *out_size = (e.attr & 0x10u) ? 0ull : (unsigned long long)e.size;
        return 0;
    }
}

static int cxs_is_dir_u(CxSrc *s, const char *rel_path)
{
    char norm[CXS_PATH_MAX];

    if (!s || cxs_norm(rel_path, norm, sizeof norm) != 0)
        return 0;

    if (s->kind == CX_SRC_DIR) {
        char        real[CXS_PATH_MAX];
        struct stat st;

        if (cxs_dir_real(s, norm, real, sizeof real) != 0)
            return 0;
        return stat(real, &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (!norm[0])
        return 1;
    {
        cxs_ent e;
        if (cxs_iso_lookup(s, norm, &e) != 0)
            return 0;
        return (e.attr & 0x10u) ? 1 : 0;
    }
}

static const uint8_t CXS_EMPTY[1] = { 0 };

static const void *cxs_map_u(CxSrc *s, const char *rel_path,
                             unsigned long long *out_len)
{
    char norm[CXS_PATH_MAX];

    if (!s || cxs_norm(rel_path, norm, sizeof norm) != 0)
        return NULL;

    if (s->kind == CX_SRC_DIR) {
        char        real[CXS_PATH_MAX];
        struct stat st;
        size_t      len = 0;
        const void *p;

        if (cxs_dir_real(s, norm, real, sizeof real) != 0)
            return NULL;
        if (stat(real, &st) != 0 || !S_ISREG(st.st_mode))
            return NULL;
        p = cxs_dir_map(s, real, &len);
        if (p && out_len)
            *out_len = (unsigned long long)len;
        return p;
    }

    {
        cxs_ent  e;
        uint64_t off;

        if (!norm[0] || cxs_iso_lookup(s, norm, &e) != 0)
            return NULL;
        if (e.attr & 0x10u)
            return NULL;                 /* a directory has no file bytes */
        if (e.size == 0u) {
            if (out_len)
                *out_len = 0ull;
            return CXS_EMPTY;
        }
        off = s->part + (uint64_t)e.sector * CXS_SECTOR;
        if (off > s->maplen || s->maplen - off < (uint64_t)e.size)
            return NULL;
        {   /* contiguous extent: no copy under mmap, one pread without it */
            const uint8_t *p = cxs_img(s, off, (uint64_t)e.size);
            if (!p)
                return NULL;
            if (out_len)
                *out_len = (unsigned long long)e.size;
            return p;
        }
    }
}

/* ------------------------------- the locked public faces of the four above */

int cx_src_stat(CxSrc *s, const char *rel_path, unsigned long long *out_size)
{
    int r;
    cx_pool_lock();
    r = cxs_stat_u(s, rel_path, out_size);
    cx_pool_unlock();
    return r;
}

int cx_src_is_dir(CxSrc *s, const char *rel_path)
{
    int r;
    cx_pool_lock();
    r = cxs_is_dir_u(s, rel_path);
    cx_pool_unlock();
    return r;
}

/* The returned pointer stays valid until cx_src_close(): every backing store
 * -- the whole-image mmap, a per-file mmap, a pread'd extent -- is held for
 * the life of the CxSrc, and only the INDEX arrays are ever realloc'd.  That
 * is what lets the copy below happen outside the lock. */
const void *cx_src_map(CxSrc *s, const char *rel_path,
                       unsigned long long *out_len)
{
    const void *p;
    cx_pool_lock();
    p = cxs_map_u(s, rel_path, out_len);
    cx_pool_unlock();
    return p;
}

long cx_src_read(CxSrc *s, const char *rel_path, unsigned long long off,
                 void *buf, long len)
{
    unsigned long long n = 0;
    const uint8_t     *p;

    if (!s || !buf || len < 0)
        return -1;
    if (len == 0)
        return 0;
    p = (const uint8_t *)cx_src_map(s, rel_path, &n);
    if (!p)
        return -1;
    if (off >= n)
        return 0;
    if ((unsigned long long)len > n - off)
        len = (long)(n - off);
    memcpy(buf, p + off, (size_t)len);
    return len;
}

/* ================================================================= list  */

static int cxs_list_push(CxSrcList *l, const char *name, int is_dir)
{
    if (l->n == l->cap) {
        int       cap = l->cap ? l->cap * 2 : 64;
        char    **nv  = (char **)realloc(l->name, (size_t)cap * sizeof *nv);
        uint8_t  *nd;

        if (!nv)
            return -1;
        l->name = nv;
        nd = (uint8_t *)realloc(l->is_dir, (size_t)cap * sizeof *nd);
        if (!nd)
            return -1;
        l->is_dir = nd;
        l->cap = cap;
    }
    l->name[l->n] = strdup(name);
    if (!l->name[l->n])
        return -1;
    l->is_dir[l->n] = (uint8_t)(is_dir ? 1 : 0);
    l->n++;
    return 0;
}

void cx_src_list_free(CxSrcList *l)
{
    int i;

    if (!l)
        return;
    for (i = 0; i < l->n; i++)
        free(l->name[i]);
    free(l->name);
    free(l->is_dir);
    l->name = NULL;
    l->is_dir = NULL;
    l->n = l->cap = 0;
}

/* readdir order, "." and ".." dropped, is_dir resolved the way the pipeline
 * always resolved it: d_type when it is decisive, stat() otherwise (which
 * FOLLOWS symlinks, matching os.walk's entry.is_dir()). */
static int cxs_list_real(const char *real, CxSrcList *out)
{
    DIR           *d = opendir(real);
    struct dirent *de;

    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char        p[CXS_PATH_MAX];
        struct stat st;
        int         isdir;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
#ifdef DT_DIR
        if (de->d_type == DT_DIR) {
            isdir = 1;
        } else if (de->d_type == DT_REG) {
            isdir = 0;
        } else
#endif
        {
            if ((size_t)snprintf(p, sizeof p, "%s/%s", real, de->d_name)
                >= sizeof p)
                continue;
            isdir = (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
        }
        if (cxs_list_push(out, de->d_name, isdir) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int cxs_list_u(CxSrc *s, const char *rel_dir, CxSrcList *out)
{
    char norm[CXS_PATH_MAX];

    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!s || cxs_norm(rel_dir, norm, sizeof norm) != 0)
        return -1;

    if (s->kind == CX_SRC_DIR) {
        char real[CXS_PATH_MAX];

        if (cxs_dir_real(s, norm, real, sizeof real) != 0)
            return -1;
        if (cxs_list_real(real, out) != 0) {
            cx_src_list_free(out);
            return -1;
        }
        return 0;
    }

    {
        const cxs_ent *v;
        int            n, i;

        if (cxs_iso_dir(s, norm, &v, &n) != 0)
            return -1;
        for (i = 0; i < n; i++)
            if (cxs_list_push(out, v[i].name, (v[i].attr & 0x10u) != 0) != 0) {
                cx_src_list_free(out);
                return -1;
            }
        return 0;
    }
}

int cx_src_list(CxSrc *s, const char *rel_dir, CxSrcList *out)
{
    int r;
    cx_pool_lock();
    r = cxs_list_u(s, rel_dir, out);
    cx_pool_unlock();
    return r;
}

/* ================================================================== shim  */

static CxSrc *g_vfs_src;
static char   g_vfs_root[CXS_PATH_MAX];   /* the spec as bound */
static char   g_vfs_real[CXS_PATH_MAX];   /* realpath() of it, when different */
static int    g_vfs_tried;

static void cxs_set_root(const char *spec)
{
    char buf[CXS_PATH_MAX];
    size_t n;

    n = strlen(spec);
    while (n > 1 && spec[n - 1] == '/')
        n--;
    if (n >= sizeof g_vfs_root)
        n = sizeof g_vfs_root - 1;
    memcpy(g_vfs_root, spec, n);
    g_vfs_root[n] = '\0';

    g_vfs_real[0] = '\0';
    if (realpath(g_vfs_root, buf) != NULL && strcmp(buf, g_vfs_root) != 0)
        snprintf(g_vfs_real, sizeof g_vfs_real, "%s", buf);
}

int cx_vfs_bind(CxSrc *src, const char *root_spec)
{
    g_vfs_tried = 1;
    /* A directory source needs no shim at all: leaving the binding empty
     * makes every cx_vfs_* call a straight libc passthrough, so directory
     * extraction runs the pre-existing code path unchanged. */
    if (!src || !root_spec || !*root_spec || cx_src_kind(src) != CX_SRC_XISO) {
        g_vfs_src = NULL;
        g_vfs_root[0] = g_vfs_real[0] = '\0';
        return 0;
    }
    g_vfs_src = src;
    cxs_set_root(root_spec);
    return 0;
}

/* $B3_ISO (or a $B3_GAME_DIR that names a file), consulted once, so a module
 * driven outside cxtract still reads the image.
 *
 * Under the source lock (see "the five"): a pool worker is the first thing in
 * the process that may ask, and two of them opening the image at once would
 * leak one CxSrc and bind the other. */
static void cxs_autobind(void)
{
    const char *e;
    struct stat st;
    CxSrc      *s;

    cx_pool_lock();
    if (g_vfs_tried) {
        cx_pool_unlock();
        return;
    }
    g_vfs_tried = 1;

    e = getenv(CX_SRC_ENV_ISO);
    if (!e || !*e) {
        e = getenv(CX_SRC_ENV_GAME);
        if (!e || !*e || stat(e, &st) != 0 || !S_ISREG(st.st_mode)) {
            cx_pool_unlock();
            return;
        }
    }
    s = cx_src_open(e);
    if (!s) {
        cx_pool_unlock();
        return;
    }
    if (cx_src_kind(s) != CX_SRC_XISO) {
        cx_src_close(s);
        cx_pool_unlock();
        return;
    }
    g_vfs_src = s;
    cxs_set_root(e);
    cx_pool_unlock();
}

CxSrc *cx_vfs_src(void)
{
    cxs_autobind();
    return g_vfs_src;
}

static const char *cxs_strip(const char *path, const char *root)
{
    size_t n;

    if (!root[0])
        return NULL;
    n = strlen(root);
    if (strncmp(path, root, n) != 0)
        return NULL;
    if (path[n] == '\0')
        return path + n;
    if (path[n] != '/')
        return NULL;
    while (path[n] == '/')
        n++;
    return path + n;
}

const char *cx_vfs_rel(const char *path)
{
    const char *r;

    if (!path)
        return NULL;
    if (!cx_vfs_src())
        return NULL;
    r = cxs_strip(path, g_vfs_root);
    if (!r)
        r = cxs_strip(path, g_vfs_real);
    return r;
}

/* "r" / "rb" / "rt" -- anything that can write is not ours. */
static int cxs_read_mode(const char *mode)
{
    if (!mode || mode[0] != 'r')
        return 0;
    return strchr(mode, '+') == NULL;
}

FILE *cx_vfs_fopen(const char *path, const char *mode)
{
    const char        *rel;
    const void        *p;
    unsigned long long n = 0;

    rel = cxs_read_mode(mode) ? cx_vfs_rel(path) : NULL;
    if (!rel)
        return fopen(path, mode);
    p = cx_src_map(g_vfs_src, rel, &n);
    if (!p) {
        errno = ENOENT;
        return NULL;
    }
    /* fmemopen() over the mapped extent: read-only, so the cast away from
     * const is never exercised, and fseek/fseeko/ftell/fread all behave as
     * they would on a real file. */
    return fmemopen((void *)(uintptr_t)p, (size_t)n, "rb");
}

int cx_vfs_stat(const char *path, unsigned long long *out_size,
                int *out_is_dir)
{
    const char *rel = cx_vfs_rel(path);
    struct stat st;

    if (!rel) {
        if (stat(path, &st) != 0)
            return -1;
        if (out_size)
            *out_size = (unsigned long long)st.st_size;
        if (out_is_dir)
            *out_is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        return 0;
    }
    if (cx_src_stat(g_vfs_src, rel, out_size) != 0)
        return -1;
    if (out_is_dir)
        *out_is_dir = cx_src_is_dir(g_vfs_src, rel);
    return 0;
}

int cx_vfs_is_dir(const char *path)
{
    const char *rel;
    struct stat st;

    if (!path || !*path)
        return 0;
    rel = cx_vfs_rel(path);
    if (!rel)
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    return cx_src_is_dir(g_vfs_src, rel);
}

int cx_vfs_is_file(const char *path)
{
    const char *rel;
    struct stat st;

    if (!path || !*path)
        return 0;
    rel = cx_vfs_rel(path);
    if (!rel)
        return stat(path, &st) == 0 && S_ISREG(st.st_mode);
    if (cx_src_stat(g_vfs_src, rel, NULL) != 0)
        return 0;
    return !cx_src_is_dir(g_vfs_src, rel);
}

int cx_vfs_exists(const char *path)
{
    const char *rel;
    struct stat st;

    if (!path || !*path)
        return 0;
    rel = cx_vfs_rel(path);
    if (!rel)
        return stat(path, &st) == 0;
    return cx_src_stat(g_vfs_src, rel, NULL) == 0;
}

int cx_vfs_listdir(const char *path, CxSrcList *out)
{
    const char *rel;

    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!path || !*path)
        return -1;
    rel = cx_vfs_rel(path);
    if (!rel) {
        if (cxs_list_real(path, out) != 0) {
            cx_src_list_free(out);
            return -1;
        }
        return 0;
    }
    return cx_src_list(g_vfs_src, rel, out);
}
