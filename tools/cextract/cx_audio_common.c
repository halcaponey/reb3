/* cx_audio_common.c -- see cx_audio_common.h for what each helper is a
 * literal port of.  Nothing in this file knows an audio format; it only
 * reproduces the python stdlib behaviour the four extractors lean on. */
#define _POSIX_C_SOURCE 200809L

#include "cx_audio_common.h"
#include "cx_src.h"                  /* the dump may be a directory OR an ISO */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>

/* ------------------------------------------------------------------ blob */

int cxf_read_file(const char *path, cxf_blob *out)
{
    FILE *f;
    long sz;
    unsigned char *d;

    out->d = NULL;
    out->n = 0;
    f = cx_vfs_fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    d = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1u);
    if (!d) { fclose(f); return -1; }
    if (sz && fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d); fclose(f); return -1;
    }
    fclose(f);
    out->d = d;
    out->n = (size_t)sz;
    return 0;
}

/* python f.read(max): fewer bytes than asked is not an error. */
int cxf_read_head(const char *path, size_t max, cxf_blob *out)
{
    FILE *f;
    unsigned char *d;
    size_t got;

    out->d = NULL;
    out->n = 0;
    f = cx_vfs_fopen(path, "rb");
    if (!f)
        return -1;
    d = (unsigned char *)malloc(max ? max : 1u);
    if (!d) { fclose(f); return -1; }
    got = fread(d, 1, max, f);
    fclose(f);
    out->d = d;
    out->n = got;
    return 0;
}

void cxf_blob_free(cxf_blob *b)
{
    free(b->d);
    b->d = NULL;
    b->n = 0;
}

long long cxf_file_size(const char *path)
{
    unsigned long long sz;

    if (cx_vfs_stat(path, &sz, NULL) != 0)
        return -1;
    return (long long)sz;
}

