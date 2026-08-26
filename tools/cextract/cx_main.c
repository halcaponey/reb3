/* cx_main.c -- `cxtract`, the driver for the C asset-extraction pipeline.
 *
 *     cxtract --track <id> --out <dir> [--game <dir>] [--only <stage>]
 *     cxtract --all-global --out <dir> [--game <dir>] [--repo <dir>]
 *
 * TWO MODES, because the pipeline has two kinds of stage.
 *
 *   --track (the default)  resolves the track, creates <dir>, then runs every
 *                          PER-TRACK stage that is compiled in, timing each.
 *   --all-global           runs every GLOBAL stage -- the car, art, audio and
 *                          generator families -- once, into one output root.
 *
 * Stage order and the CX_HAVE_* gating convention are documented in
 * cx_extract.h; the short of it is that the agents land their .c files
 * independently and build.sh globs *.c, so both tables must link with any
 * subset present.  A per-track stage whose macro is undefined is reported as
 * "not built" and skipped -- there is no reference to its symbol in the
 * translation unit at all, so the link succeeds without it.  A global stage
 * that has not landed contributes no list entry at all (its agent's
 * CX_GLOBAL_STAGES_* macro does not exist and expands to nothing), which is
 * the same tolerance reached the only way it can be reached for a table whose
 * membership the driver cannot know in advance.
 *
 * `--game` TAKES EITHER: the mounted game directory, or the Xbox ISO itself.
 * cx_src_open() sniffs which (a directory, or a regular file carrying the
 * XISO volume descriptor) and the whole pipeline reads through it -- see the
 * SOURCE section of cx_extract.h for the seam.  It defaults to $B3_ISO, then
 * $B3_GAME_DIR, then to the same literal path
 * tools/extract_tlist.py falls back to, so the C and python pipelines address
 * the same dump by default.  `--track` defaults to $B3_TRACK then US_C3_V1,
 * matching extract_tlist.DEFAULT_TRACK.  `--repo` sets $B3_REPO_DIR for the
 * generator family, whose inputs (the port's own annotated headers under src,
 * tools/emulate_sfx.py, build/burnout3.elf) live in the PORT, not in the game
 * dump; it is read-only to them -- generated headers go to --out, never back
 * into the source tree they were derived from.
 */
#define _POSIX_C_SOURCE 200809L

#include "cx_common_c.h"
#include "cx_extract.h"
#include "cx_src.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only for the generator family's repo-root default, so the driver can point
 * $B3_ELF at the same image the modules would find on their own.  Guarded on
 * agent G's own macro: no agent G, no cx_common_g.c to link against. */
#ifdef CX_HAVE_GEN_TRACKSELECT
#  include "cx_common_g.h"
#endif

/* NO RETAIL PATH IS COMPILED IN.  This tree ships no game content: point
 * $B3_GAME_ROOT at YOUR OWN dump -- the folder holding default.xbe, or an
 * .xiso image of it.  $B3_ISO and $B3_GAME_DIR name the same source and are
 * checked first.  See docs/ASSETS.md. */
#define CX_DEFAULT_GAME_DIR ""
#define CX_DEFAULT_TRACK "US_C3_V1"     /* extract_tlist.DEFAULT_TRACK */

typedef struct {
    const char *name;
    cx_stage_fn fn;                     /* NULL == module not built yet */
} cx_stage;

/* The registry.  X(MACRO, name) from cx_extract.h's CX_STAGE_LIST expands to
 * either the real entry or a NULL placeholder, so the summary can name the
 * stages that are still pending without referencing their symbols. */
#define CX_ENTRY(UP, low)                                                     \
    { #low,                                                                   \
      CX_STAGE_PICK(UP, low) },

