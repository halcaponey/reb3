/* cx_audio_rws.c -- port of tools/extract_rws.py (THE SPEC).
 *
 * RenderWare Audio streams (.rws) -> <out_root>/rws_<relpath>/<name>.wav.
 * Chunk layout (RW chunk header = u32 id, u32 size, u32 version), copied from
 * the python docstring:
 *
 *   0x80D  AudioStream container (spans whole file)
 *     0x80E  stream header (size 0x7DC in every B3 file), body at 0x18:
 *       +0x00 u32  used-header-bytes (approx)
 *       +0x04 u32 0x14, +0x08 u32 0x10, +0x0C u32 0x24, +0x10 u32 ?
 *       +0x14..+0x1F ptr/0
 *       +0x20 u32  numSegments      (1 in every B3 file)
 *       +0x24 ptr
 *       +0x28 u32  numStreams       (1 or 2)
 *       +0x2C ptr
 *       +0x30 u32  dataOffset       (0x800)
 *       +0x34 u32  clusterStride    (numStreams * 0x10000)
 *       +0x38 u32  sectorAlign      (0x800)
 *       +0x3C u32  0
 *       +0x40 u8[16] stream file UUID
 *       +0x50 char[16] stream file name (e.g. "AS_C1_V1.cr1")
 *       +0x60 6 x u32 ptr/0
 *       +0x78 u32  totalDataBytes   (dataOffset+totalDataBytes == file size,
 *                                    == 0x80F chunk size)
 *       +0x7C u32  0
 *       +0x80 u32  usedBytes[numStreams]   per-stream payload byte counts
 *       then segment record: u8[16] uuid, char[12] name ("Segment0"), u32 ?
 *       then per stream, 0x28 bytes:
 *            ptr, ptr, 0, u32 1, u32 clusterSize(0x10000), ptr,
 *            u16 ?, u16 ?, u32 blockSize(0x4000), u32 clusterSize again,
 *            u32 clusterOffset  (this stream's slice offset inside a cluster
 *                                stride: 0, 0x10000, ...)
 *       then per stream, 0x30 bytes (wave format):
 *            u32 rate; ptr; u32 usedBytes; u8 bitsPerSample; u8 channels;
 *            u16 0; u8[12] 0; u8[16] format-class UUID; char[4] short name
 *       then per stream u8[16] UUID, then per stream char[16] stream name
 *            (e.g. "aGenCrash01", "AS_C1_V11") -- used as output names.
 *     0x80F  raw sample data at dataOffset (0x800)
 *
 * Sample data: 16-bit little-endian PCM, channels interleaved per frame.
 * The file is divided into clusters of clusterStride bytes; stream i owns the
 * 0x10000-byte slice at clusterOffset[i] of every cluster.
 *
 * Note: the 36 MUSIC.RWS files are 124-byte dummies (a 0x809 wave dictionary
 * named "dummy sound bank" with zero waves); they are detected and skipped,
 * and -- because the skip happens BEFORE os.makedirs -- they leave no output
 * directory behind at all.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "cx_audio_common.h"
#include "cx_extract.h"
#include "cx_src.h"   /* the dump may be a directory OR an ISO */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define CHUNK_STREAM        0x80Du
#define CHUNK_STREAM_HEADER 0x80Eu
#define CHUNK_STREAM_DATA   0x80Fu
#define CHUNK_WAVEDICT      0x809u

#define RWS_MAX_STREAMS 8

typedef struct {
    char     name[32];
    char     uuid[33];
    uint32_t rate, used, cluster_off, cluster_size, block_size;
    uint8_t  bits, channels;
} cxf_rws_stream;

typedef struct {
    char           file_name[32], seg_name[32];
    uint32_t       n_streams, data_off, stride, total;
    cxf_rws_stream streams[RWS_MAX_STREAMS];
} cxf_rws;

/* python parse_rws().  Returns 1 for the dummy sound bank (python's None),
 * 0 on success, -1 with `err` filled in for a would-be exception. */
