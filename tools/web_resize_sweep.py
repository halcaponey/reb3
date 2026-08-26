#!/usr/bin/env python3
"""web_resize_sweep.py -- prove the web port survives a change of resolution.

THE DEFECT THIS EXISTS FOR.  A user held 60.0 fps for 37 s of racing and then
hit this, mid-race, with nothing resized and no window touched:

    [Burnout3] web: render resolution 1921x1080 (viewport changed)
    [afx] unavailable: target scene 1921x1080 incomplete (status 0x8CD9)
    [afx] unavailable: target scene 1921x1080 incomplete (status 0x8CD9)
    ... every frame, for the rest of the race, at ~20 fps

Three separate faults, and this tool has a mode for each of the two that can
be driven from outside the engine:

  --mode sweep   Drive the effects chain through a scripted list of render
                 resolutions (B3_WEB_RESIZE_SWEEP, web/b3_web.c) and require a
                 COMPLETE rebuild at every one -- including odd widths, which
                 the viewport path can no longer produce but a browser once
                 did.  Gates on an `[afx] chain ready WxH` line per step and on
                 ZERO incomplete-framebuffer statuses anywhere in the run.

  --mode wobble  Leave the viewport follow ON and nudge the page the way a
                 real browser does -- SUB-PIXEL CSS sizes and fractional
                 devicePixelRatio, via CDP.  Gates on the engine NOT changing
                 its render resolution: a layout wobble smaller than the dead
                 band must rebuild nothing at all.

Headless Chromium only, never a visible browser, and always --mute-audio: the
audio bridge is live and headless Chrome routes an AudioContext to the real
output device.

    tools/web_resize_sweep.py --iso <path> [--mode sweep|wobble|both]
"""
import argparse
import asyncio
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

import websockets

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_iso():
    """CLI > build/iso_path.txt > $B3_ISO -- the engine's own ladder.

    See the long note over the same function in tools/web_smoke.py for why
    this exists: a non-XISO image picked up by accident fails several minutes
    into a boot, with every browser-side gate healthy until it does."""
    p = os.path.join(REPO, "build", "iso_path.txt")
    if os.path.exists(p):
        try:
            v = open(p).read().strip()
            if v and os.path.exists(v):
                return v
        except OSError:
            pass
    v = os.environ.get("B3_ISO", "")
    return v if v and os.path.exists(v) else None

# The brief's sweep, verbatim, plus the return leg: a 1-px-off width, a
# smaller 16:9, an odd-by-odd, and back to where it started.
DEFAULT_SWEEP = "1920x1080,1921x1080,1080x720,637x479,1920x1080"

FATAL_MARKERS = ("FATAL", "Aborted(", "abort(", "RuntimeError", "TypeError",
                 "sent an error", "uncaught exception", "was not exported")

# `[afx] chain ready 1921x1080 (depth 24-bit, msaa 4, 16 taps)` -- the first
# build and every rebuild print the same opening, which is the point.
READY_RE = re.compile(r"\[afx\] chain ready (\d+)x(\d+)")
STEP_RE = re.compile(r"web: resize sweep step (\d+) -> (\d+)x(\d+)")
RES_RE = re.compile(r"web: render resolution (\d+)x(\d+) \(([^)]*)\)")
# Any framebuffer that came back not-complete, whatever the status.
INCOMPLETE_RE = re.compile(r"incomplete \(status 0x([0-9A-Fa-f]{4})\)")

# What the two statuses in play actually are, because the user's report named
# the wrong one and it sent the first diagnosis at the wrong attachment.
STATUS_NAMES = {
    "8CD5": "COMPLETE",
    "8CD6": "INCOMPLETE_ATTACHMENT",
    "8CD7": "INCOMPLETE_MISSING_ATTACHMENT",
    "8CD9": "INCOMPLETE_DIMENSIONS",      # GLES2/WebGL1: attachments must match
    "8CDD": "UNSUPPORTED",
    "8D56": "INCOMPLETE_MULTISAMPLE",     # NOT 0x8CD9
}

GL_FLAGS = {
    "swiftshader": ["--use-gl=angle", "--use-angle=swiftshader",
                    "--enable-unsafe-swiftshader"],
    "hw": ["--use-gl=angle", "--use-angle=vulkan",
           "--enable-features=Vulkan", "--ignore-gpu-blocklist"],
}


def find_chrome():
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("no chromium/chrome on PATH")


