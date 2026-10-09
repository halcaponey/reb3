# Car-vs-car collision — the recovered chain (2026-08-11)

> **Status (2026-08-22).** §11 landed after §§1–10 were written and they were
> not reconciled, so read the later section as authoritative where they
> disagree: **`FUN_00112E70` (car vs type-3 object) is ported** as
> `b3_carcol_resolve_traffic()`, despite the "[S, not ported]" mark in the
> overview chain and its listing under §10 "Open / not ported". `FUN_00113890`
> (type-5) *is* still genuinely open. The acceptance count in the header line
> is also stale — §11 records the current, larger figure. Hull extraction is
> now the `hulls` stage in `tools/cextract/cx_cars_hull.c`; the
> `emulate_carcol.py --extract-hulls` path still works and produces identical
> bytes.

How Burnout 3 makes two *vehicles* collide with each other: pair enumeration,
the convex-hull narrow phase, the mutual response, and the slam/takedown
classification that turns a contact into a takedown.

Evidence marks: **[C]** = confirmed by executing the real x86 under Unicorn
and asserting a 1:1 C mirror reproduces its writes; **[S]** = read from the
instructions, constants confirmed in the image, no green case; **[?]** = open.
Anything not from the binary is marked **GLUE**.

Acceptance: `tools/validate_carcol.py` — **730/730 green**. Every case seeds
two vehicles, runs the real chain under Unicorn (`tools/emulate_carcol.py`)
and the compiled `src/burnout3_carcol.c` from identical state, and diffs the
contact point, normal, per-body separation, impulses, forces, torques, impact
magnitude, slam classification, and crash triggers field for field.

All addresses are the **corrected ELF mapping** (`build/burnout3.elf`, see
HANDOFF §2). Old flat-load addresses are `new − 0x10000` in `.text`.

---

## 0. The chain at a glance

```
FUN_001AA720                       (collision manager tick)
└─ FUN_00110AF0   [C-disasm]       sort + sweep over per-object world AABBs
   │                               -> pair array (stride 0x30, cap 0x100)
   ├─ FUN_00114270 [C]             per-object world AABB  (rec +0x10 / +0x20)
   ├─ FUN_00114610 [S]             pair filter (3-vs-3, 5-vs-5, both asleep)
   └─ FUN_00111CD0 [C-disasm]      per-pair dispatch on the object type byte
      ├─ both cars, BOTH un-crashed (veh+0x210 == 0)  -> FUN_001121F0  [C]
      ├─ both cars, one/both crashed                   -> FUN_00113960  [C]
      ├─ car vs type-3 object                          -> FUN_00112E70  [S, not ported]
      └─ car vs type-5 object                          -> FUN_00113890  [?]

FUN_001121F0 / FUN_00113960 both open with
   FUN_0010A9D0  [C]  build the hull-query context (two frames, two inverse
                      frames, two hulls, mode byte)
   └─ FUN_0010ABC0 [C] coincidence reject, then
      └─ FUN_0010AC20 [C] the convex-hull narrow phase
         ├─ FUN_000116E0 matrix multiply        ├─ FUN_00013CA0 point xform
         ├─ FUN_0010B210 support-edge soup      ├─ FUN_0010C0D0 clip+dedup
         │  └─ FUN_0010C220 half-space clip     ├─ FUN_00038C00 batch xform
         ├─ FUN_0010B310 AABB centre            ├─ FUN_0010C000 closest plane
         ├─ FUN_00031330 rotate vector          └─ FUN_0010BE70 ray exit plane
```

---

## 1. Broad phase — FUN_00110AF0 [C for the predicate]

The collision world object (`param_1`) holds:

| offset | meaning |
|---|---|
| `+0x70` | object records, **stride 0x30** |
| `+0x1CB70` | object count |
| `+0xE5F0` | sort array, stride 8: `{float key, u16 obj, u8 begin}` |
| `+0x1CB74` | sort entry count (2 per object) |
| `+0xE6C90` | pair records, **stride 0x30** |
| `+0xE9C90` | pair count (cap `0x100`) |

Object record:

| offset | meaning |
|---|---|
| `+0x00` | **type byte** (0..8) |
| `+0x01` | flag |
| `+0x04` | frame (4×4) pointer |
| `+0x08` | `{bbmax, bbmin}` vec4 pair = `veh+0x1D0` |
| `+0x0C` | vehicle pointer |
| `+0x10` | world AABB **lo** (vec4) |
| `+0x20` | world AABB **hi** (vec4) |

`FUN_00114270` recomputes `+0x10`/`+0x20` from the frame rows and the box
(standard OBB→AABB: centre ± Σ|R<sub>ij</sub>|·half<sub>j</sub>) [C — 6
asserts, `validate_carcol.py` "broad phase"].

The sweep sorts the 2·N interval endpoints by the **x** key
(`rec+0x20` for the end entry, `rec+0x10` for the begin entry;
`_qsort` with comparator `LAB_00110AD0`), then walks the sorted list keeping
an active set and testing each new object against it with
`b.lo.z < a.hi.z && a.lo.z < b.hi.z` **and** `b.lo.y < a.hi.y && a.lo.y <
b.hi.y` plus `FUN_00114610`. Emitted pairs are ordered
(`FUN_00011510`/`FUN_000114E0` = max/min index).

