/* tools/cextract/wma/b3_wma.c -- ASF in memory, PCM out, no subprocess.
 *
 * Two halves, and the split matters:
 *
 *   1. AN ASF DEMUXER FOR A FILE THAT IS ALREADY IN RAM.  Rockbox ships one
 *      (libasf/asf.c) and it is NOT fetched, because it is written against
 *      `ci->read_filebuf` / `ci->advance_buffer` -- a streaming reader for a
 *      DAP pulling off a disk one buffer at a time.  Our payloads arrive
 *      whole: cx_audio_xwb.c carves a complete standalone ASF file out of the
 *      wave bank's ENTRYWAVEDATA segment and hands it over as a pointer and a
 *      length.  Reimplementing the walk over a flat buffer is ~200 lines and
 *      removes the entire callback table; adapting theirs would have meant
 *      editing vendored LGPL code, which tools/fetch_wma.sh exists to avoid.
 *      The PACKET GRAMMAR below is Rockbox's -- the length-type nibbles, the
 *      "packet length can be undefined so use the header's" quirk, the
 *      multiple-payload coalesce -- because that grammar is the format, and
 *      the format is not ours to reinvent.
 *
 *   2. A DRIVER FOR libwma, standing in for Rockbox's codec plugin.  Same
 *      three calls in the same order the plugin makes them; the difference is
 *      that the plugin streams into a playback engine and this accumulates
 *      into one block, because both callers want a .wav on the other side.
 *
 * ============================================== THE OUTPUT SCALE, MEASURED ==
 * libwma writes `fixed32` into frame_out and Rockbox's DSP consumes it through
 * ci->pcmbuf_insert, so the scale is a convention between two files neither of
 * which states it in a comment.  It was not guessed.  B3_WMA_OUT_SHIFT was
 * swept against the ffmpeg oracle over whole EA TRAX tracks and read off the
 * correlation: at the value below the decode lands on ffmpeg's, and one bit
 * either way is a clean 6 dB gain error -- a wrong answer here is not noise,
 * it is the whole track twice or half as loud.  tools/validate_wma.py --sweep
 * reproduces the measurement; see docs/RE_MUSIC.md for the numbers.
 */
#define _POSIX_C_SOURCE 200809L

#include "b3_wma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <codecs/libasf/asf.h>
#include "wmadec.h"

/* Rockbox's decoder emits 30.2 fixed point relative to full scale: the sample
 * that would be +1.0 in ffmpeg's float output arrives here as 1<<29.  s16 full
 * scale is 1<<15, so the shift is 14.  See the header block above -- this is a
 * measured constant, not a derivation, and the sweep is the evidence. */
#define B3_WMA_OUT_SHIFT 14

/* ================================ TWO CONVENTIONS libwma DOES NOT DOCUMENT ==
 * Both were found the same way -- one full-file normalised cross-correlation
 * of this decoder's output against ffmpeg's, over EA TRAX track 00 -- and both
 * are exact rather than approximate, which is what makes them safe to bake in.
 * The correlogram had a single peak of r = -0.998735 at lag -4096, with every
 * neighbouring lag falling away smoothly.  Read it in two parts:
 *
 * SIGN.  The peak is NEGATIVE.  Rockbox's fixed-point IMDCT carries the
 * opposite polarity to FFmpeg's float one, so every sample comes out inverted.
 * On a DAP that is inaudible and nobody ever noticed; against a bit-comparable
 * oracle it is the difference between +55 dB and -2 dB, so it is corrected
 * here rather than explained away.  An absolute polarity is not audible on its
 * own, but it IS audible against something else -- and this engine mixes music
 * with crash beds and engine loops that come from a different decoder.
 *
 * DELAY.  4096 samples, which is exactly 2 * frame_len at these rates.  One
 * frame_len is the MDCT overlap-add latency: a frame is only complete after
 * the next one has been windowed into it.  The other is the first superframe's
 * dropped frame -- wma_decode_superframe_init() does `nb_frames--` when
 * last_superframe_len is 0, which is true only for the first packet.  FFmpeg
 * discards that priming; Rockbox streams it, because a DAP does not care about
 * 93 ms at the head of a song.  We discard it too, so the .wav this writes
 * starts on the same sample ffmpeg's does and the two can be diffed.
 *
 * It is expressed as a multiple of frame_len, NOT as the literal 4096, so a
 * hypothetical low-rate stream (libwma picks frame_len 512 or 1024 below
 * 32 kHz) is still trimmed correctly.  Nothing on this disc is one. */
