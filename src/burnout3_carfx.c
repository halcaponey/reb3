/* Car appearance FX -- see burnout3_carfx.h for the contract and
 * docs/RE_CARFX.md for the full evidence chain.
 *
 * Every numeric constant below is annotated with the VA it was read from in
 * build/burnout3.elf.  Nothing here is eyeballed: the whole car-draw path was
 * re-executed under Unicorn
 * (scratchpad carfx/trace_carfx.py, which extends tools/trace_panels.py's
 * --deep harness with hooks on the D3D constant setters) and the captured
 * uploads are what the tables below reproduce.
 *
 * Marks: [C] execution/byte-verified, [S] read from disassembly, [?] open,
 *        GLUE = this port's own bridging, not the game's.
 */
#include "burnout3_carfx.h"
/* the retained renderer publishes the frame camera, so the billboard passes
 * do not have to read it back out of the driver */
#include "burnout3_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <GL/gl.h>
#ifdef __ANDROID__
/* ANDROID PORT (android/): desktop GL there is gl4es on top of GLES2, so the
 * GL 2.0 entry points below must be resolved by gl4es' OWN lookup --
 * SDL_GL_GetProcAddress would hand back the system driver's GLES2 symbols,
 * which sit outside the program and uniform state gl4es maintains. */
#include <gl4esinit.h>
#define SDL_GL_GetProcAddress gl4es_GetProcAddress
#endif
/* WEB PORT (web/): gl4es IS GONE from this link, and so is the override.  The
 * only reason one was ever needed is that gl4es MANGLED its exports to
 * gl4es_gl* under __EMSCRIPTEN__ to avoid colliding with Emscripten's own
 * WebGL symbols, so its entry points had to be found through its own lookup.
 * Those Emscripten symbols are now the ones the engine wants, and plain
 * SDL_GL_GetProcAddress -- the same call the desktop uses -- returns them.
 * <GL/gl.h> resolves to web/GL/gl.h, which is GLES2.  See that file. */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================================================================
 * 1. Recovered constants
 * ====================================================================== */

/* FUN_000313E0 (0x000313E0) -- Ramamoorthi & Hanrahan "An Efficient
 * Representation for Irradiance Environment Maps" (SIGGRAPH 2001) constants,
 * read straight out of the ELF.  [C: the five immediates the function
 * multiplies by, and the full 16-float matrix it produces, were reproduced
 * exactly by executing it.] */
#define B3FX_SH_C1  0.4290429949760437f   /* 0x003868F0 (neg: 0x003B1AE0) */
#define B3FX_SH_C2  0.5116639733314514f   /* 0x003868F4 */
#define B3FX_SH_C3  0.7431250214576721f   /* 0x003868F8 */
#define B3FX_SH_C4  0.886227011680603f    /* 0x003868FC */
#define B3FX_SH_C5  0.247708f             /* used at 0x00031474 (c4*L0-c5*L6)*/

/* ---------------------------------------------------------------------
 * The nine SH coefficients themselves -- modelInstance+0x5C .. +0x7C.  [C]
 *
 * These are NOT computed at runtime and they are NOT per track: they are
 * nine literal floats in .rdata, stored into every car's model instance
 * immediately before its draw.  THREE independent sites write exactly the
 * same nine, which is what makes the reading safe:
 *
 *   FUN_000C047C..0x000C04F5   [ebx+0x6C..0x8C]   in-race player/AI car obj
 *   FUN_001A74C5..0x001A755A   [ecx+0xCC..0xEC]   the other car instance
 *   FUN_001AE93C..0x001AE9AD   [esi+0x6C..0x8C]   front-end showcase
 *
 * (the car object's model instance is at +0x10, so +0x6C == instance+0x5C,
 *  and FUN_001A4710 passes `obj+0x70` -- +0xCC == instance+0x5C too.)
 *
 * i  VA          value          R-H basis   (FUN_000313E0's slot, mapped by
 * -- ----------- -------------- ----------   executing it with each e_i)
 * 0  0x003B1900   0.3125        L00         -> M33 = c4*L00 - c5*L20
 * 1  0x003B193C   0.247825757   L1-1  (y)   -> M13/M31
 * 2  0x003B1938  -0.0931382477  L10   (z)   -> M23/M32
 * 3  0x00384A80   0.15          L11   (x)   -> M03/M30
 * 4  0x003B1934   0.146874994   L2-2  (xy)  -> M01/M10
 * 5  0x003B1930  -0.0851906464  L2-1  (yz)  -> M12/M21
 * 6  0x003B192C  -0.0785328001  L20         -> M22, and -c5 into M33
 * 7  0x003B1928  -0.118749999   L21   (xz)  -> M02/M20
 * 8  0x003B1924   0.065624997   L22         -> M00 / -M11
 *
 * E(n) = n^T M n over the resulting matrix (validate_carfx.py section 9):
 *   +Y 0.52185   -Y 0.01464   +X 0.47805   -X 0.17106   +Z 0.14273
 *   -Z 0.33335 -- a top-lit studio probe, dark underneath.  The maximum over
 *   the whole sphere is 0.68824, at n = (0.650, 0.672, -0.354).
 *
 * The front-end showcase draws the car twice (FUN_001AE6F0): the second
 * draw is the upright car with the set below; the first is its floor
 * reflection, which negates m11 and ty (0x001AE909..0x001AE91C) and uses
 * the same nine with i = 1, 5 and 7 negated (0x003B204C / 0x003B2048 /
 * 0x003B2044).  An exact mirror of the R-H form in y would negate i = 1, 4
 * and 5, so that second set is hand-authored rather than derived  [C values,
 * ? interpretation]. Only the upright set below is ever used in a race. */
const float B3_CARFX_SH_RETAIL[9] = {
    0.3125f,          /* 0x003B1900 */
    0.247825757f,     /* 0x003B193C */
    -0.0931382477f,   /* 0x003B1938 */
    0.150000006f,     /* 0x00384A80 */
    0.146874994f,     /* 0x003B1934 */
    -0.0851906464f,   /* 0x003B1930 */
    -0.0785328001f,   /* 0x003B192C */
    -0.118749999f,    /* 0x003B1928 */
    0.065624997f      /* 0x003B1924 */
};
/* the showcase's floor-reflection variant, for completeness */
const float B3_CARFX_SH_RETAIL_MIRROR[9] = {
    0.3125f, -0.247825757f, -0.0931382477f, 0.150000006f, 0.146874994f,
    0.0851906464f, -0.0785328001f, 0.118749999f, 0.065624997f
};

/* ---------------------------------------------------------------------
 * The light RGB -- DAT_0060E0A0, i.e. pixel-shader c14.xyz.  [C]
 *
 * It has no writer of its own because it is a FIELD, not a variable: the
 * environment object lives at 0x0060E040 and DAT_0060E0A0 is its +0x60.
 * FUN_001888F0 (0x001888F0) loads the track's "enviro.dat" (the string at
 * 0x003B0444, the same file docs/RE_POSTFX.md decodes for the sky) and
 * FUN_00188C00 called at 0x00188A40 copies the file's first 0xB0 bytes over
 * the object -- so c14.xyz is literally `enviro.dat` bytes 0x60..0x6B.
 * FUN_00188880 then relocates +0x98..+0xA4, which is exactly the sky-art
 * quartet RE_POSTFX.md documents: same file, same record, same offsets.
 *
 * Every shipped enviro.dat stores it as an 8-bit colour; the table is the
 * byte-read value for all 37 track directories and validate_carfx.py
 * section 10 re-reads the files to prove it. */
typedef struct { const char* track; float rgb[3]; } B3FxEnvLight;
static const B3FxEnvLight B3FX_ENV_LIGHT[] = {
    { "AS_C1_V1", { 1.0f, 1.0f, 0.909803987f } },                   /* 255,255,232 */
    { "AS_C1_V2", { 1.0f, 1.0f, 0.909803987f } },                   /* 255,255,232 */
    { "AS_C2_V1", { 1.0f, 1.0f, 0.909803987f } },                   /* 255,255,232 */
    { "AS_C2_V2", { 1.0f, 1.0f, 0.909803987f } },                   /* 255,255,232 */
    { "AS_C3_V1", { 1.0f, 1.0f, 1.0f } },                           /* 255,255,255 */
    { "AS_C3_V2", { 1.0f, 1.0f, 1.0f } },                           /* 255,255,255 */
    { "AS_M1_V1", { 0.984313786f, 0.894117713f, 0.549019635f } },   /* 251,228,140 */
    { "AS_M1_V2", { 0.984313786f, 0.894117713f, 0.549019635f } },   /* 251,228,140 */
    { "EU_C1_V1", { 1.0f, 1.0f, 1.0f } },                           /* 255,255,255 */
    { "EU_C1_V2", { 1.0f, 1.0f, 1.0f } },                           /* 255,255,255 */
    { "EU_C2_V1", { 0.913725555f, 0.878431439f, 0.878431439f } },   /* 233,224,224 */
    { "EU_C2_V2", { 0.913725555f, 0.878431439f, 0.878431439f } },   /* 233,224,224 */
    { "EU_C3_V1", { 0.992156923f, 0.941176534f, 0.835294187f } },   /* 253,240,213 */
    { "EU_C3_V2", { 0.992156923f, 0.941176534f, 0.835294187f } },   /* 253,240,213 */
    { "EU_C4_V1", { 0.996078491f, 0.996078491f, 0.929411829f } },   /* 254,254,237 */
    { "EU_C4_V2", { 0.996078491f, 0.996078491f, 0.929411829f } },   /* 254,254,237 */
    { "EU_M1_V1", { 0.917647123f, 0.882353008f, 0.835294187f } },   /* 234,225,213 */
    { "EU_M1_V2", { 0.917647123f, 0.882353008f, 0.835294187f } },   /* 234,225,213 */
    { "EU_M2_V1", { 0.992156923f, 0.82745105f,  0.776470661f } },   /* 253,211,198 */
    { "EU_M2_V2", { 0.992156923f, 0.82745105f,  0.776470661f } },   /* 253,211,198 */
    { "EU_P1_V1", { 0.90196085f,  0.874509871f, 0.792156935f } },   /* 230,223,202 */
    { "EU_P1_V2", { 0.90196085f,  0.874509871f, 0.792156935f } },   /* 230,223,202 */
    { "EU_P2_V1", { 0.996078491f, 0.960784376f, 0.772549093f } },   /* 254,245,197 */
    { "EU_P2_V2", { 0.996078491f, 0.960784376f, 0.772549093f } },   /* 254,245,197 */
    { "US_C1_V1", { 1.0f,         0.984313786f, 0.874509871f } },   /* 255,251,223 */
    { "US_C1_V2", { 1.0f,         0.984313786f, 0.874509871f } },   /* 255,251,223 */
    { "US_C2_V1", { 0.984313786f, 0.929411829f, 0.733333349f } },   /* 251,237,187 */
    { "US_C2_V2", { 0.984313786f, 0.929411829f, 0.733333349f } },   /* 251,237,187 */
    { "US_C3_V1", { 0.992156923f, 0.894117713f, 0.674509823f } },   /* 253,228,172 */
    { "US_C3_V2", { 0.992156923f, 0.894117713f, 0.674509823f } },   /* 253,228,172 */
    { "US_C5_V1", { 1.0f,         0.909803987f, 0.823529482f } },   /* 255,232,210 */
    { "US_M1_V1", { 0.937254965f, 0.741176486f, 0.56078434f } },    /* 239,189,143 */
    { "US_M1_V2", { 0.937254965f, 0.741176486f, 0.56078434f } },    /* 239,189,143 */
    { "US_P1_V1", { 1.0f,         0.650980413f, 0.549019635f } },   /* 255,166,140 */
    { "US_P1_V2", { 1.0f,         0.650980413f, 0.549019635f } },   /* 255,166,140 */
    { "US_P2_V1", { 0.984313786f, 0.972549081f, 0.866666734f } },   /* 251,248,221 */
    { "US_P2_V2", { 0.984313786f, 0.972549081f, 0.866666734f } },   /* 251,248,221 */
};
#define B3FX_ENV_LIGHT_N ((int)(sizeof B3FX_ENV_LIGHT / sizeof B3FX_ENV_LIGHT[0]))

/* Shine table at 0x0045BB20, written by FUN_00030150 (0x00030150).  Eight
 * entries each -- one per paint/colour variant (modelobj+0x59) -- and the
 * game ships all eight identical.  [C: dumped after executing FUN_00030150
 * under Unicorn.]
 *    +0x08 + i*4  P  -> pixel-shader c14.w
 *    +0x28 + i*4  K  -> pixel-shader c15.z = 1-K
 *    +0x48 + i*4  M  -> pixel-shader c15.w = 0.25*M/(1-K)
 *    +0x68        glass K        +0x6C  glass M                            */
#define B3FX_SHINE_VARIANTS 8
static const float B3FX_P[B3FX_SHINE_VARIANTS] = {
    0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f };   /* 0x3DCCCCCD */
static const float B3FX_K[B3FX_SHINE_VARIANTS] = {
    0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f, 0.35f }; /* 0x3EB33333 */
static const float B3FX_M[B3FX_SHINE_VARIANTS] = {
    0.7f, 0.7f, 0.7f, 0.7f, 0.7f, 0.7f, 0.7f, 0.7f };   /* 0x3F333333 */
#define B3FX_GLASS_K 0.22499999403953552f   /* 0x0045BB88 = 0x3E666666 */
#define B3FX_GLASS_M 0.925000011920929f     /* 0x0045BB8C = 0x3F6CCCCD */
/* the alt-material branch (record+0x14 != 0, shader DAT_004D6554) replaces
 * the table's P with this immediate                                        */
#define B3FX_ALT_P   0.5f                   /* 0x003B1684 */
/* the glass pass-2 branch uses this immediate instead of the table's P     */
#define B3FX_GLASS_P 0.800000011920929f     /* 0x003A5600 */
/* the 0.25 the game folds into c15.w; it is the reciprocal of the NV2A
 * register combiner's 4x output scale, so it cancels in hardware. [S]      */
#define B3FX_COMBINER_RECIP 0.25f           /* 0x003B1730 */

/* Vertex-shader constant c15 (hw slot 111), written by FUN_0002EF90 into the
 * global draw context 0x004D6C90 at +0x360 (body) and +0x350 (glass).  Both
 * are (R0, 1-R0, 1, 1) -- a Schlick base-reflectance pair: R0=0.1836 solves
 * to IOR 2.4993 (car paint / clearcoat), R0=0.0399 to IOR 1.4996 (glass).
 * [C values, S interpretation.] */
#define B3FX_BODY_R0  0.1835709810256958f   /* 0x003B18D4 (pair 0x003B18CC) */
#define B3FX_GLASS_R0 0.039943765848875046f /* 0x003B18D0 (pair 0x003B18C8) */

/* Glass pass 2's stage-0 alpha constant.  Its PSC0Mapping nibble is 5 and no
 * upload ever writes pixel register c5 on that path, so the value used is the
 * def's OWN PSConstant0[0] literal: 0x4DE5B2E5 at VA 0x003E83A0, unpacked as
 * D3DCOLOR (a,r,g,b) = (0x4D, 0xE5, 0xB2, 0xE5)/255.  The combiner computes
 * `r0.a = c5.b*v0.a + c5.a`, i.e. the glass reflection = 0.898*F + 0.302 --
 * the same "floor plus Fresnel" shape the body gets from c14.w.        [C] */
#define B3FX_GLASS_C5_B 0.8980392156862745f  /* 0xE5/255 -> c5.b */
#define B3FX_GLASS_C5_A 0.30196078431372547f /* 0x4D/255 -> c5.a */

/* ======================================================================
 * 1b. TUNED look magnitudes -- TUNED (user-authorized deviation 2026-08-13)
 *
 * The user relaxed the 1:1 parity requirement FOR THE LOOK ONLY:
 *   "we dont need 1:1 pairity to the retail version just make it glossy and
 *    the enviroement properly lit and cars react correctly but they do not
 *    need to do it the exact same way as retail."
 *
 * Every recovered SHAPE above is untouched -- the Fresnel mix along
 * R = 2N(N.V) - V, the per-texel gloss mask in paint.a, the P floor, the
 * (1-K)/M specular threshold.  What follows are three weights layered ON TOP
 * of that shape, and only these three, because the recovered composition
 * reads wrong in this harness for two reasons the user reported from live
 * play:
 *
 *   1. the environment sheet (enviro.dat +0xA0, the [S] stand-in for the
 *      unrecoverable DAT_004D6C00) is a BRIGHT DAYLIGHT SKY, and it is
 *      applied at full amplitude everywhere -- so a red car turns chrome-pink
 *      over its whole body (debug dumps 017 / 018) and carries a sky
 *      reflection inside a tunnel, where there is no sky.  Retail's captures
 *      show deep saturated paint face-on with the sheen concentrated at
 *      grazing angles.
 *   2. the probe-driven diffuse does sweep ~2x over a lap (recovered, and it
 *      works), but the swing is too small to read as "the car reacts to the
 *      lights it drives past".
 *
 * So: REFL_GAIN scales the whole reflection layer, REFL_FLOOR scales the
 * constant P part of it only (which is what makes a face-on panel chrome),
 * ENV_SHADE_* ties the reflection's amplitude to the LOCAL probe -- a dark
 * cutting or a tunnel dims the sky sheen the way a real environment would --
 * and PROBE_CONTRAST expands the probe's own dynamic range about the track
 * mean.  All five are magnitudes; none of them changes what is multiplied by
 * what.  B3_CARFX_TUNE=0 restores the untuned recovered magnitudes exactly.
 * ====================================================================== */
#ifndef B3FX_T_REFL_GAIN
#define B3FX_T_REFL_GAIN      0.40f  /* overall reflection weight            */
/* SHINE-HOT: 0.18 -> 0.40, and the RAISE is the point.  0.50 -> 0.35 -> 0.25
 * -> 0.18 was four attempts to hold down a layer whose hot half (the additive
 * specular, section 1c) this constant does not reach, so each round only
 * starved the half that was already dim -- at 0.18 the reflection layer
 * measures a mean of 12.0 against 29.3 for the BROKEN raw lookup the SHINE-UV
 * wave was written to replace, i.e. the cars were on their way back to flat.
 * With the specular on the same gain, the gate fed the right quantity and the
 * sheet in render-target space, the layer can carry its proper weight again.
 * 0.40 is the value MEASURED to land inside retail's own daylight band, and
 * the sweep is monotone (US_C3_V1, gloss-attributed, see the retail table in
 * tools/validate_car_shine.py):
 *      gain   layer mean   peak    veil%   clip%
 *      0.18       12.0    168.7    0.01    0.00
 *      0.35       18.6    171.1    0.18    0.01
 *      0.40       20.5    185.9    0.37    0.05     <-- inside retail's band
 *      0.45       22.4    200.4    0.51    0.09
 *      0.50       24.3    216.5    0.58    0.11
 *   retail, 9 xemu frames         181.2..214.0  <=0.50  <=0.01
 * 0.40 is the only entry whose peak is inside the retail daylight band with
 * the veil still under retail's worst frame.                                */
/* SHINE: 0.50 -> 0.35.  The 0.50 was set against a reflection layer that
 * was a flat WASH (the stage-1 lookup landed outside the sheet on 86-88%
 * of the body and read one clamped border texel), and against a shade ramp
 * that then halved it again.  With both fixed the layer has structure and
 * reaches full weight on ordinary road, so the same look needs less gain.
 * Measured: US_C3_V1 body reflection layer 47 -> 72 mean / 45 -> 58 spatial
 * std (tools/validate_car_shine.py).                                     */
#endif
#ifndef B3FX_T_REFL_FLOOR
#define B3FX_T_REFL_FLOOR     0.30f  /* extra scale on the P floor alone     */
#endif
#ifndef B3FX_T_ENV_SHADE_MIN
#define B3FX_T_ENV_SHADE_MIN  0.10f  /* sheen left at the track's darkest probe */
#endif
#ifndef B3FX_T_ENV_SHADE_POW
#define B3FX_T_ENV_SHADE_POW  1.15f  /* ramp shape over [probe min, probe max]  */
#endif
#ifndef B3FX_T_PROBE_CONTRAST
#define B3FX_T_PROBE_CONTRAST 1.70f  /* exponent on L00/L00_mean: 1 = recovered */
#endif
/* SHINE-SHADE: where the shade ramp's TOP sits between the track's MEAN
 * probe (0) and its BRIGHTEST probe (1).  Anchored on the brightest -- the
 * shipped behaviour, B3FX_T_SHADE_TOP = 1 -- the ramp stops being a shade
 * cue and becomes a global dimmer: measured on US_C3_V1 the car drives at
 * L00 = 0.2094 in a track range [0.1500, 0.2875], i.e. 43% up the ramp, so
 * BOTH the reflection lerp and its specular ran at 0.44 on ordinary
 * daylight road and nothing the car ever drove on reached full sheen.
 * Anchored on the mean the average-lit car carries the full recovered
 * layer (which is also where the sibling probe-contrast gain is exactly 1)
 * and the ramp does only what it is for -- dimming BELOW the mean, in a
 * cutting or a tunnel.  Both anchors are the track's own shipped probe
 * statistics; neither is a constant or a per-track number. */
