# PROPS — the destructible track props (cones, barrier boards, bulb posts)

> **This is the props evidence record**, and the counterpart of the other
> `docs/RE_*.md` files. It was written under the name `INTEGRATION_NOTE.md`,
> which is what a few source comments and validators still call it by; it is
> published here as `RE_PROPS.md`, which is what it is. Section 10 carries the
> live open items.

> User request, with `REFERENCE IMAGES/xemu-2026-08-12-13-53-46.png` (an orange
> cone mid-tumble beside the car at the dirt-shortcut barrier): *the
> destructible track props should be there as in retail, and react the same way
> when you hit them.*

Two agents worked this wave. **PROPS-1** (host process killed mid-run, transcript
lost) found the prop tables in `static.dat`, transcribed the world-object
registration loop, wrote the first `tools/extract_props.py`, the whole of
`src/burnout3_props.c/.h` (loader, knock integrator, renderer, the car sweep)
and a worktree testbed in `burnout3_full.c`. **PROPS-2** (this pass) re-derived
every claim from `build/burnout3.elf` with the Ghidra bridge down (a capstone
sweep, `scratchpad/props/b3dis.py`), corrected the cited addresses, settled the
class-vs-model-index question from the data, **found and fixed the index-base
bug that was drawing every model past the first as a heap of shards**, wired the
recovered object-crash verdict, rebased onto `be1e68a`, and did the
verification. Attribution is marked per section.

Addresses are in the corrected map (`build/burnout3.elf`; `.text` = old flat
address + `0x10000`). `[C]` = read out of the image, `[S]` = strongly supported,
`[?]` = open, **GLUE** = invented and marked as such.

---

## 1. Where the props live in the data [C] — PROPS-1, re-verified PROPS-2

Every shipped `static.dat` carries a **second** 0x70-record model table, next to
the backdrop-LOD table `tools/extract_track.py` documents at +0x34/+0x38:

| header | meaning |
|---|---|
| `+0x36` u16 | prop MODEL count |
| `+0x3C` i32 | prop MODEL table (0x70 per record) |
| `+0x40` u16 | prop INSTANCE count |
| `+0x44` i32 | `u8[model_count]` — the per-model **prop class** |
| `+0x48` i32 | instance TRANSFORMS, 0x40 bytes each (one 4x4) |
| `+0x4C` i32 | `ptr[unit]` → `u8[model]` per-streamed-unit instance counts |
| `+0x50` i32 | `ptr[unit]` → `ptr[model]` per-(unit,model) instance lists |
| `+0x54` u16 | streamed-unit count (shared with the world loader) |

Neither the 0x40 transform stride nor the length of the +0x44 array is assumed:
`(hdr+0x4C − hdr+0x48) / count` is exactly 64.000 on all 37 files, and
`hdr+0x44 + model_count` rounded up to 16 lands exactly on `hdr+0x48` on all 37.

The reader is the world-object registration loop
`FUN_00110420 @0x001109CB..0x00110A46`, transcribed instruction by instruction:

```
001109cb  MOV  ECX,[0x737688]        ; the +0x4C counts table
001109d5  MOV  AL,[0x737686]         ; model count, the loop bound
001109e8  MOV  EDX,[ECX + unit*4]    ; counts[unit]
001109eb  MOV  AL,[EDI + EDX]        ; counts[unit][model]         EDI = model
001109f4  MOV  EAX,[0x73768c]        ; the +0x50 lists table
001109fd  MOV  EDX,[EAX + unit*4]    ; lists[unit]
00110a00  MOV  EAX,[EDX + EDI*4]     ; lists[unit][model]
00110a03  LEA  ECX,[EAX + ESI*4]     ; 4 BYTES PER LIST ENTRY
00110a0c  LEA  EDX,[EAX+EAX*2]; SHL EDX,4   ; world slot stride 0x30
00110a19  MOV  byte [slot],0x5       ; world-slot TYPE 5 = STATIC PROP
00110a1c  MOVZX EDX,word [ECX]       ; entry+0x00 u16 = instance index
00110a1f  SHL  EDX,0x6               ;   * 0x40
00110a22  ADD  EDX,[0x737678]        ;   + the +0x48 transform table
00110a28  MOV  [slot+0x04],EDX       ; slot -> the LIVE transform
00110a2b  MOVZX ECX,byte [ECX+0x2]   ; entry+0x02 u8
00110a35  IMUL ECX,ECX,0x70          ; (see below)
00110a38  ADD  ECX,[hdr + 0x3C]
00110a3f  MOV  [slot+0x08],ECX
00110a46  CALL FUN_00114270          ; world AABB from model bbox x transform
```

**The list-entry byte at +0x02 is a CLASS, not a model index** [C, PROPS-2].
PROPS-1 asserted it from the byte being 0..7 on every track; that argument is
weak on its own (a 32-model track could still use only 8). The decisive test is
`AS_C2_V1`: 3 models, `+0x44 = [1, 5, 2]`, and the per-unit lists carry exactly
those three values. Dumping the +0x3C table past the third record shows records
3, 4, 5 are **garbage** (`mat=50629`, a bbox extent of 3e16), so byte 5 cannot
be a model index — it would index off the end of a shipped file. The model an
instance draws is the list it lives in (`EDI`), which is what the extractor
emits. Retail nevertheless scales the class by 0x70 into the model table at
`0x00110A35`; on tracks whose classes exceed the model count that slot+0x08
pointer is out of range. Recorded, not reproduced. **[?]**

