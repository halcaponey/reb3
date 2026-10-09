# TODO — Burnout 3: Takedown RE harness

State as of **2026-08-22, master `332de42`**. Everything below is open; each
item names its blocker and the retail function to start from. Nothing here is
waiting on information that is missing from the executable — every physics row
has been traced far enough to name what unlocks it.

Ground rules that apply to all of it: physics/collision/triggers/control flow
are strict 1:1 with retail (`[C]`/`[S]`/`[?]` provenance with addresses, GLUE
marks on harness inventions); rendering LOOK is explicitly relaxed. Keep the
suites green — 44 `tools/validate_*.py` plus three C ones — and build the
active targets: `make -j4` (desktop Linux / WSL) and native Windows (`burnout3.exe`).
Web (`make wasm`) and Android (`android/`) are suspended for now.

Two standing gates that are not optional:

* **`tools/validate_no_baked_data.py`** — no game-derived data may be compiled
  into `src/`. It asserts both halves: the eight generated headers are gone and
  unreferenced, *and* the replacements really come off disk.
* **`tools/cextract/verify_cextract.py`** — any change to the C extraction
  pipeline must still produce byte-identical artefacts against the immutable
  Python oracle in `tools/py_extract_archive/`. Never edit the archive to make
  the gate pass.

---

## 1. Physics fidelity — the main goal

Ledger: `docs/PHYSICS_GLUE_LEDGER.md` is authoritative for the row-by-row
status (recovered / proven-unrecoverable / blocked). As of 2026-08-14 it stood
at **18 recovered / 3 proven-unrecoverable / 6 blocked**, plus 1
decided-but-not-landed; **blocker A has since closed two of the six** (PH-10,
PH-12) and unblocked a third (PH-17), so re-read the ledger rather than this
tally.

The blocked rows collapse onto **root blockers**. Work the blockers, not the
rows — each one closes two or three rows at once.

### Blocker A — the `.bgd` nav-node walk — **LARGELY CLOSED (2026-08-22)**

`FUN_00170820` (the route-following driver reached from the autopilot flag
`racecar+0x27D8`) is ported — `src/burnout3_ai.c` section 16, transcribed
branch for branch. The nav graph itself is loaded and walked at run time from
`route.bin` + `nav_edges.bin`, and `FUN_001714F0`'s navigator reset is ported
as `nav_replace_car`. `FUN_00179760` is the reset helper it invokes, not the
walker.

Consequently **PH-10, PH-12, and PH-17 are closed** — the 5 mph stuck rule
is recovered [C] (`FUN_00105340 @0x001054AF`), rescue placement uses retail
### Blocker A — the AI-DRIVE lane residue *(CLOSED — closes PH-10 residue, PH-12, PH-17)*

Mutable route-selection state (`AI+0x1F8`, `AI+0x1FC`) is now strictly unified
per-vehicle (`Vehicle->nav_latch` and `Vehicle->ai.target_mode`), eliminating cross-car
frame contamination. Recovery-state timing and resets (`FUN_001714F0` / `FUN_00179760`)
re-seed the target cursor and nav latch cleanly across all re-place paths.

* The retail graph layout is now recovered: every row is a row-relative
  `{pair, edge, link, node_count|flags}` directory, with two point IDs per
  node and forward/reverse links at `+4/+6` and `+5/+8`. The extractor exposes
  it via `BGD.nav_graph()`, `BGD.nav_nearest()`, and the exact XZ ribbon
  classifier/walker `BGD.nav_step_flags()` / `BGD.nav_walk()`, plus
  `BGD.nav_forward()`. The runtime now loads this graph from `route.bin` v3
  and maintains each AI vehicle's retail section/node cursor. The separate
  12-byte target-planning records at network `+0x1C` are exposed as
  `BGD.nav_plans()`. The runtime selects each current section's closest
  upcoming planner window and uses its A/B/C pair aim. `FUN_00174050`'s
  ribbon walker consumes bits 4, 8, 1, then 2 in retail priority order.
  Bits 4/8 take signed local steps and, only at a non-loop terminal, try the
  forward then reverse link; bits 1/2 directly take the forward/reverse link.
  `FUN_00173C60`'s one-node type-5 gate and `FUN_00178310`'s separate
  eight-node selector span belong to `FUN_00176290`; the live `+0x27D8`
  cursor follows `FUN_00174960` →
  `FUN_00175570` without that gate.
  `racecar+0x1920` is the selector's mode gate (not a route-object pointer);
  the reset-state selector carries a
  separate target cursor through type-5 entry (including its recovered
  two-successor vector tie-break) and type-4 lookahead branches.
  `FUN_00178310`'s complete span mask model is differential-tested under Unicorn
  (including the open-row clamp and wrapped mode-zero tail) and is now ported
  1:1 into the runtime via `nav_target_span_mask_ex`.

