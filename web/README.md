# The web port — WebAssembly + WebGL

The game, in a browser tab, reading the player's own Xbox disc. `src/` is
compiled **verbatim**: the whole divergence is this directory, three
`#ifdef __EMSCRIPTEN__` blocks in `src/burnout3_full.c`, the guarded present
path in `src/burnout3_postfx.c` (see "The present chain on the web" below),
one include swap in `src/burnout3_scenery.c`, an ABI pin in
`src/burnout3_vehicle_sim.h`, and a pread path in `tools/cextract/cx_src.c`.

## Build

```sh
web/fetch_deps.sh          # a no-op now: the web port has no third-party deps
make wasm                  # -> build/web/burnout3.js + burnout3.wasm
make serve                 # http://127.0.0.1:8080/web/  (COOP+COEP)
```

Toolchain: **Emscripten 6.0.8** (clang 24, node 24.19.0), `~/emsdk`.

> **On the frames this document cites.** Several sections name a capture under
> `docs/web/` — pinned comparison frames, and the smoke logs behind them. Those
> render the retail game's own artwork, so they are not shipped here: they are
> cited by filename, with the measurement stated in the text, the same way the
> `docs/RE_*.md` records cite `REFERENCE IMAGES/`. Every number below stands on
> its own without the picture.

The page **must** be served with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Without both there is no
`SharedArrayBuffer`, and without that there are no threads, no image bridge and
no game. `tools/webserve.py` sets them.

## Three things about this target that are not obvious

Each of these was **measured**, not assumed, and each one dictated a design
decision. The probes are in the commit message; the conclusions are here.

### 1. The filesystem lives on the main thread, not the game thread

`-sPROXY_TO_PTHREAD` runs `main()` on a worker. Emscripten's JS filesystem
does **not** move with it:

| | |
|---|---|
| `preRun` ran on | main thread |
| `main()` ran on | worker |
| the worker's own `FS` object | **empty** |
| the main thread's `FS` object | holds everything |
| C `fopen()` from the game thread | **works** |

i.e. every file syscall the game makes is proxied to the main thread. So:

* **IDBFS belongs in `preRun`** — it is a main-thread filesystem and
  `FS.syncfs` is a main-thread call. That is exactly where it is mounted.
* **WORKERFS is impossible.** It reads a `File` with `FileReaderSync`, which
  exists only on a worker — but the read would execute on the main thread,
  where `FileReaderSync` does not exist. It asserts on mount, and would be
  useless if it did not.

### 2. So the disc does not go through the filesystem at all

A dedicated helper worker owns the `File` and serves reads straight into the
**shared wasm heap** — under pthreads, wasm linear memory *is* a
`SharedArrayBuffer`, so both sides address the same bytes and nothing crosses a
`postMessage` copy. The handshake is a six-word control block and wasm atomics:
the game thread blocks in `memory.atomic.wait32` exactly where it would have
blocked in `pread()`.

Nothing is copied and nothing is downloaded: the 2.4 GB image is read lazily
off the user's disk, a few hundred KB at a time — the same pages the native
`mmap` would have touched.

This is also why `cx_src.c` needed a pread path at all: its XISO backend was
one `mmap()` of the whole image plus pointer arithmetic, and a 2.4 GB mapping
cannot exist in a 4 GB address space. That path is proven byte-identical to the
mapping across all 716 files and 55 directories of the retail image.

### 3. SDL2 cannot make the GL context, and cannot open the audio device

**GL.** The game thread is a worker. A transferred OffscreenCanvas renders
there perfectly well — and **is never presented**, because presenting one has
no explicit call any more: `OffscreenCanvas.commit()` was removed from
browsers, `emscripten_webgl_commit_frame()` is a documented no-op without it,
and the implicit swap happens *when the thread returns to its event loop*.
This game is a blocking `while (g_running)` loop on that worker. It never
returns.

That argument is sound as far as it goes, and for a while it put the context
**on the main thread, proxied** — which turned out to be what was actually
wrong with this port's frame rate. What the argument misses is that **a canvas
does not have to be presented to be read**. So:

> **The context is created on the game worker, on a worker-local
> `new OffscreenCanvas` that nothing ever composites.** Each frame is
> snapshotted with one `transferToImageBitmap()` and `postMessage`d — 
> transferred, not copied — to the main thread, which drops it into a
> `bitmaprenderer` context on the visible canvas inside a `requestAnimationFrame`.
> **One message a frame** replaces ~26 000 cross-thread GL dispatches.

A context created on this thread is a context this thread **owns**, and that is
the whole point: Emscripten decides direct-vs-proxied *per call, at run time*,
from a TLS flag that `emscripten_webgl_make_context_current()` sets when the
handle's owning thread is the caller (`system/lib/gl/webgl1.c`).
`GL.registerContext()` stamps that owner as whoever calls it. Nothing in gl4es
or in the engine changes; every call simply stops crossing.