Prop classes by content, stable across all 37 files:

| class | what it is | instances (all tracks) |
|---|---|---|
| 1 | **the cone family** — every track's cone, plus life rings | 4838 |
| 2 | benches, bins, litter | 1445 |
| 3 | roadwork lights, low bins | 978 |
| 4 | roadwork barrier boards, road-closed signs | 2141 |
| 5 | market/temporary barriers | 1073 |
| 6 | tall signposts, warning markers | 2056 |
| 7 | heavy blocks, tables, trunks | 824 |
| 0 | 4 stray instances on EU_M1 | 4 |

## 2. The model record and its meshes [C] — PROPS-1

0x70 bytes, relocated by `FUN_0019B4E0`: bbox MAX at +0x00, bbox MIN at +0x10,
three 0x14-byte mesh blocks at +0x20/+0x34/+0x48 (LOD1/LOD2 present per the
`+0x62` flags), material indices at +0x5C..+0x60, LOD near/far floats at
+0x64/+0x68. The vertex stride comes from the material's shader class, the two
declarations `extract_track.py` already pins as the foliage/prop/cone families:
class 8 = `pos + uv` (20 B), class 9 = `pos + NORMPACKED3 + uv` (24 B). Over all
436 prop models in all 37 tracks `(index_ptr − vertex_ptr)` is an exact multiple
of the class-derived stride **and** the vertex count exceeds the largest index,
which neither stride achieves alone. Indices are one u16 triangle strip stitched
with duplicated indices; the extractor de-strips to a triangle list.

**The bug that made this wave's first capture useless** [PROPS-2]: `build()`
wrote the concatenated index blob with **global** vertex indices while the loader
added the model's `first_vertex` on top, so every model after the first drew
another model's vertices. A `GL_DangBarr` came out as a pile of shards and a
cone as a flat yellow sheet. `props.bin` now stores **model-local** indices (the
format note says so) and the loader is unchanged. `scratchpad/props/props_zoom.png`
is the before, `props_zoom2.png` the after — striped trestle barriers and orange
cones.

## 3. Prop mass and radius [C] — PROPS-1, addresses corrected PROPS-2

`FUN_0011A020` is the prop rigid-body constructor. With `EBX` = the model record:

```
0011a0a5  body+0x1D0 = bbox MAX          0011a0af  body+0x1E0 = bbox MIN
0011a0de  SQRTSS -> body+0x1CC           radius = |bbox max|
0011a137  XMM0 = [0x3A2928] = 100.0
0011a13f  XMM1 = (max.x − min.x)          (body+0x1D0 − body+0x1E0)
0011a155  XMM0 = (max.z − min.z)          (body+0x1D8 − body+0x1E8)
0011a169  * [0x3A292C] = 200.0
0011a17d  MAXSS with 100.0
0011a191  -> body+0x1F0                   THE MASS
0011a19e  body+0x224 = DAT_0060EA20 + [0x3A7F34] (= 10.0)   the despawn clock
0011a062  body+0x204 = THE INSTANCE MATRIX ITSELF
0011a032..0011a05d save / 0011a06d..0011a094 restore  m[3] m[7] m[11] m[15]
```

So a prop's mass is its **footprint area × 200, floored at 100**, and the four
`w` slots of the instance matrix are not transform at all — they are the baked
half-range instance colour (~0.41, 0.40, 0.37 on the reference cluster), which
is why they survive the body reset. Both are emitted per model / per instance.
US_C3_V1: a `WF_Cone` floors at 100 kg, a `GL_Cone` is 156, a `GL_DangBarr`
(3.96 × 0.90 footprint) is 710.

## 4. What retail does when you hit one [C] — PROPS-1, addresses corrected PROPS-2

`FUN_00111CD0`, the collision-pair dispatcher, has four arms:

| arm | test | goes to |
|---|---|---|
| bail `@0x00111D0B` | either handle type == **8** | nothing |
| 1 `@0x00111D14..0x00111D8B` | one handle type **3**, other in {0,1,2,4,6,7} | `FUN_00112E70` (A in {0,1,2,4}) else `FUN_001135E0` |
| 2 `@0x00111D90` | both in {0,1,2} | car-vs-car |
| 3 `@0x00111DEB..0x00111E1B` | `FUN_0010FB20(other)` **and** other handle type **5** | **`FUN_00113890`** — the prop |
| 4 `@0x00111E22..0x00111E4E` | `FUN_0010FB20` both | `FUN_00113960`, the generic solver |

`FUN_00113890` then:

```
00113901  CALL FUN_001084E0   the "may I knock it" gate -- purely GEOMETRIC
                              (section 11 reads the whole of it: a 15-axis
                              SAT between two ORIENTED BOXES; no mass, size
                              or velocity veto exists)
0011392e  CALL FUN_00197A20   prop-hit score/audio, and only when the other
                              handle's type is 0/1/2, i.e. a car
0011393b  CALL FUN_00114730   PROMOTE
0011394e  CALL FUN_00113960   resolve with the generic rigid solver
                              (guarded by `type < 8` @0x00113947)
```

and `FUN_00114730` hands the prop one of exactly **16** rigid bodies: allocation
bitmask at `gameworld+0xE9CA0/+0xE9CA4` (`@0x00114737`, `@0x00114750`), 16-slot
scan `@0x0011476D`, pool base `gameworld+0xC4380` `@0x001147E4`, stride `0x780`
`@0x001147BD`, and the handle's type byte becomes **6** `@0x0011478B`. When the
pool is full the body with the smallest `+0x224` clock is recycled and **the
recycled prop's world slot is retyped to 8** `@0x0011480C` — which the
dispatcher drops outright. A prop that has lost its body stops colliding; that
is exactly the module's `B3P_SETTLED` state [S].