* **PH-10** — closed. Crash-recovery placement uses retail nav-node placement
  and heading. The surrounding retail reset state is the residue.
* **PH-12** — closed. The route driver is ported; AI-wheel handovers go through
  it.
* **PH-17** off-world / stuck watchdogs (`full.c`, `FUN_001712E0`) —
  **CLOSED / RECOVERED.** Retail's 5 mph stuck rule is ported in
  `burnout3_ai.c` (`FUN_00105340 @0x001054AF`), rescue placement uses retail
  `nav_replace_car` (`FUN_001714F0`), `immune_until` maps to `crash_latch_for`
  (`FUN_0010DD20`), and obsolete watchdogs (`beach_time`, `unstuck_side`)
  are purged.

### Blocker B — the traffic body update *(CLOSED — closes PH-07, PH-13, gap 2)*

**`FUN_00120F30`** (traffic vtable `0x003B11EC` slot +0, ~0x460 bytes) is ported
into `full.c`'s traffic section: streaming-unit gates on `+0x216`/`+0x242C`,
the towed-body link `+0x2424` (both bodies share the `+0x20E` sleep
byte, @0x00120FE6..0x00121032), the full two-rigid-body tow constraint with
spring, jackknife/rollover angular momentum removal, and breakaway detachment
(`FUN_00121400`). Direct disassembly refutes the former
``route driver behind +0x13A0`` wording: that field is the model pointer used
only for the `+0x16A4/+0x16A8` tow anchors in this function.

* **PH-07** is LANDED: traffic carries a persistent `B3RigidBody`
  through the generic car-contact pass and shared `FUN_00109560` integrator;
  the synthetic knock write-back and `1-4·dt` decay are gone. Its `+0x216`
  residency identity now comes from the streamed collision unit owning the
  ground query, and the `+0x242C` entry/exit gate clears or skips the base
  update accordingly. Partner sleep coupling, the model's full
  `+0x16A8/+0x16A4` hitch vectors, and the shared `FUN_0010F8D0` two-body
  normal deflection solve and kingpin spring now run across persistent
  tractor/trailer bodies. `FUN_001A20F0` runs its road-agent speed/cursor/
  occupancy passes before `FUN_001A6B40` updates physical traffic bodies and
  `FUN_001A8640` updates trailers; the harness now preserves that separation,
  so a coupled body sleeping outside a resident unit no longer freezes its
  route cursor. `FUN_0019FFA0` is now decoded as a four-row, clamped cubic
  ribbon sampler (each row has two u16 point IDs into 16-byte points). Its
  relocated event descriptor is also known as `{pairs, distances, branch_rows,
  point_base, count}`: `FUN_00158CC0` relocates the first three pointers in
  each 0x14-byte descriptor and supplies the shared point base. The source is
  now extracted from the event RIDX image (`param+0x3CC/+0x3D0`) through
  `BGD.traffic_paths()` (21 paths for US_C3/OFFSGRCF). The runtime now loads
  `traffic_paths.bin`, advances a distance-table cursor, samples each pair at
  its centre through the retail four-row uniform cubic B-spline, and
  retires/reseeds at segment ends. Each agent retains the retail initializer's
  0.45..0.5499 randomized lateral fraction. `FUN_001A03F0` / `FUN_001A0600`'s
  same-descriptor owner lifecycle is now refreshed after every cursor commit:
  every descriptor row names its nearest agent ahead, and each agent follows
  that linked owner rather than a world-space lane scan. The retail RNG
  sequence, cross-descriptor occupancy
  pool replacement policy, and avoidance magnitude remain. `FUN_001A20F0`
  initializes the road-agent branch-attempt counter (`+0x48`) to 1 when its
  selected racecar has `+0x1920 == 0`, otherwise 0; `FUN_001A8EE0` invokes
  the selector while that counter is nonzero and only `FUN_001A9040` commits
  its selected descriptor after the source cursor reaches the selected switch
  row. `FUN_001A0750` now identifies `branch_rows` as one 0x12-byte record
  per cursor row, with four `{u16 target_row at +0x00, u8 target_path at
  +0x0C}` columns. `traffic_paths.bin` v3 also preserves `FUN_001A28B0`'s
  TDESC `+0xA4/+0xA8` pool-window table: each 0x18 window has an inclusive
  progress range at `+0x04/+0x08` and `+0x14` six-byte `{first_row,last_row,
  path_id,direction}` requests at `+0x00`; the manager processes the current
  and two preceding windows circularly. The runtime validates the extracted
  bounds. `FUN_001A3470` occupancy-stamps each request, then
  `FUN_001A2B20` pops physical bodies and 0x50-byte road agents from separate
  free lists (`FUN_001A38F0`/`FUN_001A3A10`): physical release appends at the
  tail (FIFO reuse via `FUN_001A41A0`), while agents return at the head (LIFO reuse via `FUN_001A3A80`).
  The dual-pool lifecycle, trailer physical allocation recursion (`FUN_001A75A0`/`FUN_001A3970`),
  agent rollback on starvation (`FUN_001A2B20`), and streaming/sleep gates
  (`FUN_00120F30`/`FUN_00104840`/`FUN_001213C0`) are fully ported and verified.
