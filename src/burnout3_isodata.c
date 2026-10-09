/* burnout3_isodata.c -- the ISO data model.  See burnout3_isodata.h.
 *
 * THIS FILE IS NEVER SHIMMED.  src/burnout3_isoshim.h rewrites fopen/access/
 * IMG_Load in every other src/ translation unit; this one is compiled without
 * it (its own Makefile rule), so the fopen below is the real fopen and there
 * is no way to recurse back into the resolver.
 *
 * ============================================================== THE PATH MAP
 * Every "build/..." path the port opens belongs to exactly one MATERIALISATION
 * UNIT -- one cextract stage, or a short ordered list of them where one feeds
 * another.  The map is `TRACK_MAP` (paths under build/tracks/<ID>/) and
 * `GLOBAL_MAP` (everything else), both matched top-down on the path tail.
 *
 * Granularity is per STAGE, not per track, and that is not an optimisation
 * detail: the TRACK SELECT screen reads build/tracks/<ID>/pace.bin for all 36
 * events at boot, so a whole-track unit would have cost 36 full extractions
 * (~110 s) before the first menu.  Per stage it is 36 x ~2 ms.  A per-track
 * path the map does not recognise falls back to the FULL per-track set, which
 * is the ~3 s race-load cost the design budgets for.
 *
 * ORDERING inside a unit is a real dependency, the same one cx_main.c's stage
 * order encodes: nav_edges reads the route.bin bgd_paths writes, traffic_cars
 * reads the traffic.bin traffic writes, boostfx_art reads the .lights carfx_art
 * writes.  Those units name both stages, in that order.
 *
 * ============================================================== THE OUT ROOTS
 * The stage families disagree about what their `out_root` means, so the map
 * carries the answer per unit:
 *
 *   per-track   out_dir  = <cache>/tracks/<ID>        (receives artefacts
 *                                                      directly)
 *   car/art     out_root = <cache>/.root, which holds a `build` SYMLINK to
 *                          <cache> -- because those stages write the literal
 *                          repo-relative "build/cars/...", and because
 *                          postfx_art's manifest records CWD-RELATIVE paths,
 *                          so only a root whose child really is called
 *                          `build` reproduces the shipped manifest byte for
 *                          byte.
 *   audio       out_root = <cache>/audio -- the awd/rws modules write
 *                          <out_root>/awd_<dict>/ and <out_root>/rws_<name>/
 *                          with no build/ component of their own, and
 *                          src/burnout3_sfx.c reads build/audio/awd_fe/... .
 *
 * ================================================================ THE IMAGE
 * Two files the port opens under build/ are not any stage's output: the mapped
 * executable (build/burnout3.elf) and the string table (build/Globalus.bin).
 * Both are DUMP-DIRECT: Globalus.bin is a byte copy of the disc's
 * Data/Globalus.bin, and burnout3.elf is the disc's default.xbe run through
 * the C port of tools/xbe2elf.py below.  The conversion is required, not
 * cosmetic -- src/burnout3_trackselect_runtime.h's b3_elf_open() and
 * cx_art_font.c's cxe_elf_open() both parse ELF program headers and reject a
 * raw XBE.  Both are materialised eagerly at init: they are small (4 MB and
 * 200 KB), the track-select screen wants them on the first frame anyway, and
 * $B3_ELF / $B3_XBE_ELF / $B3_GLOBALUS have to point at real files before the
 * first stage that reads them runs.
 */
/* Guarded because both web and Android pass -D_GNU_SOURCE (=1) on the command
 * line, and an unguarded bare redefinition here is a -Wmacro-redefined every
 * time this TU is really compiled -- which, before this file last changed, was
 * rarely enough that the warning never showed up in a build log.  Same spelling
 * and same guard as src/burnout3_isoshim.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef _WIN32
#include "compat/win_posix_compat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>

/* Emscripten builds this single-threaded unless -pthread is on; either way
 * __thread is what both toolchains spell it, and the Android NDK too. */
#define B3_THREAD_LOCAL __thread

#include "burnout3_isodata.h"
#include "cx_extract.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* The shared source layer (XISO image or plain directory).  It lands
 * independently; until it does, a plain directory source is all the port
 * needs, and that path is handled here without it. */
#if defined(__has_include)
#  if __has_include("cx_src.h")
#    include "cx_src.h"
#    define B3_HAVE_CX_SRC 1
#  endif
#endif

/* ------------------------------------------------------------------ state */

/* A ROOT is a directory the user named (the disc, the cache); a PATH is a root
 * with an asset's own relative path joined onto it.  Keeping the two sizes
 * apart is what lets the compiler see that every join below fits, instead of
 * warning that a 4096-byte root plus "/tracks/<ID>" might not. */
#define B3_ROOT_MAX 4096
#define B3_PATH_MAX 8192

static int   g_mode      = B3_DATA_BUILD;
static int   g_inited    = 0;
static int   g_busy      = 0;          /* re-entry guard */
static int   g_verbose   = 0;
static char  g_source[B3_ROOT_MAX];           /* the .iso or the expanded dump */
static char  g_cache[B3_ROOT_MAX];            /* the materialised mirror of build/ */
/* g_root is g_cache with "/.root" on the end, so it needs the slack to say so */
static char  g_root[B3_ROOT_MAX + 64];        /* <cache>/.root, the repo stand-in */
static char  g_repo[B3_ROOT_MAX];             /* the real checkout */

/* The EA TRAX song count, for the per-song music unit in map_lookup().  It is
 * a property of the DISC -- 22 entries in each of the two _EATraxN.xwb banks,
 * and 44 rows in the game's own song table at VA 0x003EC458 -- and it is
 * repeated rather than included because this file is compiled WITHOUT the
 * isoshim and deliberately shares no header with the src/ engine.  The same
 * number lives in B3MUSIC_TRACKS (src/burnout3_music.h) and SONG_COUNT
 * (tools/cextract/cx_audio_eatrax.c); all three are the disc, not a choice. */
#define B3_MUSIC_TRACKS 44

#ifdef B3_HAVE_CX_SRC
static CxSrc *g_src = NULL;
#endif

/* -------------------------------------------------------- small utilities */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int path_exists(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p, tolerating a component that is already a directory (or a symlink
 * to one, which <cache>/.root/build is). */
static int mkdir_p(const char *path)
{
    char buf[B3_PATH_MAX];
    size_t n, i;

    if (!path || !*path) return -1;
    n = strlen(path);
    if (n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                if (!is_dir(buf)) { buf[i] = c; return -1; }
            }
            buf[i] = c;
        }
    }
    return 0;
}

static int mkdir_p_parent(const char *path)
{
    char buf[B3_PATH_MAX];
    char *slash;

    snprintf(buf, sizeof buf, "%s", path);
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;
    *slash = '\0';
    return mkdir_p(buf);
}

/* A ring, so several resolved paths can be alive across one expression. */
/* THE RETURN RING, AND WHY IT IS THREAD-LOCAL.
 *
 * resolve_inner() hands back a pointer into this ring, so the ring is the
 * one piece of shared mutable state a CACHE HIT touches -- and a cache hit
 * is what nearly every resolve is once the load screen is done.  The music
 * module now materialises the next song on a worker thread (see THE
 * MATERIALISE LOCK below), which put a second caller in here.
 *
 * Making it __thread rather than locking it is deliberate: the hit path
 * then takes NO lock at all, so a 0.5 s decode on the worker cannot block
 * the frame thread's own resolves behind it.  That is the whole point of
 * moving the decode off the frame path; a shared lock here would have put
 * the stall straight back. */
static pthread_mutex_t g_mat_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_main_thread;      /* set in b3_iso_init() */

#define B3_RING 8
static B3_THREAD_LOCAL char g_ring[B3_RING][B3_PATH_MAX];
static B3_THREAD_LOCAL int  g_ring_i = 0;

static char *ring_next(void)
{
    char *p = g_ring[g_ring_i];
    g_ring_i = (g_ring_i + 1) % B3_RING;
    return p;
}