#define B3_WMA_PRIME_FRAMES 2

#define ERR(...) do { if (err && errsz) snprintf(err, errsz, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------- ASF GUIDs -- */
/* Little-endian on the wire, so these are the bytes as they appear in a file
 * rather than the {D0-D1-D2-...} text form Microsoft documents them in. */
static const uint8_t GUID_HEADER[16] = {
    0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C };
static const uint8_t GUID_FILE_PROPS[16] = {
    0xA1,0xDC,0xAB,0x8C,0x47,0xA9,0xCF,0x11,0x8E,0xE4,0x00,0xC0,0x0C,0x20,0x53,0x65 };
static const uint8_t GUID_STREAM_PROPS[16] = {
    0x91,0x07,0xDC,0xB7,0xB7,0xA9,0xCF,0x11,0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65 };
static const uint8_t GUID_AUDIO_MEDIA[16] = {
    0x40,0x9E,0x69,0xF8,0x4D,0x5B,0xCF,0x11,0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B };
static const uint8_t GUID_DATA[16] = {
    0x36,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C };

/* ---------------------------------------------------- unaligned LE reads -- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* The ASF "length type" nibble: 0 = absent, 1 = byte, 2 = word, 3 = dword.
 * Note 3 means FOUR, which is why this cannot just be the nibble itself. */
static uint32_t len2b(unsigned bits) { return bits == 3u ? 4u : bits; }
static uint32_t val2b(unsigned bits, const uint8_t *p)
{
    switch (bits) {
    case 1:  return p[0];
    case 2:  return rd16(p);
    case 3:  return rd32(p);
    default: return 0;
    }
}

/* ------------------------------------------------------------ the header -- */
typedef struct {
    asf_waveformatex_t wfx;
    size_t   data_off;      /* first byte of the first data packet */
    size_t   data_end;
    double   duration;
} b3_asf_file;