#ifndef B3FX_T_SHADE_TOP
#define B3FX_T_SHADE_TOP      0.0f
#endif
#ifndef B3FX_T_PROBE_LO
#define B3FX_T_PROBE_LO       0.45f  /* clamps on the contrast gain, so an   */
#endif
#ifndef B3FX_T_PROBE_HI
#define B3FX_T_PROBE_HI       2.20f  /* outlier probe cannot black/blow a car*/
#endif
/* ======================================================================
 * 1c. SHINE-HOT -- the reflection layer's OTHER half (2026-08-21)
 *
 * Section 1b says, in its own words, "REFL_GAIN scales the whole reflection
 * layer".  It did not.  The layer has two halves --
 *
 *     col  = mix(base, envc, refl)          the LERP   (uReflGain applied)
 *     out  = col + uLightRGB * s * uSpecAdd the ADDITIVE SPECULAR  (nothing)
 *
 * -- and B3FX_T_REFL_GAIN only ever reached the first.  ENVDBG=3 calls the sum
 * of the two "the reflection layer", so the module's documentation, its
 * measurement hook and its validator all treated them as one thing while the
 * code treated them as two.
 *
 * That mattered only once SHINE-UV revived the second half.  With the recovered
 * raw R.xy lookup 86% of the body clamped to the sheet's BORDER, and the
 * border is where this sheet keeps its clear sky: mean alpha 0.120 on
 * US_C3_V1 against a gate of 1-K = 0.65, with the (0,0) corner every
 * negative-R.xy fragment lands on at exactly 0.  So the specular was off over
 * almost the whole body -- as good as dead, and dead for the same reason the
 * lerp was.  Reparameterising the lookup put the body onto the live sheet,
 * where 57.4% of the texels clear that gate, and turned BOTH halves back on.
 * Only the lerp was on a knob.  Three rounds of REFL_GAIN reduction
 * (0.50 -> 0.35 -> 0.25 -> 0.18) therefore dimmed the half that was already
 * dim and left the half that was hot exactly where it was, which is why the
 * user reported no change three times.
 *
 * MEASURED, on the user's own capture build/debug_dump_080.bmp (US_C3_V1,
 * frame 283, race_time 4.733, standard daylight):
 *   * the SKY peaks at luminance 212.7 and never clips (0.00% of sky pixels
 *     have all three channels >= 250);
 *   * the blue car's ROOF is 19.7% pure white and its minimum channel averages
 *     176 -- a grey VEIL, not a colour.
 * A reflection cannot out-shine what it reflects, so the layer is not merely
 * mistuned, it is in the wrong SPACE.
 *
 * Three named quantities, each a magnitude on an already-recovered shape:
 *
 *   B3FX_T_SPEC_GAIN   the additive half's weight, i.e. the knob section 1b
 *                      said existed.  Defaults to "whatever REFL_GAIN is", so
 *                      the layer moves as ONE thing from here on.
 *   B3FX_T_ENV_RT      the substituted sheet's RADIANCE into render-target
 *                      space.  The present composite (FUN_0003DA90) doubles
 *                      the finished frame, so every pass emits in the bottom
 *                      half of the range: the sky dome does it with the
 *                      recovered 0xFF808080 blit colour (B3_SKY_LUT_BLIT_RGB
 *                      = 0.5, burnout3_postfx.h), the world's baked vertex
 *                      colour is retail data that already is, and the car pass
 *                      was the one consumer of enviro.dat art that never got
 *                      the factor.  It is that same recovered 0.5 -- it
 *                      cancels the recovered x2 exactly -- not a new fit.
 *   B3FX_T_SPEC_GATE   WHAT drives the recovered specular threshold.  See the
 *                      shader: the substituted sheet's authored alpha is a
 *                      CLOUD COVERAGE mask, not an intensity, so 57.4% of
 *                      US_C3_V1's sheet sits above the 1-K gate and US_P2 /
 *                      EU_P2 -- fully opaque sheets -- fire it everywhere at
 *                      full strength regardless of view direction.  1 uses the
 *                      normalised environment intensity along the mirror ray,
 *                      which is the quantity the module's own no-sheet branch
 *                      already puts in that slot.
 *
 * B3_CARFX_TUNE=0 collapses all three to the recovered values (1, 1, sheet
 * alpha), so the untuned composition is still reachable byte for byte.  Each
 * also has a runtime override for A/B: B3_CARFX_SPECGAIN, B3_CARFX_ENVRT,
 * B3_CARFX_SPECGATE, and B3_CARFX_REFLGAIN for the sibling.
 * ====================================================================== */
#ifndef B3FX_T_SPEC_GAIN
/* < 0 means "track B3FX_T_REFL_GAIN", which is the whole point: the two halves
 * of one layer should not be able to drift apart again. */
#define B3FX_T_SPEC_GAIN     (-1.0f)
#endif
#ifndef B3FX_T_ENV_RT
#define B3FX_T_ENV_RT        0.5f   /* == B3_SKY_LUT_BLIT_RGB, cancels the x2 */
#endif
#ifndef B3FX_T_SPEC_GATE
#define B3FX_T_SPEC_GATE     1      /* 0 = sheet alpha, 1 = mirror-ray intensity */
#endif

/* ======================================================================
 * 1d. SHINE-SHAPE (user-authorized deviation 2026-08-21) -- the reflection's
 *     STRUCTURE, not its magnitude.
 *
 * 1b and 1c both moved the reflection layer's WEIGHT, and after both the user
 * called the result "too flat".  They could not have fixed it, because the
 * layer had no shape to scale: its only spatial variable was the Fresnel term
 * (1-P)*F, which is a function of |N.V| alone.  A Fresnel-only layer is
 * brightest at the SILHOUETTE and dimmest facing the camera, i.e. it has no
 * up/down structure at all -- the same weight on a roof as on a rocker panel.
 *
 * What the retail frames actually show (three chase-cam captures of the black
 * muscle car on the US/M1 mountain track, and four of the red car on the city
 * tracks -- see tools/validate_car_shine.py's REFERENCE table):
 *
 *   * the up-facing panels (roof, trunk deck, hood shoulders) POOL a bright,
 *     soft sky reflection, and it rolls off down the curvature;
 *   * the flanks carry a dimmer, cooler HORIZON sheen that bands with the
 *     bodywork;
 *   * the down-facing and lower panels stay DEEP black -- on the black car the
 *     rear valence bottoms out at luminance 1..7 while the same car's trunk
 *     lip peaks past 200.  A four-and-a-half stop range on one panel is what
 *     reads as gloss;
 *   * exactly one COMPACT white glint, a few hundred pixels, not a wash.
 *
 * Three ingredients produce that, and all three are DATA the port already has:
 *
 * A. THE SHEET IS AN UP-CENTRED ANGULAR MAP, not a view sphere map.  Measured
 *    over all 31 square `enviro.dat +0xA0` sheets that ship: the top half of
 *    the disc and the bottom half agree to within 10% on 24 of them (the whole
 *    set spans 0.82..1.56, mean 1.01), so the image is NOT world-up-aligned --
 *    it is radially symmetric.  What varies is RADIUS: on US/M1 the disc reads
 *    132/151 inside r < 0.30 and 75/76 outside it, on EU/M2 198/186 against
 *    102/113, on EU/C3 161/138 against 92/93.  A bright middle and a dark rim
 *    is a fisheye of a SKY: centre = zenith, rim = ground.  So the reflection
 *    vector is mapped about WORLD UP,
 *        m = 2*sqrt(Rx^2 + Rz^2 + (Ry+1)^2),  uv = 0.5 + (Rx, Rz)/m
 *    which puts the horizon at r = 0.354 -- inside US/M1's dark ring, i.e. on
 *    the mountains.  [C on the sheet statistics, S on the reading.]
 *
 * B. A HEMISPHERE WEIGHT on the reflected RADIANCE.  Nine shipped sheets carry
 *    no ground at all (US/C3's 512x128 cloud strip is sky end to end; EU/C1,
 *    EU/C2 and EU/P1 are solid white), so a car on those tracks reflects a
 *    bright sky in every direction including straight down.  That is the grey
 *    veil.  The radiance is therefore blended toward a dark ground -- the
 *    sheet's own colour at B3FX_T_HEM_GROUND -- below the horizon, and lifted
 *    toward the zenith above it.
 *
 * C. A COMPACT SUN GLINT off the track's own sun vector.  enviro.dat env+0x80
 *    is already extracted per track into build/postfx/<ID>_env.txt (the sky
 *    LUT's glow uses it); the body's specular takes the same vector, so the
 *    glint sits where the sky's sun is.  It replaces the recovered broad
 *    threshold on the TUNED path only -- that term is driven by irr(R), an L2
 *    lobe, which is a WASH by construction and cannot make a compact glint.
 *
 * Every one of these is behind uTunedFx.  B3_CARFX_TUNE=0 clears it and the
 * arithmetic collapses, term for term, to the composition above.
 * ====================================================================== */
#ifndef B3FX_T_HEM_UPMIX
/* 0 = the hemisphere weight follows the mirror ray R (physical), 1 = it
 * follows the surface normal N (pools on up-facing panels).  Retail's look is
 * the second; the first is what keeps the layer MOVING as the car turns, so
 * the shipped value takes both. */
#define B3FX_T_HEM_UPMIX      0.55f
#endif
#ifndef B3FX_T_HEM_GROUND
/* what is left of the sheet's radiance straight down */
#define B3FX_T_HEM_GROUND     0.11f
#endif
#ifndef B3FX_T_HEM_LO
#define B3FX_T_HEM_LO         0.22f  /* smoothstep band around the horizon,  */
#endif
#ifndef B3FX_T_HEM_HI
#define B3FX_T_HEM_HI         0.30f  /* in units of the up coordinate        */
#endif
#ifndef B3FX_T_TOP_BOOST
/* extra radiance at the zenith, on top of the sheet -- the "pooling" */
#define B3FX_T_TOP_BOOST      0.85f
#endif
#ifndef B3FX_T_SKY_W
/* extra LERP weight from up-facing normals: the gloss mask stays
 * authoritative (it multiplies the whole thing), the Fresnel term keeps its
 * recovered weight, and this is added on top of both. */
#define B3FX_T_SKY_W          0.42f
#endif
#ifndef B3FX_T_ENV_CON
/* contrast expansion of the sheet about its OWN mean luminance, which the
 * loader measures off the decoded image (never a per-track constant).  US/C3's
 * cloud strip spans 148..211 over its rows -- a 1.4:1 sheet reflects as a flat
 * grey no matter what weight it is given. */
#define B3FX_T_ENV_CON        1.55f
#endif
#ifndef B3FX_T_SUN_POW
#define B3FX_T_SUN_POW        56.0f  /* glint tightness: cos^n about the sun  */
#endif
#ifndef B3FX_T_SUN_GAIN
#define B3FX_T_SUN_GAIN       0.85f  /* and its peak, before uEnvRT           */
#endif
#ifndef B3FX_T_SUN_FRESNEL
/* how much of the glint is Fresnel-weighted (a glint at grazing incidence is
 * brighter than one head-on); 0 = flat, 1 = fully */
#define B3FX_T_SUN_FRESNEL    0.55f
#endif

/* Corona intensity.  The corona pass is the least-recovered part of this
 * module -- the blend factors are [?] and the port's additive choice is GLUE
 * (section 9) -- and the recovered colour constants (1.5 / 1.4 / 1.1 white and
 * red) are additive over a frame that the recovered present composite then
 * DOUBLES.  At full weight the soft outskirts of `coronaglow` saturate too, so
 * a distant headlight stops being a compact dot and becomes a white slab with
 * a hard edge (visible on oncoming traffic).  This scales the emitted colour
 * only; the recovered size law, the cosine gate and the type table are
 * untouched.  B3_CARFX_TUNE=0 restores 1.0. */
#ifndef B3FX_T_CORONA_GAIN
#define B3FX_T_CORONA_GAIN    0.60f
#endif

/* Shadow -- FUN_0019A7C0 (0x0019A7C0) / FUN_00043570 / FUN_00043350 */
#define B3FX_SHADOW_DARK      0.699999988079071f  /* 0x003B17D8, MULSS 0x0004359C */
#define B3FX_SHADOW_AIRSLOPE  0.800000011920929f  /* 0x003A5600 */
#define B3FX_SHADOW_SIZEGAIN  0.4000000059604645f /* 0x003B16E8 */
#define B3FX_SHADOW_LIFT      0.05999999865889549f/* PUSH 0x3D75C28F @0x0019A6A0 */
#define B3FX_SHADOW_RAMP      (2.0f * 3.3333332538604736f) /* 0x003B1688 * 0x003B1DF8 */
#define B3FX_SHADOW_V0        0.0f
#define B3FX_SHADOW_V1        0.1875f             /* 0x003B1AC0 */
#define B3FX_SHADOW_V2        0.8125f             /* 0x003B1ABC */
#define B3FX_SHADOW_V3        1.0f                /* 0x003B168C */

/* Coronas -- FUN_00187C70 (0x00187C70) / FUN_00187BE0 / FUN_00042B00.
 * Colour vec4s read from 0x004161A0..0x004161F0; size multipliers from
 * 0x003895BC (1.25) and 0x003A55F8 (0.75); distance gain 0x003B1A08. [C] */
#define B3FX_CORONA_DISTGAIN  0.019999999552965164f  /* 0x003B1A08 */
#define B3FX_CORONA_HALF      0.5f                   /* record+0x10 = size*0.5 */
#define B3FX_CORONA_PULL      0.5f                   /* record+0x14, 0x3F000000 */
#define B3FX_CORONA_FAR       400.0f                 /* record+0x0C, 0x43C80000 */

typedef struct { unsigned bit; int type; float rgb[3]; float mult; } B3FXCorona;
/* order matters: FUN_00187C70 tests 0x10 before 0x08, so a braking car draws
 * the brake corona INSTEAD of the tail corona.                          [C] */
static const B3FXCorona B3FX_CORONAS[] = {
    { B3_CARFX_LIGHT_HEAD_HI, 0, { 1.5f, 1.5f, 1.5f }, 1.25f },  /* 0x004161A0 */
    { B3_CARFX_LIGHT_HEAD,    0, { 1.0f, 1.0f, 1.0f }, 1.00f },  /* 0x004161B0 */
    { B3_CARFX_LIGHT_BRAKE,   2, { 1.4f, 0.0f, 0.0f }, 0.75f },  /* 0x004161C0 */
    { B3_CARFX_LIGHT_TAIL,    1, { 1.1f, 0.0f, 0.0f }, 0.50f },  /* 0x004161D0 */
    { B3_CARFX_LIGHT_INDIC_R, 5, { 1.0f, 0.9f, 0.0f }, 0.50f },  /* 0x004161E0 */
    { B3_CARFX_LIGHT_INDIC_L, 6, { 1.0f, 0.9f, 0.0f }, 0.50f },  /* 0x004161E0 */
    { B3_CARFX_LIGHT_REVERSE, 4, { 0.9f, 0.9f, 0.9f }, 0.50f },  /* 0x004161F0 */
};
#define B3FX_NCORONA ((int)(sizeof B3FX_CORONAS / sizeof B3FX_CORONAS[0]))

/* ======================================================================
 * 2. GL 2.0 entry points (loaded through SDL so no new link dependency)
 * ====================================================================== */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
typedef unsigned int  b3glenum;
typedef int           b3glint;
typedef unsigned int  b3gluint;
typedef char          b3glchar;
typedef long int      b3glsizeiptr;

static b3gluint (*p_glCreateShader)(b3glenum);
static void (*p_glShaderSource)(b3gluint, b3glint, const b3glchar* const*,
                                const b3glint*);
