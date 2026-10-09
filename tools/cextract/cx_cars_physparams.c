/* cx_cars_physparams.c -- port of tools/extract_physics_params.py.
 *
 * The vehicle physics parameter TABLE -- name, group and struct offset for
 * every tunable the game registers -- to
 * <out_root>/gen/burnout3_physics_params.h.  (The python writes src/; this
 * pipeline writes gen/ with byte-identical CONTENT.)  That header is also
 * what cx_cars_vdb.c reads back for its 64 (offset, group, name) rows, so
 * this is the head of the tuning chain.
 *
 * ================================================================ EVIDENCE
 * FUN_00132D10 registers every tunable physics value with the game's
 * ValueDB.  Each registration compiles to
 *
 *     LEA <reg>,[ESI + 0xNNN]     ; address of the field in the physics struct
 *     ...
 *     MOV ECX, <group string VA>  ; e.g. "Physics/Transmission/Engine"
 *     MOV EDX, <name string VA>   ; e.g. "Peak Torque Revs"
 *     CALL 0x001AEE20             ; register(name, group, &field, ...)
 *
 * so walking the disassembly backwards from each CALL yields name -> group ->
 * struct offset for the whole model.  The last registration in the function
 * folds the displacement into ESI itself (ADD ESI,0x1CC / PUSH ESI) rather
 * than using LEA, because ESI is dead after it.  Strings are read straight
 * out of the XBE .rdata.  The compiled-in defaults come from the constructor
 * FUN_00132950, where Ghidra renders each store as
 *
 *     *(undefined4 *)(param_1 + 0x130) = 0x3ff33333;
 *
 * and occasionally mistakes a float immediate for a pointer:
 *
 *     *(undefined1 **)(param_1 + 0xd4) = &DAT_3e4ccccd;
 *
 * Both forms carry the same 32-bit pattern, which is the IEEE-754 float.
 *
 * ========================================================= THE DEPENDENCY
 * Unlike every other module here, this one is NOT self-contained: the python
 * mines Ghidra, not the game files, and a faithful port mines the same
 * Ghidra.  It speaks HTTP to the same MCP bridge the python uses --
 * $B3_GHIDRA_MCP, default 127.0.0.1:8089 -- calling the same two endpoints
 * with the same arguments:
 *
 *     /disassemble_function?address=0x00132d10&limit=8000
 *     /decompile_function?address=0x00132950
 *
 * With the bridge down the stage fails cleanly and writes nothing, exactly as
 * the python does.  It is therefore a REGENERATION tool, not part of a cold
 * asset dump: the header it produces is checked in, and the extractor family
 * that consumes it (cx_cars_vdb.c) reads the checked-in copy.
 *
 * ============================================================ PYTHON QUIRKS
 *  Q22 the backward walk takes the CLOSEST preceding `MOV EDX,imm` as the
 *      name and the closest `MOV ECX,imm` as the group -- but only if the VA
 *      resolves to a string: a VA outside .rdata, longer than 96 bytes, or
 *      not ASCII yields None and the walk keeps looking FURTHER BACK.
 *  Q23 the window is a fixed 40 instructions, and a (group, name, offset)
 *      triple already seen is dropped, so a repeated registration collapses.
 *  Q24 cfloat() appends ".0" unless the "%.9g" result contains one of
 *      '.eEnN' -- a DIFFERENT set from cx_cars_vdb.c's ".e", which is why
 *      the two generated headers format their literals with different code.
 *  Q25 the LEA register must match `E[A-D]I?X?|E[SD]I`, so a LEA off ESI into
 *      EBP or ESP is deliberately NOT accepted as a registration operand.
 *  Q26 rows are sorted by offset with python's STABLE sort, and the group
 *      blocks are then emitted in sorted-name order while the trailing
 *      `#define B3_PHYS_*` list stays in offset order.
 */
/* getaddrinfo()/connect() are POSIX, and -std=c11 hides them by default. */
#define _POSIX_C_SOURCE 200112L

#include "cx_cars.h"
#include "cx_extract.h"

