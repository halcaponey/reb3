/* cx_art_fx.c -- ports of the three Global.txd art pullers plus the per-car
 * tables:
 *
 *   tools/extract_carfx_art.py      -> <out_root>/build/carfx/{blobbyshadow,
 *                                      coronaglow}.png, env_light.txt
 *                                   -> <out_root>/build/cars/<CLS>_<CarN>.lights
 *   tools/extract_boostfx_art.py    -> <out_root>/build/boostfx/{coronaboost,
 *                                      coronaboostred}.png
 *   tools/extract_particlefx_art.py -> <out_root>/build/particlefx/fx<n>.png
 *
 * EVERY texture name here is named BY THE RETAIL EXECUTABLE -- no asset was
 * picked by eye:
 *
 *   "blobbyshadow"  the car shadow.  FUN_00043350 looks it up by name through
 *                   FUN_0002DDF0; the string at VA 0x003AAFF8 is that
 *                   function's only xref.                                [C]
 *   "coronaglow" /  FUN_0017EE00 walks a three-entry {const char*, u32} table
 *   "coronaboost" / at VA 0x003A3E7C and creates one sprite POOL per entry:
 *   "coronaboostred" pool 0 coronaglow, pool 1 coronaboost, pool 2
 *                   coronaboostred.  The corona emitter FUN_00187BE0
 *                   hard-codes pool 0 (ADD ECX,0xC @0x00187C28); the exhaust
 *                   flame emitter FUN_001871E0 picks 1 or 2 from
 *                   FUN_00179F30's output word (carObj+0x1901).          [C]
 *   "fx*"           the crash dust/smoke/debris family carried by Global.txd
 *                   itself, bound by the FX system directly (the sprite-pool
 *                   table has no fourth entry, so these are NOT pool
 *                   clients).  `fxskid` is a .data literal at 0x003E7BC8.
 *
 * The per-car tables come out of the .bgv files:
 *
 *   * corona lights, read by FUN_001879E0/FUN_00187AC0 from
 *     *(u32*)(model + 0x1664 + type*4) with count *(u8*)(model + 0x16AC +
 *     type), records of 0x30 bytes = {float4 pos, float4 normal, float4 aux}.
 *     Verified on all 67 player .bgv files: offset != 0 <=> count != 0, every
 *     block in range, front types facing +Z and rear types -Z.           [C]
 *   * the shadow quad's four strip rows, read by FUN_0019A7C0: the OUTER rows
 *     from the bounding box (model+0xE88 / +0xE98) and the INNER rows from
 *     the axles (model+0xBF8 = wheel[1].pos.z, model+0xB38+numWheels*0x40 =
 *     wheel[nw-2].pos.z), half width at model+0xE80.                     [C]
 *   * the enviro sun colour: pixel-shader constant c14.xyz (DAT_0060E0A0) is
 *     the environment object's +0x60, and FUN_001888F0 overwrites that object
 *     with the first 0xB0 bytes of the track's enviro.dat.               [C]
 */
/* POSIX 2008 for strdup/strtok_r/getcwd/chdir/dirent under -std=c11,
 * which build.sh uses (it defines __STRICT_ANSI__). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cx_art_common.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- the name tables */
static const char *const CARFX_WANTED[]      = { "blobbyshadow", "coronaglow" };
static const char *const BOOSTFX_WANTED[]    = { "coronaboost", "coronaboostred" };
static const char *const PARTICLEFX_WANTED[] = {
    "fxsmoke", "fxexplosionsmoke", "fxexplosionfire", "fxexplosionflash",
    "fxfire", "fxdebris1", "fxdebris2", "fxglass", "fxgravel", "fxsnow",
    "fxspark", "fxpopcorndebris", "fxpopcornspark", "fxscrape"
};
/* the ones burnout3_crashfx.c refuses to run without */
static const char *const PARTICLEFX_REQUIRED[] = {
    "fxsmoke", "fxdebris1", "fxglass", "fxgravel"
};