#ifdef CX_HAVE_TLIST
#  define CX_FN_TLIST cx_extract_tlist
#else
#  define CX_FN_TLIST NULL
#endif
#ifdef CX_HAVE_TRACK
#  define CX_FN_TRACK cx_extract_track
#else
#  define CX_FN_TRACK NULL
#endif
#ifdef CX_HAVE_TEXTURES
#  define CX_FN_TEXTURES cx_extract_textures
#else
#  define CX_FN_TEXTURES NULL
#endif
#ifdef CX_HAVE_COLLISION
#  define CX_FN_COLLISION cx_extract_collision
#else
#  define CX_FN_COLLISION NULL
#endif
#ifdef CX_HAVE_ENVMAP
#  define CX_FN_ENVMAP cx_extract_envmap
#else
#  define CX_FN_ENVMAP NULL
#endif
#ifdef CX_HAVE_BGD_PATHS
#  define CX_FN_BGD_PATHS cx_extract_bgd_paths
#else
#  define CX_FN_BGD_PATHS NULL
#endif
#ifdef CX_HAVE_TRAFFIC
#  define CX_FN_TRAFFIC cx_extract_traffic
#else
#  define CX_FN_TRAFFIC NULL
#endif
/* Runs immediately after TRAFFIC and that ordering is load-bearing: it reads
 * the roster back out of the out dir's traffic.bin. */
#ifdef CX_HAVE_TRAFFIC_CARS
#  define CX_FN_TRAFFIC_CARS cx_extract_traffic_cars
#else
#  define CX_FN_TRAFFIC_CARS NULL
#endif
#ifdef CX_HAVE_NAV_EDGES
#  define CX_FN_NAV_EDGES cx_extract_nav_edges
#else
#  define CX_FN_NAV_EDGES NULL
#endif
#ifdef CX_HAVE_START_GRID
#  define CX_FN_START_GRID cx_extract_start_grid
#else
#  define CX_FN_START_GRID NULL
#endif
#ifdef CX_HAVE_PACE
#  define CX_FN_PACE cx_extract_pace
#else
#  define CX_FN_PACE NULL
#endif
#ifdef CX_HAVE_PROPS
#  define CX_FN_PROPS cx_extract_props
#else
#  define CX_FN_PROPS NULL
#endif
#ifdef CX_HAVE_SCENERY
#  define CX_FN_SCENERY cx_extract_scenery
#else
#  define CX_FN_SCENERY NULL
#endif
#ifdef CX_HAVE_LIGHT_PROBES
#  define CX_FN_LIGHT_PROBES cx_extract_light_probes
#else
#  define CX_FN_LIGHT_PROBES NULL
#endif

#define CX_STAGE_PICK(UP, low) CX_FN_##UP

static const cx_stage CX_STAGES[] = {
    CX_STAGE_LIST(CX_ENTRY)
};
#define CX_NSTAGES (sizeof(CX_STAGES) / sizeof(CX_STAGES[0]))

/* ====================================================== the GLOBAL registry
 *
 * Assembled from the per-agent lists in cx_extract.h.  Each agent owns the
 * list of ITS global stages, inside its own block, because the driver cannot
 * name a stage it has never heard of and the agents land at different times.
 * An agent that has not landed has no list macro; it is defined empty here, so
 * the concatenation below is exactly the set that exists right now.
 *
 * The trailing sentinel is not decoration: with every list empty the
 * initialiser would otherwise be `{ }`, a zero-length array, and CX_NGLOBALS
 * would have to be special-cased everywhere it sizes a local. */
#ifndef CX_GLOBAL_STAGES_A
#  define CX_GLOBAL_STAGES_A(X)
#endif
#ifndef CX_GLOBAL_STAGES_B
#  define CX_GLOBAL_STAGES_B(X)
#endif
#ifndef CX_GLOBAL_STAGES_C
#  define CX_GLOBAL_STAGES_C(X)
#endif
#ifndef CX_GLOBAL_STAGES_D
#  define CX_GLOBAL_STAGES_D(X)
#endif
#ifndef CX_GLOBAL_STAGES_E
#  define CX_GLOBAL_STAGES_E(X)
#endif
#ifndef CX_GLOBAL_STAGES_F
#  define CX_GLOBAL_STAGES_F(X)
#endif
#ifndef CX_GLOBAL_STAGES_G
#  define CX_GLOBAL_STAGES_G(X)
#endif
/* An escape hatch for anything that does not belong to one of the seven
 * lettered blocks (a local experiment, a stage added out of band). */
