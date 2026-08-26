// RUNTIME per-car PHYSICS OVERRIDES.
//
// Replaces the compiled-in src/burnout3_car_physics.h -- 332 KB of the retail
// Data/vdb.xml baked into the C source, one B3CarParam array per car and a
// 100-row index over them.  Every consumer name below keeps its old spelling,
// so call sites are unchanged; only the STORAGE moves, from const arrays
// chosen at compile time to a table filled on first use from
//
//     build/cars/car_physics.bin                 ('B3CP', version 1)
//
// which tools/cextract's `car_tuning` stage writes alongside the header it
// still generates (tools/cextract/cx_cars_vdb.c, and the format block in
// tools/cextract/cx_extract.h).  Byte-for-byte agreement with the header it
// replaces is asserted by tools/validate_no_baked_data.py.
//
// This is GAME DATA -- the publisher's vehicle tuning, read out of the user's
// own vdb.xml -- so a user-supplies-assets distribution cannot compile it in
// any more than it can compile in a track's road.
//
// ============================================================ PROVENANCE [C]
// Unchanged from the header: the values are the raw dwords of the ValueDB's
// 8-byte {u32 value, i32 hash} default records reinterpreted as f32, keyed by
// the game's own registration hash -- key "<param><group>/../Export/ValueDB/
// VehiclePhysics/<VLIST-ID>.cfg", table-CRC at 0x001AF250 with SAR semantics
// over the 256 dwords at VA 0x003F7700.  `offset` is the same 0x1D0-struct
// offset burnout3_physics_params.h carries; apply with
// b3_config_set_by_offset().  Drivable cars carry all 64 params, traffic
// (.btv) cars the 9-param reduced set FUN_00134AC0 registers.  A car with no
// VDB overrides is ABSENT from the table and falls back to
// b3_physics_defaults() -- exactly as it did when the table was compiled in.
//
// ================================================================= PRECISION
// The header wrote each value with "%.9g", which round-trips a float exactly,
// so the asset stores the same f32 bits the compiler produced from those
// literals.  This one is bit-identical with no quantisation step (unlike
// roster.bin and font.bin, whose headers printed rounded decimals).
//
// ===================================================================== LOUD
// A missing car_physics.bin does not look missing: every car silently reverts
// to the compiled defaults and the whole fleet drives the same.  That is the
// precise failure the compiled-in table used to hide, so the loader says
// which file is absent and how to extract it, and stops.

#ifndef BURNOUT3_CAR_PHYSICS_RUNTIME_H
#define BURNOUT3_CAR_PHYSICS_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format caps, not fleet counts: retail ships 107 vehicles of which 100 carry
 * VDB overrides, and the widest car carries 64 params.  The loader refuses a
 * file that claims more rather than truncating silently. */
#define B3_CAR_PHYSICS_MAX        256
#define B3_CAR_PHYSICS_PARAM_MAX  32768

typedef struct {
    unsigned short offset;   // into the game's 0x1D0 physics struct
    float          value;
} B3CarParam;

typedef struct {
    const char*       id;         // vlist vehicle ID (base-40 decoded)
    const char*       class_code; // pveh/<class>/
    const char*       file;       // matches VehicleInfo.file
    const B3CarParam* params;
    int               n_params;   // 64 drivable / 9 traffic
} B3CarPhysics;

static struct {
    int          loaded;
    int          count;
    int          n_params;
    B3CarPhysics car[B3_CAR_PHYSICS_MAX];
    char         id[B3_CAR_PHYSICS_MAX][16];
    char         cls[B3_CAR_PHYSICS_MAX][8];
    char         file[B3_CAR_PHYSICS_MAX][12];
    B3CarParam   param[B3_CAR_PHYSICS_PARAM_MAX];
} g_car_physics;

static inline unsigned b3_cp_u32(const unsigned char* d) {
    return (unsigned)d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16)
         | ((unsigned)d[3] << 24);
}

static inline void b3_car_physics_fatal(const char* path, const char* why) {
    fprintf(stderr,
        "[Burnout3] FATAL: %s -- %s.\n"
        "  This is every car's Data/vdb.xml tuning; there is no compiled-in\n"
        "  copy to fall back to, and without it the whole fleet would drive\n"
        "  on b3_physics_defaults() and look like a physics bug.\n"
        "  Extract it from your own dump:  tools/cextract/build.sh && "
        "cxtract --all-global --out .\n"
        "  (or just this stage:  cxtract --only car_tuning --out .)\n",
        path, why);
    exit(2);
}

/* build/cars/car_physics.bin, 'B3CP' version 1 (tools/cextract/cx_cars_vdb.c):
 *   +0x00 char[4] 'B3CP'   +0x04 u32 version = 1
 *   +0x08 u32 car_count    +0x0C u32 param_total
 *   +0x10 car_count x 40-byte car record {char[16] id, char[8] class,
 *                                         char[12] file, u32 n_params}
 *   then  param_total x 8-byte {u16 offset, u16 pad, f32 value}
 * Loads once; FATAL rather than substituting anything. */
