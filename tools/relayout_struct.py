#!/usr/bin/env python3
"""Re-lay a port struct so its fields sit at RETAIL's offsets.

Reads the offset annotations already on each member, sorts by offset, emits the
struct packed with explicit padding, and adds a _Static_assert per field so a
drift fails the build instead of corrupting state at runtime. Members with no
recovered offset -- or whose annotation names a DIFFERENT object -- are moved
past the retail window, because retail code reads those bytes and harness state
must not squat on them.

    python3 tools/relayout_struct.py --header src/burnout3_crash.h \
        --struct B3CrashVehicle --span 0x1600 --owner veh

--owner names the object whose offsets belong in this struct ("veh" for a view
of the vehicle). Any other qualifier (racecar+, config+, .bgv+) is treated as
another object's offset and sent to the harness side.
"""
import argparse
import re
import sys

SIZES = {"float": 4, "int": 4, "unsigned": 4, "unsigned int": 4,
         "unsigned short": 2, "short": 2, "unsigned char": 1,
         "signed char": 1, "char": 1}
DECL = re.compile(
    r'^[ \t]*(?P<type>(?:const\s+)?(?:unsigned\s+|signed\s+)?[A-Za-z_]\w*)\s+'
    r'(?P<names>[A-Za-z_]\w*(?:\s*\[[^\]]*\])*(?:\s*,\s*[A-Za-z_]\w*(?:\s*\[[^\]]*\])*)*)\s*;'
    r'(?P<rest>.*)$')
VOFF = re.compile(r'\+0x([0-9A-Fa-f]{2,4})')
NOFF = re.compile(r'_?([0-9A-Fa-f]{3,4})$')



def close_comment(text):
    """Terminate a trailing comment that the original spread over several lines.

    We keep only the first line of a member's comment. If that line opened a
    block comment and did not close it, the emitted `/*` runs on and swallows
    the NEXT declaration -- which is how `rub_target` and `air_count` silently
    vanished from B3ScoreEvents while the tool reported success."""
    if not text:
        return text
    if "/*" in text and "*/" not in text.split("/*", 1)[1]:
        return text.rstrip() + " */"
    return text