static void logline(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[Burnout3] iso: ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* --------------------------------------------------- the source (cx_src) --
 * A dump-direct read of one file out of the disc.  With cx_src linked this is
 * the shared layer, ISO or directory alike; without it, the directory case,
 * which is all a pre-cx_src tree can offer. */
static unsigned char *src_slurp(const char *rel, size_t *out_len)
{
    unsigned char *buf = NULL;

    if (out_len) *out_len = 0;

#ifdef B3_HAVE_CX_SRC
    if (g_src) {
        unsigned long long sz = 0;
        long got;
        if (cx_src_stat(g_src, rel, &sz) != 0 || sz == 0) return NULL;
        buf = (unsigned char *)malloc((size_t)sz);
        if (!buf) return NULL;
        got = cx_src_read(g_src, rel, 0, buf, (long)sz);
        if (got != (long)sz) { free(buf); return NULL; }
        if (out_len) *out_len = (size_t)sz;
        return buf;
    }
#endif
    {
        char  path[B3_PATH_MAX];
        FILE *f;
        long  sz;

        snprintf(path, sizeof path, "%s/%s", g_source, rel);
        f = fopen(path, "rb");
        if (!f) return NULL;
        if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0
            || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
        buf = (unsigned char *)malloc((size_t)sz);
        if (!buf) { fclose(f); return NULL; }
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
            free(buf); fclose(f); return NULL;
        }
        fclose(f);
        if (out_len) *out_len = (size_t)sz;
        return buf;
    }
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f;

    if (mkdir_p_parent(path) != 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ xbe -> elf --
 * tools/xbe2elf.py, in C.  One PT_LOAD per XBE section plus the header
 * itself, file offsets kept congruent to the VA mod 4096.  Faithful enough
 * that the output is byte-identical to the checked-in build/burnout3.elf. */

static unsigned int rd32(const unsigned char *d, size_t off)
{
    return (unsigned int)d[off] | ((unsigned int)d[off + 1] << 8)
         | ((unsigned int)d[off + 2] << 16) | ((unsigned int)d[off + 3] << 24);
}

static void wr16(unsigned char *d, unsigned short v)
{
    d[0] = (unsigned char)(v & 0xFF); d[1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *d, unsigned int v)
{
    d[0] = (unsigned char)(v & 0xFF);        d[1] = (unsigned char)(v >> 8);
    d[2] = (unsigned char)((v >> 16) & 0xFF); d[3] = (unsigned char)(v >> 24);
}

typedef struct {
    unsigned int vaddr, filesz, memsz, flags, raw_off;
} XbeSeg;

#define XBE_SEC_W 1
#define XBE_SEC_X 4
#define ELF_PF_X  1
#define ELF_PF_W  2
#define ELF_PF_R  4

static int xbe_to_elf(const unsigned char *d, size_t n,
                      unsigned char **out, size_t *out_len)
{
    static const unsigned int XOR_EP[3] =
        { 0xA8FC57ABu, 0x94859D4Bu, 0x40B5C16Eu };  /* retail, debug, chihiro */
    unsigned int base, hdr_size, image_size, nsections, sec_hdr_addr, ep_raw;
    unsigned int entry = 0;
    XbeSeg      *segs;
    unsigned int nseg = 0, i, j, cur;
    size_t       total;
    unsigned char *o;
    size_t       w;

    if (n < 0x180 || memcmp(d, "XBEH", 4) != 0) return -1;
    base         = rd32(d, 0x104);
    hdr_size     = rd32(d, 0x108);
    image_size   = rd32(d, 0x10C);
    nsections    = rd32(d, 0x11C);
    sec_hdr_addr = rd32(d, 0x120);
    ep_raw       = rd32(d, 0x128);

    entry = ep_raw;
    for (i = 0; i < 3; i++) {
        unsigned int v = ep_raw ^ XOR_EP[i];
        if (v >= base && v - base < image_size) { entry = v; break; }
    }

    if (nsections > 4096) return -1;
    if (sec_hdr_addr < base) return -1;
    segs = (XbeSeg *)calloc(nsections + 1, sizeof *segs);
    if (!segs) return -1;

    /* the XBE header itself, mapped read-only (include_headers=True) */
    if (hdr_size > n) hdr_size = (unsigned int)n;
    segs[nseg].vaddr   = base;
    segs[nseg].filesz  = hdr_size;
    segs[nseg].memsz   = hdr_size;
    segs[nseg].flags   = ELF_PF_R;
    segs[nseg].raw_off = 0;
    nseg++;

    {   /* sections, sorted by vaddr (a stable insertion sort, as python's
         * sorted() is stable and the shipped image has ties in neither) */
        size_t shb = (size_t)(sec_hdr_addr - base);
        for (i = 0; i < nsections; i++) {
            size_t b = shb + (size_t)i * 0x38;
            unsigned int flags, va, vs, ro, rs, ef;
            if (b + 0x18 > n) break;
            flags = rd32(d, b + 0x00);
            va    = rd32(d, b + 0x04);
            vs    = rd32(d, b + 0x08);
            ro    = rd32(d, b + 0x0C);
            rs    = rd32(d, b + 0x10);
            if ((size_t)ro > n) { ro = (unsigned int)n; rs = 0; }
            if ((size_t)ro + rs > n) rs = (unsigned int)(n - ro);
            ef = ELF_PF_R;
            if (flags & XBE_SEC_X) ef |= ELF_PF_X;
            if (flags & XBE_SEC_W) ef |= ELF_PF_W;
            /* insertion sort by vaddr, after the header segment */
            for (j = nseg; j > 1 && segs[j - 1].vaddr > va; j--)
                segs[j] = segs[j - 1];
            segs[j].vaddr   = va;
            segs[j].filesz  = rs;
            segs[j].memsz   = vs > rs ? vs : rs;
            segs[j].flags   = ef;
            segs[j].raw_off = ro;
            nseg++;
        }
    }

    /* two passes, exactly as build_elf(): lay the file out, then emit it */
    cur = 52u + 32u * nseg;
    total = cur;
    for (i = 0; i < nseg; i++) {
        unsigned int pad = (segs[i].vaddr - (unsigned int)total) & 0xFFFu;
        total += pad + segs[i].filesz;
    }
    o = (unsigned char *)calloc(1, total ? total : 1);
    if (!o) { free(segs); return -1; }

    memcpy(o, "\177ELF", 4);
    o[4] = 1; o[5] = 1; o[6] = 1; o[7] = 0;          /* 32-bit, LE, EV_CURRENT */
    wr16(o + 16, 2);        /* e_type    ET_EXEC   */
    wr16(o + 18, 3);        /* e_machine EM_386    */
    wr32(o + 20, 1);        /* e_version           */
    wr32(o + 24, entry);    /* e_entry             */
    wr32(o + 28, 52);       /* e_phoff             */
    wr32(o + 32, 0);        /* e_shoff             */
    wr32(o + 36, 0);        /* e_flags             */
    wr16(o + 40, 52);       /* e_ehsize            */
    wr16(o + 42, 32);       /* e_phentsize         */
    wr16(o + 44, (unsigned short)nseg);
    wr16(o + 46, 40);       /* e_shentsize         */
    wr16(o + 48, 0);        /* e_shnum             */
    wr16(o + 50, 0);        /* e_shstrndx          */

    w = 52u + 32u * nseg;
    for (i = 0; i < nseg; i++) {
        unsigned char *ph = o + 52 + 32 * i;
        unsigned int pad = (segs[i].vaddr - (unsigned int)w) & 0xFFFu;
        w += pad;
        wr32(ph +  0, 1);                 /* PT_LOAD */
        wr32(ph +  4, (unsigned int)w);   /* p_offset */
        wr32(ph +  8, segs[i].vaddr);
        wr32(ph + 12, segs[i].vaddr);     /* p_paddr */
        wr32(ph + 16, segs[i].filesz);
        wr32(ph + 20, segs[i].memsz);
        wr32(ph + 24, segs[i].flags);
        wr32(ph + 28, 0x1000);            /* p_align */
        if (segs[i].filesz)
            memcpy(o + w, d + segs[i].raw_off, segs[i].filesz);
        w += segs[i].filesz;
    }

    free(segs);
    *out = o;
    *out_len = total;
    return 0;
}

/* ------------------------------------------------------------ the stages */

typedef int (*b3_track_fn)(const char *, const char *, const char *,
                           const char *);
typedef int (*b3_global_fn)(const char *, const char *);

static const struct { const char *name; b3_track_fn fn; } TRACK_FN[] = {
#define X(M, n) { #n, cx_extract_##n },
    CX_STAGE_LIST(X)
#undef X
    { "traffic_cars", cx_extract_traffic_cars },
};
#define N_TRACK_FN (sizeof TRACK_FN / sizeof TRACK_FN[0])

/* Where a global stage's out_root points. */
/* NB: no wildcard-then-slash inside these comments -- it would close them. */
#define OUT_REPO  0     /* <cache>/.root  -- writes "build/..." beneath it   */
#define OUT_AUDIO 1     /* <cache>/audio  -- writes awd_ and rws_ subdirs    */

/* The globals the GAME links.
 *
 * `eatrax` is NOT in this table even though it is now linked, and that is the
 * point of THE MUSIC FAMILY below: a global stage runs once and produces its
 * whole family, which for EA TRAX is 44 songs, ~720 MB of PCM and 22.7 s of
 * decoding (measured, this disc, this machine).  Paying that at the first note
 * of a race is not on.  Music is materialised ONE SONG AT A TIME instead,
 * through its own runner, so the shuffle only ever decodes what it is about to
 * play -- 0.52 s for the song, and nothing for the other 43.
 *
 * `xwb` is not here either: it is the whole-disc bank dump, a tool for people
 * reading the audio, not something the game ever reads back. */
static const struct { const char *name; b3_global_fn fn; int out; }
GLOBAL_FN[] = {
    { "vehicle_roster", cx_extract_vehicle_roster, OUT_REPO  },
    { "car_tuning",     cx_extract_car_tuning,     OUT_REPO  },
    { "car_meshes",     cx_extract_car_meshes,     OUT_REPO  },
    { "car_paint",      cx_extract_car_paint,      OUT_REPO  },
    { "traffic_lights", cx_extract_traffic_lights, OUT_REPO  },
    { "hulls",          cx_extract_hulls,          OUT_REPO  },
    { "car_bvh",        cx_extract_car_bvh,        OUT_REPO  },
    { "txd",            cx_extract_txd,            OUT_REPO  },
    { "font",           cx_extract_font,           OUT_REPO  },
    { "carfx_art",      cx_extract_carfx_art,      OUT_REPO  },
    { "boostfx_art",    cx_extract_boostfx_art,    OUT_REPO  },
    { "particlefx_art", cx_extract_particlefx_art, OUT_REPO  },
    { "postfx_art",     cx_extract_postfx_art,     OUT_REPO  },
    { "awd",            cx_extract_awd,            OUT_AUDIO },
    { "rws",            cx_extract_rws,            OUT_AUDIO },
};
#define N_GLOBAL_FN (sizeof GLOBAL_FN / sizeof GLOBAL_FN[0])

/* ------------------------------------------------------------- the map --
 * `tail` is matched with MATCH_EXACT (whole remainder) or MATCH_PREFIX (the
 * remainder starts with it); "" is the catch-all and must come last. */
#define MATCH_EXACT  0
#define MATCH_PREFIX 1
#define MATCH_SUFFIX 2

typedef struct {
    const char *tail;
    int         how;
    const char *stages;     /* space-separated, run in order; "" = nothing */
} MapRule;

/* Paths under build/tracks/<ID>/ .  The unit key is stage+track, so the 36
 * pace.bin reads the track-select screen does at boot cost 36 pace runs, not
 * 36 whole-track runs. */
static const MapRule TRACK_MAP[] = {
    { "track.obj",        MATCH_EXACT,  "track"                    },
    { "track.mtl",        MATCH_EXACT,  "track"                    },
    { "textures/",        MATCH_PREFIX, "textures"                 },
    { "collision.bin",    MATCH_EXACT,  "collision"                },
    { "envmap.png",       MATCH_EXACT,  "envmap"                   },
    { "route.bin",        MATCH_EXACT,  "bgd_paths"                },
    { "nav_edges.bin",    MATCH_EXACT,  "bgd_paths nav_edges"      },
    { "traffic.bin",      MATCH_EXACT,  "traffic"                  },
    { "traffic_paths.bin",MATCH_EXACT,  "traffic"                  },
    { "cars/",            MATCH_PREFIX, "traffic traffic_cars"     },
    { "grid.bin",         MATCH_EXACT,  "start_grid"               },
    { "pace.bin",         MATCH_EXACT,  "pace"                     },
    { "props.bin",        MATCH_EXACT,  "props"                    },
    { "scenery.bin",      MATCH_EXACT,  "scenery"                  },
    { "light_probes.bin", MATCH_EXACT,  "light_probes"             },
    /* THE BVH READS THREE OTHER STAGES' ARTEFACTS, so the ordered list is
     * the whole dependency: track (track.obj + track.mtl + textures/),
     * props and scenery all have to be on disc before cx_bvh can walk
     * them.  Same shape as "bgd_paths nav_edges". */
    { "bvh.bin",          MATCH_EXACT,
      "track textures props scenery bvh"                               },
    /* anything else under a track: the whole set, the ~3 s race-load cost */
    { "",                 MATCH_PREFIX,
      "tlist track textures collision envmap bgd_paths traffic "
      "traffic_cars nav_edges start_grid pace props scenery light_probes "
      "bvh" },
};
#define N_TRACK_MAP (sizeof TRACK_MAP / sizeof TRACK_MAP[0])

/* Everything else under build/ .  "" as a stage list means "no stage owns
 * this" -- a config file, a log, a debug dump, or an asset family the game
 * build cannot produce (build/music, see materialise()). */
static const MapRule GLOBAL_MAP[] = {
    { "cars/car_physics.bin", MATCH_EXACT,  "car_tuning"              },
    /* the optional ray-traced sun shadow's per-car trees.  It reads the
     * .bgv/.btv containers rather than build/cars/, so unlike everything else
     * under this prefix it owns itself outright and needs no other family to
     * have run first. */
    { "cars/carbvh.bin",      MATCH_EXACT,  "car_bvh"                 },
    { "cars/roster.bin",      MATCH_EXACT,  "vehicle_roster"          },
    { "cars/parts/",          MATCH_PREFIX, "car_meshes"              },
    { ".hull",                MATCH_SUFFIX, "hulls"                   },
    { ".lights",              MATCH_SUFFIX, "carfx_art traffic_lights"},
    { ".panels",              MATCH_SUFFIX, "car_meshes"              },
    { ".wheels",              MATCH_SUFFIX, "car_meshes"              },
    { "cars/",                MATCH_PREFIX, "car_meshes car_paint"    },
    { "frontend/font.bin",    MATCH_EXACT,  "font"                    },
    { "frontend/",            MATCH_PREFIX, "txd font"                },
    { "carfx/",               MATCH_PREFIX, "carfx_art"               },
    { "boostfx/",             MATCH_PREFIX, "carfx_art boostfx_art"   },
    { "particlefx/",          MATCH_PREFIX, "particlefx_art"          },
    { "postfx/",              MATCH_PREFIX, "postfx_art"              },
    { "audio/rws",            MATCH_PREFIX, "rws"                     },
    { "audio/awd",            MATCH_PREFIX, "awd"                     },
    { "audio/",               MATCH_PREFIX, "awd rws"                 },
    /* build/music/track_NN.wav never reaches this rule -- map_lookup() catches
     * it first and gives it a per-song unit.  What lands here is the rest of
     * the family, i.e. eatrax.txt, which is a 44-row manifest and therefore
     * genuinely does need the whole batch. */
    { "music/",               MATCH_PREFIX, "eatrax"                  },
    { "Globalus.bin",         MATCH_EXACT,  "@globalus"               },
    { "burnout3.elf",         MATCH_EXACT,  "@elf"                    },
    /* build/textures/ is the legacy global texture fallback the per-track
     * textures/ superseded; nothing writes it any more. */
    { "textures/",            MATCH_PREFIX, ""                        },
};
#define N_GLOBAL_MAP (sizeof GLOBAL_MAP / sizeof GLOBAL_MAP[0])

static int rule_hit(const MapRule *r, const char *tail)
{
    size_t n;
    switch (r->how) {
    case MATCH_EXACT:  return strcmp(tail, r->tail) == 0;
    case MATCH_PREFIX: return strncmp(tail, r->tail, strlen(r->tail)) == 0;
    case MATCH_SUFFIX:
        n = strlen(r->tail);
        return strlen(tail) >= n && strcmp(tail + strlen(tail) - n, r->tail) == 0;
    }
    return 0;
}

/* ---------------------------------------------------------- unit registry */

#define N_UNITS 1024
static char g_unit[N_UNITS][96];
static int  g_nunits = 0;

static int  g_unit_rc[N_UNITS];

/* -1 = never attempted this run; otherwise the return code it ended with. */
static int unit_rc(const char *key)
{
    int i;
    for (i = 0; i < g_nunits; i++)
        if (strcmp(g_unit[i], key) == 0) return g_unit_rc[i];
    return -1;
}

static void unit_mark(const char *key, int rc)
{
    int i;
    for (i = 0; i < g_nunits; i++)
        if (strcmp(g_unit[i], key) == 0) { g_unit_rc[i] = rc; return; }
    if (g_nunits < N_UNITS) {
        snprintf(g_unit[g_nunits], sizeof g_unit[0], "%s", key);
        g_unit_rc[g_nunits] = rc;
        g_nunits++;
    }
}

/* -------------------------------------------------------- THE STAMPS --
 * The unit registry above is per PROCESS.  Across processes the cache is the
 * only memory, and "is the file there?" is the wrong question to rebuild a
 * unit from: build/cars/ is written by two different families, so the first
 * traffic car of a NEW track is missing from a cache whose .bgv fleet is
 * complete, and a bare miss re-extracts all 107 player cars (6 s) to discover
 * that the car it wanted is a .btv the per-track mirror owns.
 *
 * <cache>/.stamps/<unit> records that a unit RAN TO COMPLETION.  A stamped
 * unit is never run again, so the miss falls straight through to the family
 * that really owns the file.  A unit that failed leaves no stamp and retries
 * on the next boot.
 *
 * The trade this makes, stated plainly: the cache stops self-healing per
 * FILE.  Delete one artefact and its stage will not notice, because the stage
 * is stamped.  Deleting <cache>/.stamps (or the whole cache) re-asks the disc
 * for everything, and that is the documented repair. */
static void stamp_path(char *out, size_t n, const char *key)
{
    size_t i;
    snprintf(out, n, "%s/.stamps/%s", g_cache, key);
    for (i = strlen(g_cache) + 9; i < strlen(out); i++)
        if (out[i] == ':' || out[i] == '/') out[i] = '_';
}

static int stamp_seen(const char *key)
{
    char p[B3_PATH_MAX];
    stamp_path(p, sizeof p, key);
    return path_exists(p);
}

static void stamp_write(const char *key)
{
    char  p[B3_PATH_MAX];
    FILE *f;
    stamp_path(p, sizeof p, key);
    if (mkdir_p_parent(p) != 0) return;
    f = fopen(p, "w");
    if (f) fclose(f);
}

/* -------------------------------------- A STAMP IS ADVISORY, NEVER PROOF --
 * The paragraph above says it outright -- "delete one artefact and its stage
 * will not notice, because the stage is stamped" -- and treats that as a
 * trade.  It is not a trade the WEB port can make, because there the cache
 * loses artefacts on its own.
 *
 * A user booted with an IDBFS cache that Chrome could not fully read back:
 *
 *     web: cache load failed (UnknownError: Failed to read large IndexedDB
 *          value) -- starting from a cold cache
 *     ...
 *     FATAL: no usable build/tracks/US_C1_V1/track.obj
 *
 * IDBFS keeps one IndexedDB record per file.  The 25 MB track.obj was the
 * record Chrome could not read; every SMALL record came back, the 28 .stamps
 * among them.  So the cache came up claiming the track stage had already run,
 * while the only thing that stage produces was gone.  materialise() honoured
 * the stamp, skipped the stage, logged nothing (`ran` stayed 0, so its own
 * summary line never fired), and the resolver handed back the unresolved
 * build/ path.  Reproduced exactly by evicting that one key out of 5 134 and
 * leaving the 28 stamps intact.
 *
 * THE RULE, and it follows from the disc being the only required source: the
 * cache may say "I have already done this", and the moment the file it claims
 * to have produced is missing, that claim is FALSE and is thrown away.  The
 * stamp keeps its real job -- one boot, not 107 car meshes, to learn that a
 * .btv belongs to the other family -- because this only fires when the
 * caller's own file did not appear. */
static int stamp_drop(const char *key)
{
    char p[B3_PATH_MAX];
    stamp_path(p, sizeof p, key);
    if (!path_exists(p)) return 0;
    return remove(p) == 0 ? 1 : 0;
}

/* Forget a unit's result for THIS process, so the retry below actually runs
 * it instead of reading the mark the skipped pass just left. */
static void unit_forget(const char *key)
{
    int i;
    for (i = 0; i < g_nunits; i++)
        if (strcmp(g_unit[i], key) == 0) { g_unit_rc[i] = -1; return; }
}

/* Drop every stamp behind `stages`, and this run's marks with them.
 * Returns how many stamps were really there -- 0 means the stamps were not
 * what stopped us and there is nothing to retry. */
static int stamps_invalidate(const char *stages, const char *track_id)
{
    char  scan[512], key[96];
    char *save = NULL, *tok;
    int   n = 0;

    if (!stages || !*stages) return 0;
    snprintf(scan, sizeof scan, "%s", stages);
    for (tok = strtok_r(scan, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
        if (tok[0] == '@') continue;          /* never stamped anyway */
        if (track_id) snprintf(key, sizeof key, "T:%s:%s", tok, track_id);
        else          snprintf(key, sizeof key, "G:%s", tok);
        n += stamp_drop(key);
        unit_forget(key);
    }
    return n;
}

/* ------------------------------------------------- THE CACHE GENERATION --
 * .stamps and .absent are DERIVED state: neither is game data, both are the
 * cache's memory of what it already learned, and a wrong entry in either can
 * stop the disc being asked at all.  They therefore carry a generation, and a
 * cache written by an older one has its derived state dropped -- once, with a
 * line -- while every extracted artefact is kept.
 *
 * GEN 2 exists because gen 1 could poison itself.  A stale stamp made
 * materialise() skip the stage silently; resolve_inner() then saw "nothing
 * failed" and wrote the path into .absent, which is consulted BEFORE the map
 * and returns without extracting anything.  So one unlucky boot turned a
 * recoverable miss into a permanent one, and no later fix to the stamp rule
 * would have reached it -- .absent answers first.  Bumping the generation is
 * what repairs the caches already in that state. */
#define B3_CACHE_GEN 2

static void rmdir_tree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char p[B3_PATH_MAX];

    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        /* remove() is rmdir for an empty directory and unlink for a file, so
         * a failure here means "non-empty directory" -- .stamps is flat, so
         * this arm is defence, not a case that occurs. */
        if (remove(p) != 0) {
            rmdir_tree(p);
            remove(p);
        }
    }
    closedir(d);
    remove(dir);
}

/* Runs once, right after g_cache is known and before anything reads either
 * list.  Never touches extracted assets -- only the two memories. */
static void cache_generation_check(void)
{
    char  p[B3_PATH_MAX], s[B3_PATH_MAX];
    FILE *f;
    int   gen = 0;

    snprintf(p, sizeof p, "%s/.gen", g_cache);
    if ((f = fopen(p, "r")) != NULL) {
        if (fscanf(f, "%d", &gen) != 1) gen = 0;
        fclose(f);
    }
    if (gen != B3_CACHE_GEN) {
        snprintf(s, sizeof s, "%s/.absent", g_cache);
        if (path_exists(s) || gen == 0) {
            remove(s);
            snprintf(s, sizeof s, "%s/.stamps", g_cache);
            rmdir_tree(s);
            if (gen)
                logline("cache generation %d -> %d: dropped .stamps/.absent "
                        "(extracted assets kept)\n", gen, B3_CACHE_GEN);
        }
        if ((f = fopen(p, "w")) != NULL) {
            fprintf(f, "%d\n", B3_CACHE_GEN);
            fclose(f);
        }
    }
}

/* ------------------------------------------------------ THE ABSENT LIST --
 * Some of the paths the port asks for DO NOT EXIST ON THE DISC, legitimately:
 * b3_sfx probes every surface loop in a track's bank and Silver Lake has no
 * snow.wav; a car whose wheels are part of its body has no _wheel.obj.  A
 * plain materialise-on-miss re-runs the whole fleet -- 7 s of car meshes and
 * paints -- for one such file, on EVERY BOOT, because the miss never goes
 * away.  The in-process unit registry above stops the second ask within one
 * run; this stops the second RUN.
 *
 * <cache>/.absent records the relative path of anything a SUCCESSFULLY
 * COMPLETED unit did not produce, so the next boot answers "not on the disc"
 * without extracting anything.  A unit that FAILED is never recorded, so a
 * genuine error still retries.  The cache keeps its self-healing for files
 * that do exist: delete one and its stage runs again.  Remove <cache>/.absent
 * (or the cache) to re-ask the disc from scratch. */
#define N_ABSENT 4096
static char **g_absent = NULL;
static int    g_nabsent = 0, g_absent_loaded = 0;

static void absent_load(void)
{
    char  path[B3_PATH_MAX], line[B3_PATH_MAX];
    FILE *f;

    if (g_absent_loaded) return;
    g_absent_loaded = 1;
    g_absent = (char **)calloc(N_ABSENT, sizeof *g_absent);
    if (!g_absent) return;
    snprintf(path, sizeof path, "%s/.absent", g_cache);
    f = fopen(path, "r");
    if (!f) return;
    while (g_nabsent < N_ABSENT && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) g_absent[g_nabsent++] = strdup(line);
    }
    fclose(f);
}

static int absent_known(const char *rel)
{
    int i;
    absent_load();
    for (i = 0; i < g_nabsent; i++)
        if (g_absent[i] && strcmp(g_absent[i], rel) == 0) return 1;
    return 0;
}

static void absent_record(const char *rel)
{
    char  path[B3_PATH_MAX];
    FILE *f;

    absent_load();
    if (!g_absent || g_nabsent >= N_ABSENT || absent_known(rel)) return;
    g_absent[g_nabsent] = strdup(rel);
    if (!g_absent[g_nabsent]) return;
    g_nabsent++;
    snprintf(path, sizeof path, "%s/.absent", g_cache);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", rel);
    fclose(f);
}

/* ------------------------------------------------------- running a stage */

/* Stage chatter is a wall of text nobody asked for at a race load, so it goes
 * to /dev/null and this module prints one line per unit instead.  stderr is
 * untouched: a stage that really fails still says so.  B3_ISO_VERBOSE=1 keeps
 * the full transcript. */
static int g_saved_out = -1;

/* MAIN THREAD ONLY, and this one really is a correctness rule rather than
 * a convention.  The silencing is a dup2 over file descriptor 1, which is
 * PROCESS-global: doing it on the decode worker would send whatever the
 * game thread happened to be printing -- the FPS line, a [dj] line, the
 * frame profiler -- to /dev/null for the half second a song takes.
 *
 * Nothing is lost by skipping it there.  The only stage the worker ever
 * runs is 'T:eatrax:<n>', and cx_extract_eatrax_one() writes to stderr
 * alone; there is no stdout chatter to suppress. */
static void quiet_begin(void)
{
    int devnull;
    if (g_verbose) return;
    if (!pthread_equal(pthread_self(), g_main_thread)) return;
    fflush(stdout);
    g_saved_out = dup(1);
    devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, 1); close(devnull); }
}