* **PH-13** traffic mover / braking horizon — CLOSED. Cursor movement, speed
  law, four-knot clamped cubic B-spline pose (`FUN_0019FFA0`), avoidance nudge
  (`FUN_0019FEC0`), and dual-pool lifecycle are ported and verified.

### Blocker C — one wall source / real contact geometry *(closes PH-09's object arm, the `crash_fired` switch, gap 3)*

The live racer, wreck, and knocked-prop response paths now gather real
collision triangles into their recovered contact solvers. Wreck containment
and no-pipeline fallbacks still use sphere queries only as
anti-tunnelling/bootstrapping nets, so the systems are not fully unified yet.

* **PH-09 OBJECT arm** (`full.c:1412-1426`) — CLOSED / LANDED. The wall arm
  is recovered. Static props report their recovered class through `b3_props`.
  Type-3 traffic promotion lifecycle is ported from retail `FUN_00114910` and
  `FUN_00120BA0` (type 3 -> 4 promotion, `FUN_00040AE0` inverse frame,
  `dir * speed` velocity, COM height `(half_ext.y - center_off.y) * 0.1f`,
  designation byte, streamed flag, trailer promotion, and `FUN_00113960` wreck response).
* **`crash_fired` consumer switch — landed for live racers.** `crash_fired`
  is the retail-faithful decision and now starts the existing wall-crash
  consequence path using its contact record. Retail's latch/cooldown is
  documented `[C]` — `FUN_0010DD20`: `veh+0x210 != 0` plus the per-slot timer
  at `mgr + slot*0x3C + 0x130` — and maps onto
  `crashed_until`/`immune_until`. Debug with `B3_CFIRE_TRACE=1`.
  **Per-slot latch table now recovered and wired** (2026-08-15): the
  `+0x130` latch (the `immune_until` side) is a per-class/per-presentation
  table, not a flat value — truck fresh 7.0 / truck-presented 15.0 /
  other fresh 3.0 / other-presented 15.0 (17.0 when `FUN_00017390()` is
  live, runtime-only `[?]`), on the dilated clock. `b3_crash_latch_duration()`
  (crash.c) transcribes the `LAB_0010e431` branch; all six racer
  `immune_until` sites call it via `crash_latch_for()`. The worklog's
  "16.0/7.0/5.0/4.0/6.0" table was a mislabel of the float words.

### Remaining call-graph gaps (`PHYSICS_GLUE_LEDGER.md` "gaps, ranked")

1. **Props and debris broadphase pair ordering and A/B dispatch — CLOSED (2026-10-09)**:
   The prop *narrow* phase is retail's own (`FUN_001084E0`'s 15-axis OBB SAT,
   `b3p_obb_contact` in `src/burnout3_props.c`), post-contact world pass runs
   `b3_rigid_body_obb_soup_contact` with `+0x1D0/+0x1E0` bbox pair and `+0x20E/+0x211`
   settle latch. Edge-edge closest point `FUN_00108240` (`b3p_edge_edge_closest`) and
   prop-hit boost/score/chain accumulator `FUN_00197A20` (@0x0011392E, `b3_score_events_prop_hit`
   with `B3_SFX_PANEL_L_PROP` impact audio) are ported and verified.
   Broadphase pair ORDERING and A/B dispatch: `b3_carcol_broadphase` in `src/burnout3_carcol.c`
   ports retail `FUN_00110AF0`'s X-axis sweep-and-prune (`b3_sap_cmp` ascending sort,
   active list interval tracking, Z/Y interval overlap gates, `(min, max)` index ordering,
   cap 0x100), and `b3_carcol_resolve` ports retail `FUN_00111CD0` Arm 4 car priority
   for secondary/crashed bodies. Verified in `tools/test_sap_broadphase.c`.
2. **Crash-floor & barrier staging caller integration — CLOSED (2026-10-09)**:
   Wheel and chassis contact share one frozen raw-collision snapshot; the chassis view
   applies `FUN_0011BBE0`'s recovered wall predicate.
   Retail's crash-floor & barrier staging caller (`FUN_0017D0F0` @0x0017D0F0) is ported
   as `b3_crash_director_stage_zone`: tests vehicle rigid body distance against zone center,
   stages the closest zone's 6 boundary polygons into `veh+0x11D0` via `b3_vehicle_set_crash_floor`
   (`FUN_00125790`), and sets `flags_1351 = 1`.
   `FUN_0018BC90` is ported as `b3_crash_director_update_zones`: loops over active vehicles and
   zones (initial `min_dist = 100000.0f` matching retail float `0x003A7950`).
   Connected into the engine frame update at `0x001AA84A` right before `carcol_pass`.
   When no zones are present (`num_zones == 0`), `flags_1351` is untouched without synthesizing
   artificial records. Verified in `tools/test_crash_director_zones.c`.