The proxied path is **kept as the fallback** for a browser with no
OffscreenCanvas or no `bitmaprenderer`, and `B3_WEB_PRESENT=proxy` forces it,
which is how the before/after in
[`docs/web/webprof_sweep.md`](../docs/web/webprof_sweep.md) comes out of one
binary. The port takes over seven SDL calls — context, swap and drawable size
for GL, and open/pause/lock/unlock for audio — **as macros in `b3_web.h`**, so
not one line of the frame loop changes.

> The shell's canvas element **must** carry `id="canvas"`: that is the selector
> the present target and the proxied fallback's context are both resolved by.
> `web/pre.js` adopts the shell's canvas into that id rather than demanding it.

**What proxying cost, measured.** `B3_WEB_GLBENCH=1` times Emscripten's
dispatch on calls chosen to do nothing in the driver:

| dispatch | proxied | direct |
|---|---|---|
| one async GL call (`glDisable`) | **1.21 µs** | **0.24 µs** |
| one sync GL call (`glIsEnabled`) | **39.2 µs** | **0.24 µs** |

A race frame makes ~26 500 WebGL calls, ~6 100 of them in Emscripten's
synchronous class — `glDrawElements` and `glVertexAttribPointer` among them,
which is to say the calls gl4es makes on *every batch*. `B3_WEB_GLCOUNT=<n>`
prints that census, split by pass.

**And it was not only slow — it was wrong.** Emscripten dispatches
`glBufferSubData` asynchronously, passing a *pointer into the wasm heap*; the
main thread dereferences it later, by which time gl4es has refilled the scratch
buffer for the next draw. That race is visible: on the proxied path the HUD
plates tear (`docs/web/hud_plate_defect.png`). On the direct path they do not.

## Measuring it — and setting a knob from a browser

**Every `B3_*` environment variable reaches the shell through the query
string.** `web/index.html?B3_WEB_HWPROF=60&B3_TRACK=US_C3_V1` and so on; only
names matching `/^B3_[A-Z0-9_]+$/` are accepted, because a query string is
attacker-controlled in any link somebody can be sent. This exists because
until it did, every diagnostic in the port was reachable from a test and
unreachable from a browser — which is backwards, since the person who needs
the profiler is the person watching the frame rate.

**`B3_WEB_HWPROF=<n>`** is the one to reach for. Three lines every *n*
presents: where the frame's wall time went, what crossed to the driver, and
how much data went up. It is off by default and installs no wrappers when off.
The full reading guide, and a decision tree for a machine this repo cannot
reach, is [`docs/web/webprof_sweep.md`](../docs/web/webprof_sweep.md) wave 5.

```
[hwprof]   frame 33.63 ms (29.7 fps) = sim 8.05 + render_frame 21.49
           (of which the postfx grab 0.02) + present 0.14
           (bitmap 0.05 + postMessage 0.06) + loop 3.94 | gpu 0.03 ms
[hwsync]   per frame: readPixels 0.00 | copyTex 1.00 (300 kpx) | getError 0.00
           | getParameter 21.00 | finish/flush 1.00 | bindFramebuffer 0.00
[hwupload] per frame: texture 0.27 uploads (2 KiB) | buffer 274 writes (5744 KiB)
```

Companion knobs: `B3_WEB_HWPROF_SYNC=1` adds a timed `glFinish()` before each
present, which is the only way to see GPU time on this target (Chrome exposes
`EXT_disjoint_timer_query` and never services `TIME_ELAPSED` — on a WebGL 2
context with the `_webgl2` extension just as much as it did on WebGL 1; see
"The context is WebGL 2" below) — and
it is a real pipeline sync, so it makes the frame slower while it measures it.
`B3_WEB_GRAB_MASK=0` and `B3_VBO_ORPHAN=<mask>` turn off the two fixes wave 5
made, so both A/Bs come out of one binary.

**And the first line to read is neither of those.** The port prints, at boot:

```
[Burnout3] web: gpu = <renderer>  ctx=webgl2 readFormat=... timerQuery=...   <- THE GAME THREAD'S CONTEXT
[Burnout3] web: gpu = <renderer>   (the page's own, for contrast)
```

The engine draws through a worker-local OffscreenCanvas, so the renderer string
the *page* reports is not the one that matters, and a browser is free to back
the worker's context with software while the main thread's is on the GPU. If
those two lines disagree, nothing further down is worth reading until they
agree.

## The present chain on the web

`B3_WEB_PROF=<n>` prints a frame-time breakdown every *n* frames:

```
[webprof] frame 377 ms = scene+hud 136 + blur 184 + gamma 56 + present 1.1
```

That line is why two things differ here from the desktop build. Both are
guarded (`#ifdef __EMSCRIPTEN__`); the native path is untouched. The measured
sweep behind both decisions — and the one number that says where the *real*
ceiling is — is in [`docs/web/webprof_sweep.md`](../docs/web/webprof_sweep.md);
the side-by-side frames are `docs/web/present_compare.png` and
`docs/web/frame_desktop_vs_web.png`.

### The radial speed blur is OFF by default

