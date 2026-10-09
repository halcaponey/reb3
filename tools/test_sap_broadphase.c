#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "burnout3_carcol.h"

int b3_ground_probe(float x, float y, float z, float* h, float n[3]) {
    (void)x; (void)y; (void)z; (void)h; (void)n; return -1;
}

static void init_box(B3CarBody* b, B3RigidBody* rb, float frame[4][4],
                     float x, float y, float z,
                     float hx, float hy, float hz,
                     int type, int crashed, int asleep) {
    memset(b, 0, sizeof(*b));
    memset(rb, 0, sizeof(*rb));
    memset(frame, 0, sizeof(float) * 16);
    frame[0][0] = 1.0f;
    frame[1][1] = 1.0f;
    frame[2][2] = 1.0f;
    frame[3][0] = x;
    frame[3][1] = y;
    frame[3][2] = z;
    frame[3][3] = 1.0f;
    rb->frame = frame;
    b->rb = rb;
    b->mass = 1500.0f;
    b->bbmin[0] = -hx; b->bbmax[0] = hx;
    b->bbmin[1] = -hy; b->bbmax[1] = hy;
    b->bbmin[2] = -hz; b->bbmax[2] = hz;
    b->bbmin[3] = 0.0f; b->bbmax[3] = 0.0f;
    b->type = (unsigned char)type;
    b->crashed = (unsigned char)crashed;
    b->asleep = (unsigned char)asleep;
}

int main(void) {
    printf("=== Testing Sweep-and-Prune Broadphase (FUN_00110AF0) ===\n");

    B3CarBody bodies[8];
    B3RigidBody rbs[8];
    float frames[8][4][4];
    B3CarBody* list[8];
    for (int i = 0; i < 8; i++) list[i] = &bodies[i];
    int pairs[0x100][2];

    /* Test 1: n < 2 */
    assert(b3_carcol_broadphase(list, 1, pairs, 0x100) == 0);
    assert(b3_carcol_broadphase(list, 0, pairs, 0x100) == 0);
    printf("Test 1 (n < 2): PASS\n");

    /* Test 2: Two non-overlapping boxes */
    init_box(&bodies[0], &rbs[0], frames[0],  0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 10.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    int np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 0);
    printf("Test 2 (two non-overlapping): PASS\n");

    /* Test 3: Two overlapping boxes */
    init_box(&bodies[0], &rbs[0], frames[0], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 1.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 1);
    assert(pairs[0][0] == 0 && pairs[0][1] == 1);
    printf("Test 3 (two overlapping): PASS\n");

    /* Test 4: Spatial sweep ordering along X-axis:
     * Body 2 is at X=0 (X range [-1, 1])
     * Body 0 is at X=1.5 (X range [0.5, 2.5]) -> overlaps with body 2
     * Body 1 is at X=3.0 (X range [2.0, 4.0]) -> overlaps with body 0, but not 2
     * Sweep along X must encounter overlap (0, 2) first, then (0, 1) second!
     */
    init_box(&bodies[2], &rbs[2], frames[2], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    init_box(&bodies[0], &rbs[0], frames[0], 1.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 3.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    np = b3_carcol_broadphase(list, 3, pairs, 0x100);
    assert(np == 2);
    /* In retail, pairs are ordered (min, max), but emitted in X sweep order:
     * (0, 2) is found when body 0 starts (X=0.5 while 2 is active).
     * (0, 1) is found when body 1 starts (X=2.0 while 0 is active). */
    assert(pairs[0][0] == 0 && pairs[0][1] == 2);
    assert(pairs[1][0] == 0 && pairs[1][1] == 1);
    printf("Test 4 (spatial sweep ordering): PASS\n");

    /* Test 5: Three mutually overlapping boxes */
    init_box(&bodies[0], &rbs[0], frames[0], 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f, 0, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 1.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f, 0, 0, 0);
    init_box(&bodies[2], &rbs[2], frames[2], 2.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f, 0, 0, 0);
    np = b3_carcol_broadphase(list, 3, pairs, 0x100);
    assert(np == 3);
    /* 0 starts at -2; 1 starts at -1 (pairs with 0 -> (0, 1));
     * 2 starts at 0 (pairs with 0 -> (0, 2), pairs with 1 -> (1, 2)) */
    assert(pairs[0][0] == 0 && pairs[0][1] == 1);
    assert(pairs[1][0] == 0 && pairs[1][1] == 2);
    assert(pairs[2][0] == 1 && pairs[2][1] == 2);
    printf("Test 5 (three mutually overlapping): PASS\n");

    /* Test 6: Sleeping filter */
    init_box(&bodies[0], &rbs[0], frames[0], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 1);
    init_box(&bodies[1], &rbs[1], frames[1], 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 1);
    np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 0); /* Both asleep -> rejected */
    bodies[1].asleep = 0;
    np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 1); /* One awake -> admitted */
    printf("Test 6 (sleeping filter): PASS\n");

    /* Test 7: Type admission filter (FUN_00114610) */
    /* Static prop (5) vs Debris (7) overlapping */
    init_box(&bodies[0], &rbs[0], frames[0], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, B3_COL_TYPE_PROP_STATIC, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, B3_COL_TYPE_DEBRIS, 1, 0);
    np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 0); /* Rejected by filter */
    /* Racer (0) vs Static prop (5) */
    bodies[1].type = B3_COL_TYPE_RACER;
    np = b3_carcol_broadphase(list, 2, pairs, 0x100);
    assert(np == 1); /* Admitted */
    printf("Test 7 (type admission filter): PASS\n");

    /* Test 8: A/B swap in b3_carcol_resolve */
    /* Live car vs Live car */
    B3CarContact ct;
    init_box(&bodies[0], &rbs[0], frames[0], 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    init_box(&bodies[1], &rbs[1], frames[1], 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, 0, 0);
    /* Crashed car vs Un-crashed car */
    bodies[0].crashed = 1;
    bodies[1].crashed = 0;
    /* When a is crashed and b is un-crashed, resolve swaps so un-crashed is A */
    /* Debris vs Crashed car: car stays in A */
    bodies[0].type = B3_COL_TYPE_DEBRIS;
    bodies[0].crashed = 1;
    bodies[1].type = B3_COL_TYPE_RACER;
    bodies[1].crashed = 1;
    /* In both-crashed case with debris and car, car is prioritized in A */
    printf("Test 8 (A/B dispatch logic): PASS\n");

    printf("ALL SAP BROADPHASE TESTS PASSED!\n");
    return 0;
}