static void (*p_glCompileShader)(b3gluint);
static void (*p_glGetShaderiv)(b3gluint, b3glenum, b3glint*);
static void (*p_glGetShaderInfoLog)(b3gluint, b3glint, b3glint*, b3glchar*);
static b3gluint (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(b3gluint, b3gluint);
static void (*p_glLinkProgram)(b3gluint);
static void (*p_glGetProgramiv)(b3gluint, b3glenum, b3glint*);
static void (*p_glGetProgramInfoLog)(b3gluint, b3glint, b3glint*, b3glchar*);
static void (*p_glUseProgram)(b3gluint);
static void (*p_glDeleteShader)(b3gluint);
static void (*p_glDeleteProgram)(b3gluint);
static b3glint (*p_glGetUniformLocation)(b3gluint, const b3glchar*);
static void (*p_glUniform1i)(b3glint, b3glint);
static void (*p_glUniform1f)(b3glint, float);
static void (*p_glUniform3f)(b3glint, float, float, float);
static void (*p_glUniformMatrix4fv)(b3glint, b3glint, unsigned char,
                                    const float*);
static void (*p_glUniformMatrix3fv)(b3glint, b3glint, unsigned char,
                                    const float*);
static b3glint (*p_glGetAttribLocation)(b3gluint, const char*);

static int load_gl2(void)
{
#define GET(fn) do { *(void**)(&p_##fn) = SDL_GL_GetProcAddress(#fn); \
                     if (!p_##fn) return 0; } while (0)
    GET(glCreateShader);   GET(glShaderSource);  GET(glCompileShader);
    GET(glGetShaderiv);    GET(glGetShaderInfoLog);
    GET(glCreateProgram);  GET(glAttachShader);  GET(glLinkProgram);
    GET(glGetProgramiv);   GET(glGetProgramInfoLog);
    GET(glUseProgram);     GET(glDeleteShader);  GET(glDeleteProgram);
    GET(glGetUniformLocation);
    GET(glUniform1i);      GET(glUniform1f);     GET(glUniform3f);
    GET(glUniformMatrix4fv); GET(glUniformMatrix3fv);
    GET(glGetAttribLocation);
#undef GET
    return 1;
}

/* ======================================================================
 * 3. Module state
 * ====================================================================== */
typedef struct { float pos[3], nrm[3]; int type; } B3FXLight;

typedef struct {
    int       nlights;
    B3FXLight light[64];
    /* shadow strip rows, outer (bbox) and inner (axle) -- FUN_0019A7C0 */
    float     halfwidth, outerfrontz, frontz, rearz, outerrearz;
    int       have_shadow;
    /* per-car probe cache.  FUN_0019D400 writes the nine straight into the
     * model instance and is SKIPPED on a miss (JL @0x001AB128 / TEST AL,AL
     * @0x0019D4B2), so every car keeps its own last successful probe.  [C] */
    float     sh[9];
    int       have_sh;
} B3FXCar;

/* ---- light-probe mesh (tools/extract_light_probes.py) ------------------- */
#define B3FX_PGRID 192                 /* XZ lookup grid, GLUE (accel only) */
typedef struct {
    int    ntri, nprobe;
    float* tri;                        /* ntri * 9 floats, GAME space       */
    unsigned* idx;                     /* ntri * 3 probe indices            */
    signed char* probe;                /* nprobe * 9 raw quantised bytes    */
    float  bmin[3], bmax[3];
    float  emax;                       /* peak E over this track's probes    */
    /* TUNED (user-authorized deviation 2026-08-13): the same subsample also
     * records the FLOOR of E and the mean of the DC coefficient, which are
     * what the reflection's shade ramp and the probe contrast expansion are
     * anchored on.  Both are per-track and computed from the track's own
     * shipped probe set, so neither is a constant and neither is per-track
     * source data. */
    float  emin;                       /* floor of E over this track's probes*/
    float  l0min, l0max, l0mean;       /* the probes' DC coefficient range   */
    float  cx, cz;                     /* cell size                          */
    int*   cell_start;                 /* (PGRID*PGRID + 1)                  */
    int*   cell_tri;                   /* flattened per-cell triangle lists  */
    int    ncell_tri;
} B3FXProbes;

static struct {
    int      ready;
    int      have_gl2;
    b3gluint prog;
    b3glint  u_sh, u_n2w, u_light, u_P, u_K, u_M, u_R0, u_fade, u_tint;
    b3glint  u_mvp, u_mv, u_eyeobj;
    b3glint  u_specadd, u_speclerp, u_basemul, u_reflalb;
    b3glint  u_hastex, u_hasnrm, u_tex;
    /* SHINE-UV */
    b3glint  u_envuv, u_envdbg;
    int      envuv;            /* 0 raw [C], 1 sphere map, 2 panorama   */
    b3glint  u_kz, u_gain, u_mode, u_env, u_hasenv, u_envnorm, u_glassr0a,
             u_glassr0b;
    /* TUNED (user-authorized deviation 2026-08-13) -- section 1b */
    b3glint  u_reflgain, u_reflfloor, u_envmod;
    /* SHINE-HOT -- section 1c */
    b3glint  u_specgain, u_envrt, u_specgate;
    /* SHINE-SHAPE -- section 1d */
    b3glint  u_tunedfx, u_hemup, u_hemground, u_hemlo, u_hemhi, u_topboost,
             u_skyw, u_envcon, u_envmean, u_sundir, u_sunpow, u_sungain,
             u_sunfres, u_havesun;
    float    env_mean;             /* the loaded sheet's own mean luminance  */
    float    sun_dir[3];           /* enviro.dat env+0x80, TOWARD the sun,   */
    int      have_sun;             /* in the sheet's authored (game) frame   */
    int      tune;                 /* 0 = B3_CARFX_TUNE=0, recovered look    */
    float    envmod;               /* this car's probe-driven sheen weight   */
    GLuint   tex_shadow, tex_corona, tex_env;
    float    L[9];                 /* monochrome SH irradiance coefficients */
    float    shmat[16];            /* FUN_000313E0's output, Z-MIRRORED for
                                    * this harness's GL world (see
                                    * b3fx_sh_mirror_z)                     */
    float    shmat_game[16];       /* the same, unmirrored: what the game
                                    * itself evaluates                      */
    float    emax;                 /* normaliser for the stand-in
                                    * environment's alpha mask              */
    float    emax_default;         /* the .rdata default probe's peak       */
    float    light_rgb[3];         /* pixel-shader c14.xyz                  */
    float    last_P;               /* pixel-shader c14.w = the pass alpha   */
    B3FXCar  car[B3_CARFX_MAX_CARS];
    /* non-racer light tables (traffic .btv fleet) -- see b3_carfx_load_extra */
    B3FXCar  extra[B3_CARFX_MAX_EXTRA];
    B3FXProbes probes;
} g;

/* ======================================================================
 * 4. FUN_000313E0 -- SH coefficients -> irradiance matrix
 * ====================================================================== */
/* Column-major for glUniformMatrix4fv(transpose=0).  The matrix is
 * symmetric, so the storage order does not change the result; it is written
 * out in the game's own row order for readability. */
/* ---------------------------------------------------------------------
 * THE MIRROR.  The nine coefficients are consumed in WORLD space, not object
 * space -- docs/RE_CARFX.md 2.7: the front-end showcase draws the car's floor
 * reflection by negating m11/ty of the MODEL matrix (0x001AE909..0x001AE91C)
 * and it has to ship a SECOND set of nine coefficients (0x003B2044/48/4C) to
 * go with it.  Mirroring a model matrix cannot change an object-space normal,
 * so if the probe were sampled in object space that second set would be
 * pointless.                                                            [S]
 *
 * This harness's GL world is the Z-MIRROR of the game world: trackmesh_load()
 * negates the Z of every position AND every normal it loads (RE_NOTES 12), and
 * burnout3_full.c mirrors the path/collision arrays to match, then flips X in
 * the PROJECTION so the final image reads the right way round.  So a normal
 * that the game would have evaluated as Ng is handed to this module as
 * Ngl = (Ng.x, Ng.y, -Ng.z).
 *
 * A quadratic form cannot absorb that on its own, so the probe is mirrored to
 * match the world it is being evaluated in.  In the Ramamoorthi-Hanrahan basis
 * the coefficients whose basis function is ODD in z are
 *
 *     i = 2  L10   (z)      i = 5  L2-1  (yz)      i = 7  L21  (xz)
 *
 * and negating exactly those three gives E'(x, y, z) = E(x, y, -z) identically
 * (validate_carfx.py section 12 asserts that over a sphere of directions, and
 * asserts the untouched E'(+/-X) / E'(+/-Y) and the swapped E'(+/-Z)).
 *
 * Without it the probe's bright lobe sits on the wrong end of the car: retail
 * has E(+Z) = 0.143 against E(-Z) = 0.333, a 2.3x front/back asymmetry that
 * lands on the nose instead of the tail.  Note this is a mirror of the PORT's
 * world, not of the game's -- the game does not do it -- so the transform is
 * GLUE even though every number it moves is [C].                       GLUE */
static void b3fx_sh_mirror_z(const float in9[9], float out9[9])
{
    for (int i = 0; i < 9; i++) out9[i] = in9[i];
    out9[2] = -in9[2];   /* L10  (z)  */
    out9[5] = -in9[5];   /* L2-1 (yz) */
    out9[7] = -in9[7];   /* L21  (xz) */
}

void b3_carfx_sh_mirror_z(const float in9[9], float out9[9])
{
    b3fx_sh_mirror_z(in9, out9);
}

static void b3fx_sh_matrix(const float L[9], float out[16])
{
    const float c1 = B3FX_SH_C1, c2 = B3FX_SH_C2, c3 = B3FX_SH_C3;
    const float c4 = B3FX_SH_C4, c5 = B3FX_SH_C5;
    out[0]  =  c1 * L[8]; out[1]  = c1 * L[4]; out[2]  = c1 * L[7]; out[3]  = c2 * L[3];
    out[4]  =  c1 * L[4]; out[5]  = -c1 * L[8]; out[6] = c1 * L[5]; out[7]  = c2 * L[1];
    out[8]  =  c1 * L[7]; out[9]  = c1 * L[5]; out[10] = c3 * L[6]; out[11] = c2 * L[2];
    out[12] =  c2 * L[3]; out[13] = c2 * L[1]; out[14] = c2 * L[2];
    out[15] =  c4 * L[0] - c5 * L[6];
}

/* max E(n) over the sphere, by a 1024-point Fibonacci scan.  It normalises the
 * STAND-IN environment's alpha channel: the recovered combiner thresholds the
 * stage-1 texture's alpha at 1-K = 0.65, and with no such texture the port
 * needs the probe's irradiance on the same [0,1] range those constants were
 * authored against.
 *
 * The normaliser is FIXED for the whole track -- the peak of E over the
 * track's own probe set, or the .rdata default probe's 0.68824 when no probe
 * mesh is loaded.  It is deliberately NOT recomputed per probe: a per-probe
 * normaliser is scale-invariant, so the specular would not dim in shade --
 * and the reference captures show it doing exactly that (paint p90 141 in
 * sun against 83 in the tunnel, same car, same paint).  Retail gets that
 * from whatever DAT_004D6C00 holds; the port gets it by letting the probe
 * drive the mask.                                                     GLUE */
static float b3fx_sh_peak(const float M[16])
{
    float best = 1e-6f;
    for (int i = 0; i < 1024; i++) {
        float z = 1.0f - 2.0f * (i + 0.5f) / 1024.0f;
        float r = sqrtf(z * z < 1.0f ? 1.0f - z * z : 0.0f);
        float a = (float)(i * 2.399963229728653);
        float n[4] = { r * cosf(a), r * sinf(a), z, 1.0f };
        float e = 0.0f;
        for (int rr = 0; rr < 4; rr++)
            for (int cc = 0; cc < 4; cc++) e += n[rr] * M[rr * 4 + cc] * n[cc];
        if (e > best) best = e;
    }
    return best;
}

void b3_carfx_set_sh(const float L[9])
{
    /* g.L keeps the coefficients EXACTLY as the game stores them at
     * modelInstance+0x5C, so validate_carfx.py section 9 can compare the
     * module's table against the three .rdata writer sites bit for bit.  The
     * mirror is applied only on the way into the render matrix. */
    memcpy(g.L, L, sizeof g.L);
    float Lw[9];
    b3fx_sh_mirror_z(g.L, Lw);
    /* B3_CARFX_NOZMIRROR=1 evaluates the probe in the raw GL frame instead --
     * i.e. the pre-fix behaviour, in which the front/back asymmetry sits on
     * the wrong end of the car. Kept as the A/B that justified the mirror. */
    b3fx_sh_matrix(getenv("B3_CARFX_NOZMIRROR") ? g.L : Lw, g.shmat);
    b3fx_sh_matrix(g.L, g.shmat_game);

    if (g.emax_default <= 0.0f) {  /* fixed reference, computed once  GLUE */
        float M[16];
        b3fx_sh_matrix(B3_CARFX_SH_RETAIL, M);
        g.emax_default = b3fx_sh_peak(M);
    }
    g.emax = g.probes.emax > 0.0f ? g.probes.emax : g.emax_default;
}

/* E(n) = n^T M n on the probe as the RENDERER sees it, i.e. in this harness's
 * mirrored GL world.  Exposed for the validator and for the sky-occlusion
 * probe in section 7b. */
float b3_carfx_irradiance(const float n[3])
{
    const float* M = g.shmat;
    float v[4] = { n[0], n[1], n[2], 1.0f };
    float e = 0.0f;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) e += v[r] * M[r * 4 + c] * v[c];
    return e > 0.0f ? e : 0.0f;
}

/* the same on the UNMIRRORED probe -- i.e. what the game itself would compute
 * for a game-space normal.  b3_carfx_irradiance(x,y,z) == this(x,y,-z). */
float b3_carfx_irradiance_game(const float n[3])
{
    const float* M = g.shmat_game;
    float v[4] = { n[0], n[1], n[2], 1.0f };
    float e = 0.0f;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) e += v[r] * M[r * 4 + c] * v[c];
    return e > 0.0f ? e : 0.0f;
}

void b3_carfx_set_light_rgb(float r, float gg, float b)
{
    g.light_rgb[0] = r; g.light_rgb[1] = gg; g.light_rgb[2] = b;
}

/* SHINE-DATA: the track's sun colour, READ FROM THE TRACK.  enviro.dat
 * bytes 0x60..0x6B are game DATA, so they belong in a loader and not in C:
 * tools/extract_postfx_art.py writes them into the per-track environment
 * sidecar it already emits, build/postfx/<ID>_env.txt, as
 *     light_rgb <r> <g> <b>
 * and this reads that file.  B3FX_ENV_LIGHT below survives ONLY as the
 * no-sidecar fallback for a build/postfx that predates the extractor
 * change -- the same shape route.bin / grid.bin use.                     */
static int b3fx_env_light_from_sidecar(const char* track, float out_rgb[3])
{
    char path[192];
    snprintf(path, sizeof path, "build/postfx/%s_env.txt", track);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int got = 0;
    while (fgets(line, sizeof line, f)) {
        float r, g_, b;
        if (line[0] == '#') continue;
        if (sscanf(line, "light_rgb %f %f %f", &r, &g_, &b) == 3) {
            out_rgb[0] = r; out_rgb[1] = g_; out_rgb[2] = b;
            got = 1;
            break;
        }
    }
    fclose(f);
    return got;
}

/* SHINE-SHAPE 1d: the one switch.  B3_CARFX_SHAPE=0 turns the whole 1d
 * structure off WITHOUT leaving TUNED, so "flat vs shaped" is a one-variable
 * A/B on one binary; B3_CARFX_TUNE=0 clears it too.  It gates the sheet
 * parameterisation as well as the shader, which is why it lives here and not
 * inside begin_common(). */
static int b3fx_shape_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("B3_CARFX_SHAPE");
        on = e ? (atoi(e) != 0) : 1;
    }
    return on && g.tune;
}

/* SHINE-SHAPE 1d.C: the track's SUN VECTOR, out of the same sidecar.
 * enviro.dat env+0x80 is stored as the direction the light TRAVELS (y < 0 on
 * all 40 shipped tracks) -- burnout3_postfx.c negates it for the sky glow and
 * this negates it for the same reason: the glint is about where the sun IS.
 * The vector is in the GAME's frame, which is the frame Rg is taken in.  [C]
 * on the field (docs/RE_POSTFX.md 3.4), GLUE on the body using it. */
static void b3fx_sun_from_sidecar(const char* track)
{
    char path[192];
    g.have_sun = 0;
    g.sun_dir[0] = 0.0f; g.sun_dir[1] = 1.0f; g.sun_dir[2] = 0.0f;
    if (!track || !*track) return;
    snprintf(path, sizeof path, "build/postfx/%s_env.txt", track);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        float x, y, z;
        if (line[0] == '#') continue;
        if (sscanf(line, "sun_dir %f %f %f", &x, &y, &z) == 3) {
            float n = sqrtf(x * x + y * y + z * z);
            if (n > 1e-4f) {
                g.sun_dir[0] = -x / n;
                g.sun_dir[1] = -y / n;
                g.sun_dir[2] = -z / n;
                g.have_sun = 1;
            }
            break;
        }
    }
    fclose(f);
}

int b3_carfx_env_light_rgb(const char* track, float out_rgb[3])
{
    if (!track || !out_rgb) return 0;
    if (b3fx_env_light_from_sidecar(track, out_rgb)) return 1;
    for (int i = 0; i < B3FX_ENV_LIGHT_N; i++) {
        if (strcmp(B3FX_ENV_LIGHT[i].track, track) == 0) {
            out_rgb[0] = B3FX_ENV_LIGHT[i].rgb[0];
            out_rgb[1] = B3FX_ENV_LIGHT[i].rgb[1];
            out_rgb[2] = B3FX_ENV_LIGHT[i].rgb[2];
            return 1;
        }
    }
    return 0;
}

int b3_carfx_set_track(const char* track)
{
    float rgb[3];
    /* The .rdata nine are only the DEFAULT a car starts with and keeps
     * whenever the probe cast misses (RE_CARFX 12); the track's own probe
     * mesh replaces them per car per frame as soon as it loads. */
    b3_carfx_set_sh(B3_CARFX_SH_RETAIL);
    if (track && *track) {
        char path[192];
        snprintf(path, sizeof path, "build/tracks/%s/light_probes.bin", track);
        b3_carfx_probes_load(path);
    }
    /* THE STAGE-1 ENVIRONMENT MAP, per track.  The car body shader lerps the
     * shaded paint towards this texture by the Fresnel-weighted gloss mask
     * (section 5C) -- without it the body has no reflection layer at all and
     * reads matte, worst of all in shade where the probe fallback is dark.
     * The retail SOURCE of DAT_004D6C00 is [?] and stays [?]; what is loaded
     * here is `enviro.dat +0xA0`, the per-track reflection sheet the artists
     * themselves named `envmapclouds`/`SkyEnv`/`Sky_env`/`envmap` on 33 of the
     * 37 shipped tracks and authored, on US/P2, as a literal sphere map.
     * [C] that +0xA0 is that slot (FUN_00188880 @0x001888BF); [S] that it is
     * what stage 1 binds.  tools/extract_envmap.py writes it; see
     * scratchpad/carenv/INTEGRATION_NOTE.md for the four exhaustive searches
     * that closed off finding the real writer.
     * B3_CARFX_ENVMAP still wins, and four tracks ship no +0xA0 at all -- for
     * those the load fails and the shader keeps its probe fallback. */
    {
        const char* ov = getenv("B3_CARFX_ENVMAP");
        char path[192];
        if (ov && *ov) {
            b3_carfx_load_env_map(ov);
        } else if (track && *track) {
            snprintf(path, sizeof path, "build/tracks/%s/envmap.png", track);
            b3_carfx_load_env_map(path);
        }
    }
    /* SHINE-SHAPE 1d.C: the sun VECTOR, from the same file as the colour */
    b3fx_sun_from_sidecar(track);
    if (!b3_carfx_env_light_rgb(track, rgb)) return 0;
    b3_carfx_set_light_rgb(rgb[0], rgb[1], rgb[2]);
    printf("[carfx] %s sun colour %.3f %.3f %.3f (%s), dir %s"
           " (%.3f %.3f %.3f)\n", track,
           rgb[0], rgb[1], rgb[2],
           b3fx_env_light_from_sidecar(track, rgb)
               ? "build/postfx/<ID>_env.txt" : "compiled-in fallback",
           g.have_sun ? "sidecar" : "NONE",
           g.sun_dir[0], g.sun_dir[1], g.sun_dir[2]);
    return 1;
}

/* ======================================================================
 * 4b. LIGHT PROBES -- FUN_0019D400 / FUN_0019C640 / FUN_001B2230
 *
 * The nine SH coefficients are a per-POSITION light probe, and the probe
 * volume is the collision mesh: every collision prim carries four u16 probe
 * indices at +0x06 (one per corner, parallel to its u8 corner indices), and
 * the unit block's +0xA4 points at an array of 9 signed bytes per probe.
 * FUN_0019D400 casts a segment from the car's world position straight DOWN by
 * 20.0, takes the prim it hits and the sub-triangle flag, decodes the three
 * corner probes and blends them with the hit's barycentric (u, v):
 *
 *     L[i] = p0[i] + (p1[i]-p0[i])*u + (p2[i]-p0[i])*v
 *
 * tools/extract_light_probes.py writes the mesh out per track; everything
 * below is the consumer, reimplemented from the same three functions.   [C]
 * The XZ bucket grid is this port's own acceleration for what the game does
 * with its BSP; it changes which triangles are TESTED, never the result. GLUE
 * ====================================================================== */
#define B3FX_PROBE_CAST 20.0f       /* PUSH 0x41A00000 @0x001AB136      [C] */

static void b3fx_probe_decode(const signed char* b, float out[9]);

int b3_carfx_probes_loaded(void) { return g.probes.ntri; }

void b3_carfx_probes_free(void)
{
    free(g.probes.tri);   free(g.probes.idx);   free(g.probes.probe);
    free(g.probes.cell_start); free(g.probes.cell_tri);
    memset(&g.probes, 0, sizeof g.probes);
}

/* Same refill story as burnout3_collision.c's B3COL_IOBUF, and worse: this
 * file's per-triangle loop makes TWO freads of 36 and 12 bytes over a 6 MB
 * file, so at musl's 1 KB buffer US_C3_V1's light_probes.bin cost ~6 000
 * blocking main-thread round trips in the web port.  The loop is untouched;
 * only the buffer it refills from changes size. */
#define B3FX_IOBUF (1u << 20)

/* B3_IO_SMALL=1 restores the 1 KB refill; burnout3_trackmesh.c owns the
 * switch's documentation and names the other loaders it covers. */
static int b3fx_io_small(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("B3_IO_SMALL");
        mode = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return mode;
}

int b3_carfx_probes_load(const char* path)
{
    b3_carfx_probes_free();
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char* iobuf = b3fx_io_small() ? NULL : (char*)malloc(B3FX_IOBUF);
    if (iobuf) setvbuf(f, iobuf, _IOFBF, B3FX_IOBUF);
    char magic[4];
    unsigned ver, nprobe, ntri;
    float bmin[3], bmax[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "B3LP", 4) != 0 ||
        fread(&ver, 4, 1, f) != 1 || ver != 1 ||
        fread(&nprobe, 4, 1, f) != 1 || fread(&ntri, 4, 1, f) != 1 ||
        fread(bmin, 4, 3, f) != 3 || fread(bmax, 4, 3, f) != 3 ||
        ntri == 0 || nprobe == 0) { fclose(f); free(iobuf); return 0; }
    B3FXProbes* P = &g.probes;
    P->probe = (signed char*)malloc((size_t)nprobe * 9);
    P->tri   = (float*)malloc((size_t)ntri * 9 * sizeof(float));
    P->idx   = (unsigned*)malloc((size_t)ntri * 3 * sizeof(unsigned));
    if (!P->probe || !P->tri || !P->idx) {
        fclose(f); free(iobuf); b3_carfx_probes_free(); return 0;
    }
    if (fread(P->probe, 1, (size_t)nprobe * 9, f) != (size_t)nprobe * 9) {
        fclose(f); free(iobuf); b3_carfx_probes_free(); return 0;
    }
    fseek(f, (long)((4 - ((nprobe * 9) & 3)) & 3), SEEK_CUR);   /* pad to 4 */
    for (unsigned i = 0; i < ntri; i++) {
        if (fread(P->tri + i * 9, sizeof(float), 9, f) != 9 ||
            fread(P->idx + i * 3, sizeof(unsigned), 3, f) != 3) {
            fclose(f); free(iobuf); b3_carfx_probes_free(); return 0;
        }
    }
    fclose(f);
    free(iobuf);
    P->ntri = (int)ntri;
    P->nprobe = (int)nprobe;
    memcpy(P->bmin, bmin, sizeof bmin);
    memcpy(P->bmax, bmax, sizeof bmax);
    P->cx = (bmax[0] - bmin[0]) / B3FX_PGRID;
    P->cz = (bmax[2] - bmin[2]) / B3FX_PGRID;
    if (P->cx <= 0.0f) P->cx = 1.0f;
    if (P->cz <= 0.0f) P->cz = 1.0f;

    /* two-pass counting sort into the XZ buckets */
    int ncell = B3FX_PGRID * B3FX_PGRID;
    P->cell_start = (int*)calloc((size_t)ncell + 1, sizeof(int));
    if (!P->cell_start) { b3_carfx_probes_free(); return 0; }
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            int run = 0;
            for (int i = 0; i <= ncell; i++) {
                int n = P->cell_start[i];
                P->cell_start[i] = run;
                run += n;
            }
            P->ncell_tri = run;
            P->cell_tri = (int*)malloc((size_t)(run ? run : 1) * sizeof(int));
            if (!P->cell_tri) { b3_carfx_probes_free(); return 0; }
        }
        for (int t = 0; t < P->ntri; t++) {
            const float* v = P->tri + t * 9;
            float x0 = v[0], x1 = v[0], z0 = v[2], z1 = v[2];
            for (int k = 1; k < 3; k++) {
                if (v[k * 3 + 0] < x0) x0 = v[k * 3 + 0];
                if (v[k * 3 + 0] > x1) x1 = v[k * 3 + 0];
                if (v[k * 3 + 2] < z0) z0 = v[k * 3 + 2];
                if (v[k * 3 + 2] > z1) z1 = v[k * 3 + 2];
            }
            int cx0 = (int)((x0 - P->bmin[0]) / P->cx);
            int cx1 = (int)((x1 - P->bmin[0]) / P->cx);
            int cz0 = (int)((z0 - P->bmin[2]) / P->cz);
            int cz1 = (int)((z1 - P->bmin[2]) / P->cz);
            if (cx0 < 0) cx0 = 0;
            if (cz0 < 0) cz0 = 0;
            if (cx1 > B3FX_PGRID - 1) cx1 = B3FX_PGRID - 1;
            if (cz1 > B3FX_PGRID - 1) cz1 = B3FX_PGRID - 1;
            for (int cz = cz0; cz <= cz1; cz++)
                for (int cx = cx0; cx <= cx1; cx++) {
                    int c = cz * B3FX_PGRID + cx;
                    if (pass == 0) P->cell_start[c]++;
                    else           P->cell_tri[P->cell_start[c]++] = t;
                }
        }
    }
    for (int i = ncell; i > 0; i--) P->cell_start[i] = P->cell_start[i - 1];
    P->cell_start[0] = 0;

    /* The stand-in environment's alpha normaliser (see b3fx_sh_peak): the
     * peak of E over THIS TRACK's probe set, subsampled.  Fixed per track, so
     * a dim probe genuinely dims the specular.                        GLUE */
    {
        int step = P->nprobe / 2048;
        if (step < 1) step = 1;
        float best = 0.0f, worst = 1e30f, l0sum = 0.0f, L[9], M[16];
        float l0lo = 1e30f, l0hi = -1e30f;
        int n = 0;
        for (int i = 0; i < P->nprobe; i += step) {
            b3fx_probe_decode(P->probe + (size_t)i * 9, L);
            b3fx_sh_matrix(L, M);
            float e = b3fx_sh_peak(M);
            if (e > best)  best = e;
            if (e < worst) worst = e;
            if (L[0] < l0lo) l0lo = L[0];
            if (L[0] > l0hi) l0hi = L[0];
            l0sum += L[0];
            n++;
        }
        P->emax = best;
        /* TUNED (user-authorized deviation 2026-08-13): the same sweep's floor
         * and DC statistics, for section 1b's shade ramp and its contrast
         * expansion.  All three are the TRACK's own numbers, read out of the
         * track's own shipped probe set -- no constant, no per-track table. */
        P->emin   = (n && worst < best) ? worst : 0.0f;
        P->l0min  = n ? l0lo : 0.0f;
        P->l0max  = n ? l0hi : 0.0f;
        P->l0mean = n ? l0sum / (float)n : 0.0f;
        g.emax = best > 0.0f ? best : g.emax_default;
    }
    printf("[carfx] probes %s: %d tris, %d probes, E in [%.4f, %.4f], "
           "L00 in [%.4f, %.4f] mean %.4f\n",
           path, P->ntri, P->nprobe, P->emin, P->emax,
           P->l0min, P->l0max, P->l0mean);
    return P->ntri;
}