static int asf_parse_header(const uint8_t *d, size_t n, b3_asf_file *f,
                            char *err, size_t errsz)
{
    uint64_t hdr_size;
    uint32_t nobj, k;
    size_t   pos = 30;
    int      have_audio = 0, have_props = 0;

    memset(f, 0, sizeof *f);
    if (n < 30 || memcmp(d, GUID_HEADER, 16) != 0) {
        ERR("not an ASF file (no header GUID)");
        return -1;
    }
    hdr_size = rd64(d + 16);
    nobj     = rd32(d + 24);
    if (hdr_size > n) {
        ERR("ASF header claims %llu bytes, payload is %zu",
            (unsigned long long)hdr_size, n);
        return -1;
    }

    for (k = 0; k < nobj; k++) {
        const uint8_t *g;
        uint64_t osize;
        const uint8_t *body;
        size_t blen;

        if (pos + 24 > hdr_size)
            break;
        g     = d + pos;
        osize = rd64(d + pos + 16);
        if (osize < 24 || pos + osize > hdr_size) {
            ERR("ASF object %u has impossible size %llu",
                k, (unsigned long long)osize);
            return -1;
        }
        body = d + pos + 24;
        blen = (size_t)osize - 24;

        if (memcmp(g, GUID_FILE_PROPS, 16) == 0) {
            uint64_t play, preroll;
            uint32_t minp, maxp;
            if (blen < 80) { ERR("File Properties object truncated"); return -1; }
            f->wfx.numpackets = rd64(body + 32);
            play    = rd64(body + 40);
            preroll = rd64(body + 56);
            minp    = rd32(body + 68);
            maxp    = rd32(body + 72);
            /* Play duration counts the preroll the encoder padded in front;
             * ffmpeg reports the difference and so does the XWB dumper, so the
             * two duration columns can be compared without a fudge factor. */
            f->duration = (double)play / 1e7 - (double)preroll / 1e3;
            if (f->duration < 0.0) f->duration = 0.0;
            if (minp != maxp) {
                /* Every packet on this disc is fixed size; a variable-size ASF
                 * would need the per-packet length honoured below, which the
                 * grammar does support, but nothing here exercises it and an
                 * untested path is worse than a refusal. */
                ERR("variable ASF packet size (%u..%u) is unsupported", minp, maxp);
                return -1;
            }
            f->wfx.packet_size = minp;
            have_props = 1;
        } else if (memcmp(g, GUID_STREAM_PROPS, 16) == 0) {
            uint32_t tsdlen;
            uint16_t flags;
            const uint8_t *ts;
            if (blen < 54) { ERR("Stream Properties object truncated"); return -1; }
            tsdlen = rd32(body + 40);
            flags  = rd16(body + 48);
            ts     = body + 54;
            if (memcmp(body, GUID_AUDIO_MEDIA, 16) != 0)
                goto next;              /* video or a script stream; skip it */
            if (have_audio)
                goto next;              /* first audio stream wins, as ffmpeg's does */
            if (tsdlen < 18 || (size_t)(ts - body) + tsdlen > blen) {
                ERR("audio Stream Properties has no WAVEFORMATEX");
                return -1;
            }
            f->wfx.audiostream   = flags & 0x7f;
            f->wfx.codec_id      = rd16(ts + 0);
            f->wfx.channels      = rd16(ts + 2);
            f->wfx.rate          = rd32(ts + 4);
            f->wfx.bitrate       = rd32(ts + 8) * 8u;
            f->wfx.blockalign    = rd16(ts + 12);
            f->wfx.bitspersample = rd16(ts + 14);
            f->wfx.datalen       = rd16(ts + 16);
            if (f->wfx.datalen > sizeof f->wfx.data)
                f->wfx.datalen = sizeof f->wfx.data;
            if ((size_t)18 + f->wfx.datalen > tsdlen)
                f->wfx.datalen = (uint16_t)(tsdlen - 18);
            memcpy(f->wfx.data, ts + 18, f->wfx.datalen);
            have_audio = 1;
        }
next:
        pos += (size_t)osize;
    }

    if (!have_props) { ERR("ASF has no File Properties object");   return -1; }
    if (!have_audio) { ERR("ASF has no audio stream");             return -1; }

    /* The Data Object follows the header object.  Its own header is 50 bytes:
     * GUID(16) size(8) fileID(16) totalPackets(8) reserved(2). */
    if (hdr_size + 50 > n || memcmp(d + hdr_size, GUID_DATA, 16) != 0) {
        ERR("ASF Data Object missing after the header");
        return -1;
    }
    {
        uint64_t dsize = rd64(d + hdr_size + 16);
        f->data_off = (size_t)hdr_size + 50;
        f->data_end = n;
        if (dsize >= 50 && hdr_size + dsize <= n)
            f->data_end = (size_t)hdr_size + (size_t)dsize;
    }
    if (f->wfx.packet_size == 0) { ERR("ASF packet size is zero"); return -1; }
    return 0;
}

/* --------------------------------------------------------- one ASF packet -- */
/* Locate the audio payload inside packet [p, p+packet_size).  On success sets
 * the out pointer and out length to a run of bytes inside `scratch` (which
 * must hold packet_size
 * bytes and is written in place when payloads have to be coalesced).
 * Returns 1 = audio found, 0 = packet carried none, -1 = malformed. */