The present composite is `out.rgb = 2 * (T0.rgb + C0.a * T1.rgb)`, and retail
builds `T1` from a **160x120 box-filtered reduction** of its 640x480 render
target — mip level 2. This port selected that level with
`GL_TEXTURE_BASE_LEVEL` on a driver-generated chain, which an NPOT texture on
GLES2 / WebGL 1 may not have, so the web build had to drop it and the 16 taps
fell back to level 0: sixteen *sharp* offset copies of the frame, summed. Not
a smear — sixteen frames at once, which is exactly what it looked like.

With `C0.a = 0` the composite collapses to `2 * T0.rgb`, and `T0` **is** the
back buffer, so the frame grab and the opaque step-1 repaint are an identity
round trip. The web path draws step 3 alone: one untextured `DST_COLOR/ONE`
quad. Same tonality, no smear, and the grab plus the 16-tap pass are gone.

`B3_WEB_BLUR=1` puts the blur back **with the right prefilter** — an explicit
1/4-res FBO built by two bilinear halvings, which is the same pair of 2x2 box
filters that reach level 2. `B3_WEB_BLUR=2` reproduces the full-res-tap
ghosting on purpose, so the A/B can be looked at rather than described.

### The internal render resolution is pinned to 640x480

SDL2's Emscripten video driver sees that CSS is sizing the canvas
(`#canvas { width: 100%; height: 100% }`), and sets the canvas **backing
store** to the CSS size — so the port rendered at whatever the letterbox
happened to be, up to a full display's worth of pixels. `b3_web_pin_resolution()` sets the backing store *and*
`SDL_SetWindowSize()` together — both, because `render_frame()` takes its
viewport, projection aspect, HUD scale and postfx grabs from
`SDL_GetWindowSize()`, and SDL scales mouse events by `window->w / css_w`.
`object-fit: contain` in the shell's CSS pillarboxes the 4:3 drawable inside
the 16:9 frame. `B3_RES=WxH` overrides.

### A resize must not cost the frame rate — the one-pixel cliff

A user held 60.0 fps for 37 s of racing and then hit this, mid-race, with
nothing resized and no window touched:

```
[Burnout3] web: render resolution 1921x1080 (viewport changed)
[afx] unavailable: target scene 1921x1080 incomplete (status 0x8CD9)
[afx] unavailable: target scene 1921x1080 incomplete (status 0x8CD9)   ... every frame
```

and raced out the rest of the session on the legacy postfx path at ~20 fps.
**Three** separate defects, all fixed:

**1. The measurement wobbled.** `b3_web_viewport_px()` is
`(int)(css_px * dpr + 0.5)` and neither input is an integer —
`getBoundingClientRect()` reports fractional used sizes, and
`devicePixelRatio` is fractional under browser zoom and during a fullscreen
transition. Worse, the **pixel budget** (`B3_WEB_MAX_PX`) rescales anything
over 1920×1080 by `k = sqrt(MAX_PX/px)` and rounds each axis independently, so
a box a shade over budget lands on an **odd** width: 1921×1080 is a fixed point
of that arithmetic, and it is not even under the budget it was applied to
enforce. Transcribing the function and crawling `dpr` across ±0.1 % on a fixed
1920×1080 box fires **4 rebuilds**; crawling a slightly-over-budget box by
0.05 px at a time fires **5**, the first of them at exactly 1921×1080. The
defence is `B3_WEB_RES_QUANTUM` (snap the measured size down to a multiple of
4) plus `B3_WEB_RES_DEADBAND` (the every-30-presents follow ignores a
measurement that has not moved 8 device pixels on some axis). Both crawls now
fire **1**; a genuine 1080p→720p→1080p resize still fires all 3.

**2. The rebuild was broken at every size, not just odd ones.**
`afx_resize()` re-allocated the scene target's colour texture and checked
framebuffer completeness *before* `afx_scene_depth()` resized the depth
renderbuffer — so from the second build onward the check saw a new-size colour
attachment next to an **old-size depth renderbuffer**. GLES2/WebGL 1 require
every attachment to share dimensions and answer
`GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS` — **`0x8CD9`**, which is *not*
`INCOMPLETE_MULTISAMPLE` (`0x8D56`), and reading it as the latter sends you
hunting the MSAA pair, which was never stale. GL 3.0 dropped the rule, which
is exactly why the desktop never showed it: the same sweep passes on desktop
GL 4.6 with the bug still in. The fix detaches depth before the colour is
resized, restoring the first build's conditions for every build.

**3. The failure retried and printed every frame.** A failed `afx_resize()`
left `g_ready` at 1, so the next frame asked the same losing question and
printed the same line — a proxied `console.log` on the frame loop, 60 times a
second, which is a performance defect on top of the one it was reporting. A
failed rebuild now walks a ladder once: retry with MSAA retired, then retire
the whole chain for the run and say so once.

`tools/web_resize_sweep.py` is the gate (see Testing).

### …and the measurement must not stall the frame loop either

Found while fixing the above, and it explains a *different* symptom the same
user reported — real fps dropping to ~19.7 for a few windows and then
recovering completely, with nothing changed.

`b3_web_defend_pin()` asked the browser for the canvas' box every 30 presents,
**from the worker the game loop runs on**, through two calls that are both
`__proxy: 'sync'` in emsdk's `src/lib/libhtml5.js`:

