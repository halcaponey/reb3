#include "burnout3_trackmesh.h"
/* THE STATE SHADOW HAS TO SEE THIS FILE.  It sets the winding and turns
 * culling on for the whole world pass (below), and it was the ONE renderer
 * translation unit not including burnout3_render.h -- so those two calls went
 * straight to the driver while the shadow kept believing GL's defaults.  A
 * shim then filtered a later glDisable(GL_CULL_FACE) as redundant and the
 * frame came back 0.66-1.28% brighter (>2/255) with the mean up 0.2 on every
 * gate frame.  Same class of defect as the double-sided cars in the first
 * wave, and found the same way. */
#include "burnout3_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <SDL2/SDL_video.h>
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

// World headroom curve -- TUNED (user-authorized deviation 2026-08-13).
// Positive lifts, negative compresses, 0 is the identity. See
// trackmesh_group_vertex_color() for the shape, the reason and the reference
// measurements this was judged against.
#ifndef TRACKMESH_LIFT_G
#define TRACKMESH_LIFT_G 0.0f
#endif

// Minimal OBJ reader: only the subset tools/extract_track.py emits (v, vt, f
// with v/vt indices, triangles only, usemtl spans, one mtllib). Deliberately
// not a general OBJ parser.

// ---- WHOLE-FILE TEXT READING ------------------------------------------------
//
// THE DEFECT THIS FIXES.  Both readers below used to walk their file with
// fgets() into a 512-byte stack buffer.  That is a fine idiom on a desktop and
// a catastrophic one in a browser:
//
//   * musl (which is Emscripten's libc) gives every FILE a BUFSIZ = 1024 byte
//     buffer -- __fdopen.c allocates `sizeof *f + UNGET + BUFSIZ` and sets
//     f->buf_size = BUFSIZ.  So one read() syscall per KILOBYTE of file.
//   * the web port links -sPROXY_TO_PTHREAD, which puts main() on a worker
//     while the JS filesystem stays on the main browser thread.  Emscripten's
//     _fd_read therefore opens with
//         if (ENVIRONMENT_IS_PTHREAD) return proxyToMainThread(91, 0, 1, ...)
//     -- proxy mode 1, a BLOCKING round trip that the game thread cannot
//     complete until the main thread returns to its event loop.
//
//   US_C3_V1's track.obj is 16 MB, so that is ~16 000 blocking round trips for
//   one file, and the big tracks (AS_M1_V1, 77 MB) are five times that.
//   Measured: 5.2 s of warm web load for a file the desktop parses in 0.27 s.
//
// Reading the file in ONE fread instead makes it ONE round trip -- musl's
// fread hands a request larger than the buffer straight to __stdio_read, which
// reads it directly into the caller's memory (stdio/fread.c, `f->read(f, dest,
// l)`).  The line splitting then happens in wasm memory, where it costs
// nothing.
//
// EQUIVALENCE TO fgets.  tm_line() returns each line NUL-terminated with the
// '\n' removed and any '\r' left in place, which is what fgets left minus the
// newline; no parser below looks at either (they are all sscanf conversions and
// fixed-prefix strncmp).  The one real behavioural difference is that fgets
// SPLIT a line longer than 511 bytes into two "lines" and this does not -- an
// improvement, and moot for the shipped data: the longest line in any track.obj
// or track.mtl this repo produces is 62 characters.
//
// B3_IO_SMALL=1 puts the OLD 1 KB-at-a-time path back, out of the same binary,
// so the before/after above can be measured without swapping builds -- the same
// rule the rest of this port's measurements follow (see B3_WEB_PRESENT in
// docs/web/webprof_sweep.md).  It is a measurement switch and nothing else:
// both paths hand the parser identical lines.  The other two loaders that had
// the same defect read it too, so one setting moves all three:
// burnout3_collision.c (B3COL_IOBUF) and burnout3_carfx.c (B3FX_IOBUF).
typedef struct {
    char*  buf;      // the whole file, NUL-terminated  (slurp path)
    size_t len;
    size_t pos;
    FILE*  f;        // non-NULL only under B3_IO_SMALL=1 (the old path)
    char   line[512];
} TMText;

static int tm_stdio_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("B3_IO_SMALL");
        mode = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return mode;
}

static int tm_slurp(TMText* t, const char* path) {
    FILE* f = fopen(path, "rb");
    long  n;
    size_t got;

    t->buf = NULL; t->len = 0; t->pos = 0; t->f = NULL;
    if (!f) return -1;
    if (tm_stdio_mode()) {
        /* The size, so the pump's progress fraction means the same thing on
         * both sides of the A/B -- two seeks against the thousands of reads
         * this path is here to demonstrate. */
        if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0)
            t->len = (size_t)n;
        fseek(f, 0, SEEK_SET);
        t->f = f;
        return 0;
    }

    /* The size up front, so the read is one call and the buffer one malloc.
     * A stream that cannot seek (never the case for these files, but the
     * loader must not depend on that) falls back to reading in big chunks --
     * still O(size / 8 MB) syscalls rather than O(size / 1 KB). */
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        t->buf = (char*)malloc((size_t)n + 1);
        if (!t->buf) { fclose(f); return -1; }
        got = fread(t->buf, 1, (size_t)n, f);
        t->len = got;
    } else {
        size_t cap = 1u << 23;              /* 8 MB */
        t->buf = (char*)malloc(cap + 1);
        if (!t->buf) { fclose(f); return -1; }
        for (;;) {
            got = fread(t->buf + t->len, 1, cap - t->len, f);
            t->len += got;
            if (t->len < cap) break;        /* short read: end of file */
            {   char* nb = (char*)realloc(t->buf, cap * 2 + 1);
                if (!nb) { free(t->buf); t->buf = NULL; fclose(f); return -1; }
                t->buf = nb;
                cap *= 2;
            }
        }
    }
    fclose(f);
    t->buf[t->len] = '\0';
    return 0;
}

static void tm_free_text(TMText* t) {
    if (t->f) { fclose(t->f); t->f = NULL; }
    free(t->buf); t->buf = NULL; t->len = 0;
}

/* The next line, or NULL at end of file.  Terminates in place. */
static char* tm_line(TMText* t) {
    char* s;
    char* nl;
    if (t->f) {
        if (!fgets(t->line, (int)sizeof t->line, t->f)) return NULL;
        t->pos += strlen(t->line);
        return t->line;
    }
    if (t->pos >= t->len) return NULL;
    s  = t->buf + t->pos;
    nl = (char*)memchr(s, '\n', t->len - t->pos);
    if (nl) { *nl = '\0'; t->pos = (size_t)(nl - t->buf) + 1; }
    else    { t->pos = t->len; }            /* last line, no trailing newline */
    return s;
}

// ---- THE LOAD PUMP ----------------------------------------------------------
// See the contract in the header.  Additive: with no callback installed this
// costs one null test per chunk of lines.
static TrackMeshPumpFn g_pump_fn;
static void*           g_pump_user;

void trackmesh_set_pump(TrackMeshPumpFn fn, void* user) {
    g_pump_fn   = fn;
    g_pump_user = user;
}

// Directory part of `path` (including trailing slash) into `dir`.
static void path_dir(const char* path, char* dir, size_t cap) {
    const char* slash = strrchr(path, '/');
    if (!slash) { dir[0] = '\0'; return; }
    size_t n = (size_t)(slash - path) + 1;
    if (n >= cap) n = cap - 1;
    memcpy(dir, path, n);
    dir[n] = '\0';
}

