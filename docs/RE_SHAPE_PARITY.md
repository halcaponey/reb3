# Shape parity with retail — status and plan

**Goal.** Every port structure laid out exactly as the game lays it out, so the
port's C and the retail x86 address the same bytes at the same offsets. Then a
feature can be switched between "our recovered C" and "the game's own code"
with nothing marshalled in between, and the switch is a build/runtime choice
rather than a translation layer.

The exercise is not only about running retail code. Forcing the layout to match
surfaces defects a repacked struct hides — three found so far, all listed below.

> **Status (2026-08-22): the plan in this document is DONE — read it back to
> front.** It was written by appending wave after wave, so the early sections
> state the state of play at the time and were never retracted. What is true
> now, and what each stale passage should be read against:
>
> * **All ten features switch.** `build/backends.cfg` carries a live line for
>   `physics`, `carcol`, `ai`, `traffic`, `td_rules`, `score`, `crash`,
>   `camera`, `sfx` and `hud`, and `src/burnout3_emu.c` has a retail path for
>   each. Every passage below saying only `physics` is wired, that eight or
>   five features are "not wired", or listing what blocks `ai` / `crash` /
>   `traffic` / `hud` / `sfx`, is superseded — including the "What blocks the
>   five unwired features" section and the "Order of work" list.
> * **The interop marshaller is gone.** `b3_emu_apply`, the field-by-field
>   copy, no longer exists; the retail code addresses the port's own structs.
> * **`physics=retail` is drivable.** The "drops the car through the floor"
>   entry is historical; a full retail-physics run holds a normal race pace.
> * The early duplicate parity table's `B3RigidBody.frame` "divergent" and
>   `B3VehicleFull` "not started" rows are both at parity now, as the later
>   sections of this same file record.
>
> The addresses, offsets, assert counts and the defect write-ups are the
> evidence and stand as recorded.

---

## Where it stands

Measured by `_Static_assert` on every field, so the build fails if any offset
drifts. **105 fields are asserted at retail's own offset.**

| struct | retail | port | status |
|---|---|---|---|
| `B3EngineTransmission` | span `0x90`, omega `+0x54`, gear `+0x80` | same | **parity** (was already correct) |
| `B3WheelSim` | stride `0xC0`; spin `0x58`, cur_len `0x64`, attach `0x74`, surface `0xB0` | same | **parity**, 9 asserts |
| `B3RigidBody` | I0 `0x10`, Iw `0x40`, invF `0x70`, vel `0xB0` … defl `0x130`, frame ptr | same | **parity**, 13 asserts |
| `B3VehicleFull` | vehicle object, fields `0x148`..`0x1A00` | same | **parity**, 83 asserts |
| `B3CrashVehicle` | a VIEW of the vehicle, `veh+0x40`..`veh+0x1534` | same | **parity**, 26 asserts |
| `B3CrashContactAcc` | the `0xE0` record `FUN_0011AEF0` builds at ESP+0x80 | same | **parity**, 11 asserts |
| `B3PanelPiece` | class-6 body: I0 `0x10`, bbmax `0x1D0`, mass `0x1F0` | same | **parity**, 4 asserts |
| `B3CarHull` | the `0x600` hull record: counts `0x18`, planes `0xA0`, verts `0x320`, edges `0x480` | same | **parity**, 6 asserts, `sizeof 0x600` |
| `B3CarContact` | point `0x00`, normal `0x10`, impact `0x20`, hit `0x2C`, slam `0x2D` | same | **parity**, 5 asserts |
| `B3AiAggroParams` | the aggro parameter block, `+0x0B8`..`+0x11C` | same | **parity**, 26 asserts |
| `B3AiState` | `AI+0x770`..`AI+0x157C` | same | **parity**, 21 asserts |
| `B3AiAggroSpeed` | `AI+0x790`..`AI+0x1570` | same | **parity**, 13 asserts |
| `B3AiParams` | the driver parameter block, `+0x000`..`+0x020` | same | **parity**, 9 asserts |
| `B3BoostBar` | `+0x024`..`+0x055` | same | **parity**, 13 asserts |
| `B3TdfxTimer` | `+0x000`..`+0x028` | same | **parity**, 11 asserts |
| `B3CatRecord` | the score category record, `+0x00`..`+0x13` | same | **parity**, 6 asserts |
| `B3TdCamRecord` | `+0x04`..`+0x0F` | same | **parity**, 6 asserts |
| `B3TdWallResponse` | `veh+0x0B0`..`veh+0x194` | same | **parity**, 4 asserts |
| `B3TakedownScore` | the score object, `+0x068`..`+0x111C` | same | **parity**, 4 asserts |
| `B3TdfxCamera` | `+0x018`..`+0x020` | same | 3 asserts (`quat` overlaps) |
| `B3CamFollow` | `mode+0x018`..`+0x026` | same | 3 asserts (`look_back` overlaps) |

Not layout targets, and this distinction matters: `B3CrashModeState`,
`B3WreckState`, `B3WreckAftertouchIn` and `B3PanelSet` carry annotations like
`// veh+0x1354 semantics`, `// veh+0x190 stand-in`, `// .bgv+0xAC4`. Those are
**provenance** -- "this value came from there" -- not **layout** -- "this field
lives there". Only a layout claim is a parity target. Most of `crash.h`'s 143
annotations and all of `panels.h`'s `B3PanelSet` ones are provenance, so those
headers were far closer to done than the annotation count suggested.

Spot-checked against retail: `mass 0x1F0`, `com_height 0x1F4`, `half_ext 0x1D0`,
`center_off 0x1E0`, `wheel[4] 0x820` (stride `0xC0`), `wheel_count 0x1169`,
`trans 0x1448`, `throttle 0x1400`, `steer 0x1408`, `drift_state 0x1524`,
`timer 0x152C`, `flags 0x1353`, `class 0x215` — all exact.

`B3VehicleFull` is `0x1B12`: the retail window `0x0000..0x1A00`, then the
harness-side tail. Everything past the window is state with no recovered
vehicle offset, so it cannot be trampled by retail code running over the front.

Baseline when this started: **0 of 56** annotated fields sat at retail's offset.

All suites green: `validate_port` 153/153, `gameplay` 91/91, `carcol` 1012/1012,
`props` 337/337, `crash_traj` 158/158, `takedown` 974/974, `td_rules` 532/532,
`hud` 760/760, `ai` 189/189, `crashcinema` 115/115, `carfx` 223/223. (`sfx`
337/368 — pre-existing, unchanged by this work; verified against the tree
before any of it.) Game stable at 59 fps.

