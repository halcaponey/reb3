/* cx_audio_eatrax.c -- port of tools/extract_eatrax.py (THE SPEC).
 *
 * The two licensed-soundtrack wave banks
 *
 *     Tracks/_EATrax0.xwb   22 entries
 *     Tracks/_EATrax1.xwb   22 entries
 *
 * are XACT v3 wave banks whose entries are all wmav2, 2ch, 44100 Hz.  Each
 * entry is carved out verbatim as a standalone ASF file and DECODED IN
 * PROCESS (tools/cextract/wma/b3_wma.c), which writes
 *
 *     <out_root>/track_NN.wav      44100 Hz, MONO, 16-bit PCM
 *
 * -- mono because the harness's SDL device is 44100/mono/s16.
 *
 * ======================================= THE SUBPROCESS THAT USED TO BE HERE
 * This stage used to write the payload to a temp file and run
 *
 *     ffmpeg -v error -y -i <tmp>.wma -ac 1 -ar 44100 -acodec pcm_s16le <dst>
 *
 * which is why it -- and cx_audio_xwb.c -- sat in the Makefile's CX_SKIP and
 * were NOT linked into the game.  Two consequences, both now gone: music
 * existed only for someone who had run the extractor by hand and had ffmpeg on
 * PATH, and it could not exist AT ALL on the web, where there is no process to
 * fork and no WMA decoder in the browser to delegate to.
 *
 * B3_FFMPEG=1 restores the old path verbatim, for A/B.  It is the ORACLE, not
 * a fallback: tools/validate_wma.py runs both arms over the same payloads and
 * scores them, because the two decoders are different implementations and
 * byte-identity between them is not a thing to expect.  Measured over all 44
 * tracks: SNR 58.4-79.9 dB (median 78.2), correlation 1.000000, no clipping
 * introduced.  See docs/RE_MUSIC.md.
 *
 * Also writes <out_root>/eatrax.txt, the manifest
 *     index bank wave frames rate sha1 | artist | title | album
 * that tools/validate_music.py checks the C table against.
 *
 * The artist/title/album strings are NOT invented here: they are the game's
 * own, from the 44-entry song table at VA 0x003EC458 in the retail XBE
 * (24-byte stride, +0x00 title / +0x04 album / +0x08 artist as Globalus.bin
 * string indices).  This file carries only the INDICES; the text is resolved
 * from Globalus.bin at run time (env B3_GLOBALUS, else "build/Globalus.bin"
 * relative to the working directory, which is what the python's --globalus
 * default resolves to).
 *
 * ============================== THE TWO PYTHON QUIRKS, AND WHY THEY RETIRED
 * The python computed `frames = (len(wav) - 44) // 2`, assuming a 44-byte
 * canonical WAV header -- but ffmpeg's muxer emits a LIST/INFO chunk carrying
 * its ISFT encoder tag, so the payload really started at byte 78 and every
 * `frames` in the manifest was 17 SAMPLES HIGH.  Quirk 2 was the same
 * off-by-34-bytes reappearing in `mid = 44 + (frames // 3) * 2`, which put the
 * one-second non-silence probe 17 samples early (harmless: both 44 and 78 are
 * even, so the window stayed sample-aligned).
 *
 * THE ARITHMETIC IS UNCHANGED AND THE ANSWER IS NOW RIGHT.  Both formulas are
 * kept exactly as the python wrote them; what changed underneath is that this
 * stage writes the .wav ITSELF, through cxf_write_wav(), which emits the
 * canonical 44-byte header the python always assumed.  The quirk was never in
 * the formula -- it was in ffmpeg's muxer -- and removing the muxer removed
 * the quirk.  So `frames` is now the true sample count rather than 17 over.
 *
 * THAT IS A DELIBERATE, VISIBLE MANIFEST CHANGE, and it is not the only one:
 * the sha1 column is a hash of the .wav bytes, and two different decoders do
 * not produce identical bytes.  Byte-identity against the archived python was
 * never available for this stage -- the python shelled to ffmpeg too, so its
 * output was only ever as reproducible as whatever ffmpeg build was on PATH.
 * The gate is therefore an SNR comparison against a freshly generated ffmpeg
 * oracle, not a byte diff; tools/validate_wma.py is that gate.
 */