```
emscripten_get_element_css_size__proxy: 'sync'
emscripten_get_device_pixel_ratio__proxy: 'sync'
```

So twice a second the game loop **blocked** until the main thread serviced the
request — and `g_real_fps`, measured on the worker over a 30-frame window,
absorbed whatever else was in the main thread's queue: a burst of proxied
`console.log` (FULL SLAM lines, crash-trace chatter), an OPFS cache write, a
GC. A main-thread hiccup the renderer never touched became a frame-rate dip.

The read is now **pushed, not polled**. `b3_web_viewport_watch()` (in
`b3_web_lib.js`) runs once on the main thread, installs a `ResizeObserver` on
the canvas plus a re-armed `matchMedia('(resolution: Xdppx)')` for
`devicePixelRatio` (there is no dpr event; re-arming the resolution query is
the standard trick), and writes the fractional box into a shared-heap slot
under a seqlock. The worker reads four `int32`s out of its own memory and
never blocks. A browser without `ResizeObserver` keeps the old path and says
so on the log.

`B3_WEB_VP_POLL=1` forces the old blocking path and `B3_WEB_VPPROF=1` times
the measurement, so the before/after is one binary and one env apart.
Measured that way, headless Chromium/SwiftShader, racing US_C1_V1, two
consecutive ten-poll windows each:

| | mean | **max** |
|---|---|---|
| blocking main-thread poll (`B3_WEB_VP_POLL=1`) | 0.3495 / 0.2465 ms | **0.7700 / 0.3401 ms** |
| pushed slot (default) | 0.0020 / 0.0030 ms | **0.0051 / 0.0051 ms** |

Roughly **100–150×**, and read the *max* column: that is the one that lands on
a single frame. Note also what this measurement is *not* — the headless main
thread is nearly idle, which is the old path's **best** case. Its cost is a
main-thread queue round trip, so it grows with whatever the main thread
happens to be doing, and the reported dip happened while the main thread was
busy with proxied `console.log` and cache work. The pushed slot has no such
term at all: four atomic loads out of the worker's own heap.

Worth knowing for anything else on this loop: **`__proxy: 'sync'` on the game
worker is a frame-loop stall**, and Emscripten marks far more of `html5.h`
that way than is obvious. Grep before calling one per frame.

### MSAA: on the scene target, not on the canvas

The **canvas'** `antialias` attribute is off and stays off, for the reason it
always was: the direct path's OffscreenCanvas is snapshotted, never composited,
and on the proxied fallback `renderViaOffscreenBackBuffer` draws the scene into
a plain single-sampled FBO — so `antialias` would only ever multisample the one
full-screen quad the aftereffects chain blits at the end of the frame, which
has no interior edges. `B3_WEB_CANVAS_AA=1` puts that back for an A/B.

**The scene's MSAA is real and is elsewhere.** `src/burnout3_aftereffects.c`
draws the world into a multisampled colour+depth renderbuffer pair and resolves
it into the chain's scene texture with one `glBlitFramebuffer`; every pass after
that samples the RESOLVED texture, so nothing in the chain ever samples a
multisample surface. That needs `glRenderbufferStorageMultisample` +
`glBlitFramebuffer`, which is why the context is WebGL 2 (below).

`B3_MSAA=N` is the one knob and it means the same thing on both targets — the
scene target's sample count, default 4 (`B3_AFX_MSAA_DEF`). `B3_MSAA=0` turns
multisampling off everywhere, which is what `tools/web_smoke.py`, the Android
port and the headless suites set. `B3_AFX_MSAA=N` overrides it for the chain
alone.

`docs/web/msaa_scene_target.png` — the same pinned `US_C3_V1` frame 900 on both
targets, 0x and 4x, magnified 20x nearest-neighbour so a pixel stays a pixel.
The gantry beam, its mast and the signal housings go from a binary step to
graded coverage, identically on both. Whole-frame: 4x moves **12.55%** of
desktop pixels and **14.08%** of web pixels, and the two agreeing to a point
and a half is the desktop/web symmetry this was for.

Measured on an RTX 3090 through ANGLE/Vulkan in headless Chromium, 1920x1080,
`US_C3_V1`, median of the steady-state `[hwprof]` samples:

| | frame | render_frame | GPU backlog (timed `glFinish`) |
|---|---|---|---|
| `B3_MSAA=0` | 16.67 ms (60.0 fps) | 5.82 ms | 0.010 ms |
| `B3_MSAA=4` | 16.67 ms (60.0 fps) | 5.78 ms | 0.020 ms |

i.e. the cost of 4x at 1080p on this class of GPU is below this harness' own
run-to-run spread (repeat runs of the *same* config move `render_frame` by
about ±1 ms), 60 fps is held either way with ~7.4 ms of loop headroom, and the
GPU is idle in both. **The default is 4x.**