static void quiet_end(void)
{
    if (g_verbose || g_saved_out < 0) return;
    if (!pthread_equal(pthread_self(), g_main_thread)) return;
    fflush(stdout);
    dup2(g_saved_out, 1);
    close(g_saved_out);
    g_saved_out = -1;
}

static int run_global(const char *stage)
{
    size_t i;
    char   cwd[B3_ROOT_MAX], out[B3_PATH_MAX];
    int    rc = -1;

    for (i = 0; i < N_GLOBAL_FN; i++) {
        if (strcmp(GLOBAL_FN[i].name, stage) != 0) continue;
        if (GLOBAL_FN[i].out == OUT_AUDIO)
            snprintf(out, sizeof out, "%s/audio", g_cache);
        else
            snprintf(out, sizeof out, "%s", g_root);
        if (mkdir_p(out) != 0) return -1;
        /* NOT FROM A WORKER, EVER.  The chdir below is process-global: a
         * stage running here while the game thread does a relative-path
         * open would send that open somewhere else entirely.  The music
         * module's decode worker only ever asks for 'T:eatrax:<n>', which
         * is deliberately NOT in this table and never chdir()s (see the
         * note on GLOBAL_FN) -- so this can only fire if someone widens
         * what the worker is allowed to request.  Loudly, on purpose. */
        if (!pthread_equal(pthread_self(), g_main_thread)) {
            logline("%s: refused -- a chdir stage cannot run off the main "
                    "thread\n", stage);
            return -1;
        }
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
        /* the driver's contract: the CWD is the output root while a global
         * stage runs (cx_main.c run_global_mode) */
        if (chdir(out) != 0) return -1;
        quiet_begin();
        rc = GLOBAL_FN[i].fn(g_source, out);
        quiet_end();
        if (cwd[0] && chdir(cwd) != 0) { /* nothing sane left to do */ }
        return rc;
    }
    return -1;
}

