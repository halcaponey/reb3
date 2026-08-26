# Where the data comes from

**The setup walkthrough is in the [README](../README.md)** — two commands, and
the game reads your own disc. This file is the reference behind it: what the
game does with the disc at run time, how to run the extractor out of band, why
the ELF mapping matters for every RE tool, and what must never be committed.

This repository contains **no game content**. Not the executable, not the
tracks, cars, textures, audio or music, and not the data tables extracted from
them. What it contains is the code that reads those files and the recovered
logic that runs on them.

---

## 1. Your copy of the game

A dump of a disc you own, developed against the NTSC-U (USA) release. Either
form works and the port detects which:

* an **XISO image** — `Burnout 3 - Takedown (USA).xiso.iso` or similar;
* an **expanded directory** — whatever folder holds `default.xbe`:

```
Burnout 3 Takedown/
├── default.xbe
├── Data/            Globalus.bin, vdb.xml
├── GLOBAL/
├── pveh/            player + traffic vehicles (.bgv/.btv, .hwd/.lwd engine audio)
├── sound/           .awd audio dictionaries
└── Tracks/          per-track static.dat, streamed.dat, .bgd, .xwb, .rws
```

Point everything at it once:

```bash
export B3_GAME_ROOT="/path/to/Burnout 3 Takedown"
```

`$B3_GAME_ROOT` is the last step of the game's own image ladder
(`--iso=<path>` → `$B3_ISO` → `build/iso_path.txt` → `$B3_GAME_ROOT`), and it
is what every tool in `tools/` resolves through `tools/b3_paths.py`. Those
tools fail with an explicit message if it is unset, and they match path case
insensitively — the Xbox filesystem is case-insensitive, so dumps differ on
`GLOBAL/` vs `global/` and `Tracks/` vs `tracks/`.

**Nothing here has a path baked in.** No retail path is compiled into the C
either; `src/burnout3_isodata.h` and the `tools/cextract` defaults are all
empty strings with the environment in front of them.

---

## 2. What happens at run time

`./burnout3` opens the image and materialises assets on demand. The mechanism
is one seam:

* `src/burnout3_isoshim.h` is force-included into every `src/` translation unit
  and redirects `fopen`, `access`, `IMG_Load` and `SDL_LoadWAV`.
* Every loader still opens a literal `build/...` path. `b3_iso_resolve()` maps
  that to the **ISO cache** (`build/.isocache`, moved by `$B3_ISO_CACHE`).
* On a cache miss it runs the `tools/cextract` **stage** that produces the file
  — in process, against the disc — and then returns the cache path.

Not one loader had to be edited, which is the property the design was chosen
for. The exhaustive list of what the seam covers, and what is deliberately
outside it, is the comment block at the top of `src/burnout3_isodata.h`. Read
it before adding a loader.

`--build` selects the old pre-extracted `build/` tree instead. That is the
debug path; it still works, and section 3 is how to fill it.

### Nothing retail is compiled in

Eight generated headers used to bake retail data into `src/`. All eight are
gone, replaced by `src/*_runtime.h` loaders:

| gone | replaced by | reads |
|---|---|---|
| `burnout3_track_paths.h` | (the nav loader) | `build/tracks/<id>/route.bin` |
| `burnout3_start_grid.h` | (the grid loader) | `build/tracks/<id>/grid.bin` |
| `burnout3_ai_pace.h` | `burnout3_ai_pace_runtime.h` | `build/tracks/<id>/pace.bin` |
| `burnout3_traffic_data.h` | `burnout3_traffic_runtime.h` | `build/tracks/<id>/traffic.bin` |
| `burnout3_trackselect.h` | `burnout3_trackselect_runtime.h` | your `Globalus.bin` + the ELF |
| `burnout3_car_physics.h` | `burnout3_car_physics_runtime.h` | `build/cars/car_physics.bin` |
| `burnout3_vehicle_data.h` | `burnout3_vehicle_data_runtime.h` | `build/cars/roster.bin` |
| `burnout3_font.h` | `burnout3_font_runtime.h` | `build/frontend/font.bin` |

...plus the in-race HUD's Globalus **labels**, which were typed into
`src/burnout3_hud.c` as English literals with their recovered index in a
trailing comment (`burnout3_hudstr_runtime.h` resolves them out of your own
`Globalus.bin` now).

`tools/validate_no_baked_data.py` is the permanent gate and asserts both
halves: the headers are absent and nothing references them, **and** the
replacements really come off disk, proven by the loaders' own log lines on a
real boot.

