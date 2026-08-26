/* cx_cars_roster.c -- port of tools/extract_vehicles.py.
 *
 * The real vehicle roster out of pveh/vlist.bin and every
 * pveh/<CLASS>/Car*.bgv|btv header, emitted as the C header the game
 * compiles against.
 *
 * The python writes src/burnout3_vehicle_data.h.  This pipeline never writes
 * into src/ (the same rule agent A's tlist/start_grid modules follow), so the
 * artefact lands at <out_root>/gen/burnout3_vehicle_data.h with byte-identical
 * CONTENT; installing it is the caller's decision.
 *
 * Only fields verified against the files are emitted -- docs/RE_NOTES.md
 * section 5.  Mesh data is not part of this header (tools/extract_bgv.py,
 * ported in cx_cars_bgv.c, does the geometry).
 *
 *   header  +0x00 u32 magic 0x17          (BGV_MAGIC, holds for all 107)
 *           +0x08 u32 self-describing file size -- cross-checked against the
 *                 actual size on disc; a mismatch rejects the file
 *           +0x0C u32 per-file variant/feature word.  Observed across the 107
 *                 shipped vehicles: 0x0406, 0x0405, 0x0403 and 0 (traffic-only
 *                 models, which also zero the +0x18 dimension).  Validation
 *                 therefore keys on the magic plus +0x08, both of which hold
 *                 for every file on disc.
 *           +0x10 u32 section count
 *           +0x14 f32 dim_a, +0x18 f32 dim_b
 *
 * QUIRK Q11 (reproduced): within a class the files are ordered by
 * car_sort_key() -- EVERY digit of the stem concatenated and read as one
 * integer, then the stem as tie-break -- and the two extensions are two
 * separate passes, .bgv (player) entirely before .btv (traffic).
 * QUIRK Q12: the emitted `variant` uses `0x%04Xu`, so a value wider than four
 * hex digits would simply print wider; and `sections`/`data_size` print the
 * ACTUAL file size, not the +0x08 field (they are equal by construction,
 * because a mismatch rejects the file).
 */
#include "cx_cars.h"
#include "cx_extract.h"
#include "cx_src.h"   /* the dump may be a directory OR an ISO */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BGV_MAGIC 0x17u

static const char *const CLASS_CODE[8] = {
    "COMP", "CUPE", "HEVY", "HSPC", "MSCL", "SPRT", "SUPR", "TSPC"
};
static const char *const CLASS_NAME[8] = {
    "Compact", "Coupe", "Heavy", "HSpec", "Muscle", "Sports", "Super", "TSpec"
};

typedef struct {
    char     file[64];
    int      cls;
    int      traffic;
    unsigned size_actual;
    unsigned variant;
    unsigned sections;
    double   dim_a, dim_b;
} cxd_vehinfo;

/* read_header(): 0 = accepted, -1 = "unrecognised header". */
static int read_header(const char *path, cxd_vehinfo *out)
{
    cxd_blob d = { NULL, 0 };
    unsigned long long fsz = 0;

    if (cx_vfs_stat(path, &fsz, NULL) != 0)
        return -1;
    if (cxd_read_file(path, &d) != 0)
        return -1;
    if (d.n < 0x80) { cxd_blob_free(&d); return -1; }
    if (cxd_u32(&d, 0) != BGV_MAGIC
        || cxd_u32(&d, 8) != (uint32_t)fsz) {
        cxd_blob_free(&d);
        return -1;
    }
    out->size_actual = (unsigned)fsz;
    out->variant  = cxd_u32(&d, 0x0C);
    out->sections = cxd_u32(&d, 0x10);
    out->dim_a    = cxd_f32(&d, 0x14);
    out->dim_b    = cxd_f32(&d, 0x18);
    cxd_blob_free(&d);
    return 0;
}

/* --------------------------------------------- the RUNTIME asset, roster.bin
 *
 * The same roster the header above carries, as a file the port loads at boot
 * instead of compiling in (cx_extract.h, "purge 2"; the loader is
 * src/burnout3_vehicle_data_runtime.h).  Format spec in that block.
 *
 * dim_a/dim_b go through q5() because the HEADER prints them "%.5ff": the
 * compiled constant is the ROUNDED decimal, so storing the raw f32 would make
 * the loaded table differ from the table it replaces.  Reproducing the
 * header's own format and parsing it back makes the two bit-identical. */
static float q5(double v)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.5f", v);
    return (float)strtod(buf, NULL);
}

static void bin_u32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
    b[2] = (unsigned char)((v >> 16) & 0xFFu);
    b[3] = (unsigned char)((v >> 24) & 0xFFu);
    fwrite(b, 1, 4, f);
}

static void bin_f32(FILE *f, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    bin_u32(f, u);
}

