/* cx_art_main.c -- the ART/FRONTEND family's own driver, for running and
 * gating the six tools without cx_main.c (which the driver agent owns).
 *
 * Compiled ONLY under -DCX_ART_STANDALONE, so the normal build.sh glob links
 * this file as an empty translation unit.
 *
 *   gcc -Wall -Wextra -std=c11 -O2 -DCX_ART_STANDALONE \
 *       cx_art_*.c cx_common_b.c cx_png.c -o cxart -lz -lm
 *
 *   cxart <out_root> [tool ...]     tools: txd font carfx boostfx particlefx
 *                                          postfx all
 *   cxart --txd-no-palettes <out_root>       the python's default txd mode
 *   cxart --postfx-track REG/Cn_Vn <root>    one track instead of --all
 *   cxart --postfx-scan <out_root>           add the --scan cross-check
 *
 * The manifest extract_postfx_art.py writes carries CWD-relative paths, so
 * this main chdir()s into <out_root> before running anything -- the same
 * relationship the python has when run from the repo root.
 */
typedef int cxe_art_main_translation_unit_is_not_empty;

#ifdef CX_ART_STANDALONE

/* POSIX 2008 for strdup/strtok_r/getcwd/chdir/dirent under -std=c11,
 * which build.sh uses (it defines __STRICT_ANSI__). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cx_art_common.h"
#include "cx_extract.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int run_txd(const char *game, const char *root, int all_palettes)
{
    char fe[4352], gl[4352], outdir[4096];
    const char *inputs[2];

    game = cxe_game_dir(game);
    snprintf(fe, sizeof fe, "%s/Data/Frontend.txd", game);
    snprintf(gl, sizeof gl, "%s/Data/Global.txd", game);
    if (cxe_out_dir(outdir, sizeof outdir, root, "build/frontend") != 0)
        return 1;
    inputs[0] = fe;
    inputs[1] = gl;
    return cx_extract_txd_banks(inputs, 2, outdir, all_palettes, 1);
}

int main(int argc, char **argv)
{
    const char *game = NULL, *root = NULL, *ptrack = NULL;
    int all_palettes = 1, pscan = 0, rc = 0, i, ntools = 0;
    const char *tools[16];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game = argv[++i];
        } else if (strcmp(argv[i], "--txd-no-palettes") == 0) {
            all_palettes = 0;
        } else if (strcmp(argv[i], "--postfx-track") == 0 && i + 1 < argc) {
            ptrack = argv[++i];
        } else if (strcmp(argv[i], "--postfx-scan") == 0) {
            pscan = 1;
        } else if (!root) {
            root = argv[i];
        } else if (ntools < 16) {
            tools[ntools++] = argv[i];
        }
    }
    if (!root) {
        fprintf(stderr, "usage: %s [--game DIR] [--txd-no-palettes] "
                        "[--postfx-track REG/Cn_Vn] [--postfx-scan] "
                        "<out_root> [tool ...]\n", argv[0]);
        return 2;
    }
    if (cxb_mkdir_p(root) != 0) {
        fprintf(stderr, "cannot create %s\n", root);
        return 1;
    }
    /* the postfx manifest records CWD-relative paths */
    if (chdir(root) != 0) {
        fprintf(stderr, "cannot chdir to %s\n", root);
        return 1;
    }
    {
        static char abs_root[4096];
        if (!getcwd(abs_root, sizeof abs_root))
            return 1;
        root = abs_root;
    }
    if (ntools == 0) {
        tools[ntools++] = "all";
    }

    for (i = 0; i < ntools; i++) {
        const char *t = tools[i];
        int all = strcmp(t, "all") == 0;
        if (all || strcmp(t, "txd") == 0)
            rc |= run_txd(game, root, all_palettes) ? 1 : 0;
        if (all || strcmp(t, "font") == 0)
            rc |= cx_extract_font(game, root) ? 2 : 0;
        if (all || strcmp(t, "carfx") == 0)
            rc |= cx_extract_carfx_art(game, root) ? 4 : 0;
        if (all || strcmp(t, "boostfx") == 0)
            rc |= cx_extract_boostfx_art(game, root) ? 8 : 0;
        if (all || strcmp(t, "particlefx") == 0)
            rc |= cx_extract_particlefx_art(game, root) ? 16 : 0;
        if (all || strcmp(t, "postfx") == 0)
            rc |= cx_extract_postfx_art_ex(game, root, ptrack, pscan) ? 32 : 0;
    }
    return rc;
}

#endif /* CX_ART_STANDALONE */