## 5. Why a cone can never wreck you, and the object-crash interface [C] — PROPS-2

This is the section the crash wave asked for.

*Retail's answer.* `FUN_00112E70` is reachable **only** through dispatcher arm 1,
which requires a handle of **type 3**. A static prop is type 5 and a knocked one
is type 6, so a car-vs-prop pair takes arm 3 (knock) or, once promoted, arm 4
(generic solver; `FUN_0010FB20` accepts 6) — **it never reaches the crash
trigger**. The type→class map says the same from the other side: `FUN_0010FBC0`'s
jump table `@0x0010FC04` sends types 5, 6 and 7 to the class-6 arm `@0x0010FBFC`,
and `DAT_0039AE50` row 6, read out of the image, is all zeros:

```
row 0 [1 1 1 1 1 1 0]   B is a racecar
row 2 [1 0 0 1 0 0 0]   B is a type-3 OBJECT  -> crashes class 0 and 3
row 6 [0 0 0 0 0 0 0]   B is a prop (type 5/6/7)
```

*What the harness does.* `b3_props_collide_car()` fills `B3PropHit` with the
recovered `obj_class` **and** the kinematics `FUN_00112E70` wants — `vrel` is the
**prop's** point velocity minus the **car's** (`@0x00113311`), the same quantity
retail takes the normal component of `@0x00113331`. The `game_update` hunk hands
every contact to `b3_td_object_contact()` with

```
obj_class = b3_props_object_class(inst)   /* 6 for every shipped prop  [C] */
car_class = b3_td_object_class(0, 0)      /* racecar -> 0              [C] */
car_mass  = pv->fsim.mass                 /* the CAR's mass, per RE_TD_RULES */
immune    = g_race_time < pv->immune_until
vrel, n   in GAME space (z negated, RE_NOTES 12)
```

so the **retail crashability table returns the verdict**, not a harness rule:
`b3_td_object_contact` refuses at its `b3_td_object_crashable()` line and never
touches the per-car accumulator, so props cannot pre-empt a real wall or object
crash either. Over a full autodrive lap with 88 logged prop contacts (up to
158 mph closing) the object trigger fired **zero** times.

*The escape hatch, GLUE, default OFF.* `B3_PROP_CRASH_KG=<kg>` makes props at or
above that recovered mass present class **2** (a type-3 prop ENTITY), the row
that does crash a racecar. Retail ships no such promotion for `static.dat`
props, and the failure mode is exactly the one `be1e68a` had just fixed for the
soup mapping (a 75 mph *normal-component* bar with no head-on gate wrecks you out
of nowhere), so it stays off until something measures it. When a real type-3
entity family is recovered, it plugs in here: give it `obj_class = 2` and the
rest of this path is already the retail one.

## 6. `props.bin` [PROPS-1, index semantics corrected PROPS-2]

`tools/extract_props.py` (`--all`, or one track id; `B3_TRACK` honoured) writes
`build/tracks/<ID>/props.bin`. Header `'B3PP'`, version 1, then model records
(0x60: bbox, first_vertex/count, first_index/count, class, mass, radius, LOD
near/far, material flags, 32-char texture basename), instance records (0x50: the
4x4, model, class, unit, flags), vertices (0x20: pos, normal, uv) and u16
indices. Positions are RAW GAME SPACE — the loader does the Z reflection, as
`extract_track.py` and `burnout3_track_paths.h` do. **Index values are
model-local** (add `first_vertex`). Nothing per-track is compiled in anywhere.

```
37 tracks, 13359 prop instances, 436 models, no structural NOTEs
biggest EU_M2_V1 741   smallest US_C5_V1 0 (that track has no prop data at all)
US_C3_V1 (the reference track) 7 models, 123 instances: 53 WF_Cone, 18 GL_Cone,
   4 Chgo_LifeRing (75 class-1), 24 Chgo_RdWorkLight, 18 GL_DangBarr,
   5 GL_RdClosed, 1 Chgo_LitterBin
```

## 7. The knock law — GLUE, marked [PROPS-1, capped by PROPS-2]

> **SUPERSEDED — this whole section is history.** `FUN_00113960` was ported on
> 2026-08-22 (section 10) and every constant listed below has since been
> replaced by a recovered one: restitution is `DAT_004A1D98` = **0.0**, the
> drag pair is −1.0 on speed² and −2.0 on |omega|, gravity is (0, −20, 0),
> world restitution is 0.1 (`[0x003A69C4]`), the normal bend is −0.9
> (`[0x0041A4C0]`), and `B3P_KNOCK_MAX_MS` no longer exists — the launch is not
> capped because it never needed to be (section 12.1). The live block is the
> one at the top of `src/burnout3_props.c`. Kept only for the reasoning trail.
>
> *On the restitution being 0.0:* `DAT_004A1D98` lives in **.bss** and so reads
> zero in the ELF whether or not anything writes it — the emulator oracle trap.
> It was checked rather than assumed: the literal address occurs **exactly once
> in the whole image**, the read at 0x00113F4E, and there is no writer. Its
> .bss neighbours *are* written by absolute stores (`movss [0x4a1ef0]` at
> 0x0013EA62/0x0013F051/0x001CD633), so the block is directly addressed and not
> filled by a registrar walking a base pointer. The runtime value is 0.0.

