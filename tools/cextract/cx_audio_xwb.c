/* cx_audio_xwb.c -- port of tools/extract_xwb.py (THE SPEC).
 *
 * Microsoft XACT wave banks (XWB version 3, XACT1 era) -> <out_root>/
 * <bankName>/NNN.wma + NNN.wav.
 *
 * THE .wav USED TO NEED ffmpeg ON PATH.  It does not: the WMA is decoded in
 * process by tools/cextract/wma/b3_wma.c, which is why this file is no longer
 * in the Makefile's CX_SKIP and is linked into the game like every other
 * stage.  CX_NO_FFMPEG still means "dump the .wma and skip the PCM check";
 * B3_FFMPEG=1 still runs the subprocess, as the A/B oracle.
 *
 * Measured over all 885 entries in all 33 banks: every one is wmav2 (0x0161),
 * 2 ch, 160 kb/s CBR -- 44100 Hz for EA TRAX and the Movie bank, 48000 Hz for
 * the DJ/crash-FM and per-track banks.  See tools/fetch_wma.sh.
 *
 *   0x00  char[4]  "WBND"
 *   0x04  u32      version (3 in all B3 files)
 *   0x08  4 x {u32 offset, u32 length}   segment table:
 *                    seg0 BANKDATA      (0x28, len 0x28)
 *                    seg1 ENTRYMETADATA (0x50, len 24*n)
 *                    seg2 ENTRYNAMES    (0,0 in every B3 bank -- no names)
 *                    seg3 ENTRYWAVEDATA (0x800, ...)
 *
 *   BANKDATA:  u32 flags(=1 streaming), u32 entryCount, char[16] bankName,
 *              u32 entryMetaElemSize(=24), u32 entryNameElemSize(=64),
 *              u32 alignment(=0x800), u32 pad/compact(=0)
 *
 *   ENTRY (24 bytes): u32 flagsAndDuration(=0), u32 format,
 *              u32 playOffset (relative to seg3), u32 playLength,
 *              u32 loopStart, u32 loopLength
 *
 *   format dword bit-packing (WAVEBANKMINIWAVEFORMAT, XACT1 layout):
 *              tag      = bits 0-1    0=PCM, 1=Xbox ADPCM, 2=WMA
 *              channels = bits 2-4
 *              rate     = bits 5-22
 *              align    = bits 23-30  (0 for WMA entries)
 *              bits16   = bit  31     (1 = 16-bit for PCM)
 *
 * Every entry in every Burnout 3 bank is tag==2 (WMA): the play region is a
 * complete standalone ASF file, dumped verbatim as NNN.wma and handed to
 * ffmpeg for the NNN.wav.  The PCM and Xbox-ADPCM paths are ported for
 * completeness and are *unexercised* by this game's data -- same caveat the
 * python carries.
 *
 * QUIRK, faithfully reproduced: the output directory is the BANK NAME with no
 * prefix, taken straight out of BANKDATA, so two banks that shared a name
 * would share a directory.  (They do not in this game: all 33 names differ,
 * including the twelve per-track E_DJRACE.xwb, which name themselves
 * "US_C1", "AS_M1", ... rather than "DJRACE".)
 */
#define _POSIX_C_SOURCE 200809L

#include "cx_audio_common.h"
#include "cx_extract.h"
#include "b3_wma.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_PCM         0
#define TAG_XBOX_ADPCM  1
#define TAG_WMA         2

static const unsigned char ASF_MAGIC[16] = {
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};
/* ASF File Properties Object GUID (for duration parsing) */
static const unsigned char ASF_FILE_PROPS[16] = {
    0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
    0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};

static const int IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};
static const int IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767
};

/* Decode Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x0069, an IMA-ADPCM variant with
 * a 36-byte-per-channel block: 4-byte header {s16 predictor, u8 stepIdx, u8
 * pad} per channel, then 4-byte nibble groups round-robin per channel).
 * Returns malloc'd interleaved s16le bytes; *outn is the byte count.
 * NOTE: no Burnout 3 file uses this codec; this path is untested against real
 * game data and follows the standard IMA-WAV/Xbox layout. */
