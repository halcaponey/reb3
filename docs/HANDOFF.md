# reB3 — handoff

Written for whoever picks this up next. Read this before touching anything; it
will save you days, mostly by telling you which approaches are already dead.

> **Section numbers here are load-bearing.** Six source files and several
> validators cite "HANDOFF.md section 2" (the flat-load trap) and "HANDOFF
> section 5" (the methodology rules) by number. Do not renumber the sections.

**Goal:** a faithful recreation of Burnout 3: Takedown driven by data and logic
recovered from the retail Xbox executable.

**Honest state (updated 2026-08-22):** it plays. `./burnout3` reads the user's
own Xbox disc, materialises what it needs out of it, and puts you on the
recovered track-select globe; you pick one of 36 events and one of 107 cars and
race it — real geometry, real liveries, real traffic and AI, takedowns, crash
cinematics with aftertouch, the retail HUD. Nothing game-derived is compiled
into `src/`. Every recovered equation cites its address and has a differential
case against the retail x86 executing under Unicorn; harness inventions are
marked **GLUE**; open items stay `[?]`.

Where the current, authoritative status lives — this file is orientation and
war stories, not a status board:

| Question | Document |
|---|---|
| What is recovered, per subsystem | `docs/RE_MASTER.md` → the `docs/RE_*.md` it indexes |
| What in the physics is still glue, and what is blocked on what | `docs/PHYSICS_GLUE_LEDGER.md` |
| What is open and who should work it next | `TODO.md` |
| How to build, run and switch data sources | `README.md` |

---

## 1. Setup you need

**Target file** (not in repo — user-supplied game dump). Either a `.xiso.iso`
image or an expanded dump directory works; the port auto-detects.

**Build and run — this is the whole thing:**

```bash
make                                  # -> ./burnout3
./burnout3                            # reads the disc directly
./burnout3 --iso=<path to image or dump>
```

There is **no extraction step to run first**. `src/burnout3_isodata.c` resolves
every `build/...` path a loader opens to an ISO cache under `build/.isocache`
and, on a miss, runs the C extraction stage that produces it in process. See
`src/burnout3_isodata.h` for the path map and the mode switches; `--build` is
the old pre-extracted tree, kept as the debug path.

**For RE work (not for playing):**

* **Ghidra + MCP bridge.** A Ghidra instance runs with the `ghidra-mcp` plugin
  exposing an HTTP API on `127.0.0.1:8089`. Check it is alive:
  ```bash
  curl -s http://127.0.0.1:8089/analysis_status
  ```
  Useful endpoints: `/decompile_function`, `/disassemble_function`,
  `/get_function_by_address`, `/get_xrefs_to`, `/list_functions`,
  `/get_function_callers`, `/get_function_callees`, `/create_struct`,
  `/recreate_struct`, `/set_function_prototype`, `/get_struct_layout`.
  **Every request must carry `&program=burnout3.elf`** — the default served
  program is a flat-loaded duplicate and `/switch_program` lies. Capstone or
  objdump over the raw ELF is the fallback when the bridge is down.
* **Python deps:** `unicorn` (CPU emulation), `Pillow` (texture export).
* **The differential suites** are the acceptance bar. `tools/validate_*.py`
  (plus three C ones) execute the retail functions under Unicorn and diff the
  port against them. They must stay green; `TODO.md` §3 carries the tallies.

---

## 2. The single most important thing

**The XBE must not be loaded as a flat binary.** Doing so is silently wrong and
it invalidates everything downstream. Each section has a different
`VA − file_offset` delta, so every absolute data reference reads the wrong bytes:

| Reference | True value | Flat-load value |
|---|---|---|
| `0x003A2D50` | `2.5` | `0.0` |
| `0x00384A80` | `0.15` | `22.0` |
| `0x003A1238` | `90.0` | the string `"Score/Burnout Points"` |

`tools/xbe2elf.py` rebuilds the real address space as an ELF32 (one `PT_LOAD`
per section at its true VA, BSS materialised, `e_entry = 0x001D2807`).

**Import it with Ghidra's ELF loader and do NOT pass an explicit language ID** —
doing so forces the Binary loader and silently recreates the bug. If a fresh
project is needed:
```bash
python3 tools/xbe2elf.py "<path>/default.xbe" build/burnout3.elf
# import build/burnout3.elf with auto-detect, then:
python3 tools/apply_ghidra_types.py   # structs + prototypes, self-verifying
```

Old flat-load addresses translate as `new = old + 0x10000`, **`.text` only**.

The port carries a C transcription of this conversion (`burnout3_isodata.c`), so
in ISO mode `build/burnout3.elf` is produced from the disc's `default.xbe`
automatically — several loaders parse ELF program headers and reject a raw XBE.