`b3_carcol_broadphase()` implements retail `FUN_00110AF0`'s exact sweep-and-prune
chain: interval endpoint generation, ascending X sort (`b3_sap_cmp` matching `LAB_00110AD0`),
active-set interval tracking, Z and Y interval overlap tests, pair emission in `(min, max)`
index order along the spatial X sweep, and cap `0x100` (256). Verified in `tools/test_sap_broadphase.c`.

`FUN_00114610` [S]: rejects type-3 vs type-3, type-5 vs type-5, and any car
pair where **both** have `veh+0x20E == 1` (asleep).

### Object types [S, from the dispatch and the class map]

`FUN_0010C550` returns "is a car" for types **0, 1, 2**.
`FUN_0010FBC0` maps type → interaction class:
`0,2 → 0`; `1 → 1`; `3 → 2`; `4 → 3 or 5` (5 when
`veh+0x242B != DAT_0073BB8C`); everything else → 6.

Two compiled tables index by that class:

* `DAT_0039AE50[a*7+b]` (u8, "can crash") =
  `row0 [1,1,1,1,1,1,0]`, `row2 [1,0,0,1,0,0,0]`, all other rows 0.
* `DAT_0039AE88[a*7+b]` (u32, interaction kind, read by `FUN_0010FC50`) =
  `row0 [0,1,0,0,1,1,0]`, `row1 [2,0,2,2,0,2,0]`, `row2 [3,…]`,
  `row3 = row0`, `row4 [2,0,2,2,0,2,2]`, `row5 [2,2,2,2,2,2,0]`, `row6 [0…]`.
  Kind **2** means "this side is immovable in the response".

---

## 2. The collision hull is real data — .bgv +0x1060 [C]

`FUN_00122830` (the collision-object init) does
`FUN_00122C20(veh+0x220, bgvfile+0x1060)` and then sets
`veh+0x208 = veh+0x220`, relinking the record's internal offsets to
pointers. `FUN_00122C20` is a fixed-shape copy of **0x600 bytes**:

```
hull record (0x600 bytes; at .bgv +0x1060, copied to veh+0x220)
  +0x000  u32[7] header; the first five are internal offsets
          (0x1C, 0xA0, 0x320, 0x480, 0x4F8) that the copier turns into
          absolute pointers at +0x00/+0x04/+0x08/+0x0C/+0x10
  +0x014  u32  = 2
  +0x018  u8   vertex count   (<= 22)
  +0x019  u8   plane count    (<= 40)
  +0x01A  u8   edge count     (<= 60)
  +0x01C  u8[40][3]   per-plane triple (unread by the contact chain)
  +0x0A0  f32[40][4]  PLANES  {n.xyz, d}; INSIDE iff dot3(n,p) <= d
  +0x320  f32[22][4]  VERTICES (car-local)
  +0x480  u16[60]     EDGES: low byte = v0, high byte = v1
  +0x4F8  u32[22][3]  per-vertex triple (unread by the contact chain)
```

Verified on the retail files: COMP/Car1 = 16 verts / 28 planes / 42 edges,
SUPR/Car1 = 16 / 28 / 42; every one of the 107 `pveh/*.bgv|btv` files carries
a well-formed record. Extract with

```bash
python3 tools/emulate_carcol.py --extract-hulls     # -> build/cars/*.hull
```

Note the hull is **not** the `+0x1D0`/`+0x1E0` box: on COMP/Car1 the hull's
nose reaches z = 2.143 against `bbmax.z` = 2.064, and 53 of 107 cars have
hull tails behind `bbmin.z`. That difference is load-bearing — see §5.

---

## 3. Narrow phase — FUN_0010A9D0 / FUN_0010ABC0 / FUN_0010AC20 [C]

`FUN_0010A9D0` (regparm3: `EDX = vehA`, `ECX = vehB`; stack `ctx`, `mode`)
fills a 0x210-byte query context:

| ctx offset | contents |
|---|---|
| `+0x000` | A hull (`vehA+0x208`) |
| `+0x030` | A frame (4×4, rows right/up/at/**pos**) |
| `+0x070` | A inverse frame (`vehA+0x70`) |
| `+0x0F0` | B hull |
| `+0x120` | B frame |
| `+0x160` | B inverse frame |
| `+0x1E0` | mode (`0` from FUN_001121F0, `1` from FUN_00113960) |
| `+0x1E4` | result flag, set to 1 on contact |
| `+0x1F0` | contact **normal** (world) |
| `+0x200` | contact **point** (world) |

`+0x60` and `+0x150` are the copies of the two frames' positions; the solver
writes the **separated** positions back into them, so
`ctx+0x60 − frameA.pos` is A's push-out displacement (and likewise for B).

`FUN_0010ABC0`: `|posA − posB|² < 0.0009` → no contact.

`FUN_0010AC20`:

1. `M_A2B = frameA · invB`, `M_B2A = frameB · invA` (FUN_000116E0, row-vector).
2. `pA_in_B = invB · posA`, `pB_in_A = invA · posB` (FUN_00013CA0, 4 lanes).
3. A's vertices → B-local via `M_A2B`; B's vertices → A-local via `M_B2A`.
4. `dir = normalize(pB_in_A)`; `FUN_0010B210` emits **both endpoints of every
   edge of A whose two vertices satisfy `dot3(dir, vert_local) >= 0`** — the
   support-side edge soup — taking the points from the B-local vertex array.
5. `FUN_0010C0D0` clips that segment list against **all** of B's planes
   (`FUN_0010C220` per plane, ping-ponging two 120-vec4 scratch buffers at
   `DAT_005A53C0`/`DAT_005A5B40`), then copies the survivors out, dropping
   duplicates component-wise at `1e-7` (`0x0039AACC`).
6. Symmetrically for B against A's planes, appended after A's points.
   `total <= 1` → no contact.
7. Group 1 (B-local) is lifted to world by `frameB`, group 2 (A-local) by
   `frameA` (`FUN_00038C00`).
8. **Contact point** = centre of the AABB of all points (`FUN_0010B310`,
   `(min+max)*0.5`, all four lanes).
9. **Contact normal** = `normalize(rot(frameA, planeA[iA]) −
   rot(frameB, planeB[iB]))` where `iA`/`iB` are the planes with the smallest
   `|dot3(n, cp_local) − d|` (`FUN_0010C000`). `|n|² < 2.3283064e-10`
   (`0x003B191C`) → no contact.
10. **Penetration**: `FUN_0010BE70` shoots the segment
    `cp_local → cp_local + centre_to_centre*100` (`0x003A2928`) through the
    hull's planes and returns the **last plane crossed**; then
    `pen = plane.d − dot3(cp_local, plane.n)`, clamped at 0, and the body's
    position is moved by `rot(frame, plane.n * −pen)`. Each body is separated
    along **its own** exit plane. `ctx+0x1E4 = 1`.

Verified: 8 poses (side-by-side, nose-to-tail, angled, T-bone with two
different cars, deep overlap, rear-into-nose, pitched, and a no-contact case)
— point, normal and both displacements match to 1e-4, and the miss case
misses. 36 asserts.

---

## 4. Racer vs racer — FUN_001121F0 [C]

`thiscall`-ish, `[EBP+8] = pair`. `A = pair+0x24 → veh`, `B = pair+0x28 → veh`.
`FUN_00111CD0` only routes here when **both** cars have `veh+0x210 == 0`
(RE_NOTES §14: `+0x210` set selects the crashed/simplified path).

1. `FUN_0010A9D0(vehA, vehB, ctx, mode = 0)`; `pair+0x2C = hit`. No hit → out.
2. `vehA+0x211 = vehB+0x211 = 1` ("touching a car this frame").
3. **Separation** (only when `ctx+0x1E4`):
   `dA = ctxPosA − posA`, `dB = ctxPosB − posB`, **both `.y` zeroed** — car
   separation is purely horizontal. `D = dA − dB`, `w = mA/(mA+mB)`:
   * `vehB+0x212` set → `vehA+0x130 += D`
   * else `vehA+0x212` set → `vehB+0x130 += −D`
   * else `vehA+0x130 += D·(1−w)` and `vehB+0x130 += D·(−w)` — the heavier
     car moves less.
4. **Contact point fix-up**: `pair+0x00 = ctx contact point` but with
   `y := posA.y * 2.0 * 0.5 + 0.1` (`0x003B1688`, `0x003B1684`,
   `0x003EBE40`), i.e. the frame-origin height plus 10 cm.
   `pair+0x10 = normal` with `.y := 0`. Both cars get `veh+0x150 = pair+0x00`.
5. **Longitudinal contact parameters**
   `tA = clamp( dot3(cp − (posA + atA·bbminA.z), atA·(bbmaxA.z − bbminA.z))
                / |atA·(bbmaxA.z − bbminA.z)|², 0, 1 )`, and `tB` likewise
   (`veh+0x1D8` = bbmax.z, `veh+0x1E8` = bbmin.z). 0 = tail plane,
   1 = nose plane.
6. **Relative velocity** `vrel = vp(B, cp) − vp(A, cp)` (`FUN_001066A0` =
   `vel + ω × (cp − pos)`), `vn_mph = |dot3(vrel, n)| · 2.23693633`
   (`0x0038994C`, the TRUE mph constant, not the physics 2.2374146).
7. **Mutual impulse** `FUN_0010F8D0(EAX = vehB, ECX = vehA, ptA, ptB, vrel,
   n, e = 0.1 [0x003EBE3C], out)`:
   ```
   j   = | −(1+e)·dot(n, vrel)
           / ( 1/mA + 1/mB + dot(n, cross(IinvA·(rA×n), rA)
                                  + cross(IinvB·(rB×n), rB)) ) |
   out = n · (−j)
   ```
   Returned in XMM0. If `j > 0`: **`vehA+0x110 += out`, `vehB+0x110 −= out`**
   — a **pure linear** impulse (no `FUN_00106500`, so no angular part).
8. **The shove** — this is the shunt. Each car gets a FORCE at the contact
   point:
   ```
   fA = n · min(mB, 2000) · (−20.0) · (|vehB+0x1408| + 1)
   fB = n · min(mA, 2000) · (+20.0) · (|vehA+0x1408| + 1)
   ```
   (`0x003EBE70` = 2000, `0x0041A4D0` = 20). Each is added to `veh+0xF0`
   **and then** routed through `FUN_001205E0`, which adds it to `+0xF0`
   *again* and, when it does not take the linear-only branch, also adds the
   torque `(cp − pos) × f` to `+0x100`. **The linear component is therefore
   applied twice and the torque once** — reproduced verbatim, and the
   differential cases confirm it. `FUN_001205E0`'s routing:
   drifting (`veh+0x1524 ∈ {1,2}`) → linear only; else if `|ω.y| > 2.0`
   (`0x003EBF68`) and `sign(ω.y) == sign(torque.y)` → linear only; else
   force + torque at the point (`FUN_001064B0`).
   The torque is what yaws the victim out of line.
9. **Impact magnitude** `pair+0x20 = (mA + mB) · vn_mph · 0.1 · 0.5`
   (`0x003EBE74`, `0x003B1684`).
10. **Crash trigger**: `vn_mph > 150` (`0x003EBE4C`) → `FUN_0010DCA0` for A
    if `DAT_0039AE50[clsA][clsB]`, and for B if `DAT_0039AE50[clsB][clsA]`;
    otherwise the game-context virtual `+0x64` is called as
    `(1, vehA, vehB, 1.0)` — a "rub". **Either way execution continues into
    the slam classification.**

---

## 5. Slam classification — the takedown entry [C]

Still inside `FUN_001121F0`. `lat = dot3(frameB.row0, posB − posA)`
(which side of B the contact is on), `spA/spB = veh+0xBC`, and
`eps = 1.52587891e-05` (`0x00384208`).

**Rear-end** (`|1 − tA| <= eps` and `|tB| <= eps`, i.e. the contact clamps to
A's nose plane and B's tail plane):

```
require vn_mph > 20            [0x003EBE60]
require spA > spB
s = min((vn_mph − 20) / 50, 1) [0x003EBE68]
light = (0.3 >= s)             [0x003EBE80];  if light and s > 0: s /= 0.3
report vtable+0x64( light ? 4 : 6, attacker = A, victim = B, s )
if not light also FUN_00141700(vehA, ..., vehB)   (sound cue)
on a true return: pair+0x2D = 2
                  victimB+0x153C = (0 > lat);  if light attackerA+0x153C = !that
```

The mirrored branch (`|1 − tB| <= eps` and `|tA| <= eps`) requires
`spB > spA` and reports with **attacker = B**.

**Side** (everything else): `ang` = heading difference of the two flattened
forward axes (`FUN_000FF160`, degrees, computed with `rsqrtss` so it is
approximate by construction).

```
hard   = vn_mph > 35                                   [0x0041A4C4]
hard  |= vn_mph > 20 and |ang| > 40                    [0x0041A4C8 / 0x0041A4CC]
thresh = (35 − 20)/40 · ang + 20
require vn_mph > thresh or hard
s = min((vn_mph − 30) / 20, 1)                         [0x003EBE5C / 0x003EBE64]
light = (0.3 >= s);  if light and s > 0: s /= 0.3
attacker = (tA > tB) ? (spB − spA > 17.8816 ? B : A)   [0x003B1B68 = 40 mph]
                     : (spA − spB > 17.8816 ? A : B)
report vtable+0x64( light ? 3 : 5, attacker, victim, s )
on a true return: pair+0x2D = 1; victim+0x153C = side; if light attacker gets !side
```

The virtual at game-context `+0x64` is the entry to the slam/BP chain
(`FUN_001989A0`, docs/RE_GAMEPLAY.md §6): it awards the attacker Slam BP and
boost and takes the same base off the victim, stamps `racecar+0x1598`
(slam timestamp) and `+0x16BC/+0x16C0` (aggressor + time), and returns
whether the slam counted. Those stamps are what drive the already-verified
steer-away envelope (`FUN_0011ECF0`) and the AI out-of-control authority
`0.1` (`FUN_00105340`) — i.e. **the physical shunt of §4.8 and the loss of
steering control are two separate mechanisms and both start here.**

**Note on reachability [C]:** the rear-end branch needs *exact* clamps on both
parameters, which only happens when the attacker's hull nose overhangs its
`+0x1D0` box and the victim's hull tail overhangs its `+0x1E0` box. 57 of 107
cars have a front overhang and 53 a rear one; COMP/Car4 (+0.197 front) into
SUPR/Car10 (−0.373 rear) reaches it, and that pair is in the suite as the
type-4/type-6 cases. COMP/Car1 into SUPR/Car10 never does (tA peaks at
0.9953) and classifies as a *side* slam instead. This is the retail
behaviour, not an artefact of the port.

---

## 6. Car vs crashed car — FUN_00113960 [C]

`FUN_00111CD0` orders the pair so **A is the un-crashed car**, then calls
this with `(param_1, pair)`, `RET 8`.

* `kindA/kindB = DAT_0039AE88[clsX][clsY]` (`FUN_0010FC50`); both 2 → return.
  A car with `veh+0x210 == 0` is then **forced to kind 2** (immovable).
* `DAT_004A52B3` (retail value **0**) selects an OBB-vs-OBB path
  (`FUN_00108EF0`); with 0 the code takes `FUN_0010A9D0(..., mode = 1)` —
  the same convex-hull narrow phase.
* `vehA+0x211 = vehB+0x211 = 1`.
* Separation, **not** y-zeroed: `D = dA − dB`;
  `kindA == 2` → `vehB+0x130 += (dB − dA)`;
  `kindB == 2` → `vehA+0x130 += D`;
  else mass split `D·(1−w)` / `D·(−w)` with `w = mA/(mA+mB)`.
  (`FUN_00114F30` refines this when `vehA+0x212` is set — mapped, **not
  ported [?]**; the suite's cases keep `+0x212` clear, where it is a no-op.)
* The normal is **blended toward the relative-velocity direction**:
  `n = normalize(n + (−0.9)·normalize(vrel))` (`0x0041A4C0`) unless `vrel` is
  degenerate (`FUN_0003B060`).
* `FUN_0010F8D0` with `e = DAT_004A1D98` (**0.0**), applied through
  `FUN_00106500` — **linear *and* angular** at the contact point, unlike the
  racer path: `kindA == 2` → only B, with the impulse scaled by
  `DAT_003B16C0 = −1`; `kindB == 2` → only A; else A gets `+imp`, B `−imp`.
* `pair+0x20 = j` (the impulse magnitude, or 0).
* Crash trigger: `j > 5000` (`0x003EBE50`), or `> 2500` (`0x003EBE54`) when a
  **traffic (type 2)** car hits an un-crashed non-traffic car; gated by
  `DAT_0039AE50` and by a recent-slam window of **1.5 s** (`0x003EBE7C`)
  measured from `racecarA+0x10DC − racecarB+0x140C` (or the attribution stamp
  `racecarA+0x15A8[victim slot]`).

---

## 7. What the staging soup does — correction to RE_NOTES §15

`FUN_00122D00` (per-vehicle poly staging into `veh+0x200`) does **not**
carry live opponents' hulls in the normal racing case:

* the 4- or 6-plane blocks appended from **`veh+0x11D0`** (`puVar6` walks
  from `veh+0x11F0` with negative displacements; RE_NOTES §15's
  "from veh+0x11f0" is off by the record head) are stamped **0x26** and gated
  by bytes `+0x1350` / `+0x1351`;
* the other-car hull sets at **`veh+0x1590`** (count `+0x3A50`) and
  **`veh+0x2010`** (count `+0x3A58`) are stamped **0x21** and are only staged
  when `byte veh+0x215 == 1` — a crash-mode state.

So in normal racing, car-vs-car does **not** go through the chassis response
`FUN_0011AEF0` at all: it is the separate manager pass above.
Surface types are ASCII (`0x20 = ' '`, `0x21 = '!'`, `0x26 = '&'`), which is
why the gather callback's skip set is `0x20/0x22/0x23/0x24`.

---

## 8. Constants (all read out of `build/burnout3.elf`)

| address | value | use |
|---|---|---|
| `0x0038994C` | 2.23693633 | m/s → mph for the impact/slam tests |
| `0x003EBE3C` | 0.1 | restitution, racer vs racer |
| `0x004A1D98` | 0.0 | restitution, car vs wreck |
| `0x0041A4D0` | 20.0 | shove force coefficient |
| `0x003EBE70` | 2000.0 | shove mass clamp |
| `0x003EBE74` | 0.1 | impact scale (× 0.5 from `0x003B1684`) |
| `0x003EBE4C` | 150.0 | crash closing speed (mph) |
| `0x003EBE60` | 20.0 | rear-end minimum (mph) |
| `0x003EBE68` | 50.0 | rear-end strength range |
| `0x003EBE5C` | 30.0 | side minimum (mph) |
| `0x003EBE64` | 20.0 | side strength range |
| `0x003EBE80` | 0.3 | light/heavy slam split |
| `0x0041A4C4/C8/CC` | 35 / 20 / 40 | side gate: hi mph, lo mph, angle |
| `0x003B1B68` | 17.8815994 | attacker Δspeed (= 40 mph) |
| `0x003EBE50` | 5000.0 | wreck-path crash impulse |
| `0x003EBE54` | 2500.0 | …when traffic hits a racer |
| `0x003EBE7C` | 1.5 | wreck-path re-crash wait (s) |
| `0x0041A4C0` | −0.9 | normal ↔ vrel blend (wreck path) |
| `0x003B16C0` | −1.0 | immovable-side impulse scale |
| `0x00384208` | 1.52587891e-05 | rear-end clamp epsilon |
| `0x003EBF68` | 2.0 | `FUN_001205E0` yaw-lock rate |
| `0x003B191C` | 2.3283064e-10 | degenerate-vector epsilon |
| `0x0039AACC` | 1e-07 | clip-point dedup |
| `0x003A2928` | 100.0 | penetration ray length |
| `0x003B188C` | 1e-04 | ray parallel-plane epsilon |
| `0x003EBE40` | 0.1 | contact-point y bias |
| `0x004A52B3` | 0 | OBB-path selector (off in retail) |

---

## 9. Ported + verified

`src/burnout3_carcol.c` / `.h`:

| C entry point | real function | mark |
|---|---|---|
| `b3_carcol_hull_load` / `_from_record` | `FUN_00122C20` layout | [C] |
| `b3_carcol_world_aabb` | `FUN_00114270` | [C] |
| `b3_carcol_aabb_overlap` / `_broadphase` | `FUN_00110AF0` predicate | [C] |
| `b3_carcol_contact` | `FUN_0010A9D0` → `FUN_0010AC20` + 9 helpers | [C] |
| `b3_carcol_point_velocity` | `FUN_001066A0` | [C] |
| `b3_carcol_mutual_impulse` | `FUN_0010F8D0` | [C] |
| `b3_carcol_apply_force` | `FUN_001205E0` (+`FUN_001064B0`/`FUN_00106590`) | [C] |
| `b3_carcol_resolve_alive` | `FUN_001121F0` | [C] |
| `b3_carcol_resolve_wreck` | `FUN_00113960` | [C] |
| `b3_carcol_resolve` | `FUN_00111CD0`'s car/car ordering + dispatch | [C] |
| `b3_carcol_hull_from_extents` | — | **GLUE** fallback |

`tools/validate_carcol.py` sections and counts:

| section | function(s) | asserts |
|---|---|---|
| narrow phase | `FUN_0010A9D0`/`FUN_0010AC20` | 36 |
| broad phase | `FUN_00114270` | 6 |
| impulse | `FUN_0010F8D0` | 4 |
| force routing | `FUN_001205E0` | 8 |
| racer vs racer | `FUN_001121F0` | 338 |
| car vs wreck | `FUN_00113960` | 338 |
| **total** | | **730** |

The response sections cover: rub (type 1), light and full side slams
(3 / 5), light and full rear-end slams (4 / 6) with the attacker on both
sides, the `> 150 mph` double-crash trigger, `+0x212` on either car, a
drifting attacker, a heavy-vs-light mass split, and the wreck path's 5000
threshold. The vtable `+0x64` arguments and the `FUN_0010DCA0` calls are
captured with Unicorn code hooks, so the classification is checked against the
real calls and not just against the port's own bookkeeping.

## 10. Open [?] / not ported

* `FUN_00114F30` — the `veh+0x212` refinement of the wreck-path separation
  (projects the displacement onto `veh+0x170`). Mapped, unported.
* `FUN_00112E70` (car vs type-3 object) and `FUN_00113890` (type-5). Type 3
  is something a car can land on — the `veh+0x211` "landed on a car" tail in
  `FUN_0011AEF0` belongs to that family.
* `FUN_000FF160`'s exact `rsqrtss` result: the side-impact angle is computed
  with the approximate reciprocal-square-root instruction, so that gate is
  hardware-approximate by construction. The port uses exact `acos`; the
  differential cases pass because they sit well away from the threshold.
* The `veh+0x2424` "linked vehicle" branches (articulated trucks) in both
  response functions: mapped, not ported (no articulated vehicle is in play).
* `DAT_004A52B3` — who sets it, and therefore when the OBB path
  (`FUN_00108EF0`) is ever taken. Retail default 0.
* The producer side of the slam report (what the game-context virtual `+0x64`
  does before `FUN_001989A0`) — RE_GAMEPLAY §8 already lists the slam boost
  transfer as [S].

---

## Driving through traffic: two independent causes

**C-1 — a WRECKED traffic car was a ghost.** [C] `FUN_00113960` @0x00113B75
forces the UN-crashed car to kind 2 = immovable, so a racer hitting a wreck
receives exactly zero and 100% of the impulse and separation go to the wreck.
Retail can do that because a crashed car keeps running its own solver, whose
integrator drains those accumulators and shoves the wreck aside. The port
reproduced the resolve faithfully (`burnout3_carcol.c:712`) but nothing
consumed the wreck's accumulators — `carcol_pass()`'s integrate loop
(`full.c:11086`), `traffic_update()` (`full.c:7690`) and the pose path all skip
`t->crashed_until > g_race_time`, while `traffic_render()` gates on `t->active`
ALONE (`full.c:8261`) and still draws it. Measured, COMP/Car1 at 45 m/s into a
wrecked HEVYCAR11: racer `imp_force (0,0,0)`, `dv = 0.0000 m/s`, while the
wreck took `(842, 5778, 17218)`. The same pair alive gives the racer
`dv = -18.0 m/s`. It compounds: a wreck is a stationary roadblock for 5 s, and
every car that piles into it becomes another drive-through body.

Not the cause, ruled out with evidence: the entity list does NOT hold copies —
`carcol_fill_racer` sets `b->rb = &v->fsim.rb` and `carcol_fill_traffic` sets
`b->rb = &t->rb` (`full.c:9761`, `:10012`). The write-back was never the bug.

**C-2 — the traffic broadphase box was LITERALS, not the hull.** The racer path
takes its box from the .bgv body box (v+0x1D0/+0x1E0); traffic hardcoded
`x = +-1.0`, `y = [-0.2, 1.2]` and took only Z from the mesh length. Against
the actual hulls that is badly undersized for anything larger than a small car:

| car | hull box (x, y) | old literal |
|---|---|---|
| HEVY_Car34 | x +-1.30, y -> **2.95** | y -> 1.20 |
| HEVY_Car24 (trailer) | x +-1.23, y -> **3.69** | y -> 1.20 |
| HEVY_Car23 (tractor) | x +-1.32, y -> 2.45 | y -> 1.20 |
| COMP_Car12 | x +-1.08 | x +-1.00 |

Trucks and trailers had 60-70% of their height outside the box, so the pair
never reached the narrow phase and the car drove straight through. The box is
now derived from the hull vertices (cached per class, with the old literals as
the fallback when a hull has no verts, so it cannot collapse to a point).

**Measured in game**, 150 s autodrive, same pair budget: player-vs-traffic
contacts **17-22 -> 69-74**, a 3.4x increase. `validate_carcol` 1037/1037
(+25 cases covering the kind-2 contract and one real integrator frame).

**C-3 - a live traffic car was routed through the WRONG RESOLVE.** [C]
This is what "live traffic never gets shoved" actually was, and the answer is
not a reconciliation law. See the next section.

---

## Live traffic is not a vehicle - the TYPE-3 arm, FUN_00112E70 [C]

The traffic spawn `FUN_001A2B20` registers every traffic car (and every
trailer and towed partner) through **`FUN_00111620`** @0x001A2DB0 / 0x001A2EEA
/ 0x001A3088, and that function writes:

```
obj+0x00 = 3                             <- the TYPE BYTE
obj+0x04 = trafficrec + 0x70             <- its own 4x4, not a frame object
obj+0x08 = *(trafficrec+0xB0) + 0xE80    <- the MODEL's box
obj+0x0C = trafficrec                    <- NOT a vehicle record
```

There is no vehicle record behind a live traffic car, so there is **no rigid
body, no +0xF0/+0x100/+0x110/+0x120/+0x130 accumulators and nothing to
integrate.** All it carries is a 4x4 at `+0x70` and a scalar speed at `+0xC4`.
`FUN_0010FB70` hands the narrow phase the model's own hull at `model+0x1060`.

`FUN_00111CD0` @0x00111D6E routes a pair with **exactly one type-3 handle** to
`FUN_00112E70` (other side 0/1/2/4) or `FUN_001135E0` (6/7), ordering the pair
so the CAR is A - never to `FUN_001121F0`. And `FUN_00114610` @0x00114613
rejects a **type-3 vs type-3** pair outright: retail never resolves live
traffic against live traffic at all.

### FUN_00112E70, in order

| # | addr | what |
|---|---|---|
| 1 | `0x00112EC4` | A is a car and `veh+0x1353 & 2` -> return |
| 2 | `0x00112ED1` | `veh+0x210 == 0` arms the RUB, but only if `abs(posA.y - posB.y) <= 2.0` [`0x003B1688`]; a crashed car can only reach the crash arm |
| 3 | `0x00113005` | `FUN_00040AE0` inverts B's frame on the stack; `FUN_0010FB70` supplies B's hull |
| 4 | `0x0011302B` | the CONVEX HULL narrow phase runs - **only** to set `crashable = hit && DAT_0039AE50[classB*7 + classA]`. Its point, normal and separation are discarded |
| 5 | `0x00113069` | a type-3 record with `+0x174 & 8` is never crashable |
| 6 | `0x0011308E` | the RESPONSE narrow phase is a **2-D CAPSULE** test in (x,z): each body's axis runs `at*(bbmin.z + bbmax.x)` -> `at*(bbmax.z - bbmax.x)` about its origin, radius `bbmax.x`. `FUN_0010FCE0` returns the closest points and the distance |
| 7 | `0x001131B6` | `gap = dist - (B.bbmax.x + A.bbmax.x)`; `gap > 0` -> return |
| 8 | `0x00113245` | `n = normalize2(pA - pB)`, y = 0. `pair+0x10 = n`, `pair+0x2C = 1` |
| 9 | `0x0011329A` | trigger, gated on `crashable && !(veh+0x1353 & 0x10) && veh+0x152C < 0`: `vrel = B.at*(rec+0xC4) - A.at*(veh+0xBC)` (no angular term on either side), `vn = abs(dot3(vrel,n)) * 2.2369363`, `pair+0x20 = mass_A * 2.0 * vn * 0.1 * 0.5` |
| 10 | `0x00113386` | `FUN_00017310` (game mode 6, or sub-mode 3/4/5) picks `abs(vrel) vs authority*20` [`0x003EBE48`] over `vn vs authority*75` [`0x003EBE44`]; over the bar the RUB flag is cleared @0x001133E4 |
| 11 | `0x001133E9` | `veh+0x212` set and `veh+0x215 != 3` -> straight to the crash arm |
| 12 | `0x0011340C` | **THE RUB.** `vehA+0x130 += n * pen` - **100 % of the penetration to the CAR, no mass split.** Contact point = `pA - A.bbmax.x*n` with `y = frameA.pos.y + 0.1` [`0x003EBE40`]; force = `n * min(mass, 2000) * 100.0` [`0x003EBE6C`] through `FUN_001205E0`; then `gamectx->vt[0x54](vehA, trafficrec)`; `pair+0x00 = cp`. **Nothing at all is written to the traffic car.** |
| 13 | `0x00113522` | **THE CRASH.** `pair+0x2C = 1`, `vehA+0x211 = 1`, `FUN_0010DCA0` crashes the car, then `FUN_00114910` and `FUN_00113960` |

### FUN_00114910 - the promotion [C-disasm]

`*param_1 = 4`: the type byte becomes **4**. A real **0x2430-byte vehicle
record** is taken from the collision world's own pool (`world+0x33780`, 64
slots, live array `world+0xE6B80` counted at `world+0xE6C8C`) and
`FUN_00120BA0` seeds it from the traffic record: the frame from `rec+0x70`
(into `veh+0x204`, inverse at `veh+0x70`), `veh+0x2420 = rec+0x173`,
`veh+0x242A = rec+0x176`, mass/inertia from the car config at
`0x479560 + rec[0x176]*0xB0`, **`veh+0xBC = rec+0xC4`** @0x00120DDD and
**`veh+0xB0..0xB8 = frame.at * speed`** @0x00120E20/0x00120E2E/0x00120E3C, and
`veh+0x242B = DAT_0073BB8C` @0x00120E44 - exactly the test `FUN_0010FBC0` uses
to give type 4 class **3**, "the designated big-hit traffic vehicle". The
handle is relinked (`+0x04 = newveh+0x204`, `+0x08 = newveh+0x1D0`,
`+0x0C = newveh`), a trailer (`rec+0x110`) and a tractor (`rec+0x10C`) are
promoted recursively and cross-linked through `+0x2424/+0x2428`, and then the
traffic-manager slot is freed (`FUN_001A3970`) and **its lane cursor
destroyed** - `rec+0x114` reset to 0xFF/defaults @0x00114BAC.. and cleared.

`FUN_00113960` is then re-run over the SAME pair @0x001135CF. Neither
`FUN_0010DCA0` nor `FUN_0010E580` writes `veh+0x210` (both are 37/46
-instruction dispatchers), so the car is still un-crashed and `FUN_00113960`
@0x00113B75 forces it to kind 2 - IMMOVABLE. The freshly promoted car
therefore takes **100 % of the separation and 100 % of the impulse.** That is
the launch.

### So: retail never reconciles a shoved traffic car with its lane

Below the bar the traffic car is not shoved - the CAR is pushed out instead,
by the whole penetration, which is why one frame clears the overlap. Above the
bar the lane cursor ceases to exist. There is no blend-back law and no traffic
integrator; the only state machine is
**path follower (type 3) -> one-way promotion -> vehicle (type 4)**.

Measured under Unicorn (COMP/Car1, 1200 kg, into a HEVY/Car11 traffic car):

| closing | outcome | racer | traffic car |
|---|---|---|---|
| 20.6 mph | rub | deflection 0.4977 m, force 120 kN at the contact | nothing |
| 111.8 mph | crash + promote | crashes, kind 2, receives nothing | impulse (2766, 1986, 28348), deflection 0.456 m |

### What the port does with that

`b3_carcol_resolve_traffic()` (`src/burnout3_carcol.c`) is FUN_00112E70's live
arm; `b3_carcol_seg_closest2d()` is FUN_0010FCE0. In this harness one
`TrafficCar` covers both retail states, so **`traffic && !crashed` is retail's
type-3 handle** and **`traffic && crashed` is retail's promoted type-4
vehicle** (which keeps the FUN_00113960 arm it already had, and the
`carcol_pass()` wreck integrator that consumes its accumulators). The pair
dispatch in `carcol_pass()` routes accordingly and applies FUN_00114610's
type-3-vs-type-3 rejection to two live traffic bodies.

Acceptance: `tools/validate_carcol.py` **1296/1296** - +259 cases over the
previous 1037, every one a field-for-field diff against `FUN_00112E70` /
`FUN_0010FCE0` executed under Unicorn (`tools/emulate_carcol.py`:
`Session.seed_traffic` / `resolve_traffic` / `seg_closest2d`, with
`FUN_00114910` hooked so the promotion is performed on the harness's own
record and `FUN_00113960` then runs for real over it).

Two things in the recovered code a clean-room version would not produce, and
the port reproduces anyway:

* `FUN_0010FCE0`'s degeneracy test @0x0010FD5F is
  `abs(dot(d1,d2)) <= 1.5258789e-05` - it rejects **orthogonal** segments, not
  parallel ones, and returns the sentinel `1000.0` [`0x003B16CC`] leaving
  every output untouched;
* it only ever evaluates the four endpoint-onto-the-other-segment
  projections, so two segments that genuinely cross report a non-zero
  distance.

**Still open [?]:** retail also routes **type-4 vs type-3** (a promoted wreck
into a live traffic car) through `FUN_00112E70` - that is its pile-up cascade.
The port leaves wreck-vs-traffic on the `FUN_00113960` arm it already had,
because whether the promoted car's `veh+0x210` is set by the time that pair is
tested is not established, and that is what decides whether the cascade takes
the rub or goes straight to the crash.
