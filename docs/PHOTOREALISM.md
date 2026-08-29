# The photorealism layer — seven effects and one option, all INSPIRED

**Nothing in this document is a claim about Burnout 3.**

Every effect described here is a modern construction that does **not exist in
the retail image**. That is not a gap in the reverse engineering; it is the
point. The Xbox had no depth texture to read, no second geometry pass to spare
and no shader budget for any of it, and a sweep of the renderer range
`0x00028000..0x00045000` finds no ambient-occlusion, shadow-map, reflection or
light-shaft machinery of any kind. An `INSPIRED` line is not evidence and must
never be cited as game behaviour — see the EVIDENCE MARKS block at the top of
`src/burnout3_aftereffects.h`.

The mandate, in one line: **retail's look is the floor, not the target.** The
default the game boots into is meant to go beyond the 2004 image. The
retail-faithful look stays exactly one environment variable away, and the
recovered-law suites go on pinning it.

---

## 0. What is retail, and is not touched

| Recovered | Where | Status |
|---|---|---|
| the x2 present composite (`SHIFTLEFTBY1`) | `FUN_0003DA90` / D3DPIXELSHADERDEF `0x003E9EA8` | unchanged, still `uExposure` |
| `C0.a = min(s, 2) * 0.5` | `0x0003DC42..0x0003DC62` | unchanged |
| the output gamma ramp `round((i/255)^0.95*255)` | `FUN_0003C8A0` | unchanged, still the last pass |
| the world program's linear fog | `FUN_00038D10`, the `MIN oFog` instruction | unchanged, **augmented** not replaced |
| the sky dome, depth writes off | `FUN_00032580` / `FUN_000323D0` | unchanged |
| the car's blob shadow | `FUN_0019A7C0` / `FUN_00043570` | unchanged, and the reason cars are **not** shadow casters |

The layer sits **between** the finished scene and that recovered ending: it
changes what the composite is handed, never what the composite is. The single
exception is the filmic curve, which replaces the composite's raw clamp — see
tier 1, and `B3_PHOTO_TONEMAP=0` restores the recovered clamp so the `[C]`
equation stays runnable rather than merely written down.

---

## 1. The architecture: one deferred pass, one geometry pass

Six of the seven effects read the scene's **depth**. Once the depth is a texture,
the cheapest place to put a light — and the only place that treats the track,
the scenery, the props, the cars and the traffic **identically** — is a
screen-space pass over the finished frame.

So the wave adds exactly **one** geometry pass to the engine (the sun's
depth-only shadow render, `b3r_shadow_begin` in `src/burnout3_render.c`) and
does everything else in `src/burnout3_aftereffects.c`, reading a depth texture
and writing a colour one.

That is worth stating as a property rather than as an implementation note:
**`burnout3_render.c`'s one world program (`B3R_VS` / `B3R_FS`) is not modified
by this wave at all.** It compiles the same source it compiled before, which is
why the "`B3_PHOTO=0` is bit-identical" gate is a fact about the source rather
than a hope about floating point.

The frame becomes:

```
b3_afx_frame_begin()      scene FBO bound and cleared
  b3r_shadow_begin()      <- the ONE new geometry pass, from the sun
    track + props + scenery, depth only, colour mask closed
  b3r_shadow_end()
  b3_afx_rebind_scene()   the scene target back, NOT cleared
  ... sky, world, cars, traffic, exactly as before ...
b3_afx_scene_done()
  [SSAO half-res + bilateral blur]
  [the DEFERRED pass: occlusion, sun shadow, per-source lights,
   reflection, haze -> T_LIT]
  downsample -> radial blur -> bloom            (all now read T_LIT)
  [god rays, quarter-res, off the prefilter]
  composite  + [filmic curve + authored grade]  <- the recovered x2 lives here
  ... the HUD draws into the UI target ...
b3_afx_frame_end()        the recovered gamma ramp
```

**Tier 7 adds no pass at all.** Its light field is derived once when the track
loads, from data the scenery loader already has in its hands, and per frame it
is a nearest-N pick and two uniform arrays inside the deferred pass that was
already running.

**The HUD is drawn after the whole layer** and is therefore never processed by
it. `tools/validate_photo.py` section 4 checks that as a structural claim.

**Everything works in world space**, reconstructed from depth by the shared
`b3W()` helper. World space is the one frame in which the shadow matrix, the
sun vector, the height fog and the AO radius are all expressible without a
second sign convention to get wrong. The caller hands in the inverse of the
**same** view-projection the world was drawn with — read back off the
renderer's own matrix stack, display mirror included — so there is no
handedness arithmetic anywhere in the layer.

**The shader source is assembled, not branched.** An effect that is off is not
in the shader. That buys two things: a frame with three effects live does not
pay for the three that are not, at all; and with `B3_PHOTO=0` the composite
string handed to the driver is **byte-identical** to the pre-wave one.

---

## 2. The seven

### Tier 1 — filmic tonemap + authored grade

The recovered ending is `clamp(2 * scene)` followed by the `[C]` gamma ramp,
and the x2 is a register-combiner shift: an operation with no shoulder, so
every render-target value above 0.5 lands on 255 and stays there. On the pinned
`US_C3_V1` frame that is 0.92% of the frame.

The layer applies the ACES filmic approximation **in linear space** — the scene
target is display-referred, so the value is squared on the way in and
square-rooted on the way out. Getting that wrong is not subtle: the first cut
fed display-referred values to the fit and measured **+37 levels of frame
mean**, which is not a tonemap, it is a wash.

The grade is a 16×16×16 cube **generated at build-all time** from six constants
and uploaded as one 256×16 texture (ESSL 1.00 has no 3-D sampler; two 2-D
fetches and a mix is what every engine that has shipped a LUT on GLES does).
There is no asset — it is reproducible from the header alone. It has a point of
view: cool shadows, warm highlights, more contrast and saturation.

Measured, all six on, pinned frame: **frame mean −0.25 levels** (the exposure
intent survives), **clipping 0.92% → 0.23%**, **saturation 34.5 → 42.4**.

### Tier 2 — SSAO

Half resolution, a twelve-sample spiral over the hemisphere the reconstructed
normal points into, rotated per pixel, then a depth-aware separable blur and a
bilateral upsample in the deferred pass.

Two things were measured and are worth recording because both produced an
effect that ran and did nothing:

* the **raw kernel mean is not a usable signal**. A pixel a human would call
  "in a corner" measures about 0.15, which after a contrast power is 0.06 —
  invisible. `B3_PHOTO_AO_GAIN` maps what the geometry produces onto what the
  eye wants.
* a **hard range cut at the radius** threw away almost every tap. The
  screen-space tap circle is wildly anisotropic in world space down a road, so
  most taps land far away; with the hard cut the occlusion buffer on the pinned
  frame was white everywhere except the car's own bumper. The taper now runs to
  twice the radius.

The normal comes from four depth taps taking the **closer** neighbour on each
axis. Averaging both is one line shorter and produces a normal that leans over
every silhouette — the bright halo around every car.

**Tier 2b — the near-field blanket, and the three things that made it.** A
player reported the whole near-field road as a uniform dark blanket with a
straight edge at mid-distance beyond which the world was sunlit, travelling
with the camera. Reproduced at the conditions it was reported at — `US_C1_V1`,
frame 655, **2048×1536, the size the game boots into** — and attributed by
measurement: of the 39.9 levels the whole layer takes off the near-field road,
**SSAO owns 38**; the sun shadow owns 2.6.

The measurement that settled it was not "how dark is the road". An occlusion
term is allowed to darken a road it can see a reason to darken. It is whether
the pass can tell an open road from a real contact **at all**: open flat road
measured **−42.62 levels** and a genuine wheel-to-road contact **−42.82**. The
same number. That is what a blanket *is*, and no gain or strength can separate
them afterwards.

* **the pass was handed the wrong texel.** It rasterises at HALF resolution,
  and every use of `uTexel` in it is a step to a neighbouring pixel of *its
  own* grid — the four depth taps the normal is reconstructed from, and the
  floor under the tap radius. It was given the scene's full-resolution texel,
  so from a half-res texel centre the step landed *inside the same half-res
  pixel*; on a surface whose depth barely changes across a screen row — a road,
  which is most of a racing frame — `ex` collapsed and `cross(ex, ey)` came out
  wherever the noise pointed. Measured over the road band 6–40 m out, on the
  grid the pass actually runs on: the normal was UP (`N.y > 0.95`) on **47.2%**
  of it, **median `N.y` 0.031 — horizontal, on flat road**. With the pass's own
  texel: **64.1%, median 0.991**. A horizontal normal turns the sampling
  hemisphere on its side, so every tap up or down the road scores near-full
  occlusion and the whole near field goes dark at once.
* **the range taper measured a radius the sampled disc no longer had.** `sr` is
  a screen radius and its cap is not cosmetic: at this FOV a 4 m world radius
  projected from three metres in front of the camera is most of the frame, so
  across the near field the cap binds and every pixel samples the *same fixed
  screen disc* — while the taper went on measuring each sample against
  `B3_PHOTO_AO_RADIUS` metres. What that reports is how many of the twelve
  fixed screen offsets happen to land within eight metres, which down a road is
  a function of **distance and nothing else**: a flat grey stepping in
  horizontal bands. The taper now uses the radius the sampled disc covers.
* **there was no angle bias.** `dot(N, dv)/l` is the sine of a sample's
  elevation above the tangent plane and is zero on a flat surface; `max(0, …)`
  on zero plus a couple of degrees of reconstruction error is not zero, it is
  the positive half, and `B3_PHOTO_AO_GAIN` then multiplies that DC by 3.6.
  `B3_PHOTO_AO_ANGBIAS` rejects below 0.22 (about 13°) and rescales what is
  left back to 0..1.

Measured, same frame, all three: open road **−42.62 → −12.99**, contact
**−42.82 → −22.73**, frame mean **−15.16 → −3.94**. On the shipped default
preset the near-field road goes from **−39.89 levels against `B3_PHOTO=0` to
−10.15**. `tools/validate_photo.py` section 9 gates it at 2048×1536 *and* at
1280×960, and gates the discrimination rather than the darkness.

### Tier 3 — depth atmospherics

An exponential in distance times an exponential in height, with the height term
integrated by its midpoint.

**The colour it fades to is the sky the object silhouettes against**, read out
of the dome itself. `b3_sky_horizon_band()` (src/burnout3_postfx.c) runs the
sky pass's own two blends — the 64×32 gradient LUT, then the alpha-over of the
cloud plate with its recovered `C0 = 0.5` halving — on the CPU, per azimuth, at
the elevation range distant geometry occupies. Both of the dome's texcoord
formulas are linear in `yhat`, so a read between two rings is exactly what the
rasteriser interpolates there. The band arrives at the shader as a mean plus
two azimuthal harmonics (five `vec3`s, no sampler, no `atan` — `cos a` and
`sin a` are the normalised `vn.xz`), so the fade target follows the view around
the compass: brighter on the sun's side, because the LUT's own glow pass
already put it there. The second harmonic is not decoration — the cloud plate
wraps **twice** around the dome, so all of its azimuthal content sits on the
even harmonics and the first cannot see it at all.

The track's sun colour (`enviro.dat +0x60`, `[C]` as a field, read through the
same sidecar `carfx` reads) still leans the scatter toward gold when you look
into the sun — but only its **hue**: it is renormalised to the sky's own
luminance in that direction, so it can tint the haze and never lift it above
the sky. The forward lobe's extra brightness stays in the *amount*, where
saturating at 1 bounds it.

#### What this replaced, and why (2026-08-27)

Reported: "far away things appear totally gray" — US_M1, a coastal storm at
dusk, distant buildings rendering as flat light-grey cut-outs against a
near-black sky. Two defects, both in this tier:

* **the fade target was a compiled-in daylight blue** (0.50/0.64/0.86) leaned
  on the sun colour — and `enviro.dat +0x60` is a **tint, not a radiance**.
  All 36 shipped tracks store it with a component at 0.90 or above; the
  darkest is US_M1's own (0.937, 0.741, 0.561), a warm dusk *hue* at full
  brightness. So there was no track whose sun colour dimmed the haze, and the
  "dusk dims the scatter" reading was never true of any frame.
  Measured on the reported viewpoint: far band **151.8** against a sky at
  **72.0** — the distant city came out **twice as bright as the weather behind
  it**, in flat neutral grey.
* **it applied both halves of the transport equation.** The paragraph below
  has always said this tier is *additive to* the recovered fog — and the code
  wrote a plain `mix`, which is inscatter *and* extinction. The extinction
  half is retail's fog, already applied to the same pixel by the world pass
  over the track's own `fog_start`/`fog_end`/`fog_far`. With the target
  corrected but the `mix` left alone, the far band came out **darker with the
  tier on than with it off** (26.4 against 28.3): the pass was taking light
  away from geometry the fog had already taken it from. It now lifts toward
  the target and never below: `mix(c, max(col, c), f)`.