/* The 12 corona light types.  Names come from the emitter FUN_00187C70's
 * bit->type dispatch plus the geometry itself (front/rear, +Z/-Z normals). */
static const char *const LIGHT_TYPES[12] = {
    "headlight", "tail", "brake", "unused3", "reverse",
    "indicator_r", "indicator_l", "unused7",
    "aux8", "aux9", "aux10", "aux11"
};

/* [C] enviro.dat +0x60 == the environment object's +0x60 == ps c14.xyz */
#define ENV_LIGHT_OFF 0x60

/* ------------------------------------------------------- per-car .lights */
static int extract_lights(const char *game, const char *cardir,
                          long *n_cars_out, long *n_lights_out)
{
    char pveh[4352];
    cxe_names classes = { NULL, 0 };
    long n_cars = 0, n_lights = 0;
    int ci, rc = 1;

    if ((size_t)snprintf(pveh, sizeof pveh, "%s/pveh", game) >= sizeof pveh)
        return 1;
    if (!cxe_is_dir(pveh)) {
        fprintf(stderr, "FAIL: %s missing\n", pveh);
        return 1;
    }
    if (cxb_mkdir_p(cardir) != 0) {
        fprintf(stderr, "cannot create %s\n", cardir);
        return 1;
    }
    if (cxe_listdir_sorted(pveh, &classes) != 0)
        return 1;

    for (ci = 0; ci < classes.n; ci++) {
        char cdir[4352];
        cxe_names files = { NULL, 0 };
        int fi;

        if ((size_t)snprintf(cdir, sizeof cdir, "%s/%s", pveh, classes.v[ci])
            >= sizeof cdir)
            continue;
        if (!cxe_is_dir(cdir))
            continue;
        if (cxe_listdir_sorted(cdir, &files) != 0)
            continue;

        for (fi = 0; fi < files.n; fi++) {
            const char *fn = files.v[fi];
            size_t fl = strlen(fn);
            char bgv[4608], out[4608], base[256];
            cxb_blob d = { NULL, 0 };
            FILE *fp;
            int nw, t, ok = 1;

            if (strncmp(fn, "Car", 3) != 0 || fl < 5
                || strcmp(fn + fl - 4, ".bgv") != 0)
                continue;
            if ((size_t)snprintf(bgv, sizeof bgv, "%s/%s", cdir, fn)
                >= sizeof bgv)
                continue;
            if (cxb_read_file(bgv, &d) != 0) {
                fprintf(stderr, "FAIL: cannot read %s\n", bgv);
                cxe_names_free(&files);
                goto done;
            }
            nw = (int)cxb_u8(&d, 0x0D);
            /* the python's struct.unpack_from raises past EOF; be explicit */
            if (d.n < (size_t)(0x16AC + 12)
                || d.n < (size_t)(0xB38 + nw * 0x40 + 4)) {
                fprintf(stderr, "FAIL: %s/%s truncated\n", classes.v[ci], fn);
                cxb_blob_free(&d);
                cxe_names_free(&files);
                goto done;
            }

            snprintf(base, sizeof base, "%.*s", (int)(fl - 4), fn);
            if ((size_t)snprintf(out, sizeof out, "%s/%s_%s.lights", cardir,
                                 classes.v[ci], base) >= sizeof out) {
                cxb_blob_free(&d);
                continue;
            }
            fp = fopen(out, "w");
            if (!fp) {
                fprintf(stderr, "cannot write %s\n", out);
                cxb_blob_free(&d);
                cxe_names_free(&files);
                goto done;
            }
            fprintf(fp,
                "# Car-FX tables, extracted by tools/extract_carfx_art.py.\n"
                "# corona light table: FUN_001879E0 reads records at\n"
                "#   *(u32*)(model+0x1664+type*4), count *(u8*)(model+0x16AC+type),\n"
                "#   stride 0x30 = {float4 pos, float4 normal, float4 aux} [C]\n"
                "# shadow quad sources: FUN_0019A7C0 [C]  (4 strip rows)\n"
                "#   halfwidth = model+0xE80.x\n"
                "#   outerfrontz = model+0xE88   (V 0)\n"
                "#   frontz      = model+0xBF8   (V 0.1875)\n"
                "#   rearz       = model+0xB38+numWheels*0x40 (V 0.8125)\n"
                "#   outerrearz  = model+0xE98   (V 1)\n");
            fprintf(fp, "numwheels %d\n", nw);
            fprintf(fp, "halfwidth %.6f\n",   (double)cxb_f32(&d, 0xE80));
            fprintf(fp, "outerfrontz %.6f\n", (double)cxb_f32(&d, 0xE88));
            fprintf(fp, "frontz %.6f\n",      (double)cxb_f32(&d, 0xBF8));
            fprintf(fp, "rearz %.6f\n",
                    (double)cxb_f32(&d, (size_t)(0xB38 + nw * 0x40)));
            fprintf(fp, "outerrearz %.6f\n",  (double)cxb_f32(&d, 0xE98));

            for (t = 0; t < 12; t++) {
                uint32_t off = cxb_u32(&d, (size_t)(0x1664 + t * 4));
                int cnt = (int)cxb_u8(&d, (size_t)(0x16AC + t));
                int k;

                if (cnt == 0) {
                    if (off != 0u)
                        ok = 0;
                    continue;
                }
                if (off == 0u
                    || (uint64_t)off + (uint64_t)cnt * 0x30u > (uint64_t)d.n) {
                    ok = 0;
                    continue;
                }
                for (k = 0; k < cnt; k++) {
                    size_t b = (size_t)off + (size_t)k * 0x30u;
                    fprintf(fp,
                            "light %d %s %.6f %.6f %.6f %.6f %.6f %.6f\n",
                            t, LIGHT_TYPES[t],
                            (double)cxb_f32(&d, b),
                            (double)cxb_f32(&d, b + 4),
                            (double)cxb_f32(&d, b + 8),
                            (double)cxb_f32(&d, b + 0x10),
                            (double)cxb_f32(&d, b + 0x14),
                            (double)cxb_f32(&d, b + 0x18));
                    n_lights++;
                }
            }
            fclose(fp);
            cxb_blob_free(&d);
            if (!ok) {
                fprintf(stderr, "FAIL: %s/%s inconsistent light table\n",
                        classes.v[ci], fn);
                cxe_names_free(&files);
                goto done;
            }
            n_cars++;
        }
        cxe_names_free(&files);
    }

    printf("  %ld cars, %ld corona lights -> %s/*.lights\n", n_cars, n_lights,
           cardir);
    *n_cars_out = n_cars;
    *n_lights_out = n_lights;
    rc = 0;

done:
    cxe_names_free(&classes);
    return rc;
}

