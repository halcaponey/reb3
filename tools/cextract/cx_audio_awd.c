/* cx_audio_awd.c -- port of tools/extract_awd.py (THE SPEC).
 *
 * RenderWare Audio Wave Dictionaries (chunk id 0x809) -> <out_root>/
 * awd_<dictname>/<wavename>.wav.  The layout below is copied verbatim from
 * the python docstring; every offset comes out of the files, nothing here is
 * per-bank knowledge.
 *
 *   0x00  u32  chunk id 0x809
 *   0x04  ptr
 *   0x08  u32  dataOffset   (0x800/0x1000/0x1800/0x2000; sector aligned)
 *   0x0C  u32  0x2C
 *   0x10  u32  0
 *   0x14  u32  dataSize     (dataOffset+dataSize == file size)
 *   0x18  u8[16] UUID       (constant 042d3a45-5fe4-4bc8-81f0-df758b01f273
 *                            in every file -- platform/format id)
 *   0x28  u32  dataOffset (again)
 *   0x2C  ptr
 *   0x30  u32  dictOffset   (0x5C in every file)
 *   0x38  u32  ?, 0x3C u32 ?, 0x44 u32 0x04000000, 0x48/0x4C ptr
 *
 *   dict @0x5C:
 *     char name[]           dictionary name, NUL terminated, (len+1) padded
 *                           to a multiple of 4
 *     ... then wave records, a serialized linked list.  Record k:
 *     u32  uuidOffset       absolute offset of this record's UUID
 *     u32  nameOffset       absolute offset of this record's name (0 == end)
 *     u32  0
 *     ptr  vtable           (0x100f2e60 in every file)
 *     fmt[0x1C]             u32 rate; u32 channels; u32 dataBytes;
 *                           u8 bitsPerSample; u8 fmtId(1=PCM);
 *                           u16+u8[10]+u16 junk/uninitialised
 *     fmt[0x1C]             identical second copy ("target" vs "source"
 *                           format; always equal in these files)
 *     u32  0
 *     u32  dataOffsetRel    wave payload offset relative to header dataOffset
 *     u32  (offset of the u32 just before the name -- serializer detail)
 *     u32  flags?           (4 or 6; meaning unknown)
 *     u32  0
 *     u32  prevLink, nextLink, ownLink   (list bookkeeping, unused here)
 *     u32  0
 *     char name[]           NUL terminated, (len+1) padded to 4
 *     u8   uuid[16]
 *     -> next record starts at uuidOffset+16
 *
 * The per-vehicle engine banks pveh/<CLASS>/Car*.hwd and Car*.lwd use the
 * exact same format and are picked up by the directory scan; because ALL of
 * them name their dictionary "high" / "low" they collide, and the python
 * qualifies a colliding directory with the source path -- reproduced here.
 */
#include "cx_audio_common.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_WAVEDICT 0x809u

typedef struct {
    char     name[256];
    char     uuid[33];
    uint32_t rate, channels, size, offset;
    uint8_t  bits, fmt_id;
} cxf_awd_wave;

typedef struct {
    uint32_t      data_off, data_size;
    char          dict[256];
    cxf_awd_wave *waves;
    size_t        nwaves;
} cxf_awd;

static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }

/* python parse_awd(); `err` receives the message that would have been the
 * ValueError/struct.error text.  0 = ok. */