`FUN_00113960`, the generic solver, is not ported (it is the car-vs-car impulse
chain), so the reaction is a ballistic tumble. Everything recovered that feeds it
is `[C]`: the 16-body pool, the 10 s body life, the mass and radius laws, the
"body taken away → stops colliding" rule. Everything else is GLUE and is named
in one block at the top of `burnout3_props.c`: restitution 0.35, ground bounce
0.30 / friction 0.70, spin gain 2.2 capped at 22 rad/s, air drag 0.25/s, lift
0.45, rest threshold 0.35 m/s, and **`B3P_KNOCK_MAX_MS = 30`** — the launch cap
PROPS-2 added: the impulse is mass-cancelling ((1+e)·vn, which is the limit of
the retail solver as m_car ≫ m_prop), so a 150 mph clip flung a cone at 90 m/s
and it left the frame in three frames. The user's reference frame is a cone
*mid-tumble beside the car*, so the launch speed is capped. Nothing about
*whether* a prop is knocked depends on it.

## 8. The harness patch [PROPS-2, from PROPS-1's testbed]

`scratchpad/props/apply_props_patch.py` — exact-match, idempotent, refuses to
write anything if any anchor is missing or ambiguous, and re-runs as a no-op.
Six hunks, all additive, anchored against `be1e68a`:

| hunk | anchor | what |
|---|---|---|
| include | `#include "burnout3_ai.h"` | `burnout3_props.h` |
| draw | `trackmesh_draw_scroll` / `trackmesh_fog_end` | `b3_props_draw()` inside the world's fog bracket |
| restart | `b3_tdfx_event_reset(); … g_state = RACING;` | `b3_props_reset()` |
| update | `traffic_update(g_delta_time);` | integrate + collide every racer + the object-trigger call |
| init | `traffic_init(); … b3_hud_init(…)` | `b3_props_load("build/tracks/<B3_TRACK>")` |
| Makefile | `src/burnout3_panels.c` | `src/burnout3_props.c` |

`B3_PROP_TRACE=1` logs every contact (`car, instance, model, prop class, object
class, mass, closing mph, impulse`).

## 9. Verification — headless only (`SDL_VIDEODRIVER=offscreen`, one instance)

Captures in `scratchpad/props/`:

| file | what it shows |
|---|---|
| `props_off.png` | the reference barrier with props disabled — nothing there |
| `props_before.png` | the same view with props: barrier boards, ROAD CLOSED signs, cones |
| `props_zoom2.png` | close-up: striped trestle barriers + orange cones (`props_zoom.png` is the same shot before the index fix) |
| `top_before.png` | top-down, the barrier line across the dirt shortcut, untouched |
| `top_1205.png`, `lap_top_1150.png` | mid-pass: the pack through the line, props tumbling |
| `lap_top_1260.png` | after: the middle of the line and the right-hand cluster swept away |
| `props_after.png`, `side_1206.png` | the player at 152 mph through the barrier line, HUD reading NEAR MISS / DRIFT — no crash |

Runtime, autodrive, deterministic `B3_FIXED_DT`, one instance at a time:

| run | props | wall clock |
|---|---|---|
| 6000 frames, US_C3_V1 (123 instances) | on | 30.04 s |
| 6000 frames, no props | off | 29.14 s (**+3.1 %**) |
| 3000 frames, EU_M2_V1 props (741 instances, the biggest table) | on | 15.72 s |
| 3000 frames, no props | off | 15.22 s (**+3.3 %**) |

A **full lap completes** (`Lap 1/3 completed!`, the run then ends on the race
clock — "Time's up", not a stall): **88 prop contacts** logged, 35 of them
class-1 cones and 52 class-4 barrier boards, closing speeds up to **118.3 mph**,
and **0 object crashes** — no prop wrecked any car, which is the retail law of
section 5.

Suites, all green on this tree:

```
tools/validate_port.py        95/95
tools/validate_carcol.py     752/752
tools/validate_td_rules.py   532/532
```

## 10. Open items

* **[?]** `0x00110A35`'s `IMUL class,0x70` into the model table — retail's
  slot+0x08 model pointer for a type-5 prop is derived from the CLASS, which on
  several tracks points past the end of the +0x3C table. The harness uses the
  list's model, which is unambiguous. Whether retail has a latent bug here or
  the pointer is only ever used for something that tolerates it (the
  `FUN_00114270` AABB) is unresolved.
* **[?]** Prop-hit **audio/score** (`FUN_00197A20 @0x0011392E`) is not ported —
  retail plays a per-class prop-hit sample and scores it. That is an SFX-wave
  item; the contact reports already carry everything it needs.
* **[?]** LOD1/LOD2 meshes and the `+0x64/+0x68` fade distances are extracted
  but unused; every prop draws LOD0 at any range.
* ~~**[?]** The generic solver `FUN_00113960` is still unported, so the knock is
  GLUE (section 7) and the car takes no reaction force from a prop at all.~~
  **CLOSED (2026-08-22):** ported — `src/burnout3_props.c` runs `FUN_00113960`'s
  arm for (car A, prop B) at a resolved contact, with the recovered normal bend
  (`@0x00113F16`) and restitution (`@0x00113F5C`). The **takedown-rules** side
  of the object trigger, `FUN_00112E70`, is ported too — `b3_td_object_contact`
  in `src/burnout3_td_rules.c`, fed by every prop contact.