static void bin_str(FILE *f, const char *s, size_t cap)
{
    char buf[64];
    size_t n = cap < sizeof buf ? cap : sizeof buf, l = s ? strlen(s) : 0;
    memset(buf, 0, n);
    if (l > n - 1) {
        /* a silently truncated key would make the runtime table miss its
         * roster row -- exactly the class of bug this purge exists to kill */
        fprintf(stderr, "[cextract] field overflow: %s does not fit in %d "
                "bytes\n", s, (int)n);
        l = n - 1;
    }
    memcpy(buf, s ? s : "", l);
    fwrite(buf, 1, n, f);
}

static int emit_roster_bin(const char *out_root, const cxd_vehinfo *veh,
                           int nveh, int have_vlist, uint32_t vl_version,
                           uint32_t vl_count)
{
    char dir[4096], out[4096];
    FILE *f;
    int ci, i, n = 0;

    cxd_path(dir, sizeof dir, out_root, "/build/cars", NULL);
    cxd_path(out, sizeof out, dir, "/roster.bin", NULL);
    if (cxd_mkdir_p(dir) != 0) {
        fprintf(stderr, "[cx_cars_roster] cannot create %s\n", dir);
        return 1;
    }
    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "[cx_cars_roster] cannot write %s\n", out);
        return 1;
    }
    fwrite("B3VR", 1, 4, f);
    bin_u32(f, 1);
    bin_u32(f, (uint32_t)nveh);
    bin_u32(f, 64);                             /* record stride */
    bin_u32(f, have_vlist ? vl_version : 0u);
    bin_u32(f, have_vlist ? vl_count : 0u);
    /* the HEADER's row order: class by class, and within a class the two
     * extension passes Q11 fixes.  The .bin must key identically, so it walks
     * the classes in the same loop the emitter above does. */
    for (ci = 0; ci < 8; ci++) {
        for (i = 0; i < nveh; i++) {
            if (veh[i].cls != ci)
                continue;
            bin_str(f, veh[i].file, 16);
            bin_str(f, CLASS_CODE[ci], 8);
            bin_str(f, CLASS_NAME[ci], 16);
            bin_u32(f, veh[i].traffic ? 1u : 0u);
            bin_u32(f, veh[i].size_actual);
            bin_u32(f, veh[i].variant);
            bin_u32(f, veh[i].sections);
            bin_f32(f, q5(veh[i].dim_a));
            bin_f32(f, q5(veh[i].dim_b));
            n++;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "[cx_cars_roster] short write on %s\n", out);
        return 1;
    }
    printf("wrote      : %s (%d vehicles)\n", out, n);
    return 0;
}

