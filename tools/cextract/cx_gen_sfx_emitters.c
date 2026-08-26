/* cx_gen_sfx_emitters.c -- the C port of tools/gen_sfx_emitters.py.
 *
 * Emit the retail emitter behind each B3SfxEvent.
 *
 * Built by matching two sources that already exist, so neither can drift
 * alone:
 *
 *   src/burnout3_sfx.h   the B3SfxEvent enum, each entry commented with the
 *                        emitter it reproduces (`FUN_0014F3E0  IMPACTNUDG ...`)
 *   tools/emulate_sfx.py EMITTERS -- the same addresses with the CALLING
 *                        CONVENTION each one needs ('ecx', 'eax_m8', ...),
 *                        which is the part the port's comment does not carry
 *
 * An event whose address is not in EMITTERS gets addr 0 and is simply not
 * switchable: its emitter has no captured convention yet, so retail cannot be
 * asked what it would play, and b3_sfx_event stays on the port for that one.
 *
 * The two python regexes are hand-rolled below.  Both are anchored the way
 * re.match is (at the start of the line / at the "(0x" the pattern opens
 * with), so the same lines match and the same ones do not -- an enum entry
 * whose comment names an ADDRESS rather than a FUN_ symbol (B3_SFX_CAR_ALARM's
 * `0x0015204D`) falls to the second pattern and lands as a 0 row, exactly as
 * it does in python.
 */
#include "cx_common_g.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CXG_SFX_MAX_EMIT 128
#define CXG_SFX_MAX_EV   256

static const char *const CXG_SFX_KINDS[] = {
    "ecx", "eax_x0", "eax_m8", "st_obj_m8", "st4",
};
#define CXG_SFX_NKINDS \
    ((int)(sizeof(CXG_SFX_KINDS) / sizeof(CXG_SFX_KINDS[0])))

typedef struct {
    uint32_t addr;
    char     kind[32];
    char     wave[64];
} cxg_sfx_conv;

typedef struct {
    char     name[96];
    uint32_t addr;
} cxg_sfx_event;

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

