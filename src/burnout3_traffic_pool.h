#ifndef BURNOUT3_TRAFFIC_POOL_H
#define BURNOUT3_TRAFFIC_POOL_H

/* [C] FUN_001A3EA0 @0x001A3F2B/@0x001A3F60: retail's free list is built by
 * two `while (i < 0xFE)` loops -- 254 physical bodies.  The port's 64 was a
 * harness guess; with retail's direction-split population law (spawn and
 * retire as separate requests, ~4 windows apart) the pool depth IS the live
 * density, and 64 starved the road to the point that a 90 s race met almost
 * no traffic at all. */
#define B3_TRAFFIC_POOL_MAX 254

typedef struct {
    int physical_head;
    int physical_tail;
    int agent_head;
    int physical_next[B3_TRAFFIC_POOL_MAX];
    int agent_next[B3_TRAFFIC_POOL_MAX];
    unsigned char physical_live[B3_TRAFFIC_POOL_MAX];
    unsigned char agent_live[B3_TRAFFIC_POOL_MAX];
    int physical_count;
    int agent_count;
} B3TrafficPool;

void b3_traffic_pool_init(B3TrafficPool* pool, int physical_count,
                          int agent_count);
/* Retail FUN_001A38F0: singly linked list pop from manager+0x36364 */
int b3_traffic_pool_acquire_physical(B3TrafficPool* pool);
/* Retail FUN_001A41A0: singly linked FIFO queue append to manager+0x36368 */
int b3_traffic_pool_release_physical(B3TrafficPool* pool, int physical_slot);
/* Retail FUN_001A3A10: singly linked LIFO pop from manager+0x3636C */
int b3_traffic_pool_acquire_agent(B3TrafficPool* pool);
/* Retail FUN_001A3A80: singly linked LIFO push to manager+0x3636C */
int b3_traffic_pool_release_agent(B3TrafficPool* pool, int agent_slot);
/* Retail FUN_001A2B20: dual acquisition with rollback on agent failure */
int b3_traffic_pool_acquire(B3TrafficPool* pool, int* physical_slot,
                            int* agent_slot);
/* Retail FUN_001A75A0 / FUN_001A3970: release physical body and/or road agent */
int b3_traffic_pool_release(B3TrafficPool* pool, int physical_slot,
                            int agent_slot);

#endif
