# Burnout 3: Takedown - Makefile
CC = gcc
# -pthread: the extraction stages walk their per-item fleets on a worker pool
# (tools/cextract/cx_pool.h).  It is a COMPILE flag as much as a link one --
# it defines _REENTRANT and picks the thread-safe errno -- so it belongs in
# CFLAGS, not only on the link line.
CFLAGS = -Wall -Wextra -std=c11 -O2 -pthread
# Rules land above `all:` as merges accrete; pin the default explicitly.
.DEFAULT_GOAL := all
SDL_CFLAGS = $(shell pkg-config --cflags sdl2 SDL2_image)
LDFLAGS = $(shell pkg-config --cflags --libs sdl2 SDL2_image) -lGL -lm -pthread

# ------------------------------------------------------------ THE DATA MODEL
# The game reads the user's own Xbox disc by default and materialises what it
# needs out of it (src/burnout3_isodata.c); the pre-extracted build/ tree is
# the --build debug path.  That means the cextract STAGES are linked into the
# game, so it can run one in process on a cache miss.
#
# TWO COMPILATION GROUPS, and the split is load-bearing:
#   * src/ is compiled with `-include src/burnout3_isoshim.h`, which redirects
#     fopen / access / IMG_Load through the resolver.  That is why not one
#     loader had to be edited.
#   * the cextract objects and burnout3_isodata.c itself are compiled WITHOUT
#     it -- they do the real file I/O, and a shimmed fopen there would recurse.
#
# cx_main.c is the standalone driver's own main(), and is the ONLY thing left
# out of the game now.  cx_audio_xwb.c and cx_audio_eatrax.c used to be skipped
# too, because they forked ffmpeg for the WMA streams -- which a game process
# has no business doing and a browser tab cannot do at all.  They decode in
# process now (see THE WMA DECODER below), so the music family materialises out
# of the disc like every other asset and the "unavailable until extracted"
# notice is gone.
CX_DIR  = tools/cextract
CX_SKIP = $(CX_DIR)/cx_main.c
CX_SRCS = $(filter-out $(CX_SKIP), $(wildcard $(CX_DIR)/*.c))
CX_OBJS = $(patsubst $(CX_DIR)/%.c, build/isoobj/%.o, $(CX_SRCS))
ISO_OBJ = build/isoobj/burnout3_isodata.o

# ------------------------------------------------------- THE WMA DECODER
# Every audio payload on the disc is WMAv2 standard (0x0161), 2 ch, 160 kb/s
# -- all 885 of them, measured from their own ASF headers.  Rockbox's libwma
# decodes exactly that, in fixed point, with no allocator and no build system
# of its own, which is what lets the same code serve the desktop binary, the
# Android .so and a wasm module.  tools/fetch_wma.sh vendors it into
# third_party/ (gitignored, like web/third_party/) and documents the licensing.
#
# WITHOUT THE FETCH, THE BUILD STILL WORKS.  b3_wma_stub.c has the same API and
# reports "no WMA decoder"; the two music stages are compiled and linked either
# way, so the stage tables are the same shape whatever the tree looks like.
# That is the property CX_SKIP did not have.
#
# The vendored sources compile WITHOUT -Wall -Wextra.  They are LGPL code we do
# not patch, and FFmpeg's deliberate switch fallthroughs in wmadeci.c would
# otherwise add ~40 warnings to every build.  Our own tools/cextract/wma/ code
# is held to the full warning set with the rest of the tree.
RB_DIR   = third_party/rockbox
WMA_DIR  = $(CX_DIR)/wma
WMA_INC  = -I$(WMA_DIR)

ifneq ($(wildcard $(RB_DIR)/codecs/libwma/wmadeci.c),)
WMA_SRCS = $(WMA_DIR)/b3_wma.c
WMA_INC += -I$(WMA_DIR)/codecs/lib -I$(RB_DIR) -I$(RB_DIR)/codecs \
           -I$(RB_DIR)/codecs/lib -I$(RB_DIR)/codecs/libwma
RB_SRCS  = $(RB_DIR)/codecs/libwma/wmadeci.c \
           $(RB_DIR)/codecs/libwma/wmafixed.c \
           $(RB_DIR)/codecs/lib/mdct.c \
           $(RB_DIR)/codecs/lib/mdct_lookup.c \
           $(RB_DIR)/codecs/lib/fft-ffmpeg.c \
           $(RB_DIR)/codecs/lib/ffmpeg_bitstream.c
else
WMA_SRCS = $(WMA_DIR)/b3_wma_stub.c
RB_SRCS  =
endif

# -DROCKBOX is what the vendored files gate their `#include <codecs/lib/
# codeclib.h>` on; without it mdct_lookup.c takes a bare-<stdlib.h> path that
# leaves ICONST_ATTR undefined.  tools/cextract/wma/ supplies the headers that
# include resolves to -- see platform.h there for what is being stood in for.
WMA_CPPFLAGS = -DROCKBOX $(WMA_INC)

WMA_OBJS = $(patsubst %.c, build/isoobj/wma/%.o, $(notdir $(WMA_SRCS))) \
           $(patsubst %.c, build/isoobj/wma/%.o, $(notdir $(RB_SRCS)))

build/isoobj/wma/b3_wma.o: $(WMA_DIR)/b3_wma.c $(WMA_DIR)/b3_wma.h
	@mkdir -p build/isoobj/wma
	$(CC) $(CFLAGS) $(WMA_CPPFLAGS) -c $< -o $@

build/isoobj/wma/b3_wma_stub.o: $(WMA_DIR)/b3_wma_stub.c $(WMA_DIR)/b3_wma.h
	@mkdir -p build/isoobj/wma
	$(CC) $(CFLAGS) $(WMA_CPPFLAGS) -c $< -o $@

build/isoobj/wma/%.o: $(RB_DIR)/codecs/libwma/%.c
	@mkdir -p build/isoobj/wma
	$(CC) -std=c11 -O2 -w $(WMA_CPPFLAGS) -c $< -o $@

build/isoobj/wma/%.o: $(RB_DIR)/codecs/lib/%.c
	@mkdir -p build/isoobj/wma
	$(CC) -std=c11 -O2 -w $(WMA_CPPFLAGS) -c $< -o $@

.PHONY: wma-deps
wma-deps:
	sh tools/fetch_wma.sh

.PHONY: all clean run test-nav-walk test-nav-selector test-soup-ray test-traffic-paths \
	test-traffic-reservations test-traffic-pool test-traffic-mix \
	test-traffic-pop test-draw-distance test-engine-audio test-audio-ring

all: burnout3 build/dump_traj

test-soup-ray: build/validate_frozen_soup
	./build/validate_frozen_soup

test-nav-walk:
	python3 tools/validate_nav_walk.py

test-nav-selector:
	python3 tools/validate_nav_selector.py

test-traffic-paths:
	python3 tools/validate_traffic_paths.py

test-traffic-mix:
	python3 tools/validate_traffic_mix.py

# retail's 160 m view gate (FUN_001A6070 @0x001A64E5): the rule that stops a
# traffic car materialising -- or vanishing -- inside the player's view.
test-traffic-pop:
	python3 tools/validate_traffic_pop.py

# the race view's near/far planes -- retail's 0.5/10000 (FUN_0002ECC0
# @0x0002EDAE/0x0002EDCC), and the distant geometry a 5000 far plane clipped.
# Renders two pinned long-sightline frames, so it needs `burnout3` built and a
# GL context; `--no-render` runs the source + shipped-asset sections only.
test-draw-distance: burnout3
	python3 tools/validate_draw_distance.py

# THE WEB AUDIO RING's consumer, on its own and in a second: the
# AudioWorkletProcessor lifted out of web/b3_web_lib.js and run against a
# SharedArrayBuffer laid out as web/b3_web.c lays it out.  No browser, no ISO
# -- which is the point, because getting a headless browser to a running race
# is the slowest gate here and this arithmetic is the easiest part to break.
test-audio-ring:
	node tools/validate_audio_ring.js

# DOES THE ENGINE VOICE FOLLOW THE RPM?  "REAL audio: 0 engine loops" printed
# for a long time with nothing in the suite to notice.  This captures the
# harness's own mix through SDL's `disk` driver -- offscreen and inaudible, no
# device is opened at all -- next to a per-frame drive log, and correlates the
# voice's pitch against the drivetrain's rpm.
test-engine-audio: burnout3
	tools/audio_capture.sh /tmp/b3_mix.raw /tmp/b3_drive.txt 30
	python3 tools/validate_engine_audio.py /tmp/b3_mix.raw /tmp/b3_drive.txt

test-traffic-reservations: build/validate_traffic_reservations
	./build/validate_traffic_reservations

test-traffic-pool: build/validate_traffic_pool
	./build/validate_traffic_pool

build/validate_frozen_soup: tools/validate_frozen_soup.c src/burnout3_collision.c src/burnout3_collision.h src/burnout3_vehicle_sim.c src/burnout3_vehicle_sim.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_frozen_soup.c src/burnout3_collision.c src/burnout3_vehicle_sim.c -lm

build/validate_traffic_reservations: tools/validate_traffic_reservations.c src/burnout3_traffic_reservations.c src/burnout3_traffic_reservations.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_traffic_reservations.c src/burnout3_traffic_reservations.c -lm

build/validate_traffic_pool: tools/validate_traffic_pool.c src/burnout3_traffic_pool.c src/burnout3_traffic_pool.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/validate_traffic_pool.c src/burnout3_traffic_pool.c -lm

# trajectory driver for the full-pipeline differential test
# (tools/validate_port.py, full-pipeline section)
# burnout3_crash.c carries the crash=retail switch, so this driver links the
# backend selector and the bridge too. validate_port pins B3_BACKENDS to
# /dev/null, so the trajectory it dumps is always the RE path.
build/dump_traj: tools/dump_traj.c src/burnout3_vehicle_sim.c src/burnout3_panels.c \
                 src/burnout3_backend.c src/burnout3_emu.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tools/dump_traj.c \
	    src/burnout3_vehicle_sim.c src/burnout3_panels.c \
	    src/burnout3_backend.c src/burnout3_emu.c -lm

SRCS = src/burnout3_full.c src/burnout3_vehicle_sim.c src/burnout3_trackmesh.c \
       src/burnout3_render.c \
       src/burnout3_hud.c src/burnout3_collision.c src/burnout3_crash.c \
       src/burnout3_carcol.c src/burnout3_takedown.c src/burnout3_score_events.c \
       src/burnout3_td_rules.c src/burnout3_sfx.c src/burnout3_ai.c \
       src/burnout3_ai_avoid.c \
       src/burnout3_carfx.c src/burnout3_postfx.c \
       src/burnout3_aftereffects.c src/burnout3_music.c \
       src/burnout3_boostfx.c src/burnout3_particlefx.c \
       src/burnout3_panels.c src/burnout3_props.c src/burnout3_scenery.c \
	       src/burnout3_traffic_reservations.c src/burnout3_traffic_pool.c \
       src/burnout3_backend.c src/burnout3_emu.c

build/isoobj/%.o: $(CX_DIR)/%.c $(CX_DIR)/cx_extract.h
	@mkdir -p build/isoobj
	$(CC) $(CFLAGS) -I$(CX_DIR) $(WMA_INC) -c $< -o $@

$(ISO_OBJ): src/burnout3_isodata.c src/burnout3_isodata.h \
            $(CX_DIR)/cx_extract.h
	@mkdir -p build/isoobj
	$(CC) $(CFLAGS) -Isrc -I$(CX_DIR) -c $< -o $@

burnout3: $(SRCS) src/*.h $(CX_OBJS) $(ISO_OBJ) $(WMA_OBJS)
	$(CC) $(CFLAGS) -Isrc -I$(CX_DIR) $(WMA_INC) $(SDL_CFLAGS) \
	    -include src/burnout3_isoshim.h \
	    $(SRCS) $(CX_OBJS) $(ISO_OBJ) $(WMA_OBJS) $(LDFLAGS) -lz -o $@

run: burnout3
	./burnout3

# ============================================================ THE WEB PORT
# `make wasm` -> build/web/burnout3.{js,wasm} + the worker files Emscripten
# emits.  See web/README.md for the whole design; the short version:
#
#   * src/ is compiled VERBATIM, with the same -include isoshim split the
#     native rule uses.  SRCS / CX_SRCS below are the SAME variables the
#     desktop binary is built from -- there is deliberately no second source
#     list to drift out of lockstep (the Android port has one, and it did).
#   * <GL/gl.h> resolves to web/GL/gl.h, which is GLES2 -- there is no
#     compatibility layer in this link any more.  gl4es was here to
#     re-implement GL 2.1 (matrix stack, glBegin, display lists, glTexEnv) on
#     GLES2; the retained renderer emits none of that, so Emscripten's own
#     WebGL bindings satisfy every entry point directly.  -I order is still
#     load-bearing: -Iweb must precede the sysroot, whose <GL/gl.h> is the
#     mgl*-namespaced legacy GL emulation and is NOT what we want.
#   * PROXY_TO_PTHREAD puts main() on a worker.  That is what makes the
#     blocking `while (g_running)` frame loop legal, and what makes WORKERFS'
#     synchronous FileReaderSync pread() of the 2.4 GB image possible at all.
#   * PTHREAD_POOL_SIZE is 9: the base 4, plus the audio bridge's mixer
#     pthread (THE AUDIO BRIDGE, web/b3_web.h), plus cx_pool's 3 extraction
#     workers and their calling thread (tools/cextract/cx_pool.h).  Sized so
#     pthread_create never has to GROW the pool at runtime -- under
#     PROXY_TO_PTHREAD growing means a round trip to the browser's main
#     thread from a worker that is mid-boot.  Both prior sizings (5 for
#     audio, 8 for cx_pool) were each unaware of the other's thread.
#
# Requires the emsdk on PATH (emcc).  EMSDK_DIR can point at a checkout that
# is not yet activated in this shell.
EMCC      ?= emcc
EMCMAKE   ?= emcmake
WEB_OUT    = build/web/burnout3.js

# -D_GNU_SOURCE: Emscripten is musl, which gates realpath/strdup/strtok_r on it
# rather than on the _POSIX_C_SOURCE these files already ask for (glibc is
# laxer).  The Android/NDK build carries the same flag for the same reason.
WASM_CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread -D_GNU_SOURCE \
              -Wno-unused-parameter -Wno-missing-field-initializers
WASM_INC    = -Isrc -I$(CX_DIR) -Iweb
WASM_PORTS  = -sUSE_SDL=2 -sUSE_SDL_IMAGE=2 -sSDL2_IMAGE_FORMATS='["png"]' \
              -sUSE_ZLIB=1

# STACK_SIZE: main()'s frame in this harness is large (the Android port needed
# 32 MB after a SIGSEGV on SDL's 1 MB Java thread).  Emscripten's default is
# 64 KB, and an overflow there is silent heap corruption, not a fault.
# FULL_ES2 is GONE with gl4es: it existed because gl4es drew immediate-mode
# geometry from CLIENT-SIDE arrays, which WebGL has no such thing as, and
# emulating them costs a heap copy and a buffer upload per draw.  The retained
# renderer draws from static VBOs only, so there is nothing to emulate.
# GL_ENABLE_GET_PROC_ADDRESS stays: burnout3_render.c and the recovered
# carfx/postfx programs resolve their GL 2.0 entry points through
# emscripten_GetProcAddress rather than linking them by name.
#
# MAX_WEBGL_VERSION=2 / MIN_WEBGL_VERSION=1 -- BOTH bindings in one binary.
# The MAX is what lets web/b3_web.c ask for a WebGL 2 context at all, and it is
# also what makes Emscripten's own emscripten_webgl2_get_proc_address() answer
# for glBlitFramebuffer and glRenderbufferStorageMultisample: those two are
# behind `#if MAX_WEBGL_VERSION >= 2` in system/lib/gl/gl.c, so without this
# flag the aftereffects chain's MSAA path cannot resolve its entry points no
# matter what context the browser hands over.  The MIN stays at 1 -- the
# default, spelled out because it is load-bearing -- so the WebGL 1 bindings
# are still linked and the fallback in b3_web_gl_create_context() is a real
# path rather than a comment.  Emscripten does NOT fall back on its own
# (settings.js says so in as many words); the port does it by hand.
#
# The GLSL needed nothing: every shader here is ESSL 1.00, which WebGL 2
# accepts unchanged.  FULL_ES2 is still gone, and is still not needed -- see
# the note above.
WASM_LDFLAGS = -pthread \
   -sMODULARIZE=1 -sEXPORT_NAME=createB3Module \
   -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=9 \
   -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sINITIAL_MEMORY=536870912 \
   -sSTACK_SIZE=33554432 \
   -sGL_ENABLE_GET_PROC_ADDRESS=1 \
   -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=2 \
   -sOFFSCREEN_FRAMEBUFFER=1 \
   -sFORCE_FILESYSTEM=1 -lidbfs.js \
   -sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV,addRunDependency,removeRunDependency,HEAPU8 \
   -sEXIT_RUNTIME=1 -sASSERTIONS=1 \
   --pre-js web/pre.js --js-library web/b3_web_lib.js \
   --embed-file $(WEB_REPO)/src@/app/src

.PHONY: wasm wasm-clean serve test-web

# The page must be cross-origin isolated (COOP+COEP) or SharedArrayBuffer --
# and therefore the whole threaded build -- is unavailable.  Serve the REPO
# ROOT: web/index.html reaches the module at ../build/web/burnout3.js.
# (the canonical `serve` target lives at the end of this file)

# The headless smoke gate.  Never opens a visible browser.
test-web: wasm
	python3 tools/web_smoke.py --iso "$(B3_ISO)"

# THE TWO COMPILATION GROUPS, exactly as the native rule splits them: the
# cextract stages and burnout3_isodata.c do the REAL file I/O and are compiled
# WITHOUT the isoshim, because a shimmed fopen there would recurse.  That is
# why they are separate objects rather than more files on the link line.
WEB_CX_OBJS = $(patsubst $(CX_DIR)/%.c, build/web/isoobj/%.o, $(CX_SRCS))
WEB_ISO_OBJ = build/web/isoobj/burnout3_isodata.o

# THE WMA DECODER, unchanged for wasm.  It is fixed-point C with no allocator,
# no threads and no I/O, so there is no emconfigure step and no port recipe --
# the same six vendored .c files and the same shim compile straight through
# emcc.  That is the whole reason libwma was chosen over a minimal libavcodec
# build: ffmpeg would have needed its own configure run inside emconfigure, a
# second one for the native build, and both pinned.  See tools/fetch_wma.sh.
#
# It is also the reason music can exist on the web at all: WebCodecs has no
# 'wmav2' decoder in any shipping engine, so there is nothing to delegate to.
WEB_WMA_OBJS = $(patsubst %.c, build/web/isoobj/wma/%.o, $(notdir $(WMA_SRCS))) \
               $(patsubst %.c, build/web/isoobj/wma/%.o, $(notdir $(RB_SRCS)))

build/web/isoobj/wma/b3_wma.o: $(WMA_DIR)/b3_wma.c $(WMA_DIR)/b3_wma.h
	@mkdir -p build/web/isoobj/wma
	$(EMCC) $(WASM_CFLAGS) $(WMA_CPPFLAGS) -Iweb -c $< -o $@

build/web/isoobj/wma/b3_wma_stub.o: $(WMA_DIR)/b3_wma_stub.c $(WMA_DIR)/b3_wma.h
	@mkdir -p build/web/isoobj/wma
	$(EMCC) $(WASM_CFLAGS) $(WMA_CPPFLAGS) -Iweb -c $< -o $@

build/web/isoobj/wma/%.o: $(RB_DIR)/codecs/libwma/%.c
	@mkdir -p build/web/isoobj/wma
	$(EMCC) -std=c11 -O2 -w -pthread -D_GNU_SOURCE $(WMA_CPPFLAGS) -Iweb -c $< -o $@

build/web/isoobj/wma/%.o: $(RB_DIR)/codecs/lib/%.c
	@mkdir -p build/web/isoobj/wma
	$(EMCC) -std=c11 -O2 -w -pthread -D_GNU_SOURCE $(WMA_CPPFLAGS) -Iweb -c $< -o $@

build/web/isoobj/%.o: $(CX_DIR)/%.c $(CX_DIR)/cx_extract.h
	@mkdir -p build/web/isoobj
	$(EMCC) $(WASM_CFLAGS) -I$(CX_DIR) $(WMA_INC) -Iweb -c $< -o $@

$(WEB_ISO_OBJ): src/burnout3_isodata.c src/burnout3_isodata.h \
                $(CX_DIR)/cx_extract.h
	@mkdir -p build/web/isoobj
	$(EMCC) $(WASM_CFLAGS) -Isrc -I$(CX_DIR) -Iweb -c $< -o $@

# THE REPO'S OWN HEADERS ARE STAGE INPUTS.  Several cextract stages read this
# checkout's headers to recover tables that live in them -- car_tuning parses
# src/burnout3_physics_params.h, the ai/crash/vehicle range generators parse
# src/burnout3_ai.h and src/burnout3_vehicle_sim.h, sfx_emitters parses
# src/burnout3_sfx.h.  Natively they are simply there, next to the binary; on
# the web nothing is, so they are embedded at /app/src (the CWD the engine
# boots into, which is what $B3_REPO_DIR ends up pointing at).
#
# ALL the headers go in, not just today's four: they are 864 KB next to a
# 2.4 MB wasm, and pinning an exact list is a tripwire for the next stage that
# reads one.  This is the port's OWN SOURCE, not disc content -- no game data
# is baked in here.
WEB_REPO = build/web/repo

$(WEB_REPO)/.stamp: src/*.h
	@mkdir -p $(WEB_REPO)/src
	cp src/*.h $(WEB_REPO)/src/
	@touch $@

wasm: $(WEB_OUT)

$(WEB_OUT): $(SRCS) src/*.h web/b3_web.c web/b3_web.h web/pre.js web/b3_web_lib.js \
            $(WEB_CX_OBJS) $(WEB_ISO_OBJ) $(WEB_WMA_OBJS) $(WEB_REPO)/.stamp
	@mkdir -p build/web
	$(EMCC) $(WASM_CFLAGS) $(WASM_INC) $(WMA_INC) $(WASM_PORTS) \
	    -include src/burnout3_isoshim.h \
	    $(SRCS) web/b3_web.c \
	    $(WEB_CX_OBJS) $(WEB_ISO_OBJ) $(WEB_WMA_OBJS) \
	    $(WASM_LDFLAGS) $(WASM_EXTRA) -o $(WEB_OUT)
	@ls -l build/web/burnout3.js build/web/burnout3.wasm

wasm-clean:
	rm -rf build/web

clean:
	rm -f burnout3 burnout3_full build/dump_traj build/validate_frozen_soup \
		build/validate_traffic_reservations build/validate_traffic_pool
	rm -rf build/isoobj

# Web shell: serve web/ + build/web/ from the repo root with the COOP/COEP
# headers SharedArrayBuffer needs.  http://127.0.0.1:$(WEB_PORT)/web/
WEB_PORT ?= 8080
.PHONY: serve
serve:
	@python3 tools/webserve.py --port $(WEB_PORT)

# Stage the statically-hostable site into dist/: the six runtime files in
# their served layout, a _headers file carrying the COOP/COEP pair
# SharedArrayBuffer requires (Cloudflare Pages / Netlify read it), and a
# root redirect to /web/.  No game data is staged -- every asset comes off
# the player's own disc at runtime.  Deploy: npx wrangler pages deploy dist
.PHONY: webdist
webdist:
	@test -f build/web/burnout3.js -a -f build/web/burnout3.wasm || \
	    { echo "webdist: build/web/burnout3.{js,wasm} missing -- run 'make wasm' first"; exit 1; }
	rm -rf dist
	mkdir -p dist/web/vendor dist/build/web
	cp web/index.html web/app.js web/style.css web/favicon.svg dist/web/
	cp web/vendor/sha256.js dist/web/vendor/
	cp build/web/burnout3.js build/web/burnout3.wasm dist/build/web/
	printf '/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n' > dist/_headers
	printf '<!doctype html><meta http-equiv="refresh" content="0; url=/web/">\n' > dist/index.html
	@echo "webdist: staged $$(du -sh dist | cut -f1) in dist/ -- npx wrangler pages deploy dist"