---|---|---|---|
| `B3EngineTransmission` | span `0x90`, omega `+0x54`, gear `+0x80` | same | **parity** (was already correct) |
| `B3WheelSim` | stride `0xC0`, spin `0x58`, cur_len `0x64`, attach `0x74`, surface `0xB0` | same | **parity** |
| `B3RigidBody` (dynamics) | I0 `0x10`, Iw `0x40`, invF `0x70`, vel `0xB0`, dir `0xC0`, omega `0xD0`, angmom `0xE0`, force `0xF0`, torque `0x100`, impF `0x110`, impT `0x120`, defl `0x130` | same | **parity**, 12 fields asserted |
| `B3RigidBody.frame` | separate object via `v+0x204` | inlined at `+0x140` | **divergent** — see below |
| `B3VehicleFull` | ≥ `0x1A00` | `0x740` | not started |
| `B3CrashVehicle` | a VIEW of the vehicle, `veh+0x40`..`veh+0x1534` | same | **parity**, 26 asserts |
| `B3CrashContactAcc` | the `0xE0` record `FUN_0011AEF0` builds at ESP+0x80 | same | **parity**, 11 asserts |
| `B3PanelPiece` | class-6 body: I0 `0x10`, bbmax `0x1D0`, mass `0x1F0` | same | **parity**, 4 asserts |
| `B3CarHull` | the `0x600` hull record: counts `0x18`, planes `0xA0`, verts `0x320`, edges `0x480` | same | **parity**, 6 asserts, `sizeof 0x600` |
| `B3CarContact` | point `0x00`, normal `0x10`, impact `0x20`, hit `0x2C`, slam `0x2D` | same | **parity**, 5 asserts |
| `B3AiAggroParams` | the aggro parameter block, `+0x0B8`..`+0x11C` | same | **parity**, 26 asserts |
| `B3AiState` | `AI+0x770`..`AI+0x157C` | same | **parity**, 21 asserts |
| `B3AiAggroSpeed` | `AI+0x790`..`AI+0x1570` | same | **parity**, 13 asserts |
| `B3AiParams` | the driver parameter block, `+0x000`..`+0x020` | same | **parity**, 9 asserts |
| `B3BoostBar` | `+0x024`..`+0x055` | same | **parity**, 13 asserts |
| `B3TdfxTimer` | `+0x000`..`+0x028` | same | **parity**, 11 asserts |
| `B3CatRecord` | the score category record, `+0x00`..`+0x13` | same | **parity**, 6 asserts |
| `B3TdCamRecord` | `+0x04`..`+0x0F` | same | **parity**, 6 asserts |
| `B3TdWallResponse` | `veh+0x0B0`..`veh+0x194` | same | **parity**, 4 asserts |
| `B3TakedownScore` | the score object, `+0x068`..`+0x111C` | same | **parity**, 4 asserts |
| `B3TdfxCamera` | `+0x018`..`+0x020` | same | 3 asserts (`quat` overlaps) |
| `B3CamFollow` | `mode+0x018`..`+0x026` | same | 3 asserts (`look_back` overlaps) |

Not layout targets, and this distinction matters: `B3CrashModeState`,
`B3WreckState`, `B3WreckAftertouchIn` and `B3PanelSet` carry annotations like
`// veh+0x1354 semantics`, `// veh+0x190 stand-in`, `// .bgv+0xAC4`. Those are
**provenance** -- "this value came from there" -- not **layout** -- "this field
lives there". Only a layout claim is a parity target. Most of `crash.h`'s 143
annotations and all of `panels.h`'s `B3PanelSet` ones are provenance, so those
headers were far closer to done than the annotation count suggested.

Baseline when this started: **0 of 56** annotated fields sat at retail's offset.

All suites green at every step: `validate_port` 153/153, `validate_gameplay`
91/91, `validate_carcol` 1012/1012, frozen-soup and traffic-pool OK.

---

## Defects the exercise surfaced

**1. The wheel record could never have been walked by retail code.** It was
`0x80` against retail's `0xC0` stride, with `spin` at `0x60` instead of `0x58`
— every field after the contact block was eight bytes late. Fixed.

**2. Harness state was squatting on retail's bytes.** `local_x`, `local_z` and
`frame_y` lived inside the wheel record with no retail offset at all. Retail
code running over that array would have trampled them, or been corrupted by
them. They now live in `wheel_local_x/_z/wheel_frame_y` on the vehicle, off
retail's memory. **Rule going forward: anything without a recovered offset does
not belong in a retail-shaped struct.**

**3. Six annotations name a different object.** `config +0x14C`,
`config +0x154`, `config +0x190`, `owner +0x1350`, `racecar+0x1920`,
`racecar+0x15CC`. Placing those at *vehicle* offsets is what produced the two
apparent "conflicts" in the first generator run — they are not conflicts, they
are an address-space bug in the annotation format. **Offsets need an owning-object
qualifier before they can be trusted as a layout source.**

---


## The recurring defect: a multi-byte field on a single-byte offset

Found **nine times**, independently, and it is the clearest signal that a
struct was repacked rather than mapped:

| field | retail | port had | consequence |
|---|---|---|---|
| `wheel_count` | byte at `veh+0x1169` | `int` | unaligned; shifted every field after it |
| `nverts/nplanes/nedges` | bytes at `hull+0x18/0x19/0x1A` | `int` each | an int at `0x18` swallows `0x19` and `0x1A` |
| `hit` / `slam_class` | bytes at `+0x2C/+0x2D` | `int` each | an int at `0x2C` swallows `0x2D` |
| `boosting`/`fixed_burn`/`ramp_done` | bytes at `+0x52/0x53/0x55` | `int` each | an int at `0x52` swallows both |
| td attribution arrays | byte`[6]` at `racecar+0x15C0/+0x15C6` | `int[8]` | see the 6-vs-8 trade-off |
| `wall`/`has_obj` | bytes at `+0x00/+0x01` | `int` each | an int at `0x00` swallows `0x01` |
| `airborne`/`drifting` | bytes at `racecar+0x10C0/+0x10C2` | `int` each | |
| `open`/`tier`/`prev_tier`/`count` | bytes at `rec+0x10..0x13` | `int` each | four in a row |
| `contact` | byte at `+0xB3` (sfx wheel) | `int` | |

The tell is always the same: **consecutive single-byte offsets**. If two
recovered fields are one byte apart, neither can be wider than a byte, whatever
the port declared. Worth checking for directly in any header not yet migrated.

Fixing `wheel_count` also removed the need for `__attribute__((packed))`
entirely: with retail's own types, every field lands on a naturally-aligned
offset and the layout holds without packing. That took the build from **246
`address-of-packed-member` warnings to 0** — which matters, because the Android
target is ARM, where an unaligned member pointer can fault rather than just run
slowly.

## The frame separation — DONE

`v+0x204` holds a POINTER to a separate frame object (`4x4 + file ptr`, aliased
at `v+0xCC0`; see `CTX0` in `tools/emulate_pipeline.py`). The port inlined the
matrix. `B3RigidBody.frame` is now `float (*frame)[4]`, which keeps all ~345
`frame[r][c]` sites indexing identically while the 64-byte matrix leaves the
retail window — it used to collide with `contact_pt_160` at any offset it was
parked at, which is what blocked `B3VehicleFull`.

All ten suites green afterwards, game stable at 59 fps.

### The hazard taxonomy — this is what made the first attempt fail

A pointer member means **anything that zeroes memory containing a body drops
the binding**, and the compiler cannot see it. Four distinct shapes, all found:

1. **`memset` of the body itself**, after the bind. 6 sites. Bind must come
   after the zeroing, never before.
2. **A function that zeroes a body it does not own** (`b3p_body_setup`,
   `b3p_mirror_rb`, `carcol_synth_rb`). The fix is to carry the pointer across
   the memset inside the callee, so any caller's binding survives.
3. **Zeroing a CONTAINER of a body** — `memset(&g_body[i], 0, sizeof ...)` on a
   `B3PropBody`. The body is nested, so the wipe is invisible at the call site.
   4 sites in the prop pool alone.
4. **Use before the owner's init runs.** The vehicle grid and the prop pool are
   both touched before `b3_vehicle_full_init` / `b3_props_load`. While the
   matrix was inline this read zeros; through a pointer it faults. Both now
   bind from an idempotent one-shot at the entry points.

5. **`memset` no longer reaches the matrix.** Callers have always relied on
   `memset(&rb, 0, sizeof rb)` to clear the 4x4 along with everything else.
   With the matrix outside the struct that memset clears the pointer's slot but
   not its target, so a body inherits whatever was on the stack. This showed up
   as an intermittent NaN in the physics about **one run in three**, with all
   twelve suites still green -- the suites seed every field they use, so they
   never see it. `b3_rigid_body_bind_frame` now zeroes the storage as well as
   binding it.