#include <ctype.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* the XBE the python reads the .rdata strings out of */
#define RDATA_VA   0x0036B7C0u
#define RDATA_RAW  0x0035C000u
#define RDATA_SIZE 0x00046B94u

#define REGISTER_FN  "CALL 0x001aee20"
#define DISASM_PATH  "/disassemble_function?address=0x00132d10&limit=8000"
#define DECOMP_PATH  "/decompile_function?address=0x00132950"

/* ================================================================== HTTP */
/* One blocking GET against the MCP bridge; the body is returned malloc'd. */
static char *http_get(const char *hostport, const char *path)
{
    char host[256], req[1024];
    const char *colon;
    const char *port = "8089";
    struct addrinfo hints, *res = NULL, *ai;
    int fd = -1;
    char *buf = NULL, *body;
    size_t cap = 0, n = 0;

    colon = strchr(hostport, ':');
    if (colon) {
        size_t l = (size_t)(colon - hostport);
        if (l >= sizeof host)
            return NULL;
        memcpy(host, hostport, l);
        host[l] = 0;
        port = colon + 1;
    } else {
        snprintf(host, sizeof host, "%s", hostport);
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return NULL;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return NULL;

    snprintf(req, sizeof req,
             "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, host);
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return NULL;
    }
    for (;;) {
        ssize_t r;
        if (n + 65536u + 1u > cap) {
            char *nb;
            cap = cap ? cap * 2 : 262144;
            while (n + 65536u + 1u > cap)
                cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        r = read(fd, buf + n, 65536);
        if (r < 0) { free(buf); close(fd); return NULL; }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    if (!buf)
        return NULL;
    buf[n] = 0;
    body = strstr(buf, "\r\n\r\n");
    if (body) {
        memmove(buf, body + 4, n - (size_t)(body + 4 - buf) + 1u);
    } else {
        body = strstr(buf, "\n\n");
        if (body)
            memmove(buf, body + 2, n - (size_t)(body + 2 - buf) + 1u);
    }
    return buf;
}

/* Read the JSON string starting at *p (which points just past the opening
 * quote) into `out`, unescaping; leaves *p on the closing quote. */
static size_t json_str(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    size_t n = 0;

    while (*s && *s != '"') {
        int c;
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'u': {                          /* only ASCII ever appears */
                unsigned v = 0;
                int k;
                for (k = 1; k <= 4 && s[k]; k++) {
                    int h = s[k];
                    h = (h >= '0' && h <= '9') ? h - '0'
                      : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                      : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0;
                    v = v * 16u + (unsigned)h;
                }
                s += 4;
                c = (int)(v & 0xFFu);
                break;
            }
            default:   c = *s; break;
            }
            if (!*s)
                break;
        } else {
            c = *s;
        }
        if (n + 1 < cap)
            out[n++] = (char)c;
        s++;
    }
    *p = s;
    if (cap)
        out[n] = 0;
    return n;
}

/* ============================================================ XBE strings */
static cxd_blob g_xbe;

/* load_strings()'s cstr(): NULL unless the VA lands inside .rdata, the string
 * is at most 96 bytes and decodes as ASCII (Q22). */
static const char *xbe_cstr(uint32_t va, char *buf, size_t cap)
{
    size_t o, e;

    if (!(va >= RDATA_VA && va < RDATA_VA + RDATA_SIZE))
        return NULL;
    o = RDATA_RAW + (va - RDATA_VA);
    if (o >= g_xbe.n)
        return NULL;
    e = o;
    while (e < g_xbe.n && g_xbe.d[e])
        e++;
    if (e >= g_xbe.n)
        return NULL;
    if (e - o > 96)
        return NULL;
    if (e - o >= cap)
        return NULL;
    {
        size_t i;
        for (i = o; i < e; i++)
            if (g_xbe.d[i] >= 0x80)              /* python: ascii decode fails */
                return NULL;
    }
    memcpy(buf, g_xbe.d + o, e - o);
    buf[e - o] = 0;
    return buf;
}

/* ======================================================= instruction forms */
static int lc_hex(const char **p, uint32_t *out)
{
    const char *s = *p;
    uint32_t v = 0;
    int n = 0;

    while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')) {
        v = v * 16u + (uint32_t)(*s <= '9' ? *s - '0' : *s - 'a' + 10);
        s++;
        n++;
    }
    if (!n)
        return 0;
    *p = s;
    *out = v;
    return 1;
}

