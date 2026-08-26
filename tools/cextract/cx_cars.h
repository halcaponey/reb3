/* cx_cars.h -- private contract for the CAR / VEHICLE extractor family.
 *
 * Agent D's half of tools/cextract: the C11 port of
 *
 * Paths below are relative to `out_root`, a REPO-ROOT STAND-IN (the
 * project-wide convention), so the asset tree matches the python's own:
 *
 *     tools/extract_bgv.py            .bgv meshes  -> build/cars .obj+.wheels
 *                                                     + .panels + parts/
 *     tools/extract_bgv_textures.py   paint pages  -> build/cars _p<K>.png
 *     tools/extract_traffic_lights.py .btv coronas -> build/cars .lights
 *     tools/extract_vehicles.py       roster       -> gen/burnout3_vehicle_data.h
 *     tools/extract_car_vdb.py        tuning       -> gen/burnout3_car_physics.h
 *     tools/extract_traffic.py        (the vehicle-asset half only:
 *         export_assets()/extract_btv() -> <track>/cars + the build/cars mirror)
 *
 * Everything in here is PRIVATE to that family (prefix `cxd_`); the public
 * stage entry points live in cx_extract.h's "agent D: cars" block.
 *
 * plus tools/extract_physics_params.py -> gen/burnout3_physics_params.h,
 * which is the one module that mines Ghidra rather than the game files.
 *
 * THE PYTHON IS THE SPEC.  Byte identity with the tools above is the
 * acceptance gate, quirks included; the reproduced quirks are enumerated
 * Q1..Q26 in the module headers -- cx_cars_bgv.c Q1-Q9, cx_cars_paint.c Q10,
 * cx_cars_roster.c Q11-Q12, cx_cars_traffic.c Q13-Q17, cx_cars_vdb.c
 * Q18-Q21, cx_cars_physparams.c Q22-Q26.  Nothing here invents format
 * knowledge: every offset is carried over with the python's own [C] citation
 * attached.
 */
#ifndef CX_CARS_H
#define CX_CARS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ paths */
/* tools/extract_bgv.py PVEH / tools/extract_traffic_lights.py B3_GAME_DIR. */
/* NO RETAIL PATH IS COMPILED IN.  This tree ships no game content: point
 * $B3_GAME_ROOT at YOUR OWN dump -- the folder holding default.xbe, or an
 * .xiso image of it.  $B3_ISO and $B3_GAME_DIR name the same source and are
 * checked first.  See docs/ASSETS.md. */
#define CXD_GAME_DIR_DEFAULT ""

/* $B3_GAME_DIR, else CXD_GAME_DIR_DEFAULT. */
const char *cxd_game_dir(void);
/* $B3_REPO_DIR (what cx_main.c's --repo sets), else $B3_REPO_ROOT, else the
 * repo this file was built from.  Only the tuning stage needs it: the ELF
 * that carries the ValueDB CRC table and the physics-parameter header the
 * python tool parses back in both live there. */
const char *cxd_repo_root(void);

/* ------------------------------------------------------------------ blobs */
typedef struct {
    unsigned char *d;
    size_t         n;
} cxd_blob;

int  cxd_read_file(const char *path, cxd_blob *out);   /* 0 = ok */
void cxd_blob_free(cxd_blob *b);
int  cxd_mkdir_p(const char *path);

/* Concatenate a NULL-terminated list of pieces into `dst`, truncating safely.
 * Used instead of snprintf("%s/%s", ...) everywhere a path is built from two
 * runtime strings: gcc cannot bound a `char *` and warns
 * -Wformat-truncation on every such call, and the build must stay clean. */
void cxd_path(char *dst, size_t cap, const char *first, ...);

/* Unchecked little-endian field reads; every call site range-checks first,
 * exactly where struct.unpack_from would have raised. */
uint8_t  cxd_u8 (const cxd_blob *b, size_t o);
int8_t   cxd_i8 (const cxd_blob *b, size_t o);
uint16_t cxd_u16(const cxd_blob *b, size_t o);
uint32_t cxd_u32(const cxd_blob *b, size_t o);
uint64_t cxd_u64(const cxd_blob *b, size_t o);
float    cxd_f32(const cxd_blob *b, size_t o);

/* ------------------------------------------------------- directory listing */
typedef struct {
    char **v;
    int    n, cap;
} cxd_names;

void cxd_names_free(cxd_names *l);
/* Every entry of `dir` whose name ends in `suffix` (case-insensitive, NULL =
 * all), sorted the way python's sorted(os.listdir()) sorts: by code point,
 * i.e. plain byte order.  `dirs_only` keeps only subdirectories. */
int  cxd_list_dir(const char *dir, const char *suffix, int dirs_only,
                  cxd_names *out);