/* ------------------------------------------------------ the enviro sun -- */
static int extract_env_light(const char *game, const char *outdir)
{
    char tracks[4352], path[4608], out[4608];
    cxe_names regions = { NULL, 0 };
    FILE *fp = NULL;
    int ri, nrows = 0;
    /* two passes: the python only creates the file when a row exists */
    struct Row { char name[128]; double r, g, b; } *rows = NULL;
    int cap = 0;

    if ((size_t)snprintf(tracks, sizeof tracks, "%s/Tracks", game)
        >= sizeof tracks)
        return 0;
    if (!cxe_is_dir(tracks)) {
        printf("[carfx-art] Tracks/ not found, skipping enviro sun table\n");
        return 0;
    }
    if (cxe_listdir_sorted(tracks, &regions) != 0)
        return 0;

    for (ri = 0; ri < regions.n; ri++) {
        char rd[4480];
        cxe_names trks = { NULL, 0 };
        int ti;

        if ((size_t)snprintf(rd, sizeof rd, "%s/%s", tracks, regions.v[ri])
            >= sizeof rd)
            continue;
        if (!cxe_is_dir(rd))
            continue;
        if (cxe_listdir_sorted(rd, &trks) != 0)
            continue;
        for (ti = 0; ti < trks.n; ti++) {
            cxb_blob d = { NULL, 0 };

            if ((size_t)snprintf(path, sizeof path, "%s/%s/enviro.dat", rd,
                                 trks.v[ti]) >= sizeof path)
                continue;
            if (!cxe_is_file(path))
                continue;
            if (cxb_read_file(path, &d) != 0)
                continue;
            if (d.n < (size_t)(ENV_LIGHT_OFF + 12)) {
                cxb_blob_free(&d);
                continue;
            }
            if (nrows >= cap) {
                int nc = cap ? cap * 2 : 64;
                void *nv = realloc(rows, (size_t)nc * sizeof *rows);
                if (!nv) { cxb_blob_free(&d); cxe_names_free(&trks); goto out; }
                rows = nv;
                cap = nc;
            }
            snprintf(rows[nrows].name, sizeof rows[nrows].name, "%s_%s",
                     regions.v[ri], trks.v[ti]);
            rows[nrows].r = (double)cxb_f32(&d, ENV_LIGHT_OFF);
            rows[nrows].g = (double)cxb_f32(&d, ENV_LIGHT_OFF + 4);
            rows[nrows].b = (double)cxb_f32(&d, ENV_LIGHT_OFF + 8);
            nrows++;
            cxb_blob_free(&d);
        }
        cxe_names_free(&trks);
    }

    if (nrows == 0)
        goto out;
    if (cxb_mkdir_p(outdir) != 0)
        goto out;
    if ((size_t)snprintf(out, sizeof out, "%s/env_light.txt", outdir)
        >= sizeof out)
        goto out;
    fp = fopen(out, "w");
    if (!fp)
        goto out;
    fprintf(fp, "# ps c14.xyz per track = enviro.dat +0x%02X (float3)\n"
                "# track        r        g        b        (8-bit)\n",
            ENV_LIGHT_OFF);
    for (ri = 0; ri < nrows; ri++)
        fprintf(fp, "%-10s %.9g %.9g %.9g   %d,%d,%d\n",
                rows[ri].name, rows[ri].r, rows[ri].g, rows[ri].b,
                (int)cxe_py_round(rows[ri].r * 255.0),
                (int)cxe_py_round(rows[ri].g * 255.0),
                (int)cxe_py_round(rows[ri].b * 255.0));
    fclose(fp);
    fp = NULL;
    {
        char rel[4608];
        if (cxe_relpath(out, rel, sizeof rel) != 0)
            snprintf(rel, sizeof rel, "%s", out);
        printf("[carfx-art] %s  (%d tracks)\n", rel, nrows);
    }

out:
    if (fp)
        fclose(fp);
    free(rows);
    cxe_names_free(&regions);
    return nrows;
}