/* ^MOV (E[A-D]X),(0x[0-9a-f]+)$ */
static int match_imm(const char *t, char *reg, uint32_t *va)
{
    const char *s = t;
    if (strncmp(s, "MOV E", 5) != 0)
        return 0;
    s += 5;
    if (!(*s >= 'A' && *s <= 'D') || s[1] != 'X' || s[2] != ',')
        return 0;
    reg[0] = 'E'; reg[1] = *s; reg[2] = 'X'; reg[3] = 0;
    s += 3;
    if (strncmp(s, "0x", 2) != 0)
        return 0;
    s += 2;
    if (!lc_hex(&s, va))
        return 0;
    return *s == 0;
}

/* ^LEA (E[A-D]I?X?|E[SD]I),\[ESI \+ (0x[0-9a-f]+)\]$   (Q25) */
static int match_lea(const char *t, uint32_t *off)
{
    const char *s = t;
    if (strncmp(s, "LEA E", 5) != 0)
        return 0;
    s += 5;
    if (*s >= 'A' && *s <= 'D') {
        s++;
        if (*s == 'I') s++;
        if (*s == 'X') s++;
    } else if ((*s == 'S' || *s == 'D') && s[1] == 'I') {
        s += 2;
    } else {
        return 0;
    }
    if (strncmp(s, ",[ESI + 0x", 10) != 0)
        return 0;
    s += 10;
    if (!lc_hex(&s, off))
        return 0;
    return s[0] == ']' && s[1] == 0;
}

/* ^ADD ESI,(0x[0-9a-f]+)$ */
static int match_addesi(const char *t, uint32_t *off)
{
    const char *s = t;
    if (strncmp(s, "ADD ESI,0x", 10) != 0)
        return 0;
    s += 10;
    if (!lc_hex(&s, off))
        return 0;
    return *s == 0;
}

/* ============================================================== defaults */
/* STORE = \(param_1 \+ (0x[0-9a-f]+|\d+)\) = (?:&DAT_([0-9a-f]{8})
 *          |(0x[0-9a-f]+|\d+)); */
typedef struct { unsigned off; uint32_t bits; } cxd_def;

static int parse_number(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (s[0] == '0' && s[1] == 'x') {
        const char *q = s + 2;
        if (!lc_hex(&q, out))
            return 0;
        *p = q;
        return 1;
    }
    if (*s >= '0' && *s <= '9') {
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10u + (uint32_t)(*s - '0');
            s++;
        }
        *p = s;
        *out = v;
        return 1;
    }
    return 0;
}

