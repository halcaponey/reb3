/* cx_common_a.h -- agent A's private helpers for the C asset extractor.
 *
 * Gamedata.bgd container walk + the geometry recovery that the Python
 * reference tools (tools/extract_bgd_paths.py, tools/extract_tlist.py)
 * perform.  Every symbol here is prefixed `cxa_` and is PRIVATE to the
 * agent-A modules (cx_tlist.c, cx_paths.c, cx_traffic.c, cx_nav_edges.c,
 * cx_grid.c); the public entry points live in cx_extract.h.
 *
 * Provenance, condensed from tools/extract_bgd_paths.py's docstring:
 *
 *   The game never maps Gamedata.bgd and never follows the two header
 *   pointers.  FUN_0018B250 is a 15-state streaming reader: it reads
 *   file[0..0x800], finds the event slot by comparing the packed 64-bit
 *   event id, then locates EVERYTHING ELSE through {size,offset} pairs
 *   inside that event's own 0x800-byte param record.  All offsets are FILE
 *   offsets.                                                            [C]
 *
 *     +0x008  u64[n]  event ids, base-40 packed        (0x0018B398)
 *     +0x198  u32[n]  event param-record file offsets  (0x0018B3C5)
 *     +0x260  u32     event COUNT                      (0x0018B37A)
 *
 *   event param record P (0x800 bytes, read at 0x0018B3EB):
 *     P+0x3B8        u32  lap count                     (RE_BGD 3)
 *     P+0x3BC/0x3C0  size/offset  event SPATIAL record  (0x0018B4D6/0x0018B4A1)
 *     P+0x3C4/0x3C8  size/offset  mode block "TDESC"    (0x0018B5BB/0x0018B569)
 *     P+0x3CC/0x3D0  size/offset  route-index section   (0x0018B643/0x0018B67B)
 *     P+0x3D4/0x3D8  size/offset  road-network section  (0x0018B71D/0x0018B755)
 *
 * All floating-point work is done in DOUBLE, exactly as CPython does, and
 * only rounded to f32 when a value is written -- the reference tools are
 * the byte-level spec.
 */
#ifndef CX_COMMON_A_H
#define CX_COMMON_A_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ misc */

/* NO RETAIL PATH IS COMPILED IN.  This tree ships no game content: point
 * $B3_GAME_ROOT at YOUR OWN dump -- the folder holding default.xbe, or an
 * .xiso image of it.  $B3_ISO and $B3_GAME_DIR name the same source and are
 * checked first.  See docs/ASSETS.md. */
#define CXA_DEFAULT_GAME_DIR ""
#define CXA_DEFAULT_TRACK "US_C3_V1"
#define CXA_DEFAULT_EVENT "OFFSGRCF"

/* FUN_001AECC0's charset: base-40, LSB char first, then reversed. [C] */
extern const char CXA_B40_CHARSET[41];

typedef struct { double x, y, z; } cxa_v3;

unsigned char *cxa_slurp(const char *path, size_t *out_len);
int   cxa_file_exists(const char *path);
void  cxa_b40(uint64_t v, char out[13]);        /* NUL-terminated, stripped */

/* little-endian scalar reads (bounds are the caller's responsibility, the
 * reference tools rely on struct.unpack_from's own range check) */
uint32_t cxa_u32(const unsigned char *d, size_t o);
uint64_t cxa_u64(const unsigned char *d, size_t o);
uint16_t cxa_u16(const unsigned char *d, size_t o);
double   cxa_f32(const unsigned char *d, size_t o);
cxa_v3   cxa_f3(const unsigned char *d, size_t o);

/* little-endian scalar writes */
void cxa_w_bytes(FILE *f, const void *p, size_t n);
void cxa_w_u8(FILE *f, unsigned v);
void cxa_w_u16(FILE *f, unsigned v);
void cxa_w_u32(FILE *f, uint32_t v);
void cxa_w_i32(FILE *f, int32_t v);
void cxa_w_f32(FILE *f, double v);
void cxa_w_v3_gl(FILE *f, cxa_v3 p);            /* GL space: z negated */
void cxa_w_pad(FILE *f, const char *s, size_t n);

