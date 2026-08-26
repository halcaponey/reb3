# reB3
If Claude and Ghidra had a baby. The finest slop in all the lands. 

A reverse engineering of *Burnout 3: Takedown* (Criterion Games / EA, 2004,
Xbox) — a faithful recreation driven by data and logic recovered from the
retail executable.

The physics, collision, scoring and AI are not a re-imagining. They are the
game's own equations, recovered from `default.xbe`, each one cited to the
address it came from and checked against the real x86 running under emulation.

---

## You need your own copy of the game

**This repository contains no game content.** No executable, no tracks, no car
meshes, no textures, no audio, and no data tables extracted from any of them.
It is code and documentation only.

You supply a dump of a disc you own — either the `.xiso` image or an expanded
directory — and the game reads it directly.

---

## Setup, start to finish

### 1. What you need

| | |
|---|---|
| **The game** | A dump of a disc you own, as a `.xiso` image or an expanded directory. Developed against the NTSC-U (USA) release. |
| **A C toolchain** | On Debian/Ubuntu: `sudo apt install build-essential libsdl2-dev libsdl2-image-dev libgl1-mesa-dev zlib1g-dev` |
| **Disk** | ~2 GB free for the extraction cache, on top of the image itself. |
| **Python** | Only for the RE tools and the differential suites — `pip install pillow capstone unicorn`. **The game itself needs none of it.** |

### 2. Build and run

```bash
make          # -> ./burnout3
./burnout3
```

That is the whole setup. **The game reads your disc directly** and materialises
what it needs, when it needs it. There is no extraction step to run first.

| Invocation | What it does |
|---|---|
| `./burnout3` | disc mode, image auto-resolved — **the default** |
| `./burnout3 --iso=<path>` | disc mode against this image or expanded dump |
| `./burnout3 --build` | the old pre-extracted `build/` tree (the debug path) |

The image is resolved from `--iso=<path>`, then `$B3_ISO`, then
`build/iso_path.txt` (written on the first successful `--iso=<path>` run, so it
is remembered), then **`$B3_GAME_ROOT`**. Nothing is compiled in — this tree
ships no game content and cannot guess where your copy lives:

```bash
export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"   # holds default.xbe
# ...or point straight at the image:
./burnout3 --iso="/path/to/Burnout 3 - Takedown (USA).xiso.iso"
```

`$B3_ISO_CACHE` moves the materialised assets (default `build/.isocache`).

A correct launch says where it is reading from, and then what it found:

```
[Burnout3] iso: global   @elf @globalus -> the retail image  (0.03 s)
[Burnout3] iso: source   <your image>
[Burnout3] iso: cache    build/.isocache  (--build for the pre-extracted tree)
[Burnout3] REAL track geometry: 97826 verts, 90246 tris
[Burnout3] GAME collision world: 60373 triangles
[Burnout3] REAL audio: 4 engine loops, EA TRAX 44/44 tracks
```

**How it works.** `src/burnout3_isoshim.h` is force-included into every `src/`
translation unit and routes `fopen` / `access` / `IMG_Load` /  `SDL_LoadWAV`
through `src/burnout3_isodata.c`. Every loader still opens a literal
`build/...` path; the resolver maps it to the cache and, on a miss, runs the C
extraction stage that produces it **in process, against the disc**. Not one
loader had to be edited. The design, and the exhaustive list of what the seam
covers, is at the top of `src/burnout3_isodata.h`.

### 3. Music (optional)

```bash
sh tools/fetch_wma.sh          # -> third_party/rockbox/ (gitignored)
make                           # rebuild; the stub is replaced by the decoder
```

Every audio payload on the disc is WMAv2, so the port needs a WMA decoder.
Rockbox's `libwma` is fetched rather than committed — it is LGPL, and keeping
it separable is the licensing story ([THIRD_PARTY.md](THIRD_PARTY.md)). Without
the fetch the build still works and the music module runs silent; it does not
fail.

### Controls

| Key | Action | | Pad | Action |
|-----|--------|---|-----|--------|
| W / Up | Throttle | | Right trigger | Throttle |
| S / Down | Brake | | Left trigger / B | Brake |
| A / D | Steer | | Left stick / D-pad | Steer |
| Space | Boost | | A (or X) | Boost — and **Impact Time**: hold it through a crash for the slow-mo aftertouch |
| Enter | Start race, dismiss results | | | |
| R | Restart | | | |
| ESC / P | Pause (audio mixer overlay) | | | |
| T | Dump gamestate + screenshot to `build/debug/` | | | |

`B3_TRACK=<id>` picks the event directly and skips the selector;
`B3_PLAYER_CAR` / `B3_PLAYER_PAINT` pick the car and livery.

### If something is wrong