* ~~**[?]** Props under-contacting (the same shape as wreck defects 2 + 3) —
  the old ledger item, never closed.~~ **CLOSED (2026-08-27):** it was the
  contact VOLUME, not the solver. See section 11.

---

## 11. The knock GATE — `FUN_001084E0` read in full [C], 2026-08-27

Two user reports, one root cause: *"hitting signs should cause them to fly off"*
and *"the hit detection / reaction of the orange traffic cones is not correct"*.

**What the harness was doing.** The prop's collision volume was a **sphere** at
the model's mid height with radius `max(halfX, halfZ)`. On shipped data that is
wrong in three different directions at once:

| model (US_C1_V1) | class | real bbox (m) | the sphere it got |
|---|---|---|---|
| `WF_sign_prop` | 6 | 1.12 × **6.14** × 0.42 | r **0.56** centred **3.07 m above the road** |
| `WF_Cone` | 1 | 0.59 × 1.04 × 0.59 | r 0.30 spanning 0.22 .. 0.82 m |
| `WF_prop_mktbarrier` | 5 | **3.43** × 1.48 × **0.44** | r **1.72** around a 0.44 m board |

A car box is about 1.2 m tall, so the signpost's sphere was **out of reach on
every frame of every lap** — class 6 is 2056 instances across the 37 tracks and
not one of them could ever be hit. That is the sign report, exactly.

The second half of the cone report was the **pre-promotion gate**. It required
the car's velocity component *along the contact normal* to exceed 0.05 m/s —
but once the car box had swallowed a cone the sphere test reported distance 0
and fell back to a hard-coded **lateral** normal, so a car driving straight
through a cone scored a normal component of ~0 and the cone was refused a body
on every frame of the pass. A dead-centre cone was never knocked; only a
glancing one was.

**What retail does.** `FUN_001084E0`, re-read out of `build/burnout3.elf` with a
capstone sweep over the PT_LOAD segments (the Ghidra bridge was refusing
connections), is a **fifteen-axis separating-axis test between two ORIENTED
BOXES**. There is no sphere in it.

```
001084EF  box A: EDX -> {bbox MAX +0x00, bbox MIN +0x10}, EAX -> its 4x4
0010851B  centre = (MAX + MIN) * [0x003B1684] (= 0.5)
00108541  world centre = row3 + row0*c.x + row1*c.y + row2*c.z
001085B4  half extents = (MAX - MIN) * 0.5
001085CE  the same two for box B (ECX -> its bbox pair)
0010869C..00108AFA   FIFTEEN x FUN_00107FD0, each TEST AL,AL / JNE 0x00108DEE:
          A's three rows, B's three rows, and the nine edge crosses that
          FUN_000328F0 builds.  ANY separating axis -> no contact.
00107FD0  FUN_00107FD0: AL=1 @0x00108076 when a.lo > b.hi @0x0010800F or
          b.lo > a.hi @0x00108020, else writes min(hi) - max(lo) @0x0010805D
00107E9A  FUN_00107E90: interval [c-r, c+r], c = axis . centre,
          r = sum_k |axis . row_k| * half_k.  The axes are UNNORMALISED --
          prop instance matrices carry scale, row lengths 1.000..1.516 on
          US_C1_V1 -- which is why
00108B4E  every axis's overlap is divided by |axis| (FUN_0002C0D0) and the
          MINIMUM normalised overlap wins (seed [0x003B172C] = FLT_MAX,
          degenerate crosses skipped by FUN_0003B060 @0x00108B32)
00108BB7  the winning axis is scaled by [0x003B16C0] (= -1) when
          dot(axis, centreB - centreA) > [0x003B16E0] (= 0), so the normal
          always points from B toward A
00108BE3  contact point = FUN_00108080's support point of the opposing box
          along -normal (its per-axis eps is [0x003B16D0] = 0.001, half
          extents at +0x60/+0x64/+0x68), pulled back by the penetration
          @0x00108C39.  Out: depth [ebp+8], point [ebp+0xC], normal
          [ebp+0x10], AL = 1.
```

`FUN_000116A0` is a bare 16-byte **copy**, not a normalise (@0x000116A4), so
retail hands the caller the winning axis unnormalised while the depth beside it
IS normalised — its own point pull-back is therefore off by `|axis|`. The port
normalises the normal and keeps retail's depth; `FUN_00113960` re-normalises at
its bend `@0x00113F49` anyway. **Marked as a deviation** at `b3p_obb_contact()`.

**The port.** `b3p_obb_project` / `b3p_axis_separates` / `b3p_obb_support` /
`b3p_obb_contact` in `src/burnout3_props.c`, one per recovered function. The
prop's box is the model bbox under the live instance matrix — the same two rows
`FUN_0011A020` copies onto the body at +0x1D0/+0x1E0. The pre-promotion guard
survives but is now the **speed** of the car at the contact point, not its
component along the normal; it exists only so a prop a *parked* car rests
against is not re-promoted every frame, and retail ships no gate there at all
(`B3P_KNOCK_MIN_SPEED`, marked GLUE).

**Measured.** `B3_PROP_AUDIT=1` sweeps the car's box from its previous frame to
its current one — blending the frame axes, 0.25 m per sample — and asks the
recovered SAT at each sample. That is an *independent* geometric truth: it does
not reuse the live gate's verdict. A prop it marks swept that the live path
never contacted is a MISS. One scripted 90 s cone-line drive on US_C1_V1
(`B3_SCENARIO=props:8`, `B3_SCENARIO_PROPCLASS=1`), the same drive on both
volumes:

| | sphere (before) | recovered OBB (after) |
|---|---|---|
| props the car's box swept | 101 | 101 |
| contacts admitted | 78 | **101** |
| **missed** | **23 (23 %)** | **0** |
| cones | 72 / 89 (81 %) | **89 / 89** |
| signposts | **0 / 5 (0 %)** | **5 / 5** |

and on a free-running autodrive lap, 75 swept / 45 admitted / 30 missed becomes
75 / 75 / 0. Every knocked prop leaves its transform, 97 % tumble past 60°, and
87 % come to rest (frozen by the world resolve) or are retired by the 16-body
pool.

`tools/validate_prop_contact.py` is the suite, 16 checks: the cone-line drive,
the signpost hit, determinism across two identical runs, and a free-running
autodrive leg. `B3_SCENARIO=props[:T]` with `B3_SCENARIO_PROPCLASS` is the
placement that guarantees it samples.

Evidence: `build/props/sign_strip.png` (the same signpost, same frames, sphere
vs box — upright and untouched, then knocked) and `build/props/sign_flight.png`
(eight consecutive frames of the post tumbling away).

### 11a. WHICH signs a track actually owns [C]

The user's screenshot showed the **yellow chevron corner boards** on
`US_C1_V1`. Those are **not props and not scenery** — they are triangles in the
world mesh under the materials `WF_roadCHEV` (42 `usemtl` groups) and
`WF_road_signs` (the black-on-yellow atlas: chevron arrow, STOP, SPEED LIMIT
10/20/40/55, BUMP, bend arrows; 9 groups). `US_C3_V1` carries the same atlas as
`GL_road_signs`, 31 groups. Nothing on disc links those triangles to a
destructible record, so **retail cannot knock them either**: `scenery.bin`'s
instance record (`tools/cextract/cx_scenery.c`, read back at
`src/burnout3_scenery.c`) is `f32[16]` transform + `model` + `unit` + `record` +
a reserved word, with **no class byte, no mass and no destructible flag**, and
the scenery model record has no `prop_class` either. Structurally so: props come
from `static.dat` `hdr+0x36/+0x3C`, which has the companion per-model class
array at `hdr+0x44`; scenery comes from `hdr+0x34/+0x38`, a bare count+table
pair with no class side-table. Retail's world-object registrar `FUN_00110420`
walks the `+0x3C` table only.

The signs a track *does* own as knockable props:

| track | model | class | mass | instances |
|---|---|---|---|---|
| US_C1_V1 | `WF_sign_prop` (NO PARKING / TOW ZONE / SPEED LIMIT 40 post) | 6 | 100 kg | 15 |
| US_C1_V1 | `WF_Roadworks1` | 4 | 191 kg | 12 |
| US_C3_V1 | `GL_RdClosed` (ROAD CLOSED AHEAD board) | 4 | 261 kg | 5 |
| US_C3_V1 | `GL_DangBarr` | 4 | 710 kg | 18 |

Those are the ones section 11 made hittable. `US_C1_V1`'s props table is 455
instances: 375 class-1 cones, 48 class-5 market barriers, 15 class-6 signposts,
12 class-4 boards, 3 class-2, 2 class-7. Per-instance `prop_class` @0x44 is
byte-identical to the model's @0x28 on every instance of both tracks.

~~**Still open after this pass.** The recovered impulse is mass-cancelling, so a
cone clipped at 90 mph leaves at ~40 m/s and can travel several hundred metres
before the quadratic drag and the ground stop it. Whether retail's launch really
is that long, or whether something upstream limits `vn`, is not settled here.~~
**CLOSED — see section 12.** The several hundred metres were never a launch.

---

## 12. The POST-CONTACT physics [C], 2026-08-28

Section 11 fixed *whether* a prop is hit. The user's next report was that the
reaction is still wrong, and the "max travel 900 m" line section 11 left
standing was the tell. It was not the impulse. **Props were falling through
the road.**

### 12.1 The launch is not long — the fall was

`tools/validate_props.py traj` is the new differential matrix: one knock, then
120 frames of flight, port against the executed retail chain
(`FUN_001066A0` → the −0.9 bend → `FUN_0010F8D0` → `FUN_00106500` →
`FUN_0011A330`), over **5 speeds × 3 hit offsets × 3 prop classes**. Position
and velocity match at every checkpoint out to two seconds. The executed answer:

| | 20 mph | 40 | 80 | 120 | 160 |
|---|---|---|---|---|---|
| cone, dead centre | 6.35 | 12.68 | 25.36 | 38.04 | 50.71 m/s |
| cone, edge | 3.42 | 6.82 | 13.64 | 20.45 | 27.27 |
| cone, graze | 1.81 | 3.57 | 7.12 | 10.67 | 14.23 |

`dv = 0.71 × closing speed` on a centred hit, and an off-centre hit spends most
of the impulse on spin instead. Two seconds of *unobstructed* flight is
37–80 m — and in the world, with the ground pass working, a cone settles and
freezes within a second or two. **Retail's launch is metres, not hundreds of
metres.** The 900 m was props accelerating to the terminal velocity of the
recovered −1.0 quadratic drag (44.7 m/s for a 100 kg cone, 61.7 m/s for a
190 kg board) in free fall below the track.

### 12.2 The narrow phase — the defect, and it was already written down