int cxf_mkdir_p(const char *path)
{
    char tmp[4096];
    size_t i, len;

    len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';
    for (i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
            return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* ------------------------------------------------------- checked LE loads */

int cxf_u8(const cxf_blob *b, size_t o, uint8_t *v)
{
    if (o >= b->n)
        return -1;
    *v = b->d[o];
    return 0;
}

int cxf_u32(const cxf_blob *b, size_t o, uint32_t *v)
{
    if (o + 4 > b->n || o > b->n)
        return -1;
    *v = (uint32_t)b->d[o] | ((uint32_t)b->d[o + 1] << 8) |
         ((uint32_t)b->d[o + 2] << 16) | ((uint32_t)b->d[o + 3] << 24);
    return 0;
}

int cxf_u64(const cxf_blob *b, size_t o, uint64_t *v)
{
    uint32_t lo, hi;
    if (cxf_u32(b, o, &lo) != 0 || cxf_u32(b, o + 4, &hi) != 0)
        return -1;
    *v = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

int cxf_cstr(const cxf_blob *b, size_t o, char *out, size_t outsz)
{
    size_t e = o;

    if (o > b->n)
        return -1;
    while (e < b->n && b->d[e] != 0)
        e++;
    if (e >= b->n)                     /* python: bytes.index -> ValueError */
        return -1;
    if (e - o + 1 > outsz)
        return -1;
    memcpy(out, b->d + o, e - o);
    out[e - o] = '\0';
    return 0;
}

int cxf_field(const cxf_blob *b, size_t o, size_t len, char *out, size_t outsz)
{
    size_t avail, e;

    if (o >= b->n)
        avail = 0;                     /* python slicing just yields b"" */
    else
        avail = (b->n - o < len) ? b->n - o : len;
    e = 0;
    while (e < avail && b->d[o + e] != 0)
        e++;
    if (e + 1 > outsz)
        return -1;
    if (e)
        memcpy(out, b->d + o, e);
    out[e] = '\0';
    return 0;
}

void cxf_strip_ascii(char *s)
{
    /* python bytes.strip() with no argument: b" \t\n\r\x0b\f" */
    static const char *WS = " \t\n\r\v\f";
    size_t a = 0, b = strlen(s);

    while (a < b && strchr(WS, s[a]) != NULL)
        a++;
    while (b > a && strchr(WS, s[b - 1]) != NULL)
        b--;
    if (a)
        memmove(s, s + a, b - a);
    s[b - a] = '\0';
}

/* ------------------------------------------------------------- file walk */

void cxf_strlist_free(cxf_strlist *l)
{
    size_t i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

int cxf_strlist_push(cxf_strlist *l, const char *s)
{
    char *c;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        char **nv = (char **)realloc(l->v, nc * sizeof *nv);
        if (!nv)
            return -1;
        l->v = nv;
        l->cap = nc;
    }
    c = strdup(s);
    if (!c)
        return -1;
    l->v[l->n++] = c;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    /* python sorts str by code point; these paths are byte strings decoded
     * with the UTF-8 filesystem encoding and are pure ASCII in this game, so
     * an unsigned byte compare is the same order. */
    const unsigned char *x = *(const unsigned char *const *)a;
    const unsigned char *y = *(const unsigned char *const *)b;
    return strcmp((const char *)x, (const char *)y);
}

void cxf_strlist_sort(cxf_strlist *l)
{
    if (l->n > 1)
        qsort(l->v, l->n, sizeof *l->v, cmp_str);
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix), i;
    if (lx > ls)
        return 0;
    for (i = 0; i < lx; i++) {
        int a = tolower((unsigned char)s[ls - lx + i]);
        int b = tolower((unsigned char)suffix[i]);
        if (a != b)
            return 0;
    }
    return 1;
}

static int walk_rec(const char *dir, const char *const *exts, size_t nexts,
                    cxf_strlist *out)
{
    CxSrcList l;
    cxf_strlist subdirs = { NULL, 0, 0 };
    size_t i;
    int rc = 0, j;

    /* os.walk swallows unreadable dirs; cx_vfs_listdir() drops "." and ".."
     * and classifies entries the way os.walk's entry.is_dir() does (that is,
     * FOLLOWING symlinks) for the directory backend. */
    if (cx_vfs_listdir(dir, &l) != 0)
        return 0;
    for (j = 0; j < l.n; j++) {
        char path[4096];

        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, l.name[j])
            >= sizeof path)
            continue;
        if (l.is_dir[j]) {
            if (cxf_strlist_push(&subdirs, path) != 0) { rc = -1; break; }
        } else {
            size_t k;
            for (k = 0; k < nexts; k++) {
                if (ends_with_ci(l.name[j], exts[k])) {
                    if (cxf_strlist_push(out, path) != 0)
                        rc = -1;
                    break;
                }
            }
            if (rc)
                break;
        }
    }
    cx_src_list_free(&l);
    for (i = 0; rc == 0 && i < subdirs.n; i++) {
        struct stat ls;
        /* followlinks=False: a symlinked directory is listed but not walked.
         * An image has no symlinks, and lstat() on an image-backed path
         * simply fails, so the branch is a no-op there. */
        if (lstat(subdirs.v[i], &ls) == 0 && S_ISLNK(ls.st_mode))
            continue;
        rc = walk_rec(subdirs.v[i], exts, nexts, out);
    }
    cxf_strlist_free(&subdirs);
    return rc;
}

int cxf_walk_ext(const char *root, const char *const *exts, size_t nexts,
                 cxf_strlist *out)
{
    char norm[4096];
    size_t len;

    len = strlen(root);
    if (len == 0 || len >= sizeof norm)
        return -1;
    memcpy(norm, root, len + 1);
    while (len > 1 && norm[len - 1] == '/')
        norm[--len] = '\0';
    /* python main(): `if os.path.isdir(p): walk... else: files.append(p)` --
     * a path that is NOT a directory is taken as one input file, extension
     * filter and all bypassed.  Keeping that makes a single-file run of the
     * C produce the same output (and the same log) as the python's. */
    if (cx_vfs_exists(norm) && !cx_vfs_is_dir(norm))
        return cxf_strlist_push(out, norm);
    return walk_rec(norm, exts, nexts, out);
}

/* ------------------------------------------------------------- os.path */

