/* tools/cextract/wma/platform.h -- THE ROCKBOX FIRMWARE, REPLACED BY NOTHING.
 *
 * third_party/rockbox/ is fetched VERBATIM (tools/fetch_wma.sh) and never
 * edited: it is LGPL code whose separability is the whole licensing story, and
 * a local patch is a merge conflict waiting for the next pin bump.  It expects
 * to be compiled inside Rockbox, so it reaches for two firmware headers --
 * <platform.h> and <codecs.h> -- and for <codecs/lib/codeclib.h>, the codec
 * support library.  This directory supplies all three, and the Makefile puts
 * it AHEAD of third_party/ on the include path so these win.
 *
 * What Rockbox wants from platform.h, measured by compiling until it stopped
 * complaining rather than by guessing:
 *
 *   * the IRAM placement attributes.  On a 2005 DAP with 96 KB of on-chip RAM
 *     these decide what runs fast; on a desktop, a phone and a wasm heap there
 *     is no such distinction and they are all empty.  MEM_ALIGN_ATTR is NOT
 *     empty -- the FFT reads coefficient arrays in pairs and the fixed-point
 *     kernels assume 16-byte alignment.
 *   * CONFIG_CPU and the CPU_* family, left UNSET so every `#if defined(...)`
 *     assembly path in asm_arm.h / fft-ffmpeg_arm.h / asm_mcf5249.h compiles
 *     out and the portable C runs.  CONFIG_CPU is defined to a value that
 *     matches no target on purpose: `#if (CONFIG_CPU == PP5022)` with the name
 *     undefined is `0 == 0`, which is TRUE, and would silently select an IRAM
 *     variant meant for a different chip.
 *   * DEBUGF, which Rockbox routes to a serial console.  Here it is a
 *     compiled-out varargs sink -- but a REAL one, not `#define DEBUGF(...)`,
 *     so the argument expressions are still type-checked.
 */
#ifndef B3_WMA_PLATFORM_H
#define B3_WMA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- CPU identification: name every target, select none of them -------- */
#define PP5020        5020
#define PP5022        5022
#define PP5024        5024
#define MCF5249       5249
#define MCF5250       5250
#define S5L8700       8700
#define S5L8701       8701
#define S5L8702       8702
#define CONFIG_CPU    0          /* matches nothing above -- see the note */

/* CPU_ARM / CPU_COLDFIRE / CPU_S5L87XX are deliberately NOT defined. */

/* ---- IRAM placement: meaningless off a DAP ----------------------------- */
#define ICODE_ATTR
#define IDATA_ATTR
#define IBSS_ATTR
#define ICONST_ATTR
#define NO_PROF_ATTR
#define ICODE_ATTR_TREMOR_MDCT
#define ATTRIBUTE_ALIGNED(n)  __attribute__((aligned(n)))
#define MEM_ALIGN_ATTR        __attribute__((aligned(16)))

/* ---- endianness -------------------------------------------------------- */
/* Every target this port builds for -- x86-64, aarch64 Android, wasm32 -- is
 * little-endian.  Say so rather than probing: a wrong answer here is silent
 * garbage audio, and there is no big-endian target to be wrong about. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  error "third_party/rockbox libwma is wired for little-endian here"
#endif
#define ROCKBOX_LITTLE_ENDIAN 1

/* ---- branch hints ------------------------------------------------------ */
/* mdct.c and fft-ffmpeg.c mark their hot-loop branches.  Rockbox defines these
 * in firmware/export/system.h, which is not fetched. */
#ifndef LIKELY
#define LIKELY(x)    __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#endif

/* ---- diagnostics ------------------------------------------------------- */
/* Type-checked and discarded.  b3_wma.c reports real failures through its own
 * return codes; a decoder that chatters on stderr inside a game frame is worse
 * than one that is quiet. */
static inline void b3_wma_debugf_sink(const char *fmt, ...) { (void)fmt; }
#define DEBUGF   b3_wma_debugf_sink
#define LOGF     b3_wma_debugf_sink

#endif /* B3_WMA_PLATFORM_H */
