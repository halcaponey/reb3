#include "burnout3_backend.h"
#include <stdio.h>
/* All diagnostics go to STDERR: several validators parse the stdout of
 * drivers that link these objects, and a stray status line breaks them. */
#include <stdlib.h>
#include <string.h>

static const char* const B3_FEATURE_NAMES[B3_FEAT_COUNT] = {
    "physics", "carcol", "ai", "traffic", "td_rules",
    "score", "crash", "camera", "sfx", "hud",
};

// What each feature would call into, for the generated file's comments.
static const char* const B3_FEATURE_NOTES[B3_FEAT_COUNT] = {
    "FUN_0011ECF0 + BE50 chain  INTEROP SCAFFOLDING, see docs/RE_SHAPE_PARITY.md",
    "FUN_001121F0 / FUN_00113960         ~1.8 ms/contact pair",
    "FUN_00105340                        ~0.04 ms/car",
    "FUN_00105150 / FUN_001A6070         needs the path tables resident",
    "FUN_00197BE0 / FUN_00105BD0         ~0.09 ms/event",
    "FUN_00197920 / FUN_001979E0         event-driven",
    "FUN_0011FC60 / FUN_0010DCA0         event-driven",
    "FUN_0015E550 / FUN_00162A90         per frame",
    "FUN_00141010                        event-driven",
    "FUN_0004D310                        emits draw lists, not pixels",
};

static B3Backend g_backend[B3_FEAT_COUNT];
static int       g_inited;

static const char* cfg_path(void)
{
    const char* p = getenv("B3_BACKENDS");
    return (p && *p) ? p : "build/backends.cfg";
}

static void write_default(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs("# Which implementation runs each feature.\n"
          "#\n"
          "#   re      this project's recovered C  (default)\n"
          "#   retail  the game's own x86 under emulation, via\n"
          "#           tools/b3_emu_server.py -- needs build/burnout3.elf\n"
          "#\n"
          "# Retail is slower but is the ground truth: it is the same code the\n"
          "# differential suites validate the port against. Flip one feature at\n"
          "# a time and drive the same corner to feel the difference.\n"
          "#\n", f);
    for (int i = 0; i < B3_FEAT_COUNT; i++)
        fprintf(f, "%-10s re      # %s\n",
                B3_FEATURE_NAMES[i], B3_FEATURE_NOTES[i]);
    fclose(f);
}

static int g_force = -1;   /* -1 = none; else a B3Backend for every feature */

void b3_backend_force_all(B3Backend b)
{
    if (g_inited) {
        fprintf(stderr, "[backend] force-all ignored: backends already "
                "initialised\n");
        return;
    }
    g_force = (int)b;
}

void b3_backend_init(void)
{
    if (g_inited) return;
    g_inited = 1;
    for (int i = 0; i < B3_FEAT_COUNT; i++) g_backend[i] = B3_BACKEND_RE;

    if (g_force >= 0) {
        /* the CLI override: every feature on one backend, cfg untouched */
        for (int i = 0; i < B3_FEAT_COUNT; i++)
            g_backend[i] = (B3Backend)g_force;
        fprintf(stderr, "[backend] CLI override: ALL features on %s "
                "(build/backends.cfg ignored)\n",
                g_force == B3_BACKEND_RETAIL ? "RETAIL" : "RE");
        return;
    }

    const char* path = cfg_path();
    FILE* f = fopen(path, "r");
    if (!f) {
        write_default(path);
        fprintf(stderr, "[backend] wrote default %s (all features on the RE port)\n",
               path);
        return;
    }
    char line[512];
    int n_retail = 0;
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = '\0';
        char name[64], val[64];
        if (sscanf(line, "%63s %63s", name, val) != 2) continue;
        for (int i = 0; i < B3_FEAT_COUNT; i++) {
            if (strcmp(name, B3_FEATURE_NAMES[i]) != 0) continue;
            if (strcmp(val, "retail") == 0) {
                g_backend[i] = B3_BACKEND_RETAIL;
                n_retail++;
            } else if (strcmp(val, "re") != 0) {
                fprintf(stderr, "[backend] %s: unknown value '%s', using re\n",
                       name, val);
            }
            break;
        }
    }
    fclose(f);
    if (n_retail) {
        fprintf(stderr, "[backend] %s:", path);
        for (int i = 0; i < B3_FEAT_COUNT; i++)
            if (g_backend[i] == B3_BACKEND_RETAIL)
                fprintf(stderr, " %s=RETAIL", B3_FEATURE_NAMES[i]);
        fprintf(stderr, "\n");
    }
}

B3Backend b3_backend_get(B3Feature f)
{
    if (!g_inited) b3_backend_init();
    if (f < 0 || f >= B3_FEAT_COUNT) return B3_BACKEND_RE;
    return g_backend[f];
}

const char* b3_backend_name(B3Feature f)
{
    if (f < 0 || f >= B3_FEAT_COUNT) return "?";
    return B3_FEATURE_NAMES[f];
}

void b3_backend_demote(B3Feature f, const char* why)
{
    if (f < 0 || f >= B3_FEAT_COUNT) return;
    if (g_backend[f] == B3_BACKEND_RE) return;
    g_backend[f] = B3_BACKEND_RE;
    fprintf(stderr, "[backend] %s demoted to re: %s\n", B3_FEATURE_NAMES[f], why);
}