> **Known, not fixed: the in-race HUD plates render as white noise.** The
> POS / LAP / speedo / boost plates come out as horizontal white streaks over
> a block of 1-texel noise, and they eat the digits drawn on top of them
> (`docs/web/hud_plate_defect.png`, the same frame on both targets). Web only,
> and it predates this work — it is in `docs/web/race_postfx.png` too. Narrowed
> so far: every affected element is one of the `hud_element01` plates in
> `src/burnout3_hud.c` (`plate_3slice` / `boost_tread`); the art itself is
> fine (64x32 RGBA, power-of-two, a dark blade on blue — nothing white in it),
> the same font renders the track-select screen perfectly, and the noise is
> drawn *over* the digits, so it is a later draw and not the plate texture's
> content. Nothing about it is postfx, and it is not the ghosting the present
> chain was causing.
>
> **FIXED (2026-08-25): the depth buffer was 16-bit.** It no longer is — see
> "The context is WebGL 2" below. The old note read: *Emscripten's offscreen
> framebuffer hard-codes `DEPTH_COMPONENT16` (`libwebgl.js`
> `resizeOffscreenFramebuffer`, with a `TODO` next to it), so the 24-bit depth
> `main()` explicitly asks for does not arrive on this target. Road decals lift
> ~0.011 units above the road, which 16 bits stops resolving at ~11 m. Fixing
> it means the scene rendering into an app-owned FBO with a depth texture
> (`WEBGL_depth_texture`) or a WebGL 2 `DEPTH_COMPONENT24` renderbuffer.* The
> second route is the one taken: the scene already renders into an app-owned
> FBO (the aftereffects chain), and on a WebGL 2 context that FBO's
> `DEPTH_COMPONENT24` renderbuffer is accepted on the first try. The boot line
> `[afx] chain ready ... depth 24-bit` is the receipt.
>
> `docs/web/depth_16_vs_24.png` — pinned `US_C3_V1` frame 900, 14x, a roadside
> billboard whose poster decal sits a hair in front of its backing panel. At 16
> bits the backing wins in vertical stripes and eats the poster; at 24 it
> resolves. Whole-frame the change is 1.20% of pixels, and the band profile is
> the signature of a depth fix rather than a rendering change: rows 60–240 (the
> distance) move by 2.2–3.1%, rows 300–480 (the near field) by 0.005–0.19%.
> Note what it is *not*: the road **paint** in the same frame is unchanged —
> the beneficiaries are lifted decals and coplanar geometry at distance.

**Audio — the image bridge, reversed.** SDL2's Emscripten backend builds a
WebAudio graph on whichever thread opens the device, and `AudioContext` is a
window-only interface that **does not exist in a worker**. SDL's own `EM_ASM`
throws and takes the thread down. No flag moves it, and for a long time the
answer was to answer `SDL_OpenAudioDevice` with `0` and run silent.

But it is not the mixer that cannot live on the worker — **only the sink**. So
the sink moves and the mixer does not:

> A dedicated pthread on the game side calls `audio_callback()` **verbatim**,
> with the spec's own 1024-frame buffer, under the same lock `fe_play()` takes
> — SDL's audio thread in every respect the engine can observe. Its 44.1 kHz
> mono S16 output is converted to float and written into a **ring in the wasm
> heap, which under pthreads is a SharedArrayBuffer**. An
> `AudioWorkletProcessor` on the main thread is handed the same buffer and the
> ring's byte offsets, and reads it from the audio rendering thread at the
> context's own rate and quantum.

Nothing crosses a `postMessage`; the two cursors are plain wasm atomics, one
writer and one reader. The worklet **never waits** — it is the audio thread,
where `Atomics.wait` is forbidden outright — so a short ring is answered with
silence *and counted*, which is what the underrun number in the log line is.

*The sample rate is the worklet's problem.* The engine mixes at 44100 and
cannot be moved off it (`g_eng_phase`, `b3_music` and the `fe_` `SDL_AudioCVT`
conversions all have 44100 compiled in), while the browser hands out whatever
its device runs at — 48000 on every machine measured. The ring is 44100 and the
worklet reads it with a fractional cursor stepping at `44100/sampleRate`. One
resampler, at the one place that knows the real rate.

*Why a pthread and not the frame loop.* A frame that takes 200 ms — a track
load, a GC pause, a materialise off the disc — would be 200 ms of silence if
the pump rode on it, and those are exactly the moments this port has. The pump
sleeps in its own thread and tops the ring up to a fill target instead. That is
also what makes a **hidden page** safe: a suspended context stops draining, the
ring reaches its target, and the pump stops producing. Bounded by construction
— there is nothing to leak. (`PTHREAD_POOL_SIZE` is 5 rather than 4 for it.)

**Measured**, headless Chromium, SwiftShader, a 30-race-second autodrive run:

| | |
|---|---|
| ring | 8192 frames capacity (185.8 ms), target 3072 (69.7 ms) |
| observed fill | 71–93 ms, sawtooth — the pump's own 1024-frame granularity |
| context | 48000 Hz, quantum 128, `44100/48000 = 0.9187` in per out |
| **latency** | **~83.0 ms** = ring 69.7 + quantum 2.7 + context `baseLatency` 10.7 |
| underruns | **0**, over the whole run; 0 silence frames |
| worklet RMS | 0.105–0.166, peak 0.194 — audible throughout |