static unsigned char *cxf_decode_xbox_adpcm(const unsigned char *data,
                                            size_t n, unsigned ch,
                                            size_t *outn)
{
    size_t block, boff, cap, cnt = 0;
    int16_t *out;
    int *chans = NULL;

    *outn = 0;
    if (ch == 0 || ch > 8)
        return NULL;
    block = 36u * ch;
    if (n < block) {
        out = (int16_t *)malloc(2);
        return (unsigned char *)out;   /* empty, as python's b"" */
    }
    /* 65 samples per channel per block (1 header + 64 nibbles) */
    cap = (n / block) * 65u * ch;
    out = (int16_t *)malloc(cap ? cap * sizeof *out : 2u);
    chans = (int *)malloc(64u * ch * sizeof *chans);
    if (!out || !chans) { free(out); free(chans); return NULL; }

    for (boff = 0; boff + block <= n; boff += block) {
        int preds[8], idxs[8];
        unsigned c;
        size_t pos, nper = 0;

        for (c = 0; c < ch; c++) {
            int16_t pred = (int16_t)((uint16_t)data[boff + 4 * c] |
                                     ((uint16_t)data[boff + 4 * c + 1] << 8));
            int idx = data[boff + 4 * c + 2];
            preds[c] = pred;
            idxs[c] = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
            out[cnt++] = pred;         /* header predictor is the 1st sample */
        }
        pos = boff + 4u * ch;
        while (pos < boff + block) {
            for (c = 0; c < ch; c++) {
                int k;
                for (k = 0; k < 4; k++) {
                    unsigned char bb = data[pos + (size_t)k];
                    int half;
                    for (half = 0; half < 2; half++) {
                        int nib = half ? (bb >> 4) : (bb & 0xF);
                        int step = IMA_STEP_TABLE[idxs[c]];
                        int diff = step >> 3;
                        int v;
                        if (nib & 1) diff += step >> 2;
                        if (nib & 2) diff += step >> 1;
                        if (nib & 4) diff += step;
                        if (nib & 8) diff = -diff;
                        v = preds[c] + diff;
                        if (v < -32768) v = -32768;
                        if (v > 32767) v = 32767;
                        preds[c] = v;
                        idxs[c] += IMA_INDEX_TABLE[nib];
                        if (idxs[c] < 0) idxs[c] = 0;
                        if (idxs[c] > 88) idxs[c] = 88;
                        chans[c * 64u + (size_t)(nper + (size_t)(k * 2 + half))] = v;
                    }
                }
                pos += 4;
            }
            nper += 8;
        }
        /* interleave the 64 nibble samples per channel */
        {
            size_t i2;
            for (i2 = 0; i2 < nper; i2++)
                for (c = 0; c < ch; c++)
                    out[cnt++] = (int16_t)chans[c * 64u + i2];
        }
    }
    free(chans);
    *outn = cnt * 2u;
    {   /* pack to little-endian bytes in place */
        unsigned char *b = (unsigned char *)out;
        size_t i2;
        for (i2 = 0; i2 < cnt; i2++) {
            uint16_t v = (uint16_t)out[i2];
            b[2 * i2] = (unsigned char)(v & 0xFF);
            b[2 * i2 + 1] = (unsigned char)(v >> 8);
        }
    }
    return (unsigned char *)out;
}

/* Parse the ASF File Properties Object -> playback duration in seconds
 * (play duration minus preroll).  Returns 0 on success. */
static int cxf_asf_duration(const cxf_blob *b, double *dur)
{
    uint64_t hdr_size;
    uint32_t nobj, k;
    size_t pos = 30;

    if (b->n < 16 || memcmp(b->d, ASF_MAGIC, 16))
        return -1;
    if (cxf_u64(b, 16, &hdr_size) != 0 || cxf_u32(b, 24, &nobj) != 0)
        return -1;
    for (k = 0; k < nobj; k++) {
        uint64_t osize;
        if (pos + 24 > b->n || (uint64_t)pos + 24u > hdr_size + 30u)
            break;
        if (cxf_u64(b, pos + 16, &osize) != 0)
            break;
        if (osize < 24)
            break;
        if (!memcmp(b->d + pos, ASF_FILE_PROPS, 16)) {
            uint64_t play, preroll;
            double v;
            if (cxf_u64(b, pos + 64, &play) != 0 ||
                cxf_u64(b, pos + 80, &preroll) != 0)
                return -1;
            v = (double)play / 1e7 - (double)preroll / 1e3;
            *dur = v > 0.0 ? v : 0.0;
            return 0;
        }
        pos += (size_t)osize;
    }
    return -1;
}