static int cxf_awd_parse(const cxf_blob *d, cxf_awd *out, char *err,
                         size_t errsz)
{
    uint32_t id, off28, dict_off, p;
    size_t cap = 0;

    memset(out, 0, sizeof *out);
    if (d->n < 0x60 || cxf_u32(d, 0, &id) != 0 || id != CHUNK_WAVEDICT) {
        snprintf(err, errsz, "not an AWD (chunk id != 0x809)");
        return -1;
    }
    if (cxf_u32(d, 0x08, &out->data_off) != 0 ||
        cxf_u32(d, 0x14, &out->data_size) != 0 ||
        cxf_u32(d, 0x28, &off28) != 0 ||
        cxf_u32(d, 0x30, &dict_off) != 0) {
        snprintf(err, errsz, "unpack_from requires a buffer of at least "
                             "the header size");
        return -1;
    }
    if (off28 != out->data_off) {
        snprintf(err, errsz, "dataOffset fields at 0x08/0x28 disagree");
        return -1;
    }
    if ((uint64_t)out->data_off + out->data_size != (uint64_t)d->n) {
        snprintf(err, errsz, "dataOffset+dataSize != file size");
        return -1;
    }
    if (cxf_cstr(d, dict_off, out->dict, sizeof out->dict) != 0) {
        snprintf(err, errsz, "subsection not found");
        return -1;
    }
    p = dict_off + pad4((uint32_t)strlen(out->dict) + 1u);

    while ((uint64_t)p + 8u <= (uint64_t)out->data_off) {
        uint32_t uuid_off, name_off, q, zero, r2, c2, s2;
        uint8_t b2, f2;
        cxf_awd_wave w;
        size_t i;

        if (cxf_u32(d, p, &uuid_off) != 0 || cxf_u32(d, p + 4, &name_off) != 0) {
            snprintf(err, errsz, "unpack_from: buffer too small at 0x%x", p);
            return -1;
        }
        if (name_off == 0 || name_off >= out->data_off ||
            uuid_off >= out->data_off)
            break;
        q = p + 16;                          /* skip zero + vtable ptr */
        memset(&w, 0, sizeof w);
        if (cxf_u32(d, q, &w.rate) != 0 || cxf_u32(d, q + 4, &w.channels) != 0 ||
            cxf_u32(d, q + 8, &w.size) != 0 ||
            cxf_u8(d, q + 12, &w.bits) != 0 || cxf_u8(d, q + 13, &w.fmt_id) != 0 ||
            cxf_u32(d, q + 0x1C, &r2) != 0 || cxf_u32(d, q + 0x20, &c2) != 0 ||
            cxf_u32(d, q + 0x24, &s2) != 0 ||
            cxf_u8(d, q + 0x28, &b2) != 0 || cxf_u8(d, q + 0x29, &f2) != 0 ||
            cxf_u32(d, q + 0x38, &zero) != 0 ||
            cxf_u32(d, q + 0x3C, &w.offset) != 0) {
            snprintf(err, errsz, "unpack_from: buffer too small at 0x%x", q);
            return -1;
        }
        if (r2 != w.rate || c2 != w.channels || s2 != w.size ||
            b2 != w.bits || f2 != w.fmt_id) {
            snprintf(err, errsz, "format copies disagree at 0x%x", q);
            free(out->waves);
            out->waves = NULL;
            return -1;
        }
        if (zero != 0) {
            snprintf(err, errsz, "expected 0 after formats at 0x%x", q + 0x38);
            free(out->waves);
            out->waves = NULL;
            return -1;
        }
        if (cxf_cstr(d, name_off, w.name, sizeof w.name) != 0) {
            snprintf(err, errsz, "subsection not found");
            free(out->waves);
            out->waves = NULL;
            return -1;
        }
        /* python: data[uuid_off:uuid_off+16].hex() -- slicing clamps. */
        for (i = 0; i < 16; i++) {
            size_t k = (size_t)uuid_off + i;
            unsigned char v = k < d->n ? d->d[k] : 0;
            static const char HEX[] = "0123456789abcdef";
            w.uuid[2 * i] = HEX[v >> 4];
            w.uuid[2 * i + 1] = HEX[v & 0xF];
        }
        w.uuid[32] = '\0';

        if (out->nwaves == cap) {
            cxf_awd_wave *nv;
            cap = cap ? cap * 2 : 32;
            nv = (cxf_awd_wave *)realloc(out->waves, cap * sizeof *nv);
            if (!nv) {
                snprintf(err, errsz, "out of memory");
                free(out->waves);
                out->waves = NULL;
                return -1;
            }
            out->waves = nv;
        }
        out->waves[out->nwaves++] = w;
        p = uuid_off + 16u;
    }
    return 0;
}