The port prints that budget itself at boot, broken into its three parts,
because only the first is ours: the ring target is the one number tuning can
move, and `baseLatency` is the browser's floor.

and over a **180-race-second soak** (98 windows, 490 s of audio): ring
70.0–92.7 ms with no drift end to end, **0 underruns, 0 silence frames, no
zero-RMS window**, wasm heap peak unchanged from the 30 s run.

> **The one underrun ever observed**, and it is worth naming because it is not
> the bridge's: a single 210 ms gap (79 quanta) in one run, in the same window
> as `web: cache -> OPFS (1320 files, 341.8 MiB)`. That is the post-load cache
> flush, a several-hundred-megabyte write **on the main browser thread** —
> already the known hazard `b3_web_lib.js` debounces for. Soak runs whose
> flushes were 10–89 MiB had none.

`B3_WEB_AUDIO_STATS=<seconds>` sets the reporting interval (0 silences it);
`tools/web_smoke.py` gates on those numbers, because there is no way to *listen*
to a headless run and an `OfflineAudioContext` is a different context from the
live one the worklet is attached to. It also passes
`--autoplay-policy=no-user-gesture-required`: a suspended `AudioContext` never
pulls its worklet, and without the flag the gate would be measuring Chrome's
autoplay policy rather than this port. Measured both ways on the same page:
**0 quanta vs 480 in 1.5 s.** (The *real* shell needs no such flag — it unlocks
the context inside the PLAY click, and `tools/web_shell_smoke.py` shows the
bridge coming up on that path with no test-only help.)

`make test-audio-ring` runs the consumer **on its own, in a second, with no
browser and no ISO**: `tools/validate_audio_ring.js` lifts the processor out of
`b3_web_lib.js` by the same delimiters the build uses and runs it against a
`SharedArrayBuffer` laid out as `b3_web.c` lays it out. A full-scale sine must
come back at RMS `1/√2` — a broken interpolator, a wrong ring mask or an
off-by-one index all still make *sound*, and all of them move that number — the
cursor must step at `mixRate/sampleRate`, a starved ring must count rather than
throw, and the index must survive the int32 counter wrap.

**EA TRAX on the web: no.** The soundtrack lives in the two `_EATrax*.xwb`
banks as WMA, and the native path decodes it with `ffmpeg`, which cannot run
here. WebCodecs was the obvious candidate and was probed directly in Chrome:
`AudioDecoder` exists in a secure context, but `isConfigSupported` returns
**false for every WMA spelling** (`wmav1`, `wmav2`, `wmapro`, `wma`,
`audio/x-ms-wma`) while `opus`, `mp3`, `mp4a.40.2` and `pcm-s16` all return
true. The decoder cannot even be *configured* for WMA, so there is nothing to
wire up. Everything else — engine, SFX, crash beds, front-end cues — comes off
the disc and plays; music is the one line of notice the port already prints.

## GL: direct WebGL, and why gl4es is gone

**There is no compatibility layer in this build any more.** `<GL/gl.h>`
resolves to [`web/GL/gl.h`](GL/gl.h), which is `<GLES2/gl2.h>`; every entry
point the engine calls is core GLES2 under the same name, so Emscripten's own
WebGL bindings satisfy the link with nothing in between.

### The context is WebGL 2, with a real WebGL 1 fallback

It was pinned to WebGL 1 while gl4es was underneath, because handing gl4es a
GLES 3 context would have changed the path under it for no gain. gl4es is gone,
and three things this port wants are WebGL 2-only:

* **`glBlitFramebuffer` + `glRenderbufferStorageMultisample`.** No WebGL 1
  extension provides them, and without them the aftereffects chain runs
  single-sampled. This is what MSAA on the web is waiting on.
* **`DEPTH_COMPONENT24` as a renderbuffer format.** WebGL 1 guarantees only
  `DEPTH_COMPONENT16`; the chain's scene FBO now takes 24 on the first try.
* **`EXT_disjoint_timer_query_webgl2`**, which is the only timer query Chrome
  offers on a WebGL 2 context. See the caveat below before believing in it.

**The GLSL needed no change at all.** Every shader in this port is ESSL 1.00 —
no `#version`, `attribute`/`varying`/`texture2D`, `gl_FragColor` — and WebGL 2
accepts ESSL 1.00 unchanged; it *adds* ESSL 3.00 rather than withdrawing 1.00.
`-sFULL_ES2` went with gl4es and is still not needed: the retained renderer
draws from static VBOs, never client arrays. The link takes exactly one new
flag, `-sMAX_WEBGL_VERSION=2`, which is also what makes Emscripten's own
`emscripten_webgl2_get_proc_address()` answer for the two entry points above
(they sit behind `#if MAX_WEBGL_VERSION >= 2` in `system/lib/gl/gl.c`).

`-sMIN_WEBGL_VERSION=1` stays, so the WebGL 1 bindings are linked too and the
fallback is a real path. Emscripten does **not** fall back on its own; the port
does it by hand, in both `b3_web_gl_create_context()` (the visible canvas) and
`b3_web_worker_ctx()` (the worker-local OffscreenCanvas, a fresh canvas per
attempt). Either outcome announces itself at boot:

```
[Burnout3] web: WebGL 2 (asked for 2) -- the MSAA resolve and DEPTH_COMPONENT24 the aftereffects chain wants are available
[Burnout3] web: WEBGL 1 FALLBACK (asked for 2, got 1) -- no glBlitFramebuffer, so the aftereffects scene target runs SINGLE-SAMPLED, and depth falls back to 16-bit
```

`B3_WEBGL=1` forces the fallback out of the same binary, which is how the
before/after is measured. It is also the neutrality control: the pinned web
frame under `B3_WEBGL=1` is **bit-identical** to the pre-WebGL-2 build's, so
everything the change moved is the two features above and not the plumbing.

**The timer query still does not service.** WebGL 2 gets the right extension
(`EXT_disjoint_timer_query_webgl2`, and the port now uses the core
`beginQuery`/`endQuery`/`getQueryParameter` path), but Chrome makes no
`TIME_ELAPSED` result available — 241 queries, nothing, on both SwiftShader and
ANGLE/Vulkan on an RTX 3090, headless. `B3_WEB_HWPROF_SYNC=1`'s timed
`glFinish` remains the only GPU-time number this harness can report. Do not
build a measurement on the query without re-checking that.

It used to be gl4es, and the reason is worth keeping because it is the measure
of what the renderer wave changed. The engine drew through the GL 2.1
**compatibility** surface — 42 `glBegin`/`glEnd` blocks, `GL_QUADS`, display
lists, the matrix stack, `glPushAttrib`, `glAlphaFunc`,
`glTexEnvi(GL_COMBINE)`, fog — and its shaders were GLSL 1.10 using
`ftransform()`, `gl_MultiTexCoord0`, `gl_TexCoord[]`, `gl_ModelViewMatrix`,
`gl_FogFragCoord`. None of that exists in WebGL, and Emscripten's
`-sLEGACY_GL_EMULATION` does not translate those shaders. gl4es re-implemented
the whole surface on GLES2 and rewrote the shaders on the way through.

The retained renderer emits **none** of it. Static VBOs, generic vertex
attributes, GLSL that takes its transform from a CPU-composed `uMVP`, and a
small set of state calls owned by the CPU state shadow in
`src/burnout3_render.h` — which is itself what replaced `glPushAttrib` /
`glPopAttrib`, because GLES2 has no attribute stack and rebuilding one out of
`glIsEnabled`/`glGetIntegerv` would have cost ~64 blocking round trips a frame.

Removing the layer is not only dead weight. gl4es sat between every draw and
WebGL: one scratch VBO per attribute, re-pointed per batch, plus a re-check of
the fixed-function state against the bound program to decide whether to compile
a shader **variant** (`fpe_ReleventState` / `fpe_CustomShader`,
`src/gl/fpe.c:1090`). With no fixed-function state left to check, all of that
was overhead on the one axis this port is bound by — WebGL calls per frame.

Two link options went with it: `-sFULL_ES2` (it existed because gl4es drew
immediate-mode geometry from *client-side arrays*, which WebGL has no such
thing as, at a heap copy and a buffer upload per draw) and the `libGL.a`
dependency itself. `-sGL_ENABLE_GET_PROC_ADDRESS=1` stays: `burnout3_render.c`
and the recovered carfx/postfx programs resolve their GL 2.0 entry points
through `SDL_GL_GetProcAddress`.

The symbol-collision problem went too. gl4es mangled its exports to `gl4es_gl*`
for `__EMSCRIPTEN__` (`USE_MGL_NAMESPACE`, 1959 defines in `gl_mangle.h`)
precisely *because* Emscripten's own libGL.js already defines `glBindTexture`
and friends. Those definitions are now the ones the engine links against, and
the `SDL_GL_GetProcAddress` → `gl4es_GetProcAddress` override in
render/carfx/postfx/trackmesh is Android-only.

`src/burnout3_scenery.c` still includes `<SDL2/SDL_opengl.h>` after
`<GL/gl.h>`; under gl4es that was a hazard (it declares GL 1.1 *itself* rather
than including `<GL/gl.h>`, and so would have introduced a second, unmangled
set of names bypassing the layer). With no layer to bypass it is simply a
second declaration of the same GLES2 functions.

**The Android port still uses gl4es** (`android/fetch_deps.sh`,
`android/app/src/main/cpp/CMakeLists.txt`): its GLES driver is real hardware,
its bring-up is tied to the EGL rebind dance, and nothing there is call-count
bound. That is the port to look at if this one ever needs a layer again.

## Filesystem layout the engine boots into

```
/app                  MEMFS, the CWD — stands in for the repo root, so every
                      literal "build/..." path in src/ resolves unchanged
/app/build/.isocache  IDBFS — materialise-on-miss writes here, synced to
                      IndexedDB after each burst that ran a stage
/app/src/*.h          embedded: several cextract stages parse this checkout's
                      OWN headers (car_tuning reads burnout3_physics_params.h)
/iso/game.xiso        a zero-byte placeholder — it exists only so the existence
                      checks in burnout3_isodata.c succeed; the bytes come
                      from the image bridge
```