After, same viewpoint: far band **49.4**, ratio to the sky **0.32**, and the
tier now *adds* 2.0 levels over the tier-off frame instead of subtracting.
`tools/validate_photo.py` section 3b gates both laws on that track; run against
the pre-fix binary it fails the first (far 119.5 vs sky 71.3) and passes the
second, which is why both exist. Day tracks: US_C3_V1's far band moves
55.3 → 50.3 and US_C1_V1's 68.9 → 64.3 — the haze is now the track's own sky
rather than a brighter constant, and the near field does not move at all.

It is **additive to** the recovered fog, not a replacement for it. Retail's fog
is `[C]` and is not this wave's to overwrite; what retail has no concept of is
aerial perspective. Because it is applied in the deferred pass, the track, the
scenery, the props, the cars and the traffic all receive the same treatment
from the same code — which the legacy per-pass fog could never do, since the
cars were never in it.

### Tier 4 — sun shadow maps

One depth-only pass over the retained track / props / scenery buffers from the
track's own sun direction (`enviro.dat +0x80`, `[C]` as a field), rendered
orthographically into a 2048² depth texture, sampled with 3×3 PCF.

**One cascade, following the camera** — a measured decision. Two cascades cost
a second full geometry pass and the geometry pass is the whole bill; a single
2048 map fitted to a 260 m box puts about 8 texels on a metre, which is enough
for a lamp post's shadow to read as a lamp post at the distance a racing camera
looks. The measured cost of the one pass is **+0.25 ms** at 1080p (section 4
below), so the headroom for a second exists — it is not taken because the
quality it would buy is at a distance the box already fades out at.

**Stable.** The box's centre is snapped to a whole shadow texel in light space
every frame. Without it every shadow edge crawls along every kerb at exactly
the speed the car is doing, and no amount of PCF hides it.

**It reuses the world program**, deliberately. A position-only shader would be
marginally cheaper per fragment and would have cost a second program, a second
set of attribute bindings and a second place to get the cut-out **alpha test**
wrong — and the alpha test is not optional: without it every chain-link fence
and every foliage card casts a solid rectangle.

**And it must set its own WINDING, which is the other half of reusing the world
pass.** The world draws through a projection that carries the display mirror
(`b3r_scale(-1,1,1)`) and compensates with `glFrontFace(GL_CCW)`. This pass's
projection is a plain ortho with no mirror, so its parity is the opposite one —
and it used to inherit the world's CCW, which left GL culling exactly
backwards: it threw away every face turned **toward** the sun and kept the ones
turned away. The track mesh is single-sided and says so at the top of its own
OBJ, so that does not dim the map, it **empties** it. Measured on `US_C1_V1`
frame 655: the road under the car was not in the map at all — the nearest drawn
texel to the car's own ground point was **13.7 m away**, only **6.2%** of the
map held anything at road level, and **70%** sat at the far plane. The one
caster group that draws with culling off (the scenery) went on casting
correctly, so the near field kept its tree and post shadows while the distant
buildings lost theirs.

Against a ray-cast of the real track and scenery geometry the map **missed
21.8%** of genuine occlusion — 76% shadowed where the truth is 98% past 100 m —
and false-shadowed **0.3%**. With the winding right: **40.2%** of the map at
the far plane, missed occlusion **17.6%**, and **91.4%** past 100 m. Culling by
`GL_BACK` in the light's clip space now means "drop the faces turned away from
the sun", which is the correct caster set and half the fragments.

**The map is readable.** `B3_PHOTO_SH_DUMP=<path>` writes the finished depth
map as a 16-bit PGM, the scene depth the lookup is done from, the camera and
light matrices, and the occlusion buffer; `B3_PHOTO_SH_DUMP_AT=<n>` picks the
pass, because the first frame of a run is a loading frame and every capture in
this tree pins one several hundred later. Every number in the two paragraphs
above came out of those files.

**Scale, do not double-darken.** The art already carries the artists' baked
occlusion in its vertex colours. `b3lit()` ramps every darkening term's
authority from a floor on a black pixel to full on a bright one, so a shadow
crossing an already-shaded wall changes it very little and a shadow crossing
bright tarmac changes it a lot — which is also what the physics says.

**Shadows are coloured, not just darker.** Losing the sun leaves the sky's
light on a surface, and the sky is blue. `B3_PHOTO_SH_COOL` pulls the shadowed
pixel toward a blue-lean version of its own luminance, and that is most of what
makes these read as photographic rather than as a multiply.

**Tier 4b — the directional term.** A shadow map answers "is the sun blocked"
and says nothing about "is this surface facing the sun", so shadows alone leave
a wall in full sun and a wall turned eighty degrees away at the same brightness.
The relight is **energy-preserving**, not a second light: the gain is exactly
1.0 at `B3_PHOTO_SUN_REF` — the average N·L of the surfaces actually on screen,
measured at 0.38 for these street scenes — and leans up or down from there. The
first cut used 0.55 and cost the frame ten levels of mean brightness, which is
a dimmer switch wearing a directional-light costume.

Both N·L-driven terms **fade out with distance** (`B3_PHOTO_SUN_NCONF`). A
normal reconstructed from two depth taps is only as good as the depth
difference between them, and down a road at a grazing angle that difference
collapses. The shadow **map** is not faded — a depth comparison needs no normal.

**Tier 4c — the normal offset is a displacement in the MAP, not in the world,
and that is what bounds it.** A player reported "a large shadow travelling with
the car spanning the road" and it was real. The lookup position is nudged along
the surface normal to keep a texel's worth of a sloped surface from shadowing
itself; the first cut scaled that nudge by `1 / max(N·L, 0.15)` — "more offset
as the surface turns away from the sun", the right instinct with the wrong
function. On a road under this track's sun that reciprocal sits at its 6.7×
ceiling, and the lookup then walked **3.7 m — fifteen texels — sideways across
the map**. Fifteen texels is not a bias, it is a different place: every road
pixel in the near field read the shadow of whatever stood ten to twenty metres
down-sun of it, which on a street is the buildings, so the near road inherited
their shade as one slab. `nconf` faded the offset out with distance, so the
slab stopped at about 200 m and travelled with the camera.

The scale is now `sin(angle(N, L))`, which is what the offset is actually for:
moving `d` metres along the normal moves the lookup `d · sin` metres sideways
**in the map's own plane**, so scaling by that same sine bounds the walk at one
`B3_PHOTO_SH_NORMOFF` — about two texels — and is zero where the surface faces
the sun and no offset is wanted. Measured on the same driving frame, same
legs, same strip of tarmac in front of the car: **−15.37 levels of false shade
before, −0.66 after.**

The other three suspects were eliminated by measurement rather than by reading:
a **20× depth bias** moved the strip by 0.00 levels (so not acne); the
**relight** leg on its own *brightens* the frame (+3.10 levels, no wedge); and
the **caster list** was inspected as a draw list rather than as an intent —
`B3_PHOTO_VERBOSE=1` prints `track 157 + props 7 + scenery 68 = 232 draws in
the map; cars 0`, with the car mesh counting its own raw `glDrawArrays`. The
attribution tool is `B3_PHOTO_SH_CASTERS=<subset>`, which keeps only the named
casters in the map for a run.

### Tier 4r — the RAY-TRACED sun shadow (optional, **off by default**)

Everything above is a depth map. This is the same question answered by
casting a ray, and it is the wave's first **optional** effect: a
`SETTINGS → RAY TRACING: ON/OFF` row in the pause overlay, persisted to
`build/settings.cfg` next to `mixer.cfg`, `B3_RT=0/1` for harnesses, and
**off** unless somebody turns it on.

`src/burnout3_rt.h` carries the whole design. The short version of why it
exists: **one cascade fitted to a 260 m box** is two limits at once, and
neither is a quality setting.

* **Contact hardness.** A shadow map has one texel size everywhere, so a
  kerb's shadow and a tower's shadow have the same edge however large the
  map is. The rays are spread over a disc of the **sun's own angular
  radius**, so the penumbra widens with the *occluder's* distance without
  anything having to measure that distance. That is the whole soft-shadow
  mechanism; there is no blur radius anywhere in it.
* **Distance.** Past the box the cascade has nothing to say and fades to
  lit. Executed and **counted** rather than asserted: of 4000 ground
  samples on `US_C3_V1`, **95 are shadowed by an occluder further away
  than the entire 260 m box** — the furthest at **886 m** — so no depth
  map fitted to that box contains the caster at all. That is
  `validate_photo`'s tier-4r section's headline leg and it runs the shipped C
  traversal against the shipped `bvh.bin`.
* **No bias apparatus.** Acne, peter-panning, the slope-scaled bias and
  the whole of tier 4c's normal-offset story are sampling artefacts of a
  *map*. A ray starting five centimetres off the surface has no texel to
  fall behind. Measured on the same road strip tier 4c is measured on:
  the map moves it **−0.72 levels**, the ray **−0.17**.

**The world is an asset, not a runtime build.**
`tools/cextract/cx_bvh.c` is a new per-track stage that builds a
binned-SAH BVH2 over the static world — the opaque track submeshes out of
`track.obj`, every prop placement out of `props.bin`, every scenery
placement out of `scenery.bin`, baked to world space with the same
column-major apply `b3r_inst_build()` bakes the draw with. It reads those
three artefacts rather than `static.dat` **deliberately**: a shadow that
disagreed with the picture would be a second decode's disagreement, and
the picture is drawn from these files.

| | `US_C3_V1` | `AS_M1_V1` (the heaviest) |
|---|---|---|
| triangles | 174,402 | 853,060 |
| — track / props / scenery | 71k / 6k / 97k | 315k / 15k / 524k |
| two-sided duplicates dropped | 15,706 | 80,951 |
| nodes | 64,731 | 301,875 |
| max depth | 27 | 31 |
| artefact | 10.9 MB | **52.9 MB** |

**All 36 shipped circuits in 35 s** — under a second each, serial, no pool —
for **951 MB** of artefact in total. The worst single track is 52.9 MB, which
is also its GPU cost: two float textures, uploaded once when the track loads
and never touched again.

**The ESSL 1.00 dialect dictated the encoding**, not the other way round.
The chain speaks one shader language on all four targets (no `#version`,
`#ifdef GL_ES` + precision — desktop GL, GLES2, WebGL 1, WebGL 2), and
that language has no stack, no bitwise operators, no `texelFetch` and no
uniform loop bounds. So:

* the tree is flattened depth-first with **escape indices** — a miss jumps
  to the escape, a hit descends to `index + 1`, and there is **no stack to
  index**;
* a leaf's first triangle and its count ride in **one float** as
  `first * 8 + (count - 1)` and come back out with `mod()` and a divide —
  exact, because a `highp` float carries 24 bits and the stage refuses a
  track that would need a 25th;
* both buffers are ordinary **NEAREST** `RGBA32F` textures addressed by
  arithmetic, with a power-of-two width so `index / width` is exact, and
  their dimensions arrive as uniforms;
* both loop bounds are spliced with `%d`, exactly as the PCF kernel and
  tier 7's light budget already are.

**Cut-outs are measured, not guessed.** A ray cannot afford a texture
fetch per hit, so a chain-link fence's coverage — the fraction of its own
sheet whose alpha passes the **game's own** `D3DRS_ALPHAREF` of 64/255
(`FUN_00038D10` @`0x00038FEE`, `[C]`) — is measured once per material at
extraction time and stored per triangle. The traversal then **attenuates**
rather than blocking: a 30 %-covered fence dapples, two overlapping reach
51 %, and a solid material blocks outright. The decal layer and the
blended pass are excluded (coplanar with the road; transparent), and the
reverse-wound duplicate every `D3DCULL_NONE` submesh carries is dropped
first-occurrence-wins — 15,706 triangles on `US_C3_V1`.

**The map is not rendered at all while the ray is on.** The ray *replaces*
tier 4's term rather than adding one, so `burnout3_full.c` skips the
depth-only geometry pass entirely — which is where tier 4's own +0.25 ms
comes back — and the deferred shader is **rebuilt** when the option is
toggled rather than branched inside. That last choice is the same one the
rest of this file makes for the same reason: a frame must not pay for an
effect it is not running, and an `if (uRtOn)` around a traversal would
leave every off frame carrying its register pressure.

**The spread pattern is screen-locked and not animated.** A golden-angle
spiral rotated per pixel by a hash of `gl_FragCoord`, with no time in it:
there is no TAA in this port and section 6 of the gate measures
frame-to-frame change on a pinned camera and correctly calls it flicker.
An animated rotation would look better in motion and would fail that gate.

**Two exact early-outs carry most of the cost**, and neither is an
approximation: a sky pixel has no surface to shade, and a pixel whose own
normal faces away from the sun is *unlit* rather than shadowed — the block
already multiplies that out, so tracing it would be paying for a number
about to be multiplied by zero.

#### The far field: it was acne, and it was not the thing that looked like it