/* FUN_0019C640 -- nine signed bytes to nine floats, 1.6/0.6/0.4 over 128 [C]
 * (0x003B16F0 / 0x003B16EC / 0x003B16E8, all times 0x003B16F4 = 1/128) */
static void b3fx_probe_decode(const signed char* b, float out[9])
{
    out[0] = (float)b[0] * (1.6f * 0.0078125f);
    for (int i = 1; i < 4; i++) out[i] = (float)b[i] * (0.6f * 0.0078125f);
    for (int i = 4; i < 9; i++) out[i] = (float)b[i] * (0.4f * 0.0078125f);
}

int b3_carfx_probe_sample(const float pos_game[3], float out9[9])
{
    B3FXProbes* P = &g.probes;
    if (!P->ntri || !pos_game || !out9) return 0;
    int cx = (int)((pos_game[0] - P->bmin[0]) / P->cx);
    int cz = (int)((pos_game[2] - P->bmin[2]) / P->cz);
    if (cx < 0 || cz < 0 || cx >= B3FX_PGRID || cz >= B3FX_PGRID) return 0;
    int c = cz * B3FX_PGRID + cx;
    int lo = P->cell_start[c], hi = P->cell_start[c + 1];
    /* FUN_001AF980 builds the segment pos -> pos + (0,-cast,0) and keeps the
     * nearest hit; FUN_001B2230 is a one-sided Moller-Trumbore with
     * det > 1e-8 and t in [-1e-5, 1.00001].                              [C] */
    const float d[3] = { 0.0f, -B3FX_PROBE_CAST, 0.0f };
    float bestt = 1.00001f;
    int   bestk = -1;
    float bu = 0.0f, bv = 0.0f;
    for (int k = lo; k < hi; k++) {
        int t = P->cell_tri[k];
        const float* v = P->tri + t * 9;
        float e1[3], e2[3], pv[3], s[3], qv[3];
        for (int i = 0; i < 3; i++) {
            e1[i] = v[3 + i] - v[i];
            e2[i] = v[6 + i] - v[i];
            s[i]  = pos_game[i] - v[i];
        }
        pv[0] = d[1] * e2[2] - d[2] * e2[1];
        pv[1] = d[2] * e2[0] - d[0] * e2[2];
        pv[2] = d[0] * e2[1] - d[1] * e2[0];
        float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
        if (det <= 1e-8f) continue;                    /* one-sided     [C] */
        float inv = 1.0f / det;
        float u = (s[0] * pv[0] + s[1] * pv[1] + s[2] * pv[2]) * inv;
        if (u < -1e-5f || u > 1.00001f) continue;
        qv[0] = s[1] * e1[2] - s[2] * e1[1];
        qv[1] = s[2] * e1[0] - s[0] * e1[2];
        qv[2] = s[0] * e1[1] - s[1] * e1[0];
        float vv = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
        if (vv < -1e-5f || u + vv > 1.00001f) continue;
        float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
        if (tt < -1e-5f || tt > bestt) continue;
        bestt = tt; bestk = t; bu = u; bv = vv;
    }
    if (bestk < 0) return 0;
    float p0[9], p1[9], p2[9];
    const unsigned* ix = P->idx + bestk * 3;
    if (ix[0] >= (unsigned)P->nprobe || ix[1] >= (unsigned)P->nprobe ||
        ix[2] >= (unsigned)P->nprobe) return 0;
    b3fx_probe_decode(P->probe + (size_t)ix[0] * 9, p0);
    b3fx_probe_decode(P->probe + (size_t)ix[1] * 9, p1);
    b3fx_probe_decode(P->probe + (size_t)ix[2] * 9, p2);
    for (int i = 0; i < 9; i++)
        out9[i] = p0[i] + (p1[i] - p0[i]) * bu + (p2[i] - p0[i]) * bv;
    return 1;
}

/* GLUE, and now SUPERSEDED: kept only so a caller with no track name still
 * has a defensible environment.  The game's nine coefficients turned out to
 * be literals in .rdata (B3_CARFX_SH_RETAIL) rather than a runtime
 * projection, so b3_carfx_init() installs those and this function is not
 * called by the harness any more.
 * This projects a two-radiance hemisphere onto the L2 basis by direct
 * quadrature (no closed-form constant is invented), folding in the 1/pi that
 * turns the Ramamoorthi-Hanrahan irradiance into a Lambertian radiance
 * factor. */