typedef struct {
    uint32_t index, tag, channels, rate, align, bits16;
    uint32_t offset, length, loop_start, loop_len;
    char     name[80];
    int      have_name;
} cxf_xwb_entry;

typedef struct {
    uint32_t       version, flags, alignment;
    char           bank_name[24];
    uint32_t       wd_off, wd_len;
    cxf_xwb_entry *entries;
    size_t         nentries;
} cxf_xwb;

static int cxf_xwb_parse(const cxf_blob *d, cxf_xwb *out, char *err,
                         size_t errsz)
{
    uint32_t seg[4][2];
    uint32_t bd_off, bd_len, count, meta_size, name_size, ne_off, ne_len;
    uint32_t me_off, i;

    memset(out, 0, sizeof *out);
    if (d->n < 4 || memcmp(d->d, "WBND", 4)) {
        snprintf(err, errsz, "not an XWB (missing WBND magic)");
        return -1;
    }
    if (cxf_u32(d, 4, &out->version) != 0) {
        snprintf(err, errsz, "unpack_from: header truncated");
        return -1;
    }
    if (out->version > 4) {
        snprintf(err, errsz, "XWB version %u not supported (only v<=4, "
                 "4-segment XACT1 layout; Burnout 3 uses v3)", out->version);
        return -1;
    }
    for (i = 0; i < 4; i++)
        if (cxf_u32(d, 8 + 8u * i, &seg[i][0]) != 0 ||
            cxf_u32(d, 8 + 8u * i + 4, &seg[i][1]) != 0) {
            snprintf(err, errsz, "unpack_from: segment table truncated");
            return -1;
        }
    bd_off = seg[0][0];
    bd_len = seg[0][1];
    /* python slices `bank = data[bd_off:bd_off+bd_len]` then unpacks from it,
     * so every read below is bounds-checked against that slice. */
    if ((uint64_t)bd_off + 8u > (uint64_t)d->n || bd_len < 8) {
        snprintf(err, errsz, "unpack_from: BANKDATA truncated");
        return -1;
    }
    if (cxf_u32(d, bd_off, &out->flags) != 0 ||
        cxf_u32(d, bd_off + 4, &count) != 0) {
        snprintf(err, errsz, "unpack_from: BANKDATA truncated");
        return -1;
    }
    /* python: bank_name = bank[8:24].split(b"\0")[0] -- a plain slice of the
     * BANKDATA segment, which never raises; it just yields fewer bytes when
     * the segment is short.  Clamp to the segment, not to the file. */
    {
        size_t navail = bd_len > 8 ? bd_len - 8 : 0;
        if (navail > 16)
            navail = 16;
        if (cxf_field(d, bd_off + 8, navail, out->bank_name,
                      sizeof out->bank_name) != 0) {
            snprintf(err, errsz, "unpack_from: BANKDATA truncated");
            return -1;
        }
    }
    if (bd_len < 36 ||
        cxf_u32(d, bd_off + 24, &meta_size) != 0 ||
        cxf_u32(d, bd_off + 28, &name_size) != 0 ||
        cxf_u32(d, bd_off + 32, &out->alignment) != 0) {
        snprintf(err, errsz, "unpack_from: BANKDATA truncated");
        return -1;
    }
    if (meta_size != 24) {
        snprintf(err, errsz, "unexpected entry meta size %u", meta_size);
        return -1;
    }

    ne_off = seg[2][0];
    ne_len = seg[2][1];
    me_off = seg[1][0];
    out->wd_off = seg[3][0];
    out->wd_len = seg[3][1];

    out->entries = (cxf_xwb_entry *)calloc(count ? count : 1u,
                                           sizeof *out->entries);
    if (!out->entries) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    out->nentries = count;
    for (i = 0; i < count; i++) {
        cxf_xwb_entry *e = &out->entries[i];
        uint32_t fd, fmt;
        size_t base = (size_t)me_off + (size_t)i * meta_size;

        if (cxf_u32(d, base, &fd) != 0 || cxf_u32(d, base + 4, &fmt) != 0 ||
            cxf_u32(d, base + 8, &e->offset) != 0 ||
            cxf_u32(d, base + 12, &e->length) != 0 ||
            cxf_u32(d, base + 16, &e->loop_start) != 0 ||
            cxf_u32(d, base + 20, &e->loop_len) != 0) {
            snprintf(err, errsz, "unpack_from: ENTRYMETADATA truncated");
            free(out->entries);
            out->entries = NULL;
            return -1;
        }
        (void)fd;
        e->index = i;
        e->tag      = fmt & 3u;
        e->channels = (fmt >> 2) & 7u;
        e->rate     = (fmt >> 5) & 0x3FFFFu;
        e->align    = (fmt >> 23) & 0xFFu;
        e->bits16   = (fmt >> 31) & 1u;
        if (ne_len) {
            size_t no = (size_t)ne_off + (size_t)i * name_size;
            if (cxf_field(d, no, name_size, e->name, sizeof e->name) == 0)
                e->have_name = 1;
        }
    }
    return 0;
}

