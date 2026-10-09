#include "burnout3_traffic_pool.h"

#include <stdio.h>

static int expect_pair(B3TrafficPool* pool, int physical, int agent) {
    int got_physical = -1;
    int got_agent = -1;
    return b3_traffic_pool_acquire(pool, &got_physical, &got_agent)
        && got_physical == physical && got_agent == agent;
}

int main(void) {
    B3TrafficPool pool;
    int physical, agent;

    b3_traffic_pool_init(&pool, 3, 3);
    if (!expect_pair(&pool, 0, 0) || !expect_pair(&pool, 1, 1)) return 1;
    if (!b3_traffic_pool_release(&pool, 0, 0)) return 1;
    if (!expect_pair(&pool, 2, 0)) return 1;
    if (!b3_traffic_pool_release(&pool, 1, 1)
        || !b3_traffic_pool_release(&pool, 2, 0)) return 1;
    if (!expect_pair(&pool, 0, 0) || !expect_pair(&pool, 1, 1)
        || !expect_pair(&pool, 2, 2)) return 1;
    if (b3_traffic_pool_acquire(&pool, &physical, &agent)) return 1;
    if (!b3_traffic_pool_release(&pool, 0, 0)
        || b3_traffic_pool_release(&pool, 0, 0)) return 1;

    /* Test granular physical (trailer) vs agent operations and rollback */
    b3_traffic_pool_init(&pool, 2, 1);
    /* Pop tractor + agent */
    if (!b3_traffic_pool_acquire(&pool, &physical, &agent)) return 1;
    if (physical != 0 || agent != 0) return 1;
    /* Pop trailer physical body only (FUN_001A2B20 / 0x001A2E03) */
    int trailer = b3_traffic_pool_acquire_physical(&pool);
    if (trailer != 1) return 1;
    /* Agent stack is now empty: next dual acquire must fail and rollback without consuming physical */
    if (b3_traffic_pool_acquire(&pool, &physical, &agent)) return 1;
    /* Release trailer physical only (FUN_001A75A0 trailer recursion / 0x001A3970) */
    if (!b3_traffic_pool_release_physical(&pool, trailer)) return 1;
    /* Release tractor physical and agent */
    if (!b3_traffic_pool_release(&pool, 0, 0)) return 1;
    /* After releasing trailer (1) then tractor (0), physical FIFO order is 1, then 0 */
    if (b3_traffic_pool_acquire_physical(&pool) != 1) return 1;
    if (b3_traffic_pool_acquire_physical(&pool) != 0) return 1;
    if (b3_traffic_pool_acquire_physical(&pool) != -1) return 1;

    puts("traffic physical FIFO and road-agent LIFO pool lifecycle: OK");
    return 0;
}