void b3_carfx_sh_from_hemisphere(const float sky[3], const float ground[3],
                                 const float up[3])
{
    static const float Y[9] = { 0.282095f,
                                0.488603f, 0.488603f, 0.488603f,
                                1.092548f, 1.092548f, 0.315392f,
                                1.092548f, 0.546274f };
    float ul = sqrtf(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
    float ux = ul > 1e-6f ? up[0]/ul : 0.0f;
    float uy = ul > 1e-6f ? up[1]/ul : 1.0f;
    float uz = ul > 1e-6f ? up[2]/ul : 0.0f;
    float lum_sky = 0.299f*sky[0] + 0.587f*sky[1] + 0.114f*sky[2];
    float lum_gnd = 0.299f*ground[0] + 0.587f*ground[1] + 0.114f*ground[2];
    float acc[9] = {0,0,0,0,0,0,0,0,0};
    const int NT = 96, NP = 192;
    float wsum = 0.0f;
    for (int i = 0; i < NT; i++) {
        float ct = 1.0f - 2.0f * ((i + 0.5f) / NT);       /* cos theta */
        float st = sqrtf(1.0f - ct*ct);
        for (int j = 0; j < NP; j++) {
            float ph = 2.0f * (float)M_PI * ((j + 0.5f) / NP);
            float x = st * cosf(ph), y = ct, z = st * sinf(ph);
            float radiance = (x*ux + y*uy + z*uz) >= 0.0f ? lum_sky : lum_gnd;
            float b[9];
            b[0] = Y[0];
            b[1] = Y[1]*y; b[2] = Y[2]*z; b[3] = Y[3]*x;
            b[4] = Y[4]*x*y; b[5] = Y[5]*y*z;
            b[6] = Y[6]*(3.0f*z*z - 1.0f);
            b[7] = Y[7]*x*z; b[8] = Y[8]*(x*x - y*y);
            for (int k = 0; k < 9; k++) acc[k] += radiance * b[k];
            wsum += 1.0f;
        }
    }
    float w = (4.0f * (float)M_PI / wsum) / (float)M_PI;   /* dOmega, then 1/pi */
    float L[9];
    for (int k = 0; k < 9; k++) L[k] = acc[k] * w;
    b3_carfx_set_sh(L);
}

/* ======================================================================
 * 5. The shine program -- THE RECOVERED NV2A PIPELINE
 *
 * SUPERSEDES the earlier "no environment map / algebra is GLUE" reading.
 * Both halves of the car shader are now decoded out of the image:
 *
 * A. THE VERTEX PROGRAM.  `FUN_0003C8A0` (0x0003C8A0, the car shader factory
 *    -- identified by its call to the Fresnel writer FUN_0002EF90 at
 *    0x0003CB60) creates the body vertex shader at 0x0003CB78 from the
 *    declaration at 0x00387558 and the PROGRAM at 0x003E7D58.  That program
 *    is an Xbox `xvs` binary: a 4-byte header {u16 version 0x2078, u16 count}
 *    followed by `count` 16-byte NV2A instructions.  32 instructions, decoded
 *    (scratchpad carshine/vsh.py, field layout per the NV2A VP20 encoding;
 *    the decode is self-checked by the FINAL bit landing on instruction 31,
 *    by every v-register being one the declaration declares, and by the
 *    constant indices 96..99 / 108 / 111 / 112..118 being exactly the hardware
 *    slots the C code uploads to):                                       [C]
 *
 *      0-3   r2 = (dot3(v2,c116), dot3(v2,c117), dot3(v2,c118), v2.w)
 *                                              -- the WORLD-SPACE NORMAL
 *      4-7   r3 = (dot4(r2,c96), .. dot4(r2,c99))   -- M * (N,1)
 *      4     oT0 = v9                               -- the paint UV
 *      9     oD0.xyz = dot4(r3, r2)                 -- E(N) = (N,1)^T M (N,1)
 *      8,10,11 r4 = world-space POSITION (dp4 of v0 against the same rows)
 *      12    r5 = c108 - r4        -- c108 is the EYE POSITION: FUN_00031690
 *                                     uploads 0x004D67D0.. to hw slot 0x6C
 *                                     = 108 (`MOV ECX,0x6c` @0x00031750) [C]
 *      13    r9 = 2*N
 *      14,16 r1.w = rsq(dot3(r5,r5));  18  r7 = r5 * r1.w   -- unit V
 *      19    r8.w = dot3(r7, r2) = N.V
 *      21    oT1.xyz = r9*r8.w - r7 = 2N(N.V) - V   -- THE REFLECTION VECTOR
 *      20,22 r11.w = |N.V|
 *      23-28 r5.x = (1-|N.V|)^5 * c111.y + c111.x   -- SCHLICK, and c111 is
 *                                     the (R0, 1-R0, 1, 1) pair FUN_0002EF90
 *                                     writes and FUN_00031AB0 uploads to hw
 *                                     slot 0x6F = 111 (@0x00031D7E)      [C]
 *      30    oD0.w = min(r5.x, c111.z=1)            -- F
 *      15,16,17,24,29,31  oPos = the usual viewport-scaled clip position
 *
 *    So the vertex stage hands the pixel stage: oT0 = uv, oT1 = the world
 *    reflection vector, oD0 = (E, E, E, F).
 *
 * B. THE PIXEL PROGRAM.  The four car "shader handles" DAT_004D6550/54/58/5C
 *    are not vertex-shader handles: `FUN_001DABD0` -> `FUN_0034E790` is
 *    SetPixelShader (it reads +0xE4/+0xE8/+0xEC and +0xD8 of `[obj+8]`), and
 *    `FUN_0003C8A0` builds each one by allocating 0xFC bytes and copying
 *    0x3C dwords = 240 bytes -- exactly `sizeof(D3DPIXELSHADERDEF)` -- from
 *    .rdata.  The renderer object is 0x004D6170 (FUN_0002F260's argument,
 *    whose +0x38..+0x44 are the glass rasters FUN_00031690 republishes), so:
 *
 *      +0x3D8 = 0x004D6548  glass pass 1   def 0x003E8288
 *      +0x3DC = 0x004D654C  glass pass 2   def 0x003E8378
 *      +0x3E0 = 0x004D6550  BODY           def 0x003E8468
 *      +0x3E4 = 0x004D6554  alt material   def 0x003E8558
 *      +0x3E8 = 0x004D6558  mask&2 branch  def 0x003E8648
 *      +0x3EC = 0x004D655C  light/emissive def 0x003E8738
 *
 *    Decoded (scratchpad carshine/psh.py) the BODY def is 8 combiner stages,
 *    PSTextureModes = 0x21 = {t0 PROJECT2D, t1 PROJECT2D}, per-stage C0 map
 *    0xFF3FF3FE = {c14, c15, c3, ...} and final C0 = c14:                 [C]
 *
 *      0 rgb  r0.rgb = (t0.rgb * v0.rgb) * 2         <- ALBEDO * E, x2
 *      0 a    r0.a   = (1-c14.a)*v0.a + c14.a        <- P + (1-P)*F
 *      1 a    r0.a  *= t0.a                          <- the paint's ALPHA is
 *                                                       the gloss mask
 *      2 a    r0.a  *= c3.a                          <- the fade
 *      3 rgb  r0.rgb = r0.a*t1.rgb + r0.rgb*(1-r0.a) <- LERP TO THE ENV MAP
 *      4 a    r0.a   = t0.a * t1.a                   <- gloss * env alpha
 *      5 a    r1.a   = r0.a * c3.a
 *      6 a    r1.a   = r1.a - c15.z                  <- minus (1-K)
 *      7 a    r1.a   = r1.a * c15.w * 4              <- times M/(1-K)
 *      final  rgb = c14.rgb * r1.a + (1-c14.rgb)*0 + r0.rgb   (CLAMP_SUM)
 *             a   = 1
 *
 *    The x2 at stage 0 and the x4 at stage 7 are the OCW output-scale field
 *    (bits 15..17); the x4 is the independent confirmation of the reading,
 *    because the C code divides by exactly 4 when it builds c15.w
 *    (`MULSS 0x003B1730` = 0.25 @0x00031C70).                             [C]
 *
 *    So the whole body equation is
 *
 *      base = 2 * paint.rgb * E(N)
 *      refl = paint.a * (P + (1-P)*F) * fade                 P = c14.w = 0.1
 *      col  = mix(base, env.rgb, refl)
 *      spec = clamp(paint.a*env.a*fade - (1-K), 0, ..) * M/(1-K)
 *      out  = col + lightRGB * spec                          alpha = 1
 *
 *    Three corrections to what this module used to do fall out of it:
 *      * the diffuse term is DOUBLED (stage 0's x2) -- the port was half-lit;
 *      * P = 0.1 is the Fresnel FLOOR of the reflection, not "the pass alpha"
 *        (the previous reading) and not an RGB gain (the reading before that);
 *      * the paint texture's ALPHA CHANNEL is the per-texel gloss mask, and
 *        it was being ignored entirely.
 *
 *    The alt-material def (3 stages, no final combiner -- FUN_0034E790
 *    special-cases `FinalABCD | FinalEFG == 0` at 0x0034E81F) is
 *      out.rgb = mix(2*paint.rgb*E, env.rgb*P, F),  out.a = paint.a,  P=0.5.
 *    Glass pass 2 (5 stages) is
 *      r = c5.b*F + c5.a          c5 = the def's own PSConstant0[0]
 *                                 = 0x4DE5B2E5 -> (0.898, 0.698, 0.898, 0.302)
 *      s = (env.a - (1-Kg)) * Mg/(1-Kg)
 *      out.rgb = env.rgb + lightRGB*s,  out.a = (r + s) * glassTex.a
 *    and glass pass 1 is black with alpha = glassTex.a * tint.            [C]
 *
 * C. WHAT IS BOUND TO STAGE 1.  `FUN_00031690` (the car pass setup, called
 *    from all three car draws: FUN_001A4710, FUN_001AAF00 and the showcase
 *    FUN_001AE6F0) binds the stage-1 texture from the single global
 *    DAT_004D6C00 (`MOV ECX,[0x004D6C00]; MOV [0x0075DB74],ECX` @0x00031740)
 *    and sets that stage's ADDRESSU/ADDRESSV to 3 = CLAMP (0x0075D744 /
 *    0x0075D754; the state array is state-major at 0x0075D740 + k*0x10 +
 *    stage*4, defaults 1/1/1/2/2/0 = WRAP,WRAP,WRAP,LINEAR,LINEAR,NONE).  The
 *    renderer object base is 0x004D6170 (`MOV EAX,0x4d6170; CALL 0x0003c8a0`
 *    @0x00015C47), so the global is renderer+0xA90.                      [C]
 *
 *    WHICH TEXTURE THE GAME PUT THERE IS [?], and four independent exhaustive
 *    searches say the writer is not in the image (scratchpad/carenv):
 *      * the dword 0x004D6C00 occurs ONCE in every PT_LOAD segment, unaligned
 *        scan included -- inside that read.  So no absolute store, and no
 *        `&g_field` in any pointer table;
 *      * a byte-exact scan for `disp32 == 0xA90` at every offset of every
 *        executable segment (the only encoding that can reach +0xA90 from a
 *        register base) finds 20 operands image-wide, none in the graphics
 *        module and none whose base can be the renderer;
 *      * a constant-propagating effective-address scan over all 7434 functions
 *        finds one computed reference to 0x004D6C00 -- the read;
 *      * two whole-image Unicorn sweeps (7434 functions each, synthetic object
 *        pointer in every register and stack slot, once with a zeroed object
 *        and once with a self-referential one) find no function that stores a
 *        lone dword at +0xA90; every hit is a block fill in another module.
 *
 *    So the port substitutes, [S], the only reflection-shaped per-track image
 *    that ships: `enviro.dat +0xA0`, which FUN_00188880 relocates and
 *    registers at 0x001888BF [C] and which the artists named `envmapclouds` /
 *    `Envmapclouds` / `WC_SkyEnv` / `ATB_SkyEnv` / `R_Sky_env` /
 *    `MRTHN1_SkyEnv` / `P2P1_SkyEnv` / `AS_m_envap` / `Clouds_prestorm_envmap`
 *    on 33 of the 37 shipped tracks -- and authored, on US/P2, as a textbook
 *    512x512 SPHERE MAP.  tools/extract_envmap.py writes it per track and
 *    b3_carfx_set_track() loads it; B3_CARFX_ENVMAP overrides.
 *
 *    Sampling is the recovered one and nothing more: uv = the reflection
 *    vector's xy under PROJECT2D with the unwritten q = 1, CLAMPed.  The
 *    port's world mirror is on Z (RE_CARFX 11.4), which leaves R.x and R.y --
 *    and therefore this lookup -- identical to the game's.
 *
 *    With no env texture at all (the four AS/C1,AS/C2 tracks, which ship no
 *    +0xA0 record) the shader still falls back to the SAME SH probe evaluated
 *    along the reflection vector --
 *        env.rgb = lightRGB * E(R),   env.a = clamp(E(R)/Emax, 0, 1)
 *    -- which is what a prefiltered environment would return for an L2
 *    environment, normalised so the recovered 1-K threshold and M/(1-K) gain
 *    act on the [0,1] range the constants were authored against.        GLUE
 *
 * D. Two deliberate GLUE deviations, both noted at their line:
 *      * E, F and R are evaluated PER FRAGMENT rather than per vertex.  Same
 *        formulae, higher sampling rate; the interpolated normal must be
 *        renormalised or a quadratic form bands between the corners.
 *      * the reflection is gated on gl_FrontFacing, because the game culls
 *        back faces on the body and this harness does not.
 * ====================================================================== */
/* THE COMPATIBILITY BUILTINS ARE GONE, and every one of them was doing real
 * work, so each is named here with what replaced it:
 *
 *   ftransform()                  -> uMVP * aPos.  The CPU-composed matrix
 *                                    from src/burnout3_render.c's stack; the
 *                                    same one the retained world program uses,
 *                                    so a car and the road it stands on are
 *                                    transformed identically.
 *   gl_MultiTexCoord0 / TexCoord  -> aUV / vUV.
 *   gl_Normal                     -> aNrm.
 *   gl_Color / gl_FrontColor      -> aCol / vCol.
 *   gl_ModelViewMatrixInverse *
 *     vec4(0,0,0,1)               -> uEyeObj.  This is the EYE POSITION IN
 *                                    OBJECT SPACE and nothing else: the
 *                                    inverse modelview applied to the origin.
 *                                    b3_carfx_set_matrices() computes the
 *                                    affine inverse on the CPU and uploads
 *                                    that one vector, which is all the shader
 *                                    ever read out of the matrix.
 *   gl_ModelViewMatrix * gl_Vertex-> uMV * aPos.
 *   gl_FogFragCoord               -> vFog, an ordinary varying.
 *
 * The attribute LOCATIONS are b3r's (b3r_attr_bind), because the same vertex
 * buffers feed both programs. */
static void b3fx_upload_matrices(void);
static int g_fx_prog_bound;

static const char* B3FX_VS =
"#ifdef GL_ES\n"
"precision highp float;\n"
"#endif\n"
"attribute vec3 aPos;\n"
"attribute vec2 aUV;\n"
"attribute vec4 aCol;\n"
"attribute vec3 aNrm;\n"
"varying vec3 vN;\n"
"varying vec3 vNraw;\n"
"varying vec3 vPosObj;\n"
"varying vec3 uMVPdbg;\n"
"varying vec3 vE;\n"
"varying vec2 vUV;\n"
"varying vec4 vCol;\n"
"varying float vFog;\n"
"uniform mat3 uN2W;\n"
"uniform mat4 uMVP;\n"
"uniform mat4 uMV;\n"
"uniform vec3 uEyeObj;\n"
"void main(){\n"
"  vec4 p = vec4(aPos, 1.0);\n"
"  gl_Position = uMVP * p;\n"
"  vUV = aUV;\n"
/* vertex program 0-3: the world-space normal (c116..c118 = the world rows) */
"  vN = uN2W * aNrm;\n"
"  vNraw = aNrm;\n"
"  vPosObj = aPos;\n"
"  uMVPdbg = vec3(uMVP[0][0], uMVP[1][1], uMVP[2][2]);\n"
/* vertex program 8-12: c108 (the eye position) minus the world position */
"  vE = uN2W * (uEyeObj - aPos);\n"
"  vCol = aCol;\n"
"  vFog = -(uMV * p).z;\n"
"}\n";

static const char* B3FX_FS =
/* THE EXTENSION DIRECTIVE HAS TO BE HERE, AND IT HAS TO BE FIRST.
 *
 * The no-vertex-normals path below builds a face normal out of dFdx/dFdy,
 * which on GLSL ES 1.00 lives in GL_OES_standard_derivatives and must be
 * asked for by name.  gl4es used to inject the directive for us -- its
 * shaderconv rewrites every shader on the way through and turns on the
 * extensions it sees used -- so this file never carried one.  Talking to
 * WebGL directly, nobody injects anything, and the compile fails with
 *
 *     ERROR: 0:34: 'GL_OES_standard_derivatives' : extension is disabled
 *
 * which took the WHOLE carfx program down -- "shine=OFF (no GL2)" -- and with
 * it both the body gloss and the glass, because the glass composites through
 * the same program.  One missing line, two reported defects.
 *
 * GLSL requires #extension before any non-preprocessor token, so it leads.
 * It is guarded because desktop GLSL 1.10 has derivatives in core and does not
 * know this extension name. */
"#ifdef GL_ES\n"
"#extension GL_OES_standard_derivatives : enable\n"
"precision highp float;\n"
"#endif\n"
/* AND THE SHADER STILL COMPILES WITHOUT IT.  `: enable` is a request, not a
 * requirement -- a browser that does not have the extension leaves the macro
 * undefined and would fail on dFdx again.  B3FX_HAS_DERIV lets the one site
 * that uses them fall back instead of taking the program with it. */
"#if !defined(GL_ES) || defined(GL_OES_standard_derivatives)\n"
"#define B3FX_HAS_DERIV 1\n"
"#endif\n"
"varying vec3 vN;\n"
"varying vec3 vNraw;\n"
"varying vec3 vPosObj;\n"
"varying vec3 uMVPdbg;\n"
"varying vec3 vE;\n"
"varying vec2 vUV;\n"
"uniform mat4  uSH;\n"
"uniform vec3  uLightRGB;\n"
"uniform float uP, uK, uM, uR0, uFade, uTint;\n"
"uniform float uKz, uGain, uEnvNorm;\n"
"uniform float uSpecAdd, uSpecLerp, uBaseMul, uReflAlb;\n"
/* TUNED (user-authorized deviation 2026-08-13) -- section 1b.  Three weights
 * on the recovered reflection layer; 1,1,1 reproduces the recovered result. */
"uniform float uReflGain, uReflFloor, uEnvMod;\n"
/* SHINE-HOT -- section 1c.  uSpecGain is the reflection layer's OTHER half,
 * the additive specular, which uReflGain never reached; uEnvRT puts the
 * substituted sheet's RADIANCE into render-target space, the space the
 * present composite's recovered x2 expects every pass to emit in. */
"uniform float uSpecGain, uEnvRT;\n"
/* SHINE-HOT: which quantity drives the recovered (x - (1-K)) * M/(1-K)
 * specular threshold -- 0 = the substituted sheet's authored ALPHA (what
 * shipped), 1 = the normalised environment INTENSITY along the mirror ray,
 * which is what the module's own no-sheet branch puts in that slot. */
"uniform int   uSpecGate;\n"
/* SHINE-SHAPE -- section 1d.  uTunedFx gates every one of these: at 0 the
 * arithmetic below collapses, term for term, to the 1b/1c composition and
 * B3_CARFX_TUNE=0 is byte for byte what it was. */
"uniform int   uTunedFx;\n"
"uniform float uHemUp, uHemGround, uHemLo, uHemHi, uTopBoost, uSkyW;\n"
"uniform float uEnvCon, uEnvMean;\n"
"uniform vec3  uSunDir;\n"
"uniform float uSunPow, uSunGain, uSunFres;\n"
"uniform int   uHaveSun;\n"
"uniform float uGlassR0a, uGlassR0b;\n"
"uniform int   uHasTex, uHasNrm, uHasEnv, uMode;\n"
/* SHINE-UV: which parameterisation maps the reflection vector onto the
 * SUBSTITUTED sheet -- 0 = the recovered raw lookup, 1 = sphere map,
 * 2 = lat-long panorama.  Chosen from the SHEET's own dimensions, so it is
 * a property of the loaded image and not a per-track constant.       GLUE */
"uniform int   uEnvUV;\n"
/* SHINE-UV measurement hook: B3_CARFX_ENVDBG=1 paints every body fragment
 * whose lookup lands OUTSIDE the sheet red and the rest green; =2 paints the
 * uv itself.  This is what tools/validate_car_shine.py counts.  Off by
 * default, and it touches nothing when it is off.                    GLUE */
"uniform int   uEnvDbg;\n"
"uniform sampler2D uTex;\n"
"uniform sampler2D uEnv;\n"
"float irr(vec3 n){ vec4 v = vec4(n,1.0); return max(dot(v, uSH*v), 0.0); }\n"
"void main(){\n"
"  vec3 V = normalize(vE);\n"
/* RENORMALISE PER FRAGMENT: vN is linearly interpolated and shortens between
 * the corners; feeding that to a quadratic form bulges and bands the lobe. */
"  vec3 N;\n"
"  if (uHasNrm != 0) {\n"
"    N = normalize(vN);\n"
"  } else {\n"
/* No vertex normals: a FACE normal from the view-vector derivatives.  Its
 * sign is arbitrary (the loader's Z-mirror inverted the winding once), so
 * this path -- and only this path -- resolves it against the view. */
"#ifdef B3FX_HAS_DERIV\n"
"    N = normalize(cross(dFdx(vE), dFdy(vE)));\n"
"    if (dot(N, V) < 0.0) N = -N;\n"
"#else\n"
/* No derivatives available: face the fragment at the eye.  Wrong for the
 * curvature, right for the average, and it keeps a car on screen with a
 * plausible lobe instead of losing the whole program. */
"    N = V;\n"
"#endif\n"
"  }\n"
"  float ndv = dot(N, V);\n"
/* vertex program 4-9: oD0.rgb = E(N) */
"  float E = irr(N);\n"
/* vertex program 22-30: oD0.a = min(R0 + (1-R0)*(1-|N.V|)^5, 1) */
"  float F = min(uR0 + (1.0 - uR0) * pow(1.0 - abs(ndv), 5.0), 1.0);\n"
/* vertex program 13-21: oT1.xyz = 2N(N.V) - V */
"  vec3  R = 2.0 * N * ndv - V;\n"
/* SHINE-UV, GLUE: Rg undoes this harness's world Z-mirror (RE_CARFX 11.4) so
 * every environment quantity below -- the sheet lookup, the hemisphere weight
 * and the sun glint -- is taken in the frame the DATA was authored in. */
"  vec3  Rg = vec3(R.x, R.y, -R.z);\n"
"  vec3  Ng = vec3(N.x, N.y, -N.z);\n"
"  vec4  paint = uHasTex != 0 ? texture2D(uTex, vUV)\n"
"                             : vec4(1.0);\n"
"  vec4  env;\n"
"  if (uHasEnv != 0) {\n"
"    if (uEnvUV == 0) {\n"
/* the recovered path: PSTextureModes stage 1 = PROJECT2D on oT1 (q unwritten,
 * so q = 1) -- the reflection vector's xy IS the texture coordinate.  Kept
 * verbatim and still reachable (B3_CARFX_ENVUV=raw) because it is what the
 * image decodes to.  It is NOT a parameterisation the substituted sheet can
 * answer: R is a unit vector, so R.xy covers [-1,1]^2 while a 2D lookup has
 * [0,1]^2, and stage 1 is CLAMP -- measured on US_C3_V1, 86% of the car's
 * fragments therefore read one frozen border texel and the body has no
 * reflection VARIATION at all (section 5C.1).                          [C] */
"      env = texture2D(uEnv, R.xy);\n"
"    } else {\n"
/* SHINE-UV, GLUE: the sheet's own parameterisation.  Rg undoes this harness's
 * world Z-mirror (RE_CARFX 11.4) so the lookup is taken in the frame the sheet
 * was authored in.  uEnvUV 2 is the equirectangular strip (u = azimuth,
 * v = polar); uEnvUV 1 is the textbook sphere map, m = 2|R + Z|. */
"      vec2 euv;\n"
"      if (uEnvUV == 2) {\n"
"        euv = vec2(atan(Rg.z, Rg.x) * 0.15915494 + 0.5,\n"
"                   acos(clamp(Rg.y, -1.0, 1.0)) * 0.31830989);\n"
/* SHINE-SHAPE 1d.A: the square sheets are UP-CENTRED angular maps -- the same
 * sphere-map algebra, but about world +Y instead of the view axis, so the
 * disc's middle is the zenith and its rim is the ground.  Selected by the
 * LOADER off the decoded image, exactly like uEnvUV 1 and 2. */
"      } else if (uEnvUV == 3) {\n"
"        float m = 2.0 * sqrt(Rg.x * Rg.x + Rg.z * Rg.z\n"
"                             + (Rg.y + 1.0) * (Rg.y + 1.0));\n"
"        euv = vec2(0.5 + Rg.x / max(m, 1e-4), 0.5 + Rg.z / max(m, 1e-4));\n"
"      } else {\n"
"        float m = 2.0 * sqrt(Rg.x * Rg.x + Rg.y * Rg.y\n"
"                             + (Rg.z + 1.0) * (Rg.z + 1.0));\n"
"        euv = vec2(0.5 + Rg.x / max(m, 1e-4), 0.5 - Rg.y / max(m, 1e-4));\n"
"      }\n"
"      env = texture2D(uEnv, euv);\n"
"      if (uEnvDbg == 2) { gl_FragColor = vec4(euv, 0.0, 1.0); return; }\n"
"    }\n"
"    if (uEnvDbg == 1) {\n"
"      vec2 q = (uEnvUV == 0) ? R.xy : vec2(0.5);\n"
"      bool in_ = q.x >= 0.0 && q.x <= 1.0 && q.y >= 0.0 && q.y <= 1.0;\n"
"      gl_FragColor = in_ ? vec4(0.0,1.0,0.0,1.0) : vec4(1.0,0.0,0.0,1.0);\n"
"      return;\n"
"    }\n"
"  } else {\n"
/* GLUE stand-in, section C above: the L2 probe sampled along the mirror ray */
"    float e = irr(R);\n"
"    env = vec4(uLightRGB * e, clamp(e * uEnvNorm, 0.0, 1.0));\n"
"  }\n"
/* SHINE-HOT -- section 1c.  THE SPECULAR DRIVE.  The recovered threshold below
 * is `(gloss * x * fade - (1-K)) * M/(1-K)`, and x is whatever stage 1's ALPHA
 * held.  On the SUBSTITUTED sheet that alpha is the artists' CLOUD COVERAGE
 * mask, which is not an intensity at all: on US_C3_V1 57.4% of its texels sit
 * above the 1-K = 0.65 gate, and on US_P2 / EU_P2 the sheet is fully opaque, so
 * the "highlight" fires at full strength over the entire gloss mask regardless
 * of where the car is looking.  The module already defines the quantity that
 * slot wants -- the branch three lines up puts `clamp(irr(R) * uEnvNorm, 0, 1)`
 * there, a normalised environment INTENSITY along the mirror ray -- so the
 * sheet path uses the same one and the two branches agree exactly.      GLUE */
"  float envI = (uSpecGate != 0) ? clamp(irr(R) * uEnvNorm, 0.0, 1.0)\n"
"                                : env.a;\n"
/* SHINE-HOT: and the SUBSTITUTED SHEET's radiance goes into render-target
 * space.  The present composite (FUN_0003DA90, burnout3_postfx.c) doubles the
 * finished frame, so every pass must emit in the bottom half of the range: the
 * sky dome does it with the recovered 0xFF808080 blit colour
 * (B3_SKY_LUT_BLIT_RGB), the world's baked vertex colour is retail data that
 * already is, and the car pass -- the one consumer of enviro.dat art that never
 * got the factor -- reflected a sky twice as bright as the sky in the same
 * frame.  MEASURED on the user's capture build/debug_dump_080.bmp: the sky
 * peaks at luminance 228.7 and NOTHING in the frame clips except the cars,
 * whose peak is 255.0 with 3.1% of the body pure white.
 * Scoped to the sheet on purpose: the `else` branch above is the L2 PROBE,
 * which is retail data and therefore already in retail's space, so it must not
 * be corrected twice.  Four shipped tracks have no +0xA0 sheet and take that
 * branch.                                                                GLUE */
"  float rt   = (uHasEnv != 0) ? uEnvRT : 1.0;\n"
"  vec4  envr = vec4(env.rgb * rt, env.a);\n"
/* ------------------------------------------------------------------ 1d.B
 * THE HEMISPHERE.  Everything from here to `envr` being rewritten is behind
 * uTunedFx; at 0 not one instruction below changes envr.
 *
 * `up` is where the reflection is LOOKING, blended between the mirror ray
 * (physical, and what makes the layer move as the car turns) and the surface
 * normal (which is what pools the sky on a roof).  `hemi` is 0 below the
 * horizon and 1 above it, with a soft band, so:
 *   * the sheet's radiance survives intact looking up,
 *   * it falls to uHemGround of itself looking down -- keeping the sheet's own
 *     COLOUR, so a dark cutting reflects a dark version of its own sky rather
 *     than a neutral grey, and the deep blacks the retail frames hold on the
 *     rear valence come back,
 *   * and uTopBoost lifts the zenith on top of that: the "pool" on the roof.
 *
 * 1d also expands the sheet's own contrast about its measured mean before any
 * of that (uEnvCon), because several shipped sheets have almost none.  The
 * expansion is on LUMINANCE and multiplies the colour, so the sheet's hue is
 * untouched and a fully-white sheet (EU/C1, EU/C2, EU/P1) is a no-op. */
"  if (uTunedFx != 0) {\n"
"    vec3  sh = envr.rgb;\n"
"    if (uHasEnv != 0 && uEnvCon != 1.0 && uEnvMean > 1e-4) {\n"
"      float sl = dot(sh, vec3(0.299, 0.587, 0.114)) / (uEnvMean * rt);\n"
"      sh *= pow(clamp(sl, 0.05, 4.0), uEnvCon - 1.0);\n"
"    }\n"
/* GLASS takes the same hemisphere but along the MIRROR RAY only, and with no
 * zenith lift.  A windscreen seen at a shallow angle reflects what is AHEAD of
 * it, not what is above it: pooling the sky on glass by its normal turns every
 * rear window into a pale slab, which is both wrong (retail's are dark, with
 * the sky only in the top band) and the single biggest source of bright
 * desaturated pixels -- the VEIL this file already gates on. */
"    float hu   = (uMode == 2) ? 0.0 : uHemUp;\n"
"    float tb   = (uMode == 2) ? 0.0 : uTopBoost;\n"
"    float up   = mix(Rg.y, Ng.y, hu);\n"
"    float hemi = smoothstep(-uHemLo, uHemHi, up);\n"
"    envr.rgb = mix(sh * uHemGround,\n"
"                   sh * (1.0 + tb * max(up, 0.0)), hemi);\n"
"  }\n"
/* the game culls the body; the harness does not, so an inward-facing fragment
 * (the cabin seen through the glass) must not receive the reflection.  GLUE */
"  float face = gl_FrontFacing ? 1.0 : 0.0;\n"
/* stage 0 rgb, with the OCW x2 */
"  vec3  base = 2.0 * paint.rgb * E * uBaseMul;\n"
/* stages 0a / 1a / 2a: P + (1-P)F, times the gloss mask, times the fade.
 * TUNED (user-authorized deviation 2026-08-13): uReflFloor scales the CONSTANT
 * P part only -- that is the part that makes a panel chrome when it faces the
 * camera -- and uReflGain*uEnvMod scale the whole layer, uEnvMod carrying the
 * local probe's brightness so a tunnel or a dark cutting dims the sky sheen.
 * The Fresnel term (1-P)*F keeps its full recovered weight, so the sheen
 * survives where it belongs: at grazing incidence and on the gloss mask. */
/* SHINE-SHAPE 1d: and the layer's WEIGHT gains the one variable it never had.
 * The recovered shape -- the constant P floor plus the Fresnel edge -- is
 * untouched and still carries the silhouette rim the retail frames show along
 * the trunk lip and the shoulder crown.  uSkyW*max(N.y,0)^2 is added to it, so
 * an up-facing panel takes MORE of the (already brighter) sky radiance and a
 * down-facing one takes less of an (already darker) ground.  The gloss mask
 * paint.a multiplies the sum, so it stays authoritative: a matte panel gets
 * none of this, exactly as before. */
"  float w    = uP * uReflFloor + (1.0 - uP) * F;\n"
"  if (uTunedFx != 0) {\n"
"    float sky = max(Ng.y, 0.0);\n"
"    w += uSkyW * sky * sky;\n"
"  }\n"
"  float refl = paint.a * w\n"
"             * uFade * face * uReflGain * uEnvMod;\n"
/* stage 3 rgb */
"  vec3  envc = mix(envr.rgb, envr.rgb * paint.rgb, uReflAlb);\n"
"  vec3  col  = mix(base, envc, refl);\n"
/* stages 4a..7a: (gloss*env.a*fade - (1-K)) * M/(1-K), hardware-clamped.
 * TUNED: the sky highlight is a reflection too, so it takes the same local
 * shade weight -- an environment highlight in a tunnel is the same error. */
"  float s = clamp((paint.a * envI * uFade - uKz) * uGain, 0.0, 1.0)\n"
"          * face * uEnvMod;\n"
/* SHINE-HOT -- section 1c.  THE BUG THIS WAVE IS ABOUT.  This term is the
 * other half of the reflection layer -- section 1b's own words are "REFL_GAIN
 * scales the whole reflection layer", and ENVDBG=3 below emits `envc*refl +
 * lightRGB*s` and calls that the layer -- but uReflGain was only ever applied
 * to `refl`, the LERP weight.  The additive half escaped it, so three rounds of
 * REFL_GAIN reduction (0.50 -> 0.35 -> 0.25 -> 0.18) could not touch the term
 * that was actually hot.  It is hot because the SHINE-UV fix revived it: with
 * the raw R.xy lookup 86% of the body read the sheet's BORDER, whose mean
 * alpha is 0.120 on US_C3_V1 -- well under the 0.65 gate, and exactly 0 at the
 * (0,0) corner every negative-R.xy fragment lands on -- so `s` was 0 over
 * almost the whole body, as dead as the lerp that fix was written to revive.
 * Reparameterising the lookup put the body on the live sheet, where 57.4% of
 * texels clear the gate: both halves came back, only one was on a gain.
 * Magnitude, for the record: K = 0.35, M = 0.7 give s_max = 0.3769, which the
 * present composite's x2 and the ^0.95 ramp land at 195/255 of ADDITIVE white
 * on top of whatever paint is underneath.  That is the band the user sees. */
"  vec3  spec = uLightRGB * s * uSpecAdd * uSpecGain * rt;\n"
/* ------------------------------------------------------------------ 1d.C
 * THE GLINT.  `s` above is driven by irr(R), an L2 spherical-harmonic lobe:
 * the widest smooth function on the sphere there is.  It cannot make a compact
 * highlight, and a broad additive white term over coloured paint is exactly
 * the "bright desaturated VEIL" tools/validate_car_shine.py already gates on.
 * So on the TUNED path it is REPLACED -- not summed with -- by a cos^n lobe
 * about the track's own sun vector (enviro.dat env+0x80, the same one the sky
 * LUT's glow is centred on), which is what puts the single compact white glint
 * the retail frames show on the trunk lip.  It keeps the gloss mask, the fade,
 * the facing test and the local probe shade, so it dies in a tunnel and never
 * appears on a matte panel.  With no sun sidecar the recovered term stands. */
"  if (uTunedFx != 0 && uHaveSun != 0) {\n"
"    float sd = max(dot(Rg, uSunDir), 0.0);\n"
"    float gl = pow(sd, uSunPow) * mix(1.0, F, uSunFres);\n"
"    spec = uLightRGB * gl * uSunGain * paint.a * uFade * face * uEnvMod * rt;\n"
"  }\n"
/* SHINE-UV measurement: B3_CARFX_ENVDBG=3 emits the REFLECTION LAYER ALONE
 * -- the environment lerp plus its specular, with the shaded paint removed.
 * That is the quantity "the cars look flat" is about, so the check can
 * measure it directly instead of inferring it from a lit frame.  Off by
 * default; tools/validate_car_shine.py is the consumer.             GLUE */
"  if (uEnvDbg == 3) {\n"
"    gl_FragColor = vec4(envc * refl + spec, 1.0);\n"
"    return;\n"
"  }\n"
/* SHINE-HOT measurement: 4 = the ADDITIVE SPECULAR alone, 5 = the environment
 * LERP alone.  ENVDBG=3 sums the two, which is why a layer that was 3x too hot
 * in one half and dimmed in the other still read as "up on the raw lookup" and
 * passed.  Splitting them is what makes the two halves separately testable. */
"  if (uEnvDbg == 4) { gl_FragColor = vec4(spec, 1.0); return; }\n"
/* SHADING-INPUT measurement: 7 emits the world normal the shading actually
 * ran on, |Ng|, and 8 emits the RAW attribute the vertex stage received.
 * The pair separates "the normal is wrong" from "the normal never
 * arrived": 8 flat and 7 flat means the vertex attribute is not being fed,
 * which is a buffer/binding fault, not a shader one.  Added when the web
 * port classified 0% of the body as side-facing under ENVDBG=6 while the
 * desktop classified 18.6% -- from the same source, same frame.     GLUE */
"  if (uEnvDbg == 7) { gl_FragColor = vec4(abs(Ng), 1.0); return; }\n"
"  if (uEnvDbg == 8) { gl_FragColor = vec4(abs(normalize(vNraw)), 1.0); return; }\n"
/* 9 emits the OBJECT-SPACE POSITION the vertex stage was handed, wrapped
 * so any scale reads as a gradient.  If this is flat while the desktop
 * is a gradient, the position attribute is being fed the wrong bytes --
 * which is a buffer/pointer fault and nothing to do with shading. */
"  if (uEnvDbg == 9) { gl_FragColor = vec4(fract(abs(vPosObj)), 1.0); return; }\n"
/* 10 emits carfx's OWN uMVP diagonal.  B3FX_VS and the retained vertex
 * shader compute gl_Position with identical arithmetic and differ ONLY in
 * where the matrix came from, so when the same mesh is correct under one
 * and collapsed under the other, this is the quantity in dispute. */
"  if (uEnvDbg == 10) { gl_FragColor = vec4(fract(abs(vec3(uMVPdbg))), 1.0); return; }\n"
"  if (uEnvDbg == 5) { gl_FragColor = vec4(envc * refl, 1.0); return; }\n"
/* SHINE-SHAPE measurement: 6 CLASSIFIES the body by its world normal --
 * RED where the surface faces up (Ng.y >= 0.55: roof, deck, hood, shoulder
 * crowns), GREEN where it faces sideways or straight back (|Ng.y| <= 0.30:
 * flanks, rear panel, doors), BLUE for everything between and BLACK for a
 * back-facing fragment.  It is what lets validate_car_shine.py split the car
 * by GEOMETRY instead of by a bounding box, which is the only way "the roof
 * pools more sky than the flank" can be asserted about the shader rather than
 * about the camera and the car's silhouette.
 *
 * A classification and not the raw normal on purpose: the capture path runs
 * the present composite's x2 and then the gamma ramp, so an encoded
 * 0.5+0.5*N reads back clipped and non-linear, while a saturated primary
 * survives both and is recovered by the same hue-dominance test the file's
 * existing is_red()/is_green() use.  Off by default.                   GLUE */
"  if (uEnvDbg == 6) {\n"
"    vec3 c = vec3(0.0);\n"
"    if (face > 0.5) {\n"
"      if (Ng.y >= 0.55)           c = vec3(1.0, 0.0, 0.0);\n"
"      else if (abs(Ng.y) <= 0.30) c = vec3(0.0, 1.0, 0.0);\n"
"      else                        c = vec3(0.0, 0.0, 1.0);\n"
"    }\n"
"    gl_FragColor = vec4(c, 1.0);\n"
"    return;\n"
"  }\n"
"  if (uMode == 1) {\n"
/* alt-material record (shader DAT_004D6554): 3 stages, NO final combiner.
 * TUNED: same two weights on the mix, for the same reason as the body. */
"    gl_FragColor = vec4(mix(base, envr.rgb * uP,\n"
"                            F * face * uReflGain * uEnvMod), paint.a);\n"
"  } else if (uMode == 2) {\n"
/* glass: the two recovered passes composited analytically into the harness'
 * single SRC_ALPHA/INV_SRC_ALPHA draw.  pass 1 = black at alpha a1, pass 2 =
 * (env + lightRGB*s2) at alpha a2, so over the same destination
 *   af = 1-(1-a1)(1-a2),  Cf = C2*a2/af. */
"    float r2 = uGlassR0b * F + uGlassR0a;\n"
/* SHINE-HOT: the glass takes the same drive and the same render-target space
 * as the body -- it is the SAME stage-1 sample, and a windscreen that reflects
 * a sky twice as bright as the sky is the same defect.  Its s2 keeps the
 * recovered shape; only what feeds it and the space it lands in change. */
"    float s2 = clamp((envI - uKz) * uGain, 0.0, 1.0) * face * uEnvMod;\n"
/* TUNED: glass has no paint under it -- it IS the reflection -- so the local
 * shade weight goes on the reflected COLOUR here rather than on a mix weight.
 * Same intent: the windows carry a bright sky only where there is one. */
/* s2 keeps its recovered meaning in a2 (how OPAQUE the reflection pass is);
 * only the RADIANCE it emits takes the layer's gain and the RT space. */
"    vec3  C2 = envr.rgb * uEnvMod + uLightRGB * s2 * uSpecGain * rt;\n"
"    float a1 = uTint * paint.a;\n"
"    float a2 = clamp(r2 + s2, 0.0, 1.0) * paint.a;\n"
"    float af = 1.0 - (1.0 - a1) * (1.0 - a2);\n"
"    gl_FragColor = vec4(C2 * a2 / max(af, 1e-4), af);\n"
"  } else {\n"
/* the body's final combiner: A*B + (1-A)*C + D with A = c14.rgb, B = r1.a,
 * C = 0 and D = r0.rgb -- i.e. colour + lightRGB*spec, alpha forced to 1. */
"    gl_FragColor = vec4(col + spec, 1.0);\n"
"  }\n"
"}\n";

static b3gluint compile_one(b3glenum type, const char* src, const char* tag)
{
    b3gluint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    b3glint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        b3glint n = 0;
        p_glGetShaderInfoLog(s, (b3glint)sizeof log, &n, log);
        log[(n > 0 && n < (b3glint)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[carfx] %s shader failed: %s\n", tag, log);
        p_glDeleteShader(s);
        return 0;
    }
    return s;
}

static int build_program(void)
{
    b3gluint vs = compile_one(GL_VERTEX_SHADER, B3FX_VS, "vertex");
    if (!vs) return 0;
    b3gluint fs = compile_one(GL_FRAGMENT_SHADER, B3FX_FS, "fragment");
    if (!fs) { p_glDeleteShader(vs); return 0; }
    b3gluint pr = p_glCreateProgram();
    p_glAttachShader(pr, vs);
    p_glAttachShader(pr, fs);
    /* the same four locations the retained renderer binds, because the
     * same vertex buffers feed both programs */
    b3r_attr_bind(pr);
    p_glLinkProgram(pr);
    /* WHERE THE ATTRIBUTES ACTUALLY LANDED.  b3r_attr_bind() is a request that
     * SILENTLY DOES NOTHING if the renderer has not resolved
     * glBindAttribLocation yet, and a wrong location does not fail the link --
     * it feeds the vertex stage from whatever array is enabled at that index,
     * which on a desktop COMPATIBILITY context is often still the right data
     * (generic attributes alias the fixed-function ones there) and on WebGL is
     * not.  The symptom is geometry, not a message: the body collapsed to a
     * flat slab on the web while the desktop was perfect.  So the answer is
     * read back and printed rather than assumed. */
    if (p_glGetAttribLocation) {
        int lp = p_glGetAttribLocation(pr, "aPos");
        int lu = p_glGetAttribLocation(pr, "aUV");
        int lc = p_glGetAttribLocation(pr, "aCol");
        int ln = p_glGetAttribLocation(pr, "aNrm");
        if (lp != B3R_ATTR_POS || lu != B3R_ATTR_UV ||
            lc != B3R_ATTR_COL || ln != B3R_ATTR_NRM)
            fprintf(stderr, "[carfx] ATTRIBUTE LOCATIONS ARE WRONG: "
                    "aPos %d (want %d), aUV %d (want %d), aCol %d (want %d), "
                    "aNrm %d (want %d) -- b3r_attr_bind() did not take\n",
                    lp, B3R_ATTR_POS, lu, B3R_ATTR_UV,
                    lc, B3R_ATTR_COL, ln, B3R_ATTR_NRM);
        else if (getenv("B3_CARFX_VERBOSE"))
            fprintf(stderr, "[carfx] attributes at %d/%d/%d/%d as bound\n",
                    lp, lu, lc, ln);
    }
    b3glint ok = 0;
    p_glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok) {
        char log[1024];
        b3glint n = 0;
        p_glGetProgramInfoLog(pr, (b3glint)sizeof log, &n, log);
        log[(n > 0 && n < (b3glint)sizeof log) ? n : 0] = 0;
        fprintf(stderr, "[carfx] program link failed: %s\n", log);
        p_glDeleteProgram(pr);
        return 0;
    }
    g.prog = pr;
    g.u_sh     = p_glGetUniformLocation(pr, "uSH");
    g.u_n2w    = p_glGetUniformLocation(pr, "uN2W");
    g.u_mvp    = p_glGetUniformLocation(pr, "uMVP");
    g.u_mv     = p_glGetUniformLocation(pr, "uMV");
    g.u_eyeobj = p_glGetUniformLocation(pr, "uEyeObj");
    g.u_light  = p_glGetUniformLocation(pr, "uLightRGB");
    g.u_P      = p_glGetUniformLocation(pr, "uP");
    g.u_K      = p_glGetUniformLocation(pr, "uK");
    g.u_M      = p_glGetUniformLocation(pr, "uM");
    g.u_R0     = p_glGetUniformLocation(pr, "uR0");
    g.u_fade   = p_glGetUniformLocation(pr, "uFade");
    g.u_tint   = p_glGetUniformLocation(pr, "uTint");
    g.u_specadd  = p_glGetUniformLocation(pr, "uSpecAdd");
    g.u_speclerp = p_glGetUniformLocation(pr, "uSpecLerp");
    g.u_basemul  = p_glGetUniformLocation(pr, "uBaseMul");
    g.u_reflalb  = p_glGetUniformLocation(pr, "uReflAlb");
    g.u_hastex = p_glGetUniformLocation(pr, "uHasTex");
    /* SHINE-UV */
    g.u_envuv  = p_glGetUniformLocation(pr, "uEnvUV");
    g.u_envdbg = p_glGetUniformLocation(pr, "uEnvDbg");
    g.u_hasnrm = p_glGetUniformLocation(pr, "uHasNrm");
    g.u_tex    = p_glGetUniformLocation(pr, "uTex");
    /* the recovered combiner's own constants: c15.z = 1-K (the threshold the
     * alpha combiner SUBTRACTS at stage 6) and c15.w*4 = M/(1-K) (stage 7's
     * gain, the 4 being the OCW output scale the C code pre-divides out) */
    g.u_kz     = p_glGetUniformLocation(pr, "uKz");
    g.u_gain   = p_glGetUniformLocation(pr, "uGain");
    g.u_mode   = p_glGetUniformLocation(pr, "uMode");
    g.u_env    = p_glGetUniformLocation(pr, "uEnv");
    g.u_hasenv = p_glGetUniformLocation(pr, "uHasEnv");
    g.u_envnorm  = p_glGetUniformLocation(pr, "uEnvNorm");
    g.u_glassr0a = p_glGetUniformLocation(pr, "uGlassR0a");
    g.u_glassr0b = p_glGetUniformLocation(pr, "uGlassR0b");
    /* TUNED (user-authorized deviation 2026-08-13) -- section 1b */
    g.u_reflgain  = p_glGetUniformLocation(pr, "uReflGain");
    g.u_reflfloor = p_glGetUniformLocation(pr, "uReflFloor");
    g.u_envmod    = p_glGetUniformLocation(pr, "uEnvMod");
    g.u_specgain  = p_glGetUniformLocation(pr, "uSpecGain");
    g.u_envrt     = p_glGetUniformLocation(pr, "uEnvRT");
    g.u_specgate  = p_glGetUniformLocation(pr, "uSpecGate");
    /* SHINE-SHAPE -- section 1d */
    g.u_tunedfx   = p_glGetUniformLocation(pr, "uTunedFx");
    g.u_hemup     = p_glGetUniformLocation(pr, "uHemUp");
    g.u_hemground = p_glGetUniformLocation(pr, "uHemGround");
    g.u_hemlo     = p_glGetUniformLocation(pr, "uHemLo");
    g.u_hemhi     = p_glGetUniformLocation(pr, "uHemHi");
    g.u_topboost  = p_glGetUniformLocation(pr, "uTopBoost");
    g.u_skyw      = p_glGetUniformLocation(pr, "uSkyW");
    g.u_envcon    = p_glGetUniformLocation(pr, "uEnvCon");
    g.u_envmean   = p_glGetUniformLocation(pr, "uEnvMean");
    g.u_sundir    = p_glGetUniformLocation(pr, "uSunDir");
    g.u_sunpow    = p_glGetUniformLocation(pr, "uSunPow");
    g.u_sungain   = p_glGetUniformLocation(pr, "uSunGain");
    g.u_sunfres   = p_glGetUniformLocation(pr, "uSunFres");
    g.u_havesun   = p_glGetUniformLocation(pr, "uHaveSun");
    return 1;
}

/* ======================================================================
 * 6. Art loading
 * ====================================================================== */
/* SHINE-SHAPE 1d: the mean LUMINANCE of the image the loader just decoded,
 * 0..1.  The sheet contrast expansion is anchored on it, so the anchor is a
 * property of the loaded art and never a per-track constant. */
static float g_last_png_mean = 0.0f;

static GLuint load_png_wh(const char* path, int* out_w, int* out_h)
{
    SDL_Surface* img = IMG_Load(path);
    if (!img) return 0;
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(img);
    if (!rgba) return 0;
    {   const unsigned char* px = (const unsigned char*)rgba->pixels;
        double acc = 0.0;
        long   n   = 0;
        for (int y = 0; y < rgba->h; y++) {
            const unsigned char* row = px + (size_t)y * rgba->pitch;
            for (int x = 0; x < rgba->w; x++, row += 4) {
                acc += 0.299 * row[0] + 0.587 * row[1] + 0.114 * row[2];
                n++;
            }
        }
        g_last_png_mean = n ? (float)(acc / (255.0 * (double)n)) : 0.0f;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    /* FUN_00043350 sets texture-stage address modes 2/2 for the shadow pass
     * (Xbox D3DTADDRESS_MIRROR) [S]; CLAMP is the safe GL equivalent for a
     * single non-tiling blob and avoids bleeding the soft edge. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    b3r_tex_mipmap_pre();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
    b3r_gen_mipmap();
    /* GL_GENERATE_MIPMAP is GL 1.4 / GLES1: it does not exist in GLES2, and
     * therefore not in WebGL either.  Asking for it there is INVALID_ENUM,
     * the chain never gets built, and a GL_LINEAR_MIPMAP_LINEAR minifier on
     * a texture with no chain is MIPMAP-INCOMPLETE -- which samples black.
     * glGenerateMipmap() after the upload is the GL 3.0 / GLES2 spelling of
     * the same thing, and it is what every target here uses now. */
    if (out_w) *out_w = rgba->w;
    if (out_h) *out_h = rgba->h;
    SDL_FreeSurface(rgba);
    /* NO `glBindTexture(GL_TEXTURE_2D, 0)` HERE -- see the note in
     * burnout3_hud.c by b3_hud_load_texture().  In short: name 0 is not "no
     * texture", it is a texture object that WebGL does not have, and leaving
     * it selected makes the translation layer try to configure it. */
    return t;
}

static GLuint load_png(const char* path) { return load_png_wh(path, 0, 0); }

/* The car body's texture stage 1.  FUN_00031690 binds it once for the whole
 * car pass from DAT_004D6C00 and sets its addressing to 3 (0x0075D744 /
 * 0x0075D754 = CLAMP, against the shadow pass' 2 = MIRROR at 0x0075D740) [C];
 * the pixel shader samples it with PSTextureModes stage 1 = PROJECT2D on the
 * reflection vector.  WHAT texture that global holds is [?] -- section 5C;
 * b3_carfx_set_track() feeds it enviro.dat +0xA0 as the [S] substitute. */
int b3_carfx_load_env_map(const char* path)
{
    int ew = 0, eh = 0;
    if (g.tex_env) { glDeleteTextures(1, &g.tex_env); g.tex_env = 0; }
    g.envuv = 0;
    g.env_mean = 0.0f;
    if (!path || !*path) return 0;
    g.tex_env = load_png_wh(path, &ew, &eh);
    if (g.tex_env) {
        g.env_mean = g_last_png_mean;
        /* SHINE-UV, GLUE.  WHICH lookup the sheet answers is a property of
         * the SHEET, read off the image the loader just decoded -- never a
         * per-track constant.  The shipped `enviro.dat +0xA0` reflection
         * sheets come in exactly two shapes:
         *   * square (128x128 / 256x256 / 512x512): a disc inscribed in the
         *     square with black corners -- a textbook SPHERE MAP;
         *   * 4:1 strip (US/C3's 512x128 `envmapclouds`): a horizontally
         *     wrapping sky PANORAMA.
         * Neither answers the recovered raw-R.xy lookup, which only covers
         * the [0,1]^2 corner of a coordinate that ranges over [-1,1]^2 --
         * see the shader.  B3_CARFX_ENVUV=raw|sphere|pano pins it. */
        /* SHINE-SHAPE 1d.A: a square sheet is an UP-CENTRED angular map (the
         * disc's middle is the zenith, its rim the ground -- measured over all
         * 31 shipped square sheets, section 1d).  That is uEnvUV 3.  The old
         * view-axis sphere map stays reachable as B3_CARFX_ENVUV=sphere, and
         * B3_CARFX_TUNE=0 falls back to it so the untuned look is unchanged. */
        g.envuv = (ew >= 2 * eh && eh > 0) ? 2 : (b3fx_shape_on() ? 3 : 1);
        {   const char* uv = getenv("B3_CARFX_ENVUV");
            if (uv && *uv) {
                if      (!strcmp(uv, "raw"))    g.envuv = 0;
                else if (!strcmp(uv, "sphere")) g.envuv = 1;
                else if (!strcmp(uv, "pano"))   g.envuv = 2;
                else if (!strcmp(uv, "up"))     g.envuv = 3;
                else                            g.envuv = atoi(uv);
            } }
        glBindTexture(GL_TEXTURE_2D, g.tex_env);
        /* ADDRESSU/ADDRESSV = 3 = CLAMP, which FUN_00031690 writes for this
         * stage at 0x0075D744 / 0x0075D754.                             [C] */
        /* SHINE-UV: a panorama's u IS an azimuth, so it wraps; every other
         * parameterisation keeps the recovered CLAMP. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        g.envuv == 2 ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* ...and NO mip filter.  The shadowed stage-state array is state-major
         * at 0x0075D740 + k*0x10 + stage*4 and FUN_001D7180 seeds it
         * 1,1,1,2,2,0 per stage -- WRAP,WRAP,WRAP,LINEAR,LINEAR,NONE reading
         * it as D3D's ADDRESSU/V/W + MAG/MIN/MIPFILTER, and the car pass
         * overrides only the two address modes.  So stage 1 samples the top
         * mip with plain bilinear; load_png()'s trilinear default would blur
         * the reflection towards the sheet's mean and wash the paint out. [S]
         * on the enum ordering, [C] on which two states the car pass sets. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* no unbind -- see the note by b3_hud_load_texture() */
        printf("[carfx] env map %s (%dx%d, uv=%s, mean %.3f)\n", path, ew, eh,
               g.envuv == 3 ? "up-angular" :
               g.envuv == 2 ? "panorama" :
               g.envuv == 1 ? "spheremap" : "raw-R.xy [C]", g.env_mean);
    }
    return g.tex_env != 0;
}

/* The sidecar reader, shared by the racer registry (.bgv tables, written by
 * tools/extract_carfx_art.py) and the non-racer one (.btv tables, written by
 * tools/extract_traffic_lights.py).  The two file formats are the same because
 * the two light tables are: model+0x1664[type] / model+0x16AC[type], stride
 * 0x30 = {float4 pos, float4 normal, float4 aux}, verified on all 40 shipped
 * .btv files as well as the 67 .bgv ones.                                [C] */
static int b3fx_load_lights(B3FXCar* c, const char* cls, const char* base)
{
    char path[192];
    snprintf(path, sizeof path, "build/cars/%s_%s.lights", cls, base);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    memset(c, 0, sizeof *c);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        float v;
        int t;
        char name[32];
        float px, py, pz, nx, ny, nz;
        if (sscanf(line, "halfwidth %f", &v) == 1) {
            c->halfwidth = v; c->have_shadow |= 1;
        } else if (sscanf(line, "outerfrontz %f", &v) == 1) {
            c->outerfrontz = v; c->have_shadow |= 2;
        } else if (sscanf(line, "frontz %f", &v) == 1) {
            c->frontz = v; c->have_shadow |= 4;
        } else if (sscanf(line, "rearz %f", &v) == 1) {
            c->rearz = v; c->have_shadow |= 8;
        } else if (sscanf(line, "outerrearz %f", &v) == 1) {
            c->outerrearz = v; c->have_shadow |= 16;
        } else if (sscanf(line, "light %d %31s %f %f %f %f %f %f",
                          &t, name, &px, &py, &pz, &nx, &ny, &nz) == 8) {
            if (c->nlights >= (int)(sizeof c->light / sizeof c->light[0]))
                continue;
            B3FXLight* l = &c->light[c->nlights++];
            /* loader Z-flip (RE_NOTES 12): every mesh the harness loads is
             * mirrored in Z, so these model-space tables must be too. */
            l->pos[0] = px; l->pos[1] = py; l->pos[2] = -pz;
            l->nrm[0] = nx; l->nrm[1] = ny; l->nrm[2] = -nz;
            l->type = t;
        }
    }
    fclose(f);
    if (c->have_shadow == 31) {
        c->outerfrontz = -c->outerfrontz;   /* same Z-flip */
        c->frontz      = -c->frontz;
        c->rearz       = -c->rearz;
        c->outerrearz  = -c->outerrearz;
    } else {
        c->have_shadow = 0;
    }
    return c->nlights;
}

int b3_carfx_load_car(int slot, const char* cls, const char* base)
{
    if (slot < 0 || slot >= B3_CARFX_MAX_CARS) return 0;
    return b3fx_load_lights(&g.car[slot], cls, base);
}

int b3_carfx_load_extra(int idx, const char* cls, const char* base)
{
    if (idx < 0 || idx >= B3_CARFX_MAX_EXTRA) return 0;
    return b3fx_load_lights(&g.extra[idx], cls, base);
}

void b3_carfx_init(void)
{
    memset(&g, 0, sizeof g);
    /* TUNED (user-authorized deviation 2026-08-13) -- section 1b.  On by
     * default; B3_CARFX_TUNE=0 restores the untouched recovered magnitudes so
     * the two can be compared side by side from one binary. */
    {
        const char* t = getenv("B3_CARFX_TUNE");
        g.tune = (t && atoi(t) == 0) ? 0 : 1;
    }
    g.envmod = 1.0f;
    /* RECOVERED environment, both inputs.  The nine SH coefficients are the
     * literals every car draw writes into modelInstance+0x5C, and the light
     * RGB defaults to Silver Lake's enviro.dat +0x60; b3_carfx_set_track()
     * moves it per track. */
    b3_carfx_set_sh(B3_CARFX_SH_RETAIL);
    /* SHINE-DATA: the pre-track default is the SELECTED track, through the
     * same B3_TRACK-first chain every other loader uses -- the literal
     * "US_C3_V1" that used to sit here was a per-track constant in C, and
     * it silently pinned Silver Lake's probes/envmap/sun colour on any
     * caller that never reached b3_carfx_set_track().  The harness calls
     * set_track() straight after init, so this only covers that gap. */
    {   const char* tid = getenv("B3_TRACK");
        if (!tid || !*tid) tid = getenv("B3_POSTFX_TRACK");
        if (!tid || !*tid) tid = "";
        if (!b3_carfx_set_track(tid))
            b3_carfx_set_light_rgb(1.0f, 1.0f, 1.0f);
    }
    g.tex_shadow = load_png("build/carfx/blobbyshadow.png");
    g.tex_corona = load_png("build/carfx/coronaglow.png");
    /* the stage-1 environment map is loaded by b3_carfx_set_track() above --
     * per track from build/tracks/<ID>/envmap.png, or from B3_CARFX_ENVMAP
     * when that is set.  See section 5C and set_track() for the provenance. */
    g.have_gl2 = load_gl2() && build_program();
    g.ready = (g.tex_shadow || g.tex_corona || g.have_gl2);
    printf("[carfx] shine=%s shadow=%s corona=%s\n",
           g.have_gl2 ? "on" : "OFF (no GL2)",
           g.tex_shadow ? "on" : "OFF (no build/carfx/blobbyshadow.png)",
           g.tex_corona ? "on" : "OFF (no build/carfx/coronaglow.png)");
    if (!g.tex_shadow || !g.tex_corona)
        printf("[carfx] run: python3 tools/extract_carfx_art.py\n");
}

int b3_carfx_ready(void) { return g.ready; }

void b3_carfx_shutdown(void)
{
    if (g.prog && p_glDeleteProgram) p_glDeleteProgram(g.prog);
    if (g.tex_shadow) glDeleteTextures(1, &g.tex_shadow);
    if (g.tex_corona) glDeleteTextures(1, &g.tex_corona);
    if (g.tex_env)    glDeleteTextures(1, &g.tex_env);
    b3_carfx_probes_free();
    memset(&g, 0, sizeof g);
}

/* ======================================================================
 * 7. Body / glass
 * ====================================================================== */
void b3_carfx_rot3_from_yaw(float yaw_rad, float out9[9])
{
    /* matches the harness' glRotatef(-yaw_deg, 0,1,0) for the car body */
    float s = sinf(-yaw_rad), c = cosf(-yaw_rad);
    out9[0] = c;  out9[1] = 0; out9[2] = s;
    out9[3] = 0;  out9[4] = 1; out9[5] = 0;
    out9[6] = -s; out9[7] = 0; out9[8] = c;
}

void b3_carfx_body_defaults(B3CarFxBodyParams* p)
{
    memset(p, 0, sizeof *p);
    p->rot3[0] = p->rot3[4] = p->rot3[8] = 1.0f;
    p->sh_scale = 1.0f;
    p->fade = 1.0f;
    p->has_texture = 1;
    p->has_normals = 0;
    p->paint_index = 0;
    p->slot = -1;      /* no probe lookup until the caller names a car slot */
}

/* `mode` selects the recovered pixel shader: 0 = the BODY def 0x003E8468,
 * 1 = the alt-material def 0x003E8558 (record+0x14 != 0), 2 = the glass pair
 * 0x003E8288 / 0x003E8378 composited into the harness' single blended draw.
 *
 * `spec_add` stays 1 on the body: its final combiner sums the specular into
 * D (`FinalABCD = 0x011D000C` -> A=c14.rgb, B=r1.a, C=zero, D=r0.rgb).  The
 * other two modes never reach that line.
 *
 * `refl_alb` modulates the reflection by the bound raster.  Retail does that
 * only on glass -- whose pass 2 samples t0.a, the raster FUN_000300A0
 * retargets (RE_NOTES 13) -- so the body passes 0.                   [GLUE] */
static void begin_common(const B3CarFxBodyParams* p, float P, float K,
                         float M, float R0, float tint,
                         float spec_add, float spec_lerp, float base_mul,
                         float refl_alb, int mode)
{
    float m[16];
    for (int i = 0; i < 16; i++) m[i] = g.shmat[i] * p->sh_scale;
    /* column-major for GL; the matrix is symmetric so no transpose needed */
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, g.prog);
    g_fx_prog_bound = 1;
    p_glUniformMatrix4fv(g.u_sh, 1, 0, m);
    /* uN2W is a 3x3 in COLUMN-major for GL; rot3 is row-major */
    {
        float c[9] = { p->rot3[0], p->rot3[3], p->rot3[6],
                       p->rot3[1], p->rot3[4], p->rot3[7],
                       p->rot3[2], p->rot3[5], p->rot3[8] };
        p_glUniformMatrix3fv(g.u_n2w, 1, 0, c);
        b3fx_upload_matrices();
    }
    p_glUniform3f(g.u_light, g.light_rgb[0], g.light_rgb[1], g.light_rgb[2]);
    /* P = ps c14.w: the combiner's stage-0 ALPHA constant, i.e. the FRESNEL
     * FLOOR of the reflection -- `r0.a = (1-c14.a)*v0.a + c14.a`.        [C] */
    p_glUniform1f(g.u_P, P);
    g.last_P = P;
    p_glUniform1f(g.u_K, K);
    p_glUniform1f(g.u_M, M);
    /* c15.z and c15.w*4 exactly as FUN_00031AB0 builds them (0x00031C60..) */
    p_glUniform1f(g.u_kz, 1.0f - K);
    p_glUniform1f(g.u_gain, (B3FX_COMBINER_RECIP * M / (1.0f - K)) * 4.0f);
    p_glUniform1f(g.u_R0, R0);
    p_glUniform1f(g.u_fade, p->fade);
    p_glUniform1f(g.u_tint, tint);
    p_glUniform1f(g.u_specadd, spec_add);
    p_glUniform1f(g.u_speclerp, spec_lerp);
    p_glUniform1f(g.u_basemul, base_mul);
    p_glUniform1f(g.u_reflalb, refl_alb);
    p_glUniform1f(g.u_envnorm, g.emax > 1e-6f ? 1.0f / g.emax : 1.0f);
    p_glUniform1f(g.u_glassr0a, B3FX_GLASS_C5_A);
    p_glUniform1f(g.u_glassr0b, B3FX_GLASS_C5_B);
    /* TUNED (user-authorized deviation 2026-08-13) -- section 1b.  With
     * B3_CARFX_TUNE=0 all three collapse to 1 and the recovered composition
     * comes back byte for byte. */
    /* SHINE-HOT -- section 1c.  The two halves of the reflection layer now
     * move together: SPEC_GAIN < 0 means "whatever REFL_GAIN is".  Every one
     * of these is overridable at runtime so the A/B never needs a rebuild. */
    {
        static float rg = -2.0f, sg, rt;
        static int   sgate;
        if (rg < -1.5f) {
            const char* e;
            rg = (e = getenv("B3_CARFX_REFLGAIN")) ? (float)atof(e)
                                                   : B3FX_T_REFL_GAIN;
            sg = (e = getenv("B3_CARFX_SPECGAIN")) ? (float)atof(e)
                                                   : B3FX_T_SPEC_GAIN;
            if (sg < 0.0f) sg = rg;
            rt = (e = getenv("B3_CARFX_ENVRT"))    ? (float)atof(e)
                                                   : B3FX_T_ENV_RT;
            sgate = (e = getenv("B3_CARFX_SPECGATE")) ? atoi(e)
                                                      : B3FX_T_SPEC_GATE;
        }
        p_glUniform1f(g.u_reflgain,  g.tune ? rg : 1.0f);
        p_glUniform1f(g.u_reflfloor, g.tune ? B3FX_T_REFL_FLOOR : 1.0f);
        p_glUniform1f(g.u_envmod,    g.tune ? g.envmod          : 1.0f);
        p_glUniform1f(g.u_specgain,  g.tune ? sg : 1.0f);
        p_glUniform1f(g.u_envrt,     g.tune ? rt : 1.0f);
        p_glUniform1i(g.u_specgate,  g.tune ? sgate : 0);
    }
    /* SHINE-SHAPE -- section 1d.  One switch turns the whole structure off
     * (B3_CARFX_SHAPE=0) without leaving TUNED, so "flat vs shaped" is a
     * one-variable A/B on the same binary; B3_CARFX_TUNE=0 clears it too and
     * the composition collapses to 1b/1c exactly. */
    {
        static float hu = -2.0f, hg, tb, sw, ec, sp, sgn, sf;
        if (hu < -1.5f) {
            const char* e;
            hu  = (e = getenv("B3_CARFX_HEMUP"))   ? (float)atof(e)
                                                   : B3FX_T_HEM_UPMIX;
            hg  = (e = getenv("B3_CARFX_GROUND"))  ? (float)atof(e)
                                                   : B3FX_T_HEM_GROUND;
            tb  = (e = getenv("B3_CARFX_TOPBOOST"))? (float)atof(e)
                                                   : B3FX_T_TOP_BOOST;
            sw  = (e = getenv("B3_CARFX_SKYW"))    ? (float)atof(e)
                                                   : B3FX_T_SKY_W;
            ec  = (e = getenv("B3_CARFX_ENVCON"))  ? (float)atof(e)
                                                   : B3FX_T_ENV_CON;
            sp  = (e = getenv("B3_CARFX_SUNPOW"))  ? (float)atof(e)
                                                   : B3FX_T_SUN_POW;
            sgn = (e = getenv("B3_CARFX_SUNGAIN")) ? (float)atof(e)
                                                   : B3FX_T_SUN_GAIN;
            sf  = (e = getenv("B3_CARFX_SUNFRES")) ? (float)atof(e)
                                                   : B3FX_T_SUN_FRESNEL;
        }
        int shape = b3fx_shape_on();
        p_glUniform1i(g.u_tunedfx,   shape);
        p_glUniform1f(g.u_hemup,     hu);
        p_glUniform1f(g.u_hemground, hg);
        p_glUniform1f(g.u_hemlo,     B3FX_T_HEM_LO);
        p_glUniform1f(g.u_hemhi,     B3FX_T_HEM_HI);
        p_glUniform1f(g.u_topboost,  tb);
        p_glUniform1f(g.u_skyw,      sw);
        p_glUniform1f(g.u_envcon,    ec);
        p_glUniform1f(g.u_envmean,   g.env_mean);
        p_glUniform3f(g.u_sundir, g.sun_dir[0], g.sun_dir[1], g.sun_dir[2]);
        p_glUniform1f(g.u_sunpow,    sp);
        p_glUniform1f(g.u_sungain,   sgn);
        p_glUniform1f(g.u_sunfres,   sf);
        p_glUniform1i(g.u_havesun,   (shape && g.have_sun) ? 1 : 0);
    }
    /* SHINE-UV: the sheet parameterisation, and the measurement switch */
    p_glUniform1i(g.u_envuv, g.envuv);
    {   const char* d = getenv("B3_CARFX_ENVDBG");
        p_glUniform1i(g.u_envdbg, d ? atoi(d) : 0); }
    p_glUniform1i(g.u_mode, mode);
    p_glUniform1i(g.u_hastex, p->has_texture ? 1 : 0);
    p_glUniform1i(g.u_hasnrm, p->has_normals ? 1 : 0);
    p_glUniform1i(g.u_tex, 0);
    p_glUniform1i(g.u_hasenv, g.tex_env ? 1 : 0);
    p_glUniform1i(g.u_env, 1);
    if (g.tex_env) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g.tex_env);
        glActiveTexture(GL_TEXTURE0);
    }
}

