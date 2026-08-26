/* burnout3_isodata.h -- THE DATA MODEL: where the port's assets come from.
 *
 * The port used to have exactly one answer: a pre-extracted `build/` tree that
 * somebody had to produce, out of band, with tools/cextract or the python
 * originals, before the game would boot at all.  That is now the DEBUG path.
 * The default is the user's own Xbox disc: the game reads the ISO (or an
 * expanded dump of it) and materialises what it needs, when it needs it.
 *
 * ================================================================== THE MODE
 *   ./burnout3                 iso mode, image auto-resolved   (THE DEFAULT)
 *   ./burnout3 --iso=<path>    iso mode, this image/dump
 *   ./burnout3 --iso           iso mode, auto-resolved (explicit)
 *   ./burnout3 --build         the old pre-extracted build/ tree, unchanged
 *
 * Environment, for harnesses that cannot pass argv:
 *   B3_DATA_MODE=iso|build     same switch
 *   B3_ISO=<path>              same image/dump (implies iso unless
 *                              B3_DATA_MODE=build says otherwise)
 *   B3_ISO_CACHE=<dir>         where materialised assets land
 *                              (default <repo>/build/.isocache)
 *
 * The image path is resolved from, in order:
 *   1. --iso=<path>
 *   2. $B3_ISO
 *   3. build/iso_path.txt      one line, written on the first successful
 *                              --iso=<path> run, so it is remembered
 *   4. $B3_GAME_ROOT           the folder holding default.xbe, or an .xiso
 *                              image of it.  NO PATH IS COMPILED IN: this
 *                              tree ships no game content and cannot guess
 *                              where your own copy lives (docs/ASSETS.md).
 * A candidate is accepted when it exists -- a .iso FILE and an expanded dump
 * DIRECTORY are both valid sources; the cx_src layer under tools/cextract
 * auto-detects which.  When nothing resolves the port falls back to build/
 * mode with ONE notice line, so a debug tree with no disc still boots.
 *
 * ========================================================== MATERIALISE-ON-MISS
 * NO LOADER IN src/ CHANGED.  Every one of them still opens a literal
 * "build/..." path.  b3_iso_resolve() is the single seam: it maps that path to
 * the ISO CACHE, and when the cache copy is not there yet it runs the cextract
 * STAGE that produces it -- in process, against the disc -- and then returns
 * the cache path.  A path with no stage behind it (a config file, a debug
 * dump, a log) belongs to the GAME rather than to the disc, and both
 * directions answer it the same way: if the build/ TREE is there, the literal
 * build/ path is used, for the read as much as for the write, so what the game
 * wrote is what the game reads back.  Only a checkout with no build/ at all
 * puts these in the cache -- which is what keeps a disc-only tree able to
 * write its config somewhere, and read it again.
 *
 * WHICH ENTRY POINTS THE SEAM COVERS is the thing to check when a loader
 * "works on my machine and nowhere else".  src/burnout3_isoshim.h takes over
 * fopen, access, IMG_Load and SDL_LoadWAV, and that list is exhaustive on
 * purpose: a `#define fopen` reaches src/ translation units ONLY, so anything
 * that opens a file inside a LIBRARY is outside the seam.  SDL_LoadWAV was
 * outside it for a long time and the cost was exact -- the four engine loops
 * and the seven front-end cues asked the filesystem for a literal
 * "build/audio/..." that iso mode never creates, so a checkout carrying a
 * legacy pre-extracted tree had sound and a clean disc-only boot had none.
 * SDL_SaveBMP is still outside it and is meant to be (see the save-game note
 * further down); anything else added here must be added to the shim too.
 *
 * src/burnout3_isoshim.h is force-included into every src/ translation unit
 * (see the Makefile) and routes fopen / access / IMG_Load through here, which
 * is why the loaders did not have to be touched one by one.
 *
 * Cost: the global stages a boot touches (roster, car tuning, the font, the
 * frontend bank) run once, a few seconds in total; a track's own set runs at
 * race load, ~3 s.  Nothing is pre-extracted.
 *
 * ffmpeg: the XWB and EA TRAX audio stages shell out to ffmpeg for WMA and are
 * deliberately NOT linked into the game.  In iso mode a missing build/music
 * costs one line and the music module runs silent.
 */
#ifndef BURNOUT3_ISODATA_H
#define BURNOUT3_ISODATA_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define B3_DATA_BUILD 0
#define B3_DATA_ISO   1

/* The last-resort image source (design rule 1, step 4): the user's own dump,
 * named by $B3_GAME_ROOT.  A user-supplies-assets distribution has no shipped
 * default to fall back on -- see docs/ASSETS.md. */
#define B3_ISO_DEFAULT_ENV "B3_GAME_ROOT"

/* Call ONCE, as the first statement of main(), before any asset is touched.
 * Consumes --iso[=path] / --build / --data=<mode> from argv (leaving every
 * other argument alone) and settles the mode, the source and the cache. */
void b3_iso_init(int argc, char **argv);

int         b3_iso_mode(void);      /* B3_DATA_ISO or B3_DATA_BUILD */
const char *b3_iso_source(void);    /* the image/dump path, "" in build mode */
const char *b3_iso_cache(void);     /* the cache root,      "" in build mode */

