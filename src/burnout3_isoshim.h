/* burnout3_isoshim.h -- the one seam, force-included into every src/ TU.
 *
 * The Makefile compiles src/ with `-include src/burnout3_isoshim.h`, so this
 * lands at the top of every translation unit before its own first #include.
 * That is the whole point: NO LOADER HAD TO BE EDITED.  Every one of them
 * still opens the literal "build/..." path it always did; the three libc /
 * SDL entry points they open it THROUGH are redirected here, once.
 *
 * In build mode each redirect is a straight pass-through, so the pre-extracted
 * tree behaves byte for byte as it did before this file existed.
 *
 * WHY THE INCLUDES BELOW COME FIRST: fopen and access are declared as
 * FUNCTIONS in <stdio.h> and <unistd.h>, and a function-like macro of the same
 * name would expand inside those declarations and wreck them.  Pulling the
 * three headers in before defining anything means the declarations are already
 * parsed by the time the names are taken over -- and all three are
 * include-guarded, so the TU's own copies are no-ops.
 *
 * The IMG_Load redirect is deliberately self-referential: C does not re-expand
 * a macro inside its own expansion, so `IMG_Load(b3_iso_resolve(p))` calls the
 * real function with a resolved path.
 *
 * ================================================= THE FOURTH ENTRY POINT ====
 * SDL_LoadWAV was the hole in this seam, and it was a silent one.
 *
 * Everything else in the audio path opens its waves with fopen -- b3_sfx.c's
 * event banks, b3_music.c's EA TRAX and crash beds -- so all of it came
 * through the resolver and worked off the disc.  The FOUR ENGINE LOOPS and the
 * SEVEN FRONT-END CUES in src/burnout3_full.c are the only waves loaded with
 * SDL_LoadWAV, and SDL_LoadWAV does its own file I/O inside libSDL2: the
 * `fopen` macro above lands in src/ translation units only, and SDL is not one
 * of them.  So those eleven waves asked the FILESYSTEM for a literal
 * "build/audio/..." path that iso mode -- the default -- never creates.
 *
 * MEASURED, both arms, iso mode with a fully populated cache:
 *     no literal build/audio/ present : "REAL audio: 0 engine loops"
 *     build/audio -> the cache's copy : "REAL audio: 4 engine loops"
 * A checkout still carrying a legacy pre-extracted tree had sound and did not
 * know why; a clean disc-only boot, and every web boot, had none.
 *
 * The redirect is spelled as SDL's own macro is (SDL_audio.h), with the path
 * resolved on the way into SDL_RWFromFile.  In build mode b3_iso_resolve()
 * returns its argument, so the pre-extracted tree behaves byte for byte as it
 * did.
 */
/* THE FEATURE-TEST MACROS COME FIRST, AND THEY ARE NOT OPTIONAL.  Being
 * force-included makes this the first thing every TU sees, ahead of the
 * `#define _DEFAULT_SOURCE 1` burnout3_full.c opens with -- and under
 * -std=c11 glibc hides setenv/strdup/symlink behind exactly those macros, so
 * without these two lines the first #include below would freeze the strict-ISO
 * view of <stdio.h> in place and every POSIX call in src/ would lose its
 * declaration.  The spellings match the ones the TUs use (_GNU_SOURCE empty as
 * in burnout3_emu.c, _DEFAULT_SOURCE 1 as in burnout3_full.c) so their own
 * later #defines are identical redefinitions and stay silent. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#ifndef BURNOUT3_ISOSHIM_H
#define BURNOUT3_ISOSHIM_H

#include <stdio.h>
#include <unistd.h>
#include <SDL2/SDL_image.h>

#include "burnout3_isodata.h"

#define fopen(p, m)   b3_iso_fopen((p), (m))
#define access(p, m)  b3_iso_access((p), (m))
#define IMG_Load(p)   IMG_Load(b3_iso_resolve(p))

/* SDL_LoadWAV is a MACRO in SDL_audio.h, not a function, so it is replaced
 * rather than wrapped -- same expansion, resolved path. */
#undef  SDL_LoadWAV
#define SDL_LoadWAV(file, spec, audio_buf, audio_len)                        \
        SDL_LoadWAV_RW(SDL_RWFromFile(b3_iso_resolve(file), "rb"), 1,        \
                       (spec), (audio_buf), (audio_len))

#endif /* BURNOUT3_ISOSHIM_H */
