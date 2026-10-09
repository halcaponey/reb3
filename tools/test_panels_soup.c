#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "burnout3_panels.h"
#include "burnout3_vehicle_sim.h"

int b3_ground_probe(float x, float y, float z, float* h, float n[3]) {
    (void)x; (void)y; (void)z;
    if (h) *h = 0.0f;
    if (n) { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }
    return 0;
}

static int s_test_gather_calls = 0;
static B3WorldPoly s_ramp_polys[1];

static int test_soup_gather_ramp(const float center[3], const float half[3],
                                 B3WorldPoly* out_polys, int max_polys) {
    (void)center; (void)half;
    s_test_gather_calls++;
    if (max_polys < 1) return 0;
    out_polys[0] = s_ramp_polys[0];
    return 1;
}

int main(void) {
    printf("=== Testing Debris / Panel World Narrow Phase (FUN_001072A0 / FUN_00106D00) ===\n");

    B3PanelSet ps;
    memset(&ps, 0, sizeof(ps));
    ps.n = 1;
    ps.kind[0] = B3_PANEL_KIND_BONNET;
    ps.state[0] = B3_PANEL_DETACHED;

    B3PanelPiece* p = &ps.piece[0];
    p->active = 1;
    p->panel = 0;
    p->mass = 260.0f;
    p->bbmax[0] =  0.5f; p->bbmax[1] =  0.2f; p->bbmax[2] =  0.5f;
    p->bbmin[0] = -0.5f; p->bbmin[1] = -0.2f; p->bbmin[2] = -0.5f;
    p->half[0] = 0.5f; p->half[1] = 0.2f; p->half[2] = 0.5f;
    b3_piece_inertia(p->mass, p->bbmax, p->bbmin, (float*)p->iinv_body);

    /* Placement: identity rotation, pos (0, 0.15, 0) falling downward */
    for (int r = 0; r < 4; r++) p->frame[r][r] = 1.0f;
    p->frame[3][1] = 0.15f;
    p->vel[1] = -2.0f; p->vel[3] = 2.0f;

    /* -------------------------------------------------------------
     * Test 1: Ground plane fallback (no soup gatherer installed)
     * ------------------------------------------------------------- */
    b3_panels_set_soup_gather(NULL);
    float ground_y[B3_PANEL_MAX] = { 0.0f };
    const float dt = 1.0f / 60.0f;

    b3_panels_pieces_update(&ps, ground_y, dt);
    assert(p->active);
    /* Penetration with y=0 should push y up and reflect/dampen vel */
    assert(p->frame[3][1] >= 0.15f);
    printf("Test 1 (Plane contact fallback when gatherer NULL): PASS\n");

    /* -------------------------------------------------------------
     * Test 2: 3D Polygon soup (sloped ramp face)
     * ------------------------------------------------------------- */
    /* Reset piece above a 45-degree ramp with normal (0, sqrt(0.5), sqrt(0.5)) */
    memset(p->frame, 0, sizeof(p->frame));
    for (int r = 0; r < 4; r++) p->frame[r][r] = 1.0f;
    p->frame[3][0] = 0.0f;
    p->frame[3][1] = 0.10f;
    p->frame[3][2] = 0.0f;
    p->vel[0] = 0.0f; p->vel[1] = -5.0f; p->vel[2] = 0.0f; p->vel[3] = 5.0f;
    p->rest = 0.0f;

    /* Sloped triangle under the piece in game space */
    const float inv_sqrt2 = 0.70710678f;
    s_ramp_polys[0].v[0][0] = -2.0f; s_ramp_polys[0].v[0][1] = -1.0f; s_ramp_polys[0].v[0][2] = -1.0f;
    s_ramp_polys[0].v[1][0] =  2.0f; s_ramp_polys[0].v[1][1] = -1.0f; s_ramp_polys[0].v[1][2] = -1.0f;
    s_ramp_polys[0].v[2][0] =  0.0f; s_ramp_polys[0].v[2][1] =  1.0f; s_ramp_polys[0].v[2][2] =  1.0f;
    s_ramp_polys[0].n[0]    =  0.0f; s_ramp_polys[0].n[1]    = inv_sqrt2; s_ramp_polys[0].n[2] = -inv_sqrt2;

    s_test_gather_calls = 0;
    b3_panels_set_soup_gather(test_soup_gather_ramp);

    b3_panels_pieces_update(&ps, ground_y, dt);
    assert(s_test_gather_calls == 1);
    /* Velocity was directed downward, ramp normal has Z component,
     * so resolved velocity should acquire Z motion after slope impulse */
    assert(fabsf(p->vel[2]) > 0.01f || p->frame[3][2] != 0.0f);
    printf("Test 2 (FUN_001072A0 3D polygon soup OBB contact): PASS\n");

    /* -------------------------------------------------------------
     * Test 3: Sleep latch skips collision & update (retail 0x001072AC)
     * ------------------------------------------------------------- */
    p->rest = 1.0f;
    s_test_gather_calls = 0;
    float saved_y = p->frame[3][1];

    b3_panels_pieces_update(&ps, ground_y, dt);
    /* Should NOT call gatherer when sleeping */
    assert(s_test_gather_calls == 0);
    assert(p->frame[3][1] == saved_y);
    printf("Test 3 (Sleep latch skips gather & integrate): PASS\n");

    printf("ALL DEBRIS / PANEL NARROW PHASE TESTS PASSED!\n");
    return 0;
}