static int extract_defaults(const char *text, cxd_def **out, int *n_out)
{
    const char *s = text;
    cxd_def *v = NULL;
    int n = 0, cap = 0;

    while ((s = strstr(s, "(param_1 + ")) != NULL) {
        const char *p = s + 11;
        uint32_t off, bits;
        int ok = 0;
        if (parse_number(&p, &off) && strncmp(p, ") = ", 4) == 0) {
            p += 4;
            if (strncmp(p, "&DAT_", 5) == 0) {
                const char *q = p + 5;
                uint32_t b = 0;
                int k;
                for (k = 0; k < 8; k++) {
                    int c = q[k];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                        break;
                    b = b * 16u + (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
                }
                if (k == 8 && q[8] == ';') {
                    bits = b;
                    p = q + 9;
                    ok = 1;
                }
            } else if (parse_number(&p, &bits) && *p == ';') {
                p++;
                ok = 1;
            }
        }
        if (ok) {
            int i, dup = -1;
            for (i = 0; i < n; i++)
                if (v[i].off == off) { dup = i; break; }
            if (dup >= 0) {
                v[dup].bits = bits;              /* python dict: overwrite */
            } else {
                if (n == cap) {
                    cap = cap ? cap * 2 : 128;
                    v = (cxd_def *)realloc(v, (size_t)cap * sizeof *v);
                    if (!v)
                        return -1;
                }
                v[n].off = off;
                v[n].bits = bits;
                n++;
            }
            s = p;                               /* non-overlapping, as finditer */
        } else {
            s += 11;
        }
    }
    *out = v;
    *n_out = n;
    return 0;
}

/* ================================================================= output */
/* cfloat(): '%.9g', then ".0" unless the result carries one of '.eEnN' (Q24),
 * then the 'f' suffix. */
static void cfloat(double v, char *out, size_t cap)
{
    snprintf(out, cap, "%.9g", v);
    if (!strpbrk(out, ".eEnN")) {
        size_t l = strlen(out);
        if (l + 3 < cap)
            memcpy(out + l, ".0", 3);
    }
    {
        size_t l = strlen(out);
        if (l + 2 < cap) {
            out[l] = 'f';
            out[l + 1] = 0;
        }
    }
}

/* ident(): re.sub(r'[^0-9A-Za-z]+', '_', s).strip('_').lower(), with a 'p_'
 * prefix when the result starts with a digit.  main() then upper()s it. */
static void ident_upper(const char *group, const char *name, char *out,
                        size_t cap)
{
    char raw[256], sub[256];
    const char *last;
    size_t i, n = 0, a, b;

    last = strrchr(group, '/');
    last = last ? last + 1 : group;
    snprintf(raw, sizeof raw, "%s_%s", last, name);

    for (i = 0; raw[i] && n + 1 < sizeof sub; i++) {
        int c = (unsigned char)raw[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')) {
            sub[n++] = (char)c;
        } else {
            if (n && sub[n - 1] == '_')           /* [^0-9A-Za-z]+ is one run */
                continue;
            sub[n++] = '_';
        }
    }
    sub[n] = 0;
    a = 0;
    b = n;
    while (a < b && sub[a] == '_') a++;           /* .strip('_') */
    while (b > a && sub[b - 1] == '_') b--;
    n = 0;
    if (b > a && sub[a] >= '0' && sub[a] <= '9') {
        if (n + 2 < cap) { out[n++] = 'P'; out[n++] = '_'; }
    }
    for (i = a; i < b && n + 1 < cap; i++)
        out[n++] = (char)toupper((unsigned char)sub[i]);
    out[n] = 0;
}

typedef struct {
    char     group[128];
    char     name[128];
    unsigned offset;
} cxd_row;

static int cmp_group_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int cx_extract_physics_params(const char *game_dir, const char *out_root)
{
    const char *mcp = getenv("B3_GHIDRA_MCP");
    char gen[4096], out[4096], p[4096];
    char *dis = NULL, *dec = NULL;
    cxd_row *rows = NULL;
    cxd_def *defs = NULL;
    char **ins = NULL;
    char **gnames = NULL;
    int nins = 0, cap = 0, nrows = 0, rcap = 0, ndefs = 0, ngroups = 0;
    int i, k, have = 0, rc = 1;
    FILE *f;

    if (!mcp || !*mcp)
        mcp = "127.0.0.1:8089";
    cxd_path(gen, sizeof gen, out_root, "/gen", NULL);
    cxd_path(out, sizeof out, gen, "/burnout3_physics_params.h", NULL);
    cxd_path(p, sizeof p, game_dir, "/default.xbe", NULL);
    if (cxd_read_file(p, &g_xbe) != 0) {
        fprintf(stderr, "[cx_cars_physparams] cannot read %s\n", p);
        return 1;
    }

    dis = http_get(mcp, DISASM_PATH);
    dec = http_get(mcp, DECOMP_PATH);
    if (!dis || !dec) {
        fprintf(stderr, "[cx_cars_physparams] no answer from the Ghidra MCP "
                "bridge at %s -- this stage mines Ghidra, exactly as "
                "tools/extract_physics_params.py does (set $B3_GHIDRA_MCP)\n",
                mcp);
        goto done;
    }

    /* the ordered instruction list; the addresses are never used */
    {
        const char *s = dis;
        while ((s = strstr(s, "\"instruction\":\"")) != NULL) {
            char t[512];
            s += 15;
            json_str(&s, t, sizeof t);
            if (nins == cap) {
                cap = cap ? cap * 2 : 512;
                ins = (char **)realloc(ins, (size_t)cap * sizeof *ins);
                if (!ins) goto done;
            }
            ins[nins] = (char *)malloc(strlen(t) + 1u);
            if (!ins[nins]) goto done;
            memcpy(ins[nins], t, strlen(t) + 1u);
            nins++;
            if (*s == '"')
                s++;
        }
    }
    /* the decompiled body of FUN_00132950 */
    {
        const char *s = strstr(dec, "\"decompiled\":\"");
        char *text;
        size_t need;
        if (!s) {
            fprintf(stderr, "[cx_cars_physparams] no `decompiled` field\n");
            goto done;
        }
        s += 14;
        need = strlen(s) + 1u;
        text = (char *)malloc(need);
        if (!text) goto done;
        json_str(&s, text, need);
        if (extract_defaults(text, &defs, &ndefs) != 0) { free(text); goto done; }
        free(text);
    }

    /* extract(): walk back from each CALL 0x001aee20 (Q22, Q23) */
    for (i = 0; i < nins; i++) {
        /* separate buffers: name and group are BOTH live across the walk */
        char nmbuf[128], gpbuf[128];
        const char *nm = NULL, *gp = NULL;
        int have_off = 0;
        uint32_t off = 0;
        int j, lo;

        if (strncmp(ins[i], REGISTER_FN, strlen(REGISTER_FN)) != 0)
            continue;
        lo = i - 40 < 0 ? 0 : i - 40;
        for (j = i - 1; j >= lo; j--) {
            const char *t = ins[j];
            char reg[8];
            uint32_t va;
            if (match_imm(t, reg, &va)) {
                if (!strcmp(reg, "EDX") && !nm)
                    nm = xbe_cstr(va, nmbuf, sizeof nmbuf);
                else if (!strcmp(reg, "ECX") && !gp)
                    gp = xbe_cstr(va, gpbuf, sizeof gpbuf);
                continue;
            }
            if (match_lea(t, &va)) {
                if (!have_off) { off = va; have_off = 1; }
                continue;
            }
            if (match_addesi(t, &va)) {
                if (!have_off) { off = va; have_off = 1; }
            }
        }
        if (nm && *nm && gp && *gp && have_off) {
            int dup = 0;
            for (k = 0; k < nrows; k++)
                if (rows[k].offset == off && !strcmp(rows[k].group, gp)
                    && !strcmp(rows[k].name, nm)) { dup = 1; break; }
            if (dup)
                continue;
            if (nrows == rcap) {
                rcap = rcap ? rcap * 2 : 128;
                rows = (cxd_row *)realloc(rows, (size_t)rcap * sizeof *rows);
                if (!rows) goto done;
            }
            snprintf(rows[nrows].group, sizeof rows[0].group, "%s", gp);
            snprintf(rows[nrows].name, sizeof rows[0].name, "%s", nm);
            rows[nrows].offset = off;
            nrows++;
        }
    }
    if (!nrows) {
        fprintf(stderr, "no registrations recovered\n");
        goto done;
    }

    /* stable sort by offset (Q26) */
    for (i = 1; i < nrows; i++) {
        cxd_row t = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].offset > t.offset) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = t;
    }
    /* the group list, in first-appearance order over the sorted rows, then
     * sorted by name for emission */
    gnames = (char **)malloc((size_t)nrows * sizeof *gnames);
    if (!gnames) goto done;
    for (i = 0; i < nrows; i++) {
        int seen = 0;
        for (k = 0; k < ngroups; k++)
            if (!strcmp(gnames[k], rows[i].group)) { seen = 1; break; }
        if (!seen)
            gnames[ngroups++] = rows[i].group;
    }
    qsort(gnames, (size_t)ngroups, sizeof *gnames, cmp_group_name);

    for (i = 0; i < nrows; i++)
        for (k = 0; k < ndefs; k++)
            if (defs[k].off == rows[i].offset) { have++; break; }

    for (k = 0; k < ngroups; k++) {
        printf("\n%s\n", gnames[k]);
        for (i = 0; i < nrows; i++) {
            int d;
            if (strcmp(rows[i].group, gnames[k]))
                continue;
            for (d = 0; d < ndefs; d++)
                if (defs[d].off == rows[i].offset)
                    break;
            if (d < ndefs) {
                float fv;
                memcpy(&fv, &defs[d].bits, 4);
                printf("    +0x%03X  %-30s %.6g\n", rows[i].offset,
                       rows[i].name, (double)fv);
            } else {
                printf("    +0x%03X  %-30s (no default)\n", rows[i].offset,
                       rows[i].name);
            }
        }
    }
    printf("\n%d parameters across %d groups; struct span +0x%03X..+0x%03X"
           " (stride 0x1D0)\n", nrows, ngroups, rows[0].offset,
           rows[nrows - 1].offset);
    printf("%d/%d have a compiled-in default from FUN_00132950\n", have, nrows);

    if (cxd_mkdir_p(gen) != 0) {
        fprintf(stderr, "[cx_cars_physparams] cannot create %s\n", gen);
        goto done;
    }
    f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "[cx_cars_physparams] cannot write %s\n", out);
        goto done;
    }
    fputs("// Generated by tools/extract_physics_params.py -- do not edit by"
          " hand.\n"
          "// Burnout 3 vehicle physics parameter table, recovered from the\n"
          "// registration function at 0x00132D10. Offsets are byte offsets"
          " into the\n"
          "// game's per-vehicle physics struct; all fields are f32.\n"
          "#ifndef BURNOUT3_PHYSICS_PARAMS_H\n"
          "#define BURNOUT3_PHYSICS_PARAMS_H\n"
          "\n"
          "typedef struct {\n"
          "    const char* group;\n"
          "    const char* name;\n"
          "    unsigned    offset;   // byte offset into the 0x1D0-byte"
          " physics struct\n"
          "    float       def;      // compiled-in default (FUN_00132950)\n"
          "    int         has_def;\n"
          "} PhysicsParam;\n"
          "\n", f);
    fprintf(f, "#define PHYSICS_PARAM_COUNT %d\n\n", nrows);
    fputs("static const PhysicsParam PHYSICS_PARAMS[PHYSICS_PARAM_COUNT] = {\n",
          f);
    for (k = 0; k < ngroups; k++) {
        fprintf(f, "    /* %s */\n", gnames[k]);
        for (i = 0; i < nrows; i++) {
            char lit[64];
            int d;
            if (strcmp(rows[i].group, gnames[k]))
                continue;
            for (d = 0; d < ndefs; d++)
                if (defs[d].off == rows[i].offset)
                    break;
            if (d < ndefs) {
                float fv;
                memcpy(&fv, &defs[d].bits, 4);
                cfloat((double)fv, lit, sizeof lit);
            } else {
                cfloat(0.0, lit, sizeof lit);
            }
            fprintf(f, "    { \"%s\", \"%s\", 0x%03Xu, %s, %d },\n",
                    gnames[k], rows[i].name, rows[i].offset, lit,
                    d < ndefs ? 1 : 0);
        }
    }
    fputs("};\n\n#define B3_PHYSICS_STRUCT_SIZE 0x1D0u\n\n", f);
    fputs("// Field offsets, for direct access into the physics struct.\n", f);
    for (i = 0; i < nrows; i++) {
        char id[256];
        ident_upper(rows[i].group, rows[i].name, id, sizeof id);
        fprintf(f, "#define B3_PHYS_%-34s 0x%03Xu\n", id, rows[i].offset);
    }
    fputs("\n#endif // BURNOUT3_PHYSICS_PARAMS_H\n", f);
    fclose(f);
    printf("wrote %s\n", out);
    rc = 0;

done:
    for (i = 0; i < nins; i++)
        free(ins[i]);
    free(ins);
    free(gnames);
    free(rows);
    free(defs);
    free(dis);
    free(dec);
    cxd_blob_free(&g_xbe);
    return rc;
}
