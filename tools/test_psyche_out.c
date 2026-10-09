#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "burnout3_td_rules.h"
#include "burnout3_backend.h"
#include "burnout3_crash.h"

/* Stub dependencies for td_rules unit testing */
B3Backend b3_backend_get(B3Feature f) {
    (void)f;
    return (B3Backend)0;
}
int b3_crash_wall_eval(const B3CrashVehicle* v, const float pt[4], const float wall_n[4], B3CrashWallEval* out) {
    (void)v; (void)pt; (void)wall_n; (void)out; return 0;
}
void b3_crash_apply_impulse(B3CrashVehicle* v, const float imp[4], const float pt[4]) {
    (void)v; (void)imp; (void)pt;
}
int b3_emu_td_slam(const void* R, float clock, int a, int v, int kind, float strength, void* out) {
    (void)R; (void)clock; (void)a; (void)v; (void)kind; (void)strength; (void)out; return 0;
}

int main(void) {
    printf("=== Testing Psyche Out Mechanics (FUN_001959A0) ===\n");

    B3TdRules R;
    b3_td_reset(&R, 2);

    /* Slot 0: Human Player (stalker)
     * Slot 1: AI Racer (target) */
    b3_td_set_car(&R, 0, 0, 0); /* cls 0 = human, grid 0 */
    b3_td_set_car(&R, 1, 1, 1); /* cls 1 = AI,    grid 1 */

    R.car[0].speed_ms = 30.0f; /* 67.1 mph >= 60 mph */
    R.car[1].speed_ms = 30.0f; /* 67.1 mph >= 60 mph */

    float pos[2][3] = {
        { 0.0f, 0.0f, 0.0f },  /* Car 0 at (0, 0, 0) */
        { 0.0f, 0.0f, 10.0f }  /* Car 1 at (0, 0, 10), directly 10m ahead */
    };

    float fwd[2][3] = {
        { 0.0f, 0.0f, 1.0f },  /* Car 0 facing +Z */
        { 0.0f, 0.0f, 1.0f }   /* Car 1 facing +Z */
    };

    /* -------------------------------------------------------------
     * Test 1: Stalker behind target, initial lock
     * ------------------------------------------------------------- */
    float clock = 10.0f;
    float dt = 0.016f;

    b3_td_tailgate_update(&R, clock, dt, pos, fwd);
    assert(R.car[0].psyche_target == 1);
    assert(R.car[1].psyche_armed == 0); /* Not armed yet (< 0.5s) */
    printf("Test 1 (Initial tracking lock): PASS\n");

    /* -------------------------------------------------------------
     * Test 2: Arming after 0.5s of tailgating
     * ------------------------------------------------------------- */
    /* Step for 0.5s */
    for (int frame = 0; frame < 32; frame++) {
        clock += dt;
        b3_td_tailgate_update(&R, clock, dt, pos, fwd);
    }
    assert(R.car[0].psyche_target == 1);
    assert(R.car[1].psyche_armed == 1); /* Now armed! */

    /* Verify authority floor on armed AI car */
    B3TdAuthority auth;
    b3_td_crash_authority_full(&R, 1, clock, &auth);
    assert(auth.value == B3_TDR_AUTHORITY_FLOOR);
    assert(auth.crash_ok == 1);
    printf("Test 2 (Arming at 0.5s & authority floor): PASS\n");

    /* -------------------------------------------------------------
     * Test 3: Cone angle gate - target turning away outside 15 deg
     * ------------------------------------------------------------- */
    /* If stalker veers off so cone angle > 15 deg (e.g. 30 deg offset) */
    pos[0][0] = 6.0f; /* dx = -6, dz = 10 -> angle = atan(6/10) = 31 deg > 15 deg */
    b3_td_tailgate_update(&R, clock, dt, pos, fwd);

    /* During grace period (dt = 0.016s <= 0.3s), target remains armed */
    assert(R.car[1].psyche_armed == 1);
    assert(R.car[0].psyche_target == 1);
    printf("Test 3a (Grace timer active under 0.3s): PASS\n");

    /* Advance clock past 0.3s grace period */
    for (int frame = 0; frame < 25; frame++) {
        clock += dt;
        b3_td_tailgate_update(&R, clock, dt, pos, fwd);
    }
    /* Grace expired: target disarmed */
    assert(R.car[1].psyche_armed == 0);
    assert(R.car[0].psyche_target == -1);
    printf("Test 3b (Disarmed after grace expired > 0.3s): PASS\n");

    /* -------------------------------------------------------------
     * Test 4: Re-arming and Psyche Out Takedown Commit
     * ------------------------------------------------------------- */
    /* Move stalker back into position */
    pos[0][0] = 0.0f;
    for (int frame = 0; frame < 35; frame++) {
        clock += dt;
        b3_td_tailgate_update(&R, clock, dt, pos, fwd);
    }
    assert(R.car[0].psyche_target == 1);
    assert(R.car[1].psyche_armed == 1);

    /* AI car 1 crashes into wall */
    B3TdCause cause;
    b3_td_cause_wall(&cause, 0);
    b3_td_on_crash(&R, clock, 1, &cause, pos);

    /* Verify claim recorded with psyche flag */
    assert(R.car[0].claim[1] == clock);
    assert(R.car[0].claim_psyche[1] == 1);

    /* Commit the takedown */
    B3TdEvent ev[4];
    clock += B3_TDR_CLEAR_WAIT_S + 0.01f;
    int n = b3_td_frame(&R, clock, 0, ev, 4);

    assert(n == 1);
    assert(ev[0].kind == B3_TDE_TAKEDOWN);
    assert(ev[0].attacker == 0);
    assert(ev[0].victim == 1);
    assert(ev[0].message == B3_TDR_MSG_PSYCHE_OUT); /* 0xA9 "PSYCHE OUT!" */
    assert(ev[0].bp == B3_TDR_BP_PSYCHE_OUT);        /* 150 BP */
    printf("Test 4 (Psyche Out Takedown commit: 0xA9 msg & +150 BP): PASS\n");

    /* -------------------------------------------------------------
     * Test 5: Target crashed clears tracking
     * ------------------------------------------------------------- */
    R.car[1].crashed = 1;
    b3_td_tailgate_update(&R, clock, dt, pos, fwd);
    assert(R.car[0].psyche_target == -1);
    printf("Test 5 (Target crashed clears stalker tracking): PASS\n");

    printf("\nAll 5 Psyche Out tests PASSED successfully!\n");
    return 0;
}