static int asf_packet_payload(uint8_t *pk, uint32_t packet_size,
                              int audiostream, uint8_t **out, uint32_t *outn)
{
    uint32_t bytesread = 0, length, padding = 0;
    uint8_t  flags, prop, ecb;
    const uint8_t *hp;
    uint32_t hlen;
    unsigned payload_count, plen_type;
    unsigned i;
    uint8_t *datap;

    *out = NULL;
    *outn = 0;
    if (packet_size < 8)
        return -1;

    ecb = pk[0];
    bytesread = 1;
    if (ecb & 0x80) {
        unsigned ec_len = ecb & 0x0f;
        /* Rockbox refuses anything but the canonical {type 0, no opaque data,
         * 2 bytes}; so does this, for the same reason -- the alternatives are
         * unexercised here and a silent misparse desynchronises every
         * subsequent packet rather than failing loudly. */
        if (((ecb >> 5) & 3) != 0 || ((ecb >> 4) & 1) != 0 || ec_len != 2)
            return -1;
        bytesread += ec_len;
    } else {
        /* No error-correction byte: what we read was the length-type flags. */
        bytesread = 0;
    }
    if (bytesread + 2 > packet_size)
        return -1;
    flags = pk[bytesread];
    prop  = pk[bytesread + 1];
    bytesread += 2;

    hlen = len2b((flags >> 1) & 3) + len2b((flags >> 3) & 3) +
           len2b((flags >> 5) & 3) + 6u;
    if (bytesread + hlen > packet_size)
        return -1;
    hp = pk + bytesread;
    bytesread += hlen;

    length = val2b((flags >> 5) & 3, hp);
    hp += len2b((flags >> 5) & 3);
    hp += len2b((flags >> 1) & 3);              /* sequence, unused */
    padding = val2b((flags >> 3) & 3, hp);
    /* send time (4) + duration (2) close out the parsing information. */

    /* "Packet length can (and often will) be undefined and we just have to use
     * the header packet size" -- Rockbox's words, and the file's behaviour. */
    if (!((flags >> 5) & 3))
        length = packet_size;
    if (length < packet_size) {
        padding += packet_size - length;
        length = packet_size;
    }
    if (length > packet_size)
        return -1;

    if (flags & 0x01) {
        if (bytesread + 1 > packet_size)
            return -1;
        payload_count = pk[bytesread] & 0x3f;
        plen_type     = (pk[bytesread] >> 6) & 3;
        bytesread++;
    } else {
        payload_count = 1;
        plen_type     = 2;                      /* unused in the single case */
    }
    if (length < bytesread)
        return -1;

    datap = pk + bytesread;
    for (i = 0; i < payload_count; i++) {
        unsigned stream_id;
        uint32_t hdrlen, replicated, datalen;

        if ((size_t)(datap - pk) + 1 > packet_size)
            return -1;
        stream_id = datap[0] & 0x7f;
        datap++;
        bytesread++;

        hdrlen = len2b(prop & 3) + len2b((prop >> 2) & 3) + len2b((prop >> 4) & 3);
        if ((size_t)(datap - pk) + hdrlen > packet_size)
            return -1;
        datap += len2b((prop >> 4) & 3);        /* media object number */
        datap += len2b((prop >> 2) & 3);        /* offset into media object */
        replicated = val2b(prop & 3, datap);
        datap += len2b(prop & 3);
        bytesread += hdrlen;

        if ((size_t)(datap - pk) + replicated > packet_size)
            return -1;
        datap     += replicated;
        bytesread += replicated;

        if (flags & 0x01) {
            if (len2b(plen_type) != 2)
                return -1;                      /* the spec pins this to a word */
            if ((size_t)(datap - pk) + 2 > packet_size)
                return -1;
            datalen = val2b(plen_type, datap);
            datap     += 2;
            bytesread += 2;
        } else {
            if (length < bytesread + padding)
                return -1;
            datalen = length - bytesread - padding;
        }
        /* A single replicated byte is the "compressed payload" form, where
         * that byte is a presentation-time delta rather than payload data. */
        if (replicated == 1) {
            if ((size_t)(datap - pk) + 1 > packet_size)
                return -1;
            datap++;
        }
        if ((size_t)(datap - pk) + datalen > packet_size)
            return -1;

        if ((int)stream_id == audiostream) {
            if (*out == NULL) {
                *out  = datap;                  /* first payload stays put */
                *outn = datalen;
            } else {
                /* Later audio payloads in the same packet are moved up against
                 * the first so libwma sees one contiguous superframe.  This
                 * writes into the caller's scratch copy, never into the source
                 * image -- which is why `pk` is a mutable copy of the packet. */
                memmove(*out + *outn, datap, datalen);
                *outn += datalen;
            }
        }
        datap     += datalen;
        bytesread += datalen;
    }
    return *out ? 1 : 0;
}

/* -------------------------------------------------------------- the API --- */

int b3_wma_available(void) { return 1; }

