/* cx_textures.c -- port of tools/extract_textures.py.
 *
 * static.dat's texture bank -> <out_dir>/textures/<name>.png.
 *
 * Format credit: Burnout Modding community (burnout.wiki) via EdnessP's Noesis
 * plugin.  Condensed from the python docstring:
 *
 *   static.dat holds the texture count at +0x16 (u16) and a pointer array at
 *   +0x18 for version > 0x25; for the old layout the count is a u32 at +0x14.
 *   Each pointer is an absolute offset to a texture record:
 *     +0x04  i32  bitmap data offset, RELATIVE to the record
 *     +0x34  u32  format: 0xB paletted, 0xC DXT1, 0xE DXT3, 0xF DXT5, 0x3A RGBA
 *     +0x38  u32  width      +0x3C  u32  height
 *     +0x40  u32  bit depth -- 4/8/32 => name at +0x48, else name at +0x44
 *
 *   Only DXT1 and DXT5 are implemented, which covers EVERY texture in the
 *   shipped track files: across all 36 tracks, 9,224 records = 4,045 DXT1 +
 *   5,179 DXT5 and nothing else, with zero LIN_* surfaces.  Unhandled formats
 *   are reported rather than silently skipped.                             [C]
 *
 *   THE V ORIGIN RULE: the first 0x14 bytes of the record ARE the Xbox
 *   D3DPixelContainer the build tool baked and the loader hands the GPU
 *   untouched (FUN_001D7040 -> FUN_0034DCA0 = D3DDevice_SetTexture pushes
 *   tex[1]/tex[3] as TX_OFFSET/TX_FORMAT), and NV2A TX_FORMAT has no origin,
 *   flip or mirror field.  So file row 0 == surface row 0 == v 0, and this
 *   tool writes PNG row 0 = surface row 0.  No consumer may flip.          [C]
 */
#include "cx_common_b.h"
#include "cx_extract.h"
#include "cx_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[128];
    unsigned n;
} SkipRow;

static void skip_bump(SkipRow *rows, int *nrows, const char *key)
{
    int i;
    for (i = 0; i < *nrows; i++) {
        if (strcmp(rows[i].key, key) == 0) {
            rows[i].n++;
            return;
        }
    }
    if (*nrows >= 16)
        return;
    snprintf(rows[*nrows].key, sizeof rows[*nrows].key, "%s", key);
    rows[*nrows].n = 1;
    (*nrows)++;
}

static int cmp_skip(const void *a, const void *b)
{
    return strcmp(((const SkipRow *)a)->key, ((const SkipRow *)b)->key);
}