static int cxf_rws_parse(const char *path, cxf_rws *out, char *err,
                         size_t errsz)
{
    cxf_blob h;
    uint32_t cid, hid, hsz, did, dsz, n_seg;
    uint32_t used[RWS_MAX_STREAMS];
    size_t b = 0x18, p;
    uint32_t i;
    long long fsz;
    int rc = -1;

    memset(out, 0, sizeof *out);
    if (cxf_read_head(path, 0x800, &h) != 0) {
        snprintf(err, errsz, "cannot read file");
        return -1;
    }
    /* python: struct.unpack_from("<III", head, 0) -- the WHOLE 12-byte RW
     * chunk header must be present before the id is even looked at, so a
     * file shorter than that is an error and NOT a dummy sound bank. */
    if (h.n < 12 || cxf_u32(&h, 0, &cid) != 0) {
        snprintf(err, errsz, "unpack_from requires a buffer of at least 12 bytes");
        goto out;
    }
    if (cid == CHUNK_WAVEDICT) { rc = 1; goto out; }   /* dummy MUSIC.RWS */
    if (cid != CHUNK_STREAM) {
        snprintf(err, errsz, "not an RWS audio stream (chunk 0x%x)", cid);
        goto out;
    }
    if (h.n < 0xC + 12 ||
        cxf_u32(&h, 0xC, &hid) != 0 || cxf_u32(&h, 0x10, &hsz) != 0) {
        snprintf(err, errsz, "unpack_from requires a buffer of at least 12 bytes");
        goto out;
    }
    if (hid != CHUNK_STREAM_HEADER) {
        snprintf(err, errsz, "missing 0x80E stream header");
        goto out;
    }
    if (cxf_u32(&h, b + 0x20, &n_seg) != 0 ||
        cxf_u32(&h, b + 0x28, &out->n_streams) != 0 ||
        cxf_u32(&h, b + 0x30, &out->data_off) != 0 ||
        cxf_u32(&h, b + 0x34, &out->stride) != 0) {
        snprintf(err, errsz, "unpack_from: header truncated");
        goto out;
    }
    if (n_seg != 1) {
        snprintf(err, errsz,
                 "numSegments=%u not supported (all B3 files have 1)", n_seg);
        goto out;
    }
    if (!(out->n_streams >= 1 && out->n_streams <= RWS_MAX_STREAMS)) {
        snprintf(err, errsz, "implausible numStreams=%u", out->n_streams);
        goto out;
    }
    if (cxf_field(&h, b + 0x50, 0x10, out->file_name, sizeof out->file_name) != 0 ||
        cxf_u32(&h, b + 0x78, &out->total) != 0) {
        snprintf(err, errsz, "unpack_from: header truncated");
        goto out;
    }
    for (i = 0; i < out->n_streams; i++)
        if (cxf_u32(&h, b + 0x80 + 4u * i, &used[i]) != 0) {
            snprintf(err, errsz, "unpack_from: header truncated");
            goto out;
        }
    p = b + 0x80 + 4u * out->n_streams;
    if (cxf_field(&h, p + 16, 12, out->seg_name, sizeof out->seg_name) != 0) {
        snprintf(err, errsz, "segment name too long");
        goto out;
    }
    p += 32;                           /* uuid[16] + name[12] + u32 */
    for (i = 0; i < out->n_streams; i++) {
        cxf_rws_stream *s = &out->streams[i];
        if (cxf_u32(&h, p + 0x10, &s->cluster_size) != 0 ||
            cxf_u32(&h, p + 0x1C, &s->block_size) != 0 ||
            cxf_u32(&h, p + 0x24, &s->cluster_off) != 0) {
            snprintf(err, errsz, "unpack_from: header truncated");
            goto out;
        }
        p += 0x28;
    }
    for (i = 0; i < out->n_streams; i++) {
        cxf_rws_stream *s = &out->streams[i];
        if (cxf_u32(&h, p, &s->rate) != 0 || cxf_u32(&h, p + 8, &s->used) != 0 ||
            cxf_u8(&h, p + 12, &s->bits) != 0 ||
            cxf_u8(&h, p + 13, &s->channels) != 0) {
            snprintf(err, errsz, "unpack_from: header truncated");
            goto out;
        }
        if (s->used != used[i]) {
            snprintf(err, errsz, "per-stream byte counts disagree "
                                 "(0x%x vs 0x%x)", s->used, used[i]);
            goto out;
        }
        p += 0x30;
    }
    for (i = 0; i < out->n_streams; i++) {
        size_t k;
        for (k = 0; k < 16; k++) {
            static const char HEX[] = "0123456789abcdef";
            unsigned char v = (p + k) < h.n ? h.d[p + k] : 0;
            out->streams[i].uuid[2 * k] = HEX[v >> 4];
            out->streams[i].uuid[2 * k + 1] = HEX[v & 0xF];
        }
        out->streams[i].uuid[32] = '\0';
        p += 16;
    }
    for (i = 0; i < out->n_streams; i++) {
        if (cxf_field(&h, p, 16, out->streams[i].name,
                      sizeof out->streams[i].name) != 0) {
            snprintf(err, errsz, "stream name too long");
            goto out;
        }
        cxf_strip_ascii(out->streams[i].name);
        p += 16;
    }

    if ((uint64_t)0x18 + hsz + 12u > (uint64_t)h.n ||
        cxf_u32(&h, 0x18 + hsz, &did) != 0 ||
        cxf_u32(&h, 0x18 + hsz + 4, &dsz) != 0) {
        snprintf(err, errsz, "unpack_from: data chunk header past the "
                             "first 0x800 bytes");
        goto out;
    }
    if (did != CHUNK_STREAM_DATA) {
        snprintf(err, errsz, "missing 0x80F data chunk");
        goto out;
    }
    if (dsz != out->total) {
        snprintf(err, errsz, "0x80F size 0x%x != totalDataBytes 0x%x",
                 dsz, out->total);
        goto out;
    }
    fsz = cxf_file_size(path);
    if (fsz < 0 || (long long)out->data_off + (long long)out->total != fsz) {
        snprintf(err, errsz, "dataOffset+totalDataBytes != file size");
        goto out;
    }
    rc = 0;

out:
    cxf_blob_free(&h);
    return rc;
}