static int run_track(const char *stage, const char *track_id)
{
    size_t i;
    char   id[64], track_dir[B3_ROOT_MAX], out[B3_PATH_MAX];
    int    rc;

    quiet_begin();
    rc = cx_resolve_track(g_source, track_id, id, sizeof id,
                          track_dir, sizeof track_dir);
    quiet_end();
    if (rc != 0) return -1;

    snprintf(out, sizeof out, "%s/tracks/%s", g_cache, id);
    if (mkdir_p(out) != 0) return -1;

    for (i = 0; i < N_TRACK_FN; i++) {
        if (strcmp(TRACK_FN[i].name, stage) != 0) continue;
        quiet_begin();
        rc = TRACK_FN[i].fn(g_source, track_dir, id, out);
        quiet_end();
        return rc;
    }
    return -1;
}

/* --------------------------------------------------------- THE MUSIC FAMILY
 * build/music/track_NN.wav, decoded out of Tracks/_EATraxN.xwb on demand.
 *
 * Everything else under build/ is materialised a FAMILY at a time, because the
 * families are cheap and their members are read together -- a track's textures
 * arrive with its geometry, the car fleet arrives in one pass.  Music is the
 * one family where that is exactly the wrong shape.  All 44 songs is 722 MB of
 * 44.1 kHz mono s16 and 22.7 s of decode; the game plays ONE, picked by a
 * shuffle, and moves to the next only when the first ends three minutes later.
 *
 * So the unit here is a SONG, not the family: the map hands us the index out
 * of the filename and this decodes just that one, keyed 'T:eatrax:<n>' so it
 * is stamped, memoised and retried exactly like every other unit.  A track
 * change costs 0.52 s of decode inside b3_music_pump()'s read-ahead, which has
 * an 11.9 s ring in front of it.
 *
 * A NULL `song` runs the whole family and writes the manifest -- what
 * build/music/eatrax.txt asks for, and what nothing at run time does. */