// Fill in the per-group material state from the MTL that tools/extract_track.py
// writes: the map_Kd path plus its "# <key> <value>" annotation lines, each of
// which records one field of the game's own 0x28-byte material record. Texture
// paths in the MTL are relative to the MTL's own directory.
//
// The MTL's `newmtl` identity is (texture, shader class, flag word), not the
// texture alone -- US_C3_V1 ships GL_road5, GL_road7 and GL_road7grass as both
// an unshaded class-0 record and a specular class-1 one, and several backdrop
// cards appear with and without D3DCULL_NONE. Distinct records get a
// "<texture>.mNN" suffix; nothing here has to know that, it just matches the
// usemtl name it was given.
static void resolve_materials(TrackMesh* m, const char* obj_path, const char* mtl_name) {
    if (!mtl_name[0] || m->group_count == 0) return;

    char dir[256], mtl_path[512];
    path_dir(obj_path, dir, sizeof(dir));
    snprintf(mtl_path, sizeof(mtl_path), "%s%s", dir, mtl_name);

    TMText mtl;
    char* line;
    if (tm_slurp(&mtl, mtl_path) != 0) return;

    char cur[TRACKMESH_NAME_LEN] = "";
    while ((line = tm_line(&mtl)) != NULL) {
        char arg[256];
        int ival;
        unsigned uval;
        float s, p, q;
        int gate;
        int ai, af, as, ap;
        // The scene block sits in the MTL's header comment, before any
        // `newmtl`, and describes the whole track (see TrackScene).
        if (sscanf(line, "# scene_light_dir %f %f %f", &s, &p, &q) == 3) {
            // Same reflection the positions get: the game's vector is in game
            // space and TrackMesh lives in the mirrored one.
            m->scene.light_dir[0] = s;
            m->scene.light_dir[1] = p;
            m->scene.light_dir[2] = -q;
            m->scene.valid = 1;
            continue;
        }
        if (sscanf(line, "# scene_light_rgb %f %f %f", &s, &p, &q) == 3) {
            m->scene.light_rgb[0] = s;
            m->scene.light_rgb[1] = p;
            m->scene.light_rgb[2] = q;
            continue;
        }
        if (sscanf(line, "# scene_fog_rgb %f %f %f", &s, &p, &q) == 3) {
            m->scene.fog_rgb[0] = s;
            m->scene.fog_rgb[1] = p;
            m->scene.fog_rgb[2] = q;
            continue;
        }
        if (sscanf(line, "# scene_fog_range %f %f %f", &s, &p, &q) == 3) {
            m->scene.fog_start = s;
            m->scene.fog_end = p;
            m->scene.fog_far = q;
            m->scene.fog_enabled = (p > s);
            continue;
        }
        if (sscanf(line, "newmtl %63s", cur) == 1) continue;
        if (!cur[0]) continue;

        // Everything below applies to every group using the current material.
        for (int g = 0; g < m->group_count; g++) {
            TrackMeshGroup* grp = &m->groups[g];
            if (strcmp(grp->material, cur) != 0) continue;
            if (sscanf(line, "map_Kd %255s", arg) == 1)
                snprintf(grp->texture, sizeof(grp->texture), "%s%s", dir, arg);
            else if (sscanf(line, "# class %d", &ival) == 1) {
                grp->cls = ival;
                // Every material the extractor writes carries a "# class"
                // line, so this is the presence flag for the whole record.
                grp->have_material = 1;
            }
            else if (sscanf(line, "# flags 0x%x", &uval) == 1)
                grp->flags = uval;
            else if (strncmp(line, "# alpha_test 1", 14) == 0)
                grp->alpha_test = 1;
            else if (strncmp(line, "# alpha_blend 1", 15) == 0)
                grp->alpha_blend = 1;
            else if (strncmp(line, "# vcolor 0", 10) == 0)
                grp->use_vertex_color = 0;
            else if (sscanf(line, "# shine %f %f %d", &s, &p, &gate) == 3) {
                grp->shine_strength = s;
                grp->shine_power = p;
                grp->shine_gate = gate;
            }
            else if (sscanf(line, "# alpha_scalar %f", &s) == 1)
                grp->alpha_scalar = s;
            else if (sscanf(line, "# uv_scroll %f %f", &s, &p) == 2) {
                grp->uv_scroll_rate = s;
                grp->uv_scroll_step = p;
            }
            // FUN_0019B1E0's frame-cycle arm: one `# anim_frames` header, then
            // one `# anim_frame <k> <label> <path>` per frame. Every field is
            // straight out of the material record -- see TrackMeshAnim for the
            // recovered algorithm and why <label> is a keyframe TIME.
            else if (sscanf(line, "# anim_frames %d %f %f %d %d %d %d",
                            &ival, &s, &p, &ai, &af, &as, &ap) == 7) {
                if (!grp->anim)
                    grp->anim = (TrackMeshAnim*)calloc(1, sizeof(TrackMeshAnim));
                if (grp->anim) {
                    TrackMeshAnim* a = grp->anim;
                    a->count = ival > TRACKMESH_MAX_FRAMES
                             ? TRACKMESH_MAX_FRAMES : ival;
                    a->period = s;
                    a->hold = p;        // +0x18, the value the file ships
                    a->last = 0.0f;     // +0x1C, zeroed at load
                    a->label = ai;      // +0x12
                    a->index = af;      // +0x10
                    a->step = as;       // +0x13
                    a->pingpong = ap;   // flag bit 0x100
                }
            }
            else if (sscanf(line, "# anim_frame %d %d %255s",
                            &ival, &ai, arg) == 3) {
                if (grp->anim && ival >= 0 && ival < grp->anim->count) {
                    grp->anim->frame_label[ival] = ai;
                    snprintf(grp->anim->frame_texture[ival],
                             sizeof(grp->anim->frame_texture[ival]),
                             "%s%s", dir, arg);
                }
            }
            else if (strncmp(line, "# decal 1", 9) == 0)
                grp->decal = 1;
            else if (strncmp(line, "# alpha 1", 9) == 0)
                grp->alpha = 1;
        }
    }
    tm_free_text(&mtl);
}