What *is* still committed in `src/` is recovered **program structure**, not
content: `burnout3_physics_params.h` (the ValueDB registrar's parameter names
and byte offsets), the `*_ranges.h` tables and `burnout3_vehicle_retail.h`
(retail's own struct offsets, each with a `_Static_assert`). Those are the
shape of the game's code, which is what this project recovers.

---

## 3. Extracting out of band — `cxtract`

The same stages the game runs on a cache miss are available as a standalone
driver, which is how you fill a `build/` tree for `--build`, or produce assets
to look at.

```bash
tools/cextract/build.sh /tmp/cxtract          # build the driver
/tmp/cxtract --list                           # the per-track stages
/tmp/cxtract --list-global                    # the car / art / audio families
/tmp/cxtract --track US_C3_V1 --out build --game "$B3_GAME_ROOT"
/tmp/cxtract --all-global     --out build --game "$B3_GAME_ROOT"
```

14 per-track stages (`CX_STAGE_LIST` in `tools/cextract/cx_extract.h`): `tlist`,
`track`, `textures`, `collision`, `envmap`, `bgd_paths`, `traffic`,
`traffic_cars`, `nav_edges`, `start_grid`, `pace`, `props`, `scenery`,
`light_probes` — plus the dump-global car, art, audio and generator families.
`$B3_TRACK` picks the circuit when `--track` is not given. Nothing is hardcoded
per track: the ids come from the shipped track list.

### The Python archive is the oracle

`tools/py_extract_archive/` holds the original Python extractors, **unmodified**
apart from the `$B3_GAME_ROOT` path resolution every tool here carries.
`tools/cextract/verify_cextract.py` is the permanent differential gate: it
demands the C output be **byte-identical** to a fresh run of the archive
(pixel-identical for PNGs) and fails on a file present on only one side.

Never edit an archived tool to make the gate pass — the archive is the spec.
`tools/py_extract_archive/README.md` says which tool each `cxtract` stage
replaced. The old `tools/<name>.py` paths remain as forwarding stubs, because
several validators import them as parsing libraries.

---

## 4. The corrected ELF

Every RE tool and every differential suite reads `build/burnout3.elf`, not the
XBE. This is the one place people usually go wrong.

```bash
python3 tools/xbe2elf.py "$B3_GAME_ROOT/default.xbe" build/burnout3.elf
```

**Do not load `default.xbe` into a disassembler as a flat binary.** Each XBE
section has a different `VA − file_offset` delta, so every absolute data
reference lands on the wrong bytes — and it fails *silently*. Measured on this
image: `0x003A2D50` is `2.5` but reads as `0.0`; `0x00384A80` is `0.15` but
reads as `22.0`; one float constant reads as the string `"Score/Burnout
Points"`.

`xbe2elf.py` rebuilds the real address space as an ELF32 — one `PT_LOAD` per
section at its true VA, BSS materialised, `e_entry = 0x001D2807`.

|  | Flat binary | Corrected ELF |
|---|---|---|
| Functions found | 7,267 | 7,434 |
| Entry point | `0x0` (bogus) | `0x1D2807` (real) |
| Junk functions in header/`.data` | 7 | 0 |
| Float constants | wrong | correct |
| Data/string xrefs | meaningless | resolve |

Old→new address translation: `new = old + 0x10000`, **`.text` only**.

If you import it into Ghidra, use the **ELF loader and do not pass an explicit
language ID** — doing so forces the Binary loader and reproduces the bug.
`tools/apply_ghidra_types.py` then applies the recovered structs and prototypes
to the database.

`build/burnout3.elf` is a derived copy of the retail executable. It is
gitignored. Do not redistribute it.

---

## 5. Checking it worked

```bash
make test-soup-ray test-traffic-pool test-traffic-reservations test-audio-ring
python3 tools/validate_port.py            # the physics differential vs retail
python3 tools/validate_carcol.py          # car-vs-car collision vs retail
python3 tools/validate_no_baked_data.py   # nothing retail is compiled in
```

The `validate_*` suites are differential tests: they execute the *retail* code
out of `build/burnout3.elf` under Unicorn and compare it against this port,
function by function. They need the ELF from section 4. If a suite fails after
you change something, the port diverged from the game — that is the whole point
of them.

`validate_carcol.py` compares against the real collision hulls, which live in
the `.bgv`/`.btv` files rather than in any extracted asset. Pull them out once:

```bash
python3 -c "import sys; sys.path.insert(0,'tools'); \
            import emulate_carcol as ec; ec.extract_hulls()"   # -> build/cars/*.hull
```

---

## 6. What must never be committed

`.gitignore` already covers all of it, but for the avoidance of doubt:

* `default.xbe`, any disc image, and `build/burnout3.elf`
* everything under `build/` — geometry, textures, audio, music, collision, the
  per-track binaries, the ISO cache
* screenshots or video of the retail game, and any retail display string

Cite a retail frame or an address in the documentation freely — the `docs/`
here do, by filename. Do not check the bytes in.

Format credits for the two formats first documented by other people are in
[THIRD_PARTY.md](../THIRD_PARTY.md).