static int run_music(const char *song)
{
    char out[B3_PATH_MAX];
    int  rc;

    snprintf(out, sizeof out, "%s/music", g_cache);
    if (mkdir_p(out) != 0) return -1;

    quiet_begin();
    if (song && *song) {
        rc = cx_extract_eatrax_one(g_source, out, atoi(song));
    } else {
        /* The batch arm resolves artist/title/album from Globalus.bin, which
         * cx_extract_eatrax looks for relative to the CWD unless told
         * otherwise.  In iso mode it lives in the cache, so say so rather than
         * relying on where we happen to be standing. */
        char glob[B3_PATH_MAX];
        snprintf(glob, sizeof glob, "%s/Globalus.bin", g_cache);
        if (path_exists(glob))
            setenv("B3_GLOBALUS", glob, 1);
        rc = cx_extract_eatrax(g_source, out);
    }
    quiet_end();
    return rc;
}

/* ------------------------------------------------------ THE DJ VOICE BANKS
 * build/audio/<BANK>/NNN.wav, out of the disc's XACT wave banks, one BANK at
 * a time -- the same shape as THE MUSIC FAMILY above and for the same reason.
 *
 * `xwb` is the whole-disc dump: 33 banks, 885 entries, the two 361 MB
 * _EATraxN.xwb among them.  That is why it is not in GLOBAL_FN and the game
 * cannot ask for it.  What Crash FM actually wants is ONE bank -- the twelve
 * global DJ* banks are 5 to 20 lines each, and a race's own commentary is the
 * per-track E_DJRACE.xwb, which names itself after its track.  So the unit
 * here is a BANK, keyed 'T:djbank:<BANK>' so it is stamped, memoised and
 * retried exactly like every other unit.
 *
 * `bank` is the OUTPUT directory name, which is the bank's own BANKDATA name
 * and not its filename: on disc these carry a language prefix (E_ for
 * English, also I_ S_ F_ G_ J_).  cx_extract_xwb_one() resolves one to the
 * other by reading headers, so nothing here has to know the prefix. */
static int run_djbank(const char *bank)
{
    char out[B3_PATH_MAX];
    int  rc;

    if (!bank || !*bank) return -1;
    snprintf(out, sizeof out, "%s/audio", g_cache);
    if (mkdir_p(out) != 0) return -1;

    quiet_begin();
    rc = cx_extract_xwb_one(g_source, out, bank);
    quiet_end();
    return rc;
}

/* ------------------------------------------------------ the direct copies */

static int materialise_globalus(void)
{
    unsigned char *d;
    size_t         n = 0;
    char           out[B3_PATH_MAX];
    int            rc;

    d = src_slurp("Data/Globalus.bin", &n);
    if (!d) {
        fprintf(stderr, "[Burnout3] iso: %s has no Data/Globalus.bin\n",
                g_source);
        return -1;
    }
    snprintf(out, sizeof out, "%s/Globalus.bin", g_cache);
    rc = write_file(out, d, n);
    free(d);
    return rc;
}

static int materialise_elf(void)
{
    unsigned char *d, *elf = NULL;
    size_t         n = 0, elen = 0;
    char           out[B3_PATH_MAX];
    int            rc;

    d = src_slurp("default.xbe", &n);
    if (!d) {
        fprintf(stderr, "[Burnout3] iso: %s has no default.xbe\n", g_source);
        return -1;
    }
    rc = xbe_to_elf(d, n, &elf, &elen);
    free(d);
    if (rc != 0) {
        fprintf(stderr, "[Burnout3] iso: default.xbe is not an XBE image\n");
        return -1;
    }
    snprintf(out, sizeof out, "%s/burnout3.elf", g_cache);
    rc = write_file(out, elf, elen);
    free(elf);
    return rc;
}

/* --------------------------------------------------------- materialise -- */

/* Run every stage in `stages` (space separated) for `track_id` (NULL = the
 * global families), skipping any unit already attempted this run. */
/* ----------------------------------------------------------- THE PUMP HOOK
 * See the contract in burnout3_isodata.h.  Additive: with no callback
 * installed nothing below costs anything but a null test per unit. */

static b3_iso_progress_fn g_prog_cb   = NULL;
static void              *g_prog_user = NULL;
static int                g_prog_in   = 0;   /* re-entry guard */

void b3_iso_set_progress(b3_iso_progress_fn cb, void *user)
{
    g_prog_cb   = cb;
    g_prog_user = user;
}

/* Never re-entrant.  The callback draws a frame, and a frame that somehow
 * reached back into materialise() would run a stage from inside a stage --
 * the one thing the unsynchronised resolve ring cannot survive. */
static void prog_notify(const char *stage, const char *track_id,
                        int index, int total)
{
    if (!g_prog_cb || g_prog_in) return;
    g_prog_in = 1;
    g_prog_cb(stage, track_id, index, total, g_prog_user);
    g_prog_in = 0;
}

/* Run every stage the unit names that has not already run this process.
 * Returns 0 when every one of them ended ok -- this run or earlier -- and
 * non-zero when any failed, which is what tells the caller whether a file
 * still missing afterwards means "not on the disc" or "we could not look". */
#ifdef __EMSCRIPTEN__
/* WEB PORT (web/): the cache is an IDBFS mount, and IDBFS only reaches
 * IndexedDB on an explicit syncfs.  The natural place to spend that is the end
 * of a burst that actually RAN a stage -- the one moment when several MB of
 * freshly extracted assets exist in memory and nowhere else.
 *
 * Implemented in web/b3_web_lib.js (proxied to the main browser thread, where
 * the filesystem lives) rather than as an EM_ASM block, so this file can stay
 * on -std=c11. */
extern void b3_web_cache_sync(void);
#endif

/* `ran_out`, when given, is INCREMENTED by the number of stages that really
 * executed.  The caller needs it because "nothing ran" and "everything ran and
 * produced nothing" are opposite facts about the disc, and only the second one
 * is worth remembering in .absent.
 *
 * `stamped_out` counts something narrower and is the trigger for the retry in
 * resolve_inner(): units skipped PURELY on the strength of a stamp, i.e. on a
 * claim made by an EARLIER PROCESS and backed by nothing but the cache.  A unit
 * skipped because this process already ran it (the registry) is not counted --
 * that is first-hand knowledge, and re-running the family on it would re-extract
 * the whole car fleet every time the port asks for one of the files that
 * legitimately are not on the disc (Silver Lake's snow.wav, a car whose wheels
 * are part of its body).  Measured: without the distinction, a cold boot
 * re-ran car_meshes and awd mid-load. */