/* ------------------------------------------------------------ the entries */
int cx_extract_carfx_art(const char *game_dir, const char *out_root)
{
    const char *game = cxe_game_dir(game_dir);
    char txd[4352], outdir[4096], cardir[4096];
    long n_cars = 0, n_lights = 0;
    int rc;

    if ((size_t)snprintf(txd, sizeof txd, "%s/Data/Global.txd", game)
        >= sizeof txd)
        return 1;
    if (cxe_out_dir(outdir, sizeof outdir, out_root, "build/carfx") != 0)
        return 1;
    if (cxe_join(cardir, sizeof cardir, out_root, "build/cars") != 0)
        return 1;

    printf("[carfx-art] textures from %s\n", txd);
    rc = cxe_pull_named(txd, outdir, CARFX_WANTED, 2, NULL, 0,
                        CXE_REPORT_CARFX);
    if (rc)
        return rc;
    printf("[carfx-art] per-car tables from %s/pveh\n", game);
    if (extract_lights(game, cardir, &n_cars, &n_lights) != 0)
        return 1;
    printf("[carfx-art] enviro sun colours from %s/Tracks\n", game);
    extract_env_light(game, outdir);
    printf("[carfx-art] OK\n");
    return 0;
}

/* Sanity-report the type-8 (exhaust) records already in build/cars. */
static long report_emitters(const char *cardir)
{
    cxe_names ns = { NULL, 0 };
    long tot = 0;
    int ncars = 0, i;
    char sample_file[256] = "", sample_line[512] = "";

    if (!cxe_is_dir(cardir) || cxe_listdir_sorted(cardir, &ns) != 0) {
        printf("  (%s absent -- run tools/extract_carfx_art.py first)\n",
               cardir);
        return 0;
    }
    for (i = 0; i < ns.n; i++) {
        char p[4608], line[512];
        FILE *f;
        size_t l = strlen(ns.v[i]);
        int n = 0;

        if (l < 8 || strcmp(ns.v[i] + l - 7, ".lights") != 0)
            continue;
        if ((size_t)snprintf(p, sizeof p, "%s/%s", cardir, ns.v[i]) >= sizeof p)
            continue;
        f = fopen(p, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "light 8 ", 8) != 0)
                continue;
            n++;
            if (!sample_file[0]) {
                size_t sl = strlen(line);
                while (sl && (line[sl - 1] == '\n' || line[sl - 1] == '\r'))
                    line[--sl] = 0;
                snprintf(sample_file, sizeof sample_file, "%s", ns.v[i]);
                snprintf(sample_line, sizeof sample_line, "%s", line);
            }
        }
        fclose(f);
        if (n)
            ncars++;
        tot += n;
    }
    printf("  type-8 exhaust emitters: %ld records over %d cars\n", tot, ncars);
    if (sample_file[0])
        printf("    e.g. %s: %s\n", sample_file, sample_line);
    cxe_names_free(&ns);
    return tot;
}