int b3_wma_probe(const void *asf, size_t asf_len, b3_wma_info *info,
                 char *err, size_t errsz)
{
    b3_asf_file f;

    if (err && errsz) err[0] = '\0';
    if (!asf || asf_len == 0) { ERR("empty payload"); return -1; }
    if (asf_parse_header((const uint8_t *)asf, asf_len, &f, err, errsz) != 0)
        return -1;
    if (info) {
        info->codec_id    = f.wfx.codec_id;
        info->rate        = f.wfx.rate;
        info->channels    = f.wfx.channels;
        info->bitrate     = f.wfx.bitrate;
        info->block_align = f.wfx.blockalign;
        info->packets     = f.wfx.numpackets;
        info->duration    = f.duration;
    }
    return 0;
}

int b3_wma_decode(const void *asf, size_t asf_len,
                  int16_t **out, size_t *out_frames, b3_wma_info *info,
                  char *err, size_t errsz)
{
    b3_asf_file      f;
    WMADecodeContext ctx;
    const uint8_t   *d = (const uint8_t *)asf;
    uint8_t         *scratch = NULL;
    int16_t         *pcm = NULL;
    size_t           cap = 0, nframes = 0;
    size_t           pos;
    unsigned         ch;
    int              rc = -1;

    if (err && errsz) err[0] = '\0';
    if (out) *out = NULL;
    if (out_frames) *out_frames = 0;
    if (!asf || !out || !out_frames) { ERR("bad arguments"); return -1; }

    if (asf_parse_header(d, asf_len, &f, err, errsz) != 0)
        return -1;
    if (info) {
        info->codec_id    = f.wfx.codec_id;
        info->rate        = f.wfx.rate;
        info->channels    = f.wfx.channels;
        info->bitrate     = f.wfx.bitrate;
        info->block_align = f.wfx.blockalign;
        info->packets     = f.wfx.numpackets;
        info->duration    = f.duration;
    }
    if (f.wfx.codec_id != ASF_CODEC_ID_WMAV1 &&
        f.wfx.codec_id != ASF_CODEC_ID_WMAV2) {
        /* WMA Pro (0x162), Lossless (0x163) and Voice (0x00A) are different
         * codecs that share a container.  Nothing on this disc is one of them
         * -- all 885 payloads probe as 0x161 -- so this is a guard against a
         * future input, and it names the tag so the report is actionable. */
        ERR("codec 0x%03X is not WMA v1/v2 (this decoder handles those only)",
            (unsigned)f.wfx.codec_id);
        return -1;
    }
    if (f.wfx.channels == 0 || f.wfx.channels > MAX_CHANNELS) {
        ERR("%u channels; libwma is built for at most %d",
            (unsigned)f.wfx.channels, MAX_CHANNELS);
        return -1;
    }
    if (f.wfx.packet_size > (1u << 20)) {
        ERR("ASF packet size %u is implausible", (unsigned)f.wfx.packet_size);
        return -1;
    }

    memset(&ctx, 0, sizeof ctx);
    if (wma_decode_init(&ctx, &f.wfx) < 0) {
        ERR("wma_decode_init rejected the stream");
        return -1;
    }

    scratch = (uint8_t *)malloc(f.wfx.packet_size);
    if (!scratch) { ERR("out of memory"); return -1; }

    for (pos = f.data_off; pos + f.wfx.packet_size <= f.data_end;
         pos += f.wfx.packet_size) {
        uint8_t *payload = NULL;
        uint32_t plen = 0;
        int      got, i;

        /* The packet is copied because coalescing multiple payloads rewrites
         * it in place, and the source may be a read-only mapping of the disc
         * image -- cx_src_map() hands out exactly that. */
        memcpy(scratch, d + pos, f.wfx.packet_size);
        got = asf_packet_payload(scratch, f.wfx.packet_size,
                                 f.wfx.audiostream, &payload, &plen);
        if (got < 0) {
            /* A malformed packet is survivable: skip it and keep the stream
             * going.  A run of them will show up as a short duration, which
             * the callers already check against the ASF header's own. */
            continue;
        }
        if (got == 0 || plen == 0)
            continue;
        if (plen > MAX_CODED_SUPERFRAME_SIZE)
            continue;

        wma_decode_superframe_init(&ctx, payload, (int)plen);
        for (i = 0; i < ctx.nb_frames; i++) {
            int n = wma_decode_superframe_frame(&ctx, payload, (int)plen);
            size_t k;
            if (n <= 0)
                break;      /* libwma has reset its bit reservoir; next packet */

            if (nframes + (size_t)n > cap) {
                size_t ncap = cap ? cap * 2 : (size_t)f.wfx.rate * 8;
                int16_t *np;
                while (ncap < nframes + (size_t)n)
                    ncap *= 2;
                np = (int16_t *)realloc(pcm, ncap * f.wfx.channels * sizeof *pcm);
                if (!np) { ERR("out of memory"); goto done; }
                pcm = np;
                cap = ncap;
            }
            for (k = 0; k < (size_t)n; k++) {
                for (ch = 0; ch < f.wfx.channels; ch++) {
                    /* Negated: see the polarity note at the top of this file. */
                    int32_t v = -((*ctx.frame_out)[ch][k] >> B3_WMA_OUT_SHIFT);
                    if (v >  32767) v =  32767;
                    if (v < -32768) v = -32768;
                    pcm[(nframes + k) * f.wfx.channels + ch] = (int16_t)v;
                }
            }
            nframes += (size_t)n;
        }
    }

    if (nframes == 0) {
        ERR("decoded no audio (%llu packets in the header)",
            (unsigned long long)f.wfx.numpackets);
        goto done;
    }
    /* Drop the priming -- see the DELAY note at the top of this file.  Moving
     * the tail down rather than returning an offset keeps the ownership story
     * simple: the caller gets one malloc'd block it can free(). */
    {
        size_t prime = (size_t)B3_WMA_PRIME_FRAMES * (size_t)ctx.frame_len;
        if (prime >= nframes) {
            ERR("stream is shorter than the decoder's own priming (%zu frames)",
                nframes);
            goto done;
        }
        nframes -= prime;
        memmove(pcm, pcm + prime * f.wfx.channels,
                nframes * f.wfx.channels * sizeof *pcm);
    }
    *out = pcm;
    *out_frames = nframes;
    pcm = NULL;
    rc = 0;

done:
    free(scratch);
    free(pcm);
    return rc;
}