/* tools/extract_vehicles.py car_sort_key(): (int(all digits in the stem), stem). */
void cxd_sort_car_key(cxd_names *l);

/* ------------------------------------------------------------ .bgv / .btv */
/* record: 0x1C bytes at rec, +0x0C index offset rel record, +0x10 u16 INDEX
 * COUNT [C deep-traced FUN_00031AB0 -- a count, never a byte size], +0x18 u16
 * mask, +0x1A u8 texture slot. */
typedef struct {
    uint16_t  mask;
    uint8_t   tex;
    int       ntris;
    uint16_t *tris;          /* 3 * ntris */
} cxd_rec;

/* A part object: s8 record count at +0, u32 record-array offset rel part at
 * +4.  `valid` == 0 is python's `None` (the whole part is rejected); a valid
 * part may still hold zero records, which python renders as a falsy []. */
typedef struct {
    cxd_rec *rec;
    int      n;
    int      valid;
} cxd_part;

/* One LOD section (18-slot part table, relinked by FUN_00031010). */
typedef struct {
    int64_t  pool;
    cxd_part body;                   /* the embedded one-piece car, S+0x60 */
    cxd_part slots[10];              /* 0 aperture body, 1..6 panels, 7..9 wheel */
    int      slot_present[10];       /* python: key in the dict at all */
    int      maxidx;
    int      valid;
} cxd_section;

/* 0x18-byte vertex: f32[3] pos, NORMPACKED3 normal at +0x0C, f32[2] uv at
 * +0x10.  Stored as double because every emitted digit comes from python
 * double arithmetic. */
typedef struct {
    double x, y, z;
    double u, v;
    double nx, ny, nz;
} cxd_vert;

void cxd_part_free(cxd_part *p);
void cxd_section_free(cxd_section *s);

/* tools/extract_bgv.py parse_part / parse_section / read_verts. */
void cxd_parse_part(const cxd_blob *d, int64_t P, cxd_part *out);
void cxd_parse_section(const cxd_blob *d, int64_t S, cxd_section *out);
/* count vertices from `pool`; caller has already proven the range. */
cxd_vert *cxd_read_verts(const cxd_blob *d, int64_t pool, int count);

/* The truthy slot lookup: python `sec['slots'].get(k)` used in a boolean
 * context -- NULL unless the key exists, parsed, and holds >= 1 record. */
const cxd_part *cxd_slot(const cxd_section *s, int k);

/* Pick the section with the most body triangles across LOD 0..4, python's
 * strictly-greater scan (first maximum wins).  Returns 0 on success. */
int cxd_best_section(const cxd_blob *d, cxd_section *out);

/* ---------------------------------------------------------------- OBJ out */
/* One `o <name>` span.  tex < 0 reproduces python's legacy 2-tuple group,
 * which emits no `usemtl` line at all (the .btv exporter still uses it). */
typedef struct {
    char           name[64];
    int            tex;
    const cxd_rec *rec;              /* triangles come from here */
} cxd_group;

/* tools/extract_bgv.py write_obj().  Returns 1 if a file was written, 0 if
 * no vertex was referenced (python returns False and writes nothing). */
int cxd_write_obj(const char *path, const cxd_vert *verts, int nverts,
                  const cxd_group *groups, int ngroups,
                  const char *header_note);

/* --------------------------------------------------------------- .wheels  */
/* +0x18 f32 radius, +0xB80 4x4 f32 x numWheels stride 0x40 rows
 * Right/Up/At/Pos [C: FUN_0012FEE0]. */
typedef struct {
    double pos[3];
    int    mirror;
} cxd_wheel;

int cxd_read_wheels(const cxd_blob *d, double *radius_out,
                    cxd_wheel *out, int cap);   /* -> count */

/* ------------------------------------------------------------ paint pages */
/* tools/extract_bgv_textures.py extract_textures(): decode every palette
 * variant of the .bgv/.btv's own texture record (header +0x60).  On success
 * *images is a malloc'd array of `*count` RGBA8 buffers of w*h*4 bytes.
 * Returns 0, or -1 with *err pointing at the python's own message. */
int cxd_extract_paint(const cxd_blob *d, char *name_out, size_t name_cap,
                      int *w_out, int *h_out,
                      unsigned char ***images, int *count,
                      const char **err);
void cxd_free_paint(unsigned char **images, int count);

/* ------------------------------------------------------------- formatting */
/* python repr() of a float: the shortest string that round-trips, with a
 * ".0" forced onto integral values.  Only reachable through the VDB sanity
 * gate's `"%r" % (gears,)` message. */
void cxd_py_repr_double(double v, char *out, size_t cap);

#endif /* CX_CARS_H */