/* python deinterleave(): collect one stream's payload from its cluster
 * slices.  A short read at EOF is passed through so the caller can raise
 * SHORT-READ exactly where the python does. */
static unsigned char *cxf_rws_deinterleave(const char *path, const cxf_rws *in,
                                           const cxf_rws_stream *s, size_t *got)
{
    FILE *f;
    unsigned char *buf;
    size_t remaining = s->used, filled = 0;
    long long pos;

    *got = 0;
    buf = (unsigned char *)malloc(remaining ? remaining : 1u);
    if (!buf)
        return NULL;
    f = cx_vfs_fopen(path, "rb");
    if (!f) { free(buf); return NULL; }
    pos = (long long)in->data_off + (long long)s->cluster_off;
    while (remaining > 0) {
        size_t take = s->cluster_size < remaining ? s->cluster_size : remaining;
        size_t n;
        if (fseeko(f, (off_t)pos, SEEK_SET) != 0)
            break;
        n = fread(buf + filled, 1, take, f);
        filled += n;
        if (n < take)                  /* f.read() hit EOF: python appends the
                                        * short block and keeps looping, but
                                        * every further seek is past EOF and
                                        * reads b"" -- same total. */
            break;
        remaining -= take;
        pos += (long long)in->stride;
    }
    fclose(f);
    *got = filled;
    return buf;
}

static int cxf_rws_extract(const char *path, const char *out_root,
                           const char *tag, int *n_ok, int *n_fail,
                           int *dummy, char *err, size_t errsz)
{
    cxf_rws in;
    char out_dir[3072];
    char used_names[RWS_MAX_STREAMS][96];
    size_t n_used = 0;
    uint32_t i;
    int pr;

    *n_ok = *n_fail = *dummy = 0;
    pr = cxf_rws_parse(path, &in, err, errsz);
    if (pr < 0)
        return -1;
    if (pr == 1) {
        printf("== %s == dummy 'sound bank' stub, no audio (skipped)\n", path);
        *dummy = 1;
        return 0;
    }

    snprintf(out_dir, sizeof out_dir, "%s/rws_%s", out_root, tag);
    if (cxf_mkdir_p(out_dir) != 0) {
        snprintf(err, errsz, "cannot create %s", out_dir);
        return -1;
    }
    printf("\n== %s  ('%s', segment '%s', %u stream(s)) ==\n",
           path, in.file_name, in.seg_name, in.n_streams);
    printf("%-4s %-16s %-6s %-6s %-3s %8s %8s  %s\n",
           "idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status");

    for (i = 0; i < in.n_streams; i++) {
        const cxf_rws_stream *s = &in.streams[i];
        char status[256], codec[16], sdur[32], srms[32];
        unsigned char *pcm = NULL;
        size_t plen = 0, frame;
        double dur = 0.0, rms = 0.0;
        int have_dur = 0, have_rms = 0, nonconst = 0;

        status[0] = '\0';
#define ADD(s_) do { if (status[0]) strncat(status, " ", \
                         sizeof status - strlen(status) - 1); \
                     strncat(status, (s_), sizeof status - strlen(status) - 1); \
                } while (0)
        if (s->bits != 16) {
            char t[48];
            snprintf(t, sizeof t, "UNEXPECTED-BITS(%d)", (int)s->bits);
            ADD(t);
        }
        if (!(s->rate >= 8000 && s->rate <= 48000))
            ADD("BAD-RATE");
        if (!(s->channels == 1 || s->channels == 2 || s->channels == 4 ||
              s->channels == 6))
            ADD("BAD-CH");

        if (!status[0]) {
            pcm = cxf_rws_deinterleave(path, &in, s, &plen);
            if (!pcm) {
                snprintf(err, errsz, "out of memory / cannot reopen %s", path);
                return -1;
            }
            if (plen != s->used)
                ADD("SHORT-READ");
            frame = 2u * (size_t)s->channels;
            plen -= plen % frame;
            dur = (double)plen / ((double)frame * (double)s->rate);
            have_dur = 1;
            cxf_pcm16_stats(pcm, plen, &rms, &nonconst);
            have_rms = 1;
            if (rms < 1.0 || !nonconst)
                ADD("SILENT");
        }
        if (!status[0]) {
            char name[96], wp[3400];
            size_t k;
            if (s->name[0])
                snprintf(name, sizeof name, "%s", s->name);
            else
                snprintf(name, sizeof name, "stream%u", i);
            for (k = 0; k < n_used; k++)
                if (!strcmp(used_names[k], name)) {
                    /* python: name = "%s_%d" % (name, i) */
                    size_t l = strlen(name);
                    snprintf(name + l, sizeof name - l, "_%u", i);
                    break;
                }
            if (n_used < RWS_MAX_STREAMS)
                snprintf(used_names[n_used++], sizeof used_names[0], "%s", name);
            snprintf(wp, sizeof wp, "%s/%s.wav", out_dir, name);
            if (cxf_write_wav(wp, pcm, plen, s->rate, s->channels, 2) != 0) {
                free(pcm);
                snprintf(err, errsz, "cannot write %s", wp);
                return -1;
            }
            (*n_ok)++;
        } else {
            (*n_fail)++;
        }
        free(pcm);
        snprintf(codec, sizeof codec, "pcm%d", (int)s->bits);
        if (have_dur) snprintf(sdur, sizeof sdur, "%.2f", dur);
        else          snprintf(sdur, sizeof sdur, "-");
        if (have_rms) snprintf(srms, sizeof srms, "%.0f", rms);
        else          snprintf(srms, sizeof srms, "-");
        printf("%-4u %-16s %-6s %-6u %-3u %8s %8s  %s\n",
               i, s->name, codec, s->rate, s->channels, sdur, srms,
               status[0] ? status : "OK");
