# The web port's frame-time sweep

Every row is one headless run of the real shell (`tools/web_shell_smoke.py`'s
driver, extended to set the engine environment), same disc, same track
(`US_C3_V1`), same autodrive, 150 s each, numbers from the engine's own
`B3_WEB_PROF=30` line — the mean of the last six stable prints.

**SwiftShader, so absolute fps means nothing.** Chromium `--headless=new`
rasterises WebGL in software; what is comparable here is one row against
another, and the *shape* of the answer.

| # | internal res | MSAA | radial blur | frame (ms) | vs. shipped |
|---|---|---|---|---|---|
| b | 1280x720 | 4 | on, taps at FULL res (as shipped) | **370** | — |
| c | 1280x720 | 4 | **off** | 303 | −18% |
| d | 1280x720 | 0 | off | 301 | −19% |
| e | **640x480** | **0** | **off**  ← the new default | **272** | **−26%** |
| f | 320x240 | 0 | off | 259 | −30% |
| g | 640x480 | 0 | on, taps at 1/4 res (`B3_WEB_BLUR=1`) | 303 | −18% |
| h | 640x480 | 0 | on, taps at full res (`B3_WEB_BLUR=2`) | 313 | −15% |

A second sweep, all at the new default (640x480, no MSAA, no blur), on gl4es'
draw batching — the obvious lever once the answer above is "call-bound":

| # | `LIBGL_BATCH` | frame (ms) |
|---|---|---|
| i | unset (gl4es default: off) | **282** |
| j | `1` (merge draws up to 100 vertices) | 293 |
| k | `8` (up to 800 vertices) | 290 |

Batching is **not** enabled, because it did not help: both settings came out
3–4% *slower*, which is inside this harness's run-to-run spread but is
certainly not a win. Measured and dropped rather than assumed and shipped.

## What the rows say

**Removing the blur is the single biggest win** (b → c, −18%). It is also the
only change here that removes a *visual defect* rather than trading picture for
speed: see `present_compare.png`.

**MSAA was already doing nothing** (c → d, 0%). With
`renderViaOffscreenBackBuffer` the scene is drawn into a plain single-sampled
FBO and `antialias` only ever applied to the real canvas, whose entire contents
are one blit quad. Turning it off is free and principled, not measured.

**THE PORT IS NOT FILL-BOUND.** d → f drops the pixel count by **12x**
(921 600 → 76 800) and buys **14%**. `scene+hud` does not move at all across
that range — 139 ms at 720p, 146 ms at 240p. Whatever the frame costs, it is
not being paid per pixel.

That matters more than any row in the table, because it says where the ceiling
is: the WebGL context lived on the **main thread**, so
`emscripten_GetProcAddress` handed gl4es the *proxying* wrappers from
Emscripten's `system/lib/gl/webgl1.c` and **every GL call this renderer made
crossed a thread boundary**.

---

# The second wave: unproxying the context, and counting the calls

Everything above says "call-bound" as an *inference*. This section turns it
into a census, removes the boundary, and says exactly what is left.

## 1. The census — what a race frame actually asks WebGL for

`B3_WEB_GLCOUNT=<n>` wraps every method on the live `WebGLRenderingContext` and
bumps a bucket in the shared wasm heap, so the number is of **real WebGL
calls**, at the exact point where a call stops being the port's business and
becomes the browser's. The same probe measures the proxied build and the direct
one without changing.

`US_C3_V1`, mid-race, mean of the stable prints:

| bucket | calls / frame |
|---|---|
| vertex (`bindBuffer`, `bufferSubData`, `vertexAttribPointer`, …) | **20 700** |
| draw (`drawArrays`, `drawElements`) | **2 190** |
| texture (`bindTexture`, `tex*`) | 1 520 |
| uniform | 1 750 |
| program | 160 |
| query / state / other | ~330 |
| **TOTAL** | **≈ 26 500** |
| — of which in Emscripten's **SYNCHRONOUS** dispatch class | **≈ 6 100** |

By name, the top of the distribution (`*` = sync-dispatched):

```
bindBuffer 8278, bufferSubData 6023, vertexAttribPointer* 5960,
drawArrays 2130, bindTexture 1501, uniformMatrix4fv 1471,
vertexAttrib4f 479, uniform1fv 226, useProgram 155, drawElements* 63
```

Two things fall straight out of that. **12.2 WebGL calls per draw**, because
gl4es keeps one VBO per attribute and re-points all three every batch. And
`vertexAttribPointer` — one of the three — is in Emscripten's synchronous
class, so under proxying **every batch blocked the game thread on the main
thread, three times.**

## 2. What the boundary cost, measured directly

The frame time cannot answer this on this harness: SwiftShader's per-call cost
is so large that it swamps the thread hop, which is the reverse of a real GPU.
So `B3_WEB_GLBENCH=1` times the dispatch itself, on calls chosen for what they
do *not* do — entry points taken from `SDL_GL_GetProcAddress`, so gl4es is not
in the loop:

| dispatch | proxied | direct | ratio |
|---|---|---|---|
| async GL call (`glDisable(GL_DITHER)`) | 1.214 µs | **0.240 µs** | 5.1x |
| sync GL call (`glIsEnabled(GL_DITHER)`) | 39.159 µs | **0.242 µs** | **162x** |
| `glGetError()` — a real WebGL pipeline sync | 215 µs | 137 µs | 1.6x |

`glGetError` is in the table to stop its own cost masquerading as proxy cost:
it is a genuine command-buffer round trip and costs ~137 µs with no proxying at
all. `glIsEnabled` is answered inside Chromium's command-buffer client without
reaching the GPU process, so the 39 µs → 0.24 µs is the thread hop and nothing
else.

Multiplied through the census, the boundary was costing, per frame:

```
6 100 sync   x 39.2 us  = 239 ms
20 400 async x  1.21 us =  25 ms
```

**Read that carefully.** 264 ms is larger than the whole SwiftShader frame, so
it cannot all be additive: in a saturated frame much of a sync call's wait
overlaps the main thread doing GL work that had to happen anyway. It is an
*upper* bound, and the honest statement of the mechanism is that the two
threads were **serialised** — the game thread could never run more than one
batch ahead of the compositor. The bound matters because on a real GPU there is
very little main-thread work to hide behind: the less the main thread has to
do, the closer each of those 6 100 waits gets to its full 39 µs.

## 3. And it was not only slow — it was WRONG

Emscripten dispatches `glBufferSubData` **asynchronously**, passing a raw
pointer into the wasm heap. The main thread dereferences that pointer later —
by which time gl4es has refilled the scratch buffer for the next draw. That is
a data race on every buffer upload, and it is visible: at the same pinned frame,
same binary, the only difference being `B3_WEB_PRESENT=proxy`, the HUD plates
tear.

Both frames are in `hud_plate_defect.png`'s family; the pinned pair here differ
on 18.6% of pixels, up to 246/255 on a channel, entirely in the HUD plates.
The pair is `present_proxy_vs_direct.png` — same binary, same pinned frame 600,
one environment variable apart. **The direct path is a rendering fix, not just
a speed-up.**

## 4. The fix

A **worker-local `new OffscreenCanvas`** — not the transferred DOM one. Nothing
ever composites it, so the "an OffscreenCanvas can only be presented when the
worker reaches its event loop" problem does not arise: it is snapshotted with
`transferToImageBitmap()` and the bitmap is `postMessage`d (transferred, not
copied) to the main thread, which hands it to a `bitmaprenderer` context on the
visible canvas inside a `requestAnimationFrame`.

The context is created *on the game thread*, so
`GL.registerContext()` stamps it as owned by that thread, and
`emscripten_webgl_make_context_current()` sets the TLS flag that makes every
one of `webgl1.c`'s wrappers take its direct branch. **Nothing in gl4es or in
the engine changed.**

| | proxied | direct |
|---|---|---|
| WebGL calls per frame **crossing a thread** | **26 500** | **0** |
| — of them blocking round trips | **6 100** | **0** |
| present (`B3_WEB_PROF` column) | 2.2 – 3.5 ms | **0.10 – 0.12 ms** |
| SwiftShader frame | 277 – 295 ms | 245 – 267 ms |

Backpressure is a two-deep in-flight cap in shared memory: over it, the frame
is dropped *before* the bitmap is allocated. Stale frames are dropped on the
main thread and closed; a painted one is consumed by
`transferFromImageBitmap()`, which takes ownership. Over a 5½-minute soak
(~1 300 presents) the in-flight count read **0 at every 20-second sample**, the
main-thread JS heap sat at 3.3 – 4.1 MiB with no trend, the wasm heap did not
move off 1017.7 MiB, and the frame time did not decay.

## 5. The second lever: the per-vertex call volume

The census is of calls *leaving* gl4es. Underneath it the engine was making a
far larger number of calls *into* gl4es: `trackmesh_draw_shine()` alone emits
`glColor3f` + `glTexCoord2fv` + `glVertex3fv` for every vertex of every
specular group, every frame — of the order of 200 000 calls a frame on
`US_C3_V1`. gl4es absorbs them into the same 2 190 batches, so they never
showed up in the census; they are pure CPU on the game thread.

Two passes were converted to fixed-function **vertex arrays** — GL 1.1, so the
desktop path is unaffected, and the same vertices in the same order, so the
picture is unchanged:

* `trackmesh_draw_shine()` and `trackmesh_draw_scroll()` fill one interleaved
  scratch buffer per group and issue one `glDrawArrays`.
* the sky dome (264 static vertices, drawn 3x through a 1 344-entry index list
  that already existed) became three `glDrawElements` off static arrays —
  8 064 calls a frame to about 15.

| | before | after |
|---|---|---|
| desktop render rate (offscreen, 640x480, `US_C3_V1`) | 156 – 161 Hz | **227 – 233 Hz** |
| web `scene+hud` (SwiftShader) | 125 – 138 ms | **117 – 126 ms** |
| WebGL calls per frame | 26 500 | 26 600 (unchanged — see below) |

The desktop number is the real measurement of what this bought: **+45% render
rate**, which is CPU the web build no longer spends either. The WebGL call
count did *not* move, and that is expected — gl4es was already collapsing
immediate mode into the same batches. What went away is everything above gl4es.

### The pixel gate, and one real trap in it

