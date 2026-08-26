/* b3_web.h -- THE WEB PORT's platform seam (WebAssembly + WebGL, Emscripten).
 *
 * The web build compiles src/ VERBATIM, exactly as the Android port does.  The
 * whole divergence is this file plus two `#ifdef __EMSCRIPTEN__` blocks in
 * src/burnout3_full.c and three one-line GetProcAddress redirects that already
 * existed for Android.
 *
 * See web/README.md for the build recipe and the FS/threading layout.
 */
#ifndef B3_WEB_H
#define B3_WEB_H

#ifdef __cplusplus
extern "C" {
#endif

/* gl4es bring-up.  Call ONCE, after SDL_GL_CreateContext() has made the ES
 * context current and before the first gl* call.  gl4es is built with
 * NO_INIT_CONSTRUCTOR, so nothing else will do it. */
void b3_web_gl_init(void);

/* ==================================================== THE IMAGE BRIDGE ====
 * The path by which the 2.4 GB Xbox image reaches the engine, and the one
 * piece of this port that is NOT what the original plan called for.  The
 * reason is worth stating exactly, because it is not obvious:
 *
 *   Under -sPROXY_TO_PTHREAD the game runs on a worker, but Emscripten's
 *   JS filesystem lives on the MAIN BROWSER THREAD and every file syscall the
 *   game makes is proxied there.  (Measured, not assumed: a file created in
 *   preRun is invisible to the game thread's own `FS` object yet readable
 *   through C's fopen -- so the read executed on the main thread.)
 *
 *   WORKERFS reads a File with FileReaderSync, which exists ONLY on a worker.
 *   Mounting it therefore asserts, and could not have worked anyway: the read
 *   would run on the main thread, where FileReaderSync does not exist.
 *
 * So the image does not go through the filesystem at all.  A dedicated helper
 * worker owns the File and has FileReaderSync; the game thread reaches it
 * through the WASM HEAP -- which under pthreads IS a SharedArrayBuffer, and so
 * is genuinely shared -- using a small control block and wasm atomics.  The
 * game thread blocks in memory.atomic.wait32 exactly as it would block in
 * pread(), so cx_src's read path stays synchronous and unchanged in shape.
 *
 * Nothing is copied: the File is read lazily off the user's disk, a few
 * hundred KB at a time, the same as the native mmap only ever touched the
 * pages it needed.
 *
 * THE CONTRACT PATH is "/iso/game.xiso" -- web/pre.js leaves a zero-byte
 * placeholder there so that every existence check in burnout3_isodata.c
 * succeeds, and cx_src diverts the actual reads here. */

/* ======================================================= THE GL SHIMS ====
 * The game thread is a worker, so its WebGL context has to come from an
 * OffscreenCanvas transferred to it (-sOFFSCREENCANVASES_TO_PTHREAD).  SDL2
 * cannot make that context: its Emscripten backend calls getContext() on the
 * DOM canvas, and the browser refuses --
 *
 *   InvalidStateError: Failed to execute 'getContext' on 'HTMLCanvasElement':
 *   Cannot get context from a canvas that has transferred its control to
 *   offscreen.
 *
 * (Measured, not guessed: the same probe showed the RAW route --
 * emscripten_webgl_create_context("#canvas") on the worker -- succeeding and
 * becoming current.)
 *
 * So the port takes over exactly three SDL calls and leaves everything else to
 * SDL: window bookkeeping, events and gamepads are untouched.  (Audio is taken
 * over too, by four more macros, and for a related reason -- see THE AUDIO
 * BRIDGE below.)  They are taken over as MACROS rather than edits, so not one
 * line of the frame loop in src/burnout3_full.c changes -- which also keeps
 * this diff off the hot areas other work is touching.
 *
 * Gated on SDL_h_ because this header is also included by tools/cextract/
 * cx_src.c for the image bridge, and that translation unit has no SDL. */
#ifdef SDL_h_
SDL_GLContext b3_web_gl_create_context(SDL_Window *w);
void          b3_web_gl_swap(SDL_Window *w);
void          b3_web_gl_drawable_size(SDL_Window *w, int *out_w, int *out_h);

/* ====================================================== THE AUDIO BRIDGE ====
 * THE IMAGE BRIDGE, REVERSED -- and for the same reason, stated the same way.
 *
 * SDL2's Emscripten audio backend builds a WebAudio graph on the thread that
 * opens the device.  `AudioContext` DOES NOT EXIST IN A WORKER -- it is a
 * window-only interface -- so on the game pthread SDL's own EM_ASM throws
 *
 *   TypeError: Cannot read properties of undefined (reading 'audioContext')
 *
 * and takes the thread down with it.  This is a property of SDL2 + workers,
 * not of this port: there is no attribute or flag that moves it.  For a long
 * time the answer was to hand back 0 and run silent.
 *
 * It is not the mixer that cannot live on the worker -- it is only the SINK.
 * So the sink moves and the mixer does not:
 *
 *   THE GAME SIDE (a worker).  A dedicated pthread -- SDL's own audio thread
 *   in every respect that matters -- calls the engine's SDL callback with the
 *   spec's own buffer size, VERBATIM.  audio_callback() in
 *   src/burnout3_full.c is not touched, not recompiled differently and not
 *   told where it is: the per-voice aftertouch timescale, the fe_ cues, the
 *   b3_sfx event waves and the b3_music stream all run exactly as they do on
 *   the desktop.  Its 44.1 kHz mono S16 output is converted to float and
 *   written into a RING IN THE WASM HEAP -- which under pthreads IS a
 *   SharedArrayBuffer, and so is genuinely shared.
 *
 *   THE PAGE SIDE (the main thread).  An AudioWorkletProcessor, given the
 *   same SharedArrayBuffer and the ring's byte offsets, reads that ring from
 *   the audio rendering thread at the context's own rate and quantum.
 *
 * Nothing is copied across a postMessage boundary, and no lock is taken
 * between the two: the ring's producer and consumer cursors are plain wasm
 * atomics, one writer and one reader.  The worklet must never block -- it is
 * the audio thread -- so it never waits: a short read fills silence and counts
 * itself, which is what the underrun number in the log line is.
 *
 * THE SAMPLE RATE IS THE WORKLET'S PROBLEM.  The engine mixes at 44100 and
 * cannot be moved off it (g_eng_phase, b3_music and the fe_ SDL_AudioCVT
 * conversions all have 44100 compiled into them), while the browser hands out
 * whatever its device runs at -- 48000 on every machine this was measured on.
 * So the ring is 44100 and the worklet reads it with a FRACTIONAL cursor
 * stepping at 44100/sampleRate.  One resampler, at the one place that knows
 * the real rate.
 *
 * WHY A PTHREAD AND NOT THE FRAME LOOP.  A frame that takes 200 ms -- a track
 * load, a GC pause, a materialise off the disc -- would be 200 ms of silence
 * if the pump rode on it, and those are exactly the moments the port has.  The
 * pump sleeps in its own thread and tops the ring up to a fill target instead,
 * which is also what makes a HIDDEN PAGE safe: a suspended context stops
 * draining, the ring reaches its target, and the pump simply stops producing.
 * Bounded by construction -- there is nothing to leak.
 *
 * If the shell hands over no AudioContext at all (Module.audioContext), the
 * open still answers 0 and the port runs silent exactly as it used to. */

/* Four SDL entry points, taken over as macros for the same reason the GL ones
 * are: not one line of src/ changes.  Lock/Unlock are real -- fe_play()
 * mutates the front-end voice table under them and the pump holds the same
 * mutex across the callback, which is SDL's own contract. */
SDL_AudioDeviceID b3_web_open_audio_device(const SDL_AudioSpec *want);
void b3_web_pause_audio_device(SDL_AudioDeviceID dev, int pause_on);
void b3_web_lock_audio_device(SDL_AudioDeviceID dev);
void b3_web_unlock_audio_device(SDL_AudioDeviceID dev);

#ifndef B3_WEB_IMPL          /* b3_web.c calls the real SDL entry points */
#define SDL_GL_CreateContext(w)         b3_web_gl_create_context(w)
#define SDL_GL_SwapWindow(w)            b3_web_gl_swap(w)
#define SDL_GL_GetDrawableSize(w, a, b) b3_web_gl_drawable_size((w), (a), (b))
#define SDL_OpenAudioDevice(a, b, c, d, e) \
            (((void)(a), (void)(b), (void)(d), (void)(e)), \
             b3_web_open_audio_device(c))
#define SDL_PauseAudioDevice(d, p)      b3_web_pause_audio_device((d), (p))
#define SDL_LockAudioDevice(d)          b3_web_lock_audio_device(d)
#define SDL_UnlockAudioDevice(d)        b3_web_unlock_audio_device(d)
#endif
#endif /* SDL_h_ */

/* ===================================================== THE FRAME PROFILER ====
 * Web-only, and it exists because "the port feels slow" is not a number.
 *
 * The three stages that can be attributed without touching one line of the
 * shared frame loop are timed here:
 *
 *   B3_WEB_PROF_BLUR   b3_postfx_blur()   -- the present composite
 *   B3_WEB_PROF_GAMMA  b3_postfx_gamma()  -- the output ramp
 *   B3_WEB_PROF_SWAP   the present itself -- emscripten_webgl_commit_frame()
 *
 * and the WHOLE frame is measured in b3_web_gl_swap() as the wall time from
 * one present's return to the next's start, so "everything else" falls out as
 * FRAME - (BLUR + GAMMA).  SWAP is the load-bearing one on this target: the
 * GL context is proxied to the main thread, every gl* call the game makes is
 * queued there, and commit_frame is a SYNCHRONOUS proxied call -- so the time
 * spent inside it is the time the main thread needs to drain everything this
 * frame queued, plus the GPU.  A big SWAP means the port is call-bound, not
 * fill-bound, and that is a different fix from a cheaper shader.
 *
 * B3_WEB_PROF=<n> prints one line every n frames (0/unset = off). */
/* ===================================================== THE HARDWARE PROFILE
 * B3_WEB_HWPROF=<n>.  The four slots above answer "which postfx pass costs";
 * they cannot answer the question a player on a fast GPU actually has, which
 * is "where did my frame go".  These five do, and they are deliberately the
 * WALL-CLOCK spans a player can act on:
 *
 *   B3_WEB_PROF_SIM     the retail inner loop -- physics, AI, traffic, audio
 *   B3_WEB_PROF_SCENE   render_frame() end to end (postfx included)
 *   B3_WEB_PROF_GRAB    postfx_grab_frame() ALONE -- the one call in the frame
 *                       that can turn into a synchronous GPU->CPU readback
 *                       (see the note over postfx_grab_frame in
 *                       src/burnout3_postfx.c), split out of GAMMA so a
 *                       readback cannot hide inside a shader pass's number
 *   B3_WEB_PROF_XFER    transferToImageBitmap() alone, timed in JS
 *   B3_WEB_PROF_POST    postMessage() alone, timed in JS
 *
 * XFER and POST are measured on the JS side and handed back through the
 * counter block, because they are two halves of one C call and the split is
 * the whole question: a slow transferToImageBitmap is the browser making a
 * copy of the drawing buffer, a slow postMessage is the structured-clone /
 * transfer path.  They are DIFFERENT bugs with different fixes.
 *
 * ON THE PROXIED FALLBACK (B3_WEB_PRESENT=proxy) the counters still work --
 * they are armed on whichever thread owns the context object -- but XFER, POST
 * and the GPU clock read zero, because that path presents through
 * emscripten_webgl_commit_frame() and never reaches b3_web_present_frame().
 * SWAP still covers the whole present there, which is the number that mattered
 * on that path anyway.
 *
 * The GPU's own elapsed time for the frame comes from
 * EXT_disjoint_timer_query_webgl2 where the browser offers it (it is a WebGL 2
 * extension and this port asks for a WebGL 1 context, so "n/a" is the
 * EXPECTED answer -- see b3_web_lib.js).  B3_WEB_HWPROF_SYNC=1 substitutes a
 * timed glFinish() before the present, which is a real pipeline sync and
 * therefore PERTURBS the thing it measures: opt-in, never a default. */
enum { B3_WEB_PROF_BLUR = 0, B3_WEB_PROF_GAMMA, B3_WEB_PROF_SWAP,
       B3_WEB_PROF_FRAME, B3_WEB_PROF_SIM, B3_WEB_PROF_SCENE,
       B3_WEB_PROF_GRAB, B3_WEB_PROF_SLOTS };
void   b3_web_prof(int slot, double ms);
double b3_web_now_ms(void);

/* 1 while B3_WEB_HWPROF is armed.  postfx checks it before paying for the
 * extra b3_web_now_ms() pair around the grab -- off, the grab is untimed and
 * the frame is byte-for-byte the frame it was. */
int    b3_web_hwprof_on(void);

/* ======================================================== THE ZONE COUNTER
 * Which SUBSYSTEM makes the GL calls.
 *
 * The call counter (B3_WEB_GLCOUNT) counts WebGL calls where they leave the
 * port, which is a number and not yet an answer: 26 000 calls a frame does not
 * say whether to go after the track, the cars or the HUD.  A zone is opened
 * around each pass in render_frame() and the counter's running totals are
 * differenced across it.
 *
 * This is EXACT ONLY ON THE DIRECT PATH, and that is not a caveat to bury: it
 * works because a call the game thread makes has already executed by the time
 * the next line of C runs.  Under proxying the calls are drained later, on
 * another thread, and a difference taken here would attribute them to whatever
 * zone happened to be open when the queue got around to them.  b3_web.c says
 * so in the report line rather than printing a number that looks precise.
 *
 * Off unless B3_WEB_GLCOUNT is set; B3_ZONE() compiles to nothing off-web, so
 * the desktop binary is unchanged to the byte. */
enum { B3_ZONE_NONE = 0, B3_ZONE_TRACK, B3_ZONE_SKY, B3_ZONE_PROPS,
       B3_ZONE_SCENERY, B3_ZONE_CARS, B3_ZONE_TRAFFIC, B3_ZONE_FX,
       B3_ZONE_HUD, B3_ZONE_POST, B3_ZONE_N };
void b3_web_glc_zone(int zone);

/* 1 when `path` is the bridged image.  Arms the bridge on first use. */
int b3_web_iso_claims(const char *path);

/* The image's size in bytes, or 0 if the bridge is not armed. */
unsigned long long b3_web_iso_size(void);

/* pread(), served by the helper worker.  Bytes read, or -1. */
long b3_web_iso_pread(void *dst, unsigned long long off, unsigned long len);

#ifdef __cplusplus
}
#endif

#endif /* B3_WEB_H */