static int cxf_xwb_bank(const char *path, const char *out_root, int decode,
                        int use_ffmpeg, int *n_ok, int *n_fail,
                        char *err, size_t errsz)
{
    cxf_blob d;
    cxf_xwb in;
    char bank_dir[3072];
    size_t i;
    int rc = -1;

    *n_ok = *n_fail = 0;
    if (cxf_read_file(path, &d) != 0) {
        snprintf(err, errsz, "cannot read file");
        return -1;
    }
    if (cxf_xwb_parse(&d, &in, err, errsz) != 0) {
        cxf_blob_free(&d);
        return -1;
    }
    snprintf(bank_dir, sizeof bank_dir, "%s/%s", out_root, in.bank_name);
    if (cxf_mkdir_p(bank_dir) != 0) {
        snprintf(err, errsz, "cannot create %s", bank_dir);
        goto done;
    }

    printf("\n== %s  (bank '%s', xwb v%u, %zu entries) ==\n",
           path, in.bank_name, in.version, in.nentries);
    printf("%-4s %-12s %-7s %-6s %-3s %8s %8s  %s\n",
           "idx", "name", "codec", "rate", "ch", "dur(s)", "rms", "status");

    for (i = 0; i < in.nentries; i++) {
        cxf_xwb_entry *e = &in.entries[i];
        cxf_blob payload;
        char status[512], name[80], codec[16], sdur[32], srms[32];
        double dur = 0.0, rms = 0.0;
        int have_dur = 0, have_rms = 0, nonconst = 0, hard_fail = 0;
        size_t s, en;

        s = (size_t)in.wd_off + (size_t)e->offset;
        if (s > d.n) s = d.n;
        en = s + (size_t)e->length;
        if (en > d.n) en = d.n;
        payload.d = d.d + s;
        payload.n = en - s;

        if (e->have_name && e->name[0])
            snprintf(name, sizeof name, "%s", e->name);
        else
            snprintf(name, sizeof name, "%03u", e->index);
        if (e->tag == TAG_PCM)             snprintf(codec, sizeof codec, "pcm");
        else if (e->tag == TAG_XBOX_ADPCM) snprintf(codec, sizeof codec, "xadpcm");
        else if (e->tag == TAG_WMA)        snprintf(codec, sizeof codec, "wma");
        else snprintf(codec, sizeof codec, "tag%u", e->tag);

        status[0] = '\0';
        /* python: hard_fail = any(s for s in status if not s.startswith("("))
         * -- a parenthesised note is informational, everything else fails, so
         * the flag is decided as each status is appended. */
#define ADD(s_) do { const char *s__ = (s_); \
                     if (status[0]) strncat(status, " ", \
                         sizeof status - strlen(status) - 1); \
                     strncat(status, s__, sizeof status - strlen(status) - 1); \
                     if (s__[0] != '(') hard_fail = 1; \
                } while (0)
        if (!(e->rate >= 8000 && e->rate <= 48000))
            ADD("BAD-RATE");
        if (!(e->channels == 1 || e->channels == 2 || e->channels == 4 ||
              e->channels == 6))
            ADD("BAD-CH");
        if (payload.n != e->length)
            ADD("TRUNCATED");

        if (e->tag == TAG_WMA) {
            char wma_path[3200], wav_path[3200];
            FILE *f;

            if (payload.n < 16 || memcmp(payload.d, ASF_MAGIC, 16))
                ADD("NO-ASF-MAGIC");
            else if (cxf_asf_duration(&payload, &dur) == 0)
                have_dur = 1;
            snprintf(wma_path, sizeof wma_path, "%s/%03u.wma", bank_dir, e->index);
            f = fopen(wma_path, "wb");
            if (!f) {
                snprintf(err, errsz, "cannot write %s", wma_path);
                goto done;
            }
            if (payload.n && fwrite(payload.d, 1, payload.n, f) != payload.n) {
                fclose(f);
                snprintf(err, errsz, "short write on %s", wma_path);
                goto done;
            }
            fclose(f);

            if (decode && !status[0]) {
                /* wma_to_wav(), in process now.  Same shape as the python's:
                 * decode, write the .wav, re-open it and cross-check
                 * rate/channels/duration against the bank's own metadata --
                 * which is the point of the exercise, because a decoder that
                 * agrees with the container is a decoder that read the right
                 * bytes.  B3_FFMPEG=1 runs the old subprocess instead. */
                uint32_t drate = 0, dch = 0, dsw = 0;
                unsigned char *pcm = NULL;
                size_t pn = 0;
                int ok;

                snprintf(wav_path, sizeof wav_path, "%s/%03u.wav",
                         bank_dir, e->index);
                if (use_ffmpeg) {
                    const char *av[10];
                    av[0] = "ffmpeg";  av[1] = "-v";      av[2] = "error";
                    av[3] = "-y";      av[4] = "-i";      av[5] = wma_path;
                    av[6] = "-acodec"; av[7] = "pcm_s16le";
                    av[8] = wav_path;  av[9] = NULL;
                    ok = (cxf_run(av) == 0 && cxf_file_size(wav_path) >= 0);
                } else {
                    int16_t    *s16 = NULL;
                    size_t      frames = 0;
                    b3_wma_info info;
                    char        werr[256];
                    ok = 0;
                    if (b3_wma_decode(payload.d, payload.n, &s16, &frames,
                                      &info, werr, sizeof werr) == 0) {
                        ok = cxf_write_wav(wav_path, (const unsigned char *)s16,
                                           frames * info.channels * 2u,
                                           info.rate, info.channels, 2u) == 0;
                        free(s16);
                    }
                }
                if (!ok)
                    ADD("DECODE-FAIL");
                else if (cxf_read_wav(wav_path, &drate, &dch, &dsw,
                                      &pcm, &pn) != 0 || dsw != 2)
                    ADD("DECODE-FAIL");
                else {
                    char t[64];
                    cxf_pcm16_stats(pcm, pn, &rms, &nonconst);
                    have_rms = 1;
                    if (drate != e->rate) {
                        snprintf(t, sizeof t, "RATE-MISMATCH(%u)", drate);
                        ADD(t);
                    }
                    if (dch != e->channels) {
                        snprintf(t, sizeof t, "CH-MISMATCH(%u)", dch);
                        ADD(t);
                    }
                    if (have_dur) {
                        double got = (double)pn /
                            (2.0 * (double)dch * (double)drate);
                        double tol = dur * 0.05;
                        if (tol < 0.5) tol = 0.5;
                        if (fabs(got - dur) > tol) {
                            snprintf(t, sizeof t, "DUR-MISMATCH(%.2f!=%.2f)",
                                     got, dur);
                            ADD(t);
                        }
                    }
                    if (rms < 1.0 || !nonconst)
                        ADD("SILENT");
                }
                free(pcm);
            } else if (!decode) {
                ADD("(wma dumped; decoding disabled -> no PCM check)");
            }
        } else if (e->tag == TAG_XBOX_ADPCM) {
            size_t pn = 0;
            unsigned char *pcm = cxf_decode_xbox_adpcm(payload.d, payload.n,
                                                       e->channels, &pn);
            char wp[3200];
            if (!pcm) {
                snprintf(err, errsz, "adpcm decode failed");
                goto done;
            }
            if (e->channels) {
                size_t expect = (size_t)(e->length / (36u * e->channels)) *
                                65u * e->channels;
                if (pn / 2u != expect)
                    ADD("LEN-MISMATCH");
            }
            cxf_pcm16_stats(pcm, pn, &rms, &nonconst);
            have_rms = 1;
            dur = (double)pn / (2.0 * (double)e->channels * (double)e->rate);
            have_dur = 1;
            if (rms < 1.0 || !nonconst)
                ADD("SILENT");
            snprintf(wp, sizeof wp, "%s/%03u.wav", bank_dir, e->index);
            if (cxf_write_wav(wp, pcm, pn, e->rate, e->channels, 2) != 0) {
                free(pcm);
                snprintf(err, errsz, "cannot write %s", wp);
                goto done;
            }
            free(pcm);
        } else if (e->tag == TAG_PCM) {
            uint32_t width = e->bits16 ? 2u : 1u;
            char wp[3200];
            if (e->bits16) {
                cxf_pcm16_stats(payload.d, payload.n, &rms, &nonconst);
                have_rms = 1;
                if (rms < 1.0 || !nonconst)
                    ADD("SILENT");
            }
            dur = (double)payload.n /
                  ((double)width * (double)e->channels * (double)e->rate);
            have_dur = 1;
            snprintf(wp, sizeof wp, "%s/%03u.wav", bank_dir, e->index);
            if (cxf_write_wav(wp, payload.d, payload.n, e->rate,
                              e->channels, width) != 0) {
                snprintf(err, errsz, "cannot write %s", wp);
                goto done;
            }
        } else {
            ADD("UNKNOWN-TAG");
        }

        *n_fail += hard_fail;
        *n_ok += !hard_fail;
        if (have_dur) snprintf(sdur, sizeof sdur, "%.2f", dur);
        else          snprintf(sdur, sizeof sdur, "-");
        if (have_rms) snprintf(srms, sizeof srms, "%.0f", rms);
        else          snprintf(srms, sizeof srms, "-");
        printf("%-4u %-12s %-7s %-6u %-3u %8s %8s  %s\n",
               e->index, name, codec, e->rate, e->channels, sdur, srms,
               status[0] ? status : "OK");
#undef ADD
    }
    printf("-- %s: %d ok, %d failed\n", in.bank_name, *n_ok, *n_fail);
    rc = 0;