size_t b3_wma_downmix_mono(int16_t *pcm, size_t frames, unsigned channels)
{
    size_t i;

    if (!pcm || channels <= 1)
        return frames;
    for (i = 0; i < frames; i++) {
        int32_t acc = 0;
        unsigned c;
        for (c = 0; c < channels; c++)
            acc += pcm[i * channels + c];
        /* Plain average, matching swresample's default downmix matrix for the
         * stereo case (0.5/0.5).  Rounded away from zero so the fold does not
         * introduce a DC offset on quiet passages. */
        acc = acc >= 0 ? (acc + (int32_t)channels / 2) / (int32_t)channels
                       : (acc - (int32_t)channels / 2) / (int32_t)channels;
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        pcm[i] = (int16_t)acc;
    }
    return frames;
}

int16_t *b3_wma_resample(const int16_t *pcm, size_t frames, unsigned channels,
                         uint32_t from_rate, uint32_t to_rate,
                         size_t *out_frames)
{
    size_t n, i;
    int16_t *out;

    if (out_frames) *out_frames = 0;
    if (!pcm || !channels || !from_rate || !to_rate)
        return NULL;
    if (from_rate == to_rate) {
        out = (int16_t *)malloc(frames * channels * sizeof *out);
        if (!out) return NULL;
        memcpy(out, pcm, frames * channels * sizeof *out);
        if (out_frames) *out_frames = frames;
        return out;
    }
    n = (size_t)((double)frames * (double)to_rate / (double)from_rate);
    if (n == 0) n = 1;
    out = (int16_t *)malloc(n * channels * sizeof *out);
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        double  src = (double)i * (double)from_rate / (double)to_rate;
        size_t  i0  = (size_t)src;
        size_t  i1  = i0 + 1 < frames ? i0 + 1 : (frames ? frames - 1 : 0);
        double  t   = src - (double)i0;
        unsigned c;
        if (i0 >= frames) i0 = frames ? frames - 1 : 0;
        for (c = 0; c < channels; c++) {
            double a = pcm[i0 * channels + c], b = pcm[i1 * channels + c];
            double v = a + (b - a) * t;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[i * channels + c] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
    }
    if (out_frames) *out_frames = n;
    return out;
}