#define _POSIX_C_SOURCE 200809L

#include "cx_audio_common.h"
#include "cx_extract.h"
#include "b3_wma.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* --- the song table, VA 0x003EC458, 44 x 24 bytes ----------------------- */
/* (title_id, album_id, artist_id) per entry, in table order.  Entries 0..39
 * use the contiguous Globalus block 456..575; entries 40..43 were appended
 * late in development and use 3255..3266. */
#define SONG_COUNT 44
#define PER_BANK   22

static const char *const BANKS[2] = { "_EATrax0.xwb", "_EATrax1.xwb" };

static void song_ids(int i, int *title, int *album, int *artist)
{
    if (i < 40) {
        *title  = 456 + 3 * i;
        *album  = 457 + 3 * i;
        *artist = 458 + 3 * i;
    } else {
        int k = i - 40;
        *title  = 3255 + 3 * k;
        *album  = 3256 + 3 * k;
        *artist = 3257 + 3 * k;
    }
}

/* --- Globalus.bin ------------------------------------------------------- */
/* count at +0x08, u32 offset table at +0x10, UTF-16LE strings terminated by
 * a 00 00 pair (the scan walks two bytes at a time from the offset). */
static int globalus_one(const cxf_blob *b, int i, char *out, size_t outsz)
{
    uint32_t off;
    size_t end;

    if (cxf_u32(b, 0x10 + (size_t)i * 4, &off) != 0)
        return -1;
    end = off;
    while (end + 1 < b->n && !(b->d[end] == 0 && b->d[end + 1] == 0))
        end += 2;
    if (end < off)
        return -1;
    if (cxf_utf16le_to_utf8(b->d + off, end - off, out, outsz) != 0)
        return -1;
    cxf_strip_unicode(out);
    return 0;
}

/* --- XWB ---------------------------------------------------------------- */
typedef struct {
    uint32_t codec, channels, rate, offset, length;
} cxf_trax_entry;

static int trax_bank(const char *path, cxf_blob *blob, cxf_trax_entry *out,
                     size_t *n)
{
    uint32_t ver, seg[4][2], count, i, meta, data;

    if (cxf_read_file(path, blob) != 0) {
        fprintf(stderr, "missing bank: %s\n", path);
        return -1;
    }
    if (blob->n < 4 || memcmp(blob->d, "WBND", 4)) {
        fprintf(stderr, "%s: not a WBND wave bank\n", path);
        return -1;
    }
    if (cxf_u32(blob, 4, &ver) != 0 || ver != 3) {
        fprintf(stderr, "%s: unexpected XWB version %u\n", path, ver);
        return -1;
    }
    for (i = 0; i < 4; i++)
        if (cxf_u32(blob, 8 + 8u * i, &seg[i][0]) != 0 ||
            cxf_u32(blob, 8 + 8u * i + 4, &seg[i][1]) != 0)
            return -1;
    if (cxf_u32(blob, seg[0][0] + 4, &count) != 0)
        return -1;
    meta = seg[1][0];
    data = seg[3][0];
    if (count != PER_BANK) {
        fprintf(stderr, "%s: expected %d entries, found %u\n",
                path, PER_BANK, count);
        return -1;
    }
    for (i = 0; i < count; i++) {
        uint32_t fmt, off, ln;
        size_t base = (size_t)meta + (size_t)i * 24u;
        /* python: struct.unpack_from("<6I", d, meta + i*24) -- all 24 bytes */
        if (base + 24u > blob->n ||
            cxf_u32(blob, base + 4, &fmt) != 0 ||
            cxf_u32(blob, base + 8, &off) != 0 ||
            cxf_u32(blob, base + 12, &ln) != 0)
            return -1;
        out[i].codec    = fmt & 3u;
        out[i].channels = (fmt >> 2) & 7u;
        out[i].rate     = (fmt >> 5) & 0x3FFFFu;
        out[i].offset   = data + off;
        out[i].length   = ln;
    }
    *n = count;
    return 0;
}