/* geometry helpers -- dist/dist2 are XZ-ONLY, as in the reference tools */
double cxa_dist2(cxa_v3 a, cxa_v3 b);
double cxa_dist(cxa_v3 a, cxa_v3 b);
double cxa_median(double *v, size_t n);         /* sorts v in place */

/* ------------------------------------------------------------------- bgd */

typedef struct {
    int      index;
    char     id[13];
    uint32_t param, laps;
    uint32_t spatial_size, spatial_off;
    uint32_t tdesc_size,   tdesc_off;
    uint32_t ridx_size,    ridx_off;
    uint32_t net_size,     net_off;
} cxa_event;

typedef struct {
    unsigned char *d;
    size_t         n;
    uint32_t       count;
    cxa_event     *events;
    int            nevents;
} cxa_bgd;

int  cxa_bgd_open(cxa_bgd *b, const char *path);
void cxa_bgd_close(cxa_bgd *b);
/* the named event, else the first with a sane road-network section */
cxa_event *cxa_bgd_event(cxa_bgd *b, const char *name);

typedef struct { uint32_t start_node; double length, f2, f3; } cxa_secrec;

typedef struct {
    uint32_t    base, rows, nodes, nsec, idx, ptr20;
    cxa_secrec *secs;
    double      lap;
} cxa_net;

int  cxa_network(cxa_bgd *b, const cxa_event *ev, cxa_net *out);
void cxa_net_free(cxa_net *n);

typedef struct {
    uint32_t section, node_count, flags;
    uint32_t pairs, edges, links;               /* absolute file offsets */
} cxa_navrow;

typedef struct {
    uint32_t    points, point_count;
    cxa_navrow *rows;
    uint32_t    nrows;
} cxa_graph;

int  cxa_nav_graph(cxa_bgd *b, const cxa_event *ev, cxa_graph *out);
void cxa_graph_free(cxa_graph *g);

typedef struct {
    uint16_t node_a, node_b, node_c, speed;
    uint8_t  section, byte9, flags, byte11;
} cxa_navplan;

int  cxa_nav_plans(cxa_bgd *b, const cxa_event *ev,
                   cxa_navplan **out, uint32_t *out_count);

/* ------------------------------------------------------- start-grid slots */

typedef struct { cxa_v3 right, up, at, pos; uint32_t node; } cxa_gridslot;

int cxa_grid(cxa_bgd *b, const cxa_event *ev, cxa_gridslot out[6]);

/* --------------------------------------------------------- traffic (RIDX) */

typedef struct {
    uint32_t index, row_count;
    uint32_t pairs_off;      /* row_count x {u16 point_a, u16 point_b} */
    uint32_t dist_off;       /* row_count x {f32 distance, f32 width}  */
    uint32_t aux_off;        /* row_count x u8[0x12] branch rows       */
} cxa_tpath;

typedef struct {
    uint32_t   base, size, points, point_count;
    cxa_tpath *paths;
    uint32_t   npaths;
} cxa_tpaths;

int  cxa_traffic_paths(cxa_bgd *b, const cxa_event *ev, cxa_tpaths *out);
void cxa_tpaths_free(cxa_tpaths *t);

/* ----------------------------------------------------- TDESC (mode block) */

typedef struct {
    char     id[13];
    uint8_t  colours[8];
    uint32_t weight, tail;
} cxa_listrec;

typedef struct {
    uint32_t     off, cls, total;
    cxa_listrec *records;
    uint32_t     nrecords;
} cxa_list;

typedef struct { uint8_t record, slot; double mph; }    cxa_speed;
typedef struct { uint8_t record, slot; double rate[7]; } cxa_rate;

