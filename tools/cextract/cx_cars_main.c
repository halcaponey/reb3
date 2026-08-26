/* cx_cars_main.c -- standalone driver for the CAR / VEHICLE family.
 *
 * cx_main.c belongs to the gen-tools agent and is not touched here, so this
 * file carries agent D's own entry point, compiled ONLY under
 * -DCX_CARS_STANDALONE.  Without that macro it contributes nothing, which is
 * what lets tools/cextract/build.sh keep globbing *.c.
 *
 *   gcc -Wall -Wextra -std=c11 -O2 -DCX_CARS_STANDALONE \
 *       tools/cextract/cx_cars_*.c tools/cextract/cx_png.c -o cxcars -lz -lm
 *
 *   cxcars <out_root> [stage ...]
 *
 *     meshes | paint | lights | roster | tuning     dump-global stages
 *     physparams                                    needs the Ghidra bridge
 *     traffic <track_out_dir> [<track_out_dir> ...] the per-track .btv mirror
 *     all                                           every dump-global stage
 *
 * <out_root> is a REPO-ROOT STAND-IN: assets land in <out_root>/build/cars,
 * generated headers in <out_root>/gen, per-track .btv assets in the
 * <track_out_dir> given (normally <out_root>/build/tracks/<ID>).
 *
 * $B3_GAME_DIR points at the mounted game (default the same path the python
 * tools hardcode); $B3_REPO_DIR (or $B3_REPO_ROOT) at the checkout, for the
 * two inputs the VDB stage reads outside the game; $B3_CARS_OUT overrides the
 * traffic stage's build/cars mirror.
 */
#ifdef CX_CARS_STANDALONE

#include "cx_cars.h"
#include "cx_extract.h"

#include <stdio.h>
#include <string.h>

static int run_all(const char *game, const char *out)
{
    int rc = 0;
    rc |= cx_extract_car_meshes(game, out);
    rc |= cx_extract_car_paint(game, out);
    rc |= cx_extract_traffic_lights(game, out);
    rc |= cx_extract_vehicle_roster(game, out);
    rc |= cx_extract_car_tuning(game, out);
    return rc;
}

int main(int argc, char **argv)
{
    const char *game = cxd_game_dir();
    const char *out;
    int i, rc = 0;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <out_root> [meshes|paint|lights|roster|tuning|all]\n"
                "       %s <out_root> traffic <track_out_dir> ...\n", argv[0],
                argv[0]);
        return 2;
    }
    out = argv[1];
    if (argc == 2)
        return run_all(game, out);

    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "all"))          rc |= run_all(game, out);
        else if (!strcmp(argv[i], "meshes"))  rc |= cx_extract_car_meshes(game, out);
        else if (!strcmp(argv[i], "paint"))   rc |= cx_extract_car_paint(game, out);
        else if (!strcmp(argv[i], "lights"))  rc |= cx_extract_traffic_lights(game, out);
        else if (!strcmp(argv[i], "roster"))  rc |= cx_extract_vehicle_roster(game, out);
        else if (!strcmp(argv[i], "tuning"))  rc |= cx_extract_car_tuning(game, out);
        else if (!strcmp(argv[i], "physparams"))
            rc |= cx_extract_physics_params(game, out);
        else if (!strcmp(argv[i], "traffic")) {
            /* every remaining argument is a per-track out_dir
             * (the build/tracks/<ID> that already holds traffic.bin) */
            for (i++; i < argc; i++)
                rc |= cx_extract_traffic_cars(game, NULL, NULL, argv[i]);
            break;
        } else {
            fprintf(stderr, "unknown stage %s\n", argv[i]);
            return 2;
        }
    }
    return rc;
}

#endif /* CX_CARS_STANDALONE */