/* --- one track ---------------------------------------------------------- */
typedef struct {
    int      ok;
    int      bank, wave;
    long     frames;
    int      rate;
    int      peak;
    char     sha[13];
    char     artist[512], title[512], album[512];
    uint32_t srate;
    char     err[256];
} cxf_trax_result;

/* THE DEFAULT PATH: decode in this process, downmix, write the .wav.
 * 0 on success, -1 with r->err set. */
static int trax_via_libwma(const unsigned char *payload, size_t plen,
                           const char *dst, int rate, cxf_trax_result *r)
{
    int16_t     *pcm = NULL;
    size_t       frames = 0;
    b3_wma_info  info;
    char         werr[256];
    int          rc;

    if (b3_wma_decode(payload, plen, &pcm, &frames, &info,
                      werr, sizeof werr) != 0) {
        snprintf(r->err, sizeof r->err, "wma decode: %.230s", werr);
        return -1;
    }
    /* Stereo -> mono, the SDL device's format.  ffmpeg's `-ac 1` is a 0.5/0.5
     * matrix and so is this; the A/B in tools/validate_wma.py covers the
     * difference, which is rounding. */
    b3_wma_downmix_mono(pcm, frames, info.channels);
    if ((uint32_t)rate != info.rate) {
        /* Only reachable through B3_EATRAX_RATE.  The disc is 44100 and so is
         * the device, so the shipping path never resamples -- which matters,
         * because a linear resampler is not what the oracle uses and an SNR
         * measured through one says nothing about the decoder. */
        size_t   n = 0;
        int16_t *rs = b3_wma_resample(pcm, frames, 1, info.rate,
                                      (uint32_t)rate, &n);
        if (!rs) {
            free(pcm);
            snprintf(r->err, sizeof r->err, "resample %u -> %d failed",
                     info.rate, rate);
            return -1;
        }
        free(pcm);
        pcm = rs;
        frames = n;
    }
    rc = cxf_write_wav(dst, (const unsigned char *)pcm, frames * 2u,
                       (uint32_t)rate, 1u, 2u);
    free(pcm);
    if (rc != 0) {
        snprintf(r->err, sizeof r->err, "cannot write %.230s", dst);
        return -1;
    }
    return 0;
}

/* THE ORACLE ARM, B3_FFMPEG=1: the exact command line this stage used to run,
 * kept so the two decoders can be scored against each other on the same
 * payloads.  Not a fallback -- if the vendored decoder is missing, that is a
 * build problem to fix, not something to paper over with a host binary that
 * may not be installed. */
static int trax_via_ffmpeg(const unsigned char *payload, size_t plen, int idx,
                           const char *dst, int rate, cxf_trax_result *r)
{
    char        tmp[4096], rate_s[32];
    const char *av[14];
    FILE       *f;
    int         fd, attempt;
    const char *td = getenv("TMPDIR");

    fd = -1;
    for (attempt = 0; attempt < 4096 && fd < 0; attempt++) {
        snprintf(tmp, sizeof tmp, "%s/cxf_eatrax_%ld_%d_%d.wma",
                 (td && *td) ? td : "/tmp", (long)getpid(), idx, attempt);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        snprintf(r->err, sizeof r->err, "cannot create temp file");
        return -1;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(tmp);
        snprintf(r->err, sizeof r->err, "cannot create temp file");
        return -1;
    }
    if (plen && fwrite(payload, 1, plen, f) != plen) {
        fclose(f);
        unlink(tmp);
        snprintf(r->err, sizeof r->err, "short write on temp file");
        return -1;
    }
    fclose(f);

    snprintf(rate_s, sizeof rate_s, "%d", rate);
    av[0]  = "ffmpeg";  av[1]  = "-v";        av[2]  = "error";
    av[3]  = "-y";      av[4]  = "-i";        av[5]  = tmp;
    av[6]  = "-ac";     av[7]  = "1";
    av[8]  = "-ar";     av[9]  = rate_s;
    av[10] = "-acodec"; av[11] = "pcm_s16le";
    av[12] = dst;       av[13] = NULL;
    if (cxf_run(av) != 0) {
        unlink(tmp);
        snprintf(r->err, sizeof r->err, "ffmpeg failed");
        return -1;
    }
    unlink(tmp);
    return 0;
}