static int materialise(const char *stages, const char *track_id,
                       const char *why, int *ran_out, int *stamped_out)
{
    char  list[512], key[96];
    char *save = NULL, *tok;
    double t0 = now_s();
    int    ran = 0, failed = 0;
    int    prog_n = 0, prog_i = 0;

    if (!stages || !*stages) return 0;
    snprintf(list, sizeof list, "%s", stages);

    /* THE DENOMINATOR, before anything runs.  A unit already attempted this
     * process, or stamped by an earlier boot, costs nothing; counting it
     * would park a warm cache's bar at 90% and then jump.  Same two
     * predicates the loop below uses, so the count cannot disagree with it.
     * Only paid for when someone is actually watching. */
    if (g_prog_cb) {
        char  scan[512], skey[96];
        char *ssave = NULL, *stok;
        snprintf(scan, sizeof scan, "%s", stages);
        for (stok = strtok_r(scan, " ", &ssave); stok;
             stok = strtok_r(NULL, " ", &ssave)) {
            if (track_id) snprintf(skey, sizeof skey, "T:%s:%s", stok, track_id);
            else          snprintf(skey, sizeof skey, "G:%s", stok);
            if (unit_rc(skey) >= 0) continue;
            if (stok[0] != '@' && stamp_seen(skey)) continue;
            prog_n++;
        }
    }

    for (tok = strtok_r(list, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
        int rc = 0, prev;

        if (track_id) snprintf(key, sizeof key, "T:%s:%s", tok, track_id);
        else          snprintf(key, sizeof key, "G:%s", tok);
        prev = unit_rc(key);
        if (prev >= 0) { if (prev) failed++; continue; }
        /* Ran to completion on an earlier boot -- see THE STAMPS.  The two
         * '@' units are exempt: they are the boot-critical image pair, they
         * cost 10 ms, and $B3_ELF / $B3_GLOBALUS must name files that are
         * really there, so they stay self-healing rather than stamped. */
        if (tok[0] != '@' && stamp_seen(key)) {
            unit_mark(key, 0);
            if (stamped_out) (*stamped_out)++;
            continue;
        }

        /* One frame for the player, between units -- outside run_global()'s
         * chdir and outside quiet_begin()'s stdout redirect, which is the
         * only window where drawing and logging both still work. */
        prog_notify(tok, track_id, prog_i, prog_n);

        if (!strcmp(tok, "@globalus"))   rc = materialise_globalus();
        else if (!strcmp(tok, "@elf"))   rc = materialise_elf();
        /* THE MUSIC FAMILY borrows the per-track machinery for its unit key --
         * 'T:eatrax:<song>' -- but a song is not a race track, so it must not
         * reach run_track()'s TRACK_FN table.  Checked before the track_id
         * test, which is the whole reason the branch is here. */
        else if (!strcmp(tok, "eatrax")) rc = run_music(track_id);
        /* THE DJ VOICE BANKS do the same borrowing, for the same reason: the
         * unit key is 'T:djbank:<BANK>' and a bank is not a race track, so
         * this too has to be caught before the track_id test.  A per-track
         * commentary bank IS named for its track, which makes the ordering
         * look accidental and is exactly why it is spelled out. */
        else if (!strcmp(tok, "djbank")) rc = run_djbank(track_id);
        else if (track_id)               rc = run_track(tok, track_id);
        else                             rc = run_global(tok);
        unit_mark(key, rc != 0);
        if (rc == 0 && tok[0] != '@') stamp_write(key);
        ran++;
        prog_i++;
        if (rc != 0) {
            failed++;
            fprintf(stderr, "[Burnout3] iso: stage %s%s%s FAILED (for %s)\n",
                    tok, track_id ? " " : "", track_id ? track_id : "", why);
        }
    }
    if (prog_n) prog_notify(NULL, track_id, prog_i, prog_n);
    if (ran)
        logline("%-8s %s -> %s  (%.2f s%s)\n",
                track_id ? track_id : "global", stages, why, now_s() - t0,
                failed ? ", WITH FAILURES" : "");
#ifdef __EMSCRIPTEN__
    if (ran) b3_web_cache_sync();   /* WEB PORT: push the burst to IndexedDB */
#endif
    if (ran_out) *ran_out += ran;
    return failed;
}

/* ------------------------------------------------------------- resolution */

/* IS `p` (n chars, not NUL-terminated) THE NAME OF A DJ VOICE BANK?
 *
 * This is the whole of the audio/ interception rule in map_lookup() below, and
 * it is deliberately a WHITELIST rather than "anything under audio/ that is
 * not awd_ or rws_".  The banks are exactly two shapes:
 *
 *   DJ...                      the twelve global banks -- DJGEN, DJWWW, DJAS,
 *                              DJEU, DJUS, DJMCR, DJMBL, DJMRR, DJMEL, DJMRA,
 *                              DJMGP, DJMFO
 *   <AS|EU|US>_<A-Z><0-9>      the eighteen per-track commentary banks, which
 *                              name themselves after their track: US_C1,
 *                              AS_M1, EU_P2 ...
 *
 * Everything else under build/audio/ belongs to somebody else -- awd_* to the
 * awd stage, rws_* to the rws stage, EATrax0/EATrax1 and Movie to the
 * whole-disc xwb dump the game never runs -- and must fall through to the
 * GLOBAL_MAP rules that own it. */
static int is_dj_bank(const char *p, size_t n)
{
    if (n >= 2 && p[0] == 'D' && p[1] == 'J')
        return 1;
    if (n == 5 && p[2] == '_' &&
        p[3] >= 'A' && p[3] <= 'Z' && p[4] >= '0' && p[4] <= '9' &&
        (strncmp(p, "AS", 2) == 0 || strncmp(p, "EU", 2) == 0 ||
         strncmp(p, "US", 2) == 0))
        return 1;
    return 0;
}

/* `rel` is the path after "build/".  Fills `stages` and, for a per-track
 * path, `track_id`.  Returns 1 when a rule matched. */
static int map_lookup(const char *rel, const char **stages, char *track_id,
                      size_t tid_sz)
{
    size_t i;

    track_id[0] = '\0';
    /* THE MUSIC FAMILY: one unit per SONG, not per family -- see run_music().
     * The index comes out of the filename the game asked for, so the shuffle
     * picking song 31 decodes song 31 and nothing else.  Anything else under
     * music/ (eatrax.txt, most obviously) falls through to the GLOBAL_MAP rule
     * and gets the whole 44-song batch, which is what a manifest needs. */
    if (strncmp(rel, "music/", 6) == 0) {
        unsigned idx;
        char     tail[16];
        if (sscanf(rel + 6, "track_%u.%15s", &idx, tail) == 2 &&
            strcmp(tail, "wav") == 0 && idx < B3_MUSIC_TRACKS) {
            snprintf(track_id, tid_sz, "%u", idx);
            *stages = "eatrax";
            return 1;
        }
    }
    /* THE DJ VOICE BANKS: one unit per BANK, not per family -- see
     * run_djbank().  The bank the game is about to play names itself in the
     * path it opens (build/audio/DJGEN/003.wav), so that bank is extracted
     * and the other 32 are not.
     *
     * The DIRECTORY is the unit, so every file under it takes the rule -- the
     * .wma the extractor writes beside each .wav included.  is_dj_bank()
     * above decides which directories those are; anything else under audio/
     * falls through to the awd/rws rules in GLOBAL_MAP that own it. */
    if (strncmp(rel, "audio/", 6) == 0) {
        const char *p = rel + 6, *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : 0;
        if (n > 0 && n < tid_sz && is_dj_bank(p, n)) {
            memcpy(track_id, p, n);
            track_id[n] = '\0';
            *stages = "djbank";
            return 1;
        }
    }
    if (strncmp(rel, "tracks/", 7) == 0) {
        const char *p = rel + 7, *slash = strchr(p, '/');
        size_t n;
        if (!slash) return 0;
        n = (size_t)(slash - p);
        if (n == 0 || n >= tid_sz) return 0;
        memcpy(track_id, p, n);
        track_id[n] = '\0';
        for (i = 0; i < N_TRACK_MAP; i++)
            if (rule_hit(&TRACK_MAP[i], slash + 1)) {
                *stages = TRACK_MAP[i].stages;
                return 1;
            }
        return 0;
    }
    for (i = 0; i < N_GLOBAL_MAP; i++)
        if (rule_hit(&GLOBAL_MAP[i], rel)) {
            *stages = GLOBAL_MAP[i].stages;
            return 1;
        }
    return 0;
}

/* Run every family that could produce `rel`, once.  Adds to *ran; returns
 * non-zero if any of them failed.  Factored out because the stamp retry below
 * has to repeat EXACTLY this, the shared build/cars/ case included. */
static int run_owners(const char *stages, const char *tid, const char *rel,
                      const char *out, int *ran, int *stamped)
{
    int failed = materialise(stages, tid, rel, ran, stamped);

    /* build/cars/ is shared ground: car_meshes / car_paint cover the .bgv
     * fleet, but a track's .btv traffic cars land there through the per-track
     * traffic_cars mirror.  If the file is still missing, that is which half
     * it belongs to. */
    if (!path_exists(out) && strncmp(rel, "cars/", 5) == 0) {
        const char *t = getenv("B3_TRACK");
        if (t && *t)
            failed += materialise("traffic traffic_cars", t, rel, ran, stamped);
    }
    return failed;
}

/* IS THE REAL build/ TREE THERE?  The single question that decides where a
 * file the GAME owns -- a config, a log, a debug dump -- lives, asked the same
 * way by the write rule and by the non-asset read below so the two can never
 * disagree.  It is the TREE that is asked about, never the file: a read that
 * fell back to the cache whenever the build/ copy happened to be missing would
 * put a probe for a file just deleted from build/ straight onto a stale cache
 * copy of it, which is how tools/validate_aftertouch.py's cleared crash traces
 * came back from the dead.  A `build` that is a symlink to a directory counts,
 * which is what <cache>/.root/build is while a stage has chdir()ed into it. */
static int build_tree_here(void)
{
    return is_dir("build");
}

static const char *resolve_inner(const char *path, int allow_materialise)
{
    const char *rel, *stages = NULL, *tid;
    char        track_id[64];
    char       *out;
    int         failed, ran, stamped, dropped;

    if (g_mode != B3_DATA_ISO || !path) return path;
    if (strncmp(path, "build/", 6) != 0) return path;
    rel = path + 6;

    out = ring_next();
    snprintf(out, B3_PATH_MAX, "%s/%s", g_cache, rel);

    if (!allow_materialise || g_busy) {
        /* a stage's own re-entry (or an explicit no-materialise ask): never
         * run anything.  A WRITE no longer arrives here -- resolve_write()
         * below is the one rule for those. */
        if (!path_exists(out) && path_exists(path)) return path;
        return out;
    }

    if (!map_lookup(rel, &stages, track_id, sizeof track_id)) {
        /* NOT AN ASSET AT ALL: config, log, debug dump -- a file the GAME
         * writes rather than one a stage extracts, so resolve_write() put it
         * in the real build/ whenever build/ is there.  The read has to agree,
         * and has to agree BEFORE the cache is consulted: a copy misfiled into
         * the cache by the old write rule would otherwise shadow the file just
         * written next door for ever -- build/.isocache/crash_trace_001.log
         * and build/.isocache/debug_dump_084.txt were exactly that, and the
         * crash-trace numbering probe reading the shadow is what stopped
         * tools/validate_aftertouch.py seeing its own run.
         *
         * The cache is still the answer for a build-less tree, which is the
         * half of this rule that lets such a tree read back the mixer.cfg it
         * had nowhere else to write.  (The absent list is consulted below
         * rather than here on purpose: only a MAPPED path is ever recorded
         * absent, so a non-asset can never be in it.) */
        if (build_tree_here()) return path;
        return out;
    }

    if (path_exists(out)) return out;
    /* A path a completed unit did not produce on an earlier run: the disc
     * does not have it, and running the unit again would only prove that
     * a second time -- expensively.  See THE ABSENT LIST. */
    if (absent_known(rel)) return path_exists(path) ? path : out;

    if (!*stages) {
        /* A path the map knows but no stage owns -- the legacy build/textures/
         * fallback, and any future entry parked with an empty stage list.
         * Falling back to the real build/ when it is there is the point of
         * keeping the debug tree.
         *
         * build/music USED TO BE HERE, with a one-line "unavailable until
         * extracted" notice, because its stage forked ffmpeg and was left out
         * of the link.  It decodes in process now and has real stages, so it
         * takes the ordinary path below like every other asset. */
        return path_exists(path) ? path : out;
    }

    tid = track_id[0] ? track_id : NULL;

    /* THE MATERIALISE LOCK.  Everything below here mutates state that is
     * global to the process -- g_busy, the stamp files, the absent list --
     * and since the music module started pre-materialising the next song
     * on a worker, two threads can arrive.  Only the SLOW path takes it:
     * a cache hit returned above without ever coming near it, which is
     * what keeps a 0.5 s decode on the worker from stalling the frame
     * thread's own resolves. */
    pthread_mutex_lock(&g_mat_lock);
    /* Someone may have materialised it while we waited for the lock. */
    if (path_exists(out)) { pthread_mutex_unlock(&g_mat_lock); return out; }

    g_busy  = 1;
    ran     = 0;
    stamped = 0;
    failed  = run_owners(stages, tid, rel, out, &ran, &stamped);

    /* THE STAMP RETRY.  See "A STAMP IS ADVISORY, NEVER PROOF" above.  We are
     * only here because `out` was missing, so if nothing ran, the cache talked
     * us out of asking the disc -- and the disc is the only source that has to
     * work.  Throw the claim away and ask it for real. */
    dropped = 0;
    if (!path_exists(out) && stamped) {
        dropped = stamps_invalidate(stages, tid);
        if (strncmp(rel, "cars/", 5) == 0) {
            const char *t = getenv("B3_TRACK");
            if (t && *t) dropped += stamps_invalidate("traffic traffic_cars", t);
        }
        if (dropped) {
            logline("%s: the cache says its stage already ran, but the file is "
                    "not there -- dropping %d stale stamp(s) and re-extracting "
                    "from the disc\n", rel, dropped);
            failed = run_owners(stages, tid, rel, out, &ran, &stamped);
        }
    }
    g_busy = 0;
    pthread_mutex_unlock(&g_mat_lock);

    if (path_exists(out)) return out;

    /* NOT RESOLVED, and it must never be silent again -- the defect above
     * reached a FATAL with not one line explaining it.
     *
     * WHETHER TO REMEMBER IT AS ABSENT is a question about the DISC, and the
     * answer turns on who said so.  `failed` is 0 only when every owning unit
     * ended cleanly -- this call or earlier in this process -- so that is
     * first-hand evidence that the disc does not have the file, and it is what
     * keeps .absent doing its job (b3_sfx probes a snow.wav that Silver Lake
     * does not have on every boot; without the record, every boot re-extracts
     * the bank to rediscover it).
     *
     * The one thing that is NOT evidence is an unverified cache stamp: a claim
     * by an earlier process, backed by storage that may have lost the file.
     * The retry above turns that into evidence by re-running the unit, so the
     * only way to still be holding one here is a stamp we could not drop. */
    if (failed)
        logline("%s: NOT RESOLVED -- a stage that owns it failed (see above)\n",
                rel);
    else if (stamped && !dropped)
        logline("%s: NOT RESOLVED, and the only thing that stopped the disc "
                "being asked was a cache stamp this could not re-check -- NOT "
                "recording it absent\n", rel);
    else
        absent_record(rel);
    return path_exists(path) ? path : out;
}

/* THE WRITE RULE, and it is the mirror of the non-asset read above -- one
 * question, build_tree_here(), asked by both.
 *
 * A shimmed write is ALWAYS the game's own output: a config, a crash
 * trace, a T-key debug dump, a screenshot.  It is never a stage's, and that is
 * structural rather than a hope -- the Makefile builds the cextract stages and
 * this file as their own objects, WITHOUT src/burnout3_isoshim.h (see THE TWO
 * COMPILATION GROUPS there), so nothing that materialises an asset can reach
 * b3_iso_fopen at all.  Which is what makes it safe to say: if the real build/
 * tree is there, the file the game was told to write goes THERE, at the
 * literal path the caller named.
 *
 * It used to go to the cache whenever the file did not already exist, so every
 * FIRST write of a file landed in build/.isocache/ -- the T-key dump's .txt
 * went there while its .bmp went to build/ (SDL_SaveBMP is not shimmed), and
 * tools/validate_aftertouch.py cleared build/crash_trace_*.log and then could
 * not see the trace the run had just written.
 *
 * The cache is still the answer when build/ is ENTIRELY ABSENT: a checkout
 * with no build/ tree must still boot off the disc and have somewhere to put
 * its mixer.cfg.  (With the default cache that window is narrow -- b3_iso_init
 * creates build/.isocache, and build/ with it -- but $B3_ISO_CACHE moves the
 * cache anywhere, and then build/ really can stay missing.) */
static const char *resolve_write(const char *path)
{
    char *out;

    if (g_mode != B3_DATA_ISO || !path) return path;
    if (strncmp(path, "build/", 6) != 0) return path;
    if (build_tree_here()) return path;

    out = ring_next();
    snprintf(out, B3_PATH_MAX, "%s/%s", g_cache, path + 6);
    return out;
}

const char *b3_iso_resolve(const char *path)
{
    return resolve_inner(path, 1);
}

const char *b3_iso_resolve_nomat(const char *path)
{
    return resolve_write(path);
}

/* --------------------------------------------------------- the primitives */

static int mode_writes(const char *mode)
{
    return mode && (strchr(mode, 'w') || strchr(mode, 'a')
                    || strchr(mode, '+'));
}

FILE *b3_iso_fopen(const char *path, const char *mode)
{
    const char *p;

    if (g_mode != B3_DATA_ISO) return fopen(path, mode);
    if (mode_writes(mode)) {
        p = resolve_write(path);
        /* mkdir -p the parent, in WHICHEVER tree the write landed in -- a
         * fresh cache has no subdirectories at all, and a real build/ need not
         * have the one this path names (build/debug/).  Only for a "build/..."
         * path: an absolute $B3_DRIVE_LOG is the caller's business, not ours. */
        if (strncmp(path, "build/", 6) == 0) mkdir_p_parent(p);
        return fopen(p, mode);
    }
    return fopen(b3_iso_resolve(path), mode);
}

int b3_iso_access(const char *path, int mode)
{
    if (g_mode != B3_DATA_ISO) return access(path, mode);
    return access(b3_iso_resolve(path), mode);
}

/* The availability question, answered off the disc's own track list rather
 * than by extracting the answer.  See the header note.  Memoised because the
 * selector asks 36 times in one loop and the answer cannot change. */
int b3_iso_track_available(const char *track_id)
{
    static struct { char id[16]; int ok; } memo[64];
    static int    n_memo = 0;
    char          id[64], track_dir[B3_ROOT_MAX];
    int           i, ok;

    if (g_mode != B3_DATA_ISO) return -1;
    if (!track_id || !*track_id) return 0;
    for (i = 0; i < n_memo; i++)
        if (strcmp(memo[i].id, track_id) == 0) return memo[i].ok;

    quiet_begin();
    ok = cx_resolve_track(g_source, track_id, id, sizeof id,
                          track_dir, sizeof track_dir) == 0;
    quiet_end();

    if (n_memo < (int)(sizeof memo / sizeof memo[0])
        && strlen(track_id) < sizeof memo[0].id) {
        snprintf(memo[n_memo].id, sizeof memo[0].id, "%s", track_id);
        memo[n_memo].ok = ok;
        n_memo++;
    }
    return ok;
}

/* THE SAME QUESTION, FOR MUSIC, AND FOR THE SAME REASON.
 *
 * b3_music_init() decides which of the 44 songs are playable by opening each
 * one.  Under materialise-on-miss that probe IS the extraction: 44 fopens
 * would decode 722 MB of PCM and cost 22.7 s at boot, to answer a question
 * about a disc that either has the two wave banks or does not.
 *
 * So availability means what it should mean -- THE DISC HAS THIS SONG --
 * answered off the bank the song lives in.  A stat of two files, memoised, in
 * place of decoding the whole soundtrack.  The song still materialises when
 * the shuffle actually reaches it.
 *
 * Same contract as b3_iso_track_available(): 1 = yes, 0 = no, -1 in build
 * mode, meaning "not my question, probe build/ the way you always did". */
/* IS IT ALREADY ON DISK?  The question a caller has to be able to ask
 * before it decides whether resolving a path is free or is a stage.
 *
 * b3_iso_resolve() cannot answer it, because asking IS the extraction --
 * that is the whole design.  This looks only at the cache and never runs
 * anything, so the music module can tell "open it now, on this frame"
 * from "hand it to the worker and come back", which is what keeps a
 * mid-race song change off the frame path. */
int b3_iso_is_materialised(const char *path)
{
    char        out[B3_PATH_MAX];
    const char *rel, *stages = NULL;
    char        track_id[64];

    if (!path) return 0;
    if (g_mode != B3_DATA_ISO) return path_exists(path);
    if (strncmp(path, "build/", 6) != 0) return path_exists(path);
    rel = path + 6;

    snprintf(out, sizeof out, "%s/%s", g_cache, rel);
    if (path_exists(out)) return 1;              /* already in the cache */

    /* IT MUST MIRROR resolve_inner() EXACTLY, and the first version of it
     * did not: it answered "yes" for anything sitting in a real build/
     * tree next door.  But resolve_inner does NOT prefer that copy for a
     * mapped path with an owning stage -- it extracts into the cache
     * regardless -- so the music module believed a song was free, opened
     * it on the frame thread, and took the whole 0.45 s decode it was
     * built to avoid.  Only these two cases resolve without a stage. */
    if (!map_lookup(rel, &stages, track_id, sizeof track_id))
        return 1;                                /* not an asset at all  */
    return (!stages || !*stages) ? 1 : 0;        /* mapped, but unowned  */
}

int b3_iso_music_available(int song)
{
    /* -1 = not asked yet, 0/1 = the answer, per bank. */
    static int memo[2] = { -1, -1 };
    int        bank;

    if (g_mode != B3_DATA_ISO) return -1;
    if (song < 0 || song >= B3_MUSIC_TRACKS) return 0;
    bank = song / (B3_MUSIC_TRACKS / 2);
    if (bank < 0 || bank > 1) return 0;

    if (memo[bank] < 0) {
#ifdef B3_HAVE_CX_SRC
        unsigned long long sz = 0;
        char rel[64];
        snprintf(rel, sizeof rel, "Tracks/_EATrax%d.xwb", bank);
        memo[bank] = (g_src && cx_src_stat(g_src, rel, &sz) == 0 && sz > 0);
#else
        memo[bank] = 0;
#endif
    }
    return memo[bank];
}

/* AND ONCE MORE, FOR THE DJ.  Crash FM decides which of its banks it can play
 * the same way b3_music_init() does -- by opening one -- and under
 * materialise-on-miss that probe IS the extraction.  So the answer comes off
 * the disc's own directory: a stat of the bank file, memoised.
 *
 * THE LIMIT, AND IT IS A REAL ONE.  A bank's source path is not derivable
 * from its output name in general.  The twelve global banks are
 * Tracks/<prefix><NAME>.xwb, where the prefix is the LANGUAGE (E_ English,
 * I_ S_ F_ G_ J_ for the rest) and only the disc knows which of those it
 * shipped, so all six are tried.  The eighteen per-track banks are all called
 * E_DJRACE.xwb, one per Tracks/<REGION>/<TRACK>_V1/ -- their output name
 * ("US_C1") appears nowhere in their path, and finding one means reading
 * headers, which is cx_extract_xwb_one()'s job and not a probe's.  Those get
 * -1, "I do not know": the caller then does what it did before, which for a
 * per-track bank is the right answer anyway, because a track that is loading
 * has already been resolved.
 *
 * Same contract as the two probes above: 1 = yes, 0 = no, -1 = not my
 * question. */
int b3_iso_dj_available(const char *bank)
{
    static struct { char name[24]; int ok; } memo[32];
    static int    n_memo = 0;
    int           i, ok = 0;

    if (g_mode != B3_DATA_ISO) return -1;
    if (!bank || !*bank) return 0;
    if (bank[0] != 'D' || bank[1] != 'J') return -1;   /* per-track: unknown */
    for (i = 0; i < n_memo; i++)
        if (strcmp(memo[i].name, bank) == 0) return memo[i].ok;

#ifdef B3_HAVE_CX_SRC
    {
        /* the six language prefixes retail builds the filename with */
        static const char PFX[] = "EISFGJ";
        unsigned long long sz = 0;
        char               rel[64];
        size_t             k;

        for (k = 0; !ok && k + 1 < sizeof PFX; k++) {
            snprintf(rel, sizeof rel, "Tracks/%c_%s.xwb", PFX[k], bank);
            ok = g_src && cx_src_stat(g_src, rel, &sz) == 0 && sz > 0;
        }
    }
#endif

    if (n_memo < (int)(sizeof memo / sizeof memo[0])
        && strlen(bank) < sizeof memo[0].name) {
        snprintf(memo[n_memo].name, sizeof memo[0].name, "%s", bank);
        memo[n_memo].ok = ok;
        n_memo++;
    }
    return ok;
}

/* --------------------------------------------------------------- the init */

int         b3_iso_mode(void)   { return g_mode; }
const char *b3_iso_source(void) { return g_source; }
const char *b3_iso_cache(void)  { return g_cache; }

/* The remembered image path: one line in build/iso_path.txt, written on the
 * first successful --iso=<path> run so the next bare ./burnout3 finds it. */
static int read_remembered(char *out, size_t cap)
{
    FILE *f = fopen("build/iso_path.txt", "r");
    size_t n;

    if (!f) return 0;
    if (!fgets(out, (int)cap, f)) { fclose(f); return 0; }
    fclose(f);
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return n > 0;
}

static void remember(const char *p)
{
    FILE *f;
    if (mkdir_p("build") != 0) return;
    f = fopen("build/iso_path.txt", "w");
    if (!f) return;
    fprintf(f, "%s\n", p);
    fclose(f);
}

void b3_iso_init(int argc, char **argv)
{
    const char *cli_path = NULL, *e;
    char        remembered[B3_ROOT_MAX];
    int         want = -1;          /* -1 undecided, 0 build, 1 iso */
    int         from_cli = 0, i;

    if (g_inited) return;
    /* main() calls this as its first statement, so whoever we are IS the
     * main thread -- the one allowed to run a chdir stage (see run_global). */
    g_main_thread = pthread_self();
    g_inited = 1;
    g_verbose = (e = getenv("B3_ISO_VERBOSE")) && *e && strcmp(e, "0");

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--build") || !strcmp(a, "--data=build")) {
            want = 0;
        } else if (!strcmp(a, "--iso") || !strcmp(a, "--data=iso")) {
            want = 1;
        } else if (!strncmp(a, "--iso=", 6)) {
            want = 1; cli_path = a + 6; from_cli = 1;
        }
    }
    if (want < 0) {
        e = getenv("B3_DATA_MODE");
        if (e && !strcmp(e, "build")) want = 0;
        else if (e && !strcmp(e, "iso")) want = 1;
    }
    if (want == 0) {
        g_mode = B3_DATA_BUILD;
        return;                     /* byte-for-byte the old behaviour */
    }

    /* image path: CLI > $B3_ISO > build/iso_path.txt > $B3_GAME_ROOT */
    if (cli_path && *cli_path && path_exists(cli_path)) {
        snprintf(g_source, sizeof g_source, "%s", cli_path);
    } else if ((e = getenv("B3_ISO")) && *e && path_exists(e)) {
        snprintf(g_source, sizeof g_source, "%s", e);
        from_cli = 0;
    } else if (read_remembered(remembered, sizeof remembered)
               && path_exists(remembered)) {
        snprintf(g_source, sizeof g_source, "%s", remembered);
        from_cli = 0;
    } else if ((e = getenv(B3_ISO_DEFAULT_ENV)) && *e && path_exists(e)) {
        snprintf(g_source, sizeof g_source, "%s", e);
        from_cli = 0;
    } else {
        g_mode = B3_DATA_BUILD;
        g_source[0] = '\0';
        printf("[Burnout3] iso: no disc image found "
               "(--iso=<path>, $B3_ISO, build/iso_path.txt, $B3_GAME_ROOT) -- "
               "falling back to the pre-extracted build/ tree\n");
        fflush(stdout);
        return;
    }

    g_mode = B3_DATA_ISO;
    if (from_cli) remember(g_source);