---

## 3. What is verified

Everything here is either confirmed by two independent derivations or checked
against executing the real code. Each `docs/RE_*.md` marks its claims
`[C]`/`[S]`/`[?]`; `docs/RE_MASTER.md` is the index across all of them.

### Executable
Image base `0x00010000`, entry `0x001D2807`, 17 sections, RenderWare RW36,
7,434 functions. 147 Xbox kernel imports named from the thunk table at
`0x0036B7C0` (`tools/xboxkrnl_ordinals.py`).

### Physics parameters — 73 across two config structs
- `FUN_00132D10` registers 64 params (`+0xB8`..`+0x1CC`, struct stride `0x1D0`)
- `FUN_00134AC0` registers 9 more (`+0x88`..`+0xA8`) — mass + suspension only,
  i.e. the reduced traffic-vehicle config
- `FUN_00132950` holds all 64 compiled-in defaults
- `FUN_00134710` copies config → live vehicle, which gave the live layout free

**Independently corroborated:** 14 of 16 spot-checked defaults appear as exact
floats in the community VDB dump (`Sokka06/burnout-data-tool`,
`data/vdb/VDB_ps2_bo3_release.XML`). The VDB is 8-byte records of
`float value` + `uint32 hashed key`; the hashing is why no parameter names
appear in any shipped data file.

**Per-car tuning** is the retail `Data/vdb.xml`, keyed by
`"<param><group>/../Export/ValueDB/VehiclePhysics/<VLIST-ID>.cfg"` (param name
FIRST — the reason cfgpath-first CRC guesses matched nothing), hashed by the
table-CRC at `0x001AF250` with **SAR** (arithmetic) shift and no final
inversion. It is loaded at run time now, not compiled in.

### Physics equations
The port runs `b3_vehicle_step_full()`, a 1:1 C composition of the real
per-frame pipeline (input stage `FUN_0011ECF0` including engine, then
`FUN_0011BE50`'s main path over substeps: force pass `FUN_0011D460`, suspension
pre-pass `FUN_001239C0`, force pass `FUN_00123FD0`, stop-check, integrator
`FUN_00109560`), on a fixed 60 Hz tick accumulator, in GAME coordinate space.

The acceptance oracle is `tools/emulate_pipeline.py` — the REAL functions
running multi-frame under one persistent Unicorn session — with
`tools/dump_traj.c` running the C from identical state and
`tools/validate_port.py` diffing them, including full multi-hundred-frame
trajectory runs that keep gear and drift state equal on every frame.