static int cxf_awd_extract(const char *path, const cxf_blob *d,
                           const cxf_awd *a, const char *out_root,
                           const char *dir_name, int *n_ok, int *n_fail)
{
    char out_dir[3072];
    size_t i;

    snprintf(out_dir, sizeof out_dir, "%s/%s", out_root, dir_name);
    if (cxf_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "[cx_audio_awd] cannot create %s\n", out_dir);
        return -1;
    }

    printf("\n== %s  (dict '%s', %zu waves, data @0x%x+0x%x) ==\n",
           path, a->dict, a->nwaves, a->data_off, a->data_size);
    printf("%-4s %-24s %-6s %-6s %-3s %8s %8s  %s\n",
           "idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status");

    for (i = 0; i < a->nwaves; i++) {
        const cxf_awd_wave *w = &a->waves[i];
        char status[256];
        char codec[16], sdur[32], srms[32];
        const unsigned char *pcm = NULL;
        size_t plen = 0, frame;
        double dur = 0.0, rms = 0.0;
        int have_dur = 0, have_rms = 0, nonconst = 0;

        status[0] = '\0';
#define ADD(s) do { if (status[0]) strncat(status, " ", sizeof status - \
                        strlen(status) - 1); \
                    strncat(status, (s), sizeof status - strlen(status) - 1); \
               } while (0)

        if (w->fmt_id != 1 || w->bits != 16) {
            char t[64];
            snprintf(t, sizeof t, "UNEXPECTED-FMT(id=%d,bits=%d)",
                     (int)w->fmt_id, (int)w->bits);
            ADD(t);
        }
        if (!(w->rate >= 2000 && w->rate <= 48000))
            ADD("BAD-RATE");
        if (!(w->channels == 1 || w->channels == 2 || w->channels == 4 ||
              w->channels == 6))
            ADD("BAD-CH");
        if ((uint64_t)w->offset + w->size > (uint64_t)a->data_size)
            ADD("OUT-OF-RANGE");

        if (!status[0]) {
            size_t base = a->data_off;
            size_t s = base + w->offset;
            size_t e = s + w->size;
            if (s > d->n) s = d->n;
            if (e > d->n) e = d->n;
            pcm = d->d + s;
            plen = e - s;
            if (plen != w->size)
                ADD("TRUNCATED");
            frame = 2u * (size_t)w->channels;
            plen -= plen % frame;
            dur = (double)plen / ((double)frame * (double)w->rate);
            have_dur = 1;
            cxf_pcm16_stats(pcm, plen, &rms, &nonconst);
            have_rms = 1;
            if (rms < 1.0 || !nonconst)
                ADD("SILENT");
        }
        if (!status[0]) {
            char wp[3400];
            snprintf(wp, sizeof wp, "%s/%s.wav", out_dir, w->name);
            if (cxf_write_wav(wp, pcm, plen, w->rate, w->channels, 2) != 0) {
                fprintf(stderr, "[cx_audio_awd] cannot write %s\n", wp);
                return -1;
            }
            (*n_ok)++;
        } else {
            (*n_fail)++;
        }
        snprintf(codec, sizeof codec, "pcm%d", (int)w->bits);
        if (have_dur) snprintf(sdur, sizeof sdur, "%.2f", dur);
        else          snprintf(sdur, sizeof sdur, "-");
        if (have_rms) snprintf(srms, sizeof srms, "%.0f", rms);
        else          snprintf(srms, sizeof srms, "-");
        printf("%-4zu %-24s %-6s %-6u %-3u %8s %8s  %s\n",
               i, w->name, codec, w->rate, w->channels, sdur, srms,
               status[0] ? status : "OK");
#undef ADD
    }
    printf("-- %s: %d ok, %d failed\n", a->dict, *n_ok, *n_fail);
    return 0;
}