A player reported *"with ray tracing on, things far away have a shimmer /
flash"*. Nothing in the suite could see it: `photo_strip` pins a frame so
that nothing moves, and `validate_photo` section 6 measures a **pinned**
camera. The defect only exists while the camera is moving.

`tools/rt_shimmer.py` renders consecutive frames of the same deterministic
race and measures the pixels the world is **not** moving — discovered from
the ray-tracing-off control leg rather than guessed at by screen row, which
is why a first attempt at horizontal bands found nothing (the near road
changes 20 grey levels a frame and a shimmer of eight is invisible beneath
it). On that mask, hard flips of more than 8 grey levels between two
consecutive frames:

| leg | hard flips |
|---|---|
| the depth-map control | **0** |
| tier 4r as it shipped | 475 |
| `B3_RT_SUN_DEG=0` — no cone at all | 454 |
| `B3_RT_RAYS=32` | 472 |
| `B3_RT_STEPS=512` | 475 |
| `B3_RT_RANGE=3000` | 475 |
| `B3_RT_ORIGIN=0.5` — a bigger **flat** push | 567 (worse) |
| `B3_RT_DIST_BIAS=1e-6` | **179** |
| all three corrections | **126** |

So the screen-locked spiral was not the cause and neither were the budget,
the range or the sample count. *"A ray starting a hair off the surface has
no texel to fall behind"* was half right: it has no texel, but it does have
a **position error**, and that error grows with range two ways — a depth
buffer's world-space quantum goes as `dist² / near` (about 4 cm at 600 m
against a flat 5 cm push), and one pixel down a road at a grazing angle
spans tens of metres along the view ray.