static void decode_one(int idx, const cxf_blob *blob, const cxf_trax_entry *e,
                       const char *outdir, int rate, cxf_trax_result *r)
{
    char dst[4096];
    const unsigned char *payload;
    size_t plen;
    cxf_blob wav;
    unsigned char digest[20];
    char hex[41];
    const char *use_ffmpeg = getenv("B3_FFMPEG");

    if (e->codec != 2) {
        snprintf(r->err, sizeof r->err, "codec %u is not WMA", e->codec);
        return;
    }
    payload = blob->d + e->offset;
    plen = e->length;
    if ((size_t)e->offset > blob->n)
        plen = 0;
    else if ((size_t)e->offset + plen > blob->n)
        plen = blob->n - e->offset;
    if (plen < 8 || memcmp(payload, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)) {
        snprintf(r->err, sizeof r->err, "payload is not ASF");
        return;
    }

    snprintf(dst, sizeof dst, "%s/track_%02d.wav", outdir, idx);

    if (use_ffmpeg && *use_ffmpeg && strcmp(use_ffmpeg, "0") != 0) {
        if (trax_via_ffmpeg(payload, plen, idx, dst, rate, r) != 0)
            return;
    } else if (!b3_wma_available()) {
        /* Built without tools/fetch_wma.sh having been run.  Say which of the
         * two things to do rather than just failing. */
        snprintf(r->err, sizeof r->err,
                 "no WMA decoder in this build (sh tools/fetch_wma.sh, "
                 "or B3_FFMPEG=1)");
        return;
    } else if (trax_via_libwma(payload, plen, dst, rate, r) != 0) {
        return;
    }

    if (cxf_read_file(dst, &wav) != 0) {
        snprintf(r->err, sizeof r->err, "cannot read the decoded wav");
        return;
    }
    if (wav.n < 12 || memcmp(wav.d, "RIFF", 4) || memcmp(wav.d + 8, "WAVE", 4)) {
        cxf_blob_free(&wav);
        snprintf(r->err, sizeof r->err, "ffmpeg output is not RIFF/WAVE");
        return;
    }
    /* The python's arithmetic, unchanged -- and now exact, because this stage
     * writes the canonical 44-byte header it always assumed.  See the header
     * block: the 17-sample error lived in ffmpeg's LIST/INFO chunk, not here.
     * (B3_FFMPEG=1 puts the chunk back, and with it the 17.) */
    r->frames = (long)((wav.n - 44u) / 2u);
    /* Same base, same reasoning: the peak window now starts where it says. */
    {
        size_t mid = 44u + (size_t)(r->frames / 3) * 2u;
        size_t avail = mid < wav.n ? wav.n - mid : 0;
        size_t win = (size_t)rate * 2u;
        size_t k;
        int peak = 0;
        if (win > avail)
            win = avail;
        if (win >= 2) {
            for (k = 0; k + 1 < win; k += 2) {
                int v = (int16_t)((uint16_t)wav.d[mid + k] |
                                  ((uint16_t)wav.d[mid + k + 1] << 8));
                int a = v < 0 ? -v : v;
                if (a > peak)
                    peak = a;
            }
        }
        r->peak = peak;
    }
    cxf_sha1(wav.d, wav.n, digest);
    cxf_hex(digest, 20, hex);
    memcpy(r->sha, hex, 12);
    r->sha[12] = '\0';
    cxf_blob_free(&wav);

    r->rate = rate;
    r->srate = e->rate;
    r->ok = 1;
}

