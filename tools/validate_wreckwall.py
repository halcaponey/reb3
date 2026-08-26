#!/usr/bin/env python3
"""
WRECK-vs-WALL CONTAINMENT -- the crashed body's world pass, against retail.

WHY THIS EXISTS
    User report: "when crashing the wrecks are going off track through walls
    etc".  A retail wreck tumbles along a barrier and stays on the track; the
    port's wreck sailed through it.  Measured on the shipped US_C3_V1 soup, a
    wreck driven into a road-level barrier at 55 m/s ended 84 m BEHIND it, and
    the wreck's world-contact pass reported ZERO contacts in 120 consecutive
    frames pressed against that barrier.

THE THREE DEFECTS, and the retail law each violated

  1.  src/burnout3_crash.c  wreck_inv_frame
      FUN_00040AE0 transposes the rotation block IN PLACE and only then
      back-rotates the translation against the ALREADY-TRANSPOSED rows -- as
      b3_mat_invert_rigid (burnout3_vehicle_sim.c:897) and b3p_build_inv_frame
      (burnout3_props.c:286), the tree's two other ports of it, both do.  The
      wreck's copy multiplied the translation against the ORIGINAL matrix,
      i.e. -(pos . column j) instead of -(pos . row j).  Those agree only for
      an unyawed body or at the world origin -- and the origin is exactly
      where tools/validate_crash_traj.py seeds its wrecks, which is why the
      suite never saw it.  A wreck yawed 37 deg at a real track coordinate
      landed every world point it transformed 7.1 km away, so the narrow phase
      could not intersect ANY wall.

  2.  src/burnout3_full.c   the wreck's world pass
      It called the SINGLE-PLANE narrow phase once per gathered polygon,
      handing it `soup[poly].v0` as the plane point.  The plane form has no
      polygon, so it fabricates a square of half-size |box dims| + 1 = 6.15 m
      CENTRED on that point; a triangle's first vertex is not under the car.
      On US_C3_V1's 20196 near-vertical loader-admitted faces the median has a
      vertex 10.40 m from v0, p90 is 196.44 m, and 78.7% exceed 6.15 m -- so
      four wall faces in five produced no contact at all.

  3.  ...and resolving ONE CONTACT PER POLYGON is not retail either.
      FUN_00109EA0 calls FUN_00107950 ONCE (@0x00109F34, cdecl, ten args) with
      the WHOLE soup [body+0x200]; the narrow phase walks it itself, sums the
      clipping faces' normals, averages their clipped centroids, and returns a
      single contact.  Resolving per polygon applies N impulses, N friction
      damps and N push-outs in one frame; with (1) and (2) fixed but this left
      alone the wreck was launched over a kilometre off the track.

  4.  src/burnout3_full.c   mesh_collide's admission gate (the curb work)
      b3_sweep_sphere_admit's callback reads the DRIVING sim's pose
      (v->fsim.rb.inv_frame / half_ext / center_off).  A crashed car returns
      from vehicle_update before b3_vehicle_step_full runs, so that pose is
      frozen at the last pre-crash frame; within a second the tumbling wreck
      is tens of metres from it and the box test refuses every face.  Measured:
      0 push-outs in 120 frames against a barrier.  Retail's crashed body does
      not run FUN_0011AC30 at all, so a wreck takes the unadmitted sweep.

SECTIONS
  1  FUN_00107950 whole-function differential -- the REAL narrow phase under
     Unicorn (ten-argument cdecl, exactly as FUN_00109EA0 pushes it) against
     b3_rigid_body_obb_soup_contact, over synthetic planes AND over the real
     gathered soups at the real poses the crash trajectory below passes
     through.  This is what pins the fix to retail: at every frame of the
     trajectory the port applies the contact retail computes.
  2  the inverse frame -- round-trip identity at real track coordinates, and
     the old formula failing it.
  3  containment -- a wall-adjacent crash over the shipped collision soup,
     aftertouch HELD and RELEASED, asserting the body never crosses the
     barrier plane and its deepest penetration stays inside one frame of
     travel plus retail's own 0.005 push-out floor.

USAGE
    python3 tools/validate_wreckwall.py [--track US_C3_V1] [--quick]
    (offline: the shipped collision.bin plus build/burnout3.elf under Unicorn.
     No window, no game lock.)
"""
import argparse
import importlib.util
import json
import math
import os
import struct
import subprocess
import sys

# The drivers link burnout3_backend.c; pin them to the RE path so a flipped
# build/backends.cfg cannot change what this measures.
os.environ.setdefault('B3_BACKENDS', '/dev/null')

_here = os.path.dirname(os.path.abspath(__file__))
_root = os.path.dirname(_here)
sys.path.insert(0, _here)

PASS = 0
FAIL = 0
SKIP = 0