Desktop offscreen screenshot at a pinned frame (`SDL_VIDEODRIVER=offscreen`,
`B3_FIXED_DT`, `B3_SHOT_FRAME=600`), before vs after, `cmp`: **bit-identical**.

It was not, at first — 11 pixels moved by 2/255, all in one cluster on the
horizon. The cause was not the arithmetic, which is the same arithmetic: it was
that the shine pass's `glColor3f` is a **three**-component colour, and the
first version pointed a four-component colour array at it. The component count
is part of the vertex format the driver compiles its transform path from, so a
4-component array where the immediate path had a 3-component colour is a
different path with different rounding. `trackmesh_va_draw()` takes the
component count for exactly that reason.

The web has a pinned frame too now: `tools/web_smoke.py --pin-frame N` sets
`B3_FIXED_DT` and the engine's own `B3_SHOT` path, then reads the BMP back out
of the module filesystem — deterministic, and bit-comparable between two web
builds.

## 6. Where the rest of the frame is — the map for the next wave

`B3_WEB_GLCOUNT` also splits the census by render pass (`b3_web_glc_zone()`,
tagged in `render_frame()`; exact only on the direct path, because there a call
has executed by the time the next line of C runs):

| pass | WebGL calls / frame | draws / frame | share of calls |
|---|---|---|---|
| **track** (display list + scroll + shine) | **15 474** | **1 281** | **58%** |
| **scenery** | **5 886** | **523** | **22%** |
| props | 1 290 | 123 | 5% |
| hud | ~1 250 | ~95 | 5% |
| traffic | ~1 280 | ~105 | 5% |
| cars (bodies, glass, shadows, coronas, boost) | ~1 330 | ~66 | 5% |
| sky | 56 | 3 | 0.2% |
| particles | ~70 | ~4 | 0.3% |
| postfx (blur + gamma) | ~60 | 2 | 0.2% |

**80% of the remaining calls are the track and the scenery, and both are
draw-count-bound, not vertex-bound.** 1 281 track draws is very nearly the
track mesh's 990 material groups plus its ~250 shine groups plus its ~40
animated ones; 523 scenery draws is one per visible instance. At 12.2 WebGL
calls per draw, cutting the draws is the only thing that cuts the calls.

gl4es cannot do it: `LIBGL_BATCH=1-1000` moved the track from 1 281 draws to
1 273 — 0.6%, i.e. nothing — because gl4es can only merge draws whose state is
already identical and consecutive, and **every one of these groups rebinds a
texture.** Measured and dropped, again.

So the next 10x is a **content** change, not a driver knob:

1. **Merge the track's opaque groups by texture** at extraction time
   (`tools/cextract/cx_track_mesh.c`), or atlas the 122 track textures. 990
   groups against 122 textures is an 8x ceiling on the biggest single item.
   The risk to guard is draw ORDER: the blended and decal groups must keep
   theirs, and coplanar road decals will z-fight if opaque groups are
   reordered. The pinned-frame gate above is exactly the instrument for it.
2. **Instance or merge the scenery** — 523 draws for 32 distinct models is
   ~16 instances per model, each currently its own `glCallList` with its own
   matrix.
3. **`draw_text()` draws every string nine times** (eight shadow offsets plus
   the fill, `burnout3_hud.c:419`, and again at `:1344`). Collapsing that into
   one buffered draw is a 9x cut on the HUD for no visual change.
4. Cheap, no VBO needed: hoist `glGetFloatv(GL_MODELVIEW_MATRIX)` out of the
   per-car corona and boost-flame draws, hoist `state_begin`/`state_end` out of
   `b3_hud_opponent_tag`, and give `b3_props_draw()` the distance cull that
   `b3_scenery_draw()` already has.

## 7. What can and cannot be claimed about 60 fps

**Can:** the port no longer makes a single cross-thread GL call, the ~6 100
blocking round trips a frame are gone, the present costs 0.10 ms instead of
2.2–3.5 ms, and the HUD tearing that the proxy race caused is gone with them.
The measured per-call costs (39.2 µs → 0.24 µs synchronous, 1.21 µs → 0.24 µs
asynchronous) are scheduling costs, not GPU costs, so they transfer to real
hardware unchanged. The vertex-array conversion is worth a measured **+45%**
on desktop, and that is CPU the browser build saves too.

**Cannot:** this harness cannot measure real-hardware fps. SwiftShader is a CPU
rasteriser, and it is where ~250 ms of the ~250 ms frame goes — the gamma pass
alone, one full-canvas `glCopyTexSubImage2D` plus a full-screen quad, is
120–140 ms of it and would be sub-millisecond on any GPU. So the SwiftShader
frame time is *not* a proxy for the shipped frame time, and the fact that it
only moved ~10% across phase 1 says nothing about a real GPU, where the removed
cost is nearly the whole budget rather than 7% of it.

What is left to be careful about on real hardware is the ~26 500 WebGL calls a
frame. Chromium's per-WebGL-call cost is roughly 0.2–0.5 µs, so that is
**5–13 ms of a 16.7 ms budget** before the GPU does anything. Section 6 is the
plan for it, and it is the difference between "playable" and "60".

---

# The third wave: the LOAD, not the frame

Everything above is about a frame. None of it touched the several seconds
before the first one, which is what a player actually complained about:
"loading track geometry" sat there, the bar frozen, for minutes on a cold
visit.

## 1. The mechanism — a loader's cost is its read() COUNT, not its byte count

Two facts multiply:

* **musl gives every `FILE` a 1024-byte buffer.** `__fdopen.c` allocates
  `sizeof *f + UNGET + BUFSIZ` and sets `f->buf_size = BUFSIZ`, and Emscripten's
  `stdio.h` has `#define BUFSIZ 1024`. One `read()` per kilobyte.
