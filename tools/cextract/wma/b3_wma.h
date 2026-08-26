/* tools/cextract/wma/b3_wma.h -- IN-PROCESS WMA, the whole public surface.
 *
 * One call: hand it the bytes of a complete ASF/WMA file, get interleaved
 * signed 16-bit PCM back.  That is the entire contract, and it is deliberately
 * this small because it has to hold on three targets at once -- the desktop
 * binary, the Android .so, and a wasm module inside a browser tab -- and
 * because the thing on the other side of it is a GAME FRAME, not a shell.
 *
 * WHAT THIS REPLACES.  tools/cextract/cx_audio_xwb.c and cx_audio_eatrax.c
 * used to write the carved ASF payload to a temporary file and run
 *
 *     ffmpeg -v error -y -i <tmp>.wma -ac 1 -ar 44100 -acodec pcm_s16le <dst>
 *
 * which is why both stages sat in the Makefile's CX_SKIP and were NOT linked
 * into the game: fork/exec of a host binary is not something a game process
 * does, and on the web there is no process to fork.  Music therefore existed
 * only for someone who had run the extractor by hand, and never existed at all
 * in a browser.  With this header those two stages become ordinary in-process
 * stages like every other one, and build/music/track_NN.wav materialises out
 * of the disc on demand exactly the way track geometry and car paint do.
 *
 * WHY NOT WebCodecs ON THE WEB.  The browser decode API's codec registry is a
 * closed list, and WMA is not on it -- no shipping engine exposes 'wmav2'.
 * The same C compiled to wasm is the only route, which is the reason the
 * decoder underneath is fixed-point with no allocator and no toolchain of its
 * own (see tools/fetch_wma.sh for the codec identification and the licensing).
 *
 * FAILURE IS A VALUE, NEVER A CRASH.  Every entry point returns a code and
 * fills a caller-owned error string.  A truncated payload, a WMA Pro stream,
 * an ASF with no audio stream: all of them are a non-zero return and a
 * sentence, because the caller is sometimes a batch extractor printing a
 * status column and sometimes a game deciding whether track 12 is playable.
 */
#ifndef B3_WMA_H
#define B3_WMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the stream turned out to be.  Filled even when decoding then fails, so
 * a caller can report "WMA Pro, unsupported" rather than "error". */
typedef struct {
    uint32_t codec_id;      /* 0x161 = WMAv2.  Every payload on this disc.   */
    uint32_t rate;          /* 44100 (EA TRAX, Movie) or 48000 (DJ banks)    */
    uint32_t channels;      /* 2 in every entry                              */
    uint32_t bitrate;       /* bits/s, 160000 throughout                     */
    uint32_t block_align;   /* 7431 @44.1k, 6827 @48k -- one WMA superframe  */
    uint64_t packets;       /* ASF data packets, from the File Properties obj */
    double   duration;      /* seconds, play duration minus preroll          */
} b3_wma_info;

/* 1 when a real decoder is linked, 0 when this build got the stub.
 * The stub exists so `make` succeeds on a checkout that has not run
 * tools/fetch_wma.sh; see b3_wma_stub.c. */
int b3_wma_available(void);

/* Probe the ASF header only.  Cheap -- it never touches the data packets --
 * so a caller can ask "what is this?" without paying for a 3-minute decode.
 * 0 on success. */
int b3_wma_probe(const void *asf, size_t asf_len, b3_wma_info *info,
                 char *err, size_t errsz);

/* Decode the whole file.  On success *out is a malloc'd block of
 * (*out_frames * info->channels) interleaved int16 samples -- the caller owns
 * it and frees it with free().  0 on success, non-zero with *err set.
 *
 * `info` may be NULL.  On failure *out is NULL and *out_frames is 0. */
int b3_wma_decode(const void *asf, size_t asf_len,
                  int16_t **out, size_t *out_frames, b3_wma_info *info,
                  char *err, size_t errsz);

/* Fold interleaved `channels`-channel s16 down to mono, in place, returning
 * the frame count (unchanged).  This is ffmpeg's default downmix for the
 * stereo case -- a plain average of the channels -- and it is here rather than
 * in the callers because BOTH music stages need it and getting it subtly
 * different in two places is how an A/B against the oracle turns into a
 * mystery.  Mono in, nothing happens. */
size_t b3_wma_downmix_mono(int16_t *pcm, size_t frames, unsigned channels);

/* Linear resample interleaved s16 from `from_rate` to `to_rate`.  Returns a
 * malloc'd block and its frame count, or NULL.  Only reached when someone sets
 * B3_EATRAX_RATE away from the disc's own 44100; the default path never
 * resamples and is therefore never compared against ffmpeg's much better
 * resampler.  Documented so nobody reads an SNR number taken through it as a
 * statement about the decoder. */
int16_t *b3_wma_resample(const int16_t *pcm, size_t frames, unsigned channels,
                         uint32_t from_rate, uint32_t to_rate,
                         size_t *out_frames);

#ifdef __cplusplus
}
#endif

#endif /* B3_WMA_H */