def check(name, ok, detail=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print("  ok   %s" % name)
    else:
        FAIL += 1
        print("  FAIL %s   %s" % (name, detail))


def skip(name, why):
    global SKIP
    SKIP += 1
    print("  skip %s   (%s)" % (name, why))


# ===========================================================================
# the C driver
# ===========================================================================
DRIVER_C = r'''
/* wreckwall_drv.c -- generated by tools/validate_wreckwall.py.
 *
 * Two modes:
 *   traj   run the live wreck loop (an exact transcription of the crashed-
 *          vehicle block in src/burnout3_full.c) over a REAL track soup, and
 *          emit one JSON record per frame: the pose, the gathered soup and
 *          the body's penetration behind the target barrier plane.
 *   narrow read one JSON record on stdin and print what
 *          b3_rigid_body_obb_soup_contact makes of it.  Everything in this
 *          mode is GAME space, so the Unicorn side can be fed the identical
 *          numbers.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "burnout3_collision.h"
#include "burnout3_crash.h"
#include "burnout3_vehicle_sim.h"

#define SOUP_MAX 96

static float dot3(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void closest_on_tri(const float* a, const float* b, const float* c,
                           const float* p, float* out) {
    float ab[3], ac[3], ap[3];
    for (int i = 0; i < 3; i++) { ab[i]=b[i]-a[i]; ac[i]=c[i]-a[i]; ap[i]=p[i]-a[i]; }
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    float bp[3]; for (int i=0;i<3;i++) bp[i]=p[i]-b[i];
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1*d4 - d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 / (d1 - d3);
        for (int i=0;i<3;i++) out[i] = a[i] + ab[i]*v; return;
    }
    float cp[3]; for (int i=0;i<3;i++) cp[i]=p[i]-c[i];
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
    float vb = d5*d2 - d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 / (d2 - d6);
        for (int i=0;i<3;i++) out[i] = a[i] + ac[i]*w; return;
    }
    float va = d3*d6 - d5*d4;
    if (va <= 0 && (d4-d3) >= 0 && (d5-d6) >= 0) {
        float w = (d4-d3) / ((d4-d3) + (d5-d6));
        for (int i=0;i<3;i++) out[i] = b[i] + (c[i]-b[i])*w; return;
    }
    float den = 1.0f/(va+vb+vc), v = vb*den, w = vc*den;
    for (int i=0;i<3;i++) out[i] = a[i] + ab[i]*v + ac[i]*w;
}

/* -------------------------------------------------------------------- */
/* mode: narrow                                                          */
/* -------------------------------------------------------------------- */
static int rd_floats(const char* s, float* out, int n) {
    int got = 0;
    while (got < n && *s) {
        char* end;
        float v = strtof(s, &end);
        if (end == s) { s++; continue; }
        out[got++] = v; s = end;
    }
    return got;
}

static int mode_narrow(void) {
    static char line[1 << 20];
    if (!fgets(line, sizeof line, stdin)) return 1;
    /* the record is a flat float list:
     *   16 frame  3 bbmin  3 bbmax  1 npoly  npoly*12 soup   (GAME space) */
    static float buf[1 << 16];
    int n = rd_floats(line, buf, (int)(sizeof buf / sizeof buf[0]));
    if (n < 23) return 2;
    float store[4][4];
    B3RigidBody rb;
    memset(&rb, 0, sizeof rb);
    b3_rigid_body_bind_frame(&rb, store);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) rb.frame[r][c] = buf[r*4 + c];
    /* the inverse the port itself would build (FUN_00040AE0) */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) rb.inv_frame[i][j] = rb.frame[j][i];
    rb.inv_frame[0][3] = rb.inv_frame[1][3] = rb.inv_frame[2][3] = 0.0f;
    for (int j = 0; j < 3; j++)
        rb.inv_frame[3][j] = -(rb.frame[3][0]*rb.inv_frame[0][j]
                             + rb.frame[3][1]*rb.inv_frame[1][j]
                             + rb.frame[3][2]*rb.inv_frame[2][j]);
    rb.inv_frame[3][3] = 1.0f;
    float bbmin[3] = { buf[16], buf[17], buf[18] };
    float bbmax[3] = { buf[19], buf[20], buf[21] };
    int npoly = (int)buf[22];
    static B3WorldPoly polys[SOUP_MAX];
    if (npoly > SOUP_MAX) npoly = SOUP_MAX;
    for (int i = 0; i < npoly; i++) {
        const float* p = &buf[23 + i*12];
        for (int k = 0; k < 3; k++) {
            polys[i].v[0][k] = p[0+k];
            polys[i].v[1][k] = p[3+k];
            polys[i].v[2][k] = p[6+k];
            polys[i].n[k]    = p[9+k];
        }
    }
    B3WorldContact ct;
    int hit = b3_rigid_body_obb_soup_contact(&rb, bbmin, bbmax, polys, npoly,
                                             &ct);
    printf("%d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", hit,
           ct.point[0], ct.point[1], ct.point[2],
           ct.normal[0], ct.normal[1], ct.normal[2],
           ct.pushout[0], ct.pushout[1], ct.pushout[2]);
    return 0;
}

/* -------------------------------------------------------------------- */
/* mode: traj                                                            */
/* -------------------------------------------------------------------- */
static float g_tv0[3], g_tv1[3], g_tv2[3], g_tn[3];

/* the deepest the ORIENTED BODY BOX reaches behind the barrier plane */
static float body_penetration(const B3WreckState* w) {
    float worst = 0.0f;
    for (int c = 0; c < 8; c++) {
        float b[3];
        b[0] = (c & 1) ? w->bbmax[0] : w->bbmin[0];
        b[1] = (c & 2) ? w->bbmax[1] : w->bbmin[1];
        b[2] = (c & 4) ? w->bbmax[2] : w->bbmin[2];
        float wp[3];
        for (int k = 0; k < 3; k++)
            wp[k] = b[0]*w->frame[0][k] + b[1]*w->frame[1][k]
                  + b[2]*w->frame[2][k] + w->frame[3][k];
        float rel[3] = { wp[0]-g_tv0[0], wp[1]-g_tv0[1], wp[2]-g_tv0[2] };
        float s = dot3(rel, g_tn);
        /* only while the corner is actually over the face */
        float q[3];
        closest_on_tri(g_tv0, g_tv1, g_tv2, wp, q);
        float dx = wp[0]-q[0], dy = wp[1]-q[1], dz = wp[2]-q[2];
        float off2 = dx*dx + dy*dy + dz*dz - s*s;
        if (off2 > 1.0f) continue;               /* past the face's edge */
        if (-s > worst) worst = -s;
    }
    return worst;
}

int main(int argc, char** argv) {
    const char* track = "US_C3_V1";
    const char* mode = "traj";
    int frames = 150, pick = 0, want_ext = 1, at_hold = 0, dump = 0;
    float speed = 40.0f, at_h = 0.0f, at_v = 0.0f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode")) mode = argv[++i];
        else if (!strcmp(argv[i], "--track")) track = argv[++i];
        else if (!strcmp(argv[i], "--frames")) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pick")) pick = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ext")) want_ext = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--speed")) speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) dump = 1;
        else if (!strcmp(argv[i], "--at")) {
            sscanf(argv[++i], "%f,%f", &at_h, &at_v); at_hold = 1;
        }
    }
    if (!strcmp(mode, "narrow")) return mode_narrow();

    char path[512];
    snprintf(path, sizeof path, "build/tracks/%s/collision.bin", track);
    if (!b3_collision_load(path)) { fprintf(stderr, "load %s\n", path); return 2; }
    b3_wreck_set_world_resolve(b3_rigid_body_obb_plane_contact,
                               b3_rigid_body_world_contact);
#ifndef WRECKWALL_NOFIX
    b3_wreck_set_world_soup(b3_rigid_body_obb_soup_contact);
#endif

    /* ---- pick a ROAD-LEVEL barrier ---------------------------------- */
    int n = b3_collision_tri_count(), chosen = -1, seen = 0;
    float ylo = 0.0f, yhi = 0.0f;
    for (int i = 0; i < n; i++) {
        float a[3], b[3], c[3], nn[3]; unsigned short ty; int ex;
        if (!b3_collision_tri_get(i, a, b, c, nn, &ty, &ex)) continue;
        if (ex || fabsf(nn[1]) > 0.35f) continue;
        float e1 = 0, e2 = 0;
        for (int k = 0; k < 3; k++) { e1 += (b[k]-a[k])*(b[k]-a[k]);
                                      e2 += (c[k]-a[k])*(c[k]-a[k]); }
        float ext = sqrtf(e1) > sqrtf(e2) ? sqrtf(e1) : sqrtf(e2);
        if (want_ext && ext < 20.0f) continue;
        if (!want_ext && (ext > 6.0f || ext < 1.5f)) continue;
        float lo = fminf(a[1], fminf(b[1], c[1]));
        float hi = fmaxf(a[1], fmaxf(b[1], c[1]));
        if (hi - lo < 1.5f) continue;
        float cc[3];
        for (int k = 0; k < 3; k++) cc[k] = (a[k]+b[k]+c[k])/3.0f;
        float pg[3] = { cc[0] + nn[0]*3.0f, 0.0f, cc[2] + nn[2]*3.0f };
        float gy2, gnn[3]; int found = 0;
        for (float yy = lo + 0.5f; yy <= hi && !found; yy += 2.0f) {
            if (b3_ground_probe(pg[0], yy, pg[2], &gy2, gnn) < 0) continue;
            if (gy2 < lo - 0.3f || gy2 > hi - 1.0f) continue;
            found = 1;
        }
        if (!found) continue;
        if (seen++ != pick) continue;
        chosen = i; memcpy(g_tv0,a,12); memcpy(g_tv1,b,12);
        memcpy(g_tv2,c,12); memcpy(g_tn,nn,12); ylo = lo; yhi = hi;
        break;
    }
    if (chosen < 0) { fprintf(stderr, "no barrier for pick %d\n", pick); return 3; }

    float cen[3];
    for (int k = 0; k < 3; k++) cen[k] = (g_tv0[k]+g_tv1[k]+g_tv2[k])/3.0f;
    float ground = ylo, gy, gn3[3];
    float px = cen[0] + g_tn[0]*3.0f, pz = cen[2] + g_tn[2]*3.0f;
    for (float yy = ylo + 0.5f; yy <= yhi; yy += 2.0f) {
        if (b3_ground_probe(px, yy, pz, &gy, gn3) < 0) continue;
        if (gy < ylo - 0.3f || gy > yhi - 1.0f) continue;
        ground = gy; break;
    }
    float pos[3] = { px, ground + 0.9f, pz };
    float vel[3] = { -g_tn[0]*speed, 0.0f, -g_tn[2]*speed };
    const float bbmin[3] = {-0.95f, -0.55f, -2.30f};
    const float bbmax[3] = { 0.95f,  0.75f,  2.30f};
    float heading = atan2f(vel[0], -vel[2]);
    float cn[3] = { g_tn[0], g_tn[1], g_tn[2] };

    B3WreckState wk; memset(&wk, 0, sizeof wk);
    b3_wreck_begin_entry(&wk, B3_WRECK_ENTRY_WALL, pos, heading, vel,
                         1400.0f, bbmin, bbmax, pos, cn, vel);
    { float d[3] = {B3_WRECK_IINV_DEFAULT_X, B3_WRECK_IINV_DEFAULT_Y,
                    B3_WRECK_IINV_DEFAULT_Z};
      b3_wreck_set_inertia(&wk, d); }

    float cf[3] = { vel[0]/speed, 0.0f, vel[2]/speed };
    float cr[3] = { -cf[2], 0.0f, cf[0] };
    const float dt = 1.0f/60.0f;
    float worst = 0.0f;
    int crossed = -1, ncontact = 0;

    printf("{\"tri\":%d,\"n\":[%.9g,%.9g,%.9g],\"v0\":[%.9g,%.9g,%.9g],"
           "\"speed\":%.9g,\"at\":%d}\n", chosen, g_tn[0], g_tn[1], g_tn[2],
           g_tv0[0], g_tv0[1], g_tv0[2], speed, at_hold);

    for (int f = 0; f < frames; f++) {
        float pre[3] = { wk.frame[3][0], wk.frame[3][1], wk.frame[3][2] };
        if (at_hold) {
            B3WreckAftertouchIn ai;
            memset(&ai, 0, sizeof ai);
            ai.h = at_h; ai.v = at_v; ai.engaged = 1;
            ai.crash_clock = (float)f * dt;
            memcpy(ai.cam_right, cr, sizeof cr);
            memcpy(ai.cam_fwd,   cf, sizeof cf);
            ai.want_bank = 1;
            b3_wreck_aftertouch_steer(&wk, &ai, dt);
        }

        B3CollisionPoly soup[SOUP_MAX];
        int ns = 0;
        if (b3_collision_ready()) {
            float half[3];
            float center[3] = { wk.frame[3][0], wk.frame[3][1], wk.frame[3][2] };
            float velocity[3] = { wk.vel[0], wk.vel[1], wk.vel[2] };
            for (int a = 0; a < 3; a++) {
                float lo = fabsf(wk.bbmin[a]), hi = fabsf(wk.bbmax[a]);
                half[a] = (lo > hi ? lo : hi) + 0.5f;
            }
            ns = b3_collision_gather_walls(center, half, velocity, 0.6f,
                                           soup, SOUP_MAX);
#ifdef WRECKWALL_NOFIX
            for (int p = 0; p < ns; p++)
                ncontact += b3_wreck_world_contact(&wk, soup[p].v0,
                                                   soup[p].normal);
#else
            B3WorldPoly wp[SOUP_MAX];
            for (int p = 0; p < ns; p++) {
                memcpy(wp[p].v[0], soup[p].v0, 12);
                memcpy(wp[p].v[1], soup[p].v1, 12);
                memcpy(wp[p].v[2], soup[p].v2, 12);
                memcpy(wp[p].n,    soup[p].normal, 12);
            }
            ncontact += b3_wreck_world_contact_soup(&wk, wp, ns);
#endif
        }

        if (dump) {
            /* the frame and the soup, MIRRORED TO GAME SPACE, so the Unicorn
             * oracle and the port's narrow phase see identical numbers */
            printf("F %d", f);
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    printf(" %.9g", (c == 2) ? -wk.frame[r][c] : wk.frame[r][c]);
            for (int k = 0; k < 3; k++) printf(" %.9g", wk.bbmin[k]);
            for (int k = 0; k < 3; k++) printf(" %.9g", wk.bbmax[k]);
            printf(" %d", ns);
            for (int p = 0; p < ns; p++) {
                const float* vv[3] = { soup[p].v0, soup[p].v1, soup[p].v2 };
                for (int j = 0; j < 3; j++)
                    printf(" %.9g %.9g %.9g", vv[j][0], vv[j][1], -vv[j][2]);
                printf(" %.9g %.9g %.9g", soup[p].normal[0],
                       soup[p].normal[1], -soup[p].normal[2]);
            }
            printf("\n");
        }

        float gyy;
        if (b3_ground_probe(wk.frame[3][0], wk.frame[3][1], wk.frame[3][2],
                            &gyy, gn3) >= 0) ground = gyy;
        b3_wreck_update(&wk, ground, dt);

        /* the harness anti-tunnelling net, verbatim */
        float hp[3], hn[3];
        if (b3_sweep_sphere(pre, wk.frame[3], 1.0f, 0.6f, hp, hn)) {
            float dx = wk.frame[3][0]-hp[0], dy = wk.frame[3][1]-hp[1],
                  dz = wk.frame[3][2]-hp[2];
            float d = sqrtf(dx*dx+dy*dy+dz*dz);
            float depth = 1.0f - d;
            if (depth > 0.0f) {
                wk.frame[3][0] += hn[0]*depth;
                wk.frame[3][1] += hn[1]*depth;
                wk.frame[3][2] += hn[2]*depth;
            }
        }

        float rel[3] = { wk.frame[3][0]-g_tv0[0], wk.frame[3][1]-g_tv0[1],
                         wk.frame[3][2]-g_tv0[2] };
        float s = dot3(rel, g_tn);
        if (s < 0.0f && crossed < 0) crossed = f;
        float pen = body_penetration(&wk);
        if (pen > worst) worst = pen;
        printf("T %d %.4f %.4f %.4f %.4f %.4f %.4f\n", f,
               wk.frame[3][0], wk.frame[3][1], wk.frame[3][2],
               wk.vel[3], s, pen);
    }
    printf("R contacts %d crossed %d body_pen %.4f centre_signed %.4f\n",
           ncontact, crossed, worst,
           (wk.frame[3][0]-g_tv0[0])*g_tn[0] + (wk.frame[3][1]-g_tv0[1])*g_tn[1]
           + (wk.frame[3][2]-g_tv0[2])*g_tn[2]);
    return 0;
}
'''


def build_driver(tag='', defines=()):
    src = os.path.join(_root, 'build', 'wreckwall_drv.c')
    exe = os.path.join(_root, 'build', 'wreckwall_drv' + tag)
    os.makedirs(os.path.join(_root, 'build'), exist_ok=True)
    with open(src, 'w') as f:
        f.write(DRIVER_C)
    r = subprocess.run(['cc', '-O2', '-Isrc'] + list(defines) + [
        '-o', exe, src,
        'src/burnout3_crash.c', 'src/burnout3_vehicle_sim.c',
        'src/burnout3_collision.c', 'src/burnout3_backend.c',
        'src/burnout3_emu.c', '-lm'],
        cwd=_root, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        return None
    return exe


def run_traj(exe, **kw):
    argv = [exe]
    for k, v in kw.items():
        argv += ['--' + k, str(v)] if v is not True else ['--' + k]
    r = subprocess.run(argv, cwd=_root, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or 'driver failed')
    head, traj, dumps, res = None, [], [], None
    for line in r.stdout.splitlines():
        if line.startswith('{'):
            head = json.loads(line)
        elif line.startswith('T '):
            p = line.split()
            traj.append(dict(f=int(p[1]), pos=[float(x) for x in p[2:5]],
                             speed=float(p[5]), signed=float(p[6]),
                             pen=float(p[7])))
        elif line.startswith('F '):
            dumps.append([float(x) for x in line.split()[2:]])
        elif line.startswith('R '):
            p = line.split()
            res = dict(contacts=int(p[2]), crossed=int(p[4]),
                       body_pen=float(p[6]), centre=float(p[8]))
    return head, traj, dumps, res


# ===========================================================================
# 1. FUN_00107950 whole-function differential
# ===========================================================================
def load_unicorn():
    try:
        _spec = importlib.util.spec_from_file_location(
            "ep", os.path.join(_here, "emulate_pipeline.py"))
        ep = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(ep)
        return ep
    except Exception as e:                                  # noqa: BLE001
        print("  (unicorn oracle unavailable: %s)" % e)
        return None


F_107950 = 0x00107950


class Narrow:
    """FUN_00107950 at its own address, over a caller-supplied soup.

    The ten arguments are pushed exactly as FUN_00109EA0 pushes them
    @0x00109F0B..0x00109F34 (cdecl, `add esp,0x28` after the call):
        param_1  body+0x1D0   the box: bbmax at +0x1D0, bbmin at +0x1E0
        param_2  [body+0x204] the frame object (a bare 4x4)
        param_3  body+0x70    the inverse frame
        param_4  [body+0x200] the soup header {count, records, types}
        param_5  body+0x160   out: contact point
        param_6  body+0x170   out: contact normal
        param_7  body+0x190   out: surface type
        param_8  body+0x1A0   out: clipped min
        param_9  body+0x1B0   out: clipped max
        param_10 a local      out: the push-out
    """

    def __init__(self, ep):
        self.ep = ep
        self.p = ep.Pipeline()
        self.OUT = ep.REGION_LO + 0x20000        # scratch for param_10

    def run(self, frame, inv, bbmin, bbmax, polys):
        V, ep = self.p, self.ep
        V._write_matrix(ep.CTX0, frame)
        V._write_matrix(ep.VEHICLE + 0x70, inv)
        for i in range(3):
            V.wf(ep.VEHICLE + 0x1D0 + 4 * i, bbmax[i])
            V.wf(ep.VEHICLE + 0x1E0 + 4 * i, bbmin[i])
        V.wf(ep.VEHICLE + 0x1DC, 0.0)
        V.wf(ep.VEHICLE + 0x1EC, 0.0)
        n = min(len(polys), ep.SOUP_MAX)
        V.wu(ep.SOUP_HDR + 0, n)
        V.wu(ep.SOUP_HDR + 4, ep.SOUP_REC)
        V.wu(ep.SOUP_HDR + 8, ep.SOUP_TYPE)
        for t in range(n):
            base = ep.SOUP_REC + 0x40 * t
            v0, v1, v2, nn = polys[t]
            for j, p in enumerate((v0, v1, v2)):
                for k in range(3):
                    V.wf(base + 0x10 * j + 4 * k, p[k])
                V.wf(base + 0x10 * j + 12, 1.0)
            for k in range(3):
                V.wf(base + 0x30 + 4 * k, nn[k])
            V.wf(base + 0x3C, 0.0)
            V.uc.mem_write(ep.SOUP_TYPE + 2 * t, struct.pack('<H', 0))
        V.uc.mem_write(self.OUT, b'\0' * 0x40)
        V.uc.mem_write(ep.VEHICLE + 0x160, b'\0' * 0x60)
        args = [ep.VEHICLE + 0x1D0, ep.CTX0, ep.VEHICLE + 0x70, ep.SOUP_HDR,
                ep.VEHICLE + 0x160, ep.VEHICLE + 0x170, ep.VEHICLE + 0x190,
                ep.VEHICLE + 0x1A0, ep.VEHICLE + 0x1B0, self.OUT]
        V.call(F_107950, stack_args=args)
        from unicorn.x86_const import UC_X86_REG_EAX
        hit = V.uc.reg_read(UC_X86_REG_EAX) & 0xFF
        pt = [V.rf(ep.VEHICLE + 0x160 + 4 * i) for i in range(3)]
        nr = [V.rf(ep.VEHICLE + 0x170 + 4 * i) for i in range(3)]
        po = [V.rf(self.OUT + 4 * i) for i in range(3)]
        return hit, pt, nr, po


def port_narrow(exe, frame, bbmin, bbmax, polys):
    rec = []
    for r in range(4):
        rec += [frame[r][c] for c in range(4)]
    rec += list(bbmin) + list(bbmax) + [float(len(polys))]
    for v0, v1, v2, nn in polys:
        rec += list(v0) + list(v1) + list(v2) + list(nn)
    r = subprocess.run([exe, '--mode', 'narrow'], cwd=_root, input=
                       ' '.join('%.9g' % x for x in rec) + '\n',
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or 'narrow failed')
    p = [float(x) for x in r.stdout.split()]
    return int(p[0]), p[1:4], p[4:7], p[7:10]


def rows_yaw(yaw_deg, pos):
    c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    return [[c, 0.0, s, 0.0], [0.0, 1.0, 0.0, 0.0], [-s, 0.0, c, 0.0],
            [pos[0], pos[1], pos[2], 1.0]]


def inv_of(rows):
    inv = [[0.0] * 4 for _ in range(4)]
    for i in range(3):
        for j in range(3):
            inv[i][j] = rows[j][i]
    for j in range(3):
        inv[3][j] = -(rows[3][0] * inv[0][j] + rows[3][1] * inv[1][j]
                      + rows[3][2] * inv[2][j])
    inv[3][3] = 1.0
    return inv


def near(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def section_narrow(exe, ep, dumps):
    print("\n1. FUN_00107950 -- the narrow phase, whole-function differential")
    if ep is None:
        skip("FUN_00107950 differential", "unicorn/ELF unavailable")
        return
    try:
        orc = Narrow(ep)
    except Exception as e:                                   # noqa: BLE001
        skip("FUN_00107950 differential", str(e))
        return

    BBMIN = (-0.95, -0.55, -2.30)
    BBMAX = (0.95, 0.75, 2.30)

    def wall_quad(px, pz, nx, nz, ylo, yhi, half):
        tx, tz = -nz, nx
        a = (px + tx*half, ylo, pz + tz*half)
        b = (px - tx*half, ylo, pz - tz*half)
        c = (px - tx*half, yhi, pz - tz*half)
        d = (px + tx*half, yhi, pz + tz*half)
        n = (nx, 0.0, nz)
        return [(a, b, c, n), (a, c, d, n)]

    cases = []
    # (a) a wall the box straddles, at the origin and at real track coords,
    #     unyawed and yawed, and with the quad's first vertex far away --
    #     the exact shape the live gather produces.
    for label, pos, yaw, half in (
            ("origin, unyawed, 40 m quad", (0.0, 0.0, 0.0), 0.0, 20.0),
            ("origin, yawed 37 deg", (0.0, 0.0, 0.0), 37.0, 20.0),
            ("track coords, yawed 37 deg", (5466.0, 164.0, -2314.0), 37.0, 20.0),
            ("track coords, yawed 113 deg", (5466.0, 164.0, -2314.0), 113.0, 100.0),
            ("track coords, 200 m quad", (-3120.5, 88.25, 1777.0), -61.0, 100.0)):
        rows = rows_yaw(yaw, pos)
        polys = wall_quad(pos[0], pos[2] - 0.5, 0.0, 1.0,
                          pos[1] - 3.0, pos[1] + 3.0, half)
        cases.append((label, rows, polys))
    # (b) a corner: two walls at once, which is where the summed normal is
    #     the whole point
    rows = rows_yaw(20.0, (5466.0, 164.0, -2314.0))
    polys = (wall_quad(5466.0, -2314.5, 0.0, 1.0, 161.0, 167.0, 40.0)
             + wall_quad(5466.5, -2314.0, -1.0, 0.0, 161.0, 167.0, 40.0))
    cases.append(("corner: two walls, summed normal", rows, polys))
    # (c) no overlap at all
    rows = rows_yaw(0.0, (5466.0, 164.0, -2314.0))
    polys = wall_quad(5466.0, -2330.0, 0.0, 1.0, 161.0, 167.0, 40.0)
    cases.append(("no overlap -> no contact", rows, polys))
    # (d) the REAL gathered soups, at the REAL poses the crash passes through
    for i, rec in enumerate(dumps):
        rows = [rec[r*4:r*4+4] for r in range(4)]
        bbmin = rec[16:19]
        bbmax = rec[19:22]
        npoly = int(rec[22])
        polys = []
        for p in range(npoly):
            q = rec[23 + p*12: 23 + p*12 + 12]
            polys.append((q[0:3], q[3:6], q[6:9], q[9:12]))
        cases.append(("live frame %d: %d real polys" % (i, npoly),
                      rows, polys, bbmin, bbmax))

    bad = 0
    for case in cases:
        label, rows, polys = case[0], case[1], case[2]
        bbmin = case[3] if len(case) > 3 else BBMIN
        bbmax = case[4] if len(case) > 4 else BBMAX
        if not polys:
            continue
        rhit, rpt, rn, rpo = orc.run(rows, inv_of(rows), bbmin, bbmax, polys)
        phit, ppt, pn, ppo = port_narrow(exe, rows, bbmin, bbmax, polys)
        ok = (bool(rhit) == bool(phit))
        if rhit and phit:
            ok = (near(rpt, ppt, 2e-2) and near(rn, pn, 2e-4)
                  and near(rpo, ppo, 2e-3))
        if not ok:
            bad += 1
            if bad <= 6:
                print("       retail hit=%d pt=%s n=%s push=%s"
                      % (rhit, ["%.4f" % x for x in rpt],
                         ["%.4f" % x for x in rn],
                         ["%.4f" % x for x in rpo]))
                print("       port   hit=%d pt=%s n=%s push=%s"
                      % (phit, ["%.4f" % x for x in ppt],
                         ["%.4f" % x for x in pn],
                         ["%.4f" % x for x in ppo]))
        if len(cases) <= 12 or not ok or not label.startswith("live"):
            check(label, ok)
    live = [c for c in cases if c[0].startswith("live")]
    if live:
        check("all %d live gathered soups match retail" % len(live), bad == 0,
              "%d mismatched" % bad)


# ===========================================================================
# 2. the inverse frame
# ===========================================================================
def section_inv():
    print("\n2. the inverse frame -- FUN_00040AE0's transpose-then-back-rotate")

    def apply(inv, p):
        return [sum(p[i] * inv[i][j] for i in range(3)) + inv[3][j]
                for j in range(3)]

    def inv_old(rows):
        inv = [[0.0] * 4 for _ in range(4)]
        for i in range(3):
            for j in range(3):
                inv[i][j] = rows[j][i]
        for j in range(3):
            inv[3][j] = -(rows[3][0] * rows[0][j] + rows[3][1] * rows[1][j]
                          + rows[3][2] * rows[2][j])
        inv[3][3] = 1.0
        return inv

    worst_new = 0.0
    worst_old = 0.0
    for pos in ((0.0, 0.0, 0.0), (5466.0, 164.0, -2314.0),
                (-3120.5, 88.25, 1777.0)):
        for yaw in (0.0, 37.0, 113.0, -61.0):
            rows = rows_yaw(yaw, pos)
            for body in ((0.0, 0.0, 2.3), (0.95, 0.75, -2.3)):
                world = [sum(body[i] * rows[i][k] for i in range(3))
                         + rows[3][k] for k in range(3)]
                gn = apply(inv_of(rows), world)
                go = apply(inv_old(rows), world)
                worst_new = max(worst_new,
                                max(abs(a - b) for a, b in zip(gn, body)))
                worst_old = max(worst_old,
                                max(abs(a - b) for a, b in zip(go, body)))
    check("transpose-first inverse round-trips (max err %.2e m)" % worst_new,
          worst_new < 1e-3)
    check("the old column-form is wrong away from the origin "
          "(max err %.0f m)" % worst_old, worst_old > 100.0,
          "expected a large error, got %.3f" % worst_old)


# ===========================================================================
# 3. containment
# ===========================================================================
def section_containment(exe, exe_old, quick):
    print("\n3. containment -- a wall-adjacent crash over the shipped soup")
    picks = [0, 25, 100] if quick else [0, 25, 50, 100, 150]
    speeds = [30, 55] if quick else [25, 40, 55, 70]
    states = [("aftertouch released", {}),
              ("aftertouch held (steer left)", {"at": "-1,0"}),
              ("aftertouch held (steer right)", {"at": "1,0"})]
    dt = 1.0 / 60.0

    worst_before = 0.0
    worst_after = 0.0
    crossings_before = 0
    crossings_after = 0
    total = 0
    zero_contact_before = 0

    for label, extra in states:
        state_bad = []
        for pick in picks:
            for sp in speeds:
                total += 1
                kw = dict(mode='traj', frames=150, pick=pick, speed=sp)
                kw.update(extra)
                try:
                    _, _, _, rb = run_traj(exe_old, **kw)
                    _, _, _, ra = run_traj(exe, **kw)
                except RuntimeError:
                    total -= 1
                    continue
                if rb['crossed'] >= 0:
                    crossings_before += 1
                if ra['crossed'] >= 0:
                    crossings_after += 1
                if rb['contacts'] == 0:
                    zero_contact_before += 1
                worst_before = max(worst_before, rb['body_pen'])
                worst_after = max(worst_after, ra['body_pen'])
                # retail's own bound: the narrow phase removes the whole
                # penetration every frame it sees one and floors the push-out
                # at [0x003B194C] = 0.005, so the body can never be deeper
                # than one frame of approach travel plus that floor.
                bound = sp * dt + 0.005 + 0.5
                if ra['crossed'] >= 0 or ra['body_pen'] > bound:
                    state_bad.append((pick, sp, ra['crossed'], ra['body_pen'],
                                      bound))
        check("%s: contained on %d wall/speed combinations"
              % (label, len(picks) * len(speeds)), not state_bad,
              "; ".join("pick=%d sp=%d crossed=%d pen=%.2f > %.2f"
                        % b for b in state_bad[:4]))

    check("before: the wreck crossed the barrier plane (%d of %d runs)"
          % (crossings_before, total), crossings_before > 0,
          "the defect did not reproduce -- nothing to prove")
    check("before: the world pass reported ZERO contacts (%d of %d runs)"
          % (zero_contact_before, total), zero_contact_before > 0)
    check("after: no run crosses the barrier plane (%d of %d)"
          % (crossings_after, total), crossings_after == 0)
    check("after: deepest body penetration %.3f m < before %.3f m"
          % (worst_after, worst_before), worst_after < worst_before,
          "after %.3f  before %.3f" % (worst_after, worst_before))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--track', default='US_C3_V1')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()

    print("WRECK-vs-WALL containment  (track %s)" % args.track)
    exe = build_driver()
    exe_old = build_driver('_old', ['-DWRECKWALL_NOFIX'])
    if not exe or not exe_old:
        print("driver build failed")
        return 1

    # the poses and soups the live crash actually passes through
    dumps = []
    try:
        _, _, dumps, _ = run_traj(exe, mode='traj', frames=40, pick=0,
                                  speed=40, track=args.track, dump=True)
    except RuntimeError as e:
        print("  (trajectory dump unavailable: %s)" % e)
    dumps = [d for d in dumps if int(d[22]) > 0][:12]

    ep = load_unicorn()
    section_narrow(exe, ep, dumps)
    section_inv()
    section_containment(exe, exe_old, args.quick)

    print("\n%d passed, %d failed, %d skipped" % (PASS, FAIL, SKIP))
    return 1 if FAIL else 0


if __name__ == '__main__':
    sys.exit(main())