int cx_extract_boostfx_art(const char *game_dir, const char *out_root)
{
    const char *game = cxe_game_dir(game_dir);
    char txd[4352], outdir[4096], cardir[4096];
    int rc;

    if ((size_t)snprintf(txd, sizeof txd, "%s/Data/Global.txd", game)
        >= sizeof txd)
        return 1;
    if (cxe_out_dir(outdir, sizeof outdir, out_root, "build/boostfx") != 0)
        return 1;
    if (cxe_join(cardir, sizeof cardir, out_root, "build/cars") != 0)
        return 1;

    printf("boost-flame art (names read out of burnout3.elf):\n");
    rc = cxe_pull_named(txd, outdir, BOOSTFX_WANTED, 2, NULL, 0,
                        CXE_REPORT_BOOSTFX);
    report_emitters(cardir);
    return rc;
}

int cx_extract_particlefx_art(const char *game_dir, const char *out_root)
{
    const char *game = cxe_game_dir(game_dir);
    char txd[4352], outdir[4096];

    if ((size_t)snprintf(txd, sizeof txd, "%s/Data/Global.txd", game)
        >= sizeof txd)
        return 1;
    if (cxe_out_dir(outdir, sizeof outdir, out_root, "build/particlefx") != 0)
        return 1;

    printf("crash dust/debris art (names read out of Data/Global.txd):\n");
    return cxe_pull_named(txd, outdir,
                          PARTICLEFX_WANTED,
                          (int)(sizeof PARTICLEFX_WANTED
                                / sizeof PARTICLEFX_WANTED[0]),
                          PARTICLEFX_REQUIRED,
                          (int)(sizeof PARTICLEFX_REQUIRED
                                / sizeof PARTICLEFX_REQUIRED[0]),
                          CXE_REPORT_PARTICLEFX);
}