3. **Four manager stages audited — PROVEN NON-PHYSICS / PRESENTATION & MEMORY (CLOSED)**:
   `FUN_00114E60` (16 m proximity cache ported), `FUN_0010D1C0` (crash cooldown & camera tracker),
   `FUN_00164FB0` x2 (viewport/listener presentation), `FUN_00111850` (dead entity recycling).
4. **`FUN_0011BE50`'s own head is ported (0x0011BE5F..0x0011BF43) — CLOSED (SAME-ORDER)**:
   Pipeline gate `racecar+0x19A8`, crashed branch `veh+0x210` / `crash_clock_1530`, and collision hit latch `veh+0x153F` / `stamp_1538` (`flags_1353 |= 0x10`).

### Open `[?]` questions — highest value first (`docs/RE_NOTES.md`)

* **The wreck recovery clock — RESOLVED (2026-08-15), no bug.** The
  "25 s of wall time" is the *intended* value, and the harness already
  reproduces it 1:1. `g_delta_time = b3_tdfx_update()` returns the dilated
  `period/divisor` (takedown.c:872), `g_race_time += g_delta_time`, and the
  player never leaves the RACING state (there is no `g_state = CRASHED`
  writer), so `crashed_until = g_race_time + 5.0f` is already 5.0 GAME
  seconds = 25 s wall at divisor 5 — a direct mirror of
  `FUN_00198E60`'s `racecar+0x10DC = dilated_clock + 5.0` @0x00198F65. The
  worklog's "5 s WALL cap bug" was a false alarm built on the assumption
  that `g_race_time` runs on wall time. No change made.
* **Which crash-entry site fires for which crash kind — RESOLVED (2026-08-15),
  per-kind table landed.** The old `b3_wreck_begin` mixed two sites (a 0.40
  corner torque that belongs to the car entry with a 0.65 launch that belongs
  to the rollover). It is now split by a per-kind table: `b3_wreck_begin_entry`
  is the single funnel (full.c:2174); `wreck_begin_for` classifies each crash
  consequence site into CAR / WALL / ROLLOVER; the table (crash.c:1124-1139) is
  CAR = 0x0A corner, 0.40 spin, 0.0 launch (torque-only), ROLLOVER = 0x08
  corner, 0.90 spin, 0.65 launch, WALL = null kick (0/0/0). The wall crash's
  real answer is "neither — only `FUN_0011AEF0`'s impulse", and that is what
  the null WALL row produces. The old `b3_wreck_begin(` wrapper has zero
  remaining callers. ROLLOVER detection is active and wired (`src/burnout3_full.c`):
  inverted vehicles (`rb->frame[1][1] < 0.0f`) during ground or world contact trigger
  `B3_WRECK_ENTRY_ROLLOVER`, and crash entries occurring while inverted are promoted to
  ROLLOVER, firing retail's 0.65 linear launch (`0x0011C421`) and 0.90 spin (`0x0011C439`).
* **The alternate type-3 request lifecycle — RESOLVED / AUDITED.**
  TDESC `+0xB4 & 0x04` selects the `FUN_001A5C70` queue. `FUN_001A3AE0` state 6 maps
  TDESC schedule row `+0x38/+0x3C` 0x0C descriptors into `manager+0x30+slot*4`;
  each descriptor points to 0x20 records whose `+0x1B` bit 0 reaches `FUN_001A2B20`.
  The 3,005 static records across the 377 shipped event TDESCs all have that bit clear
  (`+0x1B & 1 == 0`), proving the designation flag is never set statically. Normal scheduling
  (`FUN_001A5910`) and dynamic road agents (`FUN_001A6070`) explicitly push zero.
  The live vehicle promotion lifecycle (`FUN_00114910` / `FUN_00120BA0`) is fully ported
  in `src/burnout3_full.c` and `src/burnout3_carcol.c`: promoted traffic vehicles correctly
  receive designation byte `DAT_0073BB8C` (`+0x242B`), mapping them to carcol class 3 (or 5
  when undesignated), and standard carcol and wreck rules govern the physical body.

---

## 2. Windows Native Target (Active)

Targeting native Windows execution (`burnout3.exe`) without requiring WSL or emulation.