The origin push and the trace's own `tmin` therefore grow as `origin +
B3_RT_DIST_BIAS · dist²`, scaled by two grazing angles: the **sun's** (the
ordinary slope scale, which `B3_PHOTO_SH_SLOPE` does for the map) and the
**view's**, which a map does not need because a map's error lives in the
light's frame and a depth-reconstructed position's lives in the camera's.
Each is a blend from the old behaviour, so `B3_RT_DIST_BIAS=0
B3_RT_SLOPE=0 B3_RT_GRAZE=0` restores tier 4r exactly and puts the defect
back — which is what `make test-rt-shimmer` gates on.

The cone is left exactly as it was. Narrowing it with distance was the
obvious second fix and would have cost the contact hardness the whole
option exists for, in exchange for the 4% above.

### Tier 4rc — the cars' own trees (on with tier 4r, `B3_RT_CARS`)

*INSPIRED.* The Xbox drew a `blobbyshadow` quad under each car
(`FUN_0019A7C0` / `FUN_00043570`, `[C]`) and had no acceleration structure
of any kind.

Tier 4r's own header used to say cars could not be in the tree because *a
BVH is built once and a car moves every frame*. That is true of the tree
and not of the car: a car is a **rigid body**, so its triangles never move
relative to each other and only the whole model does. Build the tree once
in **model space** and the entire per-frame cost is one 3×4 matrix per
instance.

* **The artefact** is `build/cars/carbvh.bin` (`'B3CV'` v1,
  `tools/cextract/cx_car_bvh.c`): one tree per vehicle for the whole fleet
  in one file — 106 models (67 `.bgv` + 39 `.btv`), 381,026 triangles,
  140,708 nodes, depth 16, 1.8 s to build. The node and triangle records
  are `bvh.bin`'s own, written by the **shared** builder
  (`cx_bvh_build.c`), because the shader that walks one walks the other.
* **The runtime packs a selection.** The file is 24 MB of float texture and
  a race needs about a dozen of the 106, so `b3_rt_car_select()` copies
  just those and rewrites every escape index and leaf packing onto the
  packed arrays. Six racers come to 10,556 nodes / 28,352 triangles — 1.6 MB.
* **The trace** does the static world first, then per instance a ray-sphere
  reject in world space, a 3×4 into model space, and the same stackless
  escape walk one level down. The two transmittances are **multiplied**,
  because transmittance composes. Five `vec4` an instance; the model's box
  is not uploaded because the root node's box already is it.
* **One texture**, where the world's tree gets two. That is a hard
  constraint: ESSL 1.00 guarantees eight fragment texture image units and
  the deferred pass already binds scene, depth, AO, shadow, SSR and the
  world's two.
* **The pose is `car_lamp_pose()`'s** — the same rigid frame the body's
  draw matrix is built from and the coronas and headlights ride. There is
  deliberately no second way to say where a car is.

**Retail's blob is suppressed for exactly the cars the ray took**, asked of
what was actually *uploaded* rather than of what was wanted. A blob under a
car that is already casting is the same darkness twice — and worse than
twice, because the blob is a fixed ground-plane ellipse and the traced
shadow has the car's outline wherever the sun happens to be. A car past the
instance budget, a car whose model the extractor refused, and every frame
with ray tracing off all keep it.

`B3_RT_CARS` picks the set: `racers` (default), `all` (…plus the nearest
traffic), `off`.

#### What tier 4rc still cannot do

* **Damage states.** A wreck is traced with its **intact hull**. Damage
  here is discrete record swapping and a wrecked body is an aperture shell
  plus however many panels are still attached — a tree whose *contents*
  change while the car is being driven, which is the one thing the whole
  construction exists to avoid. The difference is the door and bonnet
  apertures, at the moment the player is watching a car tumble.
* **Wheels are baked at rest.** Spin is not modelled (a tyre is very nearly
  a solid of revolution) and neither is steer (a few degrees about a 0.3 m
  chord). They *are* baked in, at their `.bgv+0xB80` attach positions,
  because they are the lowest thing on a car and a shadow without them is a
  floating slab with four gaps under it — 114,740 of the fleet's 381k
  triangles. `B3_RT_CAR_WHEELS=0` builds hull-only.
* **Knocked props** are still out, exactly as before.
* **Night tracks lose a grounding cue.** On the darkest tracks (US_P1's
  sunset) the traced shadow carries far less contrast than retail's
  stylised ellipse did, so a car reads slightly less planted than it did
  with the blob. The traced shadow is the more correct one; the blob was
  doing a job beyond representing the sun. Keeping a reduced blob where the
  traced term is weak is the obvious follow-up and is not done here.
* **`TSPC_Car5` has no tree** — its mesh fails the same plausibility gate
  the mesh exporters apply (a 72.8 m Z extent), so the renderer draws a box
  for it and it keeps its blob. The two agree about which vehicles are
  readable, which is the point.

### Tier 5 — SSR on the shine spans

A screen-space depth march run **only where the track's own material flags
already declare a shiny surface**: the class-1/7/10 additive specular groups
`burnout3_render.c` gathers into its shine spans. Those spans are the game's own
statement about which surfaces reflect, so the port does not have to invent a
gloss channel.

The mask is **one draw call** — the spans are contiguous in one VBO — into a
colour target with the **scene's own depth attached**, so the mask is
depth-tested and a shiny road behind a car is not marked reflective through it.

It is **coverage, not the specular value**, and that was measured the wrong way
round first. Handing the mask the shine pass's own per-frame colours is free
(they are already uploaded) and produces a mask that is black almost everywhere,
because a specular term is near zero except at an actual glint: the first cut
moved 0.02% of the pinned frame. What the reflection needs to know is not "is
this pixel glinting" but "is this surface the kind that reflects", and the
material flag already answers that. The angular falloff is left to the Fresnel
term in the march, where it belongs.

A miss is **not black**: the alpha channel carries "did this pixel get a
reflection" and the deferred pass mixes by it, so a miss leaves the pixel
exactly as the existing env-map path drew it.

**It nearly shipped off.** On the numbers it was the obvious cut — the most
expensive of the six and the narrowest, touching 2–10% of the frame. Then the
contact sheet was looked at: on wet tarmac the reflection of the **car itself**
lands on the road behind it, and it is the most photographic thing in the
frame. The measurement was right about the cost and had nothing to say about
the value, which is what `tools/photo_strip.py` exists for.

### Tier 6 — god rays

Two quarter-resolution passes: a **source** holding only what the sky
contributes (a hard depth cut at the far plane, falling off with distance from
the sun's projected position), then an ordinary decaying radial scatter toward
it. Added **before** the filmic curve, which is the whole argument for doing
tier 1 first: added after it they would clip, and a clipped light shaft is a
white wedge with a hard edge.

The sun is a point at infinity, so it projects as a `w = 0` homogeneous vector;
a non-positive `w` means it is behind the camera and the shafts are simply not
drawn. On `US_C3_V1` that is the correct answer for most of a lap —
`B3_PHOTO_VERBOSE=1` prints the sun's projected uv every 120 frames, which is
also how you find a frame worth photographing.

### Tier 7 — per-source lights

Streetlights, traffic-light heads, neon signs and lit shopfronts each cast real
light, accumulated against depth and the reconstructed normal in the same
deferred pass, after the sun's shadow and before the reflection and the air.
The interesting half is not the accumulation, which is an ordinary bounded
point-light loop, but **where the lights come from**.

**There is no light table on the disc.** That was established before anything
was written, and it is the whole reason this tier is built the way it is:

| looked at | what is there | is it a light? |
|---|---|---|
| `docs/RE_BGD.md`, every chunk type | events, sections, traffic, road network | no |
| props / scenery tables (`cx_props.c`, `cx_scenery.c`) | a 0x70 model record and a 4×4 placement, with a class field that says CONE or BARRIER | no |
| light probes (`cx_light_probes.c`) | a 9-coefficient SH bake of the light a car **receives** at a point on the collision mesh | no — received, not emitted |
| `sfx_emitters` | car-impact sound events, with no position field at all | no |
| `docs/RE_POSTFX.md` | one global sun/sky corona | no |
| the `.bgv` **car** model, `model+0x1664` / `+0x16AC` | 0x30-byte light records in MODEL space, per type (head / tail / brake / indicator / exhaust) — `docs/RE_CARFX.md` 534–549, `[C]` | **yes, and it is the only one** |

So the one light table retail ships is **per car, inside the model, instanced
to the placement**. This tier takes that shape and applies it to the scenery,
which is where the streetlights are — arriving at the table from the art,
because the art is the only place it exists.

**The derivation, per MODEL, once at load** (`scenery_lights_build` in
`src/burnout3_scenery.c`):

1. scan the model's texture for **emissive texels** — bright, opaque, and
   sitting well above the texture's own mean over its **opaque** texels only
   (these sheets are cut-outs; counting the transparent background makes every
   foliage card read as a bright thing on black);
2. find the model's **vertices whose uv lands on those texels** and take their
   centroid. That is the bulb, in model space. Not the bounding box: a lamp
   post's bbox centre is halfway down the pole and a traffic-light gantry's is
   out over the road with nothing there at all;
3. take the **mean colour of those same texels** — sodium orange, fluorescent
   white and neon come out of the art for free, and no colour for any light in
   this game is written down anywhere in this tree;
4. instance it through every placement of that model.

**Three tests decide what is a bulb, and the third is the one that matters.**
"Bright and opaque" alone calls a white van a light. A bulb is a bright patch
on a *dark* thing (`B3_PHOTO_LIGHT_EMIS_DK`, `_EMIS_DL`), it is at least a per
cent of its own sheet (`_EMIS_MIN` — the floor was 0.12% and let a palm tree in
on `AS_M1_V1`, three vertices of sky-lit frond edge), and its texels are
**concentrated**: their spread about their own centroid, in the model's own
radii, must be small (`B3_PHOTO_LIGHT_TIGHT`). A light source is a small bright part of
a bigger object. That last test is what rejects `bk_fruitcrates`,
`bk_plasticstool` and `KS_sign_arrow` while keeping `GL_streetlight2` (spread
0.15) and `WF_tlightPost` (0.04).

**What it finds, measured:**

| track | lights | from models | examples |
|---|---|---|---|
| `US_C3_V1` | 67 | 8 of 32 | `GL_streetlight2` ×22, `GL_tlightPost` ×16, `Chgo_PhoneBox` ×7 |
| `US_P1_V1` | 112 | 12 of 56 | `Chgo_OrnateLamp` ×22, `WF_tlightPost` ×12, `WF_SignSPEED` ×22 |
| `AS_M1_V1` | 115 | 3 of 70 | `bk_marketelecpole` ×43, `bk_streetclutter1` ×72 |

**And it is honest about what it cannot separate.** On the waterfront tracks
the moored boats' white superstructures pass every test — they are brighter
than the street lamps (texel luminance 0.924 against 0.855), on a sheet that is
just as dark, and just as concentrated. There is no numerical rule that keeps
the lamps and drops the boats, because the art genuinely paints them the same.
They are left in: a lit boat at a dock is a defensible answer, and a fudged
threshold that happened to exclude them on the two tracks it was fitted to
would not be.

**Nearest-N per frame**, to a point that **leads the eye** by
`B3_PHOTO_LIGHT_AHEAD` — a chase camera sits behind the car and half of what is
"near" it is behind the car with it. The pick is a partial selection, not a
sort: the shader has room for N and nothing about the frame depends on their
order, so it is O(lights) with an N-entry running worst. A light outside its
own radius contributes exactly zero, which is what makes dropping it from the
list an exact operation rather than an approximation.

**Day is subtle, dusk sings, and the number in between is the track's own
hour.** There is no night flag in `enviro.dat` and, measured across all 36
circuits' sidecars, no night *track* either: every sun in the game sits between
15° and 45° above the horizon. What the roster has is a **dusk end**, and both
halves of it are `enviro.dat` fields — `+0x80`'s vector (`EU_M2` at 15° is the
lowest sun in the game; `US_C5` at 45° the highest) and `+0x60`'s colour
(`US_P1`'s is 1.00/0.65/0.55, a sunset; `AS_C3`'s and `EU_C1`'s are pure
white). Half a dusk factor from each. It comes out **0.61 on `US_P1`**, 0.56 on
`EU_M2`, **0.20 on `US_C3`** and 0 on `US_C5`, and the tier's gain is mixed
from `B3_PHOTO_LIGHT_DAY` to full by it. No track is named anywhere.

**It nearly shipped as a white-out.** The first gain was 1.30, which on
`US_P1_V1` under the elevated section was **+62 levels of frame mean** — the
underpass filled in and the sunset behind it went. Two things were wrong: the
gain, and the reach. 3.4 model radii put a street lamp at the 28 m ceiling and
twelve of those overlap into a flat ambient lift with no pools in it; the tell
was that the picture got brighter without getting more *lit*. At 2.0 radii, a
15 m ceiling and a gain of 0.30 the same frame gains **+4.6 levels**, the
clipping does not move at all (0.10% either way), and the light is where the
lamps are: the brightest **5% of pixels carry 99.9%** of it.

Measured on the shipped default, `US_C3_V1` (a daylight track, dusk 0.20):
**+0.29 to +0.43 levels** across a driving capture. That is the restraint half
of the mandate, and it is the day multiplier doing it rather than a decision
about which tracks get lamps.

### Tier 7b — the racers' own headlights

The beam's origin and aim are the car's own recovered lamp table (type 0, the
same records the corona pass draws), instanced through the same rigid pose the
bodywork draws with, so the beam leaves the point on the bodywork the glow
sprite is drawn at — on a tumbling wreck as much as on a straight.

**Two things were wrong with it and a player saw both.** The report was "the
opponent's car shows a bright blue-white glow at its REAR, and no light pools
appear on the road ahead of any car anywhere".

* **The rotation was applied transposed.** `R` is the row-major object→world
  matrix the corona pass builds, and the corona pass applies it as
  `world.x = R[0]*m.x + R[1]*m.y + R[2]*m.z`. This tier indexed it down the
  columns instead — the transpose, which for a rotation is the **inverse** — so
  the beam was placed and aimed by the car's rotation run backwards. Worked
  through on the player's own pose in the reporting dump (yaw 1.37, heading
  (+0.98, 0, −0.20)): the type-0 lamps sit at model (0, 0.305, −1.772) facing
  (0, 0, −1), which the corona pass puts **1.74 m in front** of the car's
  origin aiming along the heading; transposed, the same table lands **1.74 m
  behind** it aiming at (−0.98, 0, −0.20). Measured, the player's beam alone,
  road pixels the beam lifts, positioned along the car's own heading from its
  centre: **100% behind, −1.8 to −2.9 m, weighted mean −2.8 m** before;
  **100% ahead, 0 to +33.5 m, weighted mean +16.5 m** after. The corona sprites
  were always right because they never took this path.
* **The beams went out in daylight.** They rode `dusk` **squared**, on the
  argument that a headlight at noon is doing nothing at all. That is true of a
  headlight pointed at sunlit tarmac and false of every other surface a racing
  car drives past. The roster's dusk factor only spans 0.14 (`US_C1_V1`, a
  bright afternoon) to 0.61 (`US_P1`'s sunset), so squaring left `US_C1` at
  **0.02** — two per cent, i.e. off — on a track that spends a third of its lap
  under a stadium deck with the lights on.

The mix is now `day + (1 − day) · dusk`, always at least `day`, and the floor
ships at **0.30**. The floor is not a brightness in itself: `b3lit` hands a
light most of its authority on a dark pixel and little on a bright one, so one
number reads as a pool in shade and stays quiet in the sun. Measured on
`US_C1_V1` against the same frames with `B3_PHOTO_HEAD_ON=0`: inside the
stadium tunnel the pool covers **8.5% of the frame, +6.5 levels mean over it,
+66 at its brightest**; out in open sun the whole frame moves **+4.2**. Strips:
`build/photo/beams_us_c1_tunnel.png` and `build/photo/beams_us_p1_dusk.png`.

#### …and a third round, because the player still could not see them

The report came back unchanged: *"I am still not seeing the player or opponent
cars' headlights properly casting light onto the track."* Both fixes above were
in and both were real. The tunnel strip above was real too. What was not real
was the inference from it, and the shape of that mistake is worth keeping: **a
frame chosen because the effect shows on it is not evidence that a player sees
the effect.**

Round three measured an ordinary lap instead. Six moments ten seconds apart, at
the user's own 2048×1536, the same drive rendered twice (`B3_PHOTO_HEAD_ON` 1
against 0), differenced over the **road band** — rows 42–62% of the frame,
which is where the pool lands once the chase camera's own car has hidden the
first ten metres of it.

| leg | `US_C1_V1` day, road-band p99 | `US_P1_V1` dusk |
|---|---|---|
| before | 8.0 – 50.8 levels, **1.0–2.0% of the band lit** on open road | 7.3 – 57.1, **0.9–3.3% lit** |
| after  | **111 – 138 levels, 13–58% lit** | **86 – 228 levels, 4–25% lit** |

### Round four: "the headlights are too bright"

The same report, from the same player, in the other direction — which is what a
one-way gate produces. A floor on its own can only ever be answered by turning
the thing up, and round three turned it up until the next sentence was "too
bright".

**Which knob answers it is the whole of the finding.** `GAIN` scales every
surface the beam touches by one factor, so halving it halves the tunnel wall
along with the blown road — and the tunnel is the case rounds one through three
were all about. `WRAP` is `mix(N·L, 1, w)` and is *by construction* a
grazing-incidence term: the road at 10 m has N·L = 0.061 and is almost entirely
made of wrap, while a wall has N·L = 1 and is `mix(1, 1, w)` = 1 for any `w` at
all. So wrap can dim the road and leave the wall alone, and gain cannot.

Swept on section 12's own drive — 2048×1536, six moments, road-band p99, `GAIN`
held at 0.70:

| `WRAP` | `US_C1_V1` day, worst → best | `US_P1_V1` dusk, worst → best |
|---|---|---|
| 0.85 *(was)* | 111.9 → 137.4 | 86.1 → 229.0 |
| 0.60 | 84.4 → 115.4 | 59.7 → 206.8 |
| 0.45 | 64.3 → 97.8 | 43.8 → 185.0 |
| **0.40 *(is)*** | **57.4 → 91.0** | **38.7 → 176.1** |
| 0.35 | 50.8 → 83.7 | 33.6 → 164.7 |

0.40 is **half** the worst-frame road delta to the level (111.9 → 57.4) and the
underpass keeps **77%** of its punch (229.0 → 176.1). Read those two columns
against each other and that is the wrap term doing exactly what the arithmetic
above says: the open road loses 49%, the frames that pool lose 23%. Halving
`GAIN` would have taken both down by half and undone round three.

**Both ends are now pinned**, so the next round cannot be a one-way gate
either: `BEAM_FLOOR` = 8 levels (the pool reads) and `BEAM_CEIL` = 80 on the
least-lit of the six moments (the road is a pool and not a wash — 80 sits
between the pre-tune 111.9 and the shipped 57.4). `BEAM_TUNNEL` = 140 on
`US_P1_V1`'s brightest moment is the other half: it fails a change that took
the wall down with the road, which is precisely what lowering `GAIN` would do.

**And the player can disagree.** The pause menu's third SETTINGS row is
`HEADLIGHTS`, cycling OFF → LOW → MED → HIGH — wrap 0.25 / 0.40 / 0.85, with
MED the shipped default and HIGH the round-three look to the digit. Four stops
rather than the MSAA row's three because this row answers a question of taste
and taste gets disagreed with twice. It is the cheapest of the three rows by a
wide margin: RAY TRACING rebuilds an assembled shader and MSAA rebuilds the
whole chain, while this drops the knob cache and lets the next frame re-read
it — the value it moves is a float in a uniform that goes up every frame
anyway. `$B3_PHOTO_HEAD_WRAP` and `$B3_PHOTO_HEAD_ON` still win outright and
grey the row, so every beam leg in the suite stays hermetic against
`build/settings.cfg`.

The frames that *did* pool before — 70, 84, 154 levels — were every one of them
a tunnel wall, a barrier or the back of a rival: a **vertical** surface. That
is the whole diagnosis, and it is N·L.

Three candidate causes were instrumented before anything was changed:

1. **Budget starvation — ruled out.** The engine's own per-frame light ledger
   (`B3_PHOTO_LIGHT_STATS=1`, one line a frame) over 60-second drives on both
   tracks: **6 beams admitted on 6 of 6 racers in every one of 7745 frames**,
   never fewer, with 5.0–5.5 scenery lamps beside them. The reserve-first
   policy was working exactly as written.
2. **The source not glowing — ruled out.** A counter on the corona pass
   (`b3_carfx_corona_head_count()`) says head-lamp coronas are emitted on
   **10.4 of a possible 12 per frame** on `US_C1_V1` and 5.1 on `US_P1_V1`.
   They are there. What they cannot do is show the player his *own* headlights:
   `FUN_00187BE0` rejects a corona whose lamp normal faces away from the eye,
   which is correct for a billboard and means that in chase camera nobody's
   head lamps face the camera except those of the cars **behind** you. The
   source glare is legitimately invisible from where the player sits, so the
   road pool has to carry the whole read.
3. **Perceptual defeat — this was it.** The recovered type-0 lamp sits a
   **measured 0.615 m** above the road (`b3_ground_probe` under the beam
   origin, sampled over a lap). The road at 10 m therefore takes **0.0613** of
   the beam and at 30 m **0.0205**, while a tunnel wall in the same beam takes
   **1.0**. The pass was delivering two to six per cent of a headlight to the
   one surface the feature exists to light.

**The fix is three changes to the beams and the beams only.**

* **A wrap term.** `mix(N·L, 1, 0.85)` — Valve's half-Lambert, with the honest
  physical excuse: a sealed beam is a 15 cm emitter behind a fluted lens, not a
  point, and asphalt scatters what grazes it. The road at 10 m goes 0.061 →
  0.859, the fourteen-fold the measurement said was missing. A wall is
  `mix(1, 1, w)` = 1, **unchanged**, so the tunnel case that already worked is
  left exactly where it was.
* **An elliptical footprint with a cutoff**, replacing `pow(cos, k)`. A bell
  has a peak and no rim, and once the wrap term gave the road enough light to
  see, what the road showed read as *the exposure went up* rather than *that
  car has its lights on*. A round spotlight with a smoothstepped rim did not
  fix it either, and the reason is the camera: the chase view looks **down the
  beam's own axis**, so a round footprint is seen edge-on and its rims fall
  outside the frame near the car and converge on the horizon far from it. You
  cannot see the shape of a cone you are standing inside. What a real headlight
  has is a **cutoff** — wide across the road, shallow in elevation — and that
  edge runs *across* the view, so it survives. The test is elliptical in
  tangent space: horizontal and vertical offsets from the aim, each over its
  own spread, summed in quadrature, hot core inside `HOT` and a smoothstep to
  the rim.
* **The aim moved up, from 0.16 to 0.062.** The road at distance *d* sits only
  `atan(0.615/d)` below the lamp — 3.5° at 10 m, 1.8° at 20, 1.2° at 30 — so a
  beam aimed 9.1° down puts its hot core at 4 m, *under the car*, behind the
  bodywork, where the chase camera cannot see it. At 3.55° with a 3.2° cutoff
  the same beam holds full power from **6.5 m to 21 m** and fades out through
  the range window, which is exactly where the car stops hiding the road. The
  range went 34 m → **60 m** for the same reason: `att` at 25 m is 0.21 of a
  34 m beam and 0.63 of a 60 m one, and the near half of a 34 m pool was behind
  the very car that cast it.

Two supporting changes came with them. The colour went from near-white to a
halogen **3200 K** (1.00 / 0.89 / 0.68): sunlit asphalt is neutral grey, a pool
that is merely *brighter* grey is an exposure change and the eye forgives those
without noticing, while a pool that is a different **colour** is a second light
source and the eye cannot help noticing. And the beams took **their own gain,
day floor and lit-bias** as shader uniforms (`uHeadK`), which retired a real
piece of indirection: while the lamps and the beams shared one uniform gain,
the only way for the beams to have their own day curve was to smuggle the
*ratio* of the two through the per-light colour and let the shader cancel it.

**The lit-bias is the one number that went the other way, and the measurement
is why.** It was set to 0.90 first, on the argument that a headlight the player
has asked three times to see should not be damped on sunlit tarmac the way a
streetlight at noon deserves to be. Sweeping it says the argument was already
answered by the wrap term: at 0.90 the `US_C1_V1` road pool measures 118 levels
and at 0.55 it measures 111 — seven levels of a hundred and ten — while the
`B3_PHOTO_LIGHT_DUSK=1` stress leg goes from **4.01% of the frame clipped to
0.06%**. The beams never needed an exemption from the bright-pixel ramp. They
needed the grazing-incidence fix, and once they had it the ramp does what it
was written to do.

**The budget reservation became real at every setting.** The beams used to be
carved out of `B3_PHOTO_LIGHT_N` and capped at half of it, which held at the
shipped N=18 (measured above: never starved) and failed silently below it — at
N=8 the cap is 4 and two of six racers drove with no beam at all. The two
reservations now sit side by side: `B3_PHOTO_LIGHT_N` is exactly what its name
says, how many **streetlights** a frame accumulates, and one slot per racer is
added on top. Cost at 2048×1536 on an RTX 3090: tier 7 is **+0.21 ms** with the
beams retired and **+0.27 ms** with them, so the six reserved slots and the
elliptical maths together cost **0.06 ms**; all seven effects hold 114 fps.

Strips, six frames over ten seconds of ordinary chase driving, before beside
after: `build/photo/drive_c1/drive_US_C1_V1.png` and
`build/photo/drive_p1/drive_US_P1_V1.png`, made by
`tools/photo_strip.py --drive B3_PHOTO_HEAD_ON`. Gated by section 12 of
`tools/validate_photo.py`.

**No shadowing.** A point light with no shadow map leaks through walls, and
this one does. What keeps it honest is the N·L term (a surface facing away gets
nothing), the bounded radius, and the same `b3lit`-shaped authority ramp the
occlusion and the shadow use, run the other way up: a lamp adds most of its
light to a surface that is dark and very little to one that is already bright,
because the artists already painted a pool of light under every lamp post they
cared about.

### Tier 7b — the cars' own headlights

Every racer lights the road ahead of it, as a **spotlight in the same deferred
array** the scenery lamps use.

**The beam is retail's, not this port's.** The `.bgv` light table
(`model+0x1664` / `+0x16AC`, `docs/RE_CARFX.md` 534–549, `[C]`) stores each
lamp as a **position and a normal** in model space, and a position plus a
direction is a spotlight. The corona pass has drawn type 0 as a billboard
since the port began, with `B3_CARFX_LIGHT_HEAD` set on every car in daylight —
so "the headlamps are on" is retail's statement too. What this tier adds is the
light those lamps were always supposed to be casting.

`b3_carfx_car_lamps()` hands the model-space records out; the world transform
is `car_lamp_pose()`, which is now **one function shared with the corona
pass** — the beam leaves exactly the point on the bodywork the glow sprite is
drawn at, on a tumbling wreck as much as on a straight, because a second copy
of that three-way pose selection is precisely how the two would come to
disagree.

**One beam per car, not one per lamp.** A car's two headlamps sit about 1.4 m
apart, so their pools overlap over all but the first metre. The pair spelling
cost twice the budget for a difference nobody can see — and with six racers it
filled a twelve-light frame exactly and **every streetlight on the track went
out**, which is how it was noticed.

**Every racer's beam is reserved, beside the lamps' budget rather than inside
it.** `b3_photo_light_budget()` is `B3_PHOTO_LIGHT_N` scenery lamps **plus**
`b3_photo_head_slots()` beams, and the caller writes its beams into the front
of the array before the nearest-N pick runs, so the pick fills what is left.
Neither reservation can spend the other's. The earlier spelling carved the
beams out of N and capped them at half of it, which was correct at the shipped
N=18 and quietly wrong below it.

**Four numbers are ours rather than retail's**, and all four are looks: the
beam is aimed `B3_PHOTO_HEAD_AIM` **downward** (the recovered normal is dead
level, which is right for a billboard and would put a beam's pool on the
horizon); its footprint is an **ellipse**, `B3_PHOTO_HEAD_SPREAD` degrees wide
across the road and `B3_PHOTO_HEAD_CUTOFF` degrees in elevation; it takes a
**wrap** term the scenery lamps do not (`B3_PHOTO_HEAD_WRAP`); and its colour
is a halogen 3200 K. See *…and a third round* above for why each of them is
the number it is.

**The beams ride the dusk factor off a floor**, `day + (1 − day)·dusk` with
`B3_PHOTO_HEAD_DAY` at 0.30, so they are never off. They rode it **squared**
once, on the argument that a headlight at noon is doing nothing at all — true
of a headlight pointed at sunlit tarmac and false of every other surface a
racing car drives past, including the underside of the stadium deck `US_C1_V1`
spends a third of its lap beneath. The restraint that argument wanted comes
from `b3lit` instead: `B3_PHOTO_HEAD_LIT` hands a beam most of its authority on
a dark pixel and 0.55 of it on a bright one, which is a curve rather than a
switch. The corona sprites are retail's and are unchanged throughout: the lamps
*look* on at noon because retail says they are.

### Tier 7c — the cars' tail and brake lamps

The same table two rows down. `model+0x1664` / `+0x16AC` stores a record per
lamp **type**, and the corona pass has always read four of them: type 0 is the
headlamp tier 7b throws, type 1 the tail lamp, type 2 the brake lamp, type 8
the tailpipe. `b3_carfx_car_lamps()` was already type-generic, so nothing about
where a tail lamp is or what colour it is had to be invented — the colours are
`0x004161D0` (1.1, 0, 0) and `0x004161C0` (1.4, 0, 0) verbatim.

**The brake bit is tested before the tail bit, and that is retail's rule, not a
simplification.** `FUN_00187C70` walks its corona table in the order 0x02,
0x04, 0x10, 0x08, … and the tail row carries an explicit `if (light_byte &
BRAKE) continue` — so a braking car draws the brake corona **instead of** the
tail corona rather than on top of it. The light mirrors that from the *same*
predicate: `car_braking()` is now the one spelling of `last_brake > 0.05f` in
the tree, and both the corona pass and this tier ask it. Two copies of that
threshold is exactly how the pool and the sprite would come to disagree about
what a car is doing, and the disagreement would be invisible in any frame where
only one of them is on screen.

And it is the **byte** that decides, not the model. If a car is braking and its
model has no type-2 records, retail draws nothing at all — the brake row finds
no lamps and the tail row was already skipped. This reproduces that rather than
falling back to the tail, which would have been the one place the port was more
helpful than the game and therefore wrong. It is not a hypothetical: of the 107
shipped `.lights` files, **32 are missing at least one of types 1/2/8** — 17
carry no type-1 tail record at all (`COMP_Car3`, `SPRT_Car8`, every `TSPC`
traffic model), 7 no type-2 brake record, 23 no type-8 exhaust. That is why the
ledger reports five tail lights on a six-car grid, and why the gate asserts
`tails <= racers` rather than `tails == racers`: a gate that demanded one slot
per car would be demanding a lamp the artists did not model.

**They are spots, not omni points**, and the reason is the same one tier 7b
paid for twice. The recovered tail lamps sit 0.42 m above the road
(`COMP_Car10`: y = 0.417), so two metres behind the car N·L is 0.21 and eight
metres behind it is 0.05 — a plain cosine throws away 80–95% of the pool the
moment it leaves the bumper. The spot branch's `mix(nl, 1, uHeadK.z)` is a
grazing-incidence term, and a lamp mounted *lower* than a headlight needs it
more, not less. The omni branch would also have handed them the
**streetlights'** day curve (0.22 at noon), which is right for a streetlight
and wrong for a brake light: the one moment a player must be able to read a
brake lamp is the moment the car in front stands on the middle pedal, and that
moment is not reserved for dusk. A wide cone aimed down the lamp's own
recovered normal buys both for **no shader text at all**, and a third thing
that is not a workaround: a tail lamp aimed backwards puts no light on the road
*in front* of the car, which an omni point at the same place would.

The brake lamp is **bigger as well as brighter, and both ratios are retail's**:
`B3FX_CORONAS` gives the brake row a size multiplier of 0.75 against the tail
row's 0.50 and a colour of 1.4 against 1.1, so retail's own brake lamp is 1.27×
the brightness at 1.50× the extent. Carrying the size ratio over to the light's
*range* is `[S]` — a sprite's half-extent is not a lamp's reach — but it is the
ratio the artists chose for the same two lamps.

`B3_PHOTO_TAIL_GAIN` was swept, and the sweep is why it is 0.22 and not the
round 0.5 it started at. US_C1_V1, six moments, 99th percentile of the
red-channel delta over the road band behind the car:

| gain | worst frame | best frame |
| ---- | ----------- | ---------- |
| 0.20 | 13          | 39         |
| 0.35 | 26          | 66         |
| 0.55 | 44          | 100        |

0.55 was chosen on the road and looked at afterwards on the **ARMCO**, which is
where it fell over: a barrier beside the car is a vertical surface, so it takes
`mix(N·L, 1, wrap)` at N·L = 1 while the road beside it takes it at N·L = 0.05,
and the whole left-hand crash barrier went red. Same wall-versus-road split the
beams' WRAP note works through, seen from the other end. Both ends are pinned
from the first day — `TAIL_FLOOR` 5 levels, `TAIL_CEIL` 55, plus `TAIL_DUSK` 45
on the dusk track's *brightest* moment as this tier's `BEAM_TUNNEL` — so the
four-round "I cannot see it" / "it is too bright" cycle the beams went through
does not have to happen again here.

### Tier 7d — the boost flame's light

Type 8: the tailpipes `FUN_0017F730` emits the flame from (`@0x0017F73C`). An
additive billboard lights nothing, so until this tier a car could run a metre
of blue fire out of its exhausts over pitch-dark tarmac and leave no mark on
it.

**The intensity is `b3_boostfx_level()`, untouched** — 2.0 for the ignition
flare, 1.0 while it burns, and the two recovered decay rates on the way down.
So the light flares when the flame flares and dies when it dies, because it
*is* the flame's own number and not a second envelope written alongside it; a
crashed car's level is zero, so retail's wreck gate (`carObj+0x18FA`) comes
along for free.

**The colour is the sprite's own, derived.** Retail picks between two textures
by `carObj+0x1901` (the five `Car10` specials burn orange, everyone else
blue-white) and modulates each by its own recovered constant. What a flame
*emits* is the whole sprite's energy, so the light colour is the alpha-weighted
mean of each texture's texels times its own modulation constant, normalised:

| pool                 | texel mean            | × constant       | light colour          |
| -------------------- | --------------------- | ---------------- | --------------------- |
| `coronaboost`        | 0.0378 0.0621 0.1338  | 0.70 0.72 0.75   | **0.264 0.446 1.000** |
| `coronaboostred`     | 0.2593 0.0601 0.0128  | 0.80 0.80 0.80   | **1.000 0.232 0.049** |

`validate_photo` section 14 re-derives both from the shipped PNGs rather than
reading them from the header, so an art change moves the light with the sprite.
**The default flame is blue, not warm** — worth saying out loud, because every
description of this feature reaches for "a warm glow" and only five models in
the game burn orange.

**The flicker is a hash and not a `rand()`.** Retail's own flame flicker is a
per-frame `rand()`, and reusing it would have been the one change in this wave
capable of breaking every pinned-frame gate in the tree. It is a bijective
integer mix of (frame counter, car slot) — no wall clock, no PRNG state, no
dependence on how many cars boosted before this one, so a car's flame does not
change when a car ahead of it wrecks.

#### The ceiling, and why the flames get a pool

`AFX_LIGHT_MAX` is 32. It is **not** a performance budget — the deferred pass
costs about 0.005 ms per slot, so six more would be 0.03 ms — it is the length
of three `vec4` uniform arrays, and that length is what the ESSL 1.00 loop is
unrolled against. The shipped reservations are 18 streetlights + 6 beams; tier
7c's tails are 6 more, which lands on 30 and leaves **two**. A per-car flame
slot would want 36.

Could the ceiling simply rise? Asked properly rather than assumed away, and the
answer is: **not provably, on three targets.** 32 slots is already 96 `vec4`s
of fragment uniform; GLES2 — and therefore WebGL 1, and therefore the Android
build through gl4es — guarantees only `MAX_FRAGMENT_UNIFORM_VECTORS` = 16, and
every other uniform in the deferred pass draws from the same pool. What the
running context actually reports is now printed on every target, so the next
person to ask has a measurement instead of a guess:

```
[afx] PHOTO: fragment uniform capacity 1024 vec4 (MAX_FRAGMENT_UNIFORM_VECTORS);
             tier 7 asks for 96 of them (3 arrays x 32 lights)   <- desktop GL, RTX 3090
