/* cx_gen_retail_struct.c -- the C port of tools/gen_retail_struct.py.
 *
 * Emit a RETAIL-SHAPED vehicle struct from the port's own offset annotations.
 *
 * Goal: one struct whose every field sits exactly where the game puts it, so
 * the port's C and the retail x86 can run over the SAME bytes with nothing
 * marshalled between them.  Source of truth is the annotations already in
 * src/burnout3_vehicle_sim.h (`float mass;  // +0x1F0`).  This reads them,
 * sorts by offset, reports every conflict, and writes a struct with explicit
 * padding and a _Static_assert per field so the layout cannot drift silently.
 *
 * ================================================== THE FIELD PATTERN [spec]
 * The python original matches each line with
 *
 *     ^\s*(type)\s+(name)(arr)\s*;\s*(comment)?$
 *
 * where `type` is an optional `const`, an optional `unsigned`/`signed`, and an
 * identifier -- and where the WHITESPACE INSIDE the type is part of the
 * captured text, because the emitted column is `%-14s` of exactly that text.
 * The alternatives are tried in the regex engine's order (const first, then
 * unsigned before signed, each optional), so `unsigned foo;` still parses as
 * type `unsigned` / name `foo` by falling through to the shorter alternative.
 * That ordering is reproduced literally below; it is the difference between
 * `unsigned char x` reading as one type and reading as two fields.
 *
 * The offset comes from the comment's first `+0x<hex>` (2-4 digits), and
 * failing that from a trailing `_<hex>` on the NAME itself (3-4 digits) -- so
 * `aggr_time_13E0`, whose comment is prose, still lands at 0x13E0.
 */
#include "cx_common_g.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Retail's live vehicle object: the highest annotated offset is +0x1920
 * (racecar class), and FUN_00110280 strides the traffic pool well past it. */
#define CXG_RS_SPAN 0x1A00

#define CXG_RS_MAX 1024

static const struct {
    const char *name;
    unsigned    size;
} CXG_RS_SIZES[] = {
    { "float", 4 }, { "int", 4 }, { "unsigned", 4 }, { "unsigned int", 4 },
    { "unsigned short", 2 }, { "short", 2 },
    { "unsigned char", 1 }, { "signed char", 1 }, { "char", 1 },
};

typedef struct {
    unsigned off;
    char     name[128];
    char     ctype[64];
    char     arr[64];
    unsigned size;
} cxg_rs_field;

typedef struct {
    unsigned off;
    char     name[128];
    unsigned size;
    long     end;                /* -1 == ran past the span */
} cxg_rs_drop;

