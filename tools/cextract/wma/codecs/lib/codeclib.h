/* tools/cextract/wma/codecs/lib/codeclib.h -- Rockbox's codec support library,
 * cut down to what the WMA decoder actually reaches for.
 *
 * WHY A REPLACEMENT AND NOT THE REAL ONE.  Rockbox's codeclib.h is a header
 * with side effects: it does
 *
 *     #define malloc(x)  codec_malloc(x)
 *     #define free(x)    codec_free(x)
 *     #define strlen(s)  codec_strlen(s)
 *
 * and codeclib.c implements those against `ci->`, the playback engine's
 * callback table.  Pulling it in would mean pulling in the engine, and -- much
 * worse -- those macros are TEXTUAL: this header is on the include path of
 * every file compiled with the decoder, so a stray malloc() in our own code
 * would silently retarget.  So codeclib.c is not fetched, and this file
 * supplies the small set of declarations the fetched sources genuinely use.
 *
 * That set was enumerated, not guessed:
 *
 *     grep -oE "\b(av_log2|codec_[a-z]+|ff_[a-z_0-9]+|fsincos)\b" over the
 *     fetched set -- codecs/libwma (both .c files) and codecs/lib/{mdct,
 *     fft-ffmpeg, mdct_lookup, ffmpeg_bitstream}.c
 *
 * yields av_log2, fsincos, and the ff_* MDCT/FFT entry points -- and no
 * codec_malloc, no codec_strlen, no ci->.  libwma allocates NOTHING at run
 * time: every buffer in WMADecodeContext is a file-scope static in wmadeci.c
 * (coefsarray, frame_out_buf, the five window tables, the four VLC tables).
 * That is why this decoder can be handed a 7431-byte packet and asked for PCM
 * with no allocator in sight, which is exactly what a wasm worklet wants.
 */
#ifndef B3_WMA_CODECLIB_H
#define B3_WMA_CODECLIB_H

#include "platform.h"
#include "codecs.h"
#include "mdct.h"
#include "fft.h"

/* ---- av_log2 ----------------------------------------------------------- */
/* Rockbox reaches for a CLZ instruction or a 256-entry lookup, both wrapped in
 * bs_generic().  Every compiler this port uses has __builtin_clz, and the one
 * behaviour that matters is the edge case Rockbox's BS_0_0 personality exists
 * to pin down: av_log2(0) must be 0, not undefined.  __builtin_clz(0) IS
 * undefined, so the zero is branched out rather than shifted through. */
static inline unsigned int b3_av_log2(unsigned int v)
{
    return v ? (31u - (unsigned int)__builtin_clz(v)) : 0u;
}
#define av_log2(v) b3_av_log2(v)

/* ---- the MDCT / FFT the decoder calls into ----------------------------- */
/* Defined by third_party/rockbox/codecs/lib/{mdct,fft-ffmpeg,mdct_lookup}.c,
 * which are fetched.  ff_imdct_calc is the one wmadeci.c uses per block. */
extern void ff_imdct_half(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_imdct_calc(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_fft_calc_c(int nbits, FFTComplex *z);

#endif /* B3_WMA_CODECLIB_H */
