# The load, in parallel — what was threaded, what was not, and the numbers

The question was "multithreaded data loading, if it helps and is possible".
It helps in one place and not in the others, and this is the measurement that
decides which is which. Everything below is `B3_LOADPROF=1` — the engine's own
per-phase clock — from **interleaved A/B runs**: the pre-change binary and the
new one, alternating, same disc, same track, same machine, so load drift on a
busy box hits both sides equally. Medians of three rounds.

Reproduce with `B3_JOBS=1`, which makes every worker pool in this document run
its items inline on the calling thread. That is not an approximation of the old
code — it is the old control flow, and it is the first thing to try if a
parallelised stage is ever suspected of anything.

---

## 1. The profile that decided it

The cold path is where the time is, and it is not I/O. Every hot stage run
standalone (`tools/cextract/build.sh` → `cxtract --all-global --only <stage>`),
wall / user / sys:

| stage | wall | user | sys | what it is |
|---|---|---|---|---|
| car_paint | 5.20 s | 4.97 | 0.21 | 107 cars → Morton de-swizzle + zlib deflate per palette |
| postfx_art | 1.61 s | 1.57 | 0.03 | 145 sky pages → DXT decode + deflate |
| txd | 1.54 s | 1.51 | 0.02 | 808 frontend textures → decode + deflate |
| car_meshes | 1.51 s | 1.45 | 0.06 | 107 cars → OBJ/panel/wheel text |
| rws | 0.89 s | 0.51 | 0.35 | WAV extraction |
| awd | 0.21 s | 0.09 | 0.11 | WAV extraction |

**User time IS the wall clock in the top four.** One core was doing all of it
on a machine with 36. That is the whole finding: a cold boot is not waiting for
the disc, it is deflating PNGs.

The warm path is a different shape — 1.28 s total, of which the three real
items are TRACK GEOMETRY 0.33 s (an OBJ parse), TRACK TEXTURES 0.25 s (libpng
inflate + GL upload) and CAR MESHES 0.23 s.

---

## 2. What was parallelised

Two mechanisms, both in `tools/cextract/cx_pool.h`, and neither of them
touches the resolve ring, the `chdir()` in `run_global()`, or the stamp and
absent bookkeeping. `b3_iso_resolve()` stays main-thread-only, exactly as
`src/burnout3_isodata.h` promises.

**(a) The per-item parallel-for** — `cx_pool_for()`. Used where the loop body
is one independent item writing files no other item names: `car_paint` and
`car_meshes` over the 107-car fleet. The discipline that makes the output
identical rather than merely equivalent:

* the two nested walks (class, then file) are FLATTENED into one item array in
  the serial visit order, so item `i` is the same car it always was;
* each item's summary line goes into its own slot and is replayed **in index
  order** afterwards, so the stdout transcript is byte-identical too;
* `ok`/`fail` are summed after the loop, never bumped inside it;
* the two `static char msg[128]` error buffers whose address escapes through
  `*err` became `_Thread_local` (`cx_cars_bgv.c`, `cx_cars_common.c`).

**(b) The deferred PNG queue** — `cx_png_queue_begin()` / `_flush()`. Used
where the loop CANNOT move, because its order is load-bearing:
`cx_art_txd.c` resolves the one cross-bank name collision ("Takedown") in visit
order and appends to a FAILURES list; `cx_art_postfx.c` builds
`enviro_manifest.txt` out of per-track blocks in walk order; `cx_textures.c`
keeps a keyed skip tally and relies on "a later record of the same name
overwrites the earlier". So the encode moves under them instead: a queued
`cx_png_write_rgba8()` copies the pixels and returns, the flush encodes every
queued image on the pool, and the finished byte streams are written **in queue
order, one `fwrite` each**. Three rules keep it honest — a path queued twice
flushes first (which is what preserves "later wins"), a 256 MB resident pixel
budget flushes, and a write arriving from any thread but the queue's owner
takes the direct path.

Wired into: `txd`, `postfx_art`, per-track `textures`, `traffic_cars`.

**The source is locked, not redesigned.** `CxSrc` caches directory extents and
mmap'd regions lazily; two threads growing those arrays at once corrupts them.
`cx_src.c`'s five public entry points now take a recursive mutex
(`cx_pool_lock()`), so an item that reads the disc through `cxd_read_file()` is
safe and its read is serialised — deliberately, because the read is
microseconds against milliseconds of encode.