static int cxg_isw(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int cxg_isalpha_(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int cxg_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

static const char *cxg_ws(const char *p)
{
    while (cxg_is_ws(*p))
        p++;
    return p;
}

static int cxg_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* python's SIZES.get(ctype.replace("const ", "").strip()); 0 == not a type
 * this generator can size, which is how B3RigidBody and B3WheelSim drop out. */
static unsigned cxg_rs_base_size(const char *ctype)
{
    char        key[80];
    const char *p = ctype;
    size_t      w = 0, i;

    /* .replace("const ", "") -- every occurrence, as python's str.replace. */
    while (*p && w + 1 < sizeof(key)) {
        if (!strncmp(p, "const ", 6)) {
            p += 6;
            continue;
        }
        key[w++] = *p++;
    }
    key[w] = '\0';
    while (w && cxg_is_ws(key[w - 1]))
        key[--w] = '\0';
    p = key;
    while (cxg_is_ws(*p))
        p++;

    for (i = 0; i < sizeof(CXG_RS_SIZES) / sizeof(CXG_RS_SIZES[0]); i++)
        if (!strcmp(CXG_RS_SIZES[i].name, p))
            return CXG_RS_SIZES[i].size;
    return 0;
}

/* The array dimensions: every `[<n>]` multiplied together, as ARR.findall
 * does.  A `[SOMETHING]` with no digits contributes nothing, exactly as the
 * python `\[(\d+)\]` does when it fails to match. */
static unsigned cxg_rs_arr_mult(const char *arr)
{
    unsigned n = 1;

    while (*arr) {
        if (*arr == '[') {
            const char *q = arr + 1;
            unsigned    v = 0;
            int         digits = 0;

            while (*q >= '0' && *q <= '9') {
                v = v * 10u + (unsigned)(*q - '0');
                q++;
                digits++;
            }
            if (digits && *q == ']')
                n *= v;
        }
        arr++;
    }
    return n;
}

/* One attempt at the FIELD pattern with a fixed choice of the two optional
 * prefixes.  Returns 1 and fills `f` on a full-line match. */
static int cxg_rs_try(const char *line, int want_const, int sign_alt,
                      cxg_rs_field *f, const char **comment)
{
    const char *p = cxg_ws(line);
    const char *tstart = p, *b;
    size_t      n;

    if (want_const) {
        if (strncmp(p, "const", 5) != 0 || !cxg_is_ws(p[5]))
            return 0;
        p = cxg_ws(p + 5);
    }
    if (sign_alt == 1) {
        if (strncmp(p, "unsigned", 8) != 0 || !cxg_is_ws(p[8]))
            return 0;
        p = cxg_ws(p + 8);
    } else if (sign_alt == 2) {
        if (strncmp(p, "signed", 6) != 0 || !cxg_is_ws(p[6]))
            return 0;
        p = cxg_ws(p + 6);
    }
    if (!cxg_isalpha_(*p))
        return 0;
    b = p;
    while (cxg_isw(*p))
        p++;
    n = (size_t)(p - tstart);
    if (n >= sizeof(f->ctype) || b == p)
        return 0;
    memcpy(f->ctype, tstart, n);
    f->ctype[n] = '\0';

    if (!cxg_is_ws(*p))                     /* the mandatory \s+ */
        return 0;
    p = cxg_ws(p);
    if (!cxg_isw(*p))                       /* (?P<name>\w+) */
        return 0;
    b = p;
    while (cxg_isw(*p))
        p++;
    n = (size_t)(p - b);
    if (n >= sizeof(f->name))
        return 0;
    memcpy(f->name, b, n);
    f->name[n] = '\0';

    p = cxg_ws(p);
    b = p;
    while (*p == '[') {                     /* (?:\[[^\]]*\])* */
        const char *q = p + 1;

        while (*q && *q != ']')
            q++;
        if (*q != ']')
            break;
        p = q + 1;
    }
    n = (size_t)(p - b);
    if (n >= sizeof(f->arr))
        return 0;
    memcpy(f->arr, b, n);
    f->arr[n] = '\0';

    p = cxg_ws(p);
    if (*p != ';')
        return 0;
    p = cxg_ws(p + 1);
    *comment = NULL;
    if (*p == '\0')
        return 1;                           /* `$` right after the `;` */
    if (p[0] != '/' || p[1] != '/')
        return 0;                           /* the trailing group is `//`-only */
    *comment = cxg_ws(p + 2);               /* `//\s*(?P<c>.*)$` */
    return 1;
}

/* The full pattern, alternatives in the regex engine's own order. */
static int cxg_rs_match(const char *line, cxg_rs_field *f, const char **comment)
{
    static const struct { int c, s; } ALT[6] = {
        { 1, 1 }, { 1, 2 }, { 1, 0 }, { 0, 1 }, { 0, 2 }, { 0, 0 },
    };
    int i;

    for (i = 0; i < 6; i++)
        if (cxg_rs_try(line, ALT[i].c, ALT[i].s, f, comment))
            return 1;
    return 0;
}

/* OFF_C: the first `+0x<hex>` in the comment, 2 to 4 digits, greedy. */
static int cxg_rs_off_from_comment(const char *c, unsigned *off)
{
    for (; c && *c; c++) {
        if (c[0] != '+' || c[1] != '0' || c[2] != 'x')
            continue;
        {
            const char *q = c + 3;
            unsigned    v = 0;
            int         n = 0;

            while (n < 4 && cxg_hexval(*q) >= 0) {
                v = v * 16u + (unsigned)cxg_hexval(*q);
                q++;
                n++;
            }
            if (n >= 2) {
                *off = v;
                return 1;
            }
        }
    }
    return 0;
}

/* OFF_N: `_([0-9A-Fa-f]{3,4})$` on the name -- greedy, anchored at the end. */
static int cxg_rs_off_from_name(const char *name, unsigned *off)
{
    size_t len = strlen(name);
    int    n;

    for (n = 4; n >= 3; n--) {
        const char *q;
        unsigned    v = 0;
        int         i;

        if (len < (size_t)n + 1)
            continue;
        q = name + len - n;
        if (q[-1] != '_')
            continue;
        for (i = 0; i < n; i++) {
            int d = cxg_hexval(q[i]);

            if (d < 0)
                break;
            v = v * 16u + (unsigned)d;
        }
        if (i == n) {
            *off = v;
            return 1;
        }
    }
    return 0;
}

/* python's sort key (offset, name). */
static int cxg_rs_cmp(const void *a, const void *b)
{
    const cxg_rs_field *x = (const cxg_rs_field *)a;
    const cxg_rs_field *y = (const cxg_rs_field *)b;

    if (x->off != y->off)
        return x->off < y->off ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int cxg_rs_collect(const char *src, const char *want,
                          cxg_rs_field *out, int cap)
{
    char        cur[128] = "";
    const char *p = src;
    int         n = 0;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        char        line[2048];
        const char *q;

        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        while (len && cxg_is_ws(line[len - 1]))     /* line.rstrip() */
            line[--len] = '\0';

        /* ^\s*typedef struct (\w+)\s*\{ */
        q = cxg_ws(line);
        if (!strncmp(q, "typedef struct ", 15)) {
            const char *b = q + 15;
            const char *e = b;

            while (cxg_isw(*e))
                e++;
            if (e > b && *cxg_ws(e) == '{') {
                size_t l = (size_t)(e - b);

                if (l >= sizeof(cur))
                    l = sizeof(cur) - 1;
                memcpy(cur, b, l);
                cur[l] = '\0';
                goto next;
            }
        }
        /* ^\}\s*\w*\s*; -- column 0, as re.match anchors it */
        if (cur[0] && line[0] == '}') {
            const char *e = cxg_ws(line + 1);

            while (cxg_isw(*e))
                e++;
            if (*cxg_ws(e) == ';') {
                cur[0] = '\0';
                goto next;
            }
        }
        if (strcmp(cur, want) != 0)
            goto next;

        {
            cxg_rs_field f;
            const char  *comment = NULL;
            unsigned     off = 0, base;

            memset(&f, 0, sizeof(f));
            if (!cxg_rs_match(line, &f, &comment))
                goto next;
            if (!cxg_rs_off_from_comment(comment, &off) &&
                !cxg_rs_off_from_name(f.name, &off))
                goto next;
            base = cxg_rs_base_size(f.ctype);
            if (!base)
                goto next;
            f.off = off;
            f.size = base * cxg_rs_arr_mult(f.arr);
            if (n < cap)
                out[n++] = f;
        }
next:
        if (!nl)
            break;
        p = nl + 1;
    }
    return n;
}

static int cxg_rs_build(cxg_str *L, const char *repo, int *n_all, int *n_kept,
                        cxg_rs_drop *drop, int *n_drop)
{
    char          path[4096];
    char         *src;
    cxg_rs_field *fields, *kept;
    int           nf, nk = 0, nd = 0, i, npad = 0;
    unsigned      end = 0, cur = 0;

    cxg_join(path, sizeof(path), repo, "src/burnout3_vehicle_sim.h");
    src = cxg_read_text(path, NULL);
    if (!src)
        return 1;
    fields = (cxg_rs_field *)malloc(sizeof(*fields) * CXG_RS_MAX);
    kept   = (cxg_rs_field *)malloc(sizeof(*kept) * CXG_RS_MAX);
    if (!fields || !kept) {
        free(src);
        free(fields);
        free(kept);
        return 1;
    }
    nf = cxg_rs_collect(src, "B3VehicleFull", fields, CXG_RS_MAX);
    free(src);
    qsort(fields, (size_t)nf, sizeof(*fields), cxg_rs_cmp);

    /* conflicts: two fields claiming overlapping bytes */
    for (i = 0; i < nf; i++) {
        if (fields[i].off < end) {
            if (nd < CXG_RS_MAX) {
                drop[nd].off = fields[i].off;
                snprintf(drop[nd].name, sizeof(drop[nd].name), "%s",
                         fields[i].name);
                drop[nd].size = fields[i].size;
                drop[nd].end = (long)end;
                nd++;
            }
            continue;
        }
        if (fields[i].off + fields[i].size > CXG_RS_SPAN) {
            if (nd < CXG_RS_MAX) {
                drop[nd].off = fields[i].off;
                snprintf(drop[nd].name, sizeof(drop[nd].name), "%s",
                         fields[i].name);
                drop[nd].size = fields[i].size;
                drop[nd].end = -1;
                nd++;
            }
            continue;
        }
        kept[nk++] = fields[i];
        end = fields[i].off + fields[i].size;
    }

    cxg_sline(L, "// GENERATED by tools/gen_retail_struct.py -- do not edit by hand.");
    cxg_sline(L, "//");
    cxg_sline(L, "// Burnout 3's live vehicle object, at RETAIL's own offsets.");
    cxg_sline(L, "//");
    cxg_sline(L, "// The point of this shape is that there is nothing to convert: the");
    cxg_sline(L, "// port's C and the game's x86 address the same bytes at the same");
    cxg_sline(L, "// places, so a feature can be switched between them without a");
    cxg_sline(L, "// marshalling layer in the middle. Every field carries a");
    cxg_sline(L, "// _Static_assert, so an offset that drifts fails the build rather");
    cxg_sline(L, "// than corrupting state at runtime.");
    cxg_sline(L, "//");
    cxg_sline(L, "// Harness-only state does NOT belong here. Retail code runs over");
    cxg_sline(L, "// this memory; anything invented squatting on bytes we have not");
    cxg_sline(L, "// identified would be trampled, or would trample. Keep GLUE in a");
    cxg_sline(L, "// parallel struct keyed by the same index.");
    cxg_sline(L, "#ifndef BURNOUT3_VEHICLE_RETAIL_H");
    cxg_sline(L, "#define BURNOUT3_VEHICLE_RETAIL_H");
    cxg_nl(L);
    cxg_sline(L, "#include <stddef.h>");
    cxg_nl(L);
    cxg_sline(L, "#define B3_VEHICLE_RETAIL_SPAN 0x%04Xu", (unsigned)CXG_RS_SPAN);
    cxg_nl(L);
    cxg_sline(L, "// PACKED deliberately. Retail's layout has no alignment we");
    cxg_sline(L, "// control -- the wheel count is a byte at +0x1169, and a 4-byte");
    cxg_sline(L, "// field at a non-4-aligned offset makes the compiler insert its");
    cxg_sline(L, "// own padding and shift everything after it. Explicit padding");
    cxg_sline(L, "// only holds if the compiler adds none of its own.");
    cxg_sline(L, "typedef struct __attribute__((packed)) B3VehicleRetail {");
    for (i = 0; i < nk; i++) {
        int gap = (int)(22 - strlen(kept[i].name) - strlen(kept[i].arr));

        if (kept[i].off > cur) {
            cxg_sline(L, "    unsigned char _pad%02d[0x%X];", npad,
                      kept[i].off - cur);
            npad++;
        }
        if (gap < 1)
            gap = 1;
        cxg_sline(L, "    %-14s %s%s;%*s// +0x%04X", kept[i].ctype,
                  kept[i].name, kept[i].arr, gap, "", kept[i].off);
        cur = kept[i].off + kept[i].size;
    }
    if (cur < CXG_RS_SPAN)
        cxg_sline(L, "    unsigned char _pad%02d[0x%X];", npad,
                  CXG_RS_SPAN - cur);
    cxg_sline(L, "} B3VehicleRetail;");
    cxg_nl(L);
    cxg_sline(L, "_Static_assert(sizeof(B3VehicleRetail) == B3_VEHICLE_RETAIL_SPAN,");
    cxg_sline(L, "               \"vehicle span drifted\");");
    for (i = 0; i < nk; i++) {
        cxg_sline(L, "_Static_assert(offsetof(B3VehicleRetail, %s) == 0x%04X,",
                  kept[i].name, kept[i].off);
        cxg_sline(L, "               \"%s moved off retail's offset\");",
                  kept[i].name);
    }
    cxg_nl(L);
    cxg_sputs(L, "#endif // BURNOUT3_VEHICLE_RETAIL_H\n");

    *n_all = nf;
    *n_kept = nk;
    *n_drop = nd;
    free(fields);
    free(kept);
    return 0;
}

int cx_extract_gen_retail_struct(const char *game_dir, const char *out_root)
{
    const char  *repo = cxg_repo_root();
    cxg_str      L = { 0 };
    char         out[4096], rel[4096];
    cxg_rs_drop *drop;
    int          nf = 0, nk = 0, nd = 0, i, rc;

    (void)game_dir;
    drop = (cxg_rs_drop *)malloc(sizeof(*drop) * CXG_RS_MAX);
    if (!drop)
        return 1;
    cxg_gen_path(out, sizeof(out), out_root, "burnout3_vehicle_retail.h");
    rc = cxg_rs_build(&L, repo, &nf, &nk, drop, &nd);
    if (rc == 0)
        rc = cxg_str_write(&L, out);
    cxg_str_free(&L);
    if (rc == 0) {
        printf("%d annotated field(s); %d placed, %d dropped\n", nf, nk, nd);
        if (nd) {
            printf("\nDROPPED -- these need a decision before the shape is real:\n");
            for (i = 0; i < nd; i++) {
                char why[128];

                if (drop[i].end >= 0)
                    snprintf(why, sizeof(why),
                             "overlaps the field ending at 0x%04X",
                             (unsigned)drop[i].end);
                else
                    snprintf(why, sizeof(why), "runs past the 0x%04X span",
                             (unsigned)CXG_RS_SPAN);
                printf("  0x%04X %-26s (%2d B)  %s\n", drop[i].off,
                       drop[i].name, (int)drop[i].size, why);
            }
        }
        cxg_relpath(rel, sizeof(rel), out, repo);
        printf("\nwrote %s\n", rel);
    }
    free(drop);
    return rc;
}