# Nudge the canvas' CSS box by a FRACTION of a pixel, and its dpr with it.
# Both are things a real page does to itself: getBoundingClientRect() reports
# fractional used sizes, and devicePixelRatio is fractional under browser zoom
# and during a fullscreen transition.  Neither is a resize.
WOBBLE_CSS = """
(function (w, h) {
  var c = document.getElementById('canvas');
  if (!c) return 'no canvas';
  c.style.width = w + 'px';
  c.style.height = h + 'px';
  var r = c.getBoundingClientRect();
  return r.width.toFixed(4) + 'x' + r.height.toFixed(4) +
         ' dpr=' + window.devicePixelRatio;
})(%s, %s)
"""


async def run(args):
    lines = []
    fatals = []
    wobble_base = 0      # index into `lines` from which a wobble gate reads

    serve = subprocess.Popen(
        [sys.executable, os.path.join(REPO, "tools", "webserve.py"),
         "--port", str(args.port), "--root", REPO],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "B3_WEBSERVE_QUIET": "1"})
    profile = tempfile.mkdtemp(prefix="b3-resize-")
    chrome = subprocess.Popen([
        find_chrome(),
        "--headless=new",
        # The audio bridge is live and headless Chrome plays to the REAL
        # device.  Never run one of these without it.
        "--mute-audio",
        "--remote-debugging-port=%d" % args.cdp,
        "--user-data-dir=" + profile,
        "--no-first-run", "--no-default-browser-check",
        "--disable-gpu-sandbox", "--no-sandbox",
        "--autoplay-policy=no-user-gesture-required",
    ] + GL_FLAGS[args.gl] + [
        "--enable-features=SharedArrayBuffer",
        "--window-size=1400,900",
        "about:blank",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    wobble_probe = []
    try:
        ws_url = None
        for _ in range(100):
            try:
                with urllib.request.urlopen(
                        "http://127.0.0.1:%d/json/version" % args.cdp) as r:
                    ws_url = json.load(r)["webSocketDebuggerUrl"]
                break
            except Exception:
                time.sleep(0.2)
        if not ws_url:
            sys.exit("chrome devtools never came up")

        async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            nid = [0]

            def handle_event(msg):
                m = msg.get("method")
                if m == "Runtime.consoleAPICalled":
                    text = " ".join(str(a.get("value", "")) for a in
                                    msg["params"].get("args", []))
                    lines.append(text)
                    if any(k in text for k in FATAL_MARKERS):
                        fatals.append(text)
                elif m == "Log.entryAdded":
                    # WebGL errors are Log entries, not console.log -- this is
                    # where a rejected framebuffer op would surface.
                    e = msg["params"]["entry"]
                    lines.append("%s|%s" % (e.get("level", "?"),
                                            e.get("text", "")))
                elif m == "Runtime.exceptionThrown":
                    d = msg["params"]["exceptionDetails"]
                    text = "EXCEPTION|" + json.dumps(d.get("text", ""))
                    lines.append(text)
                    fatals.append(text)

            async def cmd(method, params=None, sess=None):
                nid[0] += 1
                msg = {"id": nid[0], "method": method, "params": params or {}}
                if sess:
                    msg["sessionId"] = sess
                await ws.send(json.dumps(msg))
                while True:
                    resp = json.loads(await ws.recv())
                    handle_event(resp)
                    if resp.get("id") == nid[0]:
                        return resp.get("result", {})

            async def pump(seconds):
                end = time.time() + seconds
                while time.time() < end:
                    try:
                        handle_event(json.loads(
                            await asyncio.wait_for(ws.recv(), timeout=0.5)))
                    except asyncio.TimeoutError:
                        pass

            t = await cmd("Target.createTarget", {"url": "about:blank"})
            s = await cmd("Target.attachToTarget",
                          {"targetId": t["targetId"], "flatten": True})
            sess = s["sessionId"]
            await cmd("Runtime.enable", {}, sess)
            await cmd("Log.enable", {}, sess)
            await cmd("Page.enable", {}, sess)
            await cmd("DOM.enable", {}, sess)

            env = {"B3_AUTODRIVE": "1", "B3_EXIT_AT": str(args.seconds),
                   "B3_NO_VSYNC": "1",
                   # MSAA ON, unlike web_smoke.py: the multisampled pair is
                   # part of what a rebuild has to get right, so a sweep that
                   # ran without it would prove half the path.
                   "B3_MSAA": str(args.msaa),
                   "B3_AFX_VERBOSE": "1"}
            # WHICH WebGL THE RUN GETS.  Worth being able to force, because the
            # two versions disagree about the very rule this tool tests: a
            # GLES2/WebGL 1 framebuffer requires every attachment to share
            # dimensions (and answers INCOMPLETE_DIMENSIONS, 0x8CD9, when they
            # do not), while WebGL 2 dropped the rule.  The user's failing
            # session reported 0x8CD9, so WebGL 1 semantics are what reproduce
            # it and what the fix has to satisfy.
            if args.webgl:
                env["B3_WEBGL"] = str(args.webgl)
            # --env K=V,K=V -- anything else the run needs.  B3_WEB_VPPROF=1
            # and B3_WEB_VP_POLL=1 are the pair that measures what the viewport
            # measurement costs the frame loop, before and after, out of one
            # binary (web/README.md, "the measurement must not stall the frame
            # loop either").
            for kv in args.env:
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    env[k] = v
            if args.mode == "sweep":
                env["B3_WEB_RESIZE_SWEEP"] = args.sweep
                env["B3_WEB_RESIZE_SWEEP_EVERY"] = str(args.every)
            if args.track:
                env["B3_TRACK"] = args.track
            await cmd("Page.addScriptToEvaluateOnNewDocument",
                      {"source": "window.B3_ENV=%s;window.B3_CACHE_MODE=%s;"
                                 % (json.dumps(env), json.dumps(args.cache))},
                      sess)

            await cmd("Page.navigate",
                      {"url": "http://127.0.0.1:%d/web/smoke.html" % args.port},
                      sess)
            await asyncio.sleep(2.0)

            doc = await cmd("DOM.getDocument", {}, sess)
            node = await cmd("DOM.querySelector",
                             {"nodeId": doc["root"]["nodeId"],
                              "selector": "#iso"}, sess)
            await cmd("DOM.setFileInputFiles",
                      {"files": [os.path.abspath(args.iso)],
                       "nodeId": node["nodeId"]}, sess)

            # ------------------------------------------------ wait for a race
            deadline = time.time() + args.timeout
            while time.time() < deadline:
                if any(READY_RE.search(ln) for ln in lines) and \
                        any("FPS:" in ln for ln in lines):
                    break
                try:
                    handle_event(json.loads(
                        await asyncio.wait_for(ws.recv(), timeout=1.0)))
                except asyncio.TimeoutError:
                    pass
            wobble_base = len(lines)

            if args.mode == "wobble":
                # Let the follow settle on a size first, then wobble under it.
                await pump(3.0)
                wobble_base = len(lines)
                for (cw, ch, dpr) in args.wobbles:
                    if dpr is not None:
                        await cmd("Emulation.setDeviceMetricsOverride",
                                  {"width": 1400, "height": 900,
                                   "deviceScaleFactor": dpr, "mobile": False},
                                  sess)
                    r = await cmd("Runtime.evaluate",
                                  {"expression": WOBBLE_CSS % (cw, ch),
                                   "returnByValue": True}, sess)
                    box = r.get("result", {}).get("value")
                    wobble_probe.append("css %sx%s dpr %s -> box %s"
                                        % (cw, ch, dpr, box))
                    # Several follow periods (30 presents each) per nudge, so
                    # the engine has really had the chance to act on it.
                    await pump(args.settle)
                # ...and a tail, so B3_WEB_VPPROF has polls enough to report.
                await pump(args.tail)
                lines.append("sweep: wobble baseline at line %d" % wobble_base)
            else:
                # The engine drives itself; just watch until it finishes the
                # list or the clock runs out.
                while time.time() < deadline:
                    if any("resize sweep complete" in ln for ln in lines):
                        break
                    try:
                        handle_event(json.loads(
                            await asyncio.wait_for(ws.recv(), timeout=1.0)))
                    except asyncio.TimeoutError:
                        pass
    finally:
        for p in (chrome, serve):
            try:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=5)
            except Exception:
                p.kill()
        shutil.rmtree(profile, ignore_errors=True)

    with open(args.log, "w") as f:
        f.write("\n".join(lines) + "\n")

    # ----------------------------------------------------------------- gates
    blob = "\n".join(lines)
    incompletes = INCOMPLETE_RE.findall(blob)
    unavailable = [ln for ln in lines if "[afx] unavailable" in ln]
    ready = [(int(a), int(b)) for ln in lines
             for (a, b) in READY_RE.findall(ln)]
    gates = []

    print("\n== the context, and what the engine said about its resolution ==")
    for ln in lines:
        if ("resize sweep" in ln or "render resolution" in ln
                or "[afx] chain ready" in ln or "[afx] unavailable" in ln
                or "MSAA" in ln or "retired" in ln or "direct WebGL" in ln
                or "web: gpu" in ln or "[afx] msaa" in ln
                or "[vpprof]" in ln or "ResizeObserver" in ln):
            print("  " + ln[:200])

    if wobble_probe:
        print("\n== what the page's box actually measured ==")
        for w_ in wobble_probe:
            print("  " + w_)

    if args.mode == "sweep":
        want = [tuple(int(x) for x in s.split("x"))
                for s in args.sweep.split(",")]
        steps = [(int(a), int(b)) for ln in lines
                 for (_, a, b) in STEP_RE.findall(ln)]
        gates.append(("every step was driven", steps == want))
        # Step 1 repeats the first build's size, so the chain-ready for it may
        # be the ORIGINAL build rather than a rebuild.  What must hold is that
        # every size in the list has a chain-ready of its own.
        missing = [s for s in want if s not in ready]
        gates.append(("chain ready at every size", not missing))
        gates.append(("sweep ran to completion",
                      "resize sweep complete" in blob))
        if missing:
            print("\n  NO CHAIN-READY FOR: " + ", ".join("%dx%d" % m
                                                         for m in missing))
    else:
        after = [ln for ln in lines[wobble_base:] if RES_RE.search(ln)]
        gates.append(("sub-pixel wobble changed nothing", not after))
        if after:
            print("\n  RESOLUTION CHANGED UNDER A WOBBLE:")
            for ln in after:
                print("    " + ln[:200])

    gates.append(("no incomplete framebuffers", not incompletes))
    gates.append(("chain never went unavailable", not unavailable))
    gates.append(("no FATALs", not fatals))
    gates.append(("no per-frame repeat of a decline",
                  len(unavailable) < 2))

    if incompletes:
        print("\n== incomplete framebuffer statuses ==")
        seen = {}
        for st in incompletes:
            seen[st.upper()] = seen.get(st.upper(), 0) + 1
        for st, n in seen.items():
            print("  0x%s %-32s x%d" % (st, STATUS_NAMES.get(st, "?"), n))

    print("\n== gates (%s) ==" % args.mode)
    ok = True
    for name, passed in gates:
        print("  %-34s %s" % (name, "PASS" if passed else "FAIL"))
        ok = ok and passed
    if fatals:
        print("\n== fatals ==")
        for f_ in fatals[:10]:
            print("  " + f_[:300])
    print("\nlog: %s (%d lines)" % (args.log, len(lines)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=None,
                    help="disc image; defaults to build/iso_path.txt, then "
                         "$B3_ISO")
    ap.add_argument("--mode", default="sweep",
                    choices=("sweep", "wobble", "both"))
    ap.add_argument("--sweep", default=DEFAULT_SWEEP)
    ap.add_argument("--every", type=int, default=120,
                    help="presents between sweep steps")
    ap.add_argument("--msaa", type=int, default=4)
    ap.add_argument("--webgl", type=int, default=0, choices=(0, 1, 2),
                    help="force B3_WEBGL (0 = leave the engine's own choice). "
                         "1 is the version whose framebuffer rules make the "
                         "reported 0x8CD9 reachable at all.")
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--track", default=None)
    ap.add_argument("--cache", default="opfs",
                    choices=("opfs", "idbfs", "memfs"))
    ap.add_argument("--gl", default="swiftshader", choices=tuple(GL_FLAGS))
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--cdp", type=int, default=0)
    ap.add_argument("--log", default="/tmp/b3_resize_sweep.log")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra engine env, repeatable")
    ap.add_argument("--settle", type=float, default=4.0,
                    help="wobble mode: seconds to wait after each nudge")
    ap.add_argument("--tail", type=float, default=40.0,
                    help="wobble mode: seconds to keep racing at the end, so "
                         "B3_WEB_VPPROF accumulates polls to report")
    args = ap.parse_args()
    if not args.iso:
        args.iso = default_iso()
    if not args.iso or not os.path.exists(args.iso):
        sys.exit("no disc image: pass --iso, or boot once in iso mode so "
                 "build/iso_path.txt is written, or set $B3_ISO")
    print("[sweep] disc image: %s" % args.iso)

    # Sub-pixel CSS sizes and fractional device pixel ratios -- the two ways a
    # real browser moves the measurement without anything being resized.
    args.wobbles = [
        (640.4, 480.0, None),
        (640.6, 480.0, None),      # crosses x.5: the rounding flips here
        (641.2, 480.3, None),
        (640.0, 480.0, 1.0009),    # a fullscreen-transition-shaped dpr nudge
        (640.5, 480.5, 1.0),
    ]
    for attr in ("port", "cdp"):
        if getattr(args, attr) == 0:
            s = socket.socket()
            s.bind(("127.0.0.1", 0))
            setattr(args, attr, s.getsockname()[1])
            s.close()

    if args.mode == "both":
        rc = 0
        base_log = args.log
        for m in ("sweep", "wobble"):
            args.mode = m
            args.log = base_log.replace(".log", "_%s.log" % m)
            for attr in ("port", "cdp"):
                s = socket.socket()
                s.bind(("127.0.0.1", 0))
                setattr(args, attr, s.getsockname()[1])
                s.close()
            print("\n" + "=" * 68 + "\n== %s\n" % m + "=" * 68)
            rc |= asyncio.run(run(args))
        sys.exit(rc)
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