Shapes 3 and 4 are the ones a sweep for `B3RigidBody` never finds, and they are
why the first attempt died by segfault three times over. Shape 5 is worse: it
produces no crash and no failing test, and was only caught by running the game
repeatedly and comparing against a stashed baseline (`git HEAD`: 0 NaN in 4
runs; the change: 1 in 3). **A struct-shape change needs a run-the-game
regression check, not just a green suite.**


### Hazard 6: positional aggregate initializers

Inserting parity padding into a struct that is initialised **positionally**
silently shifts every value by however many pad members precede it. The
compiler is happy; the values are simply wrong.

`b3_ai_aggro_params` and `b3_ai_params` were both positional, with a
`/* field */ value,` comment per line. After the relayout `validate_ai` fell
189 -> 160, and the failures read like behaviour changes ("timer 20 != 50",
"state 1 != 4") rather than what they were.

Both are now **designated** (`.field = value,`), which is padding-proof. Two
follow-on details, each of which cost a re-run:

* three fields were written as one positional row (`26.2f, 40.0f, 60.0f`) and
  had to be named individually -- a bare run after designated entries continues
  from the last named position, which is not where it used to land;
* `validate_ai.py` reads that initializer **as source text**, matching
  `/* field */`, so it had to learn the designated form. Its parser is also
  line-based, so the opening brace must not share a line with the first field.

**Any struct that gains padding must have its initializers checked.** Grep for
`^TYPE var = {` on every relaid struct before trusting a green build.


### Hazard 6 caught me a second time

`B3PfxDesc`, `B3PfxEmitter` and `B3PfxSurface` are initialised as POSITIONAL
ARRAYS of structs (`static const B3PfxDesc B3_PFX_DESC[] = { { "fxdebris1", 3,
0, 2.50f, ... }, ... }`). I ran the grep for positional initializers, it printed
all three, and I wrote "(none above = safe)" and relaid them anyway. Six
interior pads went into `B3PfxDesc`, shifting every value in every row.

**`validate_particlefx` stayed at 600/600 through all of it.** The suite does
not read those descriptor values, so nothing failed -- the particle system would
simply have been wrong at runtime. Reverted.

Two rules out of this:

* a positional initializer and a relayout are **mutually exclusive**; convert to
  designated first or do not relay the struct;
* **a green suite is not evidence the initializers survived.** Check for
  interior padding directly: any `_pad` member before the last real field in a
  struct with a positional initializer means the values have moved.

### Also required

* 5 `sizeof(rb.frame)` sites silently went 64 -> 8 and became `16 * sizeof(float)`.
  The other 6 `sizeof(...frame)` hits size from unrelated types and were left alone.
* `B3RigidBody car = *car_rb;` in `b3_props_test_contact` aliased the caller's
  matrix; it now copies the rows into its own storage.
* `B3_RIGID_BODY_LOCAL(name)` declares a local body already bound. Use it
  instead of a bare `B3RigidBody x;`.

## Order of work

1. Qualify every offset annotation with its owning object; re-run
   `tools/gen_retail_struct.py`.
2. The frame-object separation above.
3. `B3VehicleFull` top level, using the generated shape as the target.
4. `burnout3_crash.h`, then `burnout3_panels.h`.
5. Wire each feature onto `build/backends.cfg` as its structs reach parity, and
   delete the marshalling in `src/burnout3_emu.c` — shape parity is what makes
   that file unnecessary.

## The switch

`build/backends.cfg`, auto-created, one line per feature, `re` or `retail`.
Ten features are listed; **only `physics` is wired**, and it currently runs the
interop path (`b3_emu_apply` marshals field by field, `burnout3_full.c` gathers
collision geometry and mirrors it into game space). That code is scaffolding to
be removed at step 5, not the intended end state.

---

## Coverage, not just alignment — and the scatter transfer that solves it

With `B3VehicleFull` at retail's offsets I first tried the obvious thing: hand
the emulator the whole `0x1A00` window as raw bytes. `b3_emu_apply` — the
field-by-field marshaller — was deleted.

**That does not work, and the reason is the useful part.** Shape parity aligns
the fields we have RECOVERED. It does not make `B3VehicleFull` a *complete*
model of retail's vehicle: 82 recovered ranges cover **1240 bytes, 18.6%** of
the object. The other 81.4% is `_pad` here and real state there — config and
racecar pointers, wheel frame links, class and state bytes. The emulator seeds
all of it. Writing our window over the top replaces that seed with zeros and
the substep loop spins on NaN (measured: an all-zero window hangs
`FUN_0011BE50` past a 60,000,000-instruction budget).

* **alignment** lets both sides name the same byte — done
* **coverage** would let one side own the whole object — not done
* **the scatter transfer** needs only the first

### The scatter transfer — working

`tools/gen_vehicle_ranges.py` reads the parity assertions (the same ones that
guarantee the offsets, so the table cannot drift from the struct) and emits
`src/burnout3_vehicle_ranges.h`: 82 recovered `{offset, length}` ranges. Each
frame only those ranges move, at the SAME offset on both sides, and everything
else stays exactly as the emulator seeded it. No field is converted in either
direction — a partial byte copy between two address spaces, which is precisely
what parity bought.

**`physics=retail` runs.** 4,200 retail frames over a 150-second run,
**15.16 ms average** against a 16.67 ms budget, no demotion, the game's own
`FUN_0011AEF0` chassis collision included. That budget is for ONE car; a second
does not fit without hosting Unicorn in-process instead of over a pipe.

Seven pointer slots inside the window (`+0x200` soup, `+0x204`/`+0xCC0` frame
object, `+0x13F4`/`+0x1568` racecar, `+0x13F8` config, `+0x14D8` self) hold host
addresses meaningless in the emulator's address space; the sidecar preserves
its own across every write. That is an address-space boundary, not a data
translation, and it survives full coverage.

---

## The switch: what is live

| feature | state | cost | notes |
|---|---|---|---|
| `physics` | **live** | 15.7 ms/frame, 1 car | scatter of the 82 recovered ranges; `RelocPipeline` runs the whole retail substep loop incl. `FUN_0011AEF0` |
| `carcol` | **live** | ~6x the RE path | `FUN_001121F0` / `FUN_00113960` over the port's bytes |
| the other 8 | not wired | — | each needs parity for its structs, a range/blob mapping, and a sidecar entry |

Both live features run **retail's own code over the port's own structures**,
with nothing converted in either direction. `b3_emu_apply` -- the field-by-field
marshaller -- is deleted.

### What made carcol possible

`B3RigidBody` scatters straight to `veh+0x00`; bbmax/bbmin/mass go to
`veh+0x1D0`/`+0x1E0`/`+0x1F0`; and `B3CarHull` is byte-identical to the retail
`0x600` hull record, so it needs no conversion at all. `B3CarContact` matches
the PAIR record's offsets (`+0x00`/`+0x10`/`+0x20`/`+0x2C`/`+0x2D`), so the
result comes back as itself.

Validated against `emulate_carcol` on an identical head-on: **both report
`hit=1 slam=1 impact=16058.5 crash_a=1`.**

### The coverage lesson, again

The first attempt failed because `b3_carcol_hull_from_record` decoded six
fields out of the record and left the rest of the `0x600` zeroed -- so the
emulator relinked a hull full of holes and died. Now that `B3CarHull` IS the
record, that loader is a single `memcpy` of the whole thing, which keeps the
parts nobody has named yet. **Shape parity turns a decoder into a copy, and the
copy is what carries the unrecovered remainder.** That is the same lesson as
the vehicle's 18.6% coverage, with a happier ending: a hull record is small
enough to carry whole.