**One I/O fix travelled with it.** `cxd_write_obj()` writes 909 OBJs totalling
88 MB a line at a time; on stdio's default block buffer that is ~22 000
`write()` calls. Natively they are cheap. On the web every one is a blocking
round trip to the browser's main thread under `-sPROXY_TO_PTHREAD`, which is
what made `car_meshes` the worst cold stage there. It now gets a 1 MB
`setvbuf`, which is what `cx_track_mesh.c` already did for `track.obj` (git
`cb7f8d2` is the same argument on the read side).

**And the load path too**, not just extraction: `load_track_textures()` in
`src/burnout3_full.c` now resolves every path on this thread (one place,
unchanged materialisation), inflates the images on the pool, and uploads them
here in index order — so `glGenTextures` still hands out names in the sequence
the old loop used. It works in chunks of four per worker rather than one big
batch, which bounds the resident RGBA peak and gives the loading screen a frame
at every chunk boundary.

---

## 3. The numbers

### Desktop, cold ISO boot (empty cache, US_C3_V1)

| phase | before | after | Δ |
|---|---|---|---|
| CAR MESHES *(car_meshes + car_paint + txd + font)* | 10.561 s | 2.131 s | **−8.43** |
| RENDERER *(postfx_art)* | 2.094 s | 0.590 s | **−1.50** |
| TRAFFIC *(traffic_cars)* | 1.892 s | 0.560 s | **−1.33** |
| TRACK TEXTURES | 1.738 s | 0.456 s | **−1.28** |
| TRACK GEOMETRY | 0.851 s | 0.822 s | −0.03 |
| AUDIO | 0.268 s | 0.235 s | −0.03 |
| everything else | 0.401 s | 0.377 s | −0.02 |
| **TOTAL** | **17.805 s** | **5.371 s** | **−12.43 s, 3.3×** |

Stage-level, from the engine's own `iso:` lines: `car_meshes car_paint`
8.16 → 1.29, `txd font` 2.07 → 0.54, `postfx_art` 1.91 → 0.38,
`traffic_cars` 1.69 → 0.37, per-track `textures` 1.34 → 0.29.

### Desktop, warm load (US_C3_V1)

| phase | before | after | Δ |
|---|---|---|---|
| TRACK TEXTURES | 0.254 s | 0.132 s | **−0.12** |
| **TOTAL** | **1.284 s** | **1.192 s** | **−0.09 s, −7%** |

Every other phase is inside the noise. The warm win is only the texture
decode, and it is *understated* here: these runs are `SDL_VIDEODRIVER=offscreen`,
where `glTexImage2D` + `glGenerateMipmap` are software and dominate the phase.
On a real GPU the inflate is a bigger share of what is left.

### Web (headless Chromium, `tools/web_cold_smoke.py`, engine load clock)

| boot | before | after |
|---|---|---|
| cold, empty cache | 38.8 s | **18.0 s** |
| persistence unavailable (disc only) | 37.3 s | **15.6 s** |
| warm (OPFS cache) | 6.9 s | 6.8 s |
| damaged cache (heals) | 7.8 s | 5.2 s |
| all four boots | PASS | PASS |

Web cold is **2.2× faster**; web warm is unchanged, because the web warm load
is the OBJ parse (see §4) and nothing here touches it.

**A web trap worth recording.** The first web build after this change ran the
warm boot at 13.0 s instead of 6.9 s. The link was still
`-sPTHREAD_POOL_SIZE=4`: `PROXY_TO_PTHREAD` puts `main()` in one of those four
slots, `cx_pool` asks for three workers, and the last `pthread_create` had to
GROW the pool — which needs the browser's main thread to build a worker while
this thread waits on it. The pool is now 8 and the number went back to 6.8 s.
Pre-spawning enough is the cheap way never to find out how that goes.

### The loading screen