void cxf_splitext(const char *p, char *root, size_t rootsz)
{
    /* CPython posixpath._splitext: the extension starts at the last '.'
     * after the last '/', unless every character between the two is a '.'
     * (so ".bashrc" and "..." have no extension). */
    const char *sep = strrchr(p, '/');
    const char *dot = strrchr(p, '.');
    size_t cut = strlen(p);

    if (dot && (!sep || dot > sep)) {
        const char *q = sep ? sep + 1 : p;
        while (q < dot && *q == '.')
            q++;
        if (q < dot)
            cut = (size_t)(dot - p);
    }
    if (cut >= rootsz)
        cut = rootsz ? rootsz - 1 : 0;
    memcpy(root, p, cut);
    root[cut] = '\0';
}

int cxf_path_tag(const char *path, const char *root, char *out, size_t outsz)
{
    char norm[4096], rel[4096];
    size_t rl, i;

    rl = strlen(root);
    if (rl == 0 || rl >= sizeof norm)
        return -1;
    memcpy(norm, root, rl + 1);
    while (rl > 1 && norm[rl - 1] == '/')
        norm[--rl] = '\0';

    if (!strncmp(path, norm, rl) && path[rl] == '/') {
        if (strlen(path + rl + 1) >= sizeof rel)
            return -1;
        strcpy(rel, path + rl + 1);
    } else {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (strlen(base) >= sizeof rel)
            return -1;
        strcpy(rel, base);
    }
    cxf_splitext(rel, rel, sizeof rel);
    if (strlen(rel) >= outsz)
        return -1;
    strcpy(out, rel);
    for (i = 0; out[i]; i++)
        if (out[i] == '/')
            out[i] = '_';
    return 0;
}

/* -------------------------------------------------------------- wave I/O */

static void put_u16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