[afx] PHOTO: fragment uniform capacity 4096 vec4 (MAX_FRAGMENT_UNIFORM_VECTORS);
             tier 7 asks for 96 of them (3 arrays x 32 lights)   <- WebGL, SwiftShader
```

Both are enormously above 96 — and "enormously above on the two contexts we
measured" is still not the same claim as "enough on every device a browser
runs on". The failure mode is not graceful either: `afx_photo_build_light()`
halves the budget until the shader fits, and a halved 36 is 18, which would
take the beams and the tails down with it. **A ceiling that holds everywhere at
32 and a pool of two is worth more than a ceiling of 36 that silently becomes
18 on somebody's phone.**

And a pool is not a consolation prize here. A flame is lit for a few seconds at
a time and dark for most of a lap — measured over a 300 s soak, 4,642 of
102,220 frames carried one — which is exactly the shape a pool serves and a
reservation wastes. The two slots go to the two **nearest** cars that are
actually burning, and on the frames where nobody is, the streetlights get them
back.

`B3_PHOTO_BOOST_GAIN` was swept the same way as the tails', with two
differences worth recording. The metric is **luminance**, not red — the default
flame is blue and its red channel is a quarter of its blue, and a red-channel
gate measured almost nothing. And the measurement needed a knob that already
existed: under `B3_TESTDRIVE` the player only holds the throttle, so every
flame on a test drive belongs to the AI, and the ledger's own `flamedist`
column says the median burning car is **116 m** from the eye. Three attempts at
this leg pinned frames inside a real burn window and measured exactly zero
moved pixels — a correct light photographed from too far away.
`B3_TEST_PAD_BOOST=1`, in the tree already for `validate_aftertouch`'s
real-input leg, holds the boost button from the same local the pad writes, and
the player then burns two metres from the eye.

| gain | worst frame | best frame |
| ---- | ----------- | ---------- |
| 0.35 | 16          | 31         |
| 0.60 | 33          | 65         |
| 1.20 | 63          | 138        |
| 2.00 | 98          | 174        |

The first cut also had the cone **wider** than the tails' on the argument that
a flame radiates in every direction. It does — and a 75/65 cone from a lamp at
the rear bumper reaches up over the **roof**, which put a blue rim along the top
of the car's own bodywork. 60 across by 45 in elevation is the patch a flame
blasting backwards out of a tailpipe actually lights.

Strips: `build/photo/carlamps/brake_underpass/drive_US_P1_V1.png` (a rival
braking under the deck, tails on beside off),
`build/photo/carlamps/boost_dusk/drive_US_C1_V1.png` (the flame pool on the
road behind, at the dusk-end light curve) and
`build/photo/carlamps/chase_all_lamps/drive_US_C1_V1.png` (head, tail and boost
all live). Gated by section 14 of `tools/validate_photo.py`.

### A stand-down must be a zero, never a skipped bind

An effect's block is spliced into the deferred shader **once**, at build time,
from `g_photo_live[]`. Clearing that flag later does not remove the block from
a program that is already compiled — it removes the block's **texture bind and
its uniform upload**, and the block then runs anyway.

The SSR path did exactly that on a track with no shine mask: it sampled an
unbound `sampler2D`, which returns `(0,0,0,1)` by the spec, and mixed toward it
by the `uSsrAmt` the program was still holding from the last frame that did
upload one. That is `mix(scene, black, 0.85)` over everything the depth cut
does not exclude — **the whole world at a sixth of its brightness**, from a
stand-down that meant to cost one effect.

The shadow already had the right shape a tier earlier
(`g_shadow_tex ? sh_strength : 0`): keep the effect live so its bind and its
upload keep happening, and make the number it uploads **zero**. Then the block
is a multiply by zero, which is what "off" is supposed to mean in an assembled
shader. Tier 7 is safe by the same construction — `b3_photo_set_lights` zeroes
the tail of its arrays every frame, so a slot the caller did not fill this
frame is not a lamp left burning from the street before last.

### What the resize A/B did and did not show

A playtest report said the artifact went away on **resizing the window
mid-race**. Run as an exact A/B — one race with no resize, and the same race
with a sweep out to `1281x721` and back to `1280x720`, so the two frames are
pixel-comparable — the two frames come back **bit-identical**, on the pre-fix
binary, at the shipped defaults. So the resize cure does not reproduce in this
harness, and the mechanisms it would have implicated are ruled out here: the
chain does **not** rebuild its shaders on a resize (`afx_build_all` runs once,
from `afx_init`), a retired chain is retired for the run rather than
re-initialised, and every target the deferred pass samples is either written
every frame or has its term zeroed (which is what the stand-down fix above made
true rather than nearly true).

What that leaves is something about a real windowed context this offscreen
harness does not have. The bisection recipe under section 3 is the way to close
it: whichever `B3_PHOTO_*=0` makes it go away names the tier, and
`B3_PHOTO_SH_CASTERS=none` separates a real cast shadow from everything else.

### The drag, and why one resize was never the test

A later playtest report — "resizing the window on PC freezes the game" — turned
out to be about a property the A/B above could not see, because the A/B did
**one** resize and the report was about a **dragged border**, which is one per
frame.

`render_frame()` read `SDL_GetWindowSize()` every frame and handed it straight
to `b3_afx_frame_begin()`, and `afx_resize()` rebuilds the whole chain whenever
that size differs from the last by any amount at all — fifteen colour targets,
the depth-attachment format probe, the multisampled colour+depth renderbuffer
pair, seventeen framebuffer completeness checks. So the full rebuild was paid
**every frame of every drag**, and the chain has roughly doubled in size since
the resize path was last looked at: the six photorealism targets and the MSAA
pair are all new on it.

Measured on an RTX 3090 (the offscreen SDL driver *is* the desktop GL path),
one resize per frame, ray tracing and all seven effects live:

| | rebuilds | `render_frame` during the drag |
|---|---|---|
| 900-step continuous drag, before | 900 | 6.5 – 8.6 ms |
| 900-step continuous drag, after | 15 | 2.1 – 2.6 ms |
| 600-step oscillation, before | 600 | 6.9 – 9.2 ms |
| 600-step oscillation, after | 0 | — |
| 20-resize flood, before | 20 | — |
| 20-resize flood, after | 1 | — |

At 2020x1540 the rebuild alone was **+7.7 ms a frame**; with the retail
governor running (not `B3_FIXED_DT`) the render rate fell to 26 fps while the
sim held 62 Hz, i.e. the governor spending its full four catch-up ticks on
every rendered frame — which is what a player feels as the game coming apart.

**The fix is a settle, not the web's dead band.** The web has guarded this
since the ResizeObserver work (`B3_WEB_RES_QUANTUM` / `B3_WEB_RES_DEADBAND`);
the desktop never got anything. A dead band leaves the chain permanently
off-size at any window the band swallows, so `resize_settle()` in
`src/burnout3_full.c` waits instead: a new size is adopted once it has held
still for `B3_RESIZE_SETTLE` frames (default 4), **or** unconditionally after
`B3_RESIZE_SETTLE_MAX` frames (default 60) of never holding still. The cap is
the half that matters for a hang rather than a stutter — a drag ends, but a
window size that *oscillates* (fractional scaling rounding against a
compositor's own configure is the classic shape) does not, and without a cap
that is a rebuild every frame for the rest of the run.

While the size is unsettled the chain keeps its old targets and the final
present stretches them over the live window, so a drag is briefly soft and
never a stall. `B3_RESIZE_SETTLE=0` restores the old adopt-immediately engine
exactly, which is what the gate runs as its control leg and what a user can set
to A/B this. `B3_RESIZE_VERBOSE=1` prints what the settle is seeing.

Nothing changes at a settled size — every pinned frame in the tree runs at a
size that has not moved for hundreds of frames, where live *is* settled — and
`B3_PHOTO=0` is byte-for-byte identical to the pre-fix binary (one hash,
`4920659f`, four runs of each).

The gate is `tools/desktop_resize_sweep.py`, the desktop half of
`tools/web_resize_sweep.py`: STEPS (an ordinary resize still rebuilds once per
step), FLOOD (20 resizes in 2 s mid-race, RT on and off), DRAG (one per frame —
the rebuild count must be bounded by the cap), CONTROL (the same with the
settle off, which is where the "120 for 120" number comes from), OSC (a size
that never holds still), PAUSED (a flood with the settings menu open, the one
place a rebuild could tear a target down under the frame drawing into it).
35/35.

---

## 3. The switches

```
B3_PHOTO=0          every effect off, every new pass skipped, every new target
                    unallocated, the depth attachment back to a renderbuffer,
                    and the assembled shader source byte-identical to pre-wave.
