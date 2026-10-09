#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "burnout3_vehicle_sim.h"

int b3_ground_probe(float x, float y, float z, float* h, float n[3]) {
    (void)x; (void)y; (void)z; (void)h; (void)n; return -1;
}

int main(void) {
    printf("=== Testing Crash Director Zone Staging (FUN_0017D0F0 / FUN_0018BC90) ===\n");

    B3VehicleFull v1, v2;
    memset(&v1, 0, sizeof(v1));
    memset(&v2, 0, sizeof(v2));

    float frame1[4][4], frame2[4][4];
    b3_rigid_body_bind_frame(&v1.rb, frame1);
    b3_rigid_body_bind_frame(&v2.rb, frame2);
    frame1[0][0] = frame1[1][1] = frame1[2][2] = frame1[3][3] = 1.0f;
    frame2[0][0] = frame2[1][1] = frame2[2][2] = frame2[3][3] = 1.0f;

    /* Vehicle 1 at position (10, 0, 0) */
    frame1[3][0] = 10.0f; frame1[3][1] = 0.0f; frame1[3][2] = 0.0f;

    /* Vehicle 2 at position (100, 0, 0) */
    frame2[3][0] = 100.0f; frame2[3][1] = 0.0f; frame2[3][2] = 0.0f;

    B3VehicleFull* vehs[2] = { &v1, &v2 };

    /* Test 1: num_zones == 0 leaves flags_1351 == 0 (no synthesized records) */
    b3_crash_director_update_zones(vehs, 2, NULL, 0);
    assert(v1.flags_1351 == 0);
    assert(v2.flags_1351 == 0);
    printf("Test 1 (num_zones == 0 retains 0): PASS\n");

    /* Define sample zones */
    B3CrashDirectorZone zones[2];
    memset(zones, 0, sizeof(zones));

    /* Zone 0 at (0, 0, 0), zone_type = 2 (mode 1) */
    zones[0].center[0] = 0.0f; zones[0].center[1] = 0.0f; zones[0].center[2] = 0.0f;
    zones[0].zone_type = 2;
    /* 6 boundary points */
    for (int i = 0; i < 6; i++) {
        zones[0].points[i][0] = (float)i;
        zones[0].points[i][1] = 1.0f;
        zones[0].points[i][2] = 2.0f;
        zones[0].points[i][3] = 1.0f;
    }
    /* 3 basis vectors */
    zones[0].basis[0][0] = 1.0f;
    zones[0].basis[1][1] = 1.0f;
    zones[0].basis[2][2] = 1.0f;

    /* Zone 1 at (95, 0, 0), zone_type = 1 (mode 0) */
    zones[1].center[0] = 95.0f; zones[1].center[1] = 0.0f; zones[1].center[2] = 0.0f;
    zones[1].zone_type = 1;
    for (int i = 0; i < 6; i++) {
        zones[1].points[i][0] = (float)(i + 10);
        zones[1].points[i][1] = 2.0f;
        zones[1].points[i][2] = 3.0f;
        zones[1].points[i][3] = 1.0f;
    }
    zones[1].basis[0][0] = 1.0f;
    zones[1].basis[1][1] = 1.0f;
    zones[1].basis[2][2] = 1.0f;

    /* Test 2: FUN_0017D0F0 directly */
    float min_dist = 50.0f;
    int staged = b3_crash_director_stage_zone(&v1, zones[0].points, zones[0].basis,
                                             zones[0].center, zones[0].zone_type, &min_dist);
    /* v1 at 10 to zone 0 at 0 has distance 10.0 < 50.0 */
    assert(staged == 1);
    assert(v1.flags_1351 == 1);
    assert(fabsf(min_dist - 10.0f) < 1e-4f);
    printf("Test 2 (FUN_0017D0F0 direct staging): PASS\n");

    /* Test 3: FUN_0017D0F0 distance rejection */
    /* Try staging zone 1 (distance 85.0) with min_dist 10.0 -> must be rejected */
    staged = b3_crash_director_stage_zone(&v1, zones[1].points, zones[1].basis,
                                          zones[1].center, zones[1].zone_type, &min_dist);
    assert(staged == 0);
    assert(fabsf(min_dist - 10.0f) < 1e-4f);
    printf("Test 3 (FUN_0017D0F0 distance rejection): PASS\n");

    /* Test 4: b3_vehicle_clear_crash_floor */
    b3_vehicle_clear_crash_floor(&v1);
    assert(v1.flags_1351 == 0);
    printf("Test 4 (b3_vehicle_clear_crash_floor): PASS\n");

    /* Test 5: Full FUN_0018BC90 multi-vehicle multi-zone selection */
    b3_crash_director_update_zones(vehs, 2, zones, 2);
    /* v1 is at 10.0, so closest to zone 0 (dist 10 vs 85) */
    assert(v1.flags_1351 == 1);
    /* v1 should have zone 0 geometry */
    assert(v1.crash_floor_poly[0].p0[0] == zones[0].points[5][0]);

    /* v2 is at 100.0, so closest to zone 1 (dist 5 vs 100) */
    assert(v2.flags_1351 == 1);
    /* v2 should have zone 1 geometry (mode 0: p0 is P4) */
    assert(v2.crash_floor_poly[0].p0[0] == zones[1].points[4][0]);
    printf("Test 5 (FUN_0018BC90 multi-vehicle selection): PASS\n");

    printf("ALL CRASH DIRECTOR ZONE TESTS PASSED!\n");
    return 0;
}