#ifndef CX_GLOBAL_STAGES_EXTRA
#  define CX_GLOBAL_STAGES_EXTRA(X)
#endif

#define CX_GLOBAL_STAGE_LIST(X) \
    CX_GLOBAL_STAGES_A(X)       \
    CX_GLOBAL_STAGES_B(X)       \
    CX_GLOBAL_STAGES_C(X)       \
    CX_GLOBAL_STAGES_D(X)       \
    CX_GLOBAL_STAGES_E(X)       \
    CX_GLOBAL_STAGES_F(X)       \
    CX_GLOBAL_STAGES_G(X)       \
    CX_GLOBAL_STAGES_EXTRA(X)

/* MANUAL-ONLY global stages: registered and runnable, but skipped by a bare
 * --all-global.  cx_extract_physics_params is the standing case -- it mines
 * the Ghidra bridge rather than the game files, so a full run must not depend
 * on it being there, while `--only physics_params` still works.  Guarded on
 * its own CX_HAVE_ macro so the name costs nothing when the module is absent,
 * and extensible without touching the driver through CX_GLOBAL_MANUAL_EXTRA. */
#ifndef CX_GLOBAL_MANUAL_D
#  ifdef CX_HAVE_PHYSICS_PARAMS
#    define CX_GLOBAL_MANUAL_D(X) X(physics_params)
#  else
#    define CX_GLOBAL_MANUAL_D(X)
#  endif
#endif
#ifndef CX_GLOBAL_MANUAL_EXTRA
#  define CX_GLOBAL_MANUAL_EXTRA(X)
#endif

#define CX_GLOBAL_MANUAL_LIST(X) \
    CX_GLOBAL_MANUAL_D(X)        \
    CX_GLOBAL_MANUAL_EXTRA(X)

typedef int (*cx_global_fn)(const char *game_dir, const char *out_root);

typedef struct {
    const char  *name;
    cx_global_fn fn;
    int          manual;                /* skipped by a bare --all-global */
} cx_global_stage;

#define CX_GENTRY(low)  { #low, cx_extract_##low, 0 },
#define CX_GMENTRY(low) { #low, cx_extract_##low, 1 },

static const cx_global_stage CX_GLOBALS[] = {
    CX_GLOBAL_STAGE_LIST(CX_GENTRY)
    CX_GLOBAL_MANUAL_LIST(CX_GMENTRY)
    { NULL, NULL, 0 }                   /* sentinel; see above */
};
#define CX_NGLOBALS (sizeof(CX_GLOBALS) / sizeof(CX_GLOBALS[0]) - 1)

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --track <id> --out <dir> [--game <dir>] [--only <stage>]\n"
        "       %s --all-global --out <dir> [--game <dir>] [--repo <dir>]\n"
        "\n"
        "  --track <id>   track id, e.g. US_C3_V1   (default $B3_TRACK, else "
        CX_DEFAULT_TRACK ")\n"
        "  --all-global   run the GLOBAL stages (car / art / audio / gen)\n"
        "                 once instead of the per-track ones\n"
        "  --out <dir>    output directory.  Per-track: artefacts land in it,\n"
        "                 like python's build/tracks/<id>.  Global: the repo-\n"
        "                 root stand-in -- assets to <dir>/build, generated\n"
        "                 headers to <dir>/gen\n"
        "  --game <src>   mounted game root OR the Xbox ISO -- auto-detected\n"
        "                 (default $B3_ISO, then $B3_GAME_DIR)\n"
        "  --repo <dir>   port repo root, for the generator family's inputs\n"
        "                 (src/*.h, build/burnout3.elf)  (default $B3_REPO_DIR)\n"
        "  --gen-out <d>  send generated headers here instead of <out>/gen\n"
        "  --only <stage> run one stage only; repeatable; works in both modes.\n"
        "                 Also the only way to run a stage --list-global marks\n"
        "                 `built (--only)`, which a bare --all-global skips\n"
        "  --check        --all-global: compare instead of write, the way\n"
        "                 tools/gen_trackselect.py --check does\n"
        "  --check-dir <d> what --check compares against (default <repo>/src)\n"
        "  --list         list the per-track stages and whether they are "
        "built\n"
        "  --list-global  list the global stages that are registered\n"
        "\nper-track stages:", argv0, argv0);
    for (size_t i = 0; i < CX_NSTAGES; i++)
        fprintf(stderr, " %s", CX_STAGES[i].name);
    fprintf(stderr, "\nglobal stages:");
    if (!CX_NGLOBALS)
        fprintf(stderr, " (none registered)");
    for (size_t i = 0; i < CX_NGLOBALS; i++)
        fprintf(stderr, " %s", CX_GLOBALS[i].name);
    fprintf(stderr, "\n");
}