* [x] **Root CMake configuration** — `CMakeLists.txt` supporting native Windows MinGW-w64 and MSVC toolchains, defining `burnout3.exe`, `cxtract.exe`, and object libraries `cextract_obj` and `isodata_obj`.
* [x] **Dependencies integration** — native 64-bit Windows `SDL2`, `SDL2_image`, and `zlib` located automatically (via Scoop or system paths) with automatic post-build DLL deployment into the binary directory.
* [x] **Platform & Path compatibility** — minimal POSIX compatibility layer in `src/compat/win_posix_compat.h` providing Windows implementations of `mmap`/`munmap`, `setenv`, `realpath`, `symlink`, `mkdir`, `lstat`, `sysconf`, and `fmemopen`, plus missing OpenGL 2.0 tokens and undefining legacy 16-bit `near`/`far` macros.
* [x] **Native GL & Audio pipeline** — native Windows OpenGL context (`WGL` via SDL2 video driver) with AMD Radeon RX 9070 XT, MSAA 4x, retained shader program, and SDL2 audio. `burnout3.exe` builds and runs cleanly out of the box on Windows.

---

## 3. Android Target (Suspended for now)

Runs and renders on the Pixel (Mali, Android 16). See `docs/ANDROID_PORT.md`.

**Correctness**

* **The postfx present composite + motion blur are OFF on Android**
  (`B3_POSTFX_PRESENT` / `B3_POSTFX_BLUR` default 0 in `b3_android.c`). The
  `glCopyTexImage2D` back-buffer grab produces a solid white frame under
  gl4es on Mali. Needs an FBO-based grab; until then the phone renders without
  retail's x2 composite.
* **Dark-red blob at the bottom-centre** of the frame at the phone's ultrawide
  aspect — suspect the player car's own geometry against the near plane.
  Reproduce with a device screencap; not seen on desktop.
* **Pause/resume is untested.** SDL destroys the GL context on background;
  every VBO, shader program, texture and gl4es state the harness uploaded at
  init would need rebuilding. Expect a black screen after a task switch.
  (This used to say "display list" -- there are none left anywhere in the
  harness; see docs/web/webprof_sweep.md's fourth and fifth waves.)
* **Tilt/button ergonomics** — awaiting play feedback. Knobs already exist:
  `B3_TILT_LOCK_G` (default 0.42 g ≈ 25° for full lock), `B3_TILT_SIGN=-1` to
  invert. Button geometry is in `b3_touch.c`'s `BTN[]` table.

**Packaging**

* **Release signing config** — `assembleRelease` still has no `signingConfigs`;
  the APK is hand-signed with the debug keystore in the build loop.
* **The packer drops files the runtime now needs — RESOLVED.** `pack_assets.sh`
  now bundles all runtime per-track data files (`envmap.png`, `light_probes.bin`,
  `props.bin`, `route.bin`, `grid.bin`, `traffic.bin`, `nav_edges.bin`,
  `traffic_paths.bin`, `pace.bin`).
* **Asset diet — RESOLVED.** `pack_assets.sh` restricts `build/cars/`
  to the 8 roster slots (`roster.bin`) plus the active track's `.bgd` traffic
  set (`traffic.bin`) when `B3_PACK_ALL_CARS` is 0 (default). This drops the
  staged car payload from 133 MiB down to 39 MiB and the packed asset zip down
  to 55 MiB (< 60 MiB target). `B3_PACK_ALL_CARS=1` remains available to pack
  the full fleet.
* **Crash audio beds are not packed** (`B3_PACK_CRASH_AUDIO=1`, ~147 MiB), so
  the phone logs "no crash beds" and crashes are quiet.
* **On-device asset sideload** so another track can be `adb push`ed without a
  rebuild — the extractor already skips when the stamp matches, so a manual
  stamp write is all it takes.
* **`armeabi-v7a` + app bundle**; at that point the asset zip must move to Play
  Asset Delivery (a 150 MB APK is fine to sideload, not to publish).
* **Audio latency** — SDL OpenSLES, 1024-frame buffers at 44.1 kHz mono. Check
  for underruns; consider `SDL_HINT_AUDIODRIVER=aaudio`.

**Process**

* **Keep the two source lists in lockstep — RESOLVED.**
  `android/app/src/main/cpp/CMakeLists.txt` `B3_SRCS` is synced in lockstep with
  the Makefile's `SRCS` (including `burnout3_ai_avoid.c`, `burnout3_aftereffects.c`,
  `burnout3_dj.c`, `burnout3_scenery.c`, `burnout3_rt.c`, `burnout3_backend.c`,
  and `burnout3_emu.c`). (`burnout3_isodata.c` is absent by design — Android does
  not take the ISO path.) The web target builds from the same `SRCS` variable.

---

## 4. Validation & tooling

### The archived collision extractor's containment self-check — RESOLVED (2026-10-09)