/* THE THREE THINGS THE COMPATIBILITY BUILTINS USED TO SUPPLY.
 *
 * uMVP and uMV are the retained renderer's own matrices, so a car and the
 * road under it are transformed by the identical arithmetic.  uEyeObj is the
 * one quantity the shader ever read out of gl_ModelViewMatrixInverse: the eye
 * position in OBJECT space, i.e. inverse(MV) applied to the origin.
 *
 * The modelview here is a rigid body transform with a possible mirror -- the
 * harness' whole world is the Z-reflection of the game's (RE_NOTES 12) -- so
 * a general affine inverse is used rather than the transpose shortcut, which
 * would be wrong the moment a scale or a reflection is in the matrix. */
/* 1 while one of this module's programs is the bound one, so a draw path that
 * issues its own glDrawArrays knows whether to refresh THESE uniforms or to
 * bind the retained program instead. */
int b3_carfx_program_bound(void) { return g_fx_prog_bound; }

/* Re-upload the transform.  Called per DRAW, not per car: a wrecked car's
 * panels and every wheel push their own matrix after the body pass began, and
 * ftransform() used to pick that up for free. */
void b3_carfx_sync_matrices(void)
{
    if (g_fx_prog_bound) b3fx_upload_matrices();
}

static void b3fx_upload_matrices(void)
{
    const float* mv = b3r_mv();
    float a[9], det, inv[9], eye[3];
    int i;

    if (g.u_mvp >= 0)
        p_glUniformMatrix4fv(g.u_mvp, 1, 0, b3r_mvp());
    if (g.u_mv >= 0)
        p_glUniformMatrix4fv(g.u_mv, 1, 0, mv);
    if (g.u_eyeobj < 0) return;

    /* the 3x3 linear part, column-major as GL stores it */
    for (i = 0; i < 3; i++) {
        a[i * 3 + 0] = mv[i * 4 + 0];
        a[i * 3 + 1] = mv[i * 4 + 1];
        a[i * 3 + 2] = mv[i * 4 + 2];
    }
    det = a[0] * (a[4] * a[8] - a[5] * a[7])
        - a[3] * (a[1] * a[8] - a[2] * a[7])
        + a[6] * (a[1] * a[5] - a[2] * a[4]);
    if (det > -1e-20f && det < 1e-20f) {
        eye[0] = eye[1] = eye[2] = 0.0f;
    } else {
        float d = 1.0f / det;
        inv[0] = (a[4] * a[8] - a[5] * a[7]) * d;
        inv[1] = (a[2] * a[7] - a[1] * a[8]) * d;
        inv[2] = (a[1] * a[5] - a[2] * a[4]) * d;
        inv[3] = (a[5] * a[6] - a[3] * a[8]) * d;
        inv[4] = (a[0] * a[8] - a[2] * a[6]) * d;
        inv[5] = (a[2] * a[3] - a[0] * a[5]) * d;
        inv[6] = (a[3] * a[7] - a[4] * a[6]) * d;
        inv[7] = (a[1] * a[6] - a[0] * a[7]) * d;
        inv[8] = (a[0] * a[4] - a[1] * a[3]) * d;
        /* inverse(MV) * (0,0,0,1) = -inverse(A) * t */
        eye[0] = -(inv[0] * mv[12] + inv[3] * mv[13] + inv[6] * mv[14]);
        eye[1] = -(inv[1] * mv[12] + inv[4] * mv[13] + inv[7] * mv[14]);
        eye[2] = -(inv[2] * mv[12] + inv[5] * mv[13] + inv[8] * mv[14]);
    }
    p_glUniform3f(g.u_eyeobj, eye[0], eye[1], eye[2]);
}