B3_PHOTO=1          the default (implicit).

B3_PHOTO_TONEMAP    1     B3_PHOTO_SHADOW    1
B3_PHOTO_SSAO       1     B3_PHOTO_SSR       1
B3_PHOTO_ATMOS      1     B3_PHOTO_GODRAY    1
B3_PHOTO_LIGHTS     1     (tier 7, per-source lights + 7b/7c/7d car lamps)
```

Tier 3's fade target has three of its own, for the A/B that found the storm
defect:

```
                    TIER 3, WHERE THE SKY IS READ FROM
B3_PHOTO_ATM_SKYSRC   1    0 = the compiled-in B3_PHOTO_ATM_SKY_* constant
                           instead of the dome -- which is also what a track
                           with no gradient sheet gets (four ship without one)
B3_PHOTO_ATM_SKYLO 0.01    the dome elevations (yhat) the horizon band is
B3_PHOTO_ATM_SKYHI 0.15    averaged over -- about 0.5 to 8.6 degrees
B3_PHOTO_ATM_VERBOSE  0    print the fitted band and the fit's own residual
                           once a second (+ _DUMPBAND for the raw 64 bins)
```

`B3_PHOTO_HEAD_ON=0` retires the headlight beams alone, leaving the scenery
lamps; `B3_PHOTO_HEAD_CARS=<n>` is how many racers get them. The other two
classes of car lamp have the same pair each:

```
                    TIER 7c, THE TAIL AND BRAKE LAMPS
B3_PHOTO_TAIL_ON      1   0 retires them alone
B3_PHOTO_TAIL_CARS    6   racers whose rear lamps light the road
B3_PHOTO_TAIL_RANGE 7.0   metres the tail pool reaches
B3_PHOTO_BRAKE_REACH 1.5  x that, for the BRAKE lamp -- retail's own 0.75/0.50
                          corona size ratio, carried over to the reach  [S]
B3_PHOTO_TAIL_GAIN   0.22 x the beams' uHeadK gain (so the pause menu's
                          HEADLIGHTS row moves the tails with the beams)
B3_PHOTO_TAIL_SPREAD  70  degrees across
B3_PHOTO_TAIL_CUTOFF  55  degrees in elevation

                    TIER 7d, THE BOOST FLAME'S LIGHT
B3_PHOTO_BOOST_ON      1  0 retires it alone
B3_PHOTO_BOOST_LIGHTS  2  the TRANSIENT pool -- 32 - 18 - 6 - 6.  Not a slot
                          per car: see "the ceiling" under tier 7d
B3_PHOTO_BOOST_RANGE 8.0  metres
B3_PHOTO_BOOST_GAIN  0.35 x the beams' gain, x b3_boostfx_level()
B3_PHOTO_BOOST_FLICKER 0.18  +-18%, from a hash of (frame, slot) -- never rand()
B3_PHOTO_BOOST_SPREAD  60  degrees across
B3_PHOTO_BOOST_CUTOFF  45  degrees in elevation
```

`B3_PHOTO_LIGHT_STATS=1` prints the per-frame light ledger — one line a frame
naming the beams admitted, the racers on track, the tail lamps lit and how many
of those cars are on the brakes (with the slot mask and the nearest one's
distance), the flames and their mask, distance, level and flicker multiplier,
the scenery lamps beside them, the budget, the dusk factor, the head-lamp
coronas the corona pass emitted and the measured lamp height above the road. It
is the instrument that separates the three ways "I cannot see the headlights"
can be true, and no screenshot can tell them apart — and the masks and
distances were added for the same reason one step further on: a light that is
correct but forty metres behind the camera photographs exactly like a light
that was never written, and section 14 lost three attempts to that before the
ledger could tell them apart.

**Tier 4r is not in that table**, and that is the point: it is an *option*
rather than a tier, it is **off by default**, and it lives on the player's
side of the line.

```
the pause menu      SETTINGS -> RAY TRACING: ON / OFF, persisted to
                    build/settings.cfg next to build/mixer.cfg
B3_RT=0 / 1         overrides that file outright, for harnesses -- and says
                    so, so the menu greys the row instead of lying about it