typedef struct {
    uint32_t   off, trigger;
    uint8_t    trigger_hi;
    cxa_speed *speeds;  uint32_t nspeeds;
    cxa_rate  *rates;   uint32_t nrates;
} cxa_schedule;

typedef struct { uint16_t first_row, last_row; uint8_t path_id, direction; }
    cxa_poolreq;

typedef struct {
    uint32_t     first_progress, last_progress;
    uint8_t      request_count, refresh_count;
    cxa_poolreq *requests;
    uint32_t     nrequests;
} cxa_poolwin;

typedef struct { uint32_t path_id, start_row; } cxa_mslot;
typedef struct { cxa_mslot slots[8]; uint32_t nslots; uint8_t flags; }
    cxa_mrecord;

typedef struct {
    uint32_t      base, size;
    cxa_list      lists[6];
    char          specials[5][13];
    int           nspecials;
    double       *spawns;          /* nspawns x 6 (pos xyz, dir xyz), raw */
    uint32_t      nspawns;
    cxa_schedule *schedules;       uint32_t nschedules;
    cxa_poolwin  *pool_windows;    uint32_t npool_windows;
    cxa_mrecord  *records;         uint32_t nrecords;
} cxa_tdesc;

int  cxa_read_tdesc(cxa_bgd *b, const cxa_event *ev, cxa_tdesc *out);
void cxa_tdesc_free(cxa_tdesc *t);

/* ------------------------------------------------------- geometry recovery */

typedef struct {
    uint32_t off, end, rel;
    int      pairs;
    cxa_v3  *A, *B, *mid;
    double   width, step, length, close;
} cxa_cor;

typedef struct {
    uint32_t off;
    int      count;
    cxa_v3  *pts;
    double   length, close, med, area;
    /* filled in by cxa_analyse */
    double   corridor_offset, route_offset, dot, grid_dist;
    int      reversed;
} cxa_loop;

int  cxa_corridor(cxa_bgd *b, const cxa_net *net, cxa_cor *out);
void cxa_cor_free(cxa_cor *c);
int  cxa_lap_loops(cxa_bgd *b, const cxa_net *net, const cxa_cor *cor,
                   cxa_loop **out, int *out_count);
void cxa_loops_free(cxa_loop *loops, int n);

/* on-road support, measured against the game's own collision world */
typedef struct {
    double  *tri;        /* 9 doubles per kept triangle */
    int      ntri;
    int32_t *cell;       /* 3 int32 per (cell,triangle) entry: gx, gz, tri */
    int      ncell;
    double   cellsize;
} cxa_road;

int  cxa_road_load(cxa_road *r, const char *collision_bin);   /* 0 = ok */
void cxa_road_free(cxa_road *r);
int  cxa_road_support(const cxa_road *r, const cxa_v3 *pts, int n);

/* --------------------------------------------------------------- analysis */

typedef struct {
    cxa_bgd      *bgd;
    cxa_event    *ev;
    cxa_gridslot  grid[6];
    cxa_net       net;
    cxa_cor       cor;
    cxa_loop     *loops;
    int           nloops;
    cxa_loop     *oncoming, *raceline;
    cxa_v3       *route;          /* ALIASES cor.mid or a loop's pts */
    int           route_n;
    int           route_is_loop;  /* route_src != "corridor midline" */
    cxa_v3       *wall_a, *wall_b;
    int           wall_n;
    int           route_start, reversed_walls;
} cxa_analysis;

/* `out_dir` is only used to find collision.bin (as the Python tool's
 * build/tracks/<ID>/collision.bin); pass NULL to skip the on-road term. */
int  cxa_analyse(cxa_analysis *a, const char *gamedata_bgd,
                 const char *event, const char *out_dir);
void cxa_analysis_free(cxa_analysis *a);

/* small path helper: join(dir, name) into a malloc'd string */
char *cxa_join(const char *dir, const char *name);

#endif /* CX_COMMON_A_H */