def size_of(t, arr, extra, defines=None):
    base = extra.get(t) or SIZES.get(t.replace("const ", "").strip())
    if base is None:
        return None
    n = 1
    # Array bounds are often #defines, not literals. Sizing [B3_HULL_MAX_PLANES]
    # as if it were absent silently shrinks the field and shifts every offset
    # after it, so resolve them from the header.
    for d in re.findall(r'\[([^\]]+)\]', arr or ""):
        d = d.strip()
        if d.isdigit():
            n *= int(d)
        elif defines and d in defines:
            n *= defines[d]
        else:
            return None
    return base * n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True)
    ap.add_argument("--struct", required=True)
    ap.add_argument("--span", required=True)
    ap.add_argument("--owner", default="veh",
                    help="qualifier naming THIS struct's object (default veh)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="member that must stay harness-side despite carrying "
                         "an offset (e.g. an inlined copy of an object retail "
                         "keeps behind a pointer)")
    ap.add_argument("--size", action="append", default=[],
                    help="TYPE=BYTES for a type the tool cannot size")
    a = ap.parse_args()
    span = int(a.span, 0)
    extra = dict((s.split("=")[0], int(s.split("=")[1], 0)) for s in a.size)

    src = open(a.header).read()
    defines = {}
    for dm in re.finditer(r'^#define\s+(\w+)\s+(\d+)\b', src, re.M):
        defines[dm.group(1)] = int(dm.group(2))
    # Anchor on the CLOSING tag and walk back to the matching brace. A forward
    # regex from `typedef struct` happily starts at an earlier struct and
    # swallows everything up to our closing tag -- which silently merged two
    # different objects on the first run.
    close = re.search(r'\}\s*%s\s*;' % re.escape(a.struct), src)
    if not close:
        sys.exit("struct %s not found in %s" % (a.struct, a.header))
    depth, i = 0, close.start()
    while i >= 0:
        if src[i] == '}':
            depth += 1
        elif src[i] == '{':
            depth -= 1
            if depth == 0:
                break
        i -= 1
    if i < 0:
        sys.exit("unbalanced braces around %s" % a.struct)
    ts = src.rfind("typedef", 0, i)
    class M:
        pass
    m = M()
    m.start = lambda _ts=ts: _ts
    m.end = lambda _e=close.end(): _e
    body = src[i+1:close.start()]

    # An annotation belongs to THIS object when the word before the +0x is our
    # owner, or when there is no qualifying word at all.
    owner_ok = re.compile(r'(?:^|[\s\[(])%s\s*$' % re.escape(a.owner), re.I)
    # Everything EXCEPT this struct's own object counts as another object's
    # offset. The owner must be removed from the list, or a struct that is a
    # view of the racecar has its own `racecar+0x...` annotations discarded.
    others = [w for w in ("config", "owner", "racecar", "bgv", "score", "world",
                          "veh", "hull", "AI")
              if w.lower() != (a.owner or "").lower()]
    other = re.compile(r'(%s)\s*$' % "|".join(others), re.I)

    def offset_of(rest, name):
        for mm in VOFF.finditer(rest):
            before = rest[max(0, mm.start() - 14):mm.start()].strip().rstrip('.([')
            if other.search(before):
                continue
            return int(mm.group(1), 16)
        return None

    retail, harness, carried = [], [], []
    # Split on ';' so MULTIPLE DECLARATIONS ON ONE LINE are all seen. A
    # per-line regex silently drops all but the first, e.g.
    # `float air_total, air_best;   int air_count;` loses air_count.
    lines = []
    for raw in body.splitlines():
        st = raw.strip()
        # never split a comment: prose contains semicolons too, and this
        # tool's own banner says "packed with explicit padding; asserted below"
        if st.startswith(("//", "/*", "*")) or "*/" in st.split("/*")[0]:
            lines.append(raw)
            continue
        code, sep, rest = raw.partition("/*")
        parts = [x for x in code.split(";") if x.strip()]
        if len(parts) > 1:
            for k, part in enumerate(parts):
                lines.append(part + ";" + ((sep + rest) if k == len(parts) - 1 else ""))
        else:
            lines.append(raw)
    for line in lines:
        st = line.strip()
        d = DECL.match(line)
        if not d:
            if st and not st.startswith(("//", "/*", "*")):
                carried.append(line)
            continue
        t, rest = d.group("type"), d.group("rest")
        # Drop this tool's OWN previous padding. Re-running on an already
        # relaid struct otherwise carries the old _pad members through and
        # then emits fresh ones, giving duplicate member names.
        if re.match(r'^\s*_pad', d.group("names")):
            continue
        names = [x.strip() for x in d.group("names").split(",")]
        multi = len(names) > 1
        off_line = offset_of(rest, None)
        ok, ent = True, []
        for nm in names:
            am = re.match(r'(\w+)((?:\s*\[[^\]]*\])*)', nm)
            name, arr = am.group(1), (am.group(2) or "").replace(" ", "")
            sz = size_of(t, arr, extra, defines)
            if sz is None:
                ok = False
                break
            off = None if (multi or name in a.exclude) else off_line
            ent.append((off, t, name, arr, sz, rest.strip() if not multi else ""))
        if not ok:
            carried.append(line)
            continue
        for e in ent:
            (retail if e[0] is not None else harness).append(e)

    retail.sort(key=lambda r: r[0])
    kept, bumped, cur = [], [], 0
    for e in retail:
        if e[0] < cur:
            bumped.append((e[0], e[2], cur))
            harness.append(e)
            continue
        kept.append(e)
        cur = e[0] + e[4]

    L = ["typedef struct __attribute__((packed)) %s {" % a.struct,
         "    // ---- RETAIL WINDOW 0x0000..0x%04X: fields at the offsets the" % span,
         "    // game uses. Packed with explicit padding; asserted below.",
         ]
    cur, npad = 0, 0
    for off, t, name, arr, sz, rest in kept:
        if off > cur:
            L.append("    unsigned char _pad%02X[0x%X];" % (npad, off - cur))
            npad += 1
        L.append("    %-20s %s%s;  %s" % (t, name, arr, close_comment(rest) or "// +0x%04X" % off))
        cur = off + sz
    if cur < span:
        L.append("    unsigned char _pad%02X[0x%X];" % (npad, span - cur))
    if harness or carried:
        L += ["", "    // ---- HARNESS SIDE, past the retail window: no recovered",
              "    // offset in THIS object, so it must not squat on retail's bytes."]
    for e in harness:
        L.append("    %-20s %s%s;  %s" % (e[1], e[2], e[3], close_comment(e[5])))
    L += carried
    new = "\n".join(L) + "\n} %s;" % a.struct

    A = ["", "#define %s_RETAIL_SPAN 0x%04Xu" % (a.struct.upper(), span)]
    for off, t, name, arr, sz, rest in kept:
        A.append('_Static_assert(offsetof(%s, %s) == 0x%04X, "%s off retail");'
                 % (a.struct, name, off, name))
    tail = src[m.end():]
    # Strip THIS tool's previous assertions for this struct before adding the
    # new set. Appending without removing leaves stale asserts naming fields
    # that a later run renamed or moved, and the build fails on a phantom.
    tail = re.sub(r'\n#define %s_RETAIL_SPAN[^\n]*' % a.struct.upper(), '', tail)
    tail = re.sub(r'\n_Static_assert\(offsetof\(%s,[^\n]*\n[^\n]*"\);' % re.escape(a.struct),
                  '', tail)
    tail = re.sub(r'\n_Static_assert\(offsetof\(%s,[^\n]*' % re.escape(a.struct), '', tail)
    tail = re.sub(r'\n_Static_assert\(sizeof\(%s\)[^\n]*' % re.escape(a.struct), '', tail)
    out = src[:m.start()] + new + "\n" + "\n".join(A) + tail
    if "#include <stddef.h>" not in out:      # offsetof, for the assertions
        k = out.find("\n", out.find("#define"))
        out = out[:k+1] + "\n#include <stddef.h>\n" + out[k+1:]
    open(a.header, "w").write(out)
    print("%s: %d field(s) at retail offsets, %d harness-side, %d carried, %d bumped"
          % (a.struct, len(kept), len(harness), len(carried), len(bumped)))
    for o, n, c in bumped:
        print("   bumped 0x%04X %-20s (prev ends 0x%04X)" % (o, n, c))


if __name__ == "__main__":
    main()