int cx_extract_textures(const char *game_dir, const char *track_dir,
                        const char *track_id, const char *out_dir)
{
    char path[4352], texdir[4096];
    cxb_blob d = { NULL, 0 };
    uint32_t ver, count, table;
    uint32_t i;
    unsigned char *rgba = NULL;
    size_t rgba_cap = 0;
    SkipRow skipped[16];
    int nskip = 0;
    unsigned written = 0;
    int rc = 1;

    (void)game_dir;

    snprintf(path, sizeof path, "%s/static.dat", track_dir);
    if (cxb_read_file(path, &d) != 0) {
        fprintf(stderr, "[cx_textures] cannot read %s\n", path);
        goto done;
    }

    ver = cxb_u32(&d, 0);
    if (ver > 0x25u) {
        count = cxb_u16(&d, 0x16);
        table = cxb_u32(&d, 0x18);
    } else {
        count = cxb_u32(&d, 0x14);
        table = cxb_u32(&d, 0x18);
    }

    snprintf(texdir, sizeof texdir, "%s/textures", out_dir);
    if (cxb_mkdir_p(texdir) != 0) {
        fprintf(stderr, "[cx_textures] cannot create %s\n", texdir);
        goto done;
    }

    /* A track's texture bank is ~1.0 s of DXT decode and deflate at race
     * load.  The walk stays serial -- `skipped` is a keyed tally and two
     * records CAN carry the same name, in which case the later one is meant
     * to overwrite the earlier -- and the encode is deferred to the pool.
     * The queue's duplicate-path rule (cx_png.h) is exactly what preserves
     * "later wins" here; it flushes rather than letting the two race. */
    cx_png_queue_begin();

    for (i = 0; i < count; i++) {
        int64_t rec = (int64_t)cxb_u32(&d, (size_t)table + (size_t)i * 4u);
        int64_t bmp;
        int32_t bmp_rel;
        uint32_t fmt, w, h, bd;
        char stored[80], name[80], key[128];
        size_t need;

        if (!(rec > 0 && (uint64_t)rec < (uint64_t)d.n - 0x70u)) {
            skip_bump(skipped, &nskip, "bad record ptr");
            continue;
        }
        bmp_rel = cxb_i32(&d, (size_t)rec + 4);
        bmp = bmp_rel ? (int64_t)bmp_rel + rec : 0;
        fmt = cxb_u32(&d, (size_t)rec + 0x34);
        w = cxb_u32(&d, (size_t)rec + 0x38);
        h = cxb_u32(&d, (size_t)rec + 0x3C);
        bd = cxb_u32(&d, (size_t)rec + 0x40);

        /* python: `if not name` catches BOTH a failed decode and an empty
         * string, and the basename is likewise replaced when it comes out
         * empty (a stored name ending in a separator). */
        if (cxb_read_cstr(&d, rec + ((bd == 4u || bd == 8u || bd == 32u)
                                     ? 0x48 : 0x44), 64,
                          stored, sizeof stored) != 0 || !stored[0])
            snprintf(stored, sizeof stored, "tex_%03u", i);
        cxb_basename(stored, name, sizeof name);
        if (!name[0])
            snprintf(name, sizeof name, "tex_%03u", i);

        if (fmt != CXB_FMT_DXT1 && fmt != CXB_FMT_DXT5) {
            snprintf(key, sizeof key, "unhandled fmt 0x%X (%s)",
                     fmt, cxb_fmt_name(fmt));
            skip_bump(skipped, &nskip, key);
            continue;
        }
        if (!(w > 0u && w <= 4096u && h > 0u && h <= 4096u)
            || !(bmp > 0 && (uint64_t)bmp < (uint64_t)d.n)) {
            skip_bump(skipped, &nskip, "bad dims/offset");
            continue;
        }

        need = (size_t)w * (size_t)h * 4u;
        if (need > rgba_cap) {
            unsigned char *p = (unsigned char *)realloc(rgba, need);
            if (!p)
                goto done;
            rgba = p;
            rgba_cap = need;
        }
        if (cxb_decode_dxt(&d, bmp, (int)w, (int)h, fmt, rgba) != 0) {
            skip_bump(skipped, &nskip, "data past EOF");
            continue;
        }
        snprintf(path, sizeof path, "%s/%s.png", texdir, name);
        if (cx_png_write_rgba8(path, rgba, (int)w, (int)h) != 0) {
            fprintf(stderr, "[cx_textures] cannot write %s\n", path);
            goto done;
        }
        written++;
    }

    if (cx_png_queue_flush() != 0)
        fprintf(stderr, "[cx_textures] one or more PNGs failed to write\n");

    printf("[cx_textures] track %s\n", track_id);
    printf("texture entries : %u\n", count);
    printf("written         : %u PNGs -> %s\n", written, texdir);
    if (nskip) {
        int k;
        qsort(skipped, (size_t)nskip, sizeof skipped[0], cmp_skip);
        for (k = 0; k < nskip; k++)
            printf("  skipped %-32s %u\n", skipped[k].key, skipped[k].n);
    }
    rc = written ? 0 : 1;

done:
    /* the failure arms jump past the flush above; a second one is a no-op on
     * a drained queue and is what closes it */
    cx_png_queue_flush();
    free(rgba);
    cxb_blob_free(&d);
    return rc;
}