* **Every `read()` from the game worker is a BLOCKING round trip.** Under
  `-sPROXY_TO_PTHREAD` the JS filesystem stays on the main browser thread, so
  the generated module opens `_fd_read` with

      function _fd_read(fd,iov,iovcnt,pnum){
        if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(91,0,1,fd,...);

  — proxy mode 1, synchronous. The game thread cannot continue until the main
  thread returns to its event loop and executes it.

So the number to measure is not megabytes, it is **reads**. Wrapping
`Module.FS.read` on the main thread counts them exactly, because the main
thread is where every proxied read is executed. `US_C3_V1`, warm cache,
`B3_IO_SMALL=1` (the old path, out of the shipped binary):

| file | proxied reads | bytes | the loop |
|---|---|---|---|
| `tracks/US_C3_V1/track.obj` | **31 488** | 16.1 MB | `fgets` into a 512-byte buffer |
| `tracks/US_C3_V1/light_probes.bin` | **10 988** | 6.2 MB | two `fread`s per triangle |
| `tracks/US_C3_V1/collision.bin` | **4 646** | 2.4 MB | one `fread` per 40-byte record |
| everything else (567 files) | 27 117 | 28.9 MB | PNGs through SDL_image, mostly fine |
| **whole boot** | **74 239** | 53.6 MB | |

(The count is `FS.read` calls, and musl's `__stdio_read` passes a two-entry
iovec, so a refill is two of them — 31 488 reads of `track.obj` is 15 744
round trips.)

## 2. The fix, and what it is worth

`trackmesh_load()` and `resolve_materials()` read the whole file in **one**
`fread` and split lines in memory; the two binary loaders keep their exact
`fread`-per-record shape and are simply given a 1 MB stdio buffer. (musl's
`setvbuf` *ignores* a NULL buffer, unlike glibc's, so the block is owned by the
caller and installed before the first read — swapping buffers mid-stream would
throw away what is already buffered.)

**`B3_IO_SMALL=1` restores the old path in all three**, so the table below is
one binary, one browser profile, two runs back to back — the same discipline
`B3_WEB_PRESENT` gave the present path.

| warm load, headless Chromium | `B3_IO_SMALL=1` | shipped | |
|---|---|---|---|
| TRACK GEOMETRY | 4.965 s | **1.622 s** | 3.1x |
| COLLISION | 0.193 s | **0.035 s** | 5.5x |
| LIGHTING (light probes) | 0.291 s | **0.176 s** | 1.7x |
| CAR MESHES | 3.146 s | **1.490 s** | 2.1x |
| **TOTAL load** | **11.318 s** | **5.284 s** | **2.1x** |
| **proxied reads** | **74 239** | **5 705** | **13x** |

Desktop does not pay for it and gains slightly — `US_C3_V1` offscreen, median
of three: TOTAL 1.167 s → 1.087 s, TRACK GEOMETRY 0.310 s → 0.276 s.

**What is left** is the parse itself. 1.6 s of wasm against 0.28 s of desktop
for the same 386k `sscanf` lines is a libc gap (musl's `strtod` against
glibc's), and closing it means replacing `sscanf` with a hand-rolled float
parser — which would not be correctly rounded, would move pixels, and is
therefore off the table. The next real win on this axis is a binary mesh
format, not a faster text one.

## 3. And the screen kept moving through it

`b3_iso_set_progress` covers the gaps BETWEEN materialisation units. Nothing
covered the inside of one long parse, so the loading screen held a single frame
for the whole track load — measured: two canvas samples three seconds apart,
inside one phase, byte-identical.

`trackmesh_set_pump()` is the other half of retail's pump: the OBJ parse calls
back every 8k lines with its byte progress, and `load_car_meshes()` declares one
sub-step per car so the bar crosses its phase over the fleet rather than
reaching the end on car one (`g_ls_frac` is a monotonic maximum — retail's rule,
`DAT_0075479C`).

**The pump needed a real ceiling, and this is the trap.** "One frame per 33 ms,
measured from the START of the last one" is true on *every* call once a frame
costs more than 33 ms, so the first version ran the screen at a 100% duty cycle:
the track parse went from 0.6 s of work to 4.1 s, of which 3.5 s was 49
SwiftShader frames at ~72 ms each. The budget is now a **share of the load**:
33 ms after the last frame ENDED, or 4x what that frame cost, whichever is
longer. Cheap frames (any real GPU) keep the full 30 Hz; expensive ones throttle
to a fifth of the wall clock.

`B3_LOADPROF=1` prints each phase's wall clock and the loading-screen frames
presented inside it. A phase that shows 1 is a phase that froze:

```
TRACK GEOMETRY      1.622 s     6 frames        (was 1)
CAR MESHES          1.490 s     9 frames        (was 1)
```

and the bar's own pixels, sampled straight off the canvas at 0.25 s inside the
track phase: `barLit` 680 → 820 → 860 → 870.

## 4. `texParameter: no texture bound` — not the engine's texParameter

The console carried `WebGL: INVALID_OPERATION: texParameter: no texture bound
to target`, four times, once, during load. The standing theory was that the
engine sets wrap mode as a global render state (a faithful D3D address-mode
idiom, harmless on desktop where name 0 is a real object).

**It is not that.** `B3_WEB_TEXAUDIT=<n>` wraps `texParameteri`/`texParameterf`,
checks `TEXTURE_BINDING_2D` and prints the wasm stack of every offender. All
four:

```
glTexParameteri <- realize_1texture <- realize_textures
  <- gl4es_glPushAttrib <- state_begin <- b3_hud_draw_rect_px
  <- b3_loadscreen_frame
```

No engine `texParameter` call is in it. gl4es DEFERS sampler state — the
parameter is recorded on the bound `gltexture_t` and applied later, from
`realize_1texture()`, for whichever texture is selected when a draw or a state
push forces a flush (`src/gl/texture_params.c`). Its texture-0 stand-in
`glstate->texture.zero` is `calloc`'d, so `actual` is all zeroes while `sampler`
holds the GL defaults, and the first flush that finds it selected emits exactly
four calls — MIN, MAG, WRAP_S, WRAP_T — against a target with nothing bound.
Once, because gl4es then records them as applied.

The engine's part is the trigger, and it is a real defect: five texture loaders
and two draw passes ended with a tidy-up `glBindTexture(GL_TEXTURE_2D, 0)`, and
the first loading-screen frame after `b3_carfx_init()` flushed it. **Name 0 is
not "no texture"** — desktop GL has a default texture object there, WebGL
deleted it. Those unbinds are gone. Nothing depended on them: every textured
draw here binds its own texture, and every untextured one says
`glDisable(GL_TEXTURE_2D)`, which is what "no texture" actually means.

Browser console 4 → **0**; the audit's own count agrees, including 0.00/frame
across a whole race.

## 5. The gates

Desktop pinned frame (`SDL_VIDEODRIVER=offscreen`, `B3_FIXED_DT`, frame 240),
pristine parent commit vs this one: identical **outside the EA TRAX banner**,
which is not a regression but a known nondeterminism — `b3_music_seed()` is
never called, so `burnout3_music.c:192` seeds the shuffle bag from `time(NULL)`
and two runs of the *same* binary differ there. Web pinned frame bit-identical
across the `B3_IO_SMALL` A/B. `validate_hud` 767/767, `validate_carfx` 270/270,
`validate_tracks --tracks US_C3_V1` 9/9, `web_shell_smoke` and `web_smoke`
green, `make` and `make wasm` with no new warnings.

---

# The fourth wave: the renderer stops re-describing the world every frame

Section 6 above ends with a plan and a number: **80% of the remaining calls are
the track and the scenery, and both are draw-count-bound**, at 12.2 real WebGL
calls per draw because gl4es keeps one scratch VBO per attribute and re-points
all three every batch. This wave does what that section asked for, and takes
the passes it names off the fixed-function surface entirely.

**There is now ONE renderer.** The migrated passes did not keep a
`B3_RENDER=legacy` toggle: each one's fixed-function code was deleted in this
branch the moment its pinned-frame gate went green, so the shipped tree has a
single route and this branch's history is the A/B reference. `src/burnout3_render.{c,h}`
is that renderer.

## 1. The state audit — what the fixed-function passes actually used

Before replacing them, every state combination the four passes issue was
enumerated from the source, and the material data was replayed to find how
many DISTINCT combinations the shipped track actually produces.

| pass | distinct state tuples | what varies |
|---|---|---|
| track base (990 groups) | **6** `(textured, blend, alpha-test, depthMask)`, **118** including texture identity | texture, `alpha_blend` (flag 0x001), `alpha_test` (flag 0x010), `decal` -> depthMask (flag 0x400) |
| track scroll (16 groups) | same 6 | plus a per-frame UV offset or a per-frame texture bind |
| track shine (291 groups) | **1** | additive ONE/ONE, depth writes off, `GL_COMBINE(MODULATE, TEXTURE.alpha, PRIMARY.rgb)`, black fog |
| scenery (32 models, 972 instances) | **3** | 9 models plain, 4 blended, 19 alpha-tested; cull off |
| props (7 models, 123 instances) | **2** | 6 plain, 1 alpha-tested; cull off |
| HUD (16 `glBegin` sites) | **4** | textured/not, `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` vs `SRC_ALPHA/ONE`, colour mask `0x010101` vs full RGBA |

Every world colour is a 4-component `glColor4fv` except the shine pass', which
is a 3-component `glColor3f` — a distinction that is not cosmetic, and that
the second wave already paid for once ("The pixel gate, and one real trap in
it", above: a 4-component array where the immediate path had a 3-component
colour is a different transform path with different rounding, worth 11 pixels
at 2/255). The retained shine pass points a `glColorPointer(3, ...)` at its
buffer for exactly that reason. All 990 US_C3_V1 groups resolve a
texture, so the untextured branch of the old bake was dead code; 990/990 carry
a material record, so the texel cut-out heuristic never fires.

The whole of that is four features: **texture modulate, one alpha test, one
linear fog, and one "texture alpha times primary rgb" combiner**. That is the
shader.

## 2. What the shader is, and the two things that had to be exactly right

```glsl
/* vertex */                       /* fragment */
gl_Position    = ftransform();     c = gl_Color;
gl_FrontColor  = gl_Color*uColor;  if (uMode==1) c = texture2D(uTex,uv)*gl_Color;
gl_TexCoord[0] = uv0 + uUVOff;     if (uMode==2) c = tex.a * gl_Color;
vFog = min(|(MV*V).z|, uFogFar);   if (c.a <= uAlphaRef) discard;
                                   if (uFogOn) c.rgb = mix(uFogColor, c.rgb, f);
```

**(a) THE TRANSFORM MUST BE `ftransform()`.** The obvious retained design
composes `proj*view` on the CPU, uploads one `uMVP` and does one matrix
product in the shader. That is algebraically the fixed-function transform and
numerically a different one — float multiply-add does not associate — so every
vertex lands a fraction of a pixel away, every textured surface resamples at a
slightly different point, and every silhouette moves. Measured at the pinned
frame, track pass alone:

| transform | pixels moved | of them by > 8/255 |
|---|---|---|
| one CPU-composed `uMVP` | 1.69% | 0.80% |
| `uProj * (uMV * v)`, two mat4 uniforms | 4.69% | 0.082% |
| **`ftransform()`** | **0** | **0** |

`ftransform()` is *defined* to be invariant with the fixed-function pipeline,
which is exactly the property an A/B needs, and it costs the renderer nothing:
`render_frame()` already loads the camera, the baked geometry is in world
space, and only a knocked prop ever pushes a matrix. The colour and the
texcoord travel as `gl_FrontColor` / `gl_TexCoord[0]` for the same reason —
they are the varyings the fixed-function fragment stage interpolated, so the
interpolation is the same interpolation.

**(b) THE PASS MUST HAND BACK THE STATE THE LEGACY ONE LEFT.** The car and HUD
passes that follow the world read state the world pass sets and do not set all
of it themselves. `b3r_end()` restores the depth mask, blend, texture and
alpha-test enables (the display list's own tail), the alpha FUNC (its head),
the texture env (`b3_props_draw`/`b3_scenery_draw` set MODULATE and never
restore it) and — the one that was missed first — **CULL_FACE**, which those
two turn off and put back from a `glIsEnabled` snapshot. Leaving it off drew
every car double-sided and was worth 0.33% of the pinned frame on its own.

`GL_TEXTURE_2D`, `GL_ALPHA_TEST` and `GL_FOG` stay OFF for the whole pass. That
is not tidiness: gl4es tests the live fixed-function state against a bound user
program (`fpe_ReleventState` / `fpe_IsEmpty`, `src/gl/fpe.c:1090`) and, if any
of it is live, compiles a VARIANT of the shader and switches to it. With all of
it off, one program stays bound for the whole frame.

## 3. The merge, and the draw order it may not break

`trackmesh_decals_last()` already sorts the world so that the decal groups come
last. Replaying the shipped data through it shows the partition is cleaner than
that:

| range | groups | triangles | contents | textures |
|---|---|---|---|---|
| head `[0, 934)` | 934 | 87 147 | **zero decal, zero blended** — all depth-writing | 112 |
| tail `[934, 990)` | 56 | 3 099 | the 4 blended + the 52 decal groups | 10 |

and the two texture sets are **disjoint**. Measured on the same data: **no two
adjacent groups share a (texture, state) pair**, which is precisely why
`LIBGL_BATCH` moved the track from 1 281 draws to 1 273 and was dropped.

So the merge runs inside maximal runs of one class. An opaque run is sorted by
(texture, state) — stably, on the original group index — and merged; a run
containing a blended or decal group keeps document order and merges only
adjacent identical state. Nothing crosses a run boundary. Animated groups do
not split a run: the old bake `continue`d past them and drew them later, so the
groups on either side of one were already adjacent in the base pass.
`B3_RENDER_TRACKSORT=0` turns the in-run sort off, for comparing the two at a
pinned frame.

The buffer is **de-indexed** — `glDrawArrays` over a flat triangle list needs
no index buffer, which sidesteps WebGL 1's 16-bit index ceiling and costs
9.7 MB for this track. The port is draw-bound, not vertex-bound, so that is the
right way round.

**Scenery and props are baked into WORLD SPACE.** The instance transforms never
move, so they do not belong in the frame at all: `b3r_inst_build()` applies
each instance's matrix to its vertices once, bakes the per-instance half-range
tint into the vertex colour, lays the result out model-major (one texture and
one material state per span) and orders the instances inside each model along a
**Z-order curve**, so the per-instance LOD cull — a sphere about the eye —
selects contiguous runs instead of 500 scattered singletons. The blended models
are moved to the end, which is where they belong: they write depth, so an
opaque instance drawn after a blended one that covers it would be
depth-rejected rather than composed. A prop that a car has knocked is no longer
on its baked transform, so it is skipped there and drawn from a model-space
copy under its own matrix; at most `B3P_MAX_LIVE` = 16 of those exist at once.

## 4. The HUD: nine passes into one buffer

`draw_text()` draws every string NINE times — eight black shadow offsets plus
the fill (`burnout3_hud.c:455`) — and each one was its own `glBegin`/`glEnd`.
All sixteen `glBegin` sites in that file now feed one dynamic VBO through a
batcher that flushes only when the texture or the blend preset changes.
`GL_QUADS` becomes two triangles in the `(v0,v1,v2)+(v0,v2,v3)` order the quad
rule uses, and `GL_QUAD_STRIP`/`GL_TRIANGLE_STRIP` unroll with their own
winding preserved, so a gradient quad interpolates identically and a strip
keeps its facing.

## 5. The pixel gates

Same instrument as every wave before it: one pinned offscreen frame
(`SDL_VIDEODRIVER=offscreen`, `B3_FIXED_DT=0.0166667`, `B3_PACE_MAX_TICKS=1`,
`B3_AUTODRIVE=1`, `B3_TRACK=US_C3_V1`, `B3_SHOT_FRAME=600`, 1024x768), the
pre-deletion build against the shipped one.

**And this time the frame is actually deterministic.** Every gate in this repo
until now carried an "identical outside the EA TRAX banner" caveat, because
nothing ever called `b3_music_seed()` and `burnout3_music.c:192` opened the
shuffle bag on `time(NULL)` — two runs of the *same* binary differed on 8 440
pixels. **`B3_MUSIC_SEED=<n>` pins it.** With it set, two runs of the shipped
binary are **bit-identical**, and two runs of the reference differ by 12 pixels
at <= 2/255 in one 34x4 strip. So the numbers below are a real measurement of
the change and not of the shuffle.

| what | pixels moved | of them > 2/255 | > 8/255 | max |
|---|---|---|---|---|
| shipped binary, run vs run | **0** | 0 | 0 | 0 |
| reference binary, run vs run | 12 | 0 | 0 | 2 |
| **everything except the scenery** (track base + scroll + shine + props + HUD) | 421 (0.054%) | 53 | 31 | 190 |
| — of which the HUD alone | 45 | 0 | 0 | 2 |
| **whole frame, reference vs shipped** | **2 381 (0.303%)** | 276 (0.035%) | 144 (0.018%) | 194 |
| the same, with the in-run texture sort OFF | 2 380 | 276 | 144 | 194 |

Three causes, each identified rather than waved through:

1. **The scenery bake — 1 960 px, almost all at +-1/255.** Baking an instance
   into world space computes `view * (instance * v)`; the legacy pass computed
   `(view * instance) * v`, because the driver multiplied the two matrices and
   `ftransform()` used the product. Same algebra, different float association,
   so scenery silhouettes move a fraction of a pixel. The alternative that
   would be exact is one draw per instance with the matrix on the stack — 523
   draws instead of ~44, which is the entire item this wave exists to remove.
   Documented and kept.
2. **Distant thin alpha-tested cut-outs — 376 px, 31 of them > 8/255.** The
   treeline, the fences and the lamp poles at several hundred metres, where a
   heavily minified cut-out texture has most of its texels near the 64/255
   reference and a hair of coverage difference flips one. `max 190` is one such
   texel on a pole against the sky.
3. **One text run — 45 px, all at <= 2/255.** The scale-2.4 gold ordinal
   ("4th"), whose vertical gradient steps by less than one level per pixel;
   the fixed-function colour varying and a GLSL `varying` round it differently.
   Two other candidate causes were tested and rejected: flipping the quad
   diagonal makes it worse (680 px, whole screen), and flushing per text pass
   instead of batching makes it worse (246 px, whole screen).

**THE MERGE IS NOT ONE OF THE CAUSES**, and that is the line in the table that
proves it: turning the in-run texture sort off — which restores the legacy
group order exactly — moves the frame by **one pixel**. The reordering the
whole track pass depends on is free.

Suites, all green on the shipped tree: `validate_tracks --tracks US_C3_V1`
**9/9**, `validate_carfx` **270/270**, `validate_hud` **769/769** (two new
checks: the batcher's two blend presets must really be the two `glBlendFunc`
pairs), `validate_scenery` **315/0/0**, `validate_props` **337/337**,
`validate_draw_distance` **12 checks, 0 failed**. `make` and `make wasm` with
no new warnings.

## 6. The call count

`B3_WEB_GLCOUNT=30`, `US_C3_V1`, mid-race with autodrive, headless Chromium,
the direct (unproxied) present path. Same disc, same track, same 90-second
run; the "before" column is the parent build driven with the migration's
`B3_RENDER=legacy` switch, which is why the two columns are one binary apart
rather than one machine apart.

| pass | before: calls / draws | after: calls / draws |
|---|---|---|
| **track** (base + scroll + shine) | **15 474 / 1 281** | **612 / 206** |
| **scenery** | 4 995 – 5 315 / 443 – 473 | **76 – 80 / 38 – 40** |
| **props** | 1 290 / 123 | **167 / 41** |
| **hud** | 1 107 – 1 144 / 85 – 88 | **56 – 61 / 7 – 8** |
| cars *(not migrated)* | 1 119 – 1 329 / 56 – 65 | 983 – 1 168 / 56 – 65 |
| traffic *(not migrated)* | 834 – 946 / 67 – 76 | 836 – 948 / 67 – 76 |
| sky *(not migrated)* | 56 / 3 | 57 / 3 |
| fx *(not migrated)* | 35 – 78 / 1 – 4 | 37 – 80 / 1 – 4 |
| postfx *(not migrated)* | 59 / 2 | 64 / 2 |
| **TOTAL** | **25 178 – 25 401** | **3 003 – 3 050** |
| — of them SYNC round trips | 5 778 – 5 819 | **560 – 571** |

**8.3x.** The four migrated passes went from 22 900 calls to 911, and their
1 940 draws to 292. The number that says why is calls PER DRAW, split by which
side of the migration a pass is on:

| | draws / frame | calls / frame | calls per draw |
|---|---|---|---|
| migrated (track, scenery, props, hud) | 292 | 911 | **3.1** |
| still fixed-function (cars, traffic, sky, fx, postfx) | 129 | 1 977 | **15.3** |

3.1 is a texture bind, a state delta and a `glDrawArrays`. 15.3 is gl4es
re-uploading and re-pointing three scratch VBOs, which is exactly what section
6 above measured at 12.2 and exactly what a static VBO stops.

The track pass is **612**, against the 600 this wave was aiming at, and that
612 covers all three of its sub-passes: the merged world (157 draws), the
animated groups (16) and the shine (33).

### SwiftShader, relative only

Same runs, the engine's own `B3_WEB_PROF` clock. **SwiftShader is a CPU
rasteriser and its absolute numbers mean nothing** — it is in the table because
one row against another is still a measurement, and because a CPU rasteriser
is the one place where a WebGL call's own cost is visible at all.

| | before | after |
|---|---|---|
| frame | 225 – 240 ms | **117 – 123 ms** |
| `scene+hud` | 106 – 113 ms | **48 – 53 ms** |
| present | 0.09 – 0.11 ms | 0.09 – 0.10 ms |

(The `gamma` column moves too, from ~120 ms to ~67 ms, and it should not — that
pass did not change. SwiftShader rasterises on its own threads, so a timer
around the gamma call catches the tail of the scene's work; the honest figure
here is the frame total, not the split.)

### The web's own gates

`tools/web_smoke.py` 8/8 PASS (module booted, image bridge up, iso source
banner, gl4es up, global materialise, track geometry, no FATALs, PIXELS ON
CANVAS). `tools/web_shell_smoke.py` — the real shell, driven the way a player
drives it — 5/5 PASS. `tools/web_cold_smoke.py` — persistence unavailable,
cold, warm, and a DAMAGED cache — PASS.

`--pin-frame 240` writes the same 640x480 frame on two runs of the shipped
build, **bit-identical**, and the frame is the whole scene: road with its
decal markings, the scenery's lamp posts and treeline, the buildings, the
traffic, the player car and every HUD element. It is NOT bit-comparable
against the desktop frame and no gate here claims it is — SwiftShader and the
desktop driver are two different rasterisers, and the two frames differ on 85%
of pixels for that reason alone.

**One bug this gate caught that nothing else would have.** The first web build
of this wave passed `PIXELS ON CANVAS` and `track geometry` while drawing NO
WORLD AT ALL: `b3r_init()` gated on `SDL_GL_GetCurrentContext()`, which is NULL
for this port's whole run because the context is created on the game thread
through emscripten's own path and SDL's bookkeeping never sees it. The census
is what showed it — `track 0/0, props 0/0, scenery 0/0` — and the smoke's
canvas gate was green off the sky dome alone. The second bug was the fix for
the first: calling `gl4es_GetProcAddress` before `b3_web_gl_init()` traps the
wasm outright, because gl4es is built `NO_INIT_CONSTRUCTOR`. `b3r_init()` is
now ordered after that bring-up, and retries rather than latching, so neither
failure can come back silently.

## 7. The soak

Desktop, 5 minutes of continuous racing (`B3_EXIT_AT=330`), RSS sampled every
20 s. The retained path allocates its buffers once at load and never again;
the only per-frame upload is the shine pass' colour half and the HUD's quad
buffer, both `glBufferSubData` into a buffer that is grown by doubling and
then stops growing.

```
t=20s  227272 kB    t=120s 227408 kB    t=220s 227408 kB
t=40s  227272 kB    t=140s 227408 kB    t=240s 227408 kB
t=60s  227312 kB    t=160s 227408 kB    t=260s 227408 kB
t=80s  227360 kB    t=180s 227408 kB    t=280s 227408 kB
t=100s 227400 kB    t=200s 227408 kB    t=300s 227408 kB
```

Flat from t=120 s onward, to the kilobyte. The per-pass draw counts printed by
`B3_RENDER_STATS` over the same run do not drift either.

## 8. What is and is not proven about 60 fps

**The one thing this wave changes is the number of WebGL calls, and that number
is exact.** Every other term in the budget is either measured on a CPU
rasteriser (and therefore not transferable) or belongs to hardware this harness
does not have.

The measured per-call dispatch cost on the direct path is **0.240 us**
(asynchronous) and **0.242 us** (synchronous), from `B3_WEB_GLBENCH` in
section 2; Chromium's published per-WebGL-call cost is 0.2 - 0.5 us. So the
call-dispatch line of a 16.7 ms frame is:

| | calls/frame | at 0.24 us | at 0.50 us (pessimistic) |
|---|---|---|---|
| before this wave | 25 178 | **6.04 ms** | **12.59 ms** |
| after | 3 050 | **0.73 ms** | **1.53 ms** |
| — of them blocking round trips | 5 819 -> 571 | | |

plus the present, which is **0.09 - 0.12 ms measured** and unchanged.

So the line of the 16.7 ms budget that this wave owns goes from **6.0 - 12.6 ms
to 0.7 - 1.5 ms**: from 36 - 75% of a 60 Hz frame spent before the GPU is asked
to do anything, to 4 - 9%. Finishing the migration (section 9) removes about
two thirds of what is left of it, because the 1 977 calls the unmigrated passes
still make are 65% of the new total for 30% of the draws.

**What is proven:** the dispatch cost above, because it is calls x a measured
per-call cost, and both factors are measurements rather than models. The
scheduling cost of a WebGL call does not depend on the GPU.

**What is not:** everything the GPU then does. SwiftShader rasterises this
port's frame in ~230 ms, of which the gamma pass alone (one full-canvas
`glCopyTexSubImage2D` plus a screen quad) is over half; on any real GPU that
term is sub-millisecond and the SwiftShader number says nothing about it. The
SwiftShader frame time is reported here as a **relative** figure only. Whether
the remaining budget closes on the user's machine is a question only the user's
machine can answer.

## 9. What phase 2 still has to do

The passes below still emit through the fixed-function surface, and their cost
is measured, not guessed — this is the same census `B3_WEB_GLCOUNT` produces,
taken on the shipped tree:

| pass | calls / draws a frame | what it still uses |
|---|---|---|
| **cars** | ~980 - 1 330 / 55 - 65 | one display list per body/panel/glass/wheel, `glPushMatrix` + `glMultMatrixf` per car; the carfx program is already a real shader, so this is geometry plumbing only |
| **traffic** | ~755 - 950 / 60 - 76 | the same shape, and simpler: plain textured lists with no shader of their own |
| **postfx** (sky + gamma + blur) | ~115 / 5 | the sky dome already draws from client arrays; the full-screen quads are `glBegin` |
| **fx** (particles, boost flames, coronas, shadows) | ~35 - 80 / 1 - 4 | four `glBegin` billboard sites across `burnout3_particlefx.c`, `burnout3_boostfx.c` and `burnout3_carfx.c` |
| menus / frontend / load screen | not in the race census | 14 `glBegin` sites and 23 display-list operations in `burnout3_full.c` |

Source-level inventory of what is left, by file:

| file | `glBegin` | display lists | matrix stack | `glTexEnv` |
|---|---|---|---|---|
| `burnout3_full.c` | 14 | 23 | 112 | 0 |
| `burnout3_postfx.c` | 2 | 0 | 15 | 3 |
| `burnout3_carfx.c` | 2 | 0 | 5 | 2 |
| `burnout3_boostfx.c` | 1 | 0 | 0 | 1 |
| `burnout3_particlefx.c` | 1 | 0 | 0 | 1 |
| `burnout3_hud.c` | **0** | **0** | 10 (the 2D identity pair) | **0** |
| `burnout3_trackmesh.c` | **0** | **0** | **0** | **0** |
| `burnout3_scenery.c` / `_props.c` | **0** | **0** | **0** | **0** |

**And one thing phase 2 must decide, with the measurement already taken.** The
retained path transports its camera through the fixed-function matrix stack
and reads `gl_Color` / `gl_TexCoord[0]` / `ftransform()`, because that is what
makes it bit-comparable against the pass it replaced (section 2a). That is a
COMPATIBILITY-surface dependency, so gl4es cannot be retired while it stands.
Dropping it means moving to `uProj * (uMV * v)` with explicit uniforms and
generic attributes, and the cost of that is already on the table above: at the
pinned frame it moves **4.69% of pixels, but only 0.082% by more than 8/255**,
all of it sub-pixel vertex drift with no structural change. That is the right
trade to make once there is nothing left to compare against — and the wrong one
to have made while there was.

# The fifth wave: measuring on the actual GPU, and what was eating the frame

Every number above this line was taken on SwiftShader, with the standing
caveat that a software rasteriser is not a frame-rate oracle. A user then
reported the thing the caveat was hiding:

> FPS: 59 (real 14.5, sim 57.1) — on an RTX 3090, Linux Chrome, main-thread
> WebGL reporting `ANGLE (NVIDIA, RTX 3090, OpenGL ES 3.2)`.

Software-class frame rate on hardware, while the measured call budget said the
frame should cost about a millisecond of dispatch. This wave is the diagnosis.

## 1. Headless Chromium can reach the real GPU, and that changes everything

The reason every previous wave measured SwiftShader is that `--headless=new`
was assumed to imply it. It does not. **Measured**, RTX 3090 / driver 595.84,
now available as `tools/web_smoke.py --gl hw`:

| flags | the WebGL renderer that comes back |
|---|---|
| `--use-gl=angle --use-angle=vulkan --enable-features=Vulkan --ignore-gpu-blocklist` | **`ANGLE (NVIDIA, Vulkan 1.4.329 (NVIDIA GeForce RTX 3090 (0x00002204)), NVIDIA)`** |
| `--use-gl=angle --use-angle=gl` | **NO CONTEXT AT ALL** — headless has no EGL display |
| no flag at all | SwiftShader, silently |

So the Vulkan backend is not a preference, it is the only one that works
without a window system — and with it the port's frame time can be measured
honestly, on the real GPU, without ever opening a browser. `--gl swiftshader`
stays the default, so every existing gate is unchanged.

## 2. The first line to read is which GPU the GAME THREAD got

The port now prints this unconditionally at boot, and it is two lines because
there are two contexts:

```
[Burnout3] web: gpu = ANGLE (NVIDIA, Vulkan 1.4.329 (RTX 3090), NVIDIA)  [vendor Google Inc. (NVIDIA)]
                      ctx=webgl1 readFormat=RGBA/UNSIGNED_BYTE timerQuery=webgl1   <- THE GAME THREAD'S CONTEXT
[Burnout3] web: gpu = ANGLE (NVIDIA, Vulkan 1.4.329 (RTX 3090), NVIDIA)   (the page's own, for contrast)
```

The engine draws through a **worker-local OffscreenCanvas** (wave 2), so the
renderer string a player reads out of the page's own context is not the one
that matters, and a browser is free to back the worker's with software while
the main thread's is on the GPU. Nothing the port printed before this said
which of the two you had. `readFormat` is there because a decision inside
gl4es turns on it — section 4 — and `timerQuery` because it decides whether
GPU time can be reported at all.

## 3. `B3_WEB_HWPROF=<n>` — where the frame actually goes

Three lines every *n* presents. Off by default; when off, not one wrapper is
installed and the context object is exactly what the browser handed over.

```
[hwprof]   frame 42.73 ms (23.4 fps) = sim 8.21 + render_frame 28.78
           (of which the postfx grab 7.97) + present 0.16
           (bitmap 0.04 + postMessage 0.07) + loop 5.58 | gpu 0.03 ms
[hwsync]   per frame: readPixels 1.00 (300 kpx) | copyTex 0.00 | getError 0.00
           | getParameter 21.40 | finish/flush 1.00 | bindFramebuffer 0.00
[hwupload] per frame: texture 1.30 uploads (902 KiB) | buffer 247 writes (9722 KiB)
```

The two halves of the present are timed **separately** on purpose: a slow
`transferToImageBitmap` is the browser materialising a copy of the drawing
buffer, a slow `postMessage` is the transfer path, and they are different bugs
with different fixes.

**GPU time.** `EXT_disjoint_timer_query_webgl2` needs a WebGL 2 context and
this port asks for WebGL 1. Chrome *exposes* the WebGL 1
`EXT_disjoint_timer_query` and then never services `TIME_ELAPSED` on it —
measured: 241 queries started, none ever became available, and the report says
so rather than printing a zero. `B3_WEB_HWPROF_SYNC=1` substitutes a timed
`glFinish()` before each present. That is a real pipeline sync and therefore
perturbs what it measures; it is opt-in and never a default.

## 4. The finding: the postfx frame grab was a full synchronous readback

`b3_postfx_gamma()` runs last in every frame and needs the frame as a texture,
which it gets with a full-canvas `glCopyTexSubImage2D`. gl4es implements that
call twice (its `texture_read.c:151`):

```c
copytex = (dst.format==GL_RGBA && dst.type==GL_UNSIGNED_BYTE)
       || (dst.format==fb.IMPLEMENTATION_COLOR_READ_FORMAT
           && dst.type==fb.IMPLEMENTATION_COLOR_READ_TYPE);
if (copytex || !colormask[0] || !colormask[1]
            || !colormask[2] || !colormask[3])
     gles_glCopyTexSubImage2D(...);            // a GPU-side blit
else { malloc(w*h*4);
       gl4es_glReadPixels(...);                // GPU -> CPU, SYNCHRONOUS
       gl4es_glTexSubImage2D(...); }           // CPU -> GPU
```

The grab texture is `GL_RGB` and the drawing buffer's read format is
`RGBA/UNSIGNED_BYTE`, so **neither** of the first two clauses held; the colour
mask is fully open by the time the grab runs (the HUD restores it); and the
frame took the readback. Every frame. Full canvas.

**Measured**, RTX 3090 / ANGLE Vulkan / headless / US_C3_V1 mid-race:

| | before | after |
|---|---|---|
| `readPixels` per frame | **1.00 (300 kpx)** | **0.00** |
| texture uploads per frame | **1.33 (903 KiB)** | 0.27 (2 KiB) |
| the grab itself | **4.3 – 8.0 ms** | **0.02 ms** |
| `render_frame` | 21 – 29 ms | 21 – 22 ms |
| GPU backlog at the present | 0.02 – 0.03 ms | 0.02 – 0.03 ms |

and end to end, one binary, `B3_WEB_GRAB_MASK` the only difference, eight
consecutive 30-frame windows of the same pinned autodrive:

| | achieved `real` fps | mean |
|---|---|---|
| **before** | 22.8, 24.4, 23.5, 23.4, 23.3, 26.0, 26.6, 27.3 | **24.7** |
| **after** | 34.5, 31.2, 34.8, 29.9, 33.8, 42.9, 32.9, 35.9 | **34.5** |

**+40%.** And the "before" column brackets the reported 14.5 – 24.9 exactly.

### Why a readback costs so much more than a readback

`gpu 0.02 ms` is the whole story: at 23 fps on an RTX 3090 **the GPU is idle**.
A readback is not merely a slow call, it is a barrier — the CPU waits out
everything the GPU has queued, then the GPU waits out the CPU while the pixels
are converted and pushed back up, and the two stop overlapping for the rest of
the frame. A grab that is 8 ms in itself moved `render_frame` by 14.

### The fix, and the two that do not work

**The fix is the third clause**: mask one channel off and gl4es takes the blit
unconditionally. The channel is ALPHA, for the reason that makes it safe rather
than clever — **the destination is `GL_RGB` and has no alpha to lose**. gl4es'
own comment on that clause worries that a driver may honour the mask during the
copy; here there is nothing for it to honour. The mask is restored to all-open,
which is this engine's resting state and not an assumption (`burnout3_hud.c`
uses the same `(1,1,1,0)` mask for retail's `0x010101` plates and restores
all-open at `:362` and `:639`, and the grab runs after all of them).

**Allocating the texture RGBA does not work**, and the pinned frame is how we
know. The context is created `alpha:false`, so the drawing buffer has no alpha,
and GLES2/WebGL make it `INVALID_OPERATION` to `CopyTexSubImage` into a
destination carrying a component the read buffer lacks. The copy is silently
dropped, the gamma pass samples an undefined texture, and `--pin-frame 240`
comes out mean `(0,0,0)` against the reference's `(113,107,101)`. A black
screen that every log-based gate would have passed.

**The structural fix, for whoever has the frame to spend on it**, is to draw
the scene into an app-owned RGBA FBO and let the gamma pass sample that texture
directly. Then there is no copy to argue about, the 16-bit depth limitation
goes with it (an app-owned depth attachment can ask for 24), and
`postfx_grab_frame()` disappears. That is a renderer change and it belongs with
the pass migration, not with postfx.

## 5. What the same instrumentation cleared

Three standing suspects, each answered with a number rather than an argument.

**The ImageBitmap present is not the eater.** `present 0.10 – 0.16 ms`, of
which `transferToImageBitmap` is 0.03 – 0.05 and `postMessage` 0.04 – 0.07.
It does not move with scene complexity and it is 0.4% of the frame.

**The animated-texture path uploads nothing.** `texture 0.10 – 0.27 uploads
per frame, 1 – 2 KiB` once the readback is gone — and that residue is the sky
LUT (`postfx_lut_update`, 64x32 RGBA, guarded on the sun's progress moving),
not the 15 animated batches. `trackmesh_group_texture()` returns a
**pre-uploaded** GL name per frame and the draw is a `glBindTexture`; the 29
animation frames across `Chgo_Flag`, `Chgo_AccidentSign_Words` and `water` are
all resident from track load.

**Nothing in the frame calls `glFinish`, `glFlush` or `glBindFramebuffer`.**
Measured 0.00 each. That last one matters because gl4es issues an
unconditional `glGetError` after **every** framebuffer bind (its
`framebuffers.c:262`), so a frame that bound one would be paying a forced round
trip per bind. This one does not. `getError` is 0.00 as well.

## 6. What is still on the list, with its measurement

| what | measured | note |
|---|---|---|
| `getParameter` round trips | **21 – 36 per frame** | **Traced.** `DEPTH_CLEAR_VALUE` ~9.4 and `COLOR_CLEAR_VALUE` ~9.4 come in a pair from **`glPushAttrib`**: gl4es saves the clear values with `gl4es_glGetFloatv` (its `stack.c:61` and `:74`), and neither enum is in `commonGet`, so both forward to the driver. Nine pairs a frame is nine `glPushAttrib` sites — `burnout3_hud.c:333`, `burnout3_particlefx.c:846`, `burnout3_boostfx.c:339`, `burnout3_carfx.c:2388` and `:2492`, `burnout3_postfx.c:758` and `:953`. Every one of them is in a pass phase 2 is migrating, and the retained path does not use `glPushAttrib` at all — so this number goes away with them. `VIEWPORT` 3.00 is separate (one is `burnout3_particlefx.c:832`). Also worth knowing: `glGetBooleanv` is 100% pass-through in gl4es (`wrap/gles.c:885`), with no filtering whatsoever. |
| buffer writes | **~265 per frame, ~5.6 MB** | The retained renderer accounts for 9 of them (1 shine + 7-8 HUD flushes). The remaining ~255 are Emscripten's `-sFULL_ES2` client-array staging for the passes still on the fixed-function path — one `bufferSubData` per enabled client attribute per draw (`libwebgl.js:570-593`), un-orphaned, into temp buffers. **This is a phase-2 number**: finishing the pass migration removes it, and it is a stronger argument for doing so than the call count was. |
| `sim` | **3 – 8 ms** | scales with frame time, because the governor gives a slow frame more 1/60 s ticks to carry. |
| `loop` | **0.4 – 5.6 ms** | `b3_music_pump`, the captures, and the sleep limiter. The limiter resyncs when a frame overruns by more than four tick periods and then sleeps a whole period; at 40 ms frames that lands about every third frame. |

## 7. Orphaning the dynamic vertex buffers — and the pixel it found

Both dynamic buffers in `burnout3_render.c` are written at offset 0 of the same
buffer object a draw is still reading: the shine colours once a frame, and the
2D batcher's vertices **seven or eight times within one frame**, each flush
overwriting the region the previous flush's `glDrawArrays` sourced. The
standard answer is to orphan — `glBufferData(target, size, NULL, usage)` says
the old contents are dead, so there is nothing left for the driver to wait for.

`B3_VBO_ORPHAN=<mask>`: bit 0 the shine buffer, bit 1 the 2D batcher.

**It is worth nothing measurable here, and that is reported rather than
buried.** Same sweep, same build, sequential runs on the RTX 3090 through
ANGLE/Vulkan, eight 30-frame windows each:

| mask | achieved `real` fps | mean |
|---|---|---|
| 0 (off) | 44.6, 32.8, 35.6, 33.2, 31.7, 36.1, 35.8, 35.0 | **35.6** |
| 1 (shine) | 35.8, 38.5, 35.2, 34.2, 34.2, 36.0, 35.4, 34.5 | **35.5** |
| 3 (both) | 39.2, 46.7, 38.8, 37.9, 37.8, 30.6, 31.6, 33.2 | **37.0** |

The spread inside any one column (30.6 – 46.7) is several times the difference
between columns, so none of this is signal. Bit 0 ships on anyway, on the
narrow grounds that it is correct by construction, costs one GL call a frame,
and this repo cannot test the ANGLE **GL** backend that the reporting user is
actually on — not on the grounds that it was measured to help, because it was
not. An earlier reading of "+14%" for it came from comparing across two
different sweeps and was noise; re-measured inside one sweep it is a wash.

**Bit 0 is bit-identical** at the pinned frame on both targets.

**Bit 1 does not ship on, and why is a finding.** Measured on the desktop
build, offscreen, US_C3_V1 frame 240 with the sim pinned: `B3_VBO_ORPHAN=2`
against `0` differs on **5 px of 307 200 (0.0016%), max delta 3/255**, all
inside one 30x6 box. Each configuration is bit-identical to itself across runs,
so it is not jitter, and bit 0 alone is bit-identical to the baseline, so it is
this site specifically.

Orphaning cannot change what a *correct* draw reads — the same bytes reach the
same offsets before the same `glDrawArrays`. What it changes is what lies
**beyond** `g_2d.n`: stale previous-batch vertices without it, undefined
storage with it. So a difference here says something in the 2D path is
sensitive to vertices past its own count. That is a defect worth finding on its
own terms rather than papering over with a buffer hint, and until it is found
the shipped default is the behaviour the pinned frame was pinned against.

## 8. The present alternative, and why there is not one

The port snapshots a worker-local OffscreenCanvas and `postMessage`s the
bitmap, because the game loop is a blocking `while()` that never returns to its
event loop and a *transferred* canvas is only presented when it does. Wave 2
argued that. This wave **measured** it, with a three-worker repro page driven
headless:

| worker | on SwiftShader | on the RTX 3090 |
|---|---|---|
| A `transferControlToOffscreen`, blocking `for(;;)` loop, clears to a cycling colour | **BLANK** | **BLANK** |
| B the same, plus `gl.flush()` every iteration | **BLANK** | **BLANK** |
| C the control — the same worker, yielding through `setTimeout(0)` | **PAINTED** | **PAINTED** |

So Chrome does not present a transferred canvas without an event-loop yield,
`gl.flush()` does not force it, and the control proves the harness works. There
is no switch to offer. Given section 5's measurement — the present is 0.4% of
the frame — there is also nothing to gain by looking for one.

## 9. Command-buffer replay: the escape hatch, costed

If the worker's context ever *is* the problem, the alternative is to record
each frame's GL work into a SharedArrayBuffer and replay it on the main thread,
against the page's own canvas, inside a rAF. That removes OffscreenCanvas, the
worker-context ambiguity and the bitmap hop together, and the worker keeps its
blocking loop and its synchronous ISO reads.

**Costed, on the same GPU, headless**, with a synthetic frame shaped like this
port's (3 000 calls, 420 draws, 64x64 so fill contributes nothing):

| | ms/frame | µs/call |
|---|---|---|
| issue the calls straight at the context (what the worker pays today) | 0.286 | 0.095 |
| **record** them into an `Int32Array` over a `SharedArrayBuffer` | 0.073 | 0.024 |
| **replay** them from that buffer on the main thread | 0.154 | 0.051 |

Command stream: **30.2 KiB/frame**. Record + replay is **0.23 ms/frame** of
added CPU — under 1.5% of a 60 Hz budget, and replay is *cheaper* than the
direct issue it replaces, because a flat typed array decodes faster than the
JS object walk standing in for the recorder's call sites.

**Verdict: feasible and cheap, and it should not be built yet.** Three reasons,
in order of weight:

1. **The measured eater was not the present.** It was the readback in section
   4, and it is fixed. The present is 0.10 – 0.16 ms.
2. **Most of the benefit is already available without a custom command
   buffer.** Emscripten's own proxied path *is* a command buffer; what made it
   slow (wave 2) is that 68 of its 140 entry points dispatch synchronously —
   and the ones that do, `glVertexAttribPointer` and `glDrawElements` among
   them, are synchronous precisely because they read **client-side** vertex and
   index memory that may change before the main thread gets to it. A renderer
   with no client arrays does not need them to be synchronous. That is phase
   2's work, and finishing it makes the cheap version of this design available
   for free.
3. **The hard part is not the dispatch, it is the handle tables.** 0.23 ms buys
   the encode and decode of seven opcodes. A real one needs ~140 entry points
   with variable-length arguments, plus cross-thread identity for every
   texture, buffer, program, shader, framebuffer and uniform location, plus a
   copy of every dynamic vertex range at record time (the worker reuses its
   scratch buffers, so the main thread cannot be allowed to read them late).
   None of that is expensive at run time; all of it is surface to get wrong.

So: **phase 3 at the earliest, and only if a machine turns up where the present
or the worker context is measurably the cost.** It should not preempt phase 2 —
it is phase 2 that makes it cheap.

## 10. Reading a report from a machine this repo cannot reach

`B3_*` knobs now reach the shell through the query string
(`web/index.html?B3_WEB_HWPROF=60`), so the lines above can be produced by
somebody who is only playing the game. The decision tree, in the order the
lines appear:

1. **`web: gpu = ...` for THE GAME THREAD'S CONTEXT names a software
   renderer** (SwiftShader, llvmpipe, "Software") while the page's own line
   names the GPU → it is a browser configuration problem, not a port problem.
   `chrome://gpu` will say why. On Linux/NVIDIA the levers that matter are
   `chrome://flags#ignore-gpu-blocklist`, the ANGLE backend picker
   (`chrome://flags#use-angle`: try Vulkan, then OpenGL), and
   `chrome://flags#enable-vulkan`. `--enable-unsafe-webgpu` is unrelated and
   will not help.
2. **`[hwsync] readPixels` is not 0.00** → the frame is taking gl4es'
   readback path (section 4) on that machine despite the colour-mask fix.
   `B3_WEB_GRAB_MASK=0` will confirm by making it worse. The fix is the FBO in
   section 4.
3. **`[hwprof] present` is a large share of the frame** → the bitmap/postMessage
   split says which half, and section 9 is the design that removes both.
4. **`[hwupload] buffer` bytes are large** → the client-array staging in
   section 6; that is phase 2, not a knob.
5. **Everything is small and the frame is still long** → `sim` is the CPU, and
   `render_frame` minus the named parts is gl4es plus the driver. `gpu` under
   `B3_WEB_HWPROF_SYNC=1` separates those two: a GPU backlog near zero means
   the frame is CPU-bound, which every measurement in this wave says it is.


# The sixth wave: the compatibility layer comes out, and 60 fps on the GPU

Phase 2 finished the one-renderer mandate. Every fixed-function call site is
gone, the transform is a CPU-composed `uMVP`, and the web build **no longer
links gl4es at all** — `<GL/gl.h>` resolves to `web/GL/gl.h`, which is GLES2.

On the real GPU (`tools/web_smoke.py --gl hw`, ANGLE/Vulkan out of
`--headless=new`, RTX 3090), racing US_C1_V1:

| | before (user's capture) | after |
|---|---|---|
| frame | **61.4 ms (16.3 fps)** | **16.68 ms (60.0 fps)** |
| `sim` | 18.44 ms | 1.14 ms |
| `render_frame` | 32.86 ms | 8.57 ms |
| `present` | 0.50 ms | 0.04 ms |
| `loop` | 9.61 ms | 6.93 ms |
| buffer writes / frame | 433–474 (10 075–10 838 KiB) | 30 (1 832 KiB) |
| `getParameter` / frame | 28–30 | **1** |

`loop` is no longer a defect — it is the frame limiter idling because the frame
is genuinely finished in ~9.7 ms. **Read the `sim` row with suspicion**: this
wave did nothing to the simulation, and 18.44 → 1.14 ms is far too large to
claim. The two captures are not the same run (different session, different
traffic and AI population), so the honest statement is that `render_frame`
fell 3.8x and the frame is now capped rather than bound.

## What replaced glPushAttrib, and the four bugs the shadow found

GLES2 has no attribute stack, and the eight passes that bracketed themselves
with `glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
GL_CURRENT_BIT)` were doing real work with it. Rebuilding it from
`glIsEnabled`/`glGetIntegerv` would have been correct and ruinous — each of
those is a blocking round trip on WebGL, and eight pushes of ~10 queries would
have added ~80 a frame to a budget the previous wave spent cutting from 30 to
1.

So the stack moved onto the CPU: `burnout3_render.h` declares shims for the
state calls, macro-mapped so that all 126 existing call sites join in without
being rewritten, and `b3r_state_push()` / `b3r_state_pop()` snapshot the shadow.
`B3_STATE_AUDIT=1` compares the shadow against the driver at every state call
and prints any drift — which is how the following were found, in this order,
each one first visible as *the pinned frame moved and nothing in the diff
explained it*:

1. **`burnout3_trackmesh.c` did not include `burnout3_render.h`.** It sets
   `glFrontFace(GL_CW)` and enables culling for the whole world pass, so those
   two went straight to the driver while the shadow believed GL's defaults, and
   a later `glDisable(GL_CULL_FACE)` was filtered as redundant. 0.66–1.28% of
   every frame, brighter, with the mean up 0.2. (Same class as the
   double-sided cars in wave four.)
2. **`b3r_end()` was defined ABOVE the point where `render.c` re-enables the
   shims for itself**, so the three calls that hand the frame back its world
   state — depth mask on, blend off, cull restored — bypassed the shadow.
3. **`b3r_state()` kept a SECOND cache of the same five fields** and skipped
   the GL call when its own copy matched. Wrong the moment any pass set that
   state with a raw call, which the car glass does (`glDepthMask(GL_FALSE)`
   before `b3_carfx_glass_begin`). Invisible while `glPopAttrib` existed,
   because the pop reset the driver behind both caches and the stale one
   happened to be right again by luck.
4. **`postfx` switches to `GL_TEXTURE1` and back through its own
   GetProcAddress'd `p_glActiveTexture`.** While those four calls were
   invisible, the per-unit texture-bind cache recorded unit 1's binds in unit
   0's slot; filtering binds on top of that put the wrong texture on a handful
   of batches — 3.6–4.5% of every pinned frame.

`B3_STATE_NOFILTER=1` is the other half of the tool: it issues every state call
unconditionally. If a frame changes under it, some pass is moving state behind
the shims. That is what turned "the frame moved" into "the redundancy filter is
lying", in one run, after two wrong guesses had already been tried and
disproved.

## A bug the pixel gates could not have caught

`b3r_model()` — the object transform for a prop that is no longer at its baked
world position — still pushed the *fixed-function* matrix stack after the
transform flip, so it fed a stack nothing reads any more. A knocked prop would
have drawn at the world origin. No pinned gate frame contains one (a prop is
only off its baked transform after a car has hit it), so all six frames stayed
green through the entire flip. It was found by reading the remaining
fixed-function call sites, not by a gate.

## Mipmaps: GL has two spellings and no target has both

`glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE)` before the upload
is GL 1.4/GLES1; `glGenerateMipmap()` after it is GL 3.0/GLES2. WebGL has only
the second — asking for the first is `INVALID_ENUM`, the chain never gets
built, and a `GL_LINEAR_MIPMAP_LINEAR` minifier on a texture with no chain is
mipmap-incomplete, which samples black.

Moving the seven upload sites to `glGenerateMipmap` **on the desktop** moved
19.8% of the pinned frame (3.7% by more than 2/255). The difference is entirely
in minification — the magnified bottom band moved 0.06% while the mid-distance
bands moved ~6% and came out brighter — i.e. the two paths average the
alpha-keyed texels differently. Both are legal, neither is retail, so
`b3r_tex_mipmap_pre()` asks the context which one it has rather than handing it
the newer one, and the desktop keeps the chain every pinned reference in this
tree was captured against.

## The call count, and the honest verdict against the <800 target

`B3_WEB_GLCOUNT=60`, racing, direct WebGL:

```
TOTAL 2361-2447 per frame  =  draw 357 + vertex 778-838 + texture 390-398
                            + uniform 608-618 + program 72-80 + state 148-159
by pass (calls/draws):  track 501/206   cars 912/58   traffic 545/24
                        hud 118/9   scenery 86/42   postfx 70/2   props 46/11
                        sky 52/3   fx 29/1
```

Against wave four's ~26 500 a frame through gl4es this is a 11x reduction, and
against phase 2's own start (~2 450) the program and texture caches added here
took ~90 more off. **It does not meet the <800 target, and the breakdown says
exactly why**: `cars` spends 912 calls on 58 draws — 15.7 per draw — because
every car sub-mesh (body, panels, wheels, glass) re-points its vertex format,
rebinds its texture and re-uploads its matrices, and `track` spends 501 on 206
draws because merge-by-texture is per material-run, not global. Getting under
800 needs one vertex format across the car meshes and a draw-order-preserving
global texture sort for the track — a phase 3, not a knob. Nothing measured
here is bound by it: the frame is capped at 60 with 6.9 ms of idle.

## Size and build time

| | with gl4es (HEAD) | direct WebGL |
|---|---|---|
| `burnout3.wasm` | 3 363 913 B | **2 353 623 B** (−30.0%) |
| `burnout3.js` | 317 020 B | 310 188 B |
| `libGL.a` | 1 947 566 B | — |
| full `make wasm` from clean | 45 s | **18–22 s** |

The 45 s includes building gl4es itself, which is the fair comparison for a
clean checkout: `bash web/fetch_deps.sh && make wasm` no longer clones or
compiles anything third-party.

## The frame limiter

The resync arm set the deadline to `now` and then advanced it a full period, so
a frame already over budget busy-waited another 16.7 ms. Isolated on
SwiftShader (which still produces over-budget frames, ~63 ms), steady state,
last 10 samples:

| | `loop` mean | `loop` max | frame |
|---|---|---|---|
| before | 2.81 ms | 3.82 ms | 63.02 ms |
| after | **0.55 ms** | 1.10 ms | 63.02 ms |

Frame time is unchanged there because SwiftShader is the bottleneck and the
recovered sleep is simply absorbed; what the fix guarantees is that the limiter
never *adds* to a late frame. Desktop pacing is unaffected: 60.1 sim Hz before
and after, and the pinned frames are bit-identical (`B3_FIXED_DT` sets
`gov.mode = 0`, so the governor is not even on that path).

The menu `loop` of 69–135 ms with `sim`/`render` near zero is **not** this
defect and is **not** fixed here — the menu screens use their own throttle
(`if (now - last < 16) SDL_Delay(16 - (now - last))`), which cannot inject
sleep on a late frame. It stays open for the ledger.


## Merging the aftereffects chain, and turning it on for the web

Master landed `src/burnout3_aftereffects.c` after this wave began: an FBO chain
that replaces the old grab/gamma path outright, with no `glCopyTexSubImage`
grabs at all. It shipped **off on the web**, and its own note said why in
terms of gl4es, with the prediction that removing gl4es would fix both
blockers. It did — and one of the two was misattributed.

**Cost.** Re-measured on the direct-WebGL link, RTX 3090 through ANGLE/Vulkan
headless, US_C3_V1, same session, median of the last eight racing frames:

| | render_frame | frame |
|---|---|---|
| chain off | 5.39 ms | 16.67 ms (60.0 fps) |
| chain on | 5.76 ms | 16.66 ms (60.0 fps) |

**+0.37 ms**, against the +2.3 ms it cost through gl4es and a ~1.5 ms budget.
The overhead really was the compatibility layer's per-draw CPU cost.

**Correctness.** The web frame used to lose its sky. Pinned frame 900,
US_C3_V1, direct WebGL:

```
sky rows 0-96    mean 88.2 -> 96.0    std 55.5 -> 58.4
top fifth        94.6% of pixels above 16, max 255
whole frame      clipped 1.80% -> 3.11%, black 1.10% -> 0.43%
```

The sky is intact and slightly *brighter* — the chain's exposure — and keeps
its structure rather than flattening. **But note what this corrects**: the
depth attachment is still **16-bit** here, because the context is WebGL 1 and
`DEPTH_COMPONENT24` is core in WebGL 2, not this one. The sky loss was
therefore never the depth precision it was attributed to; it was gl4es' draw
path. The web default is now ON, matching the desktop, with `B3_AFX`
overriding either way.

**The module had the defect this wave found four times — twice.** It made
eight raw state calls (`glDepthMask`, `glEnable(GL_DEPTH_TEST)`, `glColorMask`,
…) without including `burnout3_render.h`, the same shape as the
`burnout3_trackmesh.c` bug above; and `afx_bind_tex()` switched texture units
through its own `p_glActiveTexture`, the same shape as the `burnout3_postfx.c`
bug above. Both fixed in the merge.

**And the second one is why the first reading of the gate was wrong**, which is
worth writing down because the wrong answer was plausible. With the chain on,
this branch first differed from master by 3.2–4.3% of the frame at >2/255,
while with the chain **off** the same frames differed by 0.10–0.46% — exactly
the transform flip's accepted cost. The obvious inference is that the chain,
which applies bloom, exposure and retail's x2 composite, was **amplifying** its
input difference about 7.5x. That inference was wrong.

`B3_STATE_AUDIT=1` said so: 41 disagreements a frame, all
`TEXTURE_BINDING_2D`, shadow 0 against driver 329/330 — the chain's own colour
attachments. The unit switch was invisible, so the bind cache filed the chain's
binds in unit 0's slot and then filtered later binds as redundant when they
were not. Real wrong-texture batches, not amplification. Routing that one call
through the shim:

| frame | before the fix | after |
|---|---|---|
| 300 | 4.34% > 2/255 | **0.06%** |
| 600 | 3.98% | **0.09%** |
| 900 | 3.42% | **0.36%** |
| 1050 | 3.52% | **0.13%** |
| 1200 | 2.01% | **0.06%** |
| crash 900 | 3.20% | **0.09%** |

with every frame mean now identical to master's to one decimal. The lesson is
the one the four earlier defects already taught and this nearly got talked out
of: when the frame moves and the diff does not explain it, **the audit is the
instrument, not the hypothesis**. A tidy story about tone mapping would have
shipped a wrong-texture bug.


# The seventh wave: the web renders at the viewport, and four defects a
# web-to-web gate could never see

The user's report on the shipped build was "WebGL perf is good now, but it
looks low res and many shaders don't appear to work correctly (car glossiness,
window transparency)". Three separate things, one of them mine.

## Resolution: the pin was a call-bound-era mitigation

The web pinned its drawable to 640x480 and let CSS scale it up. That was
reasoned from a port where every pixel went through gl4es on a proxied
main-thread context. None of that is true any more, and the pin's only
remaining effect was a soft image. The backing store is now the canvas
element's CSS box times the device pixel ratio, re-checked in the same
every-30-presents slot that used to defend the pin, and the 4:3-inside-16:9
pillarbox goes with it -- the drawable matches the element, and the engine's
existing per-frame `SDL_GetWindowSize()` read handles the aspect. The real
shell went from 640x480 to 1309x736 at the harness' window size.

**Measured on the RTX 3090 through ANGLE/Vulkan headless, US_C1_V1, chain on:**

| backing store | frame | fps | sim | render_frame | loop |
|---|---|---|---|---|---|
| 640x480 | 16.66 ms | **60.0** | 1.70 | 8.86 | 6.04 |
| 1920x1080 | 17.52 ms | **57.1** | 1.70 | 9.73 | 6.06 |
| 3840x2160 | 63.61 ms | **15.7** | 11.93 | 51.51 | 0.08 |

1080p costs **+0.87 ms of render_frame for 6.75x the pixels** -- at that size
this port is not fill-bound, which is exactly why the pin bought so little.
4K is a different regime and does **not** hold 60, so a cap is warranted and
is now in: the default renders at the viewport up to a 1080p-class pixel
count and scales the whole rectangle down past it, preserving aspect.
`B3_RES=WxH` overrides; `B3_RES=pin` restores the old 640x480 behaviour as a
documented fallback.

## The shader defects: one report, four causes

The method mattered more than any single fix. Every previous web gate compared
the web against **itself**, so a defect present since the first web build was
invisible to all of them. Comparing a pinned frame **desktop against web** --
same source, same seed, same resolution, same frame -- put all four on screen
in one image. (Frame choice matters too: by frame 900 the two sims have
diverged into different races, which reads as a rendering fault and is not
one. Frame 150 is still in lockstep.)

### 1. The carfx program did not compile on the web at all

```
[carfx] fragment shader failed: ERROR: 0:34: 'GL_OES_standard_derivatives' : extension is disabled
[carfx] shine=OFF (no GL2)
```

The no-vertex-normals path builds a face normal from `dFdx`/`dFdy`, which on
GLSL ES 1.00 needs `#extension GL_OES_standard_derivatives : enable`. gl4es'
shaderconv used to inject that; talking to WebGL directly, nobody does. **This
is the whole of the user's report**: with the program dead there was no body
gloss and no glass shader, so the paint went flat and the rear window came out
a solid white slab. One missing line, two reported symptoms. The directive is
now declared, and the shader also compiles *without* the extension -- a
`B3FX_HAS_DERIV` fallback keeps a browser that lacks it from losing the entire
program over one branch.

### 2. `b3r_sync()` wrote the retained program's uniforms into carfx's

`car_mesh_draw()` called `b3r_sync()` unconditionally. `glUniform*` always
writes to the **currently bound program**, and uniform locations are
per-program: with carfx bound, b3r's matrices landed on whichever carfx
uniform happened to occupy the same location index. The two shaders share no
uniforms at all.

On the desktop the collision landed somewhere harmless and the car looked
right. **The WebGL compiler numbers uniforms differently**, the matrix hit
something load-bearing, and the car body collapsed to a flat slab -- with
correct data, correct buffers, correct attribute locations, and a draw the
driver accepted without a single GL error. What finally placed the fault was
that the identical geometry rendered perfectly through the retained program a
few lines away; the difference could only be the uniforms.

### 3. `(unsigned)-1` reached `glBindTexture`

The renderer's "I do not know" sentinel could be captured by
`b3r_state_push()` and restored into a real bind. Desktop GL accepts any
unused name and quietly creates an empty texture object called 0xFFFFFFFF;
WebGL has no integer names, so the call is INVALID_OPERATION and the unit is
left bound to nothing. 2 266 a frame, found with `-sGL_ASSERTIONS=1`.

### 4. `glDisable(GL_TEXTURE_2D)` -- 255 INVALID_ENUMs a frame

Not a valid capability in GLES2, and a fixed-function leftover the sixth
wave's census missed because it only looked for BLEND/DEPTH_TEST/CULL_FACE.
It mattered for a reason worth remembering: **Chrome rate-limits WebGL error
reporting per context**. 255 junk errors a frame tripped "too many errors, no
more errors will be reported for this context" and took the messages that
mattered with them. The renderer's own GL_GENERATE_MIPMAP capability probe was
doing the same thing on a smaller scale -- deliberately raising an error to
ask a question -- and now reads the version string instead.

## What the tooling learned

`tools/web_smoke.py` now enables the CDP **`Log` domain**. WebGL errors are
`Log.entryAdded` entries, not `console.log` calls, so subscribing only to
`Runtime.consoleAPICalled` made every one of them invisible to every gate this
harness runs. A rejected draw is a silent no-op on the web and fine on the
desktop -- precisely the class this wave was chasing. Chrome's own words
("INVALID_ENUM: disable: invalid capability") are what broke defect 4 open.

New diagnostics, all env-gated and off by default: `B3_CARFX_ENVDBG=7/8/9/10`
(world normal, raw normal attribute, object-space position, carfx's own MVP
diagonal), `B3_GLERR` (bracket each car draw with `glGetError` -- and clear
*immediately* before the draw, because Emscripten records errors of its own
into `GL.lastError` and a stale one reads exactly like a rejected draw),
`B3_MESHDBG` (what the loader built, before any GL state can be blamed) and
`B3_STATE_DUMP` (the state shadow at a named site, no readback).

## Desktop is untouched

Every pinned desktop frame is bit-identical to master across all six gate
frames, and the suites are unchanged. The uniform stomp in defect 2 was real
on the desktop too -- it simply landed harmlessly there, which is why removing
it moves nothing.