### Cost, honestly

Both paths are for verification, not play. `physics` fits one car in a 16.67 ms
budget; `carcol` runs the game roughly six times slower because every candidate
pair costs a seed and a resolve over a pipe. Hosting Unicorn in-process instead
of over stdio is the obvious next lever -- `libunicorn.so.2` is present, only
the C header is missing.

---

## `B3TdCar`: the 8-car headroom WAS the divergence

`B3TdCar` is a view of the racecar object. It could not be made byte-exact
while the port carried `B3_TDR_MAX_CARS = 8`: the attribution arrays sit at
`racecar+0x15A8` (`claim`), `+0x15C0` (`claim_aftertouch`), `+0x15C6`
(`claim_psyche`) and the next known field is `+0x15CC` -- **6 apart**, because
retail's grid is six cars. At eight, `claim` alone spanned `0x15A8..0x15C8`,
swallowing both arrays after it, so nothing past them could be at its offset.
The same repeats at `+0x1689`/`+0x168F`.

I first wrote this up as a capability trade needing an outside decision. That
was wrong. The brief is *update the data models until we have true shape parity
with retail*, and eight-car headroom is exactly the sort of divergence that
removes. **Set to 6.** Nothing exercised more than six anyway: the game runs a
six-car grid, `validate_td_rules` uses two, `validate_crashcinema` four.

The three attribution arrays are **byte** arrays in retail, not int arrays (one
entry per car, six apart), as is `psyche_armed`.

Result: **33 fields at retail's offsets, zero bumped.** Spot-checked exact:
`cls 0x1920`, `grid 0x19BC`, `race_state 0x134C`, `crashed 0x18FA`,
`claim 0x15A8`, `claim_aftertouch 0x15C0`, `claim_psyche 0x15C6`,
`psyche_armed 0x1688`, `taken_down_by 0x1689`, `revenge_flag 0x168F`.

This unblocks `td_rules` as a switchable feature: it is stateful, so the
emulated world must own the per-car score state, which needed this struct
byte-shareable.

## Tooling notes

`tools/relayout_struct.py` grew three fixes worth knowing about, each found by
it producing wrong output:

* **anchor on the closing tag.** A forward regex from `typedef struct` happily
  starts at an EARLIER struct and swallows everything up to the closing tag,
  silently merging two objects. Walk back from `} NAME;` with brace counting.
* **resolve `#define` array bounds.** Sizing `[B3_HULL_MAX_PLANES]` as if it
  were absent shrinks the field 40x and shifts every offset after it.
* **drop its own prior padding.** Re-running on an already-relaid struct
  otherwise carries the old `_pad` members through and emits fresh ones,
  producing duplicate member names.

And the field-name extractor must take the **last identifier before the
semicolon**: matching `type name` backtracks on single-word types, so
`unsigned col[3];` yields the name `l`.

## `B3ScoreEvents`: the same byte modelled twice

Attempting parity on `B3ScoreEvents` (a view of the score object,
`score+0x358..0x18FB`) surfaced a duplication rather than a layout problem.

`air` is an embedded `B3CatRecord` at `score+0x358`. That record is `0x14` long
and its own fields run `+0x00..+0x13`. So `score+0x358 + 0x10` is `0x368` --
which is exactly where `B3ScoreEvents` **separately** declares `air_active`. The
same is true for `onc_active` (`0x384`), `drift_active` (`0x3A0`),
`nm_active` (`0x428`) and `rub_active` (`0x574`): five bytes, each modelled
both as a field of the embedded record and again as a sibling beside it.

Two models of one byte cannot both be at its offset, which is why the relayout
bumped exactly those five and nothing else.

**Fixed.** `B3CatRecord` gained its own `active` byte at `+0x10`, the five
siblings are gone, and all 35 uses across five files now read through the record
(`air.active`). Suites unchanged: `score_events` 160/160, `hud` 760/760.

The relayout of `B3ScoreEvents` itself is still not landed, and the assertion
that blocked it is worth keeping: it wanted `onc` at `score+0x374` with `air` at
`+0x358`, i.e. a **`0x1C` stride between category records**, not the `0x14` its
own fields span. So `B3CatRecord` has ~8 unrecovered bytes past `+0x13`, and it
cannot be embedded at the right stride while it also carries a harness-side
`minima` pointer (29 uses). Either the pointer moves out, or the view stays
unmapped. The parity assertion is what told us the stride -- reading the fields
alone would never have shown it.

This is the second time parity has surfaced a duplicate model -- `B3CrashVehicle`
was a second partial model of the vehicle object, reconciled earlier. The
exercise finds these because two models of one object cannot both sit at its
offsets, and the assertions say so at compile time.

---

## The switch: what is live, and the rule for what can be

| feature | state | retail entry | how it substitutes |
|---|---|---|---|
| `physics` | runs, **NOT drivable** | `FUN_0011ECF0` + BE50 chain | scatter of the recovered vehicle ranges -- see "physics=retail drops the car through the floor" below |
| `carcol` | **live** | `FUN_001121F0` / `FUN_00113960` | rigid body + hull record + contact record |
| `td_rules` | **live** | `FUN_00197BE0` | 33 racecar ranges, stateful world |
| `score` | **live** | `FUN_00197920` / `FUN_001979E0` | 22 score-object ranges |
| `camera` | **live** | `FUN_0015E550` | matrix + scalars in, eye/quat/fov out |
| `crash` | **live** | `FUN_0011AEF0` | 26 interface ranges to VEH, thiscall ECX = the vehicle, at retail's own substep call site |
| `ai` | **live** | `FUN_00105340` | racecar ranges to RC, AI-object ranges to RC+0x1A00, vehicle ranges to VEH -- three objects, three bases, EDI = the vehicle |
| `traffic` | **live** (spawn choices) | `FUN_001A5E30` / `FUN_001A5F90` | model + paint through retail, sharing the manager RNG. The population law `FUN_001A6070` stays on the port -- it reads the traffic manager, which this tree does not map |
| `hud` | **live** | `FUN_0004D310` | 6 category records in, 7 row slots out -- both shared objects at identical offsets |
| `sfx` | **live** (15 of 41 events) | the 17 emitters | the emitter runs for real to the PlaySound3D boundary; the captured {wave, gain, pitch} comes back |

### The rule

A feature is switchable when **the port and retail agree on the interface**, in
one of two ways:

1. **A shared object at identical offsets** -- physics, carcol, td_rules, score.
   This is what shape parity buys, and why those four needed their structs
   mapped first.
2. **A signature whose values map one-to-one** -- camera. `FUN_0015E550` takes a
   car matrix and scalars and returns eye/quat/fov/pitch/yaw, and
   `B3TdfxCamera` already holds exactly those, so the result lands by
   assignment. No struct-shape conversion exists to be a hack.

`hud` and `sfx` fail **both** tests: retail's paths there EMIT draw lists and
bank selections rather than mutating an object we hand over or returning values
the port already models. Substituting them needs a third mechanism -- capture
the emitted list and replay it -- and choosing that is a design decision, not a
parity problem.

### Two infrastructure hazards, each hit three times

* **A validator that links a switched module** needs the backend objects on its
  link line AND `os.environ['B3_BACKENDS'] = '/dev/null'`. Without the pin, the
  suites are not isolated from the live backend selection: flipping a feature
  silently changed `td_rules` to 467/530 and `crashcinema` to 86/87.
* **An emulator helper that builds a fresh Unicorn per call** leaks until the
  sidecar dies -- carcol at ~2900 pairs, score at ~1700 events. Cache the
  session where the helper allows it, collect on a cadence where it does not.

## A gather struct cannot be byte-shared with two objects