static int cxg_isw(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* A quoted token: `'` <body> `'`.  `want_upper` restricts the body to [A-Z]+
 * (the wave name), otherwise it is \w+ (the convention) or [^']* (the
 * description, which the pattern does not capture). */
static const char *cxg_quoted(const char *p, char *out, size_t cap,
                              int mode /* 0=[A-Z]+ 1=[^']* 2=\w+ */)
{
    const char *b;
    size_t      n;

    if (*p != '\'')
        return NULL;
    p++;
    b = p;
    while (*p && *p != '\'') {
        if (mode == 0 && !(*p >= 'A' && *p <= 'Z'))
            return NULL;
        if (mode == 2 && !cxg_isw(*p))
            return NULL;
        p++;
    }
    if (*p != '\'' || p == b)      /* the two capturing groups are 1+ */
        if (mode != 1 || *p != '\'')
            return NULL;
    n = (size_t)(p - b);
    if (out) {
        if (n >= cap)
            n = cap - 1;
        memcpy(out, b, n);
        out[n] = '\0';
    }
    return p + 1;
}

/* tools/emulate_sfx.py's EMITTERS table:
 *   \(0x([0-9A-Fa-f]{8}),\s*'([A-Z]+)',\s*'[^']*',\s*'(\w+)'          */
static int cxg_sfx_load_conv(const char *repo, cxg_sfx_conv *out, int cap)
{
    char        path[4096];
    char       *emu;
    const char *start, *end, *p;
    int         n = 0;

    cxg_join(path, sizeof(path), repo, "tools/emulate_sfx.py");
    emu = cxg_read_text(path, NULL);
    if (!emu)
        return -1;
    start = strstr(emu, "EMITTERS = [");
    if (!start) {
        free(emu);
        return 0;                  /* python: emu[i:...] with i == -1 */
    }
    end = strstr(start, "\n]");
    if (!end)
        end = start + strlen(start);

    for (p = start; (p = strstr(p, "(0x")) != NULL && p < end; ) {
        const char *q = p + 3;
        uint32_t    addr = 0;
        char        wave[64], kind[32];
        int         i, d;

        for (i = 0; i < 8; i++) {
            d = cxg_hexval(q[i]);
            if (d < 0)
                break;
            addr = addr * 16u + (uint32_t)d;
        }
        if (i != 8 || q[8] != ',') {
            p += 3;
            continue;
        }
        q = cxg_ws(q + 9);
        q = cxg_quoted(q, wave, sizeof(wave), 0);
        if (!q || *q != ',') {
            p += 3;
            continue;
        }
        q = cxg_ws(q + 1);
        q = cxg_quoted(q, NULL, 0, 1);         /* the description */
        if (!q || *q != ',') {
            p += 3;
            continue;
        }
        q = cxg_ws(q + 1);
        q = cxg_quoted(q, kind, sizeof(kind), 2);
        if (!q) {
            p += 3;
            continue;
        }
        if (n < cap) {
            int k;

            /* python's dict: a repeated address replaces the earlier row. */
            for (k = 0; k < n; k++)
                if (out[k].addr == addr)
                    break;
            if (k == n)
                n++;
            out[k].addr = addr;
            snprintf(out[k].kind, sizeof(out[k].kind), "%s", kind);
            snprintf(out[k].wave, sizeof(out[k].wave), "%s", wave);
        }
        p += 3;
    }
    free(emu);
    return n;
}

/* One enum line.  Returns 1 if it named an event; *addr is the FUN_ address
 * when the comment carries one and 0 otherwise (the m2 fallback). */
static int cxg_sfx_line(const char *line, size_t len, char *name, size_t ncap,
                        uint32_t *addr)
{
    char        buf[1024];
    const char *p, *b;
    size_t      n;

    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';

    p = cxg_ws(buf);
    if (strncmp(p, "B3_SFX_", 7) != 0)
        return 0;
    b = p;
    while (cxg_isw(*p))
        p++;
    n = (size_t)(p - b);
    if (n >= ncap)
        n = ncap - 1;
    memcpy(name, b, n);
    name[n] = '\0';

    p = cxg_ws(p);
    if (*p == '=') {                            /* (?:=\s*\d+)? */
        const char *q = cxg_ws(p + 1);

        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9')
                q++;
            p = q;
        }
    }
    p = cxg_ws(p);
    if (*p != ',')
        return 0;
    *addr = 0;

    /* the first pattern's tail: a comma, whitespace, a comment opener,
     * whitespace, then FUN_ and exactly 8 hex digits. */
    p = cxg_ws(p + 1);
    if (p[0] == '/' && p[1] == '*') {
        const char *q = cxg_ws(p + 2);

        if (strncmp(q, "FUN_", 4) == 0) {
            uint32_t v = 0;
            int      i, d;

            q += 4;
            for (i = 0; i < 8; i++) {
                d = cxg_hexval(q[i]);
                if (d < 0)
                    break;
                v = v * 16u + (uint32_t)d;
            }
            if (i == 8)
                *addr = v;
        }
    }
    return 1;
}