static void list_stages(void)
{
    size_t built = 0;

    printf("%-20s %s\n", "stage", "state");
    for (size_t i = 0; i < CX_NSTAGES; i++) {
        int ok = CX_STAGES[i].fn != NULL;
        printf("%-20s %s\n", CX_STAGES[i].name, ok ? "built" : "not built");
        built += ok ? 1u : 0u;
    }
    printf("-- %zu/%zu per-track stages built\n", built, CX_NSTAGES);
}

static void list_globals(void)
{
    printf("%-20s %s\n", "global stage", "state");
    for (size_t i = 0; i < CX_NGLOBALS; i++)
        printf("%-20s %s\n", CX_GLOBALS[i].name,
               CX_GLOBALS[i].manual ? "built (--only)" : "built");
    printf("-- %zu global stage(s) registered\n", CX_NGLOBALS);
    if (!CX_NGLOBALS)
        printf("   (no agent has defined a CX_GLOBAL_STAGES_* list yet -- see\n"
               "    the GLOBAL STAGES section of cx_extract.h)\n");
}

/* Make `p` absolute, into `out`.  realpath() when the path exists, otherwise
 * getcwd() + '/' + p, so a not-yet-created output root still resolves.  The
 * global run chdir()s, and every path handed to a stage has to survive that. */
/* strlcpy, so the deliberate truncation is a memcpy rather than a printf the
 * compiler is entitled to warn about. */