#undef ADD
    }
    printf("-- %s: %d ok, %d failed\n", tag, *n_ok, *n_fail);
    return 0;
}

/* python tag_for(): path relative to the root, extension stripped, a leading
 * "Tracks/" or "tracks/" removed, separators flattened to underscores. */
static int cxf_rws_tag(const char *path, const char *root, char *out,
                       size_t outsz)
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
    if (!strncmp(rel, "Tracks/", 7))
        memmove(rel, rel + 7, strlen(rel + 7) + 1);
    else if (!strncmp(rel, "tracks/", 7))
        memmove(rel, rel + 7, strlen(rel + 7) + 1);
    if (strlen(rel) >= outsz)
        return -1;
    strcpy(out, rel);
    for (i = 0; out[i]; i++)
        if (out[i] == '/')
            out[i] = '_';
    return 0;
}

int cx_extract_rws(const char *game_dir, const char *out_root)
{
    game_dir = cxf_game_dir(game_dir);
    static const char *const EXTS[] = { ".rws" };
    cxf_strlist files = { NULL, 0, 0 };
    size_t i;
    int tot_ok = 0, tot_fail = 0, n_dummy = 0, n_err = 0;

    if (cxf_walk_ext(game_dir, EXTS, 1, &files) != 0) {
        fprintf(stderr, "[cx_audio_rws] cannot scan %s\n", game_dir);
        cxf_strlist_free(&files);
        return 1;
    }
    cxf_strlist_sort(&files);

    for (i = 0; i < files.n; i++) {
        char tag[1536], err[4608];
        int ok = 0, fail = 0, dummy = 0;

        err[0] = '\0';
        if (cxf_rws_tag(files.v[i], game_dir, tag, sizeof tag) != 0) {
            printf("\n== %s == ERROR: path too long\n", files.v[i]);
            tot_fail += 1;
            n_err++;
            continue;
        }
        if (cxf_rws_extract(files.v[i], out_root, tag, &ok, &fail, &dummy,
                            err, sizeof err) != 0) {
            printf("\n== %s == ERROR: %s\n", files.v[i], err);
            ok = 0;
            fail = 1;
            dummy = 0;
            n_err++;
        }
        tot_ok += ok;
        tot_fail += fail;
        n_dummy += dummy;
    }
    printf("\nTOTAL: %d streams ok, %d failed, %zu files (%d dummy stubs)\n",
           tot_ok, tot_fail, files.n, n_dummy);
    cxf_strlist_free(&files);
    /* python: `return 1 if tot_fail else 0` -- see cxf_rc(). */
    return cxf_rc(n_err, tot_fail);
}