`b3_props_update`'s world pass called the **single-plane** narrow phase
`b3_rigid_body_obb_plane_contact` **once per gathered polygon**, handing it
`soup[poly].v0` — a triangle *vertex* — as the plane point. Both halves of that
are named as defects in `burnout3_vehicle_sim.h`'s own header comment, and
`burnout3_full.c` had already found and fixed exactly this pair on the **wreck**
path, where its note records that fixing one without the other "launched the
wreck over a kilometre off the track":

* the plane form has no polygon, so it fabricates a square of half-size
  `|box dims| + 1` **centred on the point it is given**; a triangle's first
  vertex is not under the prop, so large road faces produced no contact at all;
* resolving per polygon applies N impulses and N push-outs where retail applies
  **one** (`FUN_0011A490` calls `FUN_00109EA0` exactly once @0x0011A706 over the
  one soup `FUN_00109D20` gathered @0x0011A5FB).

Measured, one cone, `B3_PROP_TRACE`:

```
t=13.0  y=97.147  ground=97.000  dy=+0.147  soup=19  hit=0  |v|=0.15   at rest
t=14.0  y=88.081  ground=97.000  dy=-8.919  soup=0   hit=0  |v|=18.23  through
t=17.0  y=-16.233 ground=97.000  dy=-113.2  soup=0   hit=0  |v|=42.36  terminal
```

19 candidate polygons and zero contacts; once below the surface the gather's own
y-band no longer reached it (`soup=0`) so nothing could catch it again. Others
did not fall but **levitated** — the stacked push-outs held them 0.5–1.3 m in
the air, jittering, never settling. Now one `b3_rigid_body_obb_soup_contact`
over the whole soup and one resolve, as retail does.

### 12.3 The car's box is a bbox PAIR

`FUN_001084E0` reads box A as `{MAX +0x00, MIN +0x10}` and derives **both** the
centre (@0x0010851B `(MAX+MIN)*0.5`, carried into the world @0x00108541) and the
half extents (@0x001085B4 `(MAX-MIN)*0.5`). The collide path passed only the
MAX and centred the box on the body origin. A shipped car's box is not
symmetric: COMPCAR1 is `max (1.0157, 1.1222, 2.0636)`, `min (-1.0157, -0.1505,
-2.0866)` — a 0.64 m half-height centred 0.49 m **up**, not 1.12 m centred on
the origin, i.e. the old box hung 0.97 m below the car's real underside. The
pair is already loaded (`g_car_cen`, `.bgv+0xE90`, live vehicle +0x1E0) and
`carcol_fill_racer` was already using it correctly. `tools/validate_props.py
box` executes `FUN_001084E0` over the pair and diffs the verdict and the depth:
**300/300**, 96 of 144 sampled poses in contact.

### 12.4 The contact point had three arms; the port had one

`FUN_001084E0` picks the contact point by **which axis won**:

| | winning axis | feature | port before |
|---|---|---|---|
| @0x00108BDA | `bi < 3`, one of A's faces | a vertex of **B**, retracted by the depth (@0x00108BFD, SUBPS @0x00108C43) | wrong box |
| @0x00108C72 | `bi < 6`, one of B's faces | a vertex of **A** (@0x00108C8B, ADDPS @0x00108CD4) | correct |
| @0x00108D00 | `bi >= 6`, an edge cross | edge-edge closest point `FUN_00108240` @0x00108D3D, axis pair decoded through the byte table at 0x0039A99C | wrong box |

The port took box A's support point unconditionally. Measured over a 90 s
scripted cone drive, the winning axis was **A-face 45%, B-face 14%, edge 41%**
(`[proparm]` telemetry) — so **86% of contact points sat on the wrong box**, and
the lever arm `r = point − prop_origin` that the impulse denominator and the
torque are built from was wrong with them. That is what sets which way a clipped
Arms 1 and 2 are retail's; the edge arm `FUN_00108240` @0x00108D3D is now ported
in `src/burnout3_props.c` (`b3p_edge_edge_closest`), resolving the segment-segment
closest point with the parallel fallback @0x00108DE7. [C]

### 12.5 The settle latch

`FUN_00109560` @0x00109692 reads `+0x20E` and, when it is raised, zeroes the
body's omega (+0xD0), angular momentum (+0xE0), velocity and speed
(+0xB0..+0xBC), resets the travel direction to matrix row 2 (+0xC0), and clears
both accumulators +0xF0/+0x100 — then rejoins the normal path @0x00109728, so
+0x110 survives and a car can still knock a settled prop. The latch is cleared
only when `+0x211` says a rigid **pair** contact touched the body
(@0x00109592..0x001095C1) — being hit is what wakes it. The port raised the
latch and never acted on it, so settled props crept upward at ~0.02 m/s forever.
Both are ported now (`hit_211`, and the freeze arm in `b3p_integrate`).

### 12.6 Two more, and one deliberate piece of GLUE

* **Soup cap 32 → 96**, the same cap the chassis gather uses. At 32 the list
  truncated on dense track soup and a board crossing at 41 m/s saw a full
  32-entry list that did not contain the floor under it.
* **One frame of travel in the broad phase.** `FUN_0011BC60` sizes its query as
  `|box| + speed*dt` (@0x0011BC7A) rather than the box alone; the prop gather
  used the box alone and a body moving 0.68 m per tick could step through a
  surface its resting box would have overlapped.
