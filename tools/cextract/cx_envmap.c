/* cx_envmap.c -- port of tools/extract_envmap.py.
 *
 * The per-track CAR ENVIRONMENT MAP -- the reflection sheet the car body shader
 * samples on texture stage 1 -- out of the track's `enviro.dat`, to
 * <out_dir>/envmap.png.
 *
 * WHY [C] / [S], condensed from the python docstring:
 *   The car body's NV2A programs are recovered (docs/RE_CARFX.md 13): the
 *   vertex program at 0x003E7D58 instruction 21 emits oT1.xyz = 2N(N.V) - V,
 *   the world-space reflection vector, and the body pixel-shader def at
 *   0x003E8468 lerps the shaded paint towards t1 by the Fresnel-weighted gloss
 *   mask -- t1 IS the environment map.  FUN_00031690 binds that stage from the
 *   single global DAT_004D6C00 and clamps ADDRESSU/V.                      [C]
 *   WHAT DAT_004D6C00 HOLDS IS [?] and stays [?]; its writer is not in the
 *   image.  The substitute extracted here is [S]: enviro.dat's header carries
 *   four texture-record offsets that FUN_00188880 relocates and registers --
 *   +0x98, +0x9C, +0xA0, +0xA4 (0x00188893 / 0x001888A9 / 0x001888BF /
 *   0x001888D5) [C] -- and only +0xA0 is a reflection sheet: +0x98 is a 32x32
 *   gradient LUT, +0x9C the 1024-wide sky-dome panorama, +0xA4 the paletted
 *   sun sprite.  Four tracks ship no +0xA0 record at all (AS/C1_V1, AS/C1_V2,
 *   AS/C2_V1, AS/C2_V2); nothing is written for them and nothing is invented.
 *
 * V-ORIGIN: block row 0 is written as PNG row 0 and no flip is applied
 * anywhere, so v = 0 addresses texel row 0 -- the repo-wide rule.
 */
#include "cx_common_b.h"
#include "cx_extract.h"
#include "cx_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* [C] FUN_00188880 @0x001888BF relocates + registers this slot. */
#define ENV_TEX_FIELD_ENVMAP 0xA0

int cx_extract_envmap(const char *game_dir, const char *track_dir,
                      const char *track_id, const char *out_dir)
{
    char path[4096];
    cxb_blob d = { NULL, 0 };
    cxb_texrec r;
    int32_t off;
    unsigned char *px = NULL;
    size_t n;
    double lum = 0.0, al = 0.0;
    size_t i;
    int rc = 1;

    (void)game_dir;

    snprintf(path, sizeof path, "%s/enviro.dat", track_dir);
    if (!cxb_file_exists(path)) {
        printf("%-12s no enviro.dat\n", track_id);
        goto done;
    }
    if (cxb_read_file(path, &d) != 0) {
        fprintf(stderr, "[cx_envmap] cannot read %s\n", path);
        goto done;
    }

    off = cxb_i32(&d, ENV_TEX_FIELD_ENVMAP);
    if (!off || cxb_parse_texrec(&d, off, &r) != 0) {
        printf("%-12s enviro.dat +0xA0 EMPTY -- no env map ships for this "
               "track; runtime falls back\n", track_id);
        rc = 0;                       /* not an error: four tracks ship none */
        goto done;
    }

    n = (size_t)r.w * (size_t)r.h * 4u;
    px = (unsigned char *)malloc(n);
    if (!px)
        goto done;

    if (r.fmt == CXB_FMT_DXT1 || r.fmt == CXB_FMT_DXT3 || r.fmt == CXB_FMT_DXT5) {
        if (cxb_decode_dxt(&d, r.data_off, r.w, r.h, r.fmt, px) != 0) {
            printf("%-12s +0xA0 %-24s %4dx%-4d %-9s data@0x%06X   "
                   "[!] bitmap runs past end of file\n",
                   track_id, r.name, r.w, r.h, cxb_fmt_name(r.fmt),
                   (unsigned)r.data_off);
            goto done;
        }
    } else if (r.fmt == CXB_FMT_RGBA) {
        if (cxb_decode_rgba(&d, r.data_off, r.w, r.h, px) != 0) {
            printf("%-12s +0xA0 %-24s %4dx%-4d %-9s data@0x%06X   "
                   "[!] bitmap runs past end of file\n",
                   track_id, r.name, r.w, r.h, cxb_fmt_name(r.fmt),
                   (unsigned)r.data_off);
            goto done;
        }
    } else {
        printf("%-12s +0xA0 %-24s %4dx%-4d %-9s data@0x%06X   "
               "[!] format not decodable here\n",
               track_id, r.name, r.w, r.h, cxb_fmt_name(r.fmt),
               (unsigned)r.data_off);
        goto done;
    }

    for (i = 0; i < n; i += 4) {
        lum += (double)px[i] + (double)px[i + 1] + (double)px[i + 2];
        al += (double)px[i + 3];
    }

    if (cxb_mkdir_p(out_dir) != 0) {
        fprintf(stderr, "[cx_envmap] cannot create %s\n", out_dir);
        goto done;
    }
    snprintf(path, sizeof path, "%s/envmap.png", out_dir);
    /* row 0 of the block grid is row 0 of the PNG -- no flip.  V-origin rule. */
    if (cx_png_write_rgba8(path, px, r.w, r.h) != 0) {
        fprintf(stderr, "[cx_envmap] cannot write %s\n", path);
        goto done;
    }
    printf("%-12s +0xA0 %-24s %4dx%-4d %-9s data@0x%06X  meanRGB=%.1f "
           "meanA=%.1f  -> %s\n",
           track_id, r.name, r.w, r.h, cxb_fmt_name(r.fmt),
           (unsigned)r.data_off,
           lum / (3.0 * (double)((size_t)r.w * (size_t)r.h)),
           al / (double)((size_t)r.w * (size_t)r.h), path);
    rc = 0;

done:
    free(px);
    cxb_blob_free(&d);
    return rc;
}