static void copy_str(char *out, size_t cap, const char *s)
{
    size_t n = strlen(s);

    if (!cap)
        return;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static void abspath(char *out, size_t cap, const char *p)
{
    char buf[PATH_MAX];
    char cwd[PATH_MAX];

    if (!p || !*p) {
        copy_str(out, cap, "");
        return;
    }
    if (realpath(p, buf) != NULL) {
        copy_str(out, cap, buf);
        return;
    }
    if (p[0] == '/' || !getcwd(cwd, sizeof(cwd))) {
        copy_str(out, cap, p);
    } else {
        char joined[2 * PATH_MAX + 2];

        snprintf(joined, sizeof(joined), "%s/%s", cwd, p);
        copy_str(out, cap, joined);
    }
}

static int readable(const char *p)
{
    FILE *f = fopen(p, "rb");

    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------- the source
 * ONE CxSrc for the whole run, bound as the process-wide source so that
 * every "%s/..."-style join the stages already do resolves through it.  A
 * directory binds nothing and leaves the pipeline on plain libc; an ISO
 * binds the image.  See cx_src.h. */
static CxSrc *g_source;

static void close_source(void)
{
    cx_src_close(g_source);
    g_source = NULL;
}

static int open_source(const char *game)
{
    g_source = cx_src_open(game);
    if (!g_source) {
        fprintf(stderr, "cextract: %s is neither a game directory nor an "
                        "Xbox ISO\n", game);
        return 1;
    }
    cx_vfs_bind(g_source, game);
    atexit(close_source);
    return 0;
}

static const char *source_kind(void)
{
    return cx_src_kind(g_source) == CX_SRC_XISO ? "xiso image" : "directory";
}

/* python's `any(only == name)`, with an empty selection meaning "all". */
static int wanted(const char *const *only, size_t n_only, const char *name)
{
    size_t k;

    if (!n_only)
        return 1;
    for (k = 0; k < n_only; k++)
        if (!strcmp(only[k], name))
            return 1;
    return 0;
}

/* --------------------------------------------------------------- per-track */
static int run_track_mode(const char *game, const char *track,
                          const char *out, const char *const *only,
                          size_t n_only)
{
    char   track_id[64], track_dir[4096];
    double t_all;
    int    failed = 0, ran = 0, skipped = 0;
    double dur[CX_NSTAGES];
    int    rcs[CX_NSTAGES];
    int    did[CX_NSTAGES];

#ifdef CX_HAVE_RESOLVE_TRACK
    if (cx_resolve_track(game, track, track_id, sizeof(track_id),
                         track_dir, sizeof(track_dir)) != 0)
        return 1;
#else
    if (cxc_resolve_track(game, track, track_id, sizeof(track_id),
                          track_dir, sizeof(track_dir)) != 0)
        return 1;
#endif

    if (cxc_mkdir_p(out) != 0) {
        fprintf(stderr, "cextract: cannot create %s\n", out);
        return 1;
    }

    printf("cxtract: track %s\n", track_id);
    printf("         game  %s  [%s]\n", game, source_kind());
    printf("         src   %s\n", track_dir);
    printf("         out   %s\n", out);

    t_all = cxc_now_s();
    for (size_t i = 0; i < CX_NSTAGES; i++) {
        double t0;

        did[i] = 0;
        dur[i] = 0.0;
        rcs[i] = 0;
        if (!wanted(only, n_only, CX_STAGES[i].name))
            continue;
        if (!CX_STAGES[i].fn) {
            skipped++;
            continue;
        }
        printf("\n---- %s ----------------------------------------------\n",
               CX_STAGES[i].name);
        fflush(stdout);
        t0 = cxc_now_s();
        rcs[i] = CX_STAGES[i].fn(game, track_dir, track_id, out);
        dur[i] = cxc_now_s() - t0;
        did[i] = 1;
        ran++;
        if (rcs[i] != 0)
            failed++;
    }
    t_all = cxc_now_s() - t_all;

    printf("\n=========================================================\n");
    printf("%-20s %-10s %9s\n", "stage", "result", "seconds");
    for (size_t i = 0; i < CX_NSTAGES; i++) {
        if (did[i])
            printf("%-20s %-10s %9.3f\n", CX_STAGES[i].name,
                   rcs[i] == 0 ? "ok" : "FAILED", dur[i]);
        else if (!CX_STAGES[i].fn)
            printf("%-20s %-10s %9s\n", CX_STAGES[i].name, "not built", "-");
    }
    printf("---------------------------------------------------------\n");
    printf("%d stage(s) run, %d failed, %d not built, %.3f s total\n",
           ran, failed, skipped, t_all);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------------------ global */
static int run_global_mode(const char *game, const char *out,
                           const char *const *only, size_t n_only,
                           int check, const char *check_dir)
{
    double t_all;
    int    failed = 0, ran = 0;
    double dur[CX_NGLOBALS + 1];
    int    rcs[CX_NGLOBALS + 1];
    int    did[CX_NGLOBALS + 1];

    if (!CX_NGLOBALS) {
        fprintf(stderr, "cextract: no global stages are registered\n");
        list_globals();
        return 1;
    }

    /* --check never writes, so it needs no output root at all. */
    if (!check && cxc_mkdir_p(out) != 0) {
        fprintf(stderr, "cextract: cannot create %s\n", out);
        return 1;
    }

    printf("cxtract: global stages\n");
    printf("         game  %s  [%s]\n", game, source_kind());
    printf("         repo  %s\n", getenv("B3_REPO_DIR") ? getenv("B3_REPO_DIR")
                                                        : "(module default)");
    printf("         elf   %s\n", getenv("B3_ELF") ? getenv("B3_ELF")
                                                   : "(module default)");
    printf("         gstr  %s\n", getenv("B3_GLOBALUS")
                                  ? getenv("B3_GLOBALUS")
                                  : "(module default)");
    printf("         gen   %s\n", getenv("B3_GEN_OUT") ? getenv("B3_GEN_OUT")
                                                       : "(<out>/gen)");
    if (check)
        printf("         check %s\n", check_dir ? check_dir : "(<repo>/src)");
    else
        printf("         out   %s\n", out);

    /* THE WORKING DIRECTORY IS PART OF THE CONTRACT.  Two global families
     * resolve paths against it: the art family's postfx manifest records
     * CWD-RELATIVE paths (its python original does, and the C mirrors it byte
     * for byte), and its font stage probes build/burnout3.elf relative to CWD
     * when $B3_ELF is unset.  So the run is made deterministic here rather
     * than left to whatever shell invoked it: every path a stage receives has
     * already been made absolute, $B3_ELF has already been pointed at the
     * image, and the CWD becomes the output root for the whole loop. */
    if (!check && chdir(out) != 0) {
        fprintf(stderr, "cextract: cannot enter %s\n", out);
        return 1;
    }

    t_all = cxc_now_s();
    for (size_t i = 0; i < CX_NGLOBALS; i++) {
        double t0;

        did[i] = 0;
        dur[i] = 0.0;
        rcs[i] = 0;
        if (!wanted(only, n_only, CX_GLOBALS[i].name))
            continue;
        if (CX_GLOBALS[i].manual && !n_only)
            continue;               /* --only names it, a bare run does not */
        printf("\n---- %s ----------------------------------------------\n",
               CX_GLOBALS[i].name);
        fflush(stdout);
        t0 = cxc_now_s();
        if (check) {
#ifdef CX_HAVE_GEN_TRACKSELECT
            if (!strcmp(CX_GLOBALS[i].name, "gen_trackselect")) {
                char p[4096];

                /* NULL = the module's own default, <repo>/src/<header>, which
                 * is where the python original's OUT points. */
                if (check_dir)
                    snprintf(p, sizeof(p), "%s/burnout3_trackselect.h",
                             check_dir);
                rcs[i] = cx_gen_trackselect_check(game,
                                                  check_dir ? p : NULL);
            } else
#endif
            {
                printf("(no --check mode; skipped)\n");
                dur[i] = cxc_now_s() - t0;
                continue;
            }
        } else {
            rcs[i] = CX_GLOBALS[i].fn(game, out);
        }
        dur[i] = cxc_now_s() - t0;
        did[i] = 1;
        ran++;
        if (rcs[i] != 0)
            failed++;
    }
    t_all = cxc_now_s() - t_all;

    printf("\n=========================================================\n");
    printf("%-20s %-10s %9s\n", "global stage", "result", "seconds");
    for (size_t i = 0; i < CX_NGLOBALS; i++)
        if (did[i])
            printf("%-20s %-10s %9.3f\n", CX_GLOBALS[i].name,
                   rcs[i] == 0 ? "ok" : "FAILED", dur[i]);
    printf("---------------------------------------------------------\n");
    printf("%d global stage(s) run, %d failed, %.3f s total\n",
           ran, failed, t_all);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *track = NULL, *game = NULL, *out = NULL, *repo = NULL;
    const char *check_dir = NULL, *gen_out = NULL;
    const char *only[CX_NSTAGES + CX_NGLOBALS + 1];
    size_t      n_only = 0;
    int         all_global = 0, check = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--track") && i + 1 < argc)
            track = argv[++i];
        else if (!strcmp(a, "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(a, "--game") && i + 1 < argc)
            game = argv[++i];
        else if (!strcmp(a, "--repo") && i + 1 < argc)
            repo = argv[++i];
        else if (!strcmp(a, "--check-dir") && i + 1 < argc)
            check_dir = argv[++i];
        else if (!strcmp(a, "--gen-out") && i + 1 < argc)
            gen_out = argv[++i];
        else if (!strcmp(a, "--all-global"))
            all_global = 1;
        else if (!strcmp(a, "--check"))
            check = 1;
        else if (!strcmp(a, "--only") && i + 1 < argc) {
            if (n_only < sizeof(only) / sizeof(only[0]))
                only[n_only++] = argv[++i];
            else
                i++;
        } else if (!strcmp(a, "--list")) {
            list_stages();
            return 0;
        } else if (!strcmp(a, "--list-global")) {
            list_globals();
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "cextract: unexpected argument %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    /* $B3_ISO ahead of $B3_GAME_DIR: it is the more specific knob, and both
     * name a source cx_src_open() sniffs for itself. */
    if (!game)
        game = getenv(CX_SRC_ENV_ISO);
    if (!game || !*game)
        game = getenv("B3_GAME_DIR");
    if (!game || !*game)
        game = getenv("B3_GAME_ROOT");
    if (!game || !*game)
        game = CX_DEFAULT_GAME_DIR;
    if (!track)
        track = getenv("B3_TRACK");
    if (!track || !*track)
        track = CX_DEFAULT_TRACK;
    if (repo && *repo)
        setenv("B3_REPO_DIR", repo, 1);

    if (check && !all_global) {
        fprintf(stderr, "cextract: --check is a --all-global mode\n");
        return 2;
    }
    if (!out && !(all_global && check)) {
        fprintf(stderr, "cextract: --out <dir> is required\n");
        usage(argv[0]);
        return 2;
    }

    if (!all_global) {
        if (open_source(game) != 0)
            return 1;
        return run_track_mode(game, track, out, only, n_only);
    }

    /* Everything the global loop hands a stage is made ABSOLUTE first,
     * because the loop chdir()s into the output root (see run_global_mode).
     * A relative --out or --game that survived into a stage would resolve
     * against the wrong directory and quietly write in the wrong place. */
    {
        static char a_game[4096], a_out[4096], a_repo[4096], a_elf[4096];
        static char a_gen[4096];
        const char *r;

        abspath(a_game, sizeof(a_game), game);
        game = a_game;
        /* Bind AFTER abspath: the shim keys off the exact string the stages
         * will be handed, and the loop below chdir()s away. */
        if (open_source(game) != 0)
            return 1;
        if (gen_out && *gen_out) {
            if (cxc_mkdir_p(gen_out) != 0) {
                fprintf(stderr, "cextract: cannot create %s\n", gen_out);
                return 1;
            }
            abspath(a_gen, sizeof(a_gen), gen_out);
            setenv("B3_GEN_OUT", a_gen, 1);
        }
        r = getenv("B3_REPO_DIR");
        if (r && *r) {
            abspath(a_repo, sizeof(a_repo), r);
            setenv("B3_REPO_DIR", a_repo, 1);
            r = a_repo;
        }
        if (out && *out) {
            if (cxc_mkdir_p(out) != 0) {
                fprintf(stderr, "cextract: cannot create %s\n", out);
                return 1;
            }
            abspath(a_out, sizeof(a_out), out);
            out = a_out;
        }
        /* The REPO-SIDE INPUTS several global families reach for by a
         * CWD-relative path: the mapped retail image (the art family's font
         * stage, the generator family's trackselect) and the Globalus string
         * table (the audio family's eatrax stage).  The loop below chdir()s,
         * so a CWD-relative probe would resolve inside the output root; the
         * driver points the documented environment variables at the real
         * files instead.
         *
         * Only when the variable is NOT already set AND the file is really
         * there: an explicit override always wins, and a tree with no build/
         * still falls through to whatever each module does on its own. */
        {
            static const struct {
                const char *var, *rel;
            } NEED[] = {
                { "B3_ELF",      "build/burnout3.elf" },
                { "B3_GLOBALUS", "build/Globalus.bin" },
            };
            const char *roots[3];
            size_t      j, k;

            roots[0] = (r && *r) ? r : NULL;
            roots[1] = ".";
#ifdef CX_HAVE_GEN_TRACKSELECT
            roots[2] = CXG_DEFAULT_REPO_DIR;
#else
            roots[2] = NULL;
#endif
            for (j = 0; j < sizeof(NEED) / sizeof(NEED[0]); j++) {
                if (getenv(NEED[j].var))
                    continue;
                for (k = 0; k < sizeof(roots) / sizeof(roots[0]); k++) {
                    char cand[4096];

                    if (!roots[k])
                        continue;
                    snprintf(cand, sizeof(cand), "%s/%s", roots[k],
                             NEED[j].rel);
                    if (readable(cand)) {
                        abspath(a_elf, sizeof(a_elf), cand);
                        setenv(NEED[j].var, a_elf, 1);
                        break;
                    }
                }
            }
        }
    }

    return run_global_mode(game, out ? out : "", only, n_only, check,
                           check_dir);
}