Frames actually PRESENTED (`B3_LOADPROF`'s frame column, `B3_LOADSCREEN=1`),
one cold boot each:

| | before | after |
|---|---|---|
| frames over the whole load | 72 in 15.06 s = **4.8 fps** | 75 in 4.63 s = **16.2 fps** |
| TRACK TEXTURES | 3 frames / 1.60 s | 5 frames / 0.39 s |
| CAR MESHES | 13 frames / 8.13 s | 14 frames / 1.68 s |

No phase presents fewer frames than it did. The pump got 3.4× smoother, which
is the point: the work shrank and the pump budget did not.

---

## 4. What was NOT parallelised, and why

**The OBJ parse (`trackmesh_load`), rejected.** This is the biggest remaining
item on both the desktop warm path (0.31 s) and the web warm path (where it is
most of the 6.8 s, musl's `strtod`). Splitting the buffer at line boundaries
looks mechanical and is not: the read loop is a state machine whose `vt` arm
writes vertex ALPHA at `m->colors[nuv*4+3]` **guarded by `nuv <
m->vertex_count`** — the count of `v` lines seen SO FAR — and whose `nuv <
vcap` guard tests a capacity that GROWS during the loop. A chunked parse has to
reconstruct both, which makes it a semantic change to the mesh rather than a
mechanical one, and a subtly wrong mesh is exactly the class of defect this
project has been burned by. Worth ~0.2 s on the desktop; not worth it at that
price. If it is ever attempted, the honest first step is a counting pre-pass
for the prefix sums, and the gate is `validate_tracks` plus a pinned-frame
pixel compare.

**`load_car_meshes`, rejected as too small.** Warm 0.23 s for the whole fleet,
of which the eight paint PNGs are a small slice. Below the noise floor of the
measurement that would justify it.

**`rws` / `awd`, rejected as I/O.** 0.51 u / 0.35 s and 0.09 u / 0.11 s: they
are not CPU-bound, and `rws` runs *after* `b3_loadscreen_end()` anyway, so it
is not on the load clock at all. (It is still ~0.7 s of stutter into the first
seconds of a race — a real thing, but a different one.)

**A resident thread pool, rejected for lack of evidence.** `cx_pool_for()`
creates its workers per call and joins them, which for the texture batch is 15
create/join rounds per track. The obvious worry is that this costs more than it
saves on the web, where `pthread_create` is expensive. Measured: web warm is
6.8 s against the baseline's 6.9 s, i.e. the churn is not costing anything
there is any evidence for. Not adding a resident pool, and its shutdown
ordering against the game's exit path, to fix a cost that does not show up.

**The resolve ring and the `chdir()`, untouched.** No winner required it. The
parallelism is entirely INSIDE a stage, above the shim, so the ring is still
only ever touched by the main thread and a stage still owns the CWD for its
whole run.

---

## 5. The gates

| gate | result |
|---|---|
| `verify_cextract.py`, car_meshes | **PASS** 1043/1043 |
| `verify_cextract.py`, car_paint | **PASS** 466/466 (pixel identity) |
| `verify_cextract.py`, txd | **PASS** 808/808 |
| `verify_cextract.py`, textures (US_C3_V1) | **PASS** 177/177 |
| `verify_cextract.py`, traffic_cars (US_C3_V1) | **PASS** 86/86 |
| `verify_cextract.py`, postfx_art | 107/107 PNGs PASS; 38 text sidecars FAIL — **pre-existing**, see below |
| serial vs parallel, whole global set (21 stages) | trees IDENTICAL |
| serial vs parallel, whole per-track set (14 stages) | trees IDENTICAL |
| two consecutive cold extractions | 4794 files, byte-identical |
| `validate_tracks --tracks US_C3_V1,US_C1_V1` | 18/18 |
| `validate_no_baked_data` | 90/90 |
| `validate_scenery` | 315/0 |
| `web_cold_smoke` | all four boots PASS |
| pinned frame 600, US_C3_V1 and US_C1_V1 | **byte-identical** before/after |
| `make` / `make wasm` | zero new warnings (identical sets; only line numbers moved) |

**The postfx_art failure is not this change.** All 107 sky PNGs are
pixel-identical. The 38 failures are the 37 `<ID>_env.txt` sidecars and
`enviro_manifest.txt`, and they share one cause: the C stage never emits the
`env+0x60` `light_rgb` block that `extract_postfx_art.py:297-302` writes.
`git show HEAD:tools/cextract/cx_art_postfx.c | grep -c light_rgb` is **0** —
the gap exists at HEAD and this branch adds only comments and two queue calls
to that file. Deleting those five line prefixes from each oracle sidecar makes
37 of 37 byte-identical. It is a real open defect and it belongs to whoever
ported that stage, not here.

**One measurement trap, recorded because it cost an hour.** The pinned-frame
pixel compare failed until `build/music` was removed from both trees: the
"now playing" banner picks its track from a wall-clock-seeded shuffle, so the
pre-change binary differs from ITSELF in exactly that rectangle when music is
available. Verified both ways before believing it.