B3_RT_SHOW=1        show the row where a platform hides it
B3_RT_RAYS       4  rays across the sun's disc (clamped to 32; 16 on the web)
B3_RT_STEPS     96  traversal iterations before a ray gives up
B3_RT_SUN_DEG 0.75  the sun's angular RADIUS; 0.265 is the real one
B3_RT_RANGE    900  metres a shadow ray reaches
B3_RT_ORIGIN  0.05  metres the ray leaves the surface by, at the camera
B3_RT_STRENGTH 1.0  x B3_PHOTO_SH_STRENGTH

                    THE FAR FIELD (see tier 4r above).  Each is a BLEND from
                    the pre-fix behaviour, so zeroing all three restores tier
                    4r exactly as it shipped and puts the shimmer back --
                    which is what makes the fix a measurement.
B3_RT_DIST_BIAS 1e-6  metres of extra push per metre^2 of view distance
B3_RT_SLOPE     1.0   blend toward 1/max(N.L, 0.1) -- the sun's grazing angle
B3_RT_GRAZE     1.0   blend toward 1/max(|N.V|, 0.1) -- the VIEW's

                    TIER 4rc, THE CARS' OWN TREES
B3_RT_CARS  racers  which cars the ray may trace: off / racers / all
B3_RT_CAR_N     8   instances the shader array has room for (20 under `all`)
B3_RT_CAR_TRACE 1   0 drops the instances from the RAY while leaving the pose
                    and the blob decision alone -- the caster diagnostic
                    validate_photo section 13 measures against, and the same
                    shape B3_PHOTO_SH_CASTERS has