/* ================================================== ONE SONG, ON DEMAND ====
 * The whole-family entry point below decodes all 44 tracks and writes the
 * manifest.  That is right for `cxtract --only eatrax`, and WRONG for the
 * game: 44 tracks is ~720 MB of 44.1 kHz mono s16 and roughly a minute of
 * decoding, paid at the first note of music.  src/burnout3_isodata.c therefore
 * materialises build/music/track_NN.wav ONE SONG AT A TIME through this, and
 * the shuffle only ever asks for the song it is about to play.
 *
 * Deliberately NOT done here:
 *   * the manifest.  eatrax.txt is a whole-family artefact -- 44 rows -- and a
 *     per-song run that rewrote it would leave a one-row file behind.  It
 *     belongs to the batch path, which is the only thing that reads it
 *     (tools/validate_music.py).
 *   * Globalus.bin.  Resolving artist/title/album needs it, the manifest is
 *     the only consumer of those strings, and the game already carries the
 *     song table compiled in (src/burnout3_music.c).  Not reading it here
 *     means a song can materialise before the '@globalus' unit has run.
 *
 * `index` is 0..43.  Returns 0 on success. */
int cx_extract_eatrax_one(const char *game_dir, const char *out_root, int index)
{
    cxf_blob        bank = { NULL, 0 };
    cxf_trax_entry  ents[PER_BANK];
    cxf_trax_result res;
    char            path[4096];
    size_t          n = 0;
    int             rate = 44100, b, rc = 1;

    if (index < 0 || index >= SONG_COUNT)
        return 1;
    if (getenv("B3_EATRAX_RATE"))
        rate = atoi(getenv("B3_EATRAX_RATE"));
    game_dir = cxf_game_dir(game_dir);

    memset(&res, 0, sizeof res);
    if (cxf_mkdir_p(out_root) != 0) {
        fprintf(stderr, "[cx_audio_eatrax] cannot create %s\n", out_root);
        return 1;
    }
    b = index / PER_BANK;
    snprintf(path, sizeof path, "%s/Tracks/%s", game_dir, BANKS[b]);
    if (trax_bank(path, &bank, ents, &n) != 0)
        goto done;

    res.bank = b;
    res.wave = index % PER_BANK;
    decode_one(index, &bank, &ents[index % PER_BANK], out_root, rate, &res);
    if (!res.ok) {
        fprintf(stderr, "[cx_audio_eatrax] track %d: %s\n", index, res.err);
        goto done;
    }
    rc = 0;

done:
    cxf_blob_free(&bank);
    return rc;
}