The mystery of `tools/py_extract_archive/extract_collision.py`'s failing check:
```
[extract_collision] route XZ bounds x[4015..5812] z[1285..3137]
                    NOT CONTAINED IN (FAIL) collision bounds
```
is fully resolved:
1. **The premise of the legacy check was testing against the full nav graph**, not the driving route. The old `route_bounds()` inspected the purged `burnout3_track_paths.h`, whose points spanned all 7,015 `.bgd` navigation points (`B3_NAV_POINTS`). On US_C3_V1, nine nodes of section 7 (nodes 956..964) form a hairpin envelope out to `x=4014.59` (`z=1507.95`), whereas the streamed unit collision bounding box for that zone starts at `x=4032.53`.
2. **The actual DRIVING ROUTE (`b3_route` / `route.bin`) is fully contained**:
   - Driving route XZ bounds: `x[4070.0 .. 5760.6]`, `z[1339.9 .. 3083.8]`
   - Road strand / wall A & B bounds: `x[4062.8 .. 5767.8]`, `z[1332.7 .. 3090.9]`
   - Collision world bounds: `x[4032.5 .. 5970.4]`, `z[1262.0 .. 3265.1]`
   Every drivable route point and wall point lies well within the collision mesh with >30m margin on all sides. No vehicles fall through.
3. The modern C pipeline `tools/cextract/cx_collision.c` correctly extracts all collision triangles without referencing the stale header.
4. `generate_track()` in `src/burnout3_full.c` now dynamically formats `g_track.name` from the active track selection (`B3_TRACK` / `US_C3_V1`) instead of the hardcoded `"Bangkok (Tracks/AS/C1_V1)"` label.

* **`tools/validate_gameplay.py` — green (91/91)**. Its collision-world
  section now compares the shared u16 source grid rather than float32/float64
  decimal expansions, uses valid resident-unit samples, and probes the
  current `US_C3_V1` low-road contact instead of a stale track-specific
  coordinate.
* The suites below were green at the counts recorded here and must stay green;
  the counts themselves grow as cases are added, so treat the *names* as the
  list and each suite's own output as the count: port 151, crash_traj 134,
  crashcinema 115, td_rules 532, takedown 973, props 1705, ai 163, carcol 1296,
  sfx 399, hud 769, carfx 248, postfx 179, music 100, particlefx 600,
  boostfx 77, scenery 315, light_probes 61, no_baked_data 90,
  draw_distance 12, photo 247/247 without a reference binary (section 3b's
  six dark-sky atmospherics checks are new; with `B3_PHOTO_REF_BIN` set,
  section 11's cross-build leg is the one red, which any deliberate
  photo-stack change turns red and which section 14.1c discharges by
  measurement — see `docs/PHOTOREALISM.md` §5), prop_contact 16.
  There are now 44 `tools/validate_*.py` in total,
  plus three C suites (`validate_frozen_soup.c`, `validate_traffic_pool.c`,
  `validate_traffic_reservations.c`) with `make test-*` targets.
* **THE PHOTOREALISM LAYER IS PINNED OFF IN EVERY SUITE THAT RENDERS**
  (`docs/PHOTOREALISM.md`). Six INSPIRED screen-space effects ship ON by
  default; a suite that verifies RECOVERED pixel behaviour sets `B3_PHOTO=0`
  in its own boot environment, exactly the way it already sets
  `B3_MUSIC_SEED` and `B3_TRACK_NOSHINE`. That is sound rather than a dodge
  because `tools/validate_photo.py` section 2 **proves** `B3_PHOTO=0` renders
  bit-identically to the pre-wave build, on the desktop and on the web. **If
  that leg ever fails, every one of those pins stops being valid** and the
  suites stop measuring what they say they measure — fix the identity before
  fixing anything else.
* **Tier 4rc's own follow-ups** (`docs/PHOTOREALISM.md`, "What tier 4rc still
  cannot do"). None of these blocks anything; each is written down so it is
  reported as a known limit rather than as a bug.
  - **A wreck is traced with its intact hull.** A faithful one needs a second
    tree per car (the aperture shell) *and* a per-frame decision about which of
    up to six panels are still attached — i.e. a tree whose contents change
    while the car is being driven, which the whole two-level construction
    exists to avoid. `carbvh.bin`'s model table already has room: an instance
    names a *model* and nothing says a car may have only one.
  - **Night tracks lose a grounding cue.** On US_P1's sunset the traced shadow
    carries much less contrast than retail's stylised ellipse, so a car reads
    slightly less planted with the blob suppressed. The traced shadow is the
    more correct one; the blob was doing a job beyond representing the sun.
    Keeping a *reduced* blob where the traced term is weak is the obvious fix.
  - **Traffic retire in view.** `B3_RT_CARS=all` traces the nearest-N traffic
    by distance to the camera, so an instance can leave the set while still on
    screen. It costs a shadow, not a wrong picture, and 20 slots covers what a
    frame actually holds — but it is a pop, and the nearest-N pick is where it
    would be fixed.
  - **`TSPC_Car5` has no tree**, because its mesh fails the same plausibility
    gate the mesh exporters apply. The renderer draws a box for it and it keeps
    its blob, so the two agree; the real fix is upstream, in whatever makes
    that model parse as 72.8 m long.