// Move every decal group behind the rest of the world, keeping the relative
// order within each half.
//
// THE MECHANISM (see tools/extract_track.py's docstring for the byte-level
// citations). Road markings, arrows and blob shadows are coplanar overlays
// resting a hair above the road -- a median of +0.0114 world units over the
// surface underneath in C1_V1. The game has no depth bias to lean on: the
// NV2A polygon-offset render states 76..81 are written once, to 4/0/0/0/0/0,
// at 0x001BFC07..0x001BFCC4, and never touched again. What keeps the overlays
// on top is purely that they are drawn AFTER the road, under the ambient
// LESSEQUAL depth test (render state 57 := 0x203, 0x001BFAEC), with depth
// WRITES off: the streamed-world material apply FUN_000393C0 computes
// D3DRS_ZWRITEENABLE (render state 64, shadow slot 0x0075D5A0) as
// !(flags & 0x400) at 0x00039AF5..0x00039B1B, and 0x400 is the decal bit.
//
// "After the road" is not the file order. A streamed unit's material table is
// sorted, and a submesh's place in the frame is its material SLOT: the opaque
// slots [0, pvs+0x73) draw in FUN_001AD7A0 and the slots above the cut draw
// later, in the separate transparent pass FUN_001ADD60, after ALL opaque
// geometry of every visible unit. extract_track.py now emits both in that
// order, which fixes the 2392 of 2465 C1_V1 decal triangles that used to be
// emitted before the road they sit on.
//
// What is left over is the cross-unit case: a decal of one streamed unit lying
// on the road of another. The game gets those right because its passes span
// every visible unit at once; the harness bakes the whole track into a single
// display list with no per-cell visibility, so that global consequence is
// applied here instead.
//
// Without it the road ties with the marking on top of it once the depth
// buffer can no longer resolve a hundredth of a unit -- at distance, or under
// this harness's 0.1/5000 near/far -- and, drawn later, wins: the markings
// shutter in and out as the camera creeps forward.
//
// Depth writes are handled per group by the retained renderer's batch key
// (src/burnout3_render.c group_key()), which reads decal here and turns it
// into glDepthMask(GL_FALSE) -- the exact GL spelling of
// D3DRS_ZWRITEENABLE := 0 -- for the batch's whole span.
//
// THE RUNTIME LAW OF THE LAYER: there is none beyond this.               [C]
//
// The layer is the single biggest darkener the harness applies to the road
// (it can cost 1.9x on a patch of Silver Lake asphalt), so it was audited end
// to end for a distance fade, a per-batch alpha ramp or an LOD/range cutoff
// that would let retail draw it fainter or not at all. There is no such
// thing anywhere in the streamed world path:
//
//   * OPACITY IS A PER-MATERIAL CONSTANT. FUN_000393C0's class-6 arm builds
//     combiner factor C0 = (0, 0, 0, material+0x20) on the stack and pushes
//     it once per material:
//        000396b8  XORPS XMM0,XMM0
//        000396c1  MOVSS [ESP+0x40],XMM0     ; C0.r = 0
//        000396c7  MOVSS [ESP+0x44],XMM0     ; C0.g = 0
//        000396cd  MOVSS [ESP+0x48],XMM0     ; C0.b = 0
//        000396d3  MOVSS XMM0,[ESI + 0x20]   ; material +0x20
//        000396d9  MOVSS [ESP+0x4C],XMM0     ; C0.a = alpha scalar
//        000396f3  LEA EAX,[ESP+0x40] / PUSH / JMP -> CALL 0x0034e9a0
//     FUN_0034E9A0 clamps the float4 to [0,1] (`0x003F7BE0` = 1.0) and packs
//     it to a D3DCOLOR with a plain *255 per lane (`0x003F7BF0..FC` = 255.0,
//     four times) into NV097_SET_COMBINER_FACTOR0 (method dword 0x00040A60).
//     Nothing between that upload and the draw touches it. So 0.6 in the
//     file is 153/255 on screen, at every distance, for every batch.
//   * THE VERTEX PROGRAM HAS NO FADE. Classes 0/3/6/10 share the 9-instruction
//     program at 0x003E8828: UV scroll, `MOV oD0, v3`, the four oPos DP4s,
//     `MIN oFog, r12.z, c[120].z` and the screen-space epilogue. No view
//     vector, no length, no ramp -- and class 6's output alpha is
//     `T0.a * C0.a`, so oD0.w never reaches the blend at all.
//   * THE ONLY PER-SUBMESH GATE IS A FRUSTUM TEST. FUN_001AD510 walks a
//     material slot's submesh chain (`pvs+0x12D + unitslot*0xA9 + slot`, next
//     at submesh+0x8C, stride 0x90) and draws each link if the caller's
//     "whole unit inside" flag is set OR `FUN_001B23F0(&DAT_004D67F0, submesh)`
//     is non-zero. FUN_001B23F0 is a plain bounding-box-vs-plane-set test: it
//     dots the submesh's eight corner float4s (submesh+0x00..0x7F, right in
//     front of the fmt/count/ptr triple at +0x80/+0x84/+0x88) against two
//     four-plane sets with MOVMSKPS and returns 0 out / 1 straddling / 2
//     inside. Visible is visible; there is no distance term in it.
//   * THE UNIT LOD IS A GEOMETRY SWAP, NOT A DECAL SWITCH. FUN_0019D100 takes
//     the chunk's signed unit offset and at 0x0019D23B..0x0019D249 computes
//     |offset| and compares it with 4: units within +/-4 of the camera's cell
//     draw the blockA model, units 5..8 away draw the blockB one (the
//     lower-poly LOD block; extract_track.py extracts blockA only). The near
//     road -- everything these sheets lie on -- is always blockA.
//
// So retail lays the sheets down wherever they are, at full strength, out to
// the edge of the frustum, exactly as this loader does. The apparent "band
// our render has and retail's frame does not" on the Silver Lake pair is a
// FRAME-MATCH artefact: only 5.1% of the track's road area carries a
// Tree_shadow1 sheet (9.9% carries any decal at all), our f1100 happens to sit
// inside one and the retail capture does not. Frames 1120..1250 of the same
// stretch measure 58..62 on the same box against f1100's 30.
static void trackmesh_decals_last(TrackMesh* m) {
    if (m->group_count <= 1 || getenv("B3_TRACK_NODECALSORT")) return;

    int ndecal = 0, covered = 0;
    for (int g = 0; g < m->group_count; g++) {
        ndecal += m->groups[g].decal ? 1 : 0;
        covered += m->groups[g].triangle_count;
    }
    // Nothing to do, or the groups do not tile the index array (then moving
    // them would drop the triangles no group owns).
    if (ndecal == 0 || ndecal == m->group_count || covered != m->triangle_count)
        return;
    // Already sorted? Leave the arrays alone.
    {
        int seen_decal = 0, sorted = 1;
        for (int g = 0; g < m->group_count; g++) {
            if (m->groups[g].decal) seen_decal = 1;
            else if (seen_decal) { sorted = 0; break; }
        }
        if (sorted) return;
    }

    unsigned* idx = malloc((size_t)m->triangle_count * 3 * sizeof(unsigned));
    TrackMeshGroup* grp = malloc((size_t)m->group_count * sizeof(TrackMeshGroup));
    if (!idx || !grp) { free(idx); free(grp); return; }

    int t = 0, n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int g = 0; g < m->group_count; g++) {
            const TrackMeshGroup* s = &m->groups[g];
            if ((s->decal != 0) != pass) continue;
            memcpy(idx + (size_t)t * 3,
                   m->indices + (size_t)s->first_triangle * 3,
                   (size_t)s->triangle_count * 3 * sizeof(unsigned));
            grp[n] = *s;
            grp[n].first_triangle = t;
            n++;
            t += s->triangle_count;
        }
    }
    free(m->indices);
    m->indices = idx;
    memcpy(m->groups, grp, (size_t)m->group_count * sizeof(TrackMeshGroup));
    free(grp);
}

// The game's world geometry is SINGLE-SIDED: it draws with backface culling
// on and only turns it off per material, for the fences/foliage/signs/clutter
// that carry material flag +0x24 bit 0x20 (D3DRS_CULLMODE := D3DCULL_NONE --
// FUN_0003A3C0 @0x0003A674; see tools/extract_track.py for the full citation).
// Drawing that data double-sided shows the BACK of walls, facades and
// backdrops: the Bangkok underpass at route progress ~0.48 was sealed off by
// the reverse side of a bk_downtown_bd facade standing across the road.
//
// So culling has to be on. Front faces are CLOCKWISE here, for two reasons
// that agree: trackmesh_load mirrors the world (negate Z, below), which
// inverts triangle facing; and the harness's own screen-space geometry (HUD
// quads) is wound the same way. GL defaults to GL_CCW, hence the glFrontFace.
// The two-sided materials need no special case -- extract_track.py already
// emitted a reverse-wound duplicate of every one of their triangles.
//
// Everything the harness draws is wound that way -- track, cars, traffic and
// HUD all verified on screen. The only geometry that is not is the untextured
// box/quad *fallbacks* in burnout3_full.c, which only appear when a car or
// track mesh failed to load; a face of those may now be culled.
// B3_TRACK_NOCULL=1 restores the old double-sided behaviour if some other
// draw path turns out to need it.
static void trackmesh_gl_single_sided(void) {
    static int done = 0;
    if (done) return;
    if (getenv("B3_TRACK_NOCULL")) { done = 1; return; }
    if (!SDL_GL_GetCurrentContext()) return;   // no context yet (or headless)
    done = 1;
    glFrontFace(GL_CW);
    glEnable(GL_CULL_FACE);
}