`B3AiCar` is the struct the port hands its AI driver. Its annotations name
**two** retail objects: 8 fields at racecar offsets (`+0x40` pos, `+0x30` fwd,
`+0x2413..15` the drift-lock flags) and 8 at physics-vehicle offsets (`+0xBC`
speed, `+0xC0` carAt, ...), plus 9 bare and 1 `rc`.

Laid out with `--owner racecar` it reaches 26 fields at retail's offsets and
every suite stays green -- but it **cannot** be used to switch the feature.
A struct has one layout. With the racecar fields placed, the vehicle-side ones
are harness-side, so scattering it into the emulated racecar leaves the
emulated VEHICLE at whatever `seed_car` defaulted to. Retail's `FUN_00105340`
then reads someone else's speed and heading, returns inputs for that car, and
the port's transmission blows up on them within a few frames. Measured: seven
calls, then SIGSEGV in `b3_engine_transmission_update`.

The fix is to **split it** -- a racecar view and a vehicle view, each laid out
for its own object, with the driver taking both. That is a signature change to
`b3_ai_drive`/`b3_ai_update` and their call sites, not a layout tweak.

The general rule this adds to the switchability test: a feature is switchable
when the port and retail agree on the interface, and **a gather struct spanning
two retail objects does not agree with either**. Parity for such a struct is
still worth having -- it caught four more byte-typed fields here, two of whose
own comments already said "racecar byte" -- but it does not by itself make the
feature switchable.

### And a second obstacle in the same struct: converted units

Splitting `B3AiCar` is not sufficient either. Two of its eight vehicle-side
fields hold **converted** values, not retail's bytes:

* `engine_rpm` -- the annotation says `v+0x149C * 9.549296`, i.e. retail stores
  **rad/s** and the port stores rpm (the same factor `b3_emu_apply` used before
  it was deleted);
* `lsdm_limit_mph` -- `v+0x13AC` with an `_mph` suffix against retail's m/s.

**Settled and fixed.** `validate_ai.py` already pinned the units, and it passes
189/189 against retail: it seeds `VEH+0x149C` as `rpm / 9.549296` -- so retail
stores **rad/s** there -- and `VEH+0x1470` raw, so that one is **rpm**. Only
`engine_rpm` actually diverged; `lsdm_limit_mph` was already in retail's units
and the suffix is only a label.

`B3AiCar.engine_rpm` is now `engine_omega` and holds the game's own rad/s. The
`* 9.549296` moved off the fill site (`burnout3_full.c`) and onto the one
comparison that needs rpm (`engine_omega * 9.549296 >= change_up_rpm`). The
probe in `validate_ai.py` converts at the call so its cases can stay in rpm.
All suites unchanged.

**A unit divergence is a data-model divergence.** It is invisible to a layout
audit -- the field is at the right offset and the right size -- and only shows
up when you try to share the bytes. Worth grepping for: a field whose comment
carries a conversion factor, or whose name carries a unit the annotation does
not.

**So `ai` needs three things, in order:** the units settled, `B3AiCar` split
into a racecar view and a vehicle view, then the wiring. The wiring itself is
built and working (`b3_emu_ai_drive`, sidecar `ai`/`airanges`); it is the data
model underneath that is not ready.

## What blocks the five unwired features

None is another instance of a pattern already built here. Each needs work of a
different kind, and the kind matters more than the size:

| feature | blocked on | kind of work |
|---|---|---|
| `ai` | `engine_rpm`/`lsdm_limit_mph` hold CONVERTED units; `B3AiCar` gathers from two objects | RE verification (units at `v+0x1470`), then a struct split. The wiring is built and tested. |
| `crash` | `FUN_0011AEF0` has no standalone calling convention -- it runs only inside the physics substep chain | RE: establish the convention |
| `traffic` | `FUN_001A6070` "needs the whole manager" -- `validate_traffic_mix.py` says so at the top, which is why it replays the population law as a model instead of executing it | RE: map and seed the traffic manager object |
| `hud`, `sfx` | retail EMITS draw lists and bank selections; there is no object it mutates and no return value the port models | design: choose a capture-and-replay boundary |

The three features I predicted would follow an existing pattern all turned out
not to: `td_rules` is stateful where `carcol` is not, `ai` is a two-object
gather with converted units, and `traffic` needs an object nobody has mapped.
Worth not projecting from the five that did work.

## `B3AiCar` / `B3AiInputs`: two objects in one struct, and an object retail does not have

The AI module carried the worst instance of the duplicate-model defect, and it
is worth writing down because the symptom was not a wrong value -- it was a
feature that could not be switched at all.

### The defect

`B3AiCar` was annotated as a view of the RACECAR and laid out at racecar
offsets, but ten of its fields were annotated `v +0x...` -- the PHYSICS
VEHICLE, a different object, reached through a pointer at racecar+0x2440.
Because a struct can only be laid out for one base, those ten sat at vehicle
offsets inside a racecar-shaped struct, and `gen_ai_ranges.py` -- which
derives the transfer table from the parity assertions, exactly as designed --
faithfully emitted them. The sidecar then wrote this car's speed to
`RC+0x00BC`, its rev limit to `RC+0x1470`, and so on: the right offsets in the
wrong object. Retail's driver read the emulator's default vehicle instead,
returned inputs for a car that did not exist, and the transmission diverged a
few frames later. That is why `ai=retail` stood unwired.

Every one of the ten was also a **duplicate**. `B3VehicleFull` already models
all of them at the identical offsets:

| B3AiCar field | retail offset | already in B3VehicleFull as |
|---|---|---|
| `speed_ms` | v+0x00BC | `rb.vel[3]` |
| `car_at` | v+0x00C0 | `rb.dir` |
| `yaw_rate` | v+0x00D4 | `rb.omega[1]` |
| `veh_fwd` / `veh_right` | v+0x0204 rows 2 / 0 | `rb.frame[2]` / `rb.frame[0]` |
| `lsdm_limit_mph` | v+0x13AC | `lsdm_limit_13AC` |
| `change_up_rpm` | v+0x1470 | `trans.change_up_rpm` |
| `engine_omega` | v+0x149C | `trans.omega` |
| `gear` | v+0x14C8 | `trans.gear` |
| `drift_state` | v+0x1524 | `drift_state_1524` |
| `lsdm_active` | v+0x1550 | *(nothing -- added, as a byte)* |

### The fix

`B3AiCar` is a pure racecar view and holds a pointer to the vehicle, which is
what retail has at racecar+0x2440. The pointer deliberately sits OUTSIDE the
retail window: a host pointer is not retail's four bytes -- different width,
different address space -- so parking it at +0x2440 would be false parity and
the generator would transfer it. Same rule as `B3RigidBody.frame`.

The vehicle side then needs no table of its own. `B3_VEHICLE_RANGES`, the same
82 recovered ranges the physics path already uses, carries it. De-duplication
is what made the transfer expressible.

### `B3AiInputs` was an object retail does not have

The same audit killed the output struct outright. `FUN_00105340` writes its
results straight into the vehicle -- throttle at v+0x1400, brake +0x1404,
steer +0x1408, gear +0x14C8 -- which is precisely why the sidecar reads them
back from `VEH+off`. The port invented a struct to receive them, laid it out
at those vehicle offsets, and then put ONE racecar field in it (the +0x11EF
boost latch), so the transfer sent that byte to `VEH+0x11EF`: the same
wrong-object bug, one field wide. Every other member duplicated a
`B3VehicleFull` field at an identical offset.

The driver now writes the vehicle it is given. Three things fell out of that:

* **A comparison hack disappeared.** `validate_ai.py` had
  `mine_gear = got["gear"] if got["gear"] != 0 else float(cs["gear"])`, with
  the comment "gear_request is a delta in the C contract (0 = leave alone)".
  Retail's readback needed no such adjustment, which is the evidence: retail
  LEAVES v+0x14C8 alone and writes 1 or -1 only on the two swap branches. The
  zero-and-request convention existed solely because the invented struct was
  a fresh, memset buffer each call. The gear is now compared directly, and the
  suite still passes 189/189.

* **A field the struct could not hold turned out to be an invention.** The
  port set a `shift_kick` on gear swaps, annotated "v+0x14A4 / +0x14A0 = 0.35".
  `B3AiInputs` had no 0x14A0 member, so only half was ever written -- and
  nothing consumed either, so it was inert. Against the real vehicle it is not
  inert: v+0x14A4 is the transmission's in-shift latch, paired with the
  shift_timer at +0x14A0, and clearing it every frame cancels the mid-shift
  torque cut, letting the box start a new shift every frame. It measurably
  changed the game -- the autodrive car reached 108 mph where the baseline had
  84, which reads like an improvement and is not. Checking the evidence, the
  only documented writer of 0x14A4/0x14A0=0.35 is `FUN_0011BE50`'s **crashed**
  path at `0011BEB4` (RE_NOTES, "The crashed path is NOT the racing
  pipeline"; RE_SFX 329). Nothing puts it in `FUN_00105340`. It was removed,
  and the behaviour returned to the baseline exactly: max 160 mph, gears -1..6.

* **Two more single-byte type defects**, the same signature as the fourteen
  before: `lsdm_active` (v+0x1550) and `stop_flag` (v+0x1552) were `int` in
  the port and are seeded and read by the validator with `wb`/`rb`.

### The lesson

A duplicate model does not announce itself as a wrong value. It announces
itself as a field that cannot be shared, a validator that needs a fudge to
pass, and a feature that will not switch. When two structs claim the same
retail offset, one of them is wrong about which object it is looking at --
and when a struct has no home for a field its annotation names, the field
does not get written.

**A behaviour change that looks like an improvement deserves the same scrutiny
as one that looks like a regression.** The shift latch made the car faster.
It was a bug.

### `B3AiState`: the same defect, third instance

`B3AiState` is the AI object retail embeds at racecar+0x1A00 (the navigator
ctor `FUN_001705F0` writes the back-pointers at racecar+0x1A04/0x21A0/0x2160).
Eight of its members were annotated `v +0x...` and therefore sat at VEHICLE
offsets inside an AI-shaped struct -- so the AI object could not be
transferred at all, and `ai=retail` read a zeroed target speed at AI+0x9C4.
Three of the eight already existed in `B3VehicleFull` (`steer_1408`,
`drift_state_1524`, `authority_1534`); the other five were added at
v+0x156C..0x157C.

Splitting it surfaced a defect the suite caught immediately:

**`prev_steer` and the driver's `steer` output are the same address.** Retail
keeps one value at v+0x1408 -- this frame's steer, which is last frame's steer
when the driver next runs. The port modelled that single address twice, as
`B3AiState.prev_steer` (the slew limiter's memory) and `B3AiInputs.steer` (the
output). Independently they were consistent; collapsed onto the one field they
are not, and clearing "the output" at entry destroyed "the memory".
`validate_ai.py`'s "ooc mode 1 holds" case reads 0 where retail carries 0.3.
Retail does not clear v+0x1408 on entry, and neither does the port now.

That is the same shape as the `B3ScoreEvents` duplicate: one retail byte
range, two port fields, and no way to notice until they are forced to be the
same bytes.

### Two bugs that only a live switch could find

Shape parity got the bytes to the right place. Running the feature found two
things no differential case would have:

**The calling convention was missing.** `FUN_00105340` takes the vehicle in
**EDI** -- `validate_ai.py` has always called it as
`s.call(ea.F_DRIVER, regs={UC_X86_REG_EDI: VEH})` -- and the sidecar called it
with no registers set. The driver then ran against whatever EDI held: it wrote
nothing, and every car reported `thr 0.00 brk 0.00 str 0.00` and coasted to a
standstill. A signature is part of the interface; a struct at the right
offsets does not substitute for it.

*(An earlier draft of this section also blamed EDI for a 3.7x slowdown. That
was wrong. Frame-rate comparisons taken across rebuilds are dominated by
asset LOAD time -- this tree reads several GB at startup, so a cold-cache run
spends most of a 45 s window loading. The control settles it: `ai=re` produces
the same five status lines in the same window. Any cost figure quoted for a
retail backend has to come from a warm-cache run of fixed length, or from
counting sidecar calls directly.)*

**Moving state to its true owner exposed it to that owner's lifecycle.** The
driver's five timers belong to the vehicle (v+0x1534..0x157C) and used to sit
in `B3AiState`. `B3AiState` survives a respawn; the vehicle does not --
`b3_vehicle_full_init` memsets the whole object. Left to that memset the idle
value flipped from -1.0 to 0.0, which the driver reads as "a reverse burst is
running and has expired", so a stuck car went to NEUTRAL instead of into
reverse. It showed up only as a gear histogram: 3 samples in gear 0 where the
baseline had 12 in gear -1. The fix is `b3_ai_vehicle_state_init`, called from
both `b3_ai_state_init` and the re-init site.

The general point: **when a field moves to the object that really owns it, it
inherits that object's initialisation and reset behaviour.** Check the
lifecycle, not just the offset.

## `crash`: "no standalone calling convention" was not true

This feature was listed as blocked because `FUN_0011AEF0` "only runs inside the
physics substep loop". Both halves of that turned out to be answerable:

* **The convention was already known and already used.**
  `tools/emulate_crash_traj.py` calls it standalone at line 326:
  `sim.call(F_AEF0, regs={UC_X86_REG_ECX: VEHICLE})`. Thiscall, ECX = the
  vehicle. The same mistake as the AI driver's EDI -- the information was in
  the tree, in a validator that passes.

* **Running inside the substep loop is a property of the CALL SITE, not the
  function.** The port already calls it exactly where retail does -- at
  `0x0011C0B7`, between the tyre force pass and the suspension pre-pass,
  through `B3VehicleFull.chassis_resolve` -- and that position matters because
  everything the resolve produces is an accumulator write (+0xF0 force, +0x110
  impulse, +0x120 angular impulse, +0x130 deflection) that the integrator
  consumes and clears at the end of the SAME substep. Because the port kept
  the call site, the switch goes *inside the hook* and the position does not
  move. Nothing had to be restructured.

The interface is `B3CrashVehicle`'s 26 fields -- the port's **fourth** partial
view of the vehicle object, and, like the other three, every field in it is a
`B3VehicleFull` field at the same offset. So the retail path works off
`B3VehicleFull` directly and never builds the view. Two notes from that audit:

* `v+0x020E` (`asleep`) existed **only** in that view. It is on the vehicle now.
* `v+0x1404` is `ground_frac` in the view and `brake_1404` on the vehicle --
  one address, two names, and the bridge already wrote
  `cv.ground_frac = v->brake_1404`. `validate_ai` settles which is the game's:
  retail's driver puts the BRAKE there (`brk = rf(VEH+0x1404)`).

Measured: 51,480 resolves over 74 game-seconds, no NaN, top speed 158 mph
against the RE path's 160.

### The failure path is part of the feature

When the sidecar returned `err name 'UC_X86_REG_ECX' is not defined` (a real
bug -- a missing import), the bridge demoted `crash` to `re`, said so, and the
game finished the run with no NaN and a normal top speed. A switch that cannot
fall back safely is not a switch.

## `traffic`: the feature had more than one entry point

Listed as blocked because `FUN_001A6070` "needs the whole manager". True, and
still true -- but that is the population law, one entry point of several. Two
others are called standalone by `validate_traffic_mix.py` today:

    FUN_001A5E30   ECX = class          -> EAX = record pointer   (which model)
    FUN_001A5F90   ESI = record         -> EAX & 0xFF             (which paint)

Both are spawn-time choices, so the cost is per car rather than per frame, and
both now run on retail under `traffic=retail`. Measured: 1,370 picks over 154
game-seconds, no demote.

**The RNG has to be shared or the switch is meaningless.** Both functions draw
from the manager RNG at `0x00649B28`/`0x00649B2C`, and the port keeps the same
two words with the same seed (`0xFD462907`/`0x02B9D6F8`). The state travels
with the call and the advanced pair comes back, so there is one stream
whichever side draws from it. Without that, flipping the backend would fork the
sequence and every later spawn would differ for reasons unrelated to the
function being tested. The single-entry short-circuit matters for the same
reason: retail returns without a draw, so the switch sits *after* that test.

`FUN_001A6070` stays on the port, and `validate_traffic_mix.py` keeps checking
it as a model replay against constants read from the image (848 checks green).
The per-car traffic driver `FUN_00105150` is wired in `b3_ai_traffic_drive`
(thiscall, ECX = the vehicle, following v+0x1568 to the racecar for the target
at racecar+0x23C0) but this harness drives traffic cars along lanes and never
calls it, so that path is switchable and unexercised.

### I guessed a constant and it segfaulted the game

`CLASS_LIST_OFFSET` -- the six list triples inside the event TDESC -- I wrote
from memory as `{0: 0x60, 1: 0x78, ...}`. The real map was six lines away in
`validate_traffic_mix.py`: `{1: 0x54, 2: 0x60, 3: 0x78, 4: 0x84, 5: 0x6C,
0x0B: 0x90}` -- different offsets, no class 0, and it includes the towed
partner class 0x0B that the tractor path draws. With the wrong offsets retail
read a list that was not there, EAX came back outside the synthetic record
array, and `(EAX - records) / 0x18` produced an index that walked
`mix_entry_car[]` off the end. The crash landed in `traffic_pool_place`,
frames away from the reply that caused it.

Two guards now: the sidecar refuses a result that is not a record inside the
array it built, and the caller range-checks before the value becomes an index.
Both should have been there from the start -- a value crossing a process
boundary is untrusted input, even when the process on the other end is mine.

## `hud` and `sfx`: the boundary was already drawn, in the validators

Both were listed as needing "a capture-and-replay boundary -- a design
decision". The decision turned out to be made already, in tooling that has
been passing for months.

### `hud` -- FUN_0004D310

`tools/emulate_hud_ticker.py` runs the ticker for real with only the DRAWING
stubbed, and its `TickerTrace` exposes both halves of the interface:
`set_record()` writes a category record, `live_rows()` reads the row slots
back. Both are shared objects at identical offsets on both sides:

| | port | retail |
|---|---|---|
| in | `B3HudTickIn` | the score object's `B3CatRecord` -- value +0x00, clock +0x04, prev +0x08, open +0x10, tier +0x11, prev_tier +0x12, count +0x13, probed at score+0x374/0x390/0x418/0x598/0x5C4/0x564 |
| out | `B3TickRow` | the element's row slot, obj+0x570 + i*0x28 -- live +0x00, timer +0x08, y +0x10, tier +0x18, flash +0x1C, phase +0x20, pulse +0x24 |

What does NOT cross is the draw node. Retail builds a 2D node per row; this
harness draws its own row art. That is the LOOK the parity rules leave
relaxed, and the row STATE -- which rows are live, their tier, timer and
stacking order -- is the logic. Measured: 9,840 ticks over 164 game-seconds.

### `sfx` -- the emitters, not FUN_00141010

The feature was filed against `FUN_00141010`, and that one really is blocked:
it is the trigger the ticker fires, it walks into the Xbox's 3D DirectSound
voice manager, and `emulate_hud_ticker.py` stubs it for exactly that reason.

But the sound *law* is not in that function. It is in the 17 emitters, and
`tools/emulate_sfx.py` already runs each one for real and captures its call
into **PlaySound3D (0x001CD8D0)**. That call is the boundary: before it is the
game's decision -- which wave, what gain, what playback rate for this impulse
-- and after it is a console voice manager this harness replaces with its own
mixer, declared GLUE in `burnout3_sfx.c` since it was written.

So `sfx=retail` runs the emitter and takes the captured `{wave, gain, pitch}`.
The port keeps resolving which shipped file the wave base names, which is
asset plumbing rather than game logic, and keeps its own cooldowns --
`emulate_sfx.py` clears the emitter's cooldown bytes before every call, so
retail would always answer as if the event were fresh.

15 of the 41 events have an emitter with a captured calling convention;
`tools/gen_sfx_emitters.py` builds the table by matching the enum comments in
`burnout3_sfx.h` against `EMITTERS` in `emulate_sfx.py`, so neither source can
drift alone, and an event with no convention gets address 0 and stays on the
port. Measured: 105 fires over 217 game-seconds.

**The lesson for both:** "needs a design decision" was really "the interface is
not a struct, so I did not look for it". The boundary a validator already
draws to test a function is, in general, the boundary you can switch on.

## A type defect the parity assertions cannot catch: SIGNEDNESS

The recurring defect above -- a multi-byte field on a single-byte offset -- is
caught by a layout audit, because the offsets stop lining up. This one is not.

`B3HudTickIn.tier` (the HUD ticker's view of a score category record, rec+0x11)
was declared `unsigned char`. `B3CatRecord.tier` -- **the same byte of the same
retail object** -- is `signed char`, and retail stores **-1** there for "no
tier". Both structs had the identical offset, the identical width, and both
carried the comment "-1 = none"; one of them could not represent it.

The harness copies the record into the ticker view every frame, so -1 became
**255**. `tick_probe` promotes it to `int` and the row draws

    for (int i = 0; i < r->tier; i++)  /* 255 stars */

a line of star pips marching off the right edge of the screen, on categories
that had scored nothing at all. The player's own dump showed
`onc 103.4/t-1 ... nm 1` -- every tier -1, nothing earned -- while the HUD
showed NEAR MISS with a full row of stars. The scoring model was right the
whole time; only its display was lying.

**Why nothing caught it:**

* `_Static_assert(offsetof(...))` and `sizeof` are identical for
  `signed char` and `unsigned char`. The 476 parity assertions cannot see it.
* `validate_hud` passes 760/760: it feeds the ticker its own cases, and a
  case that never sets a negative tier never exercises the difference.
* The two structs are two views of one byte range -- the duplicate-model
  problem again -- so the disagreement lived in the gap between them.

**The check to run:** for every field a port struct shares with another view of
the same object, compare the DECLARED TYPES, not just the offsets. A sweep of
`unsigned` fields whose comment mentions -1 or "none" found this one and no
others (`slam_class` uses 0 for none, `grid_slot` is a positive index).

## `physics=retail`: the ownership split (was: drops the car through the floor)

**Status: the car drives on retail's physics now.** It was falling from the
first step; it now holds the road and reaches 116 mph median over a two-minute
run, with a residual intermittent fall-through (~7 events / 2 min) still open
at the bottom of this section. Four separate defects were in the way.

### 1. The transfer was a snapshot tool used as a co-simulation

Every recovered range was written into retail's vehicle EVERY frame and read
back. That is coherent only while both sides step from the same state -- and
under `physics=retail` the port does not step at all, so its copy goes stale
and gets pushed back. Bisected with `B3_STEPR_MAX=N`: N=0 held the car, N=1
dropped it, and span 0 is the whole rigid body, whose `inv_frame` and
`inv_inertia_world` retail REBUILDS each step and whose four accumulators it
CLEARS. The struct's own comments already said "(rebuilt)" and "(cleared)".

Replaced with an ownership model:

* `b3_emu_handover()` pushes `B3_VEHICLE_STATE_RANGES` ONCE when retail takes
  the wheel -- everything except what retail rebuilds or clears.
* `stepr` then carries INPUTS ONLY, and the port mirrors the result through
  `B3_VEHICLE_RANGES`, so it can take the wheel back on any frame.

### 2. The driver block must not be mirrored back

`b3_ai_drive` writes its inputs and its carried scratch into the vehicle
(v+0x1400..0x1414, v+0x156C..0x157C). Mirroring those back over the port wiped
the slew memory and the dither/stuck timers, the AI stopped commanding
throttle, and the car coasted to a standstill with `thr=0.00` reaching retail.
`DRIVER_OWNED` in gen_vehicle_ranges.py excludes them from the mirror; retail
receives the live inputs as step arguments and never needs them mirrored.

### 3. The emulator's soup buffer held THIRTY-ONE triangles

`SOUP_REC = 0x30028040` with `SOUP_TYPE = 0x30028800` left 0x7C0 bytes, and a
record is 0x40 -- room for 31 -- while the harness uploaded up to
`B3_EMU_SOUP_CAP = 120`. Every upload past 31 ran through the type table. The
capacity is now `SOUP_MAX = 256` with `SOUP_TYPE` derived from it, the region
enlarged to match, and both caps reference it instead of guessing.

### 4. The gather box saturated the cap

`{26, 14, 26}` returned more triangles than the cap on every upload (every
trace line read exactly "soup 120"), and the set was then truncated
arbitrarily -- so the ground under the wheels was frequently not among the
triangles sent. It now uses the box the port's OWN suspension gathers,
`{5.5, 34, 5.5}`, and re-uploads every 3 m instead of 12 so the car cannot
leave its own patch. A saturation warning fires if it ever hits the cap again.

### Ownership follows retail's own split

`FUN_0011BE50` branches on veh+0x210 and the crashed side runs a different
solver that never reaches the soup collision -- "the crashed path is NOT the
racing pipeline". The harness owns the wreck, so retail drives the RACING car
only and the port takes the wheel back for a crash, with a fresh handover when
racing resumes. `b3_emu_drop_car()` also releases the slot whenever the
harness re-places a vehicle, so a respawn re-hands rather than letting the two
integrate independently.

### Residual, open

~7 intermittent fall-throughs per two minutes remain, at varied places with
ground present in the uploaded set (checked: 8 upward-facing triangles
directly beneath the car at one). Widening the gather makes it WORSE even with
capacity to spare, which points at the unfiltered set: the port runs
`b3_collision_filter_walls` before building its chassis soup and the emulator
path sends raw gathered polys. That filter is the next thing to try.

`physics` still defaults to `re`. The other nine backends are unaffected by
any of this.

## (original diagnosis)

Reported from play ("playing all retail now; I fall through the floor") and
reproduced headlessly: with `physics=retail` the player free-falls from the
first step -- `vel.y` -0.5, -1.0, -1.4 ... (pure gravity), through the road and
on down. Every other backend is fine: with the other NINE on retail and
physics on `re` the car sits at y=149.3 on a road at y=149.2, drives to 71 mph,
no NaN, no demote.

**It is not the geometry.** The soup upload was checked directly: 120
triangles, road normals (0,1,0), the winding agrees with the supplied normal on
all 120, and the z sign matches the emulator's space (soup z=+2073, car seeded
at z=+2090). The GL->GAME conversion -- negate z on the vertices and the
normal, swap v1/v2 -- is correct as written.

**It is not the emulated pipeline.** Driven in isolation on
`_ground_plane(0.0)` the same `RelocPipeline` RESTS: body origin settles at
y=0.31, vertical velocity decays to ~0, the wheels take compression. Retail's
physics, its suspension and the soup all work.

**It is the per-frame RANGE TRANSFER.** `B3_STEPR_MAX=N` applies only the first
N merged spans:

| N | result |
|---|---|
| 0 | the car HOLDS (vel.y -0.2) -- retail owns its own state |
| 1 | free fall (vel.y -14.8) |

Span 0 is the whole rigid body -- `inv_inertia_body`, `inv_inertia_world`,
`inv_frame` and then vel/dir/omega, all adjacent so they merge. Writing the
port's copies of retail's DERIVED matrices over retail's own is what breaks it.
Dropping the derived matrices and the accumulators and sending only
vel/dir/omega/angmom still falls, because the harness keeps writing `rb->vel`
after the step (the AI governor, the pose extraction) and its own soup is
EMPTY -- the dump shows `soup 0`, since `soup_freeze` only runs inside the
port's step, which `physics=retail` skips. So the port hands retail a body that
believes it is in mid-air.

**And sending nothing is not the answer either.** At N=0 retail holds the car
but idles in neutral -- gear 0, 1 mph -- because the gear and input state never
cross.

**What it actually needs:** an ownership split that does not exist yet. Every
recovered field is currently transferred BOTH ways every frame, which is only
coherent while the port is also stepping -- and under `physics=retail` it is
not. The three classes have to be separated:

* **inputs** the port owns and pushes (throttle/brake/steer/gear request),
* **state** retail owns and the port reads back (the body, the wheels),
* **derived** that must never cross (`inv_frame`, `inv_inertia_world`, and the
  per-substep accumulators retail clears itself).

Until then `physics=retail` executes retail's code faithfully -- 1,200 frames
at 16.45 ms -- but the car is not drivable, and the earlier "live" claim in the
table above was too generous. The other nine backends are unaffected.

## `ai=retail` + `physics=retail`: one session, not two

Each was fine alone (physics median 116 mph, ai median 47) and together the car
would not accelerate at all (median 0). The cause was structural.

**Two sessions, one vehicle.** The driver ran in `ea.Session()` and the physics
in `RelocPipeline` -- two Unicorn instances, each with its own copy of the
vehicle -- and the port shuttled the driver's fields between them every frame.
Retail has no such split: `FUN_00105340` writes v+0x1400 and `FUN_0011BE50`
reads it in the SAME object. With both backends retail the AI was fed from the
physics MIRROR, one frame old and covering 23% of the object, and its view went
incoherent -- the probe caught **1026 rpm at 21 m/s in gear 2**, which is not a
state any car is in. Throttle then reached the step in **72 of 1025 frames
(7%)**, against 454 of 1105 (41%) with the port's own AI.

**The fix** is `stepra`: when both are retail the driver runs inside the
PIPELINE session and its output is the input the step consumes. No shuttle, no
contention, and `b3_ai_drive` skips the separate AI session entirely so the car
is not driven twice.

Measured after: throttle into the step **1160 of 1426 (81%)**, the car on the
road (y=149.0, vel.y~0) accelerating to ~50 mph under driver control, and the
port's mirror agreeing with the physics session frame for frame.

**A semantic conflict this surfaced.** `racecar+0x1920` is seeded 0 by
emulate_pipeline ("mode 0: normal") and carried as 1 by `B3AiCar.race_mode`
("1 = normal racing"). Both were validated -- separately, in different
sessions, with different values. Running them in ONE session forces the
question, and it is not yet answered; `B3_STEPRA_KEEP` (default `0x1920`) holds
the physics seeding and is the knob for testing the other reading.

**Still open.** The car accelerates to ~50 mph and then stops permanently
around t=10 s, with no crash logged. That is a different failure from the one
fixed here and has not been diagnosed.