#ifdef B3_HAVE_CX_SRC
    /* THE STAGE SEAM.  cx_src.h keeps the string ABI: a stage still receives
     * `game_dir` and still joins "%s/pveh/vlist.bin" onto it -- cx_vfs_bind()
     * is what makes those joined paths resolve INSIDE the image instead of on
     * the filesystem.  Without this call every stage would look for the disc's
     * files next to a .iso that is a single file, and find nothing.  A
     * DIRECTORY source binds nothing and stays a pure libc passthrough, which
     * is why dir-sourced extraction is bit-for-bit the code it always was. */
    g_src = cx_src_open(g_source);
    if (g_src) cx_vfs_bind(g_src, g_source);
    if (!g_src) {
        g_mode = B3_DATA_BUILD;
        printf("[Burnout3] iso: %s is neither an XISO image nor a game "
               "directory -- falling back to the pre-extracted build/ tree\n",
               g_source);
        fflush(stdout);
        return;
    }
#endif

    e = getenv("B3_ISO_CACHE");
    if (e && *e) snprintf(g_cache, sizeof g_cache, "%s", e);
    else         snprintf(g_cache, sizeof g_cache, "build/.isocache");
    if (mkdir_p(g_cache) != 0) {
        g_mode = B3_DATA_BUILD;
        fprintf(stderr, "[Burnout3] iso: cannot create the cache %s -- "
                "falling back to build/\n", g_cache);
        return;
    }
    {   /* absolute, because the global stages chdir() into their out_root */
        char abs[B3_ROOT_MAX];
        if (g_cache[0] != '/' && realpath(g_cache, abs))
            snprintf(g_cache, sizeof g_cache, "%s", abs);
        if (!getcwd(g_repo, sizeof g_repo)) g_repo[0] = '\0';
    }

    /* <cache>/.root is the REPO-ROOT STAND-IN the car/art/generator families
     * write through: they emit the literal "build/cars/..." and postfx_art
     * records CWD-relative paths in its manifest, so the child really has to
     * be called `build`.  One symlink buys both. */
    /* Before anything consults .stamps or .absent. */
    cache_generation_check();

    snprintf(g_root, sizeof g_root, "%s/.root", g_cache);
    mkdir_p(g_root);
    {
        char link[B3_PATH_MAX];
        snprintf(link, sizeof link, "%s/build", g_root);
        if (!path_exists(link) && symlink(g_cache, link) != 0
            && errno != EEXIST)
            fprintf(stderr, "[Burnout3] iso: cannot link %s -> %s (%s)\n",
                    link, g_cache, strerror(errno));
    }

    setenv("B3_GAME_DIR", g_source, 1);
    if (g_repo[0]) setenv("B3_REPO_DIR", g_repo, 1);

    /* The two dump-direct files, eagerly: $B3_ELF and $B3_GLOBALUS have to
     * name real files before the first stage that reads them runs, and the
     * track-select screen wants both on its first frame. */
    {
        char p[B3_PATH_MAX];

        materialise("@elf @globalus", NULL, "the retail image", NULL, NULL);
        snprintf(p, sizeof p, "%s/burnout3.elf", g_cache);
        setenv("B3_ELF", p, 1);
        setenv("B3_XBE_ELF", p, 1);
        snprintf(p, sizeof p, "%s/Globalus.bin", g_cache);
        setenv("B3_GLOBALUS", p, 1);
    }

    logline("source %s\n", g_source);
    logline("cache  %s  (--build for the pre-extracted tree)\n", g_cache);
}