int cx_extract_vehicle_roster(const char *game_dir, const char *out_root)
{
    char pveh[4096], gen[4096], out[4096], p[4096];
    cxd_vehinfo *veh = NULL;
    int nveh = 0, cap = 0;
    int ci, ext, i, total;
    int have_vlist = 0;
    uint32_t vl_version = 0, vl_count = 0;
    FILE *f;
    int per_class_player[8], per_class_traffic[8];

    cxd_path(pveh, sizeof pveh, game_dir, "/pveh", NULL);
    cxd_path(gen, sizeof gen, out_root, "/gen", NULL);
    cxd_path(out, sizeof out, gen, "/burnout3_vehicle_data.h", NULL);
    if (cxd_mkdir_p(gen) != 0) {
        fprintf(stderr, "[cx_cars_roster] cannot create %s\n", gen);
        return 1;
    }

    cxd_path(p, sizeof p, pveh, "/vlist.bin", NULL);
    {
        cxd_blob v = { NULL, 0 };
        if (cxd_read_file(p, &v) == 0 && v.n >= 8) {
            vl_version = cxd_u32(&v, 0);
            vl_count = cxd_u32(&v, 4);
            have_vlist = 1;
        }
        cxd_blob_free(&v);
    }

    memset(per_class_player, 0, sizeof per_class_player);
    memset(per_class_traffic, 0, sizeof per_class_traffic);

    for (ci = 0; ci < 8; ci++) {
        char cdir[4096];
        cxd_path(cdir, sizeof cdir, pveh, "/", CLASS_CODE[ci], NULL);
        if (!cx_vfs_is_dir(cdir))
            continue;
        for (ext = 0; ext < 2; ext++) {           /* Q11: .bgv pass, then .btv */
            cxd_names fl;
            int fi;
            if (cxd_list_dir(cdir, ext ? ".btv" : ".bgv", 0, &fl) != 0)
                continue;
            cxd_sort_car_key(&fl);
            for (fi = 0; fi < fl.n; fi++) {
                cxd_vehinfo h;
                cxd_path(p, sizeof p, cdir, "/", fl.v[fi], NULL);
                memset(&h, 0, sizeof h);
                if (read_header(p, &h) != 0) {
                    fprintf(stderr, "  ! unrecognised header: %s\n", p);
                    continue;
                }
                snprintf(h.file, sizeof h.file, "%s", fl.v[fi]);
                h.cls = ci;
                h.traffic = ext;
                if (nveh == cap) {
                    cap = cap ? cap * 2 : 128;
                    veh = (cxd_vehinfo *)realloc(veh, (size_t)cap * sizeof *veh);
                    if (!veh) { cxd_names_free(&fl); return 1; }
                }
                veh[nveh++] = h;
                if (ext) per_class_traffic[ci]++; else per_class_player[ci]++;
            }
            cxd_names_free(&fl);
        }
    }
    total = nveh;

    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "[cx_cars_roster] cannot write %s\n", out);
        free(veh);
        return 1;
    }
    fputs("// Generated by tools/extract_vehicles.py -- do not edit by hand.\n"
          "// Real vehicle roster extracted from Burnout 3 game data.\n"
          "// Geometry is NOT included: .bgv meshes are NV2A push-buffers,"
          " undecoded.\n"
          "#ifndef BURNOUT3_VEHICLE_DATA_H\n"
          "#define BURNOUT3_VEHICLE_DATA_H\n"
          "\n"
          "typedef enum { VEH_PLAYER = 0, VEH_TRAFFIC = 1 } VehicleKind;\n"
          "\n"
          "typedef struct {\n"
          "    const char*  file;      // source filename in pveh/<class>/\n"
          "    const char*  class_code;\n"
          "    const char*  class_name;\n"
          "    VehicleKind  kind;\n"
          "    unsigned     data_size; // bytes of model data on disk\n"
          "    unsigned     variant;   // header +0x0C feature word\n"
          "    unsigned     sections;  // header +0x10\n"
          "    float        dim_a;     // header +0x14\n"
          "    float        dim_b;     // header +0x18\n"
          "} VehicleInfo;\n"
          "\n", f);
    if (have_vlist)
        fprintf(f, "#define VLIST_VERSION %uu\n"
                   "#define VLIST_DECLARED_COUNT %uu\n\n",
                (unsigned)vl_version, (unsigned)vl_count);
    fprintf(f, "#define VEHICLE_COUNT %d\n\n", total);
    fputs("static const VehicleInfo VEHICLES[VEHICLE_COUNT] = {\n", f);
    for (ci = 0; ci < 8; ci++) {
        int any = 0;
        for (i = 0; i < nveh; i++)
            if (veh[i].cls == ci) { any = 1; break; }
        if (!any)
            continue;
        fprintf(f, "    /* --- %s (%s) --- */\n", CLASS_NAME[ci],
                CLASS_CODE[ci]);
        for (i = 0; i < nveh; i++) {
            if (veh[i].cls != ci)
                continue;
            fprintf(f, "    { \"%s\", \"%s\", \"%s\", %s, %uu, 0x%04Xu, %uu,"
                       " %.5ff, %.5ff },\n",
                    veh[i].file, CLASS_CODE[ci], CLASS_NAME[ci],
                    veh[i].traffic ? "VEH_TRAFFIC" : "VEH_PLAYER",
                    veh[i].size_actual, veh[i].variant, veh[i].sections,
                    veh[i].dim_a, veh[i].dim_b);
        }
    }
    fputs("};\n\n#endif // BURNOUT3_VEHICLE_DATA_H\n", f);
    fclose(f);

    if (have_vlist)
        printf("vlist.bin  : version=%u declared_count=%u\n",
               (unsigned)vl_version, (unsigned)vl_count);
    else
        printf("vlist.bin  : version=? declared_count=?\n");
    for (ci = 0; ci < 8; ci++) {
        if (!per_class_player[ci] && !per_class_traffic[ci])
            continue;
        printf("  %-5s %-8s player=%-3d traffic=%-3d\n", CLASS_CODE[ci],
               CLASS_NAME[ci], per_class_player[ci], per_class_traffic[ci]);
    }
    printf("found      : %d vehicles\n", total);
    if (have_vlist)
        printf("cross-check: %s (vlist says %u, found %d)\n",
               (int)vl_count == total ? "MATCH" : "MISMATCH",
               (unsigned)vl_count, total);
    printf("size field : all %d headers match file size\n", total);
    printf("wrote      : %s\n", out);
    /* and the same roster as the RUNTIME asset the port actually loads */
    if (emit_roster_bin(out_root, veh, nveh, have_vlist, vl_version,
                        vl_count) != 0) {
        free(veh);
        return 1;
    }
    free(veh);
    return 0;
}