int cxf_write_wav(const char *path, const unsigned char *pcm, size_t n,
                  uint32_t rate, uint32_t channels, uint32_t sampwidth)
{
    unsigned char h[44];
    FILE *f;

    if (channels == 0 || sampwidth == 0)
        return -1;
    /* [C] Wave_write: the header is written with nframes*blockalign and then
     * _patchheader() rewrites both lengths with the bytes actually written,
     * so the shipped values are simply `n`.  No pad byte is ever emitted. */
    memcpy(h + 0, "RIFF", 4);
    put_u32(h + 4, (uint32_t)(36u + n));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16);
    put_u16(h + 20, 1);                                  /* WAVE_FORMAT_PCM */
    put_u16(h + 22, channels);
    put_u32(h + 24, rate);
    put_u32(h + 28, channels * rate * sampwidth);
    put_u16(h + 32, (uint16_t)(channels * sampwidth));
    put_u16(h + 34, (uint16_t)(sampwidth * 8));
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, (uint32_t)n);

    f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(h, 1, sizeof h, f) != sizeof h) { fclose(f); return -1; }
    if (n && fwrite(pcm, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

int cxf_read_wav(const char *path, uint32_t *rate, uint32_t *channels,
                 uint32_t *sampwidth, unsigned char **pcm, size_t *n)
{
    cxf_blob b;
    size_t p;
    uint32_t riff_sz;
    int have_fmt = 0;

    *pcm = NULL;
    *n = 0;
    if (cxf_read_file(path, &b) != 0)
        return -1;
    if (b.n < 12 || memcmp(b.d, "RIFF", 4) || memcmp(b.d + 8, "WAVE", 4)) {
        cxf_blob_free(&b);
        return -1;
    }
    if (cxf_u32(&b, 4, &riff_sz) != 0) { cxf_blob_free(&b); return -1; }
    p = 12;
    /* python's wave reader walks chunks with the `chunk` module (odd chunk
     * sizes carry a pad byte) and stops at `data`. */
    while (p + 8 <= b.n) {
        char id[5];
        uint32_t sz;
        memcpy(id, b.d + p, 4);
        id[4] = '\0';
        if (cxf_u32(&b, p + 4, &sz) != 0)
            break;
        if (!strcmp(id, "fmt ")) {
            uint32_t ch, sr, bits, tag;
            if (sz < 16 || p + 8 + 16 > b.n)
                break;
            tag  = (uint32_t)b.d[p + 8] | ((uint32_t)b.d[p + 9] << 8);
            ch   = (uint32_t)b.d[p + 10] | ((uint32_t)b.d[p + 11] << 8);
            if (cxf_u32(&b, p + 12, &sr) != 0)
                break;
            bits = (uint32_t)b.d[p + 22] | ((uint32_t)b.d[p + 23] << 8);
            if (tag != 1 || ch == 0)
                break;
            *channels = ch;
            *rate = sr;
            *sampwidth = (bits + 7) / 8;
            have_fmt = 1;
        } else if (!strcmp(id, "data")) {
            size_t framesz, avail, want;
            if (!have_fmt || *sampwidth == 0)
                break;
            framesz = (size_t)(*channels) * (size_t)(*sampwidth);
            avail = b.n - (p + 8);
            want = sz < avail ? sz : avail;
            want = (want / framesz) * framesz;   /* nframes * framesize */
            *pcm = (unsigned char *)malloc(want ? want : 1u);
            if (!*pcm)
                break;
            memcpy(*pcm, b.d + p + 8, want);
            *n = want;
            cxf_blob_free(&b);
            return 0;
        }
        p += 8 + sz + (sz & 1u);
    }
    cxf_blob_free(&b);
    return -1;
}

void cxf_pcm16_stats(const unsigned char *pcm, size_t n,
                     double *rms, int *nonconst)
{
    size_t cnt = n / 2, i;
    uint64_t acc = 0;
    int lo, hi;

    if (cnt == 0) {
        *rms = 0.0;
        *nonconst = 0;
        return;
    }
    lo = hi = (int16_t)((uint16_t)pcm[0] | ((uint16_t)pcm[1] << 8));
    for (i = 0; i < cnt; i++) {
        int s = (int16_t)((uint16_t)pcm[2 * i] | ((uint16_t)pcm[2 * i + 1] << 8));
        acc += (uint64_t)((int64_t)s * (int64_t)s);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    *rms = sqrt((double)acc / (double)cnt);
    *nonconst = (hi != lo);
}

/* ------------------------------------------------------------- subprocess */

int cxf_have_ffmpeg(void)
{
    const char *pathenv = getenv("PATH");
    char buf[4096];
    const char *p;

    if (!pathenv || !*pathenv)
        pathenv = "/usr/local/bin:/usr/bin:/bin";
    p = pathenv;
    while (*p) {
        const char *e = strchr(p, ':');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 0) { l = 1; }
        if (l + 8 < sizeof buf) {
            memcpy(buf, p, l);
            buf[l] = '\0';
            strcat(buf, "/ffmpeg");
            if (access(buf, X_OK) == 0)
                return 1;
        }
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

int cxf_run(const char *const argv[])
{
#ifdef _WIN32
    (void)argv;
    return -1;
#else
    pid_t pid = fork();
    int st;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* subprocess.run(..., stdout=DEVNULL, stderr=DEVNULL) */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2)
                close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return -1;
#endif
}

/* ------------------------------------------------------------------ sha1 */

static uint32_t rol32(uint32_t v, int s)
{
    return (v << s) | (v >> (32 - s));
}

void cxf_sha1(const unsigned char *d, size_t n, unsigned char out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                      0xC3D2E1F0u };
    uint64_t bits = (uint64_t)n * 8u;
    size_t total = n + 1;
    size_t pad, i, off;
    unsigned char tail[128];
    size_t taillen;

    pad = (56 - (total % 64) + 64) % 64;
    taillen = 1 + pad + 8;
    memset(tail, 0, sizeof tail);
    tail[0] = 0x80;
    for (i = 0; i < 8; i++)
        tail[1 + pad + i] = (unsigned char)(bits >> (56 - 8 * i));

    off = 0;
    while (off < n + taillen) {
        unsigned char blk[64];
        uint32_t w[80], a, b, c, e2, f, k, t;
        int j;
        for (j = 0; j < 64; j++) {
            size_t idx = off + (size_t)j;
            blk[j] = idx < n ? d[idx] : tail[idx - n];
        }
        for (j = 0; j < 16; j++)
            w[j] = ((uint32_t)blk[4 * j] << 24) | ((uint32_t)blk[4 * j + 1] << 16) |
                   ((uint32_t)blk[4 * j + 2] << 8) | (uint32_t)blk[4 * j + 3];
        for (j = 16; j < 80; j++)
            w[j] = rol32(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
        a = h[0]; b = h[1]; c = h[2]; t = h[3]; e2 = h[4];
        for (j = 0; j < 80; j++) {
            uint32_t tmp;
            if (j < 20)      { f = (b & c) | (~b & t);            k = 0x5A827999u; }
            else if (j < 40) { f = b ^ c ^ t;                     k = 0x6ED9EBA1u; }
            else if (j < 60) { f = (b & c) | (b & t) | (c & t);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ t;                     k = 0xCA62C1D6u; }
            tmp = rol32(a, 5) + f + e2 + k + w[j];
            e2 = t; t = c; c = rol32(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += t; h[4] += e2;
        off += 64;
    }
    for (i = 0; i < 5; i++) {
        out[4 * i + 0] = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)(h[i]);
    }
}

void cxf_hex(const unsigned char *d, size_t n, char *out)
{
    static const char HEX[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = HEX[d[i] >> 4];
        out[2 * i + 1] = HEX[d[i] & 0xF];
    }
    out[2 * n] = '\0';
}

/* ------------------------------------------------------------ return code */

int cxf_rc(int hard_errors, int data_fails)
{
    if (hard_errors)
        return 1;
    if (data_fails && getenv("CX_AUDIO_PY_RC"))
        return 1;
    return 0;
}

/* -------------------------------------------------------------- game dir */

const char *cxf_game_dir(const char *game_dir)
{
    const char *g;
    if (game_dir && *game_dir)
        return game_dir;
    /* $B3_ISO names the Xbox image; cx_src/cx_vfs serve it behind the same
     * "%s/Tracks/..." joins an expanded dump takes. */
    g = getenv(CX_SRC_ENV_ISO);
    if (g && *g)
        return g;
    g = getenv("B3_GAME_DIR");
    if (g && *g)
        return g;
    g = getenv("B3_GAME_ROOT");
    if (g && *g)
        return g;
    /* NO RETAIL PATH IS COMPILED IN -- see docs/ASSETS.md. */
    return "";
}

/* --------------------------------------------------------------- unicode */

static int utf8_put(char *out, size_t outsz, size_t *o, uint32_t cp)
{
    unsigned char b[4];
    size_t k;

    if (cp < 0x80)          { b[0] = (unsigned char)cp; k = 1; }
    else if (cp < 0x800)    { b[0] = (unsigned char)(0xC0 | (cp >> 6));
                              b[1] = (unsigned char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000)  { b[0] = (unsigned char)(0xE0 | (cp >> 12));
                              b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                              b[2] = (unsigned char)(0x80 | (cp & 0x3F)); k = 3; }
    else                    { b[0] = (unsigned char)(0xF0 | (cp >> 18));
                              b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                              b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                              b[3] = (unsigned char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*o + k + 1 > outsz)
        return -1;
    memcpy(out + *o, b, k);
    *o += k;
    return 0;
}

int cxf_utf16le_to_utf8(const unsigned char *d, size_t n,
                        char *out, size_t outsz)
{
    size_t i = 0, o = 0;

    while (i + 1 < n) {
        uint32_t u = (uint32_t)d[i] | ((uint32_t)d[i + 1] << 8);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < n) {
                uint32_t lo = (uint32_t)d[i] | ((uint32_t)d[i + 1] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    i += 2;
                    u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                    if (utf8_put(out, outsz, &o, u) != 0) return -1;
                    continue;
                }
            }
            u = 0xFFFDu;               /* "replace" on a lone high surrogate */
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            u = 0xFFFDu;               /* lone low surrogate */
        }
        if (utf8_put(out, outsz, &o, u) != 0)
            return -1;
    }
    if (i < n) {                       /* trailing odd byte */
        if (utf8_put(out, outsz, &o, 0xFFFDu) != 0)
            return -1;
    }
    out[o] = '\0';
    return 0;
}

/* The code points python's str.isspace() reports for the BMP; str.strip()
 * removes exactly these from both ends. */
static int uni_space(uint32_t cp)
{
    if (cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x1F))
        return 1;
    switch (cp) {
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000:
        return 1;
    default:
        break;
    }
    return cp >= 0x2000 && cp <= 0x200A;
}

/* Decode one UTF-8 sequence at s[i]; returns its length (>=1) and the code
 * point.  Malformed input is treated as a 1-byte non-space character. */
static size_t utf8_get(const char *s, size_t i, size_t n, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[i];
    size_t k;
    uint32_t v;

    if (c < 0x80)            { *cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { k = 2; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { k = 3; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { k = 4; v = c & 0x07u; }
    else { *cp = c; return 1; }
    if (i + k > n) { *cp = c; return 1; }
    {
        size_t j;
        for (j = 1; j < k; j++) {
            unsigned char cc = (unsigned char)s[i + j];
            if ((cc & 0xC0) != 0x80) { *cp = c; return 1; }
            v = (v << 6) | (cc & 0x3Fu);
        }
    }
    *cp = v;
    return k;
}

void cxf_strip_unicode(char *s)
{
    size_t n = strlen(s), a = 0, b = n;

    for (;;) {
        uint32_t cp;
        size_t k;
        if (a >= b)
            break;
        k = utf8_get(s, a, n, &cp);
        if (!uni_space(cp))
            break;
        a += k;
    }
    while (b > a) {
        /* step back over one UTF-8 sequence */
        size_t st = b - 1;
        uint32_t cp;
        while (st > a && ((unsigned char)s[st] & 0xC0) == 0x80)
            st--;
        utf8_get(s, st, n, &cp);
        if (!uni_space(cp))
            break;
        b = st;
    }
    if (a)
        memmove(s, s + a, b - a);
    s[b - a] = '\0';
}

/* ---------------------------------------------------------- standalone CLI
 * Built only for the acceptance gate; the real driver is cx_main.c, which
 * this file must never collide with:
 *
 *   gcc -Wall -Wextra -std=c11 -O2 -DCX_AUDIO_STANDALONE \
 *       tools/cextract/cx_audio_*.c -o cxaudio -lm
 *   ./cxaudio {awd|rws|xwb|eatrax|all} <out_root> [game_dir]
 */
#ifdef CX_AUDIO_STANDALONE
#include "cx_extract.h"

/* NO RETAIL PATH IS COMPILED IN.  This tree ships no game content: point
 * $B3_GAME_ROOT at YOUR OWN dump -- the folder holding default.xbe, or an
 * .xiso image of it.  $B3_ISO and $B3_GAME_DIR name the same source and are
 * checked first.  See docs/ASSETS.md. */
#define CXF_DEFAULT_GAME ""

int main(int argc, char **argv)
{
    const char *what, *out, *game;
    int rc = 0;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s {awd|rws|xwb|eatrax|all} <out_root> [game_dir]\n",
                argv[0]);
        return 2;
    }
    what = argv[1];
    out  = argv[2];
    game = argc > 3 ? argv[3] : getenv("B3_GAME_DIR");
    if (!game || !*game)
        game = getenv("B3_GAME_ROOT");
    if (!game || !*game)
        game = CXF_DEFAULT_GAME;

    if (!strcmp(what, "awd") || !strcmp(what, "all"))
        rc |= cx_extract_awd(game, out);
    if (!strcmp(what, "rws") || !strcmp(what, "all"))
        rc |= cx_extract_rws(game, out);
    if (!strcmp(what, "xwb") || !strcmp(what, "all"))
        rc |= cx_extract_xwb(game, out);
    if (!strcmp(what, "eatrax") || !strcmp(what, "all"))
        rc |= cx_extract_eatrax(game, out);
    return rc;
}
#endif /* CX_AUDIO_STANDALONE */