* `tools/validate_car_shine.py` was already failing 4/45 in this tree BEFORE
  the photorealism wave (measured against a binary built from the wave's base
  commit, same machine, same session). Do not attribute those to the layer;
  the layer is pinned off in that suite.
* **A single pinned render is NOT reproducible in this tree.** Ten runs of one
  binary with an identical environment and a warm cache produced five distinct
  frames — differing over 700,000 pixels, i.e. different MOMENTS, not
  different pixels. Something upstream of the render occasionally spends an
  extra simulation step before the green light. `tools/afx_sweep.py`'s
  `pin_ok` and `tools/validate_photo.py`'s `world_pin` both exist to work
  around it; the cause has not been found and is worth finding.
* **`.panels` sidecars must be regenerated** after any change to the `.bgv`
  extractor — now `tools/cextract/cx_cars_bgv.c` (the Python
  `tools/extract_bgv.py` is a shim onto the archived oracle). They carry the
  `panelbb` line that seeds the recovered panel OBB. Without it
  `panel_piece_spawn` silently falls back to the invented `B3_PANEL_HALF` cube
  (`box_ok[]` records which pieces got a real box).
* **Any extractor change must re-pass `tools/cextract/verify_cextract.py`**
  against a fresh oracle run. Byte-identical, pixel-identical for PNGs, and a
  file on only one side is a failure.
* **`cx_art_postfx.c` emits the `env+0x60` `light_rgb` block — RESOLVED.**
  Both `build/postfx/<ID>_env.txt` sidecars and `enviro_manifest.txt` correctly
  emit the `env+0x60` `light_rgb` line matching the oracle in
  `tools/py_extract_archive/extract_postfx_art.py`. All 107 sky PNGs are
  pixel-identical and all 37 text sidecars match.
* **The extraction stages and the texture load run on a worker pool**
  (`tools/cextract/cx_pool.h`); `B3_JOBS=1` puts every one of them back on the
  calling thread, unchanged, which is the first thing to try when a
  parallelised stage is suspected. Cold ISO boot 17.8 s → 5.4 s desktop,
  38.8 s → 18.0 s web. The measurement, the rejected candidates and the gate
  results are `docs/LOAD_PARALLELISM.md`.

---

## 5. Web Target (Suspended for now)

Design and the measured constraints behind it: `web/README.md`. Build with
`make wasm`, serve with `make serve`, gate with `make test-web` (headless
Chromium over CDP — never a visible browser).

* **In-browser audio is deferred.** `SDL_OpenAudioDevice` is answered with `0`
  because `AudioContext` does not exist on the worker `main()` runs on. The
  route is an AudioWorklet owned by the main thread, pulling the same 44.1 kHz
  mono S16 mix `audio_callback()` produces through a ring in the shared heap —
  the image bridge's trick in the other direction. `web/README.md` §3.
* **Keep the smoke gate honest.** `tools/web_smoke.py` is the only automated
  check on this target; it must fail on a crash rather than time out green.
* Unlike Android, the web target builds from the Makefile's **same `SRCS`**
  variable — deliberately, so there is no second source list to drift. Keep it
  that way.
* **The web renders at the VIEWPORT** (canvas CSS box x devicePixelRatio),
  capped at a 1080p-class pixel count because the 3090 holds 60 there and
  manages only 15.7 fps at 4K. `B3_RES=WxH` overrides, `B3_RES=pin` restores
  the old 640x480. If a future wave makes 4K viable, the cap is one constant
  (`B3_WEB_MAX_PX` in web/b3_web.c) and the measurement to redo is the table in
  docs/web/webprof_sweep.md, seventh wave.
* **Compare the web against the DESKTOP, not against the web.** Four rendering
  defects survived every web gate this repo has because all of them were
  web-to-web. Pin the same frame on both at the same resolution and seed --
  and pick an EARLY frame, because the two sims diverge into different races
  within a few hundred frames and that reads as a rendering fault.
* **gl4es is retired on the web** (kept on Android). `<GL/gl.h>` resolves to
  `web/GL/gl.h` = GLES2; `web/fetch_deps.sh` is a no-op. If a compatibility
  built-in ever reappears in a shader or a `glBegin` in a pass, the web build
  breaks at compile time rather than silently — that is intentional.
* **The GL call count is ~2 400/frame, not the ~800 the phase-2 brief aimed
  at.** The frame is nonetheless capped at 60 on real hardware with ~6.9 ms
  idle, so this is headroom work, not a defect. The breakdown names the two
  jobs: `cars` spends 912 calls on 58 draws (every sub-mesh re-points its
  vertex format, rebinds its texture and re-uploads its matrices — one shared
  car vertex format and per-instance uniforms would collapse it), and `track`
  spends 501 on 206 draws (merge-by-texture is per material-run; a
  draw-order-preserving global texture sort would cut it). See
  `docs/web/webprof_sweep.md`, sixth wave.