done:
    free(in.entries);
    cxf_blob_free(&d);
    return rc;
}

int cx_extract_xwb(const char *game_dir, const char *out_root)
{
    game_dir = cxf_game_dir(game_dir);
    static const char *const EXTS[] = { ".xwb" };
    cxf_strlist files = { NULL, 0, 0 };
    size_t i;
    int tot_ok = 0, tot_fail = 0, n_err = 0, decode, use_ffmpeg;
    const char *ff = getenv("B3_FFMPEG");

    if (cxf_walk_ext(game_dir, EXTS, 1, &files) != 0) {
        fprintf(stderr, "[cx_audio_xwb] cannot scan %s\n", game_dir);
        cxf_strlist_free(&files);
        return 1;
    }
    cxf_strlist_sort(&files);

    /* CX_NO_FFMPEG kept its name: it is in the python's CLI, in the handoff
     * docs and in several recipes, and it always meant "dump the .wma and do
     * not decode".  That is still exactly what it does -- there is simply no
     * longer an ffmpeg involved in the decoding it is switching off. */
    use_ffmpeg = ff && *ff && strcmp(ff, "0") != 0;
    decode     = getenv("CX_NO_FFMPEG") ? 0 : 1;
    if (decode && use_ffmpeg && !cxf_have_ffmpeg()) {
        printf("note: B3_FFMPEG=1 but ffmpeg is not on PATH; "
               "using the built-in decoder\n");
        use_ffmpeg = 0;
    }
    if (decode && !use_ffmpeg && !b3_wma_available()) {
        printf("note: no WMA decoder in this build (sh tools/fetch_wma.sh); "
               "WMA entries will be dumped as .wma only\n");
        decode = 0;
    }

    for (i = 0; i < files.n; i++) {
        char err[8704];
        int ok = 0, fail = 0;
        err[0] = '\0';
        if (cxf_xwb_bank(files.v[i], out_root, decode, use_ffmpeg, &ok, &fail,
                         err, sizeof err) != 0) {
            printf("\n== %s == ERROR: %s\n", files.v[i], err);
            ok = 0;
            fail = 1;
            n_err++;
        }
        tot_ok += ok;
        tot_fail += fail;
    }
    printf("\nTOTAL: %d waves ok, %d failed, %zu banks\n",
           tot_ok, tot_fail, files.n);
    cxf_strlist_free(&files);
    /* python: `return 1 if tot_fail else 0` -- see cxf_rc(). */
    return cxf_rc(n_err, tot_fail);
}
