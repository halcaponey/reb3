/* tools/cextract/wma/codecs.h -- the Rockbox CODEC API, absent on purpose.
 *
 * In Rockbox a codec is a loadable plugin that talks to the playback engine
 * through `struct codec_api *ci` -- ci->read_filebuf, ci->pcmbuf_insert,
 * ci->yield, ci->set_elapsed.  None of that exists here, and none of it is
 * needed: the files this project vendors are the DECODER, not the codec
 * plugin.  Rockbox's own lib/rbcodec/codecs/wma.c is the plugin, and it is
 * deliberately NOT fetched -- tools/cextract/wma/b3_wma.c is this project's
 * replacement for it, driving the same three entry points
 *
 *     wma_decode_init(ctx, wfx)
 *     wma_decode_superframe_init(ctx, buf, len)
 *     wma_decode_superframe_frame(ctx, buf, len)
 *
 * over an in-memory ASF file instead of over a streaming file buffer.
 *
 * Verified rather than assumed: grepping for "ci->" across the fetched set --
 * codecs/libwma (both .c files) and codecs/lib/{mdct, fft-ffmpeg, mdct_lookup,
 * ffmpeg_bitstream}.c -- returns NOTHING.  Every ci-> reference in Rockbox's
 * codec tree lives in codeclib.c and the plugins, neither of which is fetched.
 * So this header exists only to satisfy `#include <codecs.h>` and to carry the
 * two byte-swap helpers ffmpeg_bswap.h reaches for.
 */
#ifndef B3_WMA_CODECS_H
#define B3_WMA_CODECS_H

#include "platform.h"

/* ffmpeg_bswap.h asks for these by Rockbox's names. */
static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t swap32(uint32_t v)
{
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
}

#endif /* B3_WMA_CODECS_H */