B3_RT_CAR_WHEELS 1  0 builds carbvh.bin hull-only (a BUILD-time knob)
```

**Every clamp announces itself, once.** A user set `B3_RT_RAYS=128`, saw the
frame rate not move and reported the knob as dead; it was silently 16. And
there is one **unconditional arming line** per process — `[rt] ray tracing ON
-- 4 rays, sun 0.75 deg, world 174402 tris, 6 car models (28352 tris)`, or
`[rt] ray tracing OFF (…)` naming which of the four reasons it is — because
the option is off by default, needs the layer *and* its shadow tier, and can
additionally be refused by the context. From outside, a knob that does nothing
and a feature that is not running look identical.

**MSAA is a settings row too**: `SETTINGS -> MSAA: OFF / 2x / 4x`, persisted to
the same `build/settings.cfg`, with `B3_MSAA` overriding and greying it. A
change rebuilds the chain through the ordinary **resize** path on the next
frame. See the MSAA question under section 4 for why turning it off does not
buy you rays.

It also needs `B3_PHOTO=1` **and** `B3_PHOTO_SHADOW=1`: the ray *replaces*
tier 4's term rather than adding one, so with either off there is nothing for
it to replace and it stands down. `B3_PHOTO=0` is a hard gate over it exactly
as it is over the seven, and that is checked in pixels — see section 5.

**Bisecting a look complaint** is what the per-effect envs are for, and the
recipe is worth writing down because a report of "a dark region" can come from
four of the seven. In order, each on its own run: `B3_PHOTO_SHADOW=0`,
`B3_PHOTO_SSR=0`, `B3_PHOTO_LIGHTS=0`, `B3_PHOTO_SUN_DIRECT=0` (the relight
alone), and `B3_PHOTO_SH_CASTERS=none` (the shadow map with nothing in it).
Whichever one makes it go away names the tier.

`B3_PHOTO=0` is a **hard gate**: an individual switch set under it does not turn
its effect on. That is what makes the pinning strategy sound — a suite sets one
variable and cannot be defeated by a stray env in an operator's shell.

Every magnitude in `src/burnout3_aftereffects.h` is overridable by an
environment variable of the same name; `tools/validate_photo.py` section 1
checks that mechanically, so a constant added without a knob fails the gate.

Diagnostics: `B3_PHOTO_VERBOSE=1` (the sun's projected position, the shine mask
handle, the shadow pass' **caster inventory**), `B3_PHOTO_DEBUG=ao` (the raw
occlusion buffer on the screen), `B3_PHOTO_SH_CASTERS=<subset>` (keep only
`track`, `props`, `scenery` — any combination, or `none` — in the shadow map,
which is how a shadow that should not be there is attributed to the thing that
cast it), `B3_FRAME_PROF=<n>` / `B3_FRAME_PROF_SYNC=1` (the desktop frame
profiler).

---

## 4. Cost

1920×1080, MSAA 4×, `US_C3_V1`, RTX 3090 through SDL's `offscreen` driver,
`B3_FRAME_PROF_SYNC=1` (a `glFinish` before each timestamp, so the number is
the GPU work and not the CPU time spent queueing it):

| leg | render_frame | delta |
|---|---|---|
| `B3_PHOTO=0` (reference) | 2.23 ms | — |
| 1 filmic tonemap + grade | 2.41 ms | +0.18 |
| 2 SSAO | 2.75 ms | +0.52 |
| 3 depth atmospherics | 2.34 ms | +0.11 |
| 4 sun shadow maps + directional | 2.68 ms | +0.45 |
| 5 SSR on shine spans | 2.48 ms | +0.25 |
| 6 god rays | 2.44 ms | +0.21 |
| 7 per-source lights, N=18 | 2.56 ms | +0.00 |
| **all seven** | **3.17 ms** | **+0.60** |

Whole frame all-on **4.54 ms**, i.e. **12.13 ms of headroom** against a 16.67 ms
budget with the existing chain live. The shipped `B3_PHOTO_LIGHT_N` of 18 is
eighteen **scenery lamps**, and the six racers' beams are reserved beside them,
so the array the shader declares is 24. Re-measured at 2048×1536 after that
change: tier 7 is **+0.21 ms** with the beams retired (18 slots) and
**+0.27 ms** with them (24 slots plus the elliptical cone), so the beams' own
share is **0.06 ms**, and all seven hold **114 fps**.

### Tier 4r's cost, at both sizes

Same rig, same pins, `B3_FRAME_PROF_SYNC=1`, medians over the profile windows.
`rt` is the shadow tier alone with the ray answering; `all-rt` is the shipped
default with the option turned on. **Measured after the shadow pass' winding
fix**, so the map it is being compared against is the fixed one — a comparison
against the broken cascade would have flattered the ray, and did while these
numbers were first taken.

**1920×1080, MSAA 4×** (23 windows):

| leg | frame | render_frame | delta | fps |
|---|---|---|---|---|
| `B3_PHOTO=0` (reference) | 3.57 ms | 2.62 ms | — | 280 |
| 4 sun shadow **map** | 3.76 ms | 2.66 ms | +0.04 | 266 |
| **4r RAY-TRACED** | 9.76 ms | 8.77 ms | **+6.15** | 103 |
| all seven (shipped default) | 4.44 ms | 3.44 ms | +0.82 | 225 |
| **all seven + RAY TRACING** | **10.38 ms** | 9.22 ms | **+6.60** | **96** |

**2048×1536 — the size the game BOOTS INTO, and therefore the one that
decides whether this option is shippable** (the same size section 9's blanket
legs use, and for the same reason):

| leg | frame | render_frame | delta | fps |
|---|---|---|---|---|
| `B3_PHOTO=0` (reference) | 4.17 ms | 2.98 ms | — | 240 |
| 4 sun shadow **map** | 4.33 ms | 3.06 ms | +0.08 | 231 |
| **4r RAY-TRACED** | 9.14 ms | 7.83 ms | +4.85 | 109 |
| all seven (shipped default) | 4.91 ms | 3.74 ms | +0.75 | 204 |
| **all seven + RAY TRACING** | **13.13 ms** | 11.92 ms | **+8.94** | **76** |

**76 fps at the user's own resolution with the whole stack live — 3.54 ms of
headroom against the 16.67 ms budget.** That is the tightest number in this
document by a wide margin, and it is the one to quote: the ray is the most
expensive thing here and the only thing here that is optional, and those two
facts belong to each other.

**MSAA does not multiply it**, which is the first thing a reader assumes about
a per-pixel ray. At 1080p the option costs **+5.94 ms** at MSAA 4× and
**+5.37 ms** at MSAA 0 — the same number twice. The deferred pass runs *after*
the multisample resolve, at exactly one sample per pixel, so the traversal is
evaluated once however many samples the scene was drawn with.

| `B3_RT_RAYS` | frame @1080p | fps | |
|---|---|---|---|
| 1 | 5.78 ms | 173 | hard-edged; no penumbra at all |
| 2 | 6.85 ms | 146 | a two-step ramp, and it reads as one |
| **4 (shipped)** | **11.08 ms** | **90** | the first count whose penumbra reads as one |
| 8 | 17.36 ms | 58 | **does not hold 60**, even at 1080p |

The clamp on that knob is **32** on the desktop and 16 on the web, and it is a
clamp rather than a compiled limit — the loop bound is spliced with `%d` and
there never was a fixed 16 anywhere in the traversal. It announces itself now:
a user set `B3_RT_RAYS=128`, watched the frame rate not move, and reported the
knob as dead. It was not dead, it was silently 16.

### Tier 4rc's cost, and the MSAA question

Same rig, `B3_FRAME_PROF_SYNC=1`, **2048×1536 with MSAA 4×**, all seven
effects live and the ray answering:

| leg | frame | render_frame | delta | fps |
|---|---|---|---|---|
| ray tracing, no car trees | 12.01 ms | 10.71 ms | — | 83.3 |
| **+ 6 racers (`B3_RT_CARS=racers`)** | **13.00 ms** | 11.68 ms | **+0.99** | **76.9** |
| + the nearest traffic (`=all`) | 13.24 ms | 11.94 ms | +1.23 | 75.6 |

**Both fit inside the 3.54 ms envelope.** Twenty instances cost 0.24 ms more
than eight rather than two and a half times as much, because the world-space
sphere reject kills the distant ones before they cost a transform — which is
the whole reason the top level is a linear scan and not a structure.

The default stays `racers`. `B3_RT_CARS=all` is worth offering anyway:
**traffic has no blob shadow at all in this port**, so it is the one setting
that *adds* a shadow rather than replacing one.

**"Does turning MSAA off buy me more rays?"** — asked, and the answer is no,
and not by a little. Same rig, same 2048×1536, cars live:

| | frame | fps | |
|---|---|---|---|
| MSAA 4×, 4 rays | 12.93 ms | 77.3 | |
| MSAA off, 4 rays | 12.89 ms | 77.6 | MSAA costs **0.04 ms** |
| MSAA off, 6 rays | 17.59 ms | 56.9 | **misses 60** |
| MSAA off, 8 rays | 22.35 ms | 44.7 | |

MSAA buys back four *hundredths* of a millisecond and one extra pair of rays
costs 4.7 — at this size a ray is **2.35 ms**, flat. The deferred pass runs
after the multisample resolve, so a ray costs the same whatever the sample
count is. Anyone who wants more rays has to spend **resolution**, not samples.
Keep MSAA on; it is now a settings row rather than only an env.

#### The step budget was silently deleting shadows

`B3_RT_STEPS` is the traversal's iteration cap, and it is the one number here
that had to be measured **against quality rather than against cost**, because
getting it wrong is invisible in a frame-time table.

A stackless escape walk cannot order its children, so an any-hit shadow ray
finds its blocker in **depth-first** order rather than nearest-first: the
budget is not "how deep is the tree" but "how many boxes does this ray cross
before it meets something". A ray that runs out returns the transmittance it
has — which is *lit*. So a cap set too low does not cost frame time. It
**deletes shadows**.

Counted over 700 ground rays fired at each track's own sun, on the rays a cap
can cost anything at all (the ones that were going to be **blocked**):

| cap | `US_C3_V1` | `AS_M1_V1` (853k tris) | cost, 1080p all-on |
|---|---|---|---|
| 96 | **15.1 % lost** | **28.5 % lost** | 8.88 ms |
| 128 | 2.4 % | 11.9 % | 10.70 ms |
| 192 | 0.0 % | 1.2 % | 10.91 ms |
| **256 (shipped)** | **0.0 %** | **0.0 %** | **11.24 ms** |
| 384 | 0.0 % | 0.0 % | 11.47 ms |

The first cut shipped 96 and looked fine, because the frame it was tuned on
happened not to care: `US_C3_V1` frame 420 moves **0.009 %** of its pixels
between a 96-step and a 384-step budget. Frame **1200** on the same track
moves **16.8 %** of them and **−1.60 levels** of mean luma. One pinned frame
is not a measurement of a traversal budget, and this is the second time this
document has had to say so.

The cost flattens at about the place the loss reaches zero, and for the same
reason: past ~200 the cap has stopped binding and the walk is ending on its
own — 256 to 384 buys 0.23 ms of nothing. So 256 is the smallest value that
loses nothing on any shipped track.

#### The leaf size turned out to be a WEB decision

`CXV_LEAF_TARGET` went from 4 to 8, which nearly halves the node count
(117k → 65k on `US_C3_V1`, 547k → 302k on `AS_M1_V1`) and takes the artefact
from 13.3 MB to 10.9 MB (64.1 → 52.9 on the worst track). It also takes the
96-step loss above from 19.8 % to 15.1 %, because a shallower tree is fewer
box tests per blocker.

On the **desktop** it is worth nothing measurable — the all-on leg reads
10.82 ms before and 11.01 ms after, one run apart. Eight triangle tests
instead of four is ALU the 3090 does not notice.

On the **web** it is worth a great deal, because a node visit there is two
`texture2D` fetches and the fetches are the bill rather than the arithmetic.
That is the shape of this port's whole web story and it is nice to meet it
again here.

*(And a note for whoever measures this next: in ISO mode the game reads its
artefacts out of `build/.isocache`, not out of `build/tracks`. Re-extracting
into `build/tracks` and then measuring changes **nothing at all** — a whole
leaf-size A/B was run that way before the stale cached tree was noticed, and
it dutifully reported the two configurations as identical, which they were,
because they were the same file. `rm build/.isocache/tracks/*/bvh.bin
build/.isocache/.stamps/T_bvh_*` and let the engine re-materialise.)*

### THE WEB, and it came free

The brief was "only if it comes free": one shader that compiles on both, no
web-specific optimisation, and the option appears there only if it holds the
frame. Both halves came out yes.

The ESSL 1.00 traversal compiles under **WebGL 2 unchanged** — there is no
`#ifdef` in it, exactly as there is none anywhere else in the chain — and
`RGBA32F` + `NEAREST` is core there. Measured through
`tools/web_smoke.py --gl hw` (headless Chromium on ANGLE/Vulkan, the same
RTX 3090), MSAA 4×, whole stack on, at both sizes:

| | | fps | render_frame median | p90 |
|---|---|---|---|---|
| 1920×1080 | ray tracing OFF | **60.0** | 10.39 ms | 11.60 |
| 1920×1080 | ray tracing **ON** | **60.0** | 11.01 ms | 12.09 |
| 2048×1536 | ray tracing OFF | **60.0** | 8.72 ms | 9.12 |
| 2048×1536 | ray tracing **ON** | **60.0** | 10.63 ms | 11.07 |

**Every in-race window at both sizes and both settings sits on the 60 Hz
vsync.** So `B3_RT_WEB_OPTION` is 1 and the row is offered on the web too.

Two honest caveats about that table. The web harness's run-to-run spread is
about ±2 ms — larger than the gap between the two settings and larger than
the gap between the two SIZES, which is why 2048×1536 reads *faster* than
1080p there and should not be read as anything. And the ray costs far less
on the web relative to the map than it does on the desktop, for a reason
worth naming: what the option REMOVES on the web is a second full geometry
pass over the track, the props and the scenery, and draw calls are what the
web pays for. The desktop hardly notices that pass; the web does.

(Hiding the row would never have disabled the feature: `B3_RT=1` still runs
it anywhere it can, which is what keeps that measurement repeatable.)

Three smokes and the resize sweep are green with the option in the build:
`web_smoke` 13/13 (four runs), `web_shell_smoke` 6/6, `web_cold_smoke` 4/4
including the damaged-cache heal, `web_resize_sweep` 7/7.

#### The soak

304 s, **28,800 frames**, 1080p, MSAA 4×, whole stack, **ray tracing on**,
vsync off so the frame is not pinned by the scanout: **frame median 11.23 ms
(89 fps), worst window 12.88 ms (78 fps)**, zero FATALs, one `chain ready`
line and one `shadow(RAY` line (so no rebuild churn), and **RSS flat at
751 MB for the whole of it** — the one step near the end is the EA TRAX
streamer taking its next song, which it does with the option off as well.

### Tier 7's cost against N

The question this tier raises is not what it costs but what **one more light**
costs, and at 1080p on this GPU the answer is under the measurement floor: the
whole range N = 0…32 fits inside the ±0.25 ms run-to-run spread. Measured at
**3840×2160** instead, where the deferred pass is four times the fill and the
slope is visible, three runs per point, medians:

| N | render_frame | delta | per light |
|---|---|---|---|
| 0 | 3.68 ms | — | — |
| 4 | 3.73 ms | +0.05 | 0.013 ms |
| 8 | 3.76 ms | +0.08 | 0.010 ms |
| 12 | 3.82 ms | +0.14 | 0.012 ms |
| 16 | 3.85 ms | +0.17 | 0.011 ms |
| 24 | 3.94 ms | +0.26 | 0.011 ms |
| 32 | 4.02 ms | +0.34 | 0.011 ms |

Linear, at **0.0106 ms per light at 4K** — i.e. about **0.003 ms per light at
1080p**. Confirmed at 1080p by the same three-run method: N=0 2.60 ms, N=12
**2.67 ms (+0.07)**, N=32 2.97 ms. The shipped budget of 12 is therefore not a
performance decision at all; it is a *look* decision (past a dozen the pools
start overlapping into a lift) and the headroom to double it is there.

Confirmed by a soak at 1080p on `US_P1_V1` with all seven on — 33 profile
windows, **330 s of held frames**: **60.0 fps in every window**, `render_frame`
4.71–9.25 ms under the real frame governor, **RSS flat at 263–264 MB**, one
`chain ready` line, one `shadow map` line, zero chain declines, zero FATALs.

(A first attempt at this measurement read 59.2 fps in one window and an RSS
alternating between 263 and 488 MB. Both were the harness: two soak runs had
been left overlapping, and the RSS sampler's `pgrep` was matching its own
command line. The numbers above are from a run with nothing else on the
machine, and the flat RSS is the answer to the question the bogus one raised.)

The per-effect deltas do **not** sum to the all-on delta (1.80 against 0.86),
and that is the architecture showing through rather than a measurement error:
five of the seven share one deferred pass and one depth texture, so a lone
effect pays for infrastructure the others would otherwise have shared with
it.

---

## 5. Gates

Results at the time of writing: `validate_photo` **246/247** — `B3_PHOTO=0`
renders `US_C3_V1` frame 400 **byte-for-byte identical** (`aa802238`) to a
build of the wave's own base commit. Point `B3_PHOTO_REF_BIN` at one and the
cross-build legs run.

The one red is section 11's *"the option is invisible when off"* leg, and it is
**expected and discharged rather than outstanding.** That leg compares the
*whole* photo stack against the reference binary, so any deliberate rendering
change on this side turns it red — its own note says so, and it cannot tell
"somebody added a tier" from "tier 4r is leaking". Tiers 7c and 7d are exactly
such a change. What closes the question is **section 14.1c**, which retires
just those two tiers and gets the reference's own bytes back (`0ddba560`): the
difference section 11 measured is 7c and 7d and nothing else in the stack has
moved. **Build the reference from the CURRENT master**, not an older commit.

**Two kinds of off, and the gate had to learn the difference.** 14.1c was first
written with `B3_PHOTO_TAIL_ON=0 B3_PHOTO_BOOST_ON=0` and failed on a correct
tree. Those switches retire the **lights**; they deliberately do not shorten
the shader's uniform array, because an array whose length depends on the
frame's contents makes the two legs of every A/B compile different programs.
With them off the budget is still 32, so the street spills into the spare slots
— and even on a frame where it does not (frame 400 finds only 13 lamps in
range, under either budget, so the light *contents* are identical) the picture
still moves by **75 pixels and up to 17 levels**. The loop bound is a
compile-time constant, and thirty-two additions of zero do not associate the
way twenty-four do. A zeroed slot is free in arithmetic and not in floating
point. `B3_PHOTO_TAIL_CARS=0 B3_PHOTO_BOOST_LIGHTS=0` retires the
**reservations**, restores the 18 + 6 budget and the old shader text, and gives
back the old frame. Anyone bisecting a pixel difference against a pre-7c build
wants `_CARS`, not `_ON`.

Suite battery, all under the `B3_PHOTO=0` pin and with tier 4r at its default
of OFF: `bvh` 110/110, `carfx` 270/270, `postfx` 179/179, `draw_distance`
12/12, `scenery` 315/315, `hud` 769/769, `no_baked_data` 90/90, `light_probes`
61/61, `props` 337/337, `boostfx` 77/77, `particlefx` 600/600, `port`
154/154, `tracks` 322/324 (two pre-existing AI "car stuck" failures, no
rendering involved), `car_shine` 40/45 against a **pre-wave baseline of 41/45**
on the same machine — i.e. car_shine is already failing in this tree and the
one-check difference is inside its run-to-run spread, and its five failures are
the same five (highlight band, reflection-versus-sky, the `EU_C3_V1` mask, the
veil).

**THE OPTION IS PINNED OFF FOR THE WHOLE SUITE, in one line at the top of
`main()`**, and that is not tidiness. Tier 4r persists to
`build/settings.cfg`, which lives in the directory these runs use as their
CWD, and with the option on the depth-map pass is **skipped entirely** — so
section 7's caster inventory and section 9's coverage line simply stop being
printed and two legs go red for a reason that has nothing to do with either.
A leftover `raytracing 1` did exactly that here. Several legs build their
environment by hand and one of them will always be forgotten; pinning it once,
centrally, is the only version of this that stays true.

Web: all three smokes green (`web_smoke` 13/13 over four runs,
`web_shell_smoke` 6/6, `web_cold_smoke` 4/4 including the damaged-cache heal)
and the resize sweep green (7/7) with the ray-tracing option in the build.
Tier 7 adds no target and no pass: what it adds to the web is two uniform
arrays, and the shader that declares them **halves its own budget and retries**
if a context will not take them — GLES2, and therefore WebGL 1, guarantees only
sixteen fragment uniform vectors, and "the whole deferred pass would not build"
is much too big a price for a budget that was set too high. Tier 4r adds two
data TEXTURES rather than a target, and a context that cannot make a float one
loses the option and keeps the game.

* `tools/validate_photo.py` — eleven sections. The switches **executed** through
  the GL-free half of the module; the bit-identity leg against a reference
  binary — which now runs that binary with `B3_PHOTO=0` as well, because
  handing it an empty environment was right exactly once (for a build from
  before the wave, which has no `B3_PHOTO` to read) and made every LATER
  reference render with the wave ON, reporting 97.4% of pixels differing on
  two builds that are identical; one executed leg per effect asserting it moves pixels in the region
  and direction it claims; the HUD untouched; the sky untouched by the four
  depth-gated effects; **the sun shadow on the road** (section 7 — the
  offset's bound as a law in the source, the coverage boundary resolving LIT,
  the road not shadowing itself on two DRIVING frames, and the caster list
  inspected as a draw list); **the per-source lights** (section 8 — the
  provenance written where it can be found, no track id anywhere near the
  derivation, a second track deriving a different field from the same binary,
  the dusk factor ordering two tracks by their own suns, day restrained under
  three levels and dusk worth more than twice it, and both budgets honoured at
  once — the lamps keep their whole N and the beams are reserved beside it);
  **the near-field blanket** (section 9) and **the beams** (section 10); and
  **tier 4r** (section 11 — the switch executed through a GL-free probe with
  real environments and a real config file, the shipped C traversal run
  against the shipped `bvh.bin` and asked how much of the track is shadowed
  from beyond the cascade's box, the two shadow blocks' shared text checked
  character-for-character so a fix to one cannot miss the other, both
  bit-identity legs, and the pause MENU driven end to end through the real
  SDL queue — toggle, renderer change in the same run, file written, choice
  still there in the next process);
  **the beams over a DRIVE** (section 12 — the engine's own per-frame light
  ledger asserting every racer's beam is admitted in every frame of a real
  lap, and the post-tonemap road-band delta clearing a visibility floor on the
  worst of six moments of ordinary chase driving, on a day track and a dusk
  one, at the user's own 2048×1536 — the section that exists because a curated
  tunnel frame said the feature worked while the player could not see it);
  and temporal stability on a **pinned camera**, which tier 4r's screen-locked
  cone dither is held to as well.
* `tools/photo_strip.py` — the contact sheet and the tuning loop.
  `--drive <ENV>` swaps the pinned frame for six frames over ten seconds of
  ordinary chase driving, the same drive rendered twice with that switch on
  and off, before beside after: the instrument for "would a player notice",
  where the pinned sheet answers "did the pixels move".
* `tools/photo_perf.py` — the frame-ms table above; `rt` and `all-rt` are
  tier 4r's legs.
* `tools/validate_bvh.py` (`make test-bvh`) — the BVH artefact's own gate,
  and it needs neither GL nor a game binary: the tree's invariants, the float
  packing the ESSL 1.00 traversal depends on, a byte-diff of two runs for
  determinism, the triangle multiset re-derived from `track.obj` /
  `props.bin` / `scenery.bin` by an independent implementation, and a grid of
  rays traced through the flattened tree against a brute-force test over
  every triangle.

**The harness flips a coin**, and the suite had to learn it: a single pinned
render is not reproducible in this tree (ten runs of one binary with an
identical environment produced five distinct frames, differing over 700,000
pixels — i.e. different *moments*, not different pixels). This is pre-existing
and `tools/afx_sweep.py`'s `pin_ok` was written about the same failure. Every
comparison in `validate_photo` is therefore pinned twice over: a world check on
edge overlap, and a **set intersection** for bit identity rather than one lucky
pair.