static int cxg_sfx_build(cxg_str *L, const char *repo, int *n_ev, int *n_have)
{
    char           path[4096];
    char          *src = NULL;
    cxg_sfx_conv  *conv = NULL;
    cxg_sfx_event *ev = NULL;
    const char    *i0, *j0, *p;
    int            nconv, nev = 0, have = 0, i, rc = 1;

    conv = (cxg_sfx_conv *)malloc(sizeof(*conv) * CXG_SFX_MAX_EMIT);
    ev   = (cxg_sfx_event *)malloc(sizeof(*ev) * CXG_SFX_MAX_EV);
    if (!conv || !ev)
        goto done;
    nconv = cxg_sfx_load_conv(repo, conv, CXG_SFX_MAX_EMIT);
    if (nconv < 0)
        goto done;

    cxg_join(path, sizeof(path), repo, "src/burnout3_sfx.h");
    src = cxg_read_text(path, NULL);
    if (!src)
        goto done;
    i0 = strstr(src, "B3_SFX_IMPACT_NUDGE");
    j0 = strstr(src, "} B3SfxEvent;");
    if (!i0 || !j0 || j0 < i0) {
        fprintf(stderr, "cxg: %s has no B3SfxEvent enum body\n", path);
        goto done;
    }
    for (p = i0; p < j0 && nev < CXG_SFX_MAX_EV; ) {
        const char *nl = memchr(p, '\n', (size_t)(j0 - p));
        size_t      len = nl ? (size_t)(nl - p) : (size_t)(j0 - p);

        if (cxg_sfx_line(p, len, ev[nev].name, sizeof(ev[nev].name),
                         &ev[nev].addr))
            nev++;
        if (!nl)
            break;
        p = nl + 1;
    }
    *n_ev = nev;

    cxg_sline(L, "// GENERATED by tools/gen_sfx_emitters.py -- do not edit by hand.");
    cxg_sline(L, "//");
    cxg_sline(L, "// The retail emitter behind each B3SfxEvent, with the calling");
    cxg_sline(L, "// convention it needs. Address 0 = not switchable: no captured");
    cxg_sline(L, "// convention for that emitter, so b3_sfx_event stays on the port.");
    cxg_sline(L, "#ifndef BURNOUT3_SFX_EMITTERS_H");
    cxg_sline(L, "#define BURNOUT3_SFX_EMITTERS_H");
    cxg_nl(L);
    cxg_sline(L, "#include \"burnout3_sfx.h\"");
    cxg_nl(L);
    cxg_sline(L, "typedef struct { unsigned addr; int kind; const char* wave; } B3SfxEmitter;");
    cxg_nl(L);
    cxg_sline(L, "// kind ids match tools/emulate_sfx.py's `fire(kind=...)` strings:");
    for (i = 0; i < CXG_SFX_NKINDS; i++)
        cxg_sline(L, "//   %d = %s", i, CXG_SFX_KINDS[i]);
    cxg_sline(L, "static const B3SfxEmitter B3_SFX_EMITTERS[] = {");
    for (i = 0; i < nev; i++) {
        int k, hit = -1;

        for (k = 0; k < nconv; k++)
            if (conv[k].addr == ev[i].addr) {
                hit = k;
                break;
            }
        if (hit < 0) {
            cxg_sline(L, "    { 0x0, -1, 0 },  // %s", ev[i].name);
            continue;
        }
        for (k = 0; k < CXG_SFX_NKINDS; k++)
            if (!strcmp(CXG_SFX_KINDS[k], conv[hit].kind))
                break;
        if (k == CXG_SFX_NKINDS) {   /* python: KINDS.index() raises */
            fprintf(stderr, "cxg: emitter 0x%08X has unknown convention '%s'\n",
                    ev[i].addr, conv[hit].kind);
            goto done;
        }
        have++;
        cxg_sline(L, "    { 0x%08X, %d, \"%s\" },  // %s",
                  ev[i].addr, k, conv[hit].wave, ev[i].name);
    }
    cxg_sline(L, "};");
    cxg_sline(L, "#define B3_SFX_EMITTER_COUNT "
                 "(sizeof(B3_SFX_EMITTERS)/sizeof(B3_SFX_EMITTERS[0]))");
    cxg_nl(L);
    cxg_sputs(L, "#endif // BURNOUT3_SFX_EMITTERS_H\n");
    *n_have = have;
    rc = 0;

done:
    free(conv);
    free(ev);
    free(src);
    return rc;
}

int cx_extract_gen_sfx_emitters(const char *game_dir, const char *out_root)
{
    const char *repo = cxg_repo_root();
    cxg_str     L = { 0 };
    char        out[4096], rel[4096];
    int         nev = 0, have = 0, rc;

    (void)game_dir;
    cxg_gen_path(out, sizeof(out), out_root, "burnout3_sfx_emitters.h");
    rc = cxg_sfx_build(&L, repo, &nev, &have);
    if (rc == 0)
        rc = cxg_str_write(&L, out);
    cxg_str_free(&L);
    if (rc == 0) {
        cxg_relpath(rel, sizeof(rel), out, repo);
        printf("  %s: %d event(s), %d with a callable emitter\n",
               rel, nev, have);
    }
    return rc;
}