* **The aftereffects chain is ON for the web** as of the sixth wave. Both of
  the reasons it shipped off were gl4es' and both are gone: cost is +0.14 ms of
  `render_frame` (was +2.3 ms) and the sky survives. Worth knowing: the sky
  loss was never the depth precision it was attributed to — it survived on a
  16-bit attachment. *(Superseded in part: the context is WebGL 2 now, with a
  real WebGL 1 fallback, so the depth attachment is 24-bit where the browser
  gives it — `web/README.md`, "The context is WebGL 2". The two versions
  disagree about framebuffer completeness in a way that matters: WebGL 1
  requires all attachments to share dimensions and answers
  `INCOMPLETE_DIMENSIONS` `0x8CD9` when they do not; WebGL 2 dropped the rule.
  That is which status the resize cliff reported.)*
* **The viewport measure used to be a SYNCHRONOUS main-thread round trip on
  the frame loop — fixed, the read is pushed now.** `b3_web_defend_pin()`
  asked the browser how big the canvas was every 30 presents, from the worker
  the game loop runs on, through *two* calls that are both
  `__proxy: 'sync'` in emsdk `src/lib/libhtml5.js`:
  `emscripten_get_element_css_size` **and**
  `emscripten_get_device_pixel_ratio`. So twice a second the loop *blocked*
  until the main thread got round to answering, and `g_real_fps` (measured on
  the worker over a 30-frame window) absorbed whatever the main thread was
  busy with — a burst of proxied `console.log` (FULL SLAM lines, crash-trace
  chatter), an OPFS persist (`b3CachePersistOpfs`, main-thread, debounced
  2 500 ms after a mid-race materialisation), a GC. That is the shape of the
  transient dips that recover fully. It is **not** crash-trace file I/O:
  `build/crash_trace_NNN.log` is MEMFS (`/app` and `/app/build` are MEMFS;
  only `/app/build/.isocache` is mirrored to OPFS) and `b3MemfsWalk` only
  ever walks the cache root, so traces are never persisted. The main thread
  now OWNS the measurement — `b3_web_viewport_watch()` installs a
  `ResizeObserver` plus a re-armed `matchMedia` for dpr and writes the box
  into a shared-heap slot under a seqlock; the worker reads four int32s out of
  its own heap and never blocks. `B3_WEB_VP_POLL=1` forces the old path and
  `B3_WEB_VPPROF=1` times the measurement, so the before/after comes out of
  one binary: **mean 0.35 ms / max 0.77 ms → mean 0.002 ms / max 0.005 ms**,
  about 100–150×, and that is measured against a nearly idle headless main
  thread — the old path's *best* case, since its cost is a queue round trip
  that grows with whatever else the main thread is doing.
* **The crash-trace open probe — RESOLVED.** `crash_trace_tick` in
  `src/burnout3_full.c` now maintains a persistent `static int next_n = 1`
  counter across crashes, eliminating the O(N) probe loop of `fopen(..., "r")`
  on every crash start.
* **The menu frame loop is 69–135 ms with `sim`/`render` near zero** and is
  unexplained. It is NOT the frame-limiter defect fixed in the sixth wave — the
  menu screens use their own throttle (`if (now - last < 16) SDL_Delay(...)`),
  which cannot inject sleep on a late frame. Needs its own measurement.

---

## 6. Process notes for agent waves

* Agents work in their own worktree (`git worktree add --detach
  .claude/worktrees/agent-<name> master`), never commit, and never edit
  `src/burnout3_full.c` — that ships as an idempotent patch script whose
  anchors each occur **exactly once**.
* **Write deliverables incrementally.** A wave-4 attempt died on an API credit
  error and lost everything because it saved `changes.patch` for the end.
* **Verify agent claims against Ghidra before banking them.** Wave 4's
  inertia lead was correct but its stated blocker premise was wrong, and
  Ghidra's decompile of `FUN_00109BB0` silently drops two of three axes — the
  disassembly is the ground truth, not the decompile.
* Always run the game with `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`,
  one instance at a time.
* **Never add game-derived data to `src/`.** Not a table, not a string, not a
  "temporary" constant. It goes in a data file the port loads at run time.
  `tools/validate_no_baked_data.py` will catch it, and the reason the rule is
  absolute is that a compiled-in copy does not *look* missing — the port used
  to substitute one track's road and grid on every other track and the symptom
  read as a physics bug for weeks.
* **`tools/py_extract_archive/` is immutable.** It is the oracle the C
  extraction pipeline is gated against. If the gate fails, the C side is wrong.
* **Retail is runnable.** Before arguing about whether the port matches, flip
  the feature to `retail` in `build/backends.cfg` (or run `--retail`) and
  measure. That is what the backend switch is for.