int cx_extract_eatrax(const char *game_dir, const char *out_root)
{
    game_dir = cxf_game_dir(game_dir);
    const char *gpath = getenv("B3_GLOBALUS");
    const char *only = getenv("B3_EATRAX_ONLY");
    int rate = 44100;
    cxf_blob glob = { NULL, 0 };
    cxf_blob banks[2] = { { NULL, 0 }, { NULL, 0 } };
    cxf_trax_entry ents[2][PER_BANK];
    cxf_trax_result res[SONG_COUNT];
    char man[4096];
    int want[SONG_COUNT];
    int b, i, njobs = 0, bad = 0, nres = 0, n_err = 0, rc = 1;
    FILE *f;

    memset(res, 0, sizeof res);
    for (i = 0; i < SONG_COUNT; i++)
        want[i] = 1;
    if (only && *only) {
        const char *p = only;
        for (i = 0; i < SONG_COUNT; i++)
            want[i] = 0;
        while (*p) {
            int v = atoi(p);
            if (v >= 0 && v < SONG_COUNT)
                want[v] = 1;
            while (*p && *p != ',')
                p++;
            if (*p == ',')
                p++;
        }
    }
    if (getenv("B3_EATRAX_RATE"))
        rate = atoi(getenv("B3_EATRAX_RATE"));

    if (!gpath || !*gpath)
        gpath = "build/Globalus.bin";
    if (cxf_read_file(gpath, &glob) != 0) {
        fprintf(stderr, "[cx_audio_eatrax] cannot read %s "
                "(set B3_GLOBALUS)\n", gpath);
        return 1;
    }

    /* song_list(): resolve artist/title/album for all 44 entries */
    for (i = 0; i < SONG_COUNT; i++) {
        int t, al, ar;
        song_ids(i, &t, &al, &ar);
        if (globalus_one(&glob, ar, res[i].artist, sizeof res[i].artist) != 0 ||
            globalus_one(&glob, t, res[i].title, sizeof res[i].title) != 0 ||
            globalus_one(&glob, al, res[i].album, sizeof res[i].album) != 0) {
            fprintf(stderr, "[cx_audio_eatrax] Globalus index out of range "
                    "for song %d\n", i);
            cxf_blob_free(&glob);
            return 1;
        }
    }
    cxf_blob_free(&glob);

    if (cxf_mkdir_p(out_root) != 0) {
        fprintf(stderr, "[cx_audio_eatrax] cannot create %s\n", out_root);
        return 1;
    }

    for (b = 0; b < 2; b++) {
        char path[4096];
        size_t n = 0;
        snprintf(path, sizeof path, "%s/Tracks/%s", game_dir, BANKS[b]);
        if (trax_bank(path, &banks[b], ents[b], &n) != 0)
            goto done;
    }
    for (i = 0; i < SONG_COUNT; i++)
        if (want[i])
            njobs++;

    printf("EA TRAX: %d tracks -> %s  (44100->%d Hz, mono s16)\n",
           njobs, out_root, rate);
    for (i = 0; i < SONG_COUNT; i++) {
        if (!want[i])
            continue;
        b = i / PER_BANK;
        res[i].bank = b;
        res[i].wave = i % PER_BANK;
        decode_one(i, &banks[b], &ents[b][i % PER_BANK], out_root, rate,
                   &res[i]);
        if (!res[i].ok) {
            bad++;
            n_err++;                   /* a track that would not decode */
            printf("  [%2d] FAIL  %s\n", i, res[i].err);
        } else {
            nres++;
        }
    }

    printf("%-3s %-4s %-4s %-8s %-6s %s\n",
           "#", "bank", "wave", "seconds", "peak", "artist -- title");
    for (i = 0; i < SONG_COUNT; i++) {
        double secs;
        const char *status;
        if (!want[i] || !res[i].ok)
            continue;
        secs = (double)res[i].frames / (double)res[i].rate;
        status = (res[i].peak > 1000 && secs > 30.0) ? "" : "   << SUSPECT";
        printf("%-3d %-4d %-4d %8.1f %6d  %s -- %s%s\n",
               i, res[i].bank, res[i].wave, secs, res[i].peak,
               res[i].artist, res[i].title, status);
        if (status[0])
            bad++;
    }

    snprintf(man, sizeof man, "%s/eatrax.txt", out_root);
    f = fopen(man, "w");
    if (!f) {
        fprintf(stderr, "[cx_audio_eatrax] cannot write %s\n", man);
        goto done;
    }
    fprintf(f, "# index bank wave frames rate sha1 | artist | title | album\n");
    for (i = 0; i < SONG_COUNT; i++) {
        if (!want[i] || !res[i].ok)
            continue;
        fprintf(f, "%d %d %d %ld %d %s | %s | %s | %s\n",
                i, res[i].bank, res[i].wave, res[i].frames, res[i].rate,
                res[i].sha, res[i].artist, res[i].title, res[i].album);
    }
    fclose(f);
    printf("manifest: %s\n", man);
    printf("%d/%d ok\n", nres - bad, njobs);
    /* python: `return 1 if bad else 0`, where `bad` counts both a track that
     * would not decode and one merely flagged "<< SUSPECT" -- see cxf_rc(). */
    rc = cxf_rc(n_err, bad);

done:
    for (b = 0; b < 2; b++)
        cxf_blob_free(&banks[b]);
    return rc;
}
