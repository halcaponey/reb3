#!/usr/bin/env python3
"""Car texture-slot / UV integrity suite.

THE DEFECT THIS GUARDS (user report: "the textures of the car when crashing
get messed up (UV?)").

A .bgv draw record carries its own texture slot byte at rec+0x1A, and that
byte indexes a FIVE-entry pointer table on the car draw context -- it is not
"the car's texture".  [C]

    00031abd  MOVZX EAX, byte ptr [EBX + 0x1a]             ; record tex slot
    00031ac5  MOV   EAX, dword ptr [ESI + EAX*0x4 + 0x334] ; ctx table
    00031ad3  MOV   [0x0075db70], EAX                      ; bound texture

Entry 0 is the model's own "compact1" paint page (FUN_000303D0 @0x00030546);
entries 1..4 are four SHARED Data/Global.txd pages, written verbatim at

    000317c0  MOV EAX,[0x004d61b4] / MOV [EDI+0x338],EAX   ; 1 VehicleUnderside
    000317cb  MOV ECX,[0x004d61a8] / MOV [EDI+0x33c],ECX   ; 2 UnbrokenGlass
    000317d7  MOV EDX,[0x004d61ac] / MOV [EDI+0x340],EDX   ; 3 CrackedGlass
    000317e3  MOV EAX,[0x004d61b0] / MOV [EDI+0x344],EAX   ; 4 SmashedGlass

and FUN_000300A0 moves a glass record BETWEEN 2/3/4 as the damage tier rises
(2 + shipped tint @0x000300B6/CB, 3 + 0.5 @0x00030107/16, 4 + 0.6
@0x000300E0/EF) -- so the page is part of the tier stamp, not a load-time
constant.

tools/extract_bgv.py preserves the slot as `usemtl b3tex<slot>` on every
emitted span.  The harness used to honour slot 1 only and flatten 2..4 onto
the car's livery.  That was invisible on an intact car -- <car>_intact.obj is
built from the (mask & 0x300) == 0 records, so it has no glass at all -- but
the PANEL meshes in build/cars/parts/<car>/panel*.obj are emitted UNFILTERED
and carry their own glass records (group "m100_t2": mask 0x100 = bit8 =
glass, slot 2).  Panels are drawn only while a car is wrecked.  So the livery
landed on the wreck's window geometry, sampled at UnbrokenGlass UVs, at
exactly the moment of the crash.

Two checks, and the data check runs with no GL at all:

  1. DATA.  Every `usemtl` in build/cars is a b3tex<slot> with slot in 0..4;
     the crash-only meshes (shell + panels) really do carry slot 2..4 spans
     (i.e. the defect's precondition still exists in the shipped data, so
     this suite is testing something); and the four global pages are present.

  2. RUNTIME (needs a build + a display; skipped with --data-only).  Runs the
     game briefly with B3_CARLIST_DUMP=1 and asserts, for every span the
     renderer bakes into a car display list:
       * slot 1..4 never resolves to the car's paint texture name;
       * every span carries texcoords (uv=1) -- a span drawn without them
         inherits the previous list's last texcoord, which is how the glass
         list used to sample one frozen texel of the body page;
       * the NO_GLASS panel list holds no slot>=2 span and the GLASS_ONLY
         list holds nothing else, i.e. the two passes really are disjoint.

Run: python3 tools/validate_car_texture_slots.py [--data-only] [--seconds N]
"""

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GLOBAL_PAGES = {
    1: "build/frontend/VehicleUnderside.png",
    2: "build/frontend/UnbrokenGlass.png",
    3: "build/frontend/CrackedGlass.png",
    4: "build/frontend/SmashedGlass.png",
}

# car_list_from_obj_ex flags, burnout3_full.c
F_NO_GLASS = 1
F_GLASS_ONLY = 2
F_DEFER_BIND = 4

fails = 0


def check(name, ok, detail=""):
    global fails
    print("  %-62s %s%s" % (name, "ok" if ok else "FAIL",
                            "" if ok else "   " + detail))
    if not ok:
        fails += 1
    return 0 if ok else 1


def obj_slots(path):
    """[(slot, n_triangles)] for one OBJ, in file order."""
    spans, cur, n = [], None, 0
    with open(path) as fh:
        for line in fh:
            if line.startswith("f "):
                n += 1
            elif line.startswith("usemtl "):
                if cur is not None:
                    spans.append((cur, n - start))
                cur = line.split()[1]
                start = n
    if cur is not None:
        spans.append((cur, n - start))
    out = []
    for mat, tris in spans:
        m = re.fullmatch(r"b3tex(\d+)", mat)
        out.append((int(m.group(1)) if m else None, mat, tris))
    return out


