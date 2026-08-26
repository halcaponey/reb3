// RUNTIME per-track traffic data.
//
// Replaces the compiled-in tables of src/burnout3_traffic_data.h, which
// tools/extract_traffic.py regenerates for whatever `--track` last ran and
// which the repo pins to US_C3_V1.  Every consumer name below keeps its old
// spelling, so call sites are unchanged -- only the STORAGE moves from a
// const array chosen at compile time to an array filled at startup from
//
//     build/tracks/<B3_TRACK>/traffic.bin        ('B3TR', version 4)
//
// which tools/extract_traffic.py already writes for all 36 shipped tracks
// (same pipeline as route.bin / collision.bin / grid.bin / traffic_paths.bin).
// No per-track constant appears in this file: the caps below are format
// limits, not track data, and the loader takes the track id from B3_TRACK.
//
// WHY: the compiled-in set is US_C3_V1's.  On every other track the model
// list, the oncoming line, the spawn seeds and the lane cross-section all
// describe a DIFFERENT WORLD -- the pinned oncoming polyline lies 1.0-8.9 km
// away from the track the player is actually driving -- and the string match
// that binds traffic_paths.bin's per-track spawn policy to a model resolved
// 246 of 400 mix entries to "no such car", leaving recycled pool agents
// wearing whatever model the slot's previous occupant had.
//
// The six list slots are vehicle CATEGORIES, not one flat set; slot 5 holds
// the .btv models with no front axle (rear bogie only), so a slot-4 tractor
// plus a slot-5 trailer is one articulated unit.                        [C]
// (Provenance for the .bgd walk itself: see burnout3_traffic_data.h and
// tools/extract_traffic.py -- unchanged by this file.)

#ifndef BURNOUT3_TRAFFIC_RUNTIME_H
#define BURNOUT3_TRAFFIC_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B3_TRAFFIC_CAT_COMPACT 0
#define B3_TRAFFIC_CAT_LIGHT   1
#define B3_TRAFFIC_CAT_BUS     2
#define B3_TRAFFIC_CAT_TRUCK   3
#define B3_TRAFFIC_CAT_TRACTOR 4
#define B3_TRAFFIC_CAT_TRAILER 5
#define B3_TRAFFIC_CAT_SPECIAL 6

// Format caps, sized from the widest shipped track (max observed across all
// 36 traffic.bin: 12 cars, 156 spawn seeds, 8 lanes).  Generic headroom, not
// a per-track constant; the loader rejects anything larger rather than
// truncating silently.
#define B3_TRAFFIC_CAR_MAX    32
#define B3_TRAFFIC_SPAWN_MAX  512
#define B3_TRAFFIC_LANE_MAX   32

// One record of traffic.bin's car table.  Char arrays rather than pointers so
// the whole 72-byte record reads straight off the file; every consumer used
// `.id` / `.cls` / `.car` as a `const char*`, which an array still decays to.
typedef struct {
    char  id[16];          // vlist/base-40 id (matches B3_CAR_PHYSICS)
    char  cls[8];          // pveh/<class>/
    char  car[16];         // file base, <car>.btv; mesh build/cars/<cls>_<car>.obj
    int   cat;             // B3_TRAFFIC_CAT_*, = the source list slot
    int   kingpin_spring;  // trailer +0x16BC == 1 branch            [C]
    float tow_anchor[3];   // tractor model+0x16A8 fifth wheel       [C]
    float king_anchor[3];  // trailer model+0x16A4 kingpin           [C]
} B3TrafficCarId;

typedef struct { float lat; int dir; } B3TrafficLane;

typedef struct {
    int            loaded;
    char           track[64];
    B3TrafficCarId cars[B3_TRAFFIC_CAR_MAX];
    int            car_count;
    int            special_count;   // the last `special_count` cars are specials
    float          spawn[B3_TRAFFIC_SPAWN_MAX][6];   // pos xyz, dir xyz
    int            spawn_count;
    float        (*oncoming)[3];    // malloc'd: the corridor-hugging drive line
    int            oncoming_count;
    B3TrafficLane  lanes[B3_TRAFFIC_LANE_MAX + 1];   // +1 sentinel, as before
    int            lane_count;
} B3TrafficData;

static B3TrafficData g_traffic_data = {0};

// The old spellings, now resolving to runtime storage.
#define B3_TRAFFIC_CARS        (g_traffic_data.cars)
#define B3_TRAFFIC_CAR_COUNT   (g_traffic_data.car_count)
#define B3_TRAFFIC_SPAWN       (g_traffic_data.spawn)
#define B3_TRAFFIC_SPAWN_COUNT (g_traffic_data.spawn_count)
#define B3_TRAFFIC_LANES       (g_traffic_data.lanes)
#define B3_TRAFFIC_LANE_COUNT  (g_traffic_data.lane_count)
#define B3_ONCOMING            ((const float (*)[3])g_traffic_data.oncoming)
#define B3_ONCOMING_COUNT      (g_traffic_data.oncoming_count)