static inline int b3_car_physics_load(void) {
    const char* path = getenv("B3_CAR_PHYSICS_BIN");
    unsigned char hdr[16];
    unsigned ncar, nparam, i, seen = 0;
    FILE* f;

    if (g_car_physics.loaded) return g_car_physics.count;
    if (!path || !*path) path = "build/cars/car_physics.bin";
    g_car_physics.loaded = 1;

    f = fopen(path, "rb");
    if (!f) b3_car_physics_fatal(path, "no such file");
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "B3CP", 4) != 0
        || b3_cp_u32(hdr + 4) != 1u) {
        fclose(f);
        b3_car_physics_fatal(path, "not a B3CP version 1 asset");
    }
    ncar   = b3_cp_u32(hdr + 8);
    nparam = b3_cp_u32(hdr + 12);
    if (ncar == 0 || ncar > B3_CAR_PHYSICS_MAX
        || nparam > B3_CAR_PHYSICS_PARAM_MAX) {
        fclose(f);
        b3_car_physics_fatal(path, "car or param count outside the format cap");
    }
    for (i = 0; i < ncar; i++) {
        unsigned char rec[40];
        unsigned n;
        if (fread(rec, 1, 40, f) != 40) {
            fclose(f);
            b3_car_physics_fatal(path, "truncated in the car table");
        }
        memcpy(g_car_physics.id[i],   rec,      16);
        memcpy(g_car_physics.cls[i],  rec + 16,  8);
        memcpy(g_car_physics.file[i], rec + 24, 12);
        g_car_physics.id[i][15]   = '\0';   /* never trust a file to terminate */
        g_car_physics.cls[i][7]   = '\0';
        g_car_physics.file[i][11] = '\0';
        n = b3_cp_u32(rec + 36);
        if (n > 128 || seen + n > nparam) {
            fclose(f);
            b3_car_physics_fatal(path, "a car claims more params than the file "
                                       "holds");
        }
        g_car_physics.car[i].id         = g_car_physics.id[i];
        g_car_physics.car[i].class_code = g_car_physics.cls[i];
        g_car_physics.car[i].file       = g_car_physics.file[i];
        g_car_physics.car[i].params     = &g_car_physics.param[seen];
        g_car_physics.car[i].n_params   = (int)n;
        seen += n;
    }
    for (i = 0; i < nparam; i++) {
        unsigned char rec[8];
        unsigned bits;
        float v;
        if (fread(rec, 1, 8, f) != 8) {
            fclose(f);
            b3_car_physics_fatal(path, "truncated in the param blob");
        }
        bits = b3_cp_u32(rec + 4);
        memcpy(&v, &bits, 4);
        g_car_physics.param[i].offset = (unsigned short)(rec[0] | (rec[1] << 8));
        g_car_physics.param[i].value  = v;
    }
    fclose(f);
    g_car_physics.count    = (int)ncar;
    g_car_physics.n_params = (int)nparam;
    /* stderr, not stdout: the trajectory drivers under tools/ (dump_traj,
     * curb_traj) write MACHINE-READABLE JSON on stdout, and a diagnostic
     * line in the middle of it is a parse error, not a nuisance. */
    fprintf(stderr, "[Burnout3] car physics: %d cars, %d params from %s\n",
            g_car_physics.count, g_car_physics.n_params, path);
    return g_car_physics.count;
}

static inline const B3CarPhysics* b3_car_physics_table(void) {
    b3_car_physics_load();
    return g_car_physics.car;
}

static inline int b3_car_physics_count(void) {
    return b3_car_physics_load();
}

/* One car by its vlist id ("COMPCAR1"), or NULL.  The compiled-in header named
 * each car's array directly (B3_CARPARAMS_COMPCAR1), which a runtime table
 * cannot; this is the replacement for that spelling and is what the trajectory
 * drivers under tools/ use to seed their config. */
static inline const B3CarPhysics* b3_car_physics_find(const char* id) {
    int i, n = b3_car_physics_load();
    if (!id) return 0;
    for (i = 0; i < n; i++)
        if (strcmp(g_car_physics.car[i].id, id) == 0)
            return &g_car_physics.car[i];
    return 0;
}

/* The old spellings, now resolving to runtime storage.  B3_CAR_PHYSICS_COUNT
 * is no longer a compile-time constant -- it was never used as an array bound,
 * only as a loop limit, which is why the call sites are unchanged. */
#define B3_CAR_PHYSICS        (b3_car_physics_table())
#define B3_CAR_PHYSICS_COUNT  (b3_car_physics_count())

#endif /* BURNOUT3_CAR_PHYSICS_RUNTIME_H */