/* THE SEAM.  Hand it any path a loader is about to open; it hands back the
 * path that loader should really open.  In build mode that is `path` itself,
 * byte for byte.  In iso mode a "build/..." path becomes its cache twin, with
 * the stage behind it run first if the twin is not there yet.
 *
 * The returned pointer is one of a small ring of internal buffers: fine to
 * pass straight into fopen/IMG_Load, not fine to hold across a dozen more
 * calls.  Anything that is not a "build/..." path is returned unchanged.
 *
 * MAIN THREAD ONLY, AND STILL IS.  The ring is unsynchronised, and so is the
 * stage runner behind it -- a cextract stage chdir()s into its output root
 * while it works.  Every asset open in this port is already on the main thread
 * (the audio thread reads a ring the main thread filled -- see the threading
 * note at the top of src/burnout3_music.c), and that has to stay true.
 *
 * There ARE worker threads in a load now -- the extraction stages walk their
 * per-item fleets on tools/cextract/cx_pool.h, and load_track_textures()
 * inflates a track's PNGs there -- and NONE of them calls this.  The rule they
 * keep is: THE MAIN THREAD RESOLVES, THE WORKER OPENS WHAT IT WAS HANDED.  A
 * worker only ever sees a path this function has already returned, which is
 * either an absolute cache path or (in build mode, or for a non-asset) the
 * literal one; both leave resolve_inner() at its first two tests without
 * touching the ring.  The threading is INSIDE a stage, above the shim -- the
 * stage runner, the chdir and the stamp bookkeeping are untouched. */
const char *b3_iso_resolve(const char *path);

/* WHERE A WRITE GOES.  Never runs a stage -- materialising a file's old
 * contents in order to overwrite them would be pointless -- and, because a
 * shimmed write is always the GAME's own output and never a stage's, hands
 * back the literal "build/..." path whenever the real build/ tree exists.
 * Only a tree with no build/ at all writes into the cache.  b3_iso_fopen()
 * uses this for every write mode; see THE WRITE RULE in burnout3_isodata.c. */
const char *b3_iso_resolve_nomat(const char *path);

/* The shim primitives (src/burnout3_isoshim.h maps the libc names onto
 * these).  In build mode each is a straight pass-through. */
FILE *b3_iso_fopen(const char *path, const char *mode);
int   b3_iso_access(const char *path, int mode);

/* IS THIS EVENT AVAILABLE?  The one question materialise-on-miss cannot
 * answer at the file layer, because asking it by opening the files IS the
 * extraction.  The track-select screen probes six artefacts for each of the
 * 36 events to decide which rows to draw; in iso mode that would extract
 * 1.5 GB and ~15 s of track meshes to paint a menu, none of which the player
 * asked for.  In iso mode availability instead means what it should mean --
 * THE DISC HAS THIS EVENT -- resolved out of the disc's own tlist for a few
 * microseconds, and the artefacts still materialise when a race actually
 * loads one.
 *
 * Returns 1 = yes, 0 = no, and -1 in build mode, meaning "not my question,
 * probe the build/ tree the way you always did". */
int   b3_iso_track_available(const char *track_id);

/* IS THIS SONG AVAILABLE?  The same question and the same trap, for EA TRAX.
 * b3_music_init() decides which of the 44 songs are playable by opening each
 * one; under materialise-on-miss that probe IS the extraction, and would
 * decode the whole 722 MB soundtrack at boot to answer a question about
 * whether the disc has two wave banks.  In iso mode the answer comes off the
 * banks themselves; the song is decoded when the shuffle reaches it.
 *
 * `song` is 0..43.  Returns 1 = yes, 0 = no, and -1 in build mode, meaning
 * "not my question, probe the build/ tree the way you always did". */
int   b3_iso_music_available(int song);

/* ------------------------------------------------- PROGRESS (additive seam)
 * Materialising a unit takes a while, and the CALLING thread cannot draw
 * while it does (see the MAIN THREAD ONLY note above -- a unit's own internal
 * worker pool does not change that), so the only way to show the player
 * anything is to let the stage runner call back OUT between units.  That is
 * what this is: a pump, exactly like retail's own loading loop (FUN_00156460
 * draws one frame, presents, polls, repeats).
 *
 *   stage     the unit about to run ("textures", "awd", "@elf"...), or NULL
 *             for the one final call that reports the whole set finished.
 *   track_id  the track it belongs to, NULL for a global family.
 *   index     units finished so far, 0..total.
 *   total     units this call will really run -- already-stamped and
 *             already-attempted units are excluded, so a warm cache reports
 *             a small total rather than sitting at 90% and jumping.
 *
 * The hook is called from the stage LOOP, i.e. outside the chdir() and the
 * stdout redirect a running stage lives under, and never re-entrantly.  It
 * must not open an asset itself: materialisation is locked out while a stage
 * is in flight, so any texture or font the callback draws with has to have
 * been loaded before the first unit ran.  Pass cb = NULL to remove it. */
typedef void (*b3_iso_progress_fn)(const char *stage, const char *track_id,
                                   int index, int total, void *user);
void b3_iso_set_progress(b3_iso_progress_fn cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_ISODATA_H */