**Two constants no amount of reading would have found:**
- gravity is **10.0**, not 9.81
- m/s→mph is **2.2374146**, not the true 2.2369363 (Criterion's is 0.02% high),
  and the downforce term converts `speed_ms` with it rather than reading the
  stored mph field

Row-by-row status — recovered, proven-unrecoverable, blocked — is
`docs/PHYSICS_GLUE_LEDGER.md`. Do not restate it here; it moves.

### Assets and formats
- `static.dat` Xbox track format decoded — geometry, textures (DXT1/DXT5,
  production names), materials, the second prop model/instance tables, scenery,
  and the streamed-unit layout (`docs/RE_NOTES.md`, `INTEGRATION_NOTE.md`)
- `.bgv` car geometry decoded from the game's own relinker `FUN_000310f0`
  (`BGV_EXTRACTION.md`) — the format is read AND written, round-trip
  byte-exact on all 67 shipped player cars (`tools/validate_bgv.py`)
- `.bgd` nav graph, route and traffic-path tables (`docs/RE_BGD.md`)
- `.awd` / `.rws` audio, and the XWB/EA TRAX music banks (`docs/AUDIO_NOTES.md`)
- 107-vehicle roster from `pveh/`, cross-validating exactly against
  `vlist.bin`'s declared count

All of it now goes through `tools/cextract/` (C), gated byte-for-byte against
the archived Python oracle by `tools/cextract/verify_cextract.py`.

### Retail as a runnable backend
`build/backends.cfg` flips each of ten features (`physics`, `carcol`, `ai`,
`traffic`, `td_rules`, `score`, `crash`, `camera`, `sfx`, `hud`) between the
recovered C and the game's own x86 under Unicorn (`tools/b3_emu_server.py`);
`--re` / `--retail` force all ten for one run. That is the ground truth the
differential suites measure against, and it is why parity claims here are
measurements.

---

## 4. Dead ends — do not repeat these

> Historical record. `.bgv` was **solved** after this was written (section 3,
> `BGV_EXTRACTION.md`) — by transcribing the game's own relinker, which is
> exactly the "get ground truth" lesson of section 5. The five failures below
> are kept because each one is a heuristic that will look convincing again.

### `.bgv` vehicle meshes: five failed approaches

At the time, the mesh format was unsolved, including by the Burnout modding
community — EdnessP's Noesis plugin loads `.bgv` textures then calls
`boSetDummyMdl`, and "All vehicle model support" is in its own TODO.

1. **Plain float3 scan** — all 576 KB of `Car1.bgv` yields exactly one 38-triple
   region. Not float vertices.
2. **Spatial coherence metric** — ranked the header's sub-offset tables as
   "mesh-like". Monotonically ascending tables are maximally coherent; any
   smoothness-only heuristic locks onto them.
3. **Coherence + oscillation** — found regions with aspect ratio 1.00 on all
   three axes. No car is a cube. The tell was recurring `16256` = `0x3F80`, the
   high half of float `1.0` read misaligned. Those regions are float
   transforms/matrices.
4. **Version-immediate search** (`0x403`/`0x405`/`0x406`) — the "dispatch table"
   at `0x001DAEF1` is C++ static-initialiser registration. Also, the `0x406`
   hit at `0x00169855` is bytes `C7 06 04 00 00 00` = `MOV dword ptr [ESI],4`;
   the immediate does not exist, it spans two operands.
5. **Brute-force emulation** (`tools/find_bgv_parser.py`) — scored functions on
   reading/following header pointers. Top hits `FUN_00190330`/`FUN_00190380`
   are 33-line flag-setting loops. **The detector was invalid by construction:**
   it put the buffer pointer in all four registers and four argument slots, so
   any function reading small offsets scored; and "followed" counted a read
   anywhere within `0x400` bytes of a target, nearly free over a `0x90000` file.

**Also retracted:** `0x1C80` in `Car1.bgv` is **texture data** (per the Noesis
plugin), not NV2A push-buffers as recorded earlier. And `+0x00` is the **version**
(valid `0x14`..`0x25`), not a magic number.

**Falsified:** vehicle meshes do **not** reuse the track layout. Applying the
track submodel structure at every header offset and sub-offset entry, with and
without the `0x40` bounds prefix, validates nothing.

### Other dead ends
- `PrgData.bin` is not a VDB (its header fields don't validate)
- The per-car VDB is **not** inside each `.bgv` — falsified by full-dump scan.
  It is the single retail file `Data/vdb.xml` (section 3).
- Full-game emulation via xemu is impractical here: not installed, not packaged,
  and needs a BIOS/MCPX ROM. Function-level emulation via Unicorn works instead
  and needs none of that.

---

## 5. Methodology — read this, it is the real lesson

Across this work, **every single error was caught by a second independent
derivation disagreeing, never by re-reading more carefully.** Errors made and
later caught: wheel stride (`0x30`→`0xC0`), wheel array base (`+0x894`→`+0x820`),
a missed second parameter registrar, a silent struct-packing failure, a wrong
calling convention, a mislabelled velocity vector as a force vector, a missing
4th force component, a broken coherence metric, an over-optimistic feasibility
estimate, and two premature "found it" announcements on the `.bgv` parser.

Practical rules that follow:

1. **Ground truth beats reading.** The physics work is trustworthy because
   Unicorn executes the real code and says yes or no. The geometry work produced
   five confident wrong answers because it had no equivalent — and was solved
   the moment one existed (the game's own relinker). Get ground truth before
   believing anything.
2. **A green test suite means "what I checked matches", not "correct".** The
   4-component force bug passed 7/7 because nothing compared the 4th component.
   Widen what you assert before trusting a pass.
3. **Verify every immediate-value hit by disassembling around it.** Small
   immediates (`0x403`, `0x4C`, `0x17`) occur constantly as unrelated data.
4. **Any heuristic loose enough to rank a big binary will rank noise.** If a
   metric produces a tidy top-10, assume it's measuring something structural and
   irrelevant until proven otherwise.
5. **API success responses lie.** `/create_struct` reported success while
   packing fields wrong (440 bytes instead of 464, last five fields 24 bytes
   low). `/set_function_prototype` reported success with a wrong calling
   convention that produced zero typed fields. Always assert on the *result*.
6. **When you announce a finding, you've already verified it.** Two of the worst
   errors here were reported as breakthroughs and retracted the next step.
7. **The decompiler is not the disassembly.** Ghidra's decompile of
   `FUN_00109BB0` silently drops two of three axes. When a claim rests on
   control flow or on which components are written, read the instructions.
8. **An oracle you can edit is not an oracle.** `tools/py_extract_archive/` is
   immutable for this reason: the byte-identity gate only means something if the
   reference side cannot be adjusted until it agrees.

---

## 6. What's left

**This section is deliberately a pointer, not a list.** It went stale twice by
duplicating the ledgers; the ledgers are updated as part of the work and this
file is not.

* **`TODO.md`** — the live open-item board: the physics blockers and the retail
  function each one starts from, the Android correctness/packaging items, the
  suite tallies that must stay green, and the process rules for agent waves.
* **`docs/PHYSICS_GLUE_LEDGER.md`** — the physics rows themselves: recovered,
  proven-unrecoverable, or blocked, each with its evidence.
* **`docs/RE_AI.md`** — the AI follow-up ledger; the navigator's mutable
  route-selection state and recovery-state timing are the standing open end.
* **`web/README.md` §3** — in-browser audio is DONE (the AudioWorklet reading a
  shared-heap ring); what is left open there is EA TRAX, which needs a WMA
  decoder the browser does not have — WebCodecs was probed and refuses every
  WMA config.
* **Anything marked `[?]` or GLUE** in a `docs/RE_*.md` file is an open item by
  construction. That is what the marks are for.

---

## 7. File inventory

```
BUILD / RUN
  Makefile                      desktop (`make`) and web (`make wasm`) targets
  src/burnout3_isodata.c/.h     THE DATA MODEL: disc -> assets, materialise-on-miss
  src/burnout3_isoshim.h        force-included; redirects fopen/access/IMG_Load
  src/burnout3_full.c           the harness: frame loop, frontend, race flow
  src/burnout3_render.c/.h      THE RENDERER: static VBOs + one GLSL program.
                                The world, the scenery, the props and the HUD
                                draw through it and nothing else; there is no
                                fixed-function path left in those four.
                                docs/web/webprof_sweep.md "fourth wave" has the
                                call counts, the pixel gates and what is left.
  src/burnout3_rt.c/.h          tier 4r: the OPTIONAL ray-traced sun shadow's
                                switch (build/settings.cfg), its per-track BVH
                                loader and a GL-free reference traversal the
                                suites execute.  OFF by default, and with it
                                off the frame is the build before it, bit for
                                bit.  INSPIRED -- see docs/PHOTOREALISM.md.
  src/burnout3_backend.c/.h     per-feature RE-vs-retail switch (build/backends.cfg)
  src/burnout3_emu.c            the bridge to the Unicorn sidecar
  src/*_runtime.h               loaders that replaced the eight baked-in headers

EXTRACTION
  tools/cextract/               the C pipeline; cx_main.c is the `cxtract` driver
  tools/cextract/verify_cextract.py   the byte-identity acceptance gate
  tools/py_extract_archive/     the archived Python oracle — IMMUTABLE, read its README
  tools/extract_*.py            forwarding stubs onto the archive (validators import them)

GROUND TRUTH
  tools/xbe2elf.py              XBE -> correctly mapped ELF32   [essential first step]
  tools/apply_ghidra_types.py   applies structs + prototypes to the DB, self-verifying
  tools/xboxkrnl_ordinals.py    kernel ordinal -> name table
  tools/b3_emu_server.py        runs retail x86 under Unicorn for the live backends
  tools/emulate_*.py            per-subsystem emulation oracles
  tools/validate_*.py|.c        the differential suites
  tools/probe_fields.py         identifies struct fields by perturbation
  tools/field_usage.py          static read/write map of struct accesses
  tools/find_bgv_parser.py      INVALID SCORING -- plumbing is reusable, metric is not

DOCS
  README.md                     build, run, and the honest real/verified/glue split
  TODO.md                       the live open-item board
  docs/RE_MASTER.md             master index into the per-subsystem records
  docs/PHYSICS_GLUE_LEDGER.md   the physics ledger
  BGV_EXTRACTION.md             the .bgv car geometry format
  INTEGRATION_NOTE.md           the destructible-props evidence record
```

**`src/burnout3_full.c` is not decompiled code.** It is an original harness
written to have something runnable, and it still is: recovered retail logic
lives in the dedicated modules beside it and is marked with its addresses. The
repo previously claimed to be a "full decompile" that was "FULLY PLAYABLE"; that
was false and the claim has been removed. Please keep it that way — mark
provenance on everything.

---

## 8. Credits

Track/texture formats and the VDB layout come from the Burnout Modding community:
- EdnessP's Noesis plugin `fmt_Burnout3LRD.py` — burnout.wiki, discord.gg/8zxbb4x
- `Sokka06/burnout-data-tool` — VDB/VList reader, and the reference VDB dump

The extractors here are independent reimplementations so the data can be used
without Noesis, but the format knowledge is theirs.