* **`B3P_LOST_BELOW = 25 m`, GLUE.** `FUN_0011A490` runs its whole
  gather/resolve only while the body is inside a loaded streaming unit
  (`+0x216 != 0xFF`, accumulators cleared otherwise @0x0011A6D5). The harness
  has no streaming units, so a prop with no soup, no surface under it and 25 m
  below its own authored ground is retired to `B3P_SETTLED` — which hands its
  slot back to the 16-body pool, where retail's own recycle would have put it.

### 12.7 What it measures

Same scripted 90 s cone drive, `B3_PROP_AUDIT=1`, max travel by any prop:

| | max travel |
|---|---|
| before (section 11's tree) | **900 m** — falling |
| soup narrow phase fixed | 340 m |
| + soup cap and travel margin | 177 m |
| + out-of-world retirement | **177 m** — and that prop was contacted **195 times** |

Every prop still past 30 m is one the pack contacted 15–195 times: cones being
dribbled along the road, not launched. Props at rest 88/101 → **92/101**, and a
settled prop now holds its position exactly (`|v|=0.00 frozen=1`) instead of
creeping.

**Bounded, not eliminated.** One or two props in a hundred still get through a
surface, and `B3P_LOST_BELOW` catches them rather than the narrow phase. The
case that survives is a prop landing where two surfaces meet:
`b3_rigid_body_obb_soup_contact` SUMS the clipping faces' normals, which is
`FUN_00107950`'s own algorithm, so the port is not wrong there — but the soup it
is summing over is not retail's. The prop pass gathers through
`b3_collision_gather_walls` with `wall_ny_max = 1.1` (admit everything, floors
included), and `gather_runtime_skip` only drops faces below `n.y < -0.7`, so a
downward-sloping overhang stays in the list and can contribute a downward
component. Retail gathers with `FUN_00109D20` instead, which is not recovered.
That is the next thing to pull, and until it is, the suite asserts a BOUND
(≤ max(3, 5 % of knocked)) rather than a zero — the number is small and it is
written down rather than rounded away.

`tools/validate_props.py` is **1705/1705** (was 337): `setup`, `step` — now
including the 27 rad/s spin an edge hit actually produces — `contact`, now over
every off-centre point the matrix uses, and the two new sections `box` and
`traj`.

**Honest limit.** `traj` asserts position and velocity to 1e-4 at every
checkpoint, and angular velocity component-wise only at the first frame; past
it, the spin RATE within 10%. Both halves are pinned tightly on their own
(`contact` matches the torque accumulator +0x120 exactly at every offset,
`step` matches the integrator at 27 rad/s for 15 frames) — composed, the
rotation update turns the frame 0.45 rad in a single step at that spin,
re-orthonormalises and rebuilds the world inverse inertia from the result, so
last-bit differences grow. The law is identical; the fast-tumbling trajectory
is chaotic, and the suite says which claim it is making.

### 12.8 The box, checked for a per-class table — there is none

`static.dat` `hdr+0x44` is a **`u8[model_count]`, stride 1** and carries a class
index and nothing else: the loader `FUN_0019B4E0` gives it a base fix-up only
(@0x0019B84D/57/59, no per-element walk, unlike the model table at `hdr+0x3C`
which is walked with `ADD EDX,0x70` @0x0019B6EC), the arena copy `FUN_0018CBC0`
reads it one byte at a time (@0x0018CD10/13/19) and reserves exactly
`model_count` bytes (@0x0018CCE0), and both runtime consumers `MOVZX` a byte
(@0x0014EA09, @0x00183AD3). **No dimensions, no mass, no restitution.**

No per-class shrink exists either: the only multiplier applied to an extent
anywhere in `FUN_001084E0` is the 0.5 at 0x003B1684, and `FUN_00108080` reads
the half extents at +0x60/+0x64/+0x68 raw. **A cone's collision box is its model
bbox — retail does not give it a narrow core.** The one per-class float table in
the image (stride 0x18 at `fx_mgr+0x190`, `FUN_0014E510`) is an FX/audio
threshold-and-ramp and is unreachable from the solver.

**Left open, deliberately.** `@0x00110A35 IMUL class,0x70` keys the *collision*
record by the class byte while the *drawn* mesh comes from the per-unit list
index. On the shipped data those disagree for 11515 of 13359 instances and read
**past the end of the table** on 8 of 37 tracks. The port uses the list's model,
which is unambiguous and makes collision match what is on screen; reproducing an
out-of-bounds read is not a fidelity win. Unchanged, and still the open item
section 10 recorded.

### 12.9 Open after this pass

* **`FUN_00109D20`**, retail's generic rigid-body soup gather with callback
  `FUN_00109CE0` (@0x00109CE0..0x00109D12), is **PORTED** (`b3_collision_gather_rigid`).
  It gathers all collision triangles in the body's AABB margin, filtering out only
  non-collidable surface low bytes `0x20` (chevrons), `0x22` (cameras/triggers),
  `0x23` (reverb bounds) and `0x24` (cull occluders), with no car velocity or
  normal-y wall filter. [C]
* **`FUN_00108240`**, the edge-edge closest point, is **PORTED** (`b3p_edge_edge_closest`
  in `src/burnout3_props.c`); the edge arm now computes the retail segment-segment
  closest point across the decoded axis pair (bi - 6). [C]
* The **pair ORDERING** gap (`TODO.md`, ranked gaps #1) is untouched: props are
  still resolved outside retail's broadphase, so A/B order in a pileup differs.
* Prop-hit **audio and score** (`FUN_00197A20` @0x0011392E) remain unported —
  unchanged from section 10; the contact reports already carry what it needs.