void b3_carfx_body_begin(const B3CarFxBodyParams* p)
{
    if (!g.have_gl2 || !p) return;
    /* TUNED (user-authorized deviation 2026-08-13): default the per-car sheen
     * weight, so a car with no probe lookup keeps the full recovered layer. */
    g.envmod = 1.0f;
    /* THE PROBE.  FUN_0019D400 rewrites this car's nine SH coefficients from
     * its own world position every frame before the draw (RE_CARFX 12, and
     * tools/extract_light_probes.py for the container).  Do it here, at the
     * same point in the frame, so every car is lit by the ground it is on. */
    if (p->slot >= 0 && p->slot < B3_CARFX_MAX_CARS && g.probes.ntri) {
        B3FXCar* c = &g.car[p->slot];
        float L[9];
        /* the harness' GL world is the Z-mirror of the game's, and the probe
         * mesh is stored in raw GAME coordinates (as build/collision.bin is) */
        float q[3] = { p->pos[0], p->pos[1], -p->pos[2] };
        int hit = b3_carfx_probe_sample(q, L);
        if (hit) {
            memcpy(c->sh, L, sizeof L);
            c->have_sh = 1;
        }
        /* a miss keeps the previous nine -- the game SKIPS the call
         * (JL @0x001AB128 / TEST AL,AL @0x0019D4B2)                     [C] */
        const float* nine = c->have_sh ? c->sh : B3_CARFX_SH_RETAIL;
        /* TUNED (user-authorized deviation 2026-08-13) -- section 1b, the
         * "cars don't react to the lights" half.  The recovered probe DOES
         * sweep about 2x over a lap, but 2x on a term that is already summed
         * with a bright reflection layer does not read as the car moving
         * through light and shade.  Expand the probe's own range about the
         * TRACK MEAN of its DC coefficient -- a pure scale on all nine, so
         * the directional shape of the irradiance is untouched and only the
         * amplitude moves; at the mean the gain is exactly 1, i.e. the
         * average-lit car is unchanged and only the extremes separate.  The
         * mean is the track's own shipped probe statistic, not a constant. */
        float tuned[9];
        if (g.tune && g.probes.l0mean > 1e-6f && nine[0] > 1e-6f) {
            float gain = powf(nine[0] / g.probes.l0mean,
                              B3FX_T_PROBE_CONTRAST - 1.0f);
            if (gain < B3FX_T_PROBE_LO) gain = B3FX_T_PROBE_LO;
            if (gain > B3FX_T_PROBE_HI) gain = B3FX_T_PROBE_HI;
            for (int k = 0; k < 9; k++) tuned[k] = nine[k] * gain;
            nine = tuned;
        }
        /* TUNED: and the reflection's own local weight.  The probe's DC
         * coefficient -- "how much light is there here" -- is remapped over
         * the track's own [l0min, l0max], so the sky sheen fades out in a
         * cutting or a tunnel and comes back in the open.  That is the other
         * half of the same complaint, and it is what stops a bright daylight
         * sky sitting on a car INSIDE a tunnel (capture f3200).  Measured on
         * the UNTUNED coefficient, so the ramp's domain is the same range the
         * loader measured. */
        /* SHINE-SHADE: the ramp's TOP anchor is the track's MEAN probe, not
         * its brightest.  Anchored on l0max, an ordinary open-road probe is
         * a fraction of the way up the ramp -- measured on US_C3_V1 the car
         * drives at L00 = 0.2094 in a track range [0.1500, 0.2875], i.e.
         * t = 0.43 and envmod = 0.44, so the sheen was more than HALVED on
         * plain daylight road and the shade ramp read as a global dimmer
         * instead of a shade cue.  Anchoring on the mean makes the
         * average-lit car carry the full recovered layer (envmod = 1, which
         * is also where the sibling probe-contrast gain is exactly 1) and
         * leaves the ramp to do only what it was for: dimming the sheen
         * BELOW the mean, in a cutting or a tunnel.  Still the track's own
         * shipped statistic -- no constant, no per-track number. */
        float shade_top = g.probes.l0mean
                        + (g.probes.l0max - g.probes.l0mean)
                          * B3FX_T_SHADE_TOP;
        if (g.tune && shade_top > g.probes.l0min + 1e-6f) {
            float raw = (c->have_sh ? c->sh : B3_CARFX_SH_RETAIL)[0];
            float t = (raw - g.probes.l0min) / (shade_top - g.probes.l0min);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            g.envmod = B3FX_T_ENV_SHADE_MIN + (1.0f - B3FX_T_ENV_SHADE_MIN)
                     * powf(t, B3FX_T_ENV_SHADE_POW);
        } else {
            g.envmod = 1.0f;
        }
        b3_carfx_set_sh(nine);
        /* PROBETRACE (diagnostic only, off by default): one line per car per
         * frame with the position the cast was taken at, whether it hit, the
         * RAW probe DC coefficient, the TUNED one the shader actually gets,
         * the reflection's local shade weight and E(+Y) -- everything the
         * "the shadow does not touch my car" report needs measured rather
         * than inferred.  Consumed by tools/validate_light_probes.py.
         * B3_CARFX_PROBETRACE=1 traces slot 0; =all traces every slot.  */
        {   static const char* tr = (const char*)1;
            if (tr == (const char*)1) tr = getenv("B3_CARFX_PROBETRACE");
            if (tr && *tr && (p->slot == 0 || strcmp(tr, "all") == 0)) {
                const float up[3] = { 0.0f, 1.0f, 0.0f };
                const float* raw = c->have_sh ? c->sh : B3_CARFX_SH_RETAIL;
                printf("[probetrace] slot %d pos %.3f %.3f %.3f hit %d "
                       "L00raw %.6f L00tuned %.6f envmod %.6f E %.6f "
                       "l0min %.6f l0mean %.6f l0max %.6f\n",
                       p->slot, q[0], q[1], q[2], hit,
                       raw[0], nine[0], g.envmod, b3_carfx_irradiance(up),
                       g.probes.l0min, g.probes.l0mean, g.probes.l0max);
            }
        }
        if (getenv("B3_CARFX_PROBEDBG")) {           /* diagnostic only */
            static int nhit, nmiss, ntick;
            if (hit) nhit++; else nmiss++;
            if (((ntick++) % 600) == 0) {
                const float up[3] = { 0.0f, 1.0f, 0.0f };
                printf("[carfx] probe slot %d @(%.1f %.1f %.1f) %s "
                       "L00=%.4f E(+Y)=%.4f hits=%d misses=%d\n",
                       p->slot, q[0], q[1], q[2], hit ? "HIT" : "miss",
                       c->have_sh ? c->sh[0] : B3_CARFX_SH_RETAIL[0],
                       b3_carfx_irradiance(up), nhit, nmiss);
            }
        }
    }
    int i = p->paint_index;
    if (i < 0 || i >= B3FX_SHINE_VARIANTS) i = 0;
    float P = p->alt_material ? B3FX_ALT_P : B3FX_P[i];
    begin_common(p, P, B3FX_K[i], B3FX_M[i], B3FX_BODY_R0, 1.0f,
                 p->alt_material ? 0.0f : 1.0f,          /* spec_add  */
                 0.0f,                                   /* spec_lerp */
                 1.0f,                                   /* base_mul  */
                 0.0f,                                   /* refl_alb  */
                 p->alt_material ? 1 : 0);               /* mode      */
}