`Module.b3CacheMode = 'memfs'` swaps IDBFS for MEMFS: nothing persists, and the
extracted assets stay in the wasm heap for the session (a boot plus one track
is order 100–200 MiB on top of the game's own usage).

**THE GAME'S OWN CONFIG DOES NOT PERSIST HERE YET**, and that is a known gap
rather than a surprise: `build/mixer.cfg` and `build/settings.cfg` are written
by `resolve_write()` to the real `build/` tree, which on wasm is `/app/build`
— outside the OPFS sync, which covers `/app/build/.isocache`. So the audio
mixer and the pause screen's `SETTINGS` block (the ray-tracing row) both work
within a session and both come back at their defaults on a reload. The fix is
to add the two config files to the sync list; it is not done here because it
is a change to the cache's contract and not to either feature.

**Ray tracing on the web**: it works, it is offered, and it costs nothing
extra to reach — the same ESSL 1.00 shader as the desktop, unchanged, under
WebGL 2, with `RGBA32F` + `NEAREST` data textures which are core there.
`docs/PHOTOREALISM.md` tier 4r carries the measurement (`--gl hw`, 32 in-race
windows at 1080p: every one at 60.0 fps with the option on or off).
`B3_RT_WEB_OPTION` in `src/burnout3_rt.h` is the one line that would hide the
row if a later measurement said to.

## Testing

Headless only — never a visible browser.

```sh
python3 tools/web_smoke.py        [--iso "<path to .xiso.iso>"] [--seconds 30]
python3 tools/web_shell_smoke.py  [--iso "<path to .xiso.iso>"] [--seconds 40]
python3 tools/web_cold_smoke.py   [--iso "<path to .xiso.iso>"]
python3 tools/web_resize_sweep.py [--iso "<path to .xiso.iso>"] [--mode both]
```

**`--iso` is optional and you should usually omit it.** All four follow the
engine's own ladder — `--iso` > `build/iso_path.txt` > `$B3_ISO` — and print
the image they chose. `build/iso_path.txt` is written by
`src/burnout3_isodata.c` on the first successful `--iso` run, so one correct
boot teaches every harness where the disc is. Pasting a path by hand is how
two separate agents lost the best part of an hour to a plausible-looking
**non-XISO** `Burnout 3 - Takedown (USA).iso`: the engine says

```
iso: /iso/game.xiso is neither an XISO image nor a game directory
     -- falling back to the pre-extracted build/ tree
```

and then dies minutes later on the first missing artefact
(`FATAL: build/frontend/font.bin -- no such file`), with every browser-side
gate healthy right up to that point. If you see that line, you have the wrong
image, not a broken build.

`web_resize_sweep.py` covers the resolution-change path, which nothing else
did — see "A resize must not cost the frame rate" above. `--mode sweep` drives
the engine through a list of render resolutions (`B3_WEB_RESIZE_SWEEP`, a test
hook in `b3_web.c`) including sizes the measurement can no longer produce
(1921×1080, 637×479), and requires an `[afx] chain ready WxH` line at every one
with zero incomplete framebuffers. `--mode wobble` leaves the viewport follow
on and nudges the page with **sub-pixel CSS sizes and fractional
`devicePixelRatio`** over CDP, requiring the engine *not* to change resolution.
`--webgl 1` forces the WebGL 1 context, whose framebuffer rules are the ones
that make `0x8CD9` reachable at all — run it that way to see the original
defect, and to prove the fix against the stricter of the two.

The desktop half of the same path has the same hook: `B3_RESIZE_SWEEP` in
`burnout3_full.c`, which works under `SDL_VIDEODRIVER=offscreen` (SDL's
offscreen driver honours `SDL_SetWindowSize` and its drawable follows).

Chromium `--headless=new` with SwiftShader for real (software) WebGL, driven
over CDP. The image is handed to the page through `DOM.setFileInputFiles`, so
it arrives as a genuine lazily-read `File` — the same object a player's file
picker produces, and the same code path.

`web_smoke.py` drives a bare test page and gates on log lines;
`web_shell_smoke.py` drives **the real shell** — clicks PLAY, hands over the
disc, waits, and gates on **pixels**, because a gate that stays green through
a black screen is worse than no gate.

**SwiftShader is not a frame-rate oracle.** It is a software rasteriser: fill
rate costs orders of magnitude more than on a GPU, so absolute fps from these
runs means nothing. What it *does* measure honestly is the RELATIVE weight of
the stages `B3_WEB_PROF` reports, and whether a change moved them.

**`--gl hw` is, though.** `--headless=new` does not imply SwiftShader: with
ANGLE's Vulkan backend, headless Chromium reaches the real GPU, and the port's
frame time can be measured on it without ever opening a browser.

```sh
python3 tools/web_smoke.py --iso "<image>" --gl hw --seconds 40
```

The default stays `--gl swiftshader`, because that is what every gate in this
repo is calibrated against. `--gl hw` is for measuring, and the boot line that
names the renderer (above) is what says whether it worked — a machine with no
usable GPU falls back to SwiftShader silently and the run still completes.