def data_checks():
    print("\nDATA -- build/cars texture slots")
    cars = sorted(os.path.basename(p)[:-len("_shell.obj")]
                  for p in glob.glob(os.path.join(ROOT, "build/cars/*_shell.obj")))
    check("build/cars has extracted damage sets", len(cars) > 0,
          "run tools/extract_bgv.py")
    if not cars:
        return

    bad_mat, crash_glass, cars_with_panel_glass = [], 0, 0
    for car in cars:
        meshes = [os.path.join(ROOT, "build/cars/%s_shell.obj" % car)]
        meshes += sorted(glob.glob(os.path.join(
            ROOT, "build/cars/parts/%s/panel*.obj" % car)))
        panel_glass = 0
        for p in meshes:
            for slot, mat, tris in obj_slots(p):
                if slot is None or not (0 <= slot <= 4):
                    bad_mat.append((p, mat))
                elif slot >= 2:
                    crash_glass += tris
                    if "/parts/" in p:
                        panel_glass += tris
        if panel_glass:
            cars_with_panel_glass += 1

    check("every usemtl is b3tex0..b3tex4", not bad_mat,
          str(bad_mat[:3]))
    # The precondition of the bug: crash-only geometry that is NOT on the
    # car's own page.  If this ever goes to zero the extractor changed and
    # the runtime check below stops proving anything.
    check("crash-only meshes carry slot>=2 (glass) spans", crash_glass > 0,
          "no glass in shell/panels -- extractor changed?")
    check("most cars have panel-embedded glass",
          cars_with_panel_glass >= len(cars) // 2,
          "%d/%d" % (cars_with_panel_glass, len(cars)))

    missing = [p for p in GLOBAL_PAGES.values()
               if not os.path.exists(os.path.join(ROOT, p))]
    check("all four Global.txd shared pages extracted", not missing,
          "missing %s -- run tools/extract_txd.py" % missing)

    # An intact car must have NO glass span: that is why the defect only ever
    # showed at a crash, and it is what makes "wrecked" the trigger.
    intact_glass = []
    for car in cars:
        p = os.path.join(ROOT, "build/cars/%s_intact.obj" % car)
        if not os.path.exists(p):
            continue
        if any(s is not None and s >= 2 for s, _, _ in obj_slots(p)):
            intact_glass.append(car)
    check("intact bodies carry no glass span (crash-only defect)",
          not intact_glass, str(intact_glass[:3]))


DUMP_RE = re.compile(
    r"\[carlist\] (\S+) g=(\d+) slot=(-?\d+) tris=(\d+) tex=(\d+) "
    r"paint=(\d+) uv=(\d+) flags=(\d+)")


def runtime_checks(seconds):
    print("\nRUNTIME -- B3_CARLIST_DUMP over the real list builder")
    exe = os.path.join(ROOT, "burnout3")
    if not os.path.exists(exe):
        check("./burnout3 built", False, "run make")
        return
    env = dict(os.environ)
    env.update({
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_AUDIODRIVER": "dummy",
        "B3_FIXED_DT": "0.0166667",
        "B3_PACE_MAX_TICKS": "1",
        "B3_EXIT_AT": str(seconds),
        "B3_CARLIST_DUMP": "1",
    })
    try:
        out = subprocess.run([exe], cwd=ROOT, env=env, timeout=180,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT).stdout.decode(
                                 "utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")

    rows = [DUMP_RE.match(l) for l in out.splitlines()]
    rows = [r.groups() for r in rows if r]
    check("the list builder emitted a dump", len(rows) > 0,
          "no [carlist] lines -- is the B3_CARLIST_DUMP patch applied?")
    if not rows:
        return

    flat, no_uv, mixed_body, mixed_glass = [], [], [], []
    for path, g, slot, tris, tex, paint, uv, flags in rows:
        slot, tex, paint = int(slot), int(tex), int(paint)
        uv, flags = int(uv), int(flags)
        # A slot 1..4 span must never end up on the car's own paint texture.
        # tex==0 means "the caller binds" (DEFER_BIND) or "no page at all".
        if 1 <= slot <= 4 and tex != 0 and tex == paint:
            flat.append((path, g, slot))
        if not uv:
            no_uv.append((path, g))
        if (flags & F_NO_GLASS) and slot >= 2:
            mixed_body.append((path, g, slot))
        if (flags & F_GLASS_ONLY) and slot < 2:
            mixed_glass.append((path, g, slot))

    check("no shared-page span bound to the car's paint page", not flat,
          str(flat[:3]))
    check("every emitted span carries texcoords", not no_uv, str(no_uv[:3]))
    check("panel body list holds no glass record", not mixed_body,
          str(mixed_body[:3]))
    check("panel glass list holds only glass records", not mixed_glass,
          str(mixed_glass[:3]))

    glass_spans = [r for r in rows if int(r[2]) >= 2]
    check("glass spans exist in the dump (test is live)", len(glass_spans) > 0)
    deferred = [r for r in glass_spans if int(r[7]) & F_DEFER_BIND]
    check("glass spans defer their bind (tier retarget possible)",
          len(deferred) == len(glass_spans),
          "%d/%d" % (len(deferred), len(glass_spans)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-only", action="store_true")
    ap.add_argument("--seconds", type=int, default=6)
    a = ap.parse_args()
    data_checks()
    if not a.data_only:
        runtime_checks(a.seconds)
    print("\n%s (%d failures)" % ("PASS" if not fails else "FAIL", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