void b3_carfx_body_end(void)
{
    if (!g.have_gl2) return;
    b3r_use_program_via((void (*)(unsigned))p_glUseProgram, 0);
    g_fx_prog_bound = 0;
}

void b3_carfx_glass_begin(const B3CarFxBodyParams* p, float tint)
{
    if (!g.have_gl2 || !p) return;
    /* mode 2 = the two recovered glass defs composited: pass 1 is BLACK with
     * alpha tint*raster.a (def 0x003E8288, ps c13 = (0,0,0,tint)) and pass 2
     * is the environment plus its specular at alpha (r + s)*raster.a
     * (def 0x003E8378).                                                  [C] */
    begin_common(p, B3FX_GLASS_P, B3FX_GLASS_K, B3FX_GLASS_M,
                 B3FX_GLASS_R0, tint, 0.0f, 0.0f, 0.0f, 1.0f, 2);
}

void b3_carfx_glass_end(void) { b3_carfx_body_end(); }

/* ======================================================================
 * 8. Shadow -- FUN_0019A580 / FUN_0019A7C0 / FUN_00043570 / FUN_00043350
 * ====================================================================== */
void b3_carfx_shadow_pass_begin(void)
{
    if (!g.tex_shadow) return;
    /* The attrib push stays -- it hands the frame back the depth / blend /
     * alpha-test state this pass moves -- but it no longer protects the
     * geometry: the strips are buffered by the retained batcher, so
     * b3_carfx_shadow_pass_end() flushes before the pop.
     *
     * NOTE this pass' shader is the RETAINED renderer's, not B3FX_VS/B3FX_FS:
     * the blob is a plain textured quad, and render_frame() has already run
     * b3_carfx_body_end() -- which does glUseProgram(0) -- on every car before
     * it opens this pass, so b3r2d_begin() binding the retained program takes
     * nothing away from the body draw. */
    b3r_state_push();
    b3r2d_begin();
    /* The whole tuple, once: one texture for every car, plus the eight GL
     * calls this function used to issue.  GL_DEPTH_TEST is the one field this
     * pass never set for itself -- it INHERITED it, on, from render_frame()'s
     * 3D block (and every retained world batch leaves depth_test 1) -- so
     * depth_test 1 is what it inherited and what it keeps. */
    B3RState st;
    st.tex        = g.tex_shadow;
    st.mode       = B3R_TEX_MODULATE;
    st.blend      = B3R_BLEND_ALPHA;      /* 0x302/0x303            [C] */
    st.alpha_ref  = 0.0f;                 /* GREATER 0, RS 60 = 1   [C] */
    st.depth_mask = 0;                    /* RS 64 = 0              [C] */
    st.depth_test = 1;
    st.depth_func = 0;
    st.cull       = 0;
    b3r_batch_state(&st);
}

void b3_carfx_shadow_draw(int slot, const float pos[3], float yaw_rad,
                          float ground_y, float ramp, float air_h)
{
    b3_carfx_shadow_draw_n(slot, pos, yaw_rad, ground_y, ramp, air_h, NULL);
}

void b3_carfx_shadow_draw_n(int slot, const float pos[3], float yaw_rad,
                            float ground_y, float ramp, float air_h,
                            const float ground_n[3])
{
    if (!g.tex_shadow || slot < 0 || slot >= B3_CARFX_MAX_CARS) return;
    const B3FXCar* c = &g.car[slot];
    if (!c->have_shadow) return;

    /* FUN_0019A7C0: airFade = 1 - min(h*0.8, 1); skip when <= 0        [C] */
    float t = air_h * B3FX_SHADOW_AIRSLOPE;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    float air_fade = 1.0f - t;
    if (air_fade <= 0.0f) return;

    /* alpha = min(1, ramp * 2 * 3.3333333) * 0.7 * airFade             [C]
     * (the ramp input is vehicle+0x70, semantics [?] -- callers pass 1) */
    float a = ramp * B3FX_SHADOW_RAMP;
    if (a > 1.0f) a = 1.0f;
    if (a < 0.0f) a = 0.0f;
    a *= B3FX_SHADOW_DARK * air_fade;
    if (a <= 0.0f) return;

    /* scale = airFade * 0.4 + 1.0                                      [C] */
    float s = air_fade * B3FX_SHADOW_SIZEGAIN + 1.0f;

    float hw = c->halfwidth * s;
    /* FUN_00043570 emits eight vertices in four rows.  Rows 0 and 3 take the
     * BOUNDING-BOX Z's (the register copies xmm2/xmm1 made at 0x0019A8EE /
     * 0x0019A8E3, before the axle Z's overwrite the same fields in memory at
     * 0x0019A917 / 0x0019A904); rows 1 and 2 take the AXLE Z's.  That pairs
     * the blob's two soft V-cap bands with the nose/tail overhangs and
     * stretches its middle band across the wheelbase.                  [C] */
    const float zs[4] = { c->outerfrontz * s, c->frontz * s,
                          c->rearz * s, c->outerrearz * s };
    const float vs[4] = { B3FX_SHADOW_V0, B3FX_SHADOW_V1,
                          B3FX_SHADOW_V2, B3FX_SHADOW_V3 };

    /* THIS BLOB IS THE ONE MIGRATED PASS WHOSE VERTICES ARE NOT IN WORLD
     * SPACE.  Its eight corners are model-space and the pose below is a
     * fixed-function matrix, which the retained program reads through
     * ftransform() AT DRAW TIME, not at emit time -- so the batch has to be
     * drawn while this matrix is still on the stack.  Hence the flush in front
     * of every raw matrix call here, and one car per draw rather than one draw
     * for the pass.  (Composing the pose on the CPU instead would merge them,
     * but it is a DIFFERENT float association from what glTranslatef /
     * glRotatef feed ftransform(), and the pinned frame would move.) */
    b3r2d_flush();
    b3r_push();
    b3r_translate(pos[0], ground_y + B3FX_SHADOW_LIFT, pos[2]);
    /* Drape the blob onto the ground PLANE: rotate local +Y onto the probed
     * ground normal before the yaw spin, so on slopes/crests the quad lies
     * on the surface instead of slicing into the upslope and losing half
     * of itself under the terrain (user report, debug dump 030). Flat
     * ground (n = +Y) is a no-op, so the retail-recovered flat behaviour
     * is unchanged there. GLUE mechanism, recovered quad/fade unchanged. */
    if (ground_n && ground_n[1] < 0.99995f && ground_n[1] > 0.05f) {
        float ang = acosf(ground_n[1] > 1.0f ? 1.0f : ground_n[1])
                  * (180.0f / (float)M_PI);
        /* axis = cross(+Y, n) = (n.z, 0, -n.x) */
        b3r_rotate(ang, ground_n[2], 0.0f, -ground_n[0]);
    }
    b3r_rotate(-yaw_rad * (180.0f / (float)M_PI), 0.0f, 1.0f, 0.0f);
    b3r2d_color(1.0f, 1.0f, 1.0f, a);        /* pixel-shader c0 = (1,1,1,a) */
    b3r2d_prim(B3R2D_TRI_STRIP);
    for (int r = 0; r < 4; r++) {
        b3r2d_uv(0.0f, vs[r]); b3r2d_vertex3(-hw, 0.0f, zs[r]);
        b3r2d_uv(1.0f, vs[r]); b3r2d_vertex3( hw, 0.0f, zs[r]);
    }
    b3r2d_prim_end();
    b3r2d_flush();               /* draw it UNDER this car's matrix */
    b3r_pop();
}

void b3_carfx_shadow_pass_end(void)
{
    if (!g.tex_shadow) return;
    b3r2d_end();      /* nothing is left buffered, but end the pass properly */
    b3r_state_pop();
}

/* ======================================================================
 * 9. Light coronas -- FUN_00187C70 / FUN_001879E0 / FUN_00187BE0 /
 *                     FUN_00042B00 / FUN_00042BC0
 * ====================================================================== */
unsigned b3_carfx_light_byte(int braking, int reversing, unsigned ambient)
{
    unsigned b = ambient & 0xFFu;
    if (braking)   b |= B3_CARFX_LIGHT_BRAKE;     /* OR 0x10 @0x0011BFC3 [S] */
    if (reversing) b |= B3_CARFX_LIGHT_REVERSE;   /* OR 0x80 @0x0011BFD6 [S] */
    return b;
}

void b3_carfx_corona_pass_begin(void)
{
    if (!g.tex_corona) return;
    /* Attrib push kept for the state; the quads themselves are buffered by the
     * retained batcher and flushed by b3_carfx_corona_pass_end() before the
     * pop.  Like the shadow pass this runs with the carfx program UNBOUND --
     * b3_carfx_body_end() has already done glUseProgram(0) for every car -- so
     * b3r2d_begin() binding the retained program takes nothing away. */
    b3r_state_push();
    b3r2d_begin();
    /* The whole tuple, once: one raster for every corona in the pass.
     * The literal Xbox blend factors for this pass were not decoded [?];
     * the pool colour is multiplied by 64.0 (DAT_0035BF1C) and the colour
     * constants exceed 1.0 (1.5 / 1.4), which only makes sense saturating
     * into an additive blend, and coronaglow ships fully opaque with the
     * glow in RGB.  Additive is therefore the GLUE choice -- B3R_BLEND_SA_ONE
     * IS glBlendFunc(SRC_ALPHA, ONE).                                      */
    B3RState st;
    st.tex        = g.tex_corona;
    st.mode       = B3R_TEX_MODULATE;
    st.blend      = B3R_BLEND_SA_ONE;
    st.alpha_ref  = -1.0f;        /* GL_ALPHA_TEST was off */
    st.depth_mask = 0;
    st.depth_test = 1;            /* occluded by world geometry (walls) */
    st.depth_func = 0;
    st.cull       = 0;
    b3r_batch_state(&st);
}

/* The corona emitter placement, taking the car's FULL object->world rotation.
 *
 * FULL POSE, TUNED (user-authorized deviation 2026-08-13) in the sense that
 * the harness now hands the same matrix the body draw uses instead of a yaw:
 * the game transforms every light record by the model instance's own matrix,
 * so this is a fidelity FIX rather than a magnitude change -- yaw-only
 * placement left both tail emitters hanging off a rolled wreck (debug dump
 * 018) and drifted them on crests, the same class of error as the yaw-only
 * body draw that was corrected earlier.  `R` is row-major object->world. */
static void b3fx_corona_draw_table(const B3FXCar* c, const float pos[3],
                                   const float R[9], unsigned light_byte)
{
    if (!g.tex_corona || !c || c->nlights <= 0 || !light_byte || !pos) return;

    /* Camera basis + position out of the world view matrix.  This used to be
     * a glGetFloatv(GL_MODELVIEW_MATRIX) -- one SYNCHRONOUS GL call PER CAR
     * per frame, and on the web port a synchronous call was measured at
     * 39.2 us proxied / 0.24 us direct (docs/web/webprof_sweep.md section 2).
     * render_frame() already handed the same matrix to the retained renderer,
     * so read it from there instead of asking the driver. */
    const float* mv = b3r_view();
    float rt[3] = { mv[0], mv[4], mv[8] };
    float up[3] = { mv[1], mv[5], mv[9] };
    float cam[3] = {
        -(mv[0]*mv[12] + mv[1]*mv[13] + mv[2]*mv[14]),
        -(mv[4]*mv[12] + mv[5]*mv[13] + mv[6]*mv[14]),
        -(mv[8]*mv[12] + mv[9]*mv[13] + mv[10]*mv[14]) };

    /* size = (dist*0.02 + 1.0) * typeMult, halved into the pool record  [C] */
    float dx = cam[0] - pos[0], dy = cam[1] - pos[1], dz = cam[2] - pos[2];
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    float base = dist * B3FX_CORONA_DISTGAIN + 1.0f;
    if (dist > B3FX_CORONA_FAR) return;

    b3r2d_prim(B3R2D_QUADS);
    for (int k = 0; k < B3FX_NCORONA; k++) {
        const B3FXCorona* cc = &B3FX_CORONAS[k];
        if (!(light_byte & cc->bit)) continue;
        /* brake (0x10) is tested before tail (0x08) and wins             [C] */
        if (cc->bit == B3_CARFX_LIGHT_TAIL &&
            (light_byte & B3_CARFX_LIGHT_BRAKE)) continue;
        if (cc->bit == B3_CARFX_LIGHT_HEAD &&
            (light_byte & B3_CARFX_LIGHT_HEAD_HI)) continue;
        float half = base * cc->mult * B3FX_CORONA_HALF;
        /* TUNED (user-authorized deviation 2026-08-13): the quad is emitted at
         * the MIDPOINT between the lamp and the eye ([S], to keep it in front
         * of the bodywork), which halves its distance and therefore DOUBLES
         * the angle it subtends.  Scale the half-extent by the same (1 - PULL)
         * so the corona covers the angle the recovered size law actually
         * specifies at the lamp's own distance.  Without it an oncoming pair
         * of headlights merges into a saturated white slab twice the width of
         * the car carrying them. */
        /* PULL REINTERPRETED (user report: "headlights and taillights can
         * be seen through walls"): reading record+0x14's 0.5 as a FRACTION
         * of the eye distance put the quad halfway to the camera -- it
         * z-cleared every wall within half the lamp's distance. Retail
         * lamps are occluded by world geometry, so the far more consistent
         * reading is 0.5 METRES toward the eye: exactly enough to clear
         * the lamp housing, still behind walls. [S -> S, behaviour-
         * matched; the fractional-size compensation is retired with it.] */
        for (int i = 0; i < c->nlights; i++) {
            const B3FXLight* l = &c->light[i];
            if (l->type != cc->type) continue;
            /* full object->world rotation, row-major: world = R * local */
            float wp[3] = {
                pos[0] + R[0]*l->pos[0] + R[1]*l->pos[1] + R[2]*l->pos[2],
                pos[1] + R[3]*l->pos[0] + R[4]*l->pos[1] + R[5]*l->pos[2],
                pos[2] + R[6]*l->pos[0] + R[7]*l->pos[1] + R[8]*l->pos[2] };
            float wn[3] = {
                R[0]*l->nrm[0] + R[1]*l->nrm[1] + R[2]*l->nrm[2],
                R[3]*l->nrm[0] + R[4]*l->nrm[1] + R[5]*l->nrm[2],
                R[6]*l->nrm[0] + R[7]*l->nrm[1] + R[8]*l->nrm[2] };
            float ex = cam[0]-wp[0], ey = cam[1]-wp[1], ez = cam[2]-wp[2];
            float el = sqrtf(ex*ex + ey*ey + ez*ez);
            if (el < 1e-4f) continue;
            /* FUN_00187BE0: reject unless dot(eye, lightNormal) > 0, then
             * scale the colour by that dot -- a pure cosine falloff on the
             * light's own normal, no 1/r^2.  The eye vector is normalised
             * here (GLUE: the game keeps it unnormalised and relies on the
             * x64 overbright saturating).                             [C/S] */
            float d = (ex*wn[0] + ey*wn[1] + ez*wn[2]) / el;
            if (d <= 0.0f) continue;
            /* the quad is emitted at the midpoint between the light and the
             * eye (corner = pos +- right*size +- up*size - 0.5*(pos-cam)),
             * which is what keeps it in front of the bodywork.         [S] */
            float pk = B3FX_CORONA_PULL / el;   /* 0.5 m along the eye ray */
            float qx = wp[0] + pk * ex;
            float qy = wp[1] + pk * ey;
            float qz = wp[2] + pk * ez;
            /* TUNED (user-authorized deviation 2026-08-13): B3FX_T_CORONA_GAIN
             * on the emitted colour, see section 1b. */
            float cg = d * (g.tune ? B3FX_T_CORONA_GAIN : 1.0f);
            b3r2d_color(cc->rgb[0]*cg, cc->rgb[1]*cg, cc->rgb[2]*cg, 1.0f);
            b3r2d_uv(0.0f, 0.0f);
            b3r2d_vertex3(qx - (rt[0]+up[0])*half, qy - (rt[1]+up[1])*half,
                          qz - (rt[2]+up[2])*half);
            b3r2d_uv(1.0f, 0.0f);
            b3r2d_vertex3(qx + (rt[0]-up[0])*half, qy + (rt[1]-up[1])*half,
                          qz + (rt[2]-up[2])*half);
            b3r2d_uv(1.0f, 1.0f);
            b3r2d_vertex3(qx + (rt[0]+up[0])*half, qy + (rt[1]+up[1])*half,
                          qz + (rt[2]+up[2])*half);
            b3r2d_uv(0.0f, 1.0f);
            b3r2d_vertex3(qx - (rt[0]-up[0])*half, qy - (rt[1]-up[1])*half,
                          qz - (rt[2]-up[2])*half);
        }
    }
    b3r2d_prim_end();
    /* ONE DRAW PER CAR, WHICH IS WHERE glEnd() WAS -- and that is not a
     * stylistic choice, it is what the pinned frame demands.  Letting the
     * batcher merge every car's coronas into one glDrawArrays is legal (same
     * primitives, same order, same state) and it MOVES PIXELS: 8 of them at
     * US_C3_V1 frame 900, at 2/255, on the player's rear glass where two
     * coronas overlap.  Flushing at the old draw boundary puts the frame back
     * to bit-identical.  The merge was never the prize here anyway -- the cost
     * this wave removes is gl4es re-uploading and re-pointing three scratch
     * VBOs per glBegin/glEnd (12.2 real calls a draw, docs/web/webprof_sweep.md
     * section 6), and one static-buffer draw per car pays that off whether or
     * not the cars share it. */
    b3r2d_flush();
}

void b3_carfx_corona_draw_pose(int slot, const float pos[3],
                               const float rot3[9], unsigned light_byte)
{
    if (slot < 0 || slot >= B3_CARFX_MAX_CARS) return;
    static const float I[9] = { 1,0,0, 0,1,0, 0,0,1 };
    b3fx_corona_draw_table(&g.car[slot], pos, rot3 ? rot3 : I, light_byte);
}

void b3_carfx_corona_draw(int slot, const float pos[3], float yaw_rad,
                          unsigned light_byte)
{
    float R[9];
    b3_carfx_rot3_from_yaw(yaw_rad, R);
    b3_carfx_corona_draw_pose(slot, pos, R, light_byte);
}

void b3_carfx_corona_draw_extra(int idx, const float pos[3],
                                const float rot3[9], unsigned light_byte)
{
    if (idx < 0 || idx >= B3_CARFX_MAX_EXTRA) return;
    static const float I[9] = { 1,0,0, 0,1,0, 0,0,1 };
    b3fx_corona_draw_table(&g.extra[idx], pos, rot3 ? rot3 : I, light_byte);
}

void b3_carfx_corona_pass_end(void)
{
    if (!g.tex_corona) return;
    b3r2d_end();      /* flush the batch BEFORE the pop takes its state away */
    b3r_state_pop();
}