// The oncoming polyline is optional per track (AS_M1_V1 ships zero points):
// every route_*/lane_point helper that walks it must be able to ask first.
#define B3_ONCOMING_USABLE     (g_traffic_data.oncoming_count >= 2)

static void b3_traffic_data_free(void) {
    free(g_traffic_data.oncoming);
    memset(&g_traffic_data, 0, sizeof(g_traffic_data));
}

/* build/tracks/<id>/traffic.bin, 'B3TR' version 4 (tools/extract_traffic.py):
 *   +0x00 char[4] 'B3TR'   +0x04 u32 version
 *   +0x08 u32 car_count    +0x0C u32 spawn_count   +0x10 u32 oncoming_count
 *   +0x14 u32 special_count                        +0x18 u32 lane_count
 *   +0x1C car_count   x 72-byte B3TrafficCarId record (NUL padded)
 *         spawn_count x f32[6]
 *         oncoming_count x f32[3]
 *         lane_count  x { f32 lat; i32 dir }
 * Z is already negated by the extractor (harness GL space, RE_NOTES 12). */
static int b3_traffic_data_load(void) {
    struct { char magic[4]; unsigned int version, car_count, spawn_count,
             oncoming_count, special_count, lane_count; } h;
    const char* track = getenv("B3_TRACK");
    char path[256];
    FILE* f;
    int i;

    if (!track) track = getenv("B3_POSTFX_TRACK");
    if (!track) track = "US_C3_V1";

    b3_traffic_data_free();
    snprintf(g_traffic_data.track, sizeof g_traffic_data.track, "%s", track);
    snprintf(path, sizeof path, "build/tracks/%s/traffic.bin", track);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Burnout3] traffic: no %s -- traffic disabled\n", path);
        return 0;
    }
    if (fread(&h, sizeof h, 1, f) != 1
        || memcmp(h.magic, "B3TR", 4) != 0
        || h.version != 4
        || h.car_count == 0 || h.car_count > B3_TRAFFIC_CAR_MAX
        || h.spawn_count > B3_TRAFFIC_SPAWN_MAX
        || h.lane_count > B3_TRAFFIC_LANE_MAX
        || h.special_count > h.car_count
        || h.oncoming_count > 1000000u) {
        fprintf(stderr, "[Burnout3] traffic: %s is not a usable B3TR v4 asset\n",
                path);
        fclose(f);
        return 0;
    }
    /* The record is read as one block, so a silent layout drift between the
     * extractor and this struct would corrupt every field at once. */
    if (sizeof(B3TrafficCarId) != 72) {
        fprintf(stderr, "[Burnout3] traffic: B3TrafficCarId is %d bytes, "
                "expected 72\n", (int)sizeof(B3TrafficCarId));
        fclose(f);
        return 0;
    }
    if (fread(g_traffic_data.cars, sizeof(B3TrafficCarId), h.car_count, f)
        != h.car_count)
        goto truncated;
    for (i = 0; i < (int)h.car_count; i++) {
        g_traffic_data.cars[i].id[15]  = '\0';
        g_traffic_data.cars[i].cls[7]  = '\0';
        g_traffic_data.cars[i].car[15] = '\0';
    }
    if (h.spawn_count
        && fread(g_traffic_data.spawn, 6 * sizeof(float), h.spawn_count, f)
           != h.spawn_count)
        goto truncated;
    if (h.oncoming_count) {
        g_traffic_data.oncoming = malloc((size_t)h.oncoming_count
                                         * 3 * sizeof(float));
        if (!g_traffic_data.oncoming) goto truncated;
        if (fread(g_traffic_data.oncoming, 3 * sizeof(float),
                  h.oncoming_count, f) != h.oncoming_count)
            goto truncated;
    }
    if (h.lane_count
        && fread(g_traffic_data.lanes, sizeof(B3TrafficLane), h.lane_count, f)
           != h.lane_count)
        goto truncated;
    fclose(f);

    g_traffic_data.car_count      = (int)h.car_count;
    g_traffic_data.special_count  = (int)h.special_count;
    g_traffic_data.spawn_count    = (int)h.spawn_count;
    g_traffic_data.oncoming_count = (int)h.oncoming_count;
    g_traffic_data.lane_count     = (int)h.lane_count;
    /* The trailing sentinel the old compiled table carried. */
    g_traffic_data.lanes[g_traffic_data.lane_count].lat = 0.0f;
    g_traffic_data.lanes[g_traffic_data.lane_count].dir = 0;
    g_traffic_data.loaded = 1;

    printf("[Burnout3] traffic data: %s -- %d cars (%d special), %d spawn "
           "seeds, %d oncoming pts, %d lanes\n",
           track, g_traffic_data.car_count, g_traffic_data.special_count,
           g_traffic_data.spawn_count, g_traffic_data.oncoming_count,
           g_traffic_data.lane_count);
    return 1;

truncated:
    fprintf(stderr, "[Burnout3] traffic: %s truncated\n", path);
    fclose(f);
    b3_traffic_data_free();
    return 0;
}

#endif // BURNOUT3_TRAFFIC_RUNTIME_H