int trackmesh_load(TrackMesh* m, const char* path) {
    TMText obj;
    char*  line;
    long   pump_countdown = 0;
    if (tm_slurp(&obj, path) != 0) return -1;

    memset(m, 0, sizeof(*m));
    int vcap = 1 << 14, tcap = 1 << 14;
    m->positions = malloc((size_t)vcap * 3 * sizeof(float));
    m->uvs = malloc((size_t)vcap * 2 * sizeof(float));
    m->colors = malloc((size_t)vcap * 4 * sizeof(float));
    m->normals = malloc((size_t)vcap * 3 * sizeof(float));
    m->indices = malloc((size_t)tcap * 3 * sizeof(unsigned));
    if (!m->positions || !m->uvs || !m->colors || !m->normals || !m->indices) {
        tm_free_text(&obj); trackmesh_free(m); return -1;
    }

    // Per-corner vt/vn indices, kept only so the expansion pass below can
    // rebuild the mesh if the OBJ does not use one index per vertex.
    unsigned* corner_t = malloc((size_t)tcap * 3 * sizeof(unsigned));
    unsigned* corner_n = malloc((size_t)tcap * 3 * sizeof(unsigned));
    int split_channels = 0;

    int nuv = 0, nnrm = 0, have_color = 0, have_normal = 0;
    char mtl_name[256] = "";
    while ((line = tm_line(&obj)) != NULL) {
        /* One pump every 8k lines: often enough that the biggest track's parse
         * still draws at the loading screen's own 30 Hz ceiling, rare enough
         * that the test costs nothing.  The fraction is byte progress through
         * the file, which is what the bar wants. */
        if (g_pump_fn && --pump_countdown <= 0) {
            pump_countdown = 8192;
            g_pump_fn(obj.len ? (float)obj.pos / (float)obj.len : 1.0f,
                      g_pump_user);
        }
        if (line[0] == 'v' && line[1] == ' ') {
            if (m->vertex_count >= vcap) {
                vcap *= 2;
                m->positions = realloc(m->positions, (size_t)vcap * 3 * sizeof(float));
                m->uvs = realloc(m->uvs, (size_t)vcap * 2 * sizeof(float));
                m->colors = realloc(m->colors, (size_t)vcap * 4 * sizeof(float));
                m->normals = realloc(m->normals, (size_t)vcap * 3 * sizeof(float));
                if (!m->positions || !m->uvs || !m->colors || !m->normals) {
                    tm_free_text(&obj); free(corner_t); free(corner_n);
                    trackmesh_free(m); return -1;
                }
            }
            float* p = m->positions + (size_t)m->vertex_count * 3;
            float* c = m->colors + (size_t)m->vertex_count * 4;
            // "v x y z [r g b]" -- the optional trailing triple is the game's
            // D3DCOLOR diffuse at vertex +0x10 (the widely used OBJ vertex
            // colour extension). It is HALF RANGE: 128/255 is white, because
            // every world pixel shader doubles it (SHIFTLEFT_1 on the stage-0
            // RGB output, 0x000100C0 in all six D3DPIXELSHADERDEFs). Double it
            // here, once, so the renderer can hand it straight to glColor.
            float r = 0.5f, g = 0.5f, b = 0.5f;
            int n = sscanf(line + 2, "%f %f %f %f %f %f", p, p + 1, p + 2,
                           &r, &g, &b);
            if (n >= 3) {
                // The game's data is D3D left-handed; GL is right-handed.
                // Rendering LH data through a RH camera mirrors the image
                // (billboard text read backwards). One uniform reflection
                // (negate Z) applied to ALL world data -- this loader serves
                // both track and car meshes, and burnout3_full.c applies the
                // same flip to the path/collision arrays -- makes the GL
                // output match the original game's exactly.
                p[2] = -p[2];
                if (n >= 6) have_color = 1;
                c[0] = r * 2.0f > 1.0f ? 1.0f : r * 2.0f;
                c[1] = g * 2.0f > 1.0f ? 1.0f : g * 2.0f;
                c[2] = b * 2.0f > 1.0f ? 1.0f : b * 2.0f;
                c[3] = 1.0f;                 // set from the vt line below
                m->vertex_count++;
            }
        } else if (line[0] == 'v' && line[1] == 't') {
            if (nuv < vcap) {
                float* t = m->uvs + (size_t)nuv * 2;
                // "vt u v [a]" -- the third component carries the vertex
                // ALPHA, which is the class-1 specular gate (0 or 1).
                float a = 1.0f;
                if (sscanf(line + 3, "%f %f %f", t, t + 1, &a) >= 2) {
                    if (nuv < m->vertex_count) m->colors[(size_t)nuv * 4 + 3] = a;
                    nuv++;
                }
            }
        } else if (line[0] == 'v' && line[1] == 'n') {
            if (nnrm < vcap) {
                float* nv = m->normals + (size_t)nnrm * 3;
                if (sscanf(line + 3, "%f %f %f", nv, nv + 1, nv + 2) == 3) {
                    nv[2] = -nv[2];   // same reflection as the positions
                    nnrm++;
                    have_normal = 1;
                }
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            unsigned a, b, c;
            unsigned at = 0, bt = 0, ct = 0, an = 0, bn = 0, cn = 0;
            // Emitted as "f v/vt v/vt v/vt" (track) or "f v/vt/vn ..." (cars);
            // accept bare "f v v v" too.
            //
            // TrackMesh is a FLAT vertex model: positions, uvs, colours and
            // normals are four parallel arrays that `indices` indexes with a
            // single number. That only holds if a face's vt and vn indices
            // equal its v index -- otherwise a corner's normal would be read
            // from the wrong vertex, and on a car body (where the SH specular
            // is evaluated along reflect(-V,N)) that shows up as chaotic
            // marbled streaking rather than a smooth sheen.
            //
            // Both producers do satisfy it: tools/extract_track.py and
            // tools/extract_bgv.py write one v, one vt and one vn per vertex
            // in the same order and emit "f a/a/a b/b/b c/c/c". So the fast
            // path below is exact for the data this harness ships. But that
            // is an invariant of the WRITERS, not of the format, so it is
            // CHECKED rather than assumed: any face whose vt or vn index
            // differs from its v index sets `split_channels`, and the
            // expansion pass after the read loop then rebuilds the mesh with
            // one vertex per distinct (v, vt, vn) triple. A general OBJ
            // therefore loads correctly instead of silently scrambling.
            if (sscanf(line + 2, "%u/%u/%u %u/%u/%u %u/%u/%u",
                       &a, &at, &an, &b, &bt, &bn, &c, &ct, &cn) == 9) {
                if (at != a || bt != b || ct != c ||
                    an != a || bn != b || cn != c)
                    split_channels = 1;
            } else if (sscanf(line + 2, "%u/%u %u/%u %u/%u",
                              &a, &at, &b, &bt, &c, &ct) == 6) {
                an = a; bn = b; cn = c;
                if (at != a || bt != b || ct != c) split_channels = 1;
            } else if (sscanf(line + 2, "%u//%u %u//%u %u//%u",
                              &a, &an, &b, &bn, &c, &cn) == 6) {
                at = a; bt = b; ct = c;
                if (an != a || bn != b || cn != c) split_channels = 1;
            } else if (sscanf(line + 2, "%u %u %u", &a, &b, &c) == 3) {
                at = a; bt = b; ct = c;
                an = a; bn = b; cn = c;
            } else {
                continue;
            }
            if (m->triangle_count >= tcap) {
                tcap *= 2;
                m->indices = realloc(m->indices, (size_t)tcap * 3 * sizeof(unsigned));
                if (!m->indices) {
                    tm_free_text(&obj); free(corner_t); free(corner_n);
                    trackmesh_free(m); return -1;
                }
                if (corner_t && corner_n) {
                    unsigned* rt = realloc(corner_t, (size_t)tcap * 3 * sizeof(unsigned));
                    unsigned* rn = realloc(corner_n, (size_t)tcap * 3 * sizeof(unsigned));
                    if (rt) corner_t = rt;
                    if (rn) corner_n = rn;
                    // Out of memory for the side channels only: drop the
                    // expansion capability, never the mesh.
                    if (!rt || !rn) {
                        free(corner_t); free(corner_n);
                        corner_t = corner_n = NULL;
                        split_channels = 0;
                    }
                }
            }
            unsigned* i = m->indices + (size_t)m->triangle_count * 3;
            i[0] = a - 1; i[1] = b - 1; i[2] = c - 1;   // OBJ is 1-based
            if (corner_t && corner_n) {
                unsigned* it = corner_t + (size_t)m->triangle_count * 3;
                unsigned* in = corner_n + (size_t)m->triangle_count * 3;
                it[0] = at - 1; it[1] = bt - 1; it[2] = ct - 1;
                in[0] = an - 1; in[1] = bn - 1; in[2] = cn - 1;
            }
            m->triangle_count++;
        } else if (strncmp(line, "usemtl ", 7) == 0) {
            char name[TRACKMESH_NAME_LEN];
            if (sscanf(line + 7, "%63s", name) != 1) continue;
            // Extend the current group if the material repeats back-to-back.
            if (m->group_count > 0 &&
                strcmp(m->groups[m->group_count - 1].material, name) == 0 &&
                m->groups[m->group_count - 1].first_triangle +
                m->groups[m->group_count - 1].triangle_count == m->triangle_count)
                continue;
            if (m->group_count < TRACKMESH_MAX_GROUPS) {
                TrackMeshGroup* g = &m->groups[m->group_count++];
                memset(g, 0, sizeof(*g));
                /* The sscanf above caps `name` at 63 chars, so this never
                 * truncates in practice; copy an explicit length and
                 * terminate rather than lean on strncpy's zero fill. */
                size_t nlen = strlen(name);
                if (nlen > sizeof(g->material) - 1) nlen = sizeof(g->material) - 1;
                memcpy(g->material, name, nlen);
                g->material[nlen] = '\0';
                g->first_triangle = m->triangle_count;
                g->use_vertex_color = 1;   // cleared by the MTL's "# vcolor 0"
                g->alpha_scalar = 1.0f;    // set by the MTL's "# alpha_scalar"
            }
        } else if (strncmp(line, "mtllib ", 7) == 0) {
            sscanf(line + 7, "%255s", mtl_name);
        }

        // Keep the open group's span current as faces stream in.
        if (m->group_count > 0) {
            TrackMeshGroup* g = &m->groups[m->group_count - 1];
            g->triangle_count = m->triangle_count - g->first_triangle;
        }
    }
    tm_free_text(&obj);

    if (m->vertex_count == 0 || m->triangle_count == 0) {
        free(corner_t); free(corner_n); trackmesh_free(m); return -1;
    }

    // ---- channel expansion -------------------------------------------------
    // Only runs for an OBJ whose faces index vt/vn separately from v (see the
    // face parser). Neither of this repo's extractors produces one, so on the
    // shipped data this whole block is skipped and the mesh is bit-identical
    // to what the loader has always produced. It exists so that "the normals
    // are the wrong normals" can never be a silent failure mode again.
    if (split_channels && corner_t && corner_n) {
        int ncorner = m->triangle_count * 3;
        int cap = 16;
        while (cap < ncorner * 2) cap <<= 1;
        int* hslot = malloc((size_t)cap * sizeof(int));
        unsigned* key_v = malloc((size_t)ncorner * sizeof(unsigned));
        unsigned* key_t = malloc((size_t)ncorner * sizeof(unsigned));
        unsigned* key_n = malloc((size_t)ncorner * sizeof(unsigned));
        float* np = malloc((size_t)ncorner * 3 * sizeof(float));
        float* nt = malloc((size_t)ncorner * 2 * sizeof(float));
        float* nc = malloc((size_t)ncorner * 4 * sizeof(float));
        float* nn = malloc((size_t)ncorner * 3 * sizeof(float));
        if (hslot && key_v && key_t && key_n && np && nt && nc && nn) {
            for (int i = 0; i < cap; i++) hslot[i] = -1;
            int nvert = 0;
            for (int ci = 0; ci < ncorner; ci++) {
                unsigned v = m->indices[ci], tt = corner_t[ci], nnn = corner_n[ci];
                // A face referencing a vertex we never loaded is dropped, not
                // clamped: mark the corner out of range and let the existing
                // compaction pass below delete the triangle and fix the group
                // spans, exactly as it does on the non-expanded path.
                if (v >= (unsigned)m->vertex_count) {
                    m->indices[ci] = (unsigned)-1;
                    continue;
                }
                if (tt >= (unsigned)nuv) tt = v;
                if (nnn >= (unsigned)nnrm) nnn = v;
                unsigned h = (v * 73856093u) ^ (tt * 19349663u) ^ (nnn * 83492791u);
                h &= (unsigned)(cap - 1);
                int found = -1;
                while (hslot[h] >= 0) {
                    int k = hslot[h];
                    if (key_v[k] == v && key_t[k] == tt && key_n[k] == nnn) {
                        found = k; break;
                    }
                    h = (h + 1u) & (unsigned)(cap - 1);
                }
                if (found < 0) {
                    found = nvert++;
                    hslot[h] = found;
                    key_v[found] = v; key_t[found] = tt; key_n[found] = nnn;
                    memcpy(np + (size_t)found * 3,
                           m->positions + (size_t)v * 3, 3 * sizeof(float));
                    if (m->uvs && nuv > 0)
                        memcpy(nt + (size_t)found * 2,
                               m->uvs + (size_t)tt * 2, 2 * sizeof(float));
                    else
                        nt[found * 2] = nt[found * 2 + 1] = 0.0f;
                    // rgb travels with the position, alpha with the vt (the
                    // reader stuffs the vt line's third component there)
                    nc[found * 4 + 0] = m->colors[(size_t)v * 4 + 0];
                    nc[found * 4 + 1] = m->colors[(size_t)v * 4 + 1];
                    nc[found * 4 + 2] = m->colors[(size_t)v * 4 + 2];
                    nc[found * 4 + 3] = m->colors[(size_t)tt * 4 + 3];
                    if (nnrm > 0)
                        memcpy(nn + (size_t)found * 3,
                               m->normals + (size_t)nnn * 3, 3 * sizeof(float));
                    else {
                        nn[found * 3] = 0.0f; nn[found * 3 + 1] = 1.0f;
                        nn[found * 3 + 2] = 0.0f;
                    }
                }
                m->indices[ci] = (unsigned)found;
            }
            free(m->positions); free(m->uvs); free(m->colors); free(m->normals);
            m->positions = np; m->uvs = nt; m->colors = nc; m->normals = nn;
            m->vertex_count = nvert;
            nuv = nnrm = nvert;
            np = nt = nc = nn = NULL;
        }
        free(hslot); free(key_v); free(key_t); free(key_n);
        free(np); free(nt); free(nc); free(nn);
    }
    free(corner_t); free(corner_n);
    corner_t = corner_n = NULL;

    // An OBJ written before the vertex-colour/normal channels existed (or a
    // car mesh, which this loader also serves) leaves these unset -- drop them
    // so callers can test for NULL rather than get a buffer of 0.5 grey.
    if (!have_color || nuv < m->vertex_count) { free(m->colors); m->colors = NULL; }
    if (!have_normal || nnrm < m->vertex_count) { free(m->normals); m->normals = NULL; }

    // B3_TRACK_NOVCOLOR=1: keep the alpha (the shine gate) but force the
    // colour to white, i.e. go back to the pre-recovery flat look. The baked
    // vertex lighting is dark -- the mean over every world vertex is 0.21 of
    // full on US_C3_V1 and 0.14 on AS_C1_V1, and retail makes that up with
    // the scene light record and the fog blend in the final combiner, neither
    // of which this harness reproduces yet. This is the escape hatch for
    // judging that gap by eye.
    if (m->colors && getenv("B3_TRACK_NOVCOLOR")) {
        for (int v = 0; v < m->vertex_count; v++) {
            m->colors[(size_t)v * 4 + 0] = 1.0f;
            m->colors[(size_t)v * 4 + 1] = 1.0f;
            m->colors[(size_t)v * 4 + 2] = 1.0f;
        }
    }

    // Drop any face referencing a vertex we did not load, so the renderer can
    // never index out of bounds. Group spans must shift with the compaction.
    {
        int kept = 0, gi = 0;
        int gstart[TRACKMESH_MAX_GROUPS] = {0}, gcount[TRACKMESH_MAX_GROUPS] = {0};
        for (int t = 0; t < m->triangle_count; t++) {
            while (gi < m->group_count &&
                   t >= m->groups[gi].first_triangle + m->groups[gi].triangle_count)
                gi++;
            unsigned* i = m->indices + (size_t)t * 3;
            if (i[0] < (unsigned)m->vertex_count && i[1] < (unsigned)m->vertex_count &&
                i[2] < (unsigned)m->vertex_count) {
                unsigned* d = m->indices + (size_t)kept * 3;
                d[0] = i[0]; d[1] = i[1]; d[2] = i[2];
                if (gi < m->group_count) {
                    if (gcount[gi] == 0) gstart[gi] = kept;
                    gcount[gi]++;
                }
                kept++;
            }
        }
        m->triangle_count = kept;
        for (int g = 0; g < m->group_count; g++) {
            m->groups[g].first_triangle = gstart[g];
            m->groups[g].triangle_count = gcount[g];
        }
    }

    trackmesh_gl_single_sided();
    resolve_materials(m, path, mtl_name);
    trackmesh_decals_last(m);

    // Prime the animated-material state so a renderer that never calls
    // trackmesh_tick() still draws the boards at phase 0 / frame 0, which is
    // what the loader FUN_0019AE10 case 0x17 zeroes them to.
    m->anim_time = 0.0f;
    m->anim_groups = 0;
    for (int g = 0; g < m->group_count; g++) {
        m->groups[g].uv_offset[0] = 0.0f;
        m->groups[g].uv_offset[1] = 0.0f;
        if (trackmesh_group_animated(m, g)) m->anim_groups++;
    }

    for (int k = 0; k < 3; k++) { m->min[k] = 1e30f; m->max[k] = -1e30f; }
    for (int v = 0; v < m->vertex_count; v++) {
        const float* p = m->positions + (size_t)v * 3;
        for (int k = 0; k < 3; k++) {
            if (p[k] < m->min[k]) m->min[k] = p[k];
            if (p[k] > m->max[k]) m->max[k] = p[k];
        }
    }
    return 0;
}

void trackmesh_free(TrackMesh* m) {
    free(m->positions); free(m->uvs); free(m->colors); free(m->normals);
    free(m->indices);
    for (int g = 0; g < m->group_count; g++) free(m->groups[g].anim);
    memset(m, 0, sizeof(*m));
}

void trackmesh_set_group_texture(TrackMesh* m, int group, unsigned gl_texture) {
    if (group >= 0 && group < m->group_count)
        m->groups[group].gl_texture = gl_texture;
}

// The GL spelling of the render state the streamed material apply
// FUN_000393C0 queues for one material. Every branch below is a render state
// that function writes into the deferred shadow at 0x0075D4A0 + id*4:
//
//   flags & 0x001 -> RS 0x3B D3DRS_ALPHABLENDENABLE   @0x00039B21
//   flags & 0x010 -> RS 0x3C D3DRS_ALPHATESTENABLE    @0x00039BD4
//   flags & 0x400 -> RS 0x40 D3DRS_ZWRITEENABLE := 0  @0x00039AF5
//
// with the blend factors and the alpha test set once, globally:
//   RS 62/63/74 SRCBLEND/DESTBLEND/BLENDOP := 0x302/0x303/0x8006
//               (0x00038B06/0x00038B2F/0x00038B58 -- the NV2A blend tokens
//               ARE the GL enums, so 0x302/0x303 read straight across as
//               GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA and 0x8006 as
//               GL_FUNC_ADD)
//   RS 0x3A/0x3D ALPHAFUNC/ALPHAREF := D3DCMP_GREATER / 0x40, by the world
//               setup FUN_00038D10 (0x0003901B / 0x00038FEE)
//
// THE ALPHA BITS WERE SWAPPED IN THIS HARNESS until 2026-08-12, which is why
// the tree-shadow sheets (flags 0x040F: blend, no test) were being alpha
// TESTED and drawn opaque. Render state 0x3B is ALPHABLENDENABLE and 0x3C is
// ALPHATESTENABLE, pinned by the coherent triple FUN_00038D10 writes around
// them -- RS 0x3A := 0x204 (D3DCMP_GREATER on the 0x200 base that RS 57/ZFUNC
// already established), RS 0x3C := 0, RS 0x3D := 0x40 -- which only parses as
// ALPHAFUNC / ALPHATESTENABLE / ALPHAREF, in the same order the already-pinned
// 57 ZFUNC, 62 SRCBLEND, 63 DESTBLEND, 64 ZWRITEENABLE and 67
// COLORWRITEENABLE run in.
//
// `cutout_fallback` is the caller's texel-statistics guess. It is used only
// for a group with NO material record at all (an OBJ written before the MTL
// carried the flag word, or a submesh whose material index was out of range):
// with a record, the flags are the game's own answer and there is nothing to
// guess. The presence test is TrackMeshGroup.have_material and NOT
// `(flags || cls)` -- see the header comment on have_material for the tunnel
// walls that the latter erased.
//
// COPLANAR DRAW PRIORITY: THERE IS NONE TO RECOVER, AND NONE IS NEEDED.   [C]
//
// The big roadside billboard of US_C3_V1 (Chgo_BigAdverts_02, the poster, and
// Chgo_BigAdverts_Backs, its frame / backing panel / catwalk -- dump 028) was
// reported as flickering, on the theory that two coplanar surfaces were being
// drawn in an unstable order. Both materials carry flag word 0x0032, which
// FUN_000393C0 decodes as:
//
//   bit 0x001 CLEAR -> RS 0x3B D3DRS_ALPHABLENDENABLE := 0     @0x00039B21
//   bit 0x010 SET   -> RS 0x3C D3DRS_ALPHATESTENABLE  := 1     @0x00039BD4
//   bit 0x020 SET   -> RS 0x93 D3DRS_CULLMODE := 0, TWO-SIDED  @0x00039C86
//   bit 0x400 CLEAR -> RS 0x40 D3DRS_ZWRITEENABLE     := 1     @0x00039AF5
//   bit 0x002 SET   -> 4 into the texture-stage slot 0x0075D7F0 under
//                      deferred token 0x0B @0x00039C80. A stage slot, NOT an
//                      ordering field; its purpose is still [?].
//
// So the pair is OPAQUE, alpha-TESTED, depth-WRITING cut-out geometry. It is
// not blended at all -- 0x010 is the alpha TEST bit and the BLEND bit 0x001 is
// clear. Opaque surfaces under the ambient LESSEQUAL depth test compose
// order-independently, which is exactly why the material record carries no
// sort key and the world draw loop applies no sort: the streamed renderer's
// ONLY draw-priority mechanism is the 0x400 decal bit (ZWRITE off, drawn in
// the later transparent-slot pass FUN_001ADD60), and neither material has it.
//
// The geometry agrees. The poster quad stands 0.6040 world units IN FRONT of
// the 113.99 u^2 backing panel, and the Backs triangles that ARE exactly
// coplanar with the poster (|sep| <= 1e-4) are the board's border frame, which
// ABUTS the poster without covering it: clipping every cross-material pair
// against the other in their shared plane returns 0.0 u^2 of true overlap.
// Measured over frames 4169..4171, 0-1 of ~12,000 poster pixels ever resolve
// to the Backs texture rather than the poster's.
//
// This harness could not reorder them in any case: the world is baked once
// into static vertex buffers whose batch order is decided at load
// (b3r_track_build, src/burnout3_render.c), so group order is fixed for the
// whole run and cannot vary with the camera.
//
// The residual frame-to-frame churn on the board is TEXTURE ALIASING, not
// depth. Rendered ALONE through B3_TRACK_ONLYMAT below -- with nothing left in
// the world to contend with it for depth or order -- the poster face still
// shows 6.91% strongly-reversing pixels, against 9.71% in the full scene.
//
// => No polygon offset and no extra sort belong here. Do not re-chase this.
void trackmesh_group_material(const TrackMesh* m, int group,
                              unsigned gl_texture, int cutout_fallback,
                              int* out_decal, int* out_blend, int* out_test) {
    int decal = 0, blend = 0, test = 0;
    if (group >= 0 && group < m->group_count) {
        const TrackMeshGroup* g = &m->groups[group];
        decal = g->decal;
        if (g->have_material) {
            blend = g->alpha_blend;
            test = g->alpha_test;
        } else {
            test = cutout_fallback;
        }
    } else {
        test = cutout_fallback;
    }
    if (!gl_texture) { blend = 0; test = 0; }

    // B3_TRACK_ONLYMAT=<substr>: draw ONLY the groups whose material name
    // contains <substr> and discard every other one. Diagnostic; it works
    // inside the baked display list because "discard everything" is a render
    // STATE here -- GL_GREATER against an alpha reference of 1.0, which no
    // fragment can pass -- rather than a skipped draw call. Used to answer
    // "do these two materials actually overlap in screen space?" by rendering
    // each of them alone from the same camera.
    static const char* onlymat = (const char*)1;
    if (onlymat == (const char*)1) onlymat = getenv("B3_TRACK_ONLYMAT");
    if (onlymat && group >= 0 && group < m->group_count
        && !strstr(m->groups[group].material, onlymat)) {
        /* "discard everything" as a STATE rather than a skipped draw, so it
         * works uniformly for every group: GREATER against a reference of 1.0,
         * which no fragment can pass, and depth writes off. */
        *out_decal = 1;
        *out_blend = 0;
        *out_test  = -1;          /* the caller's "reference 1.0" sentinel */
        return;
    }

    *out_decal = decal;
    *out_blend = blend;
    *out_test  = test;
}

void trackmesh_group_vertex_color(const TrackMesh* m, int group, unsigned v,
                                  float out[4]) {
    const TrackMeshGroup* g = (group >= 0 && group < m->group_count)
                            ? &m->groups[group] : NULL;
    // B3_TRACK_CLASSVIZ=1: replace the diffuse with a per-shader-class hue so
    // a screenshot says which material class covers which part of the frame.
    // Diagnostic only; the env lookup is cached because this runs once per
    // vertex of the whole track while the display list is built.
    static int classviz = -1;
    if (classviz < 0) classviz = getenv("B3_TRACK_CLASSVIZ") != NULL;
    if (classviz) {
        static const float pal[11][3] = {
            {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1},
            {1,.5f,0}, {.5f,0,1}, {0,.5f,.5f}, {.5f,.5f,.5f}, {1,1,1}
        };
        int c = g ? g->cls : 9;
        if (c < 0 || c > 10) c = 9;
        out[0] = pal[c][0]; out[1] = pal[c][1]; out[2] = pal[c][2];
        out[3] = 1.0f;
        return;
    }
    // rgb: the baked diffuse, already doubled by the loader (the world pixel
    // shaders carry SHIFTLEFT_1 on their stage-0 RGB output). Classes 8 and 9
    // have no D3DCOLOR register at all, so the game never reads it for them.
    if (m->colors && (!g || g->use_vertex_color) && v < (unsigned)m->vertex_count) {
        const float* c = m->colors + (size_t)v * 4;
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }
    // WORLD HEADROOM CURVE -- TUNED (user-authorized deviation 2026-08-13).
    //
    // The world path above is the recovered one and it is complete: 2*T*V,
    // the per-class specular/emissive adds, the fog blend, and (since the
    // PRESENT wave) the recovered present composite's x2 and the ^0.95 output
    // ramp. This is the one magnitude knob on it, applied at the single choke
    // point every world vertex colour passes through -- the display-list build
    // and the scroll pass both call this function.
    //
    // Shape: out = (1 - e^-G c) / (1 - e^-G), a one-parameter family that is
    // monotonic for every G, fixes 0 -> 0 and 1 -> 1, and is the identity at
    // G = 0. G > 0 LIFTS the shadows (what was needed before the present
    // composite existed, when the world sat ~1.6x under the references);
    // G < 0 COMPRESSES them (headroom, if the composite's x2 overshoots).
    // Because both ends are pinned, nothing clips and nothing inverts, the
    // white-vertex population (128 = white in the source data) is untouched,
    // and no per-track data is needed.
    //
    // Judged by eye against the xemu references WITH the present composite and
    // the gamma ramp in place -- which is the only meaningful comparison, the
    // references being final frames. B3_TRACK_LIFT=<G> overrides at runtime.
    {
        static float lg = 1e9f, lnorm = 1.0f;
        if (lg > 1e8f) {
            const char* e = getenv("B3_TRACK_LIFT");
            lg = e ? (float)atof(e) : TRACKMESH_LIFT_G;
            lnorm = (lg < -1e-4f || lg > 1e-4f)
                  ? 1.0f / (1.0f - expf(-lg)) : 0.0f;
        }
        if (lg < -1e-4f || lg > 1e-4f) {
            for (int i = 0; i < 3; i++)
                out[i] = (1.0f - expf(-lg * out[i])) * lnorm;
        }
    }
    // B3_TRACK_NODECAL=1: force the blended decal/shadow layer fully
    // transparent, to measure how much of the road's darkness is the shadow
    // sheets lying on it rather than the road's own baked colour. Diagnostic.
    static int nodecal = -1;
    if (nodecal < 0) nodecal = getenv("B3_TRACK_NODECAL") != NULL;
    if (nodecal && g && (g->decal || g->alpha_blend)) {
        out[3] = 0.0f;
        return;
    }
    // B3_TRACK_DECALONLY=<substr>: the same switch, inverted and narrowed --
    // keep only the decal/blended groups whose material name contains
    // <substr>, making every other one transparent, so a screenshot
    // attributes a dark band to one named sheet. Diagnostic.
    static const char* only = (const char*)1;
    if (only == (const char*)1) only = getenv("B3_TRACK_DECALONLY");
    if (only && g && (g->decal || g->alpha_blend) && !strstr(g->material, only)) {
        out[3] = 0.0f;
        return;
    }
    // w: material +0x20. Under GL_MODULATE the fragment alpha is
    // texture.a * primary.a, which is exactly the class-6 output alpha
    // tex.a * C0.a -- and C0.a IS material +0x20 (FUN_000393C0 loads it at
    // 0x0003945D/0x0003951E/... and FUN_0034E9A0 packs the float4 to a
    // D3DCOLOR with a plain *255, so 0.6 in the file means 0.6 on screen).
    out[3] = g ? g->alpha_scalar : 1.0f;
}

// The animated-material ticker FUN_0019B1E0's scroll branch, verbatim.
//
//   T      = the global clock DAT_0060EA20, in seconds (read @0x0019B1E8)
//   target = rate(+0x14) * T
//   next   = step(+0x18) + phase(+0x1C)
//   if (next <= target) phase = (step == 0) ? target : next
//
// at most one step per call, and the branch only runs for materials in the
// header+0x10 animated list whose frame count (+0x11) is < 2 and whose flag
// bit 0x200 is set (tested as byte +0x25 & 2). The material apply then pushes
// (1 - frac(phase), 0, 0) into vertex-shader constant 0x63 (0x00039CE0
// streamed / 0x0003A6C8 static) and every world vertex program starts with
// `add oT0.xy, v9.xy, c[99].xy` -- so U scrolls and V never moves.
// THE FRAME ARM is the other half of the same `if`, and it is the one the port
// was missing: `Arrows` was the only material in US_C3_V1 that takes the
// scroll arm, so tagging scrolls alone left every bulb-matrix message board in
// the game -- the SLOW/DOWN accident boards, the warning signs, the flags,
// even the water -- standing on frame 0 forever. See TrackMeshAnim for the
// decompile and for why the frame durations come out of the texture names.
static void trackmesh_tick_frames(TrackMeshAnim* a, float T) {
    if (a->count < 2) return;
    // `if (hold(+0x18) + last(+0x1C) < T)`, one swap per call at most.
    if (!(a->hold + a->last < T)) return;
    a->last = T;
    int nxt;
    if (a->pingpong) {
        // flags & 0x100. No shipped material sets it; kept for fidelity.
        int st = a->step;
        int f = a->index + st;
        a->index = f;
        if (st == 1 && a->count - 1 <= f) a->step = -1;
        else if (st == -1 && f < 1) { a->step = 1; a->label = 1; }
        if (a->index < 0) a->index = 0;
        if (a->index >= a->count) a->index = a->count - 1;
        nxt = (a->index + 1) % a->count;
        if (a->step == -1) {
            // Running backwards the arm re-reads the CURRENT frame's label and
            // subtracts, so the same keyframe spacing plays in reverse.
            if (nxt == 0) { a->hold = a->period; }
            else {
                int L = a->frame_label[a->index];
                a->hold = (float)(a->label - L) * a->period;
                a->label = L;
            }
            return;
        }
    } else {
        a->index = (a->index + 1) % a->count;
        nxt = (a->index + 1) % a->count;
        if (nxt == 0) {
            // Wrapping back to frame 0: the arm does NOT parse frame 0's name
            // (it has no digits), it hard-codes one period and label 1.
            a->hold = a->period;
            a->label = 1;
            return;
        }
    }
    {
        int L = a->frame_label[nxt];
        a->hold = (float)(L - a->label) * a->period;
        a->label = L;
    }
}

int trackmesh_tick(TrackMesh* m, float dt) {
    if (!m || m->anim_groups == 0) return 0;
    if (dt > 0.0f) m->anim_time += dt;
    for (int g = 0; g < m->group_count; g++) {
        TrackMeshGroup* grp = &m->groups[g];
        // The two arms are exclusive in the game (`frame_count < 2` picks the
        // scroll one), and the extractor tags a material for one or the other,
        // never both.
        if (grp->anim) { trackmesh_tick_frames(grp->anim, m->anim_time); continue; }
        if (grp->uv_scroll_rate <= 0.0f) continue;
        float target = grp->uv_scroll_rate * m->anim_time;
        float next = grp->uv_scroll_step + grp->uv_scroll_phase;
        if (next <= target)
            grp->uv_scroll_phase = (grp->uv_scroll_step == 0.0f) ? target : next;
        float ph = grp->uv_scroll_phase;
        grp->uv_offset[0] = 1.0f - (ph - floorf(ph));
        grp->uv_offset[1] = 0.0f;
    }
    return m->anim_groups;
}

int trackmesh_group_animated(const TrackMesh* m, int group) {
    if (!m || group < 0 || group >= m->group_count) return 0;
    const TrackMeshGroup* grp = &m->groups[group];
    // B3_TRACK_NOFRAMEANIM=1: pretend the ticker's frame arm does not exist,
    // which bakes the frame-cycling groups at frame 0 and reproduces exactly
    // what this port drew before the arm was recovered -- the SLOW/DOWN boards
    // frozen on one message. Diagnostic, for before/after captures.
    static int noframe = -1;
    if (noframe < 0) noframe = getenv("B3_TRACK_NOFRAMEANIM") != NULL;
    if (grp->anim && grp->anim->count >= 2) return !noframe;
    // rate <= 0 is frozen in retail too -- see the uv_scroll_rate comment in
    // the header -- so those stay baked, at the phase-0 offset retail shows.
    return grp->uv_scroll_rate > 0.0f;
}

unsigned trackmesh_group_texture(const TrackMesh* m, int group) {
    if (!m || group < 0 || group >= m->group_count) return 0;
    const TrackMeshGroup* grp = &m->groups[group];
    const TrackMeshAnim* a = grp->anim;
    if (a && a->count >= 2) {
        int i = a->index;
        if (i >= 0 && i < a->count && a->frame_gl[i]) return a->frame_gl[i];
    }
    return grp->gl_texture;
}

int trackmesh_load_frame_textures(TrackMesh* m, TrackMeshTexLoader fn,
                                  void* user) {
    if (!m || !fn) return 0;
    int n = 0;
    for (int g = 0; g < m->group_count; g++) {
        TrackMeshAnim* a = m->groups[g].anim;
        if (!a || a->count < 2) continue;
        for (int i = 0; i < a->count; i++) {
            if (!a->frame_texture[i][0]) continue;
            // Frames repeat across groups (every submesh of one material has
            // its own copy of the record) and across materials (`water` is
            // shared), so reuse a name already resolved for the same path.
            unsigned tex = 0;
            for (int h = 0; h <= g && !tex; h++) {
                const TrackMeshAnim* b = m->groups[h].anim;
                if (!b) continue;
                int lim = (h == g) ? i : b->count;
                for (int k = 0; k < lim; k++)
                    if (b->frame_gl[k]
                        && strcmp(b->frame_texture[k], a->frame_texture[i]) == 0) {
                        tex = b->frame_gl[k];
                        break;
                    }
            }
            if (!tex) {
                tex = fn(a->frame_texture[i], user);
                if (tex) n++;
            }
            a->frame_gl[i] = tex;
        }
    }
    return n;
}

/* The GL emission that used to live below this line -- the vertex-array
 * scratch, trackmesh_draw_scroll(), the fog vertex program with
 * trackmesh_fog_begin/end, and trackmesh_draw_shine() -- moved into
 * src/burnout3_render.c when the renderer became retained.  Their recovered
 * arithmetic and every citation with it went along; this file is now the
 * LOADER and the material MODEL, and issues no draw calls at all.
 *
 * What each one became:
 *   trackmesh_group_state()   -> trackmesh_group_material(), above: the same
 *                                decode, handed back as three ints instead of
 *                                six glEnable/glDisable pairs per group.
 *   trackmesh_fog_begin/end   -> b3r_fog(), two uniforms.  The coordinate
 *                                clamp that needed a whole vertex program is
 *                                one min() in the world vertex shader.
 *   trackmesh_draw_scroll()   -> b3r_track_draw_scroll(): the same groups in
 *                                the same order out of a static VBO, with the
 *                                UV offset as a uniform rather than a CPU
 *                                rewrite of every vertex.
 *   trackmesh_draw_shine()    -> b3r_track_draw_shine(): the same per-vertex
 *                                specular, into a dynamic colour buffer that
 *                                is uploaded once a frame instead of once a
 *                                group.
 */