int cx_extract_awd(const char *game_dir, const char *out_root)
{
    game_dir = cxf_game_dir(game_dir);
    static const char *const EXTS[] = { ".awd", ".hwd", ".lwd" };
    cxf_strlist files = { NULL, 0, 0 };
    cxf_blob *blobs = NULL;
    cxf_awd *infos = NULL;
    char (*errs)[256] = NULL;
    int *ok_idx = NULL;                /* 1 = parsed in pass 1 (never edited
                                        * afterwards: it is what python's
                                        * dict_count is built from) */
    int *failed = NULL;                /* pass-2 error, python's `errors` */
    size_t i, j, nparsed = 0;
    int tot_ok = 0, tot_fail = 0, n_err = 0, rc = 1;

    if (cxf_walk_ext(game_dir, EXTS, 3, &files) != 0) {
        fprintf(stderr, "[cx_audio_awd] cannot scan %s\n", game_dir);
        goto done;
    }
    cxf_strlist_sort(&files);
    if (files.n == 0) {
        printf("\nTOTAL: 0 waves ok, 0 failed, 0 dictionaries, 0 errors\n");
        rc = 0;
        goto done;
    }

    blobs = (cxf_blob *)calloc(files.n, sizeof *blobs);
    infos = (cxf_awd *)calloc(files.n, sizeof *infos);
    errs  = (char (*)[256])calloc(files.n, sizeof *errs);
    ok_idx = (int *)calloc(files.n, sizeof *ok_idx);
    failed = (int *)calloc(files.n, sizeof *failed);
    if (!blobs || !infos || !errs || !ok_idx || !failed)
        goto done;

    /* pass 1: parse everything, count dictionary-name collisions */
    for (i = 0; i < files.n; i++) {
        if (cxf_read_file(files.v[i], &blobs[i]) != 0) {
            snprintf(errs[i], sizeof errs[i], "cannot read file");
            continue;
        }
        if (cxf_awd_parse(&blobs[i], &infos[i], errs[i], sizeof errs[i]) != 0) {
            cxf_blob_free(&blobs[i]);
            continue;
        }
        ok_idx[i] = 1;
        nparsed++;
    }

    /* pass 2: extract; a colliding dictionary name is qualified by path */
    for (i = 0; i < files.n; i++) {
        char dir_name[2048];
        int ndup = 0, n_ok = 0, n_fail = 0;

        if (!ok_idx[i])
            continue;
        for (j = 0; j < files.n; j++)
            if (ok_idx[j] && !strcmp(infos[j].dict, infos[i].dict))
                ndup++;
        if (ndup > 1) {
            char tag[1536];
            if (cxf_path_tag(files.v[i], game_dir, tag, sizeof tag) != 0) {
                snprintf(errs[i], sizeof errs[i], "path too long");
                failed[i] = 1;
                tot_fail += 1;
                continue;
            }
            snprintf(dir_name, sizeof dir_name, "awd_%s_%s", tag, infos[i].dict);
        } else {
            snprintf(dir_name, sizeof dir_name, "awd_%s", infos[i].dict);
        }
        if (cxf_awd_extract(files.v[i], &blobs[i], &infos[i], out_root,
                            dir_name, &n_ok, &n_fail) != 0) {
            snprintf(errs[i], sizeof errs[i], "write failed");
            failed[i] = 1;
            n_ok = 0;
            n_fail = 1;
        }
        tot_ok += n_ok;
        tot_fail += n_fail;
    }

    for (i = 0; i < files.n; i++)
        if (!ok_idx[i] || failed[i]) {
            printf("\n== %s == ERROR: %s\n", files.v[i], errs[i]);
            n_err++;
        }
    printf("\nTOTAL: %d waves ok, %d failed, %zu dictionaries, %d errors\n",
           tot_ok, tot_fail, nparsed, n_err);
    /* python: `return 1 if (tot_fail or errors) else 0` -- see cxf_rc(). */
    rc = cxf_rc(n_err, tot_fail);

done:
    if (blobs)
        for (i = 0; i < files.n; i++)
            cxf_blob_free(&blobs[i]);
    if (infos)
        for (i = 0; i < files.n; i++)
            free(infos[i].waves);
    free(blobs);
    free(infos);
    free(errs);
    free(ok_idx);
    free(failed);
    cxf_strlist_free(&files);
    return rc;
}