| symptom | cause |
|---|---|
| `iso: no disc image found` | none of `--iso`, `$B3_ISO`, `build/iso_path.txt`, `$B3_GAME_ROOT` resolved |
| `... is neither an XISO image nor a game directory` | the path resolved, but the file is not an XISO — a re-packed `.iso` will do this |
| `no WMA decoder` / silent music | `tools/fetch_wma.sh` has not been run |
| `B3_GAME_ROOT is not set` from a `tools/` script | the RE tools resolve the dump through `tools/b3_paths.py`; export it |

---

## The web port

The same `src/`, compiled verbatim to WebAssembly, reading the player's own
disc out of a file picker — nothing uploaded, nothing downloaded, the 2.4 GB
image read lazily off local disk through a shared-heap bridge.

```bash
source ~/emsdk/emsdk_env.sh    # emcc on PATH (Emscripten 6.x)
make wasm                      # -> build/web/burnout3.{js,wasm}
make serve                     # http://127.0.0.1:8080/web/  (COOP+COEP)
```

The web build has **no third-party dependency**: the retained renderer emits a
strict subset of GLES 2.0, so Emscripten's own WebGL bindings satisfy the link
directly and the gl4es compatibility layer that used to be here is gone. The
build artefacts are not committed — `make wasm` produces them. `make webdist`
stages a statically-hostable copy into `dist/`.

The full design — why the disc cannot go through the filesystem, why the GL
context lives on the game worker, the audio ring, the frame-time sweep that
picked the defaults — is [`web/README.md`](web/README.md).

The Android port lives in `android/` and builds the same sources through
Gradle + NDK with GL4ES; divergence is behind `#ifdef __ANDROID__`. See
[docs/ANDROID_PORT.md](docs/ANDROID_PORT.md).

---

## State of it

**It plays.** You pick an event off the recovered track-select globe, pick a
car off the recovered car-select screen, and race a real Burnout 3 circuit in a
real Burnout 3 car against AI and traffic, with takedowns, crash cinematics
with aftertouch, and the retail HUD.

**36 events**, loaded generically — there are no per-track constants in the
source and `B3_TRACK` selects any of them. **107 vehicles** with their real
meshes, liveries and per-car tuning. Real textures, collision, nav graphs,
traffic sets, scenery, props and light probes per track; real engine and effect
audio and streamed music; the retail frontend, loading screen and in-race HUD.

It is a harness, not a shipped game: there is no career mode, and the
presentation is deliberately not pixel-matched. Handling, collision, triggers
and control flow are.

### Nothing game-derived is compiled in

Eight generated headers used to bake retail data into `src/` — the vehicle
roster, per-car tuning, fonts, track paths, start grids, AI pace, traffic sets
and the Globalus display strings. **All eight are gone**, replaced by
`src/*_runtime.h` loaders that read your own files at boot.

`tools/validate_no_baked_data.py` is the permanent gate and asserts *both*
halves: the headers are absent and nothing references them, **and** the
replacements really come off disk, proven by the loaders' own log lines on a
real boot. A compiled-in copy is worse than a missing file, because it does not
look missing — the port used to substitute one track's road, grid and traffic
on every other track, and the symptom read as a physics bug.

### The extraction pipeline is C

`tools/cextract/` is the extractor: 14 per-track stages (`CX_STAGE_LIST` in
`cx_extract.h`) plus the global car / art / audio families, with `cx_main.c` as
a standalone `cxtract` driver. It is roughly 14x faster than the Python it
replaced, which is what makes materialise-on-miss viable at all.

The original Python extractors are archived **unmodified** in
`tools/py_extract_archive/` and are the **oracle**: any change to the C side
must re-pass `tools/cextract/verify_cextract.py`, which demands byte-for-byte
identical artefacts (pixel-identical for PNGs) and fails on a file present on
only one side. The old `tools/extract_*.py` paths are forwarding stubs onto the
archive, because several validators import them as parsing libraries.

### Retail is runnable as ground truth

`build/backends.cfg` selects, per feature, whether the port runs its own
recovered C or calls **the game's own x86 under Unicorn** through
`tools/b3_emu_server.py`. Ten features switch: `physics`, `carcol`, `ai`,
`traffic`, `td_rules`, `score`, `crash`, `camera`, `sfx`, `hud`. `--re` and
`--retail` force all ten for one run. This is what the differential suites
validate against, and it is why "1:1 with retail" is a measurement here rather
than an opinion.

---

## How claims are made here

This is the part worth copying if you do your own RE project.

Every recovered fact carries provenance:

* **`[C]`** — confirmed from the image, **with the address**.
* **`[S]`** — strong inference, with the evidence stated.
* **`[?]`** — open.
* **GLUE** — an invention of this harness, not the game. Marked as such at the
  site, so it can never be mistaken for a recovered fact.

"Unrecoverable" requires proof, not a shrug.

The test suites are **differential**: they execute the retail function out of
`build/burnout3.elf` under Unicorn and compare it against this port, case by
case. When `tools/validate_carcol.py` says the car-vs-car crash gate matches,
it means the real `FUN_001121F0` was run and agreed. 41 Python suites, three C
ones and one JS one.

