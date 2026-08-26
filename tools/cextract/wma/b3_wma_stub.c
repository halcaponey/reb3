/* tools/cextract/wma/b3_wma_stub.c -- the same API, with no decoder behind it.
 *
 * third_party/ is gitignored and fetched (tools/fetch_wma.sh), so a fresh
 * clone does not have a WMA decoder in it.  The Makefile detects that and
 * links this file instead of b3_wma.c, which keeps `make` working on a
 * checkout that has never run the fetch script -- the same property the build
 * had when the music stages shelled out to ffmpeg and ffmpeg was not installed.
 *
 * WHAT A STUB BUYS THAT AN #ifdef DOES NOT.  The alternative is to keep
 * cx_audio_xwb.c and cx_audio_eatrax.c out of the link, which is exactly the
 * CX_SKIP arrangement this work exists to remove.  With a stub, the two stages
 * are ALWAYS compiled and ALWAYS linked -- into the extractor, into the game,
 * into the wasm module -- and the only thing that varies is whether the decode
 * call succeeds.  So the failure mode is a stage that runs and reports a
 * reason, not a symbol that is missing from half the builds and a stage table
 * that is a different shape depending on how the tree was checked out.
 *
 * Every entry point reports the same actionable sentence, because "no decoder"
 * is a setup problem with one fix and the user should be told it. */
#include "b3_wma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char NOPE[] =
    "no WMA decoder in this build -- run `sh tools/fetch_wma.sh`, then rebuild";

int b3_wma_available(void) { return 0; }

int b3_wma_probe(const void *asf, size_t asf_len, b3_wma_info *info,
                 char *err, size_t errsz)
{
    (void)asf; (void)asf_len;
    if (info) memset(info, 0, sizeof *info);
    if (err && errsz) snprintf(err, errsz, "%s", NOPE);
    return -1;
}

int b3_wma_decode(const void *asf, size_t asf_len,
                  int16_t **out, size_t *out_frames, b3_wma_info *info,
                  char *err, size_t errsz)
{
    (void)asf; (void)asf_len;
    if (out) *out = NULL;
    if (out_frames) *out_frames = 0;
    if (info) memset(info, 0, sizeof *info);
    if (err && errsz) snprintf(err, errsz, "%s", NOPE);
    return -1;
}

/* These two are pure PCM arithmetic with no dependency on the decoder, so they
 * are REAL here rather than stubbed.  A caller that got its samples from the
 * B3_FFMPEG=1 arm still needs them to work. */
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
    (void)pcm; (void)frames; (void)channels; (void)from_rate; (void)to_rate;
    if (out_frames) *out_frames = 0;
    return NULL;
}