```bash
python3 tools/xbe2elf.py "$B3_GAME_ROOT/default.xbe" build/burnout3.elf
python3 tools/validate_port.py         # the physics pipeline vs real x86
python3 tools/validate_carcol.py       # car-vs-car collision
python3 tools/validate_gameplay.py     # scoring / boost / takedown rules
python3 tools/validate_no_baked_data.py # nothing retail is compiled in
make test-soup-ray test-traffic-pool test-traffic-reservations test-audio-ring
```

**The ELF step is not optional for the tools.** Loading `default.xbe` as a flat
binary is *silently wrong* — each section has a different `VA − file_offset`
delta, so absolute data references land on the wrong bytes. `0x003A2D50` is
`2.5` but reads as `0.0`; one float constant reads as the string
`"Score/Burnout Points"`. `tools/xbe2elf.py` rebuilds the real address space
(one `PT_LOAD` per section at its true VA, BSS materialised,
`e_entry = 0x001D2807`). If you import it into Ghidra, use the **ELF loader and
pass no explicit language ID** — doing so forces the Binary loader and
reproduces the bug. The before/after is in [docs/ASSETS.md](docs/ASSETS.md).

Two constants that reading alone would have missed, and that emulation caught:
the game's gravity is **10.0**, not 9.81, and its m/s→mph factor is
**2.2374146**, not 2.2369363.

---

## Layout

```
src/burnout3_full.c            the harness: frame loop, frontend, race flow
src/burnout3_isodata.c/.h      THE DATA MODEL -- disc -> assets, on a cache miss
src/burnout3_isoshim.h         force-included; redirects fopen/access/IMG_Load
src/burnout3_vehicle_sim.c     the recovered per-frame vehicle pipeline
src/burnout3_collision.c       collision world and solvers
src/burnout3_ai.c, _ai_avoid.c the AI driver law and avoidance
src/burnout3_traffic_*.c       traffic pool and reservations
src/burnout3_crash.c           crash entry, wreck sim, aftertouch
src/burnout3_carcol.c          car-vs-car contact
src/burnout3_td_rules.c        takedown / near-miss / score event rules
src/burnout3_render.c          the retained renderer (GLES2-subset)
src/burnout3_hud.c             the in-race HUD, using the XBE's own fonts
src/*_runtime.h                the loaders that replaced the baked-in headers

tools/b3_paths.py              resolves B3_GAME_ROOT for the tools -- start here
tools/xbe2elf.py               XBE -> correctly mapped ELF32 (do this first)
tools/cextract/                the C extraction pipeline (cxtract)
tools/py_extract_archive/      the archived Python oracle -- IMMUTABLE
tools/emulate_*.py             run real retail functions under Unicorn
tools/validate_*.py|.c|.js     differential suites: this port vs the real code
tools/b3_emu_server.py         the retail-backend bridge
tools/blender/                 .bgv reader/writer + Blender export addon

web/                           the WASM/WebGL port and its shell page
android/                       the Android port (GL4ES, runs on device)
docs/                          the evidence records
```

### Documentation

| | |
|---|---|
| [ASSETS.md](docs/ASSETS.md) | where the data comes from, and what must never be committed |
| [HANDOFF.md](docs/HANDOFF.md) | orientation and war stories |
| [RE_MASTER.md](docs/RE_MASTER.md) | the master address / method / verdict index |
| [RE_NOTES.md](docs/RE_NOTES.md) | the master findings document |
| [PHYSICS_GLUE_LEDGER.md](docs/PHYSICS_GLUE_LEDGER.md) | every GLUE invention, and what would replace it |
| [RE_SHAPE_PARITY.md](docs/RE_SHAPE_PARITY.md) | per-frame pipeline shape parity, RE-vs-retail switching |
| [RE_AI.md](docs/RE_AI.md) | the AI driver, pace, avoidance and rubber band |
| [RE_CRASH_PARITY.md](docs/RE_CRASH_PARITY.md) | the complete crash decision surface |
| [RE_FRONTEND.md](docs/RE_FRONTEND.md) | track-select globe, car select, loading screen, HUD |
| [BGV_EXTRACTION.md](docs/BGV_EXTRACTION.md) | how the `.bgv` car mesh format was cracked |
| [RE_PROPS.md](docs/RE_PROPS.md) | destructible props |
| [LOAD_PARALLELISM.md](docs/LOAD_PARALLELISM.md) | what the load path does concurrently, and what it may not |
| [web/README.md](web/README.md) | the web port's design and measurements |
| [ANDROID_PORT.md](docs/ANDROID_PORT.md) | the Android port |
| [TODO.md](TODO.md) | what is still open, each with its blocker |

---

## Legal

The code and documentation in this repository are original work, released
under the MIT licence — see [LICENSE](LICENSE).

*Burnout 3: Takedown* is the property of its rights holders (Criterion Games /
Electronic Arts). This project is not affiliated with or endorsed by them. No
copyrighted game content is distributed here; everything the program reads
comes off a copy you provide and must legally own.

Third-party components and their licences are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
