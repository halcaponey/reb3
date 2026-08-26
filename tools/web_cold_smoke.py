#!/usr/bin/env python3
"""web_cold_smoke.py -- THE DISC IS THE ONLY REQUIRED SOURCE.

tools/web_smoke.py and tools/web_shell_smoke.py both boot with whatever cache
the profile happens to have, and both were green while a user could not start
a race at all:

    web: cache load failed (UnknownError: Failed to read large IndexedDB
         value) -- starting from a cold cache
    ...
    FATAL: no usable build/tracks/US_C1_V1/track.obj

IDBFS keeps one IndexedDB value per file, Chrome would not read the 25 MB one
back, and everything SMALL came through -- including the .stamps that record
"this stage already ran".  The engine believed them, skipped the stage, logged
nothing, and died.  Nothing in the gates covered a cache that comes back
DAMAGED rather than absent, so nothing caught it.

This is that gate.  Four boots, each of which must reach a live race:

    1. persistence UNAVAILABLE   navigator.storage.getDirectory rejects.
                                 The guaranteed baseline: everything is
                                 materialised from the ISO into memory.
    2. cold                      empty profile, persistence working.
    3. warm                      same profile again -- and it must actually BE
                                 warm, i.e. measurably faster than the cold
                                 boot that preceded it.
    4. DAMAGED                   the warm cache with one big artefact removed
                                 and its .stamps left behind -- the user's bug,
                                 reproduced deterministically.  The engine must
                                 notice the stale claim, say so, and re-extract
                                 from the disc.

Headless Chromium only; never opens a visible browser.

    python3 tools/web_cold_smoke.py --iso <path> [--track US_C1_V1]
"""
import argparse
import asyncio
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    import websockets
except ImportError:
    sys.exit("need: pip install websockets")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_iso():
    """CLI > build/iso_path.txt > $B3_ISO -- the engine's own ladder.

    See the long note over the same function in tools/web_smoke.py: a
    plausible-looking non-XISO image fails several minutes into a boot with
    every browser-side gate healthy until it does."""
    p = os.path.join(ROOT, "build", "iso_path.txt")
    if os.path.exists(p):
        try:
            v = open(p).read().strip()
            if v and os.path.exists(v):
                return v
        except OSError:
            pass
    v = os.environ.get("B3_ISO", "")
    return v if v and os.path.exists(v) else None


FATAL_MARKERS = ("FATAL", "Aborted(", "abort(", "RuntimeError", "TypeError",
                 "sent an error", "uncaught exception", "was not exported")

# Break the storage layer for real, before any module code runs.
BREAK_STORAGE = ("try{Object.defineProperty(navigator.storage,'getDirectory',"
                 "{configurable:true,value:function(){return Promise.reject("
                 "new Error('simulated storage failure'));}});}catch(e){}")

# Remove ONE file from the OPFS cache and leave the bookkeeping that claims it
# exists -- exactly what a failed large-value read leaves behind.
EVICT_OPFS = r"""
(async function () {
  var rel = %s;
  try {
    var root = await navigator.storage.getDirectory();
    var d = await root.getDirectoryHandle('b3cache', {create: false});
    var parts = rel.split('/').filter(Boolean);
    for (var i = 0; i < parts.length - 1; i++)
      d = await d.getDirectoryHandle(parts[i], {create: false});
    var name = parts[parts.length - 1];
    var size = (await (await d.getFileHandle(name)).getFile()).size;
    await d.removeEntry(name);
    return 'evicted ' + rel + ' (' + size + ' bytes), stamps left intact';
  } catch (e) { return 'ERR ' + e; }
})()
"""


def find_chrome():
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("no chromium/chrome on PATH")


async def boot(a, profile, label, cache="opfs", break_storage=False,
               evict=None):
    """One headless boot.  Returns (ok, seconds_to_race, notes)."""
    t0 = time.time()
    lines, fatals = [], []
    serve = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "webserve.py"),
         "--port", str(a.port), "--root", ROOT],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "B3_WEBSERVE_QUIET": "1"})
    chrome = subprocess.Popen([
        find_chrome(), "--headless=new", "--mute-audio",
        "--remote-debugging-port=%d" % a.cdp, "--user-data-dir=" + profile,
        "--no-first-run", "--no-default-browser-check",
        "--no-sandbox", "--disable-gpu-sandbox",
        "--use-gl=angle", "--use-angle=swiftshader",
        "--enable-unsafe-swiftshader", "--enable-features=SharedArrayBuffer",
        "--window-size=1400,900", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t_race = None
    try:
        ws_url = None
        for _ in range(150):
            try:
                with urllib.request.urlopen(
                        "http://127.0.0.1:%d/json/version" % a.cdp) as r:
                    ws_url = json.load(r)["webSocketDebuggerUrl"]
                break
            except Exception:
                time.sleep(0.2)
        if not ws_url:
            return False, None, None, ["devtools never came up"]

        async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            nid = [0]

            def note(msg):
                m = msg.get("method")
                if m == "Runtime.consoleAPICalled":
                    txt = " ".join(str(x.get("value", "")) for x in
                                   msg["params"].get("args", []))
                elif m == "Runtime.exceptionThrown":
                    txt = "EXCEPTION " + json.dumps(
                        msg["params"]["exceptionDetails"].get("text", ""))
                else:
                    return
                lines.append(txt)
                if any(k in txt for k in FATAL_MARKERS):
                    fatals.append(txt)

            async def cmd(method, params=None, sess=None):
                nid[0] += 1
                m = {"id": nid[0], "method": method, "params": params or {}}
                if sess:
                    m["sessionId"] = sess
                await ws.send(json.dumps(m))
                while True:
                    r = json.loads(await ws.recv())
                    note(r)
                    if r.get("id") == nid[0]:
                        return r.get("result", {})

            t = await cmd("Target.createTarget", {"url": "about:blank"})
            s = await cmd("Target.attachToTarget",
                          {"targetId": t["targetId"], "flatten": True})
            sess = s["sessionId"]
            await cmd("Runtime.enable", {}, sess)
            await cmd("Page.enable", {}, sess)
            await cmd("DOM.enable", {}, sess)

            # B3_LOADPROF gives the ENGINE'S OWN load time.  Wall clock to the
            # first FPS line would do instead, but it carries ~30 s of harness
            # (browser start, page load, SwiftShader reaching a frame) that is
            # identical cold or warm and swamps the difference being gated.
            env = {"B3_AUTODRIVE": "1", "B3_EXIT_AT": str(a.seconds),
                   "B3_MSAA": "0", "B3_NO_VSYNC": "1", "B3_TRACK": a.track,
                   "B3_LOADPROF": "1"}
            src = ("window.B3_ENV=%s;window.B3_CACHE_MODE=%s;"
                   % (json.dumps(env), json.dumps(cache)))
            if break_storage:
                src += BREAK_STORAGE
            await cmd("Page.addScriptToEvaluateOnNewDocument",
                      {"source": src}, sess)

            if evict:
                await cmd("Page.navigate",
                          {"url": "http://127.0.0.1:%d/web/" % a.port}, sess)
                await asyncio.sleep(1.0)
                r = await cmd("Runtime.evaluate",
                              {"expression": EVICT_OPFS % json.dumps(evict),
                               "awaitPromise": True, "returnByValue": True},
                              sess)
                lines.append("evict: " + str(r.get("result", {}).get("value")))

            await cmd("Page.navigate",
                      {"url": "http://127.0.0.1:%d/web/smoke.html" % a.port},
                      sess)
            await asyncio.sleep(2.0)
            doc = await cmd("DOM.getDocument", {}, sess)
            node = await cmd("DOM.querySelector",
                             {"nodeId": doc["root"]["nodeId"],
                              "selector": "#iso"}, sess)
            await cmd("DOM.setFileInputFiles",
                      {"files": [os.path.abspath(a.iso)],
                       "nodeId": node["nodeId"]}, sess)

            deadline = time.time() + a.timeout
            while time.time() < deadline:
                try:
                    note(json.loads(await asyncio.wait_for(ws.recv(),
                                                           timeout=2.0)))
                except asyncio.TimeoutError:
                    pass
                if t_race is None and any("FPS:" in ln for ln in lines):
                    t_race = time.time() - t0
                if fatals:
                    break
                if any("smoke: exit" in ln for ln in lines):
                    break
    finally:
        for p in (chrome, serve):
            try:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=5)
            except Exception:
                p.kill()

    with open(os.path.join(tempfile.gettempdir(),
                           "b3_cold_%s.log" % label), "w") as f:
        f.write("\n".join(lines) + "\n")
    notes = [ln for ln in lines
             if "cache" in ln.lower() or "stale stamp" in ln
             or "NOT RESOLVED" in ln]
    load = None
    for ln in lines:
        if "[loadprof] TOTAL" in ln:
            try:
                load = float(ln.split("TOTAL(since 1st)")[1].split("s")[0])
            except Exception:
                pass
    return ((not fatals and t_race is not None), t_race, load,
            notes + fatals[:2])


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=None,
                    help="disc image; defaults to build/iso_path.txt, then "
                         "$B3_ISO")
    ap.add_argument("--track", default="US_C1_V1",
                    help="the user's track; its track.obj is 25 MB, which is "
                         "the size IndexedDB would not read back")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--port", type=int, default=8797)
    ap.add_argument("--cdp", type=int, default=9797)
    a = ap.parse_args()
    if not a.iso:
        a.iso = default_iso()
    if not a.iso:
        sys.exit("no disc image: pass --iso, or boot once in iso mode so "
                 "build/iso_path.txt is written, or set $B3_ISO")
    if not os.path.exists(a.iso):
        sys.exit("no such iso: " + a.iso)
    print("[cold] disc image: %s" % a.iso)

    profile = tempfile.mkdtemp(prefix="b3-cold-")
    results = []
    try:
        # 1. persistence unavailable -- a THROWAWAY profile, so nothing it
        #    does can leak into the cold/warm pair below.
        p1 = tempfile.mkdtemp(prefix="b3-nostore-")
        try:
            results.append(("persistence UNAVAILABLE",) +
                           await boot(a, p1, "nostore", break_storage=True))
        finally:
            shutil.rmtree(p1, ignore_errors=True)

        results.append(("cold (empty cache)",) +
                       await boot(a, profile, "cold"))
        results.append(("warm (same profile)",) +
                       await boot(a, profile, "warm"))
        results.append(("DAMAGED cache (artefact gone, stamps kept)",) +
                       await boot(a, profile, "damaged",
                                  evict="tracks/%s/track.obj" % a.track))
    finally:
        shutil.rmtree(profile, ignore_errors=True)

    print("\n== boots ==")
    ok = True
    for name, passed, secs, load, notes in results:
        extra = ""
        if secs:
            extra = "  (race at %.1f s" % secs
            extra += ", load %.1f s)" % load if load else ")"
        print("  %-42s %s%s" % (name, "PASS" if passed else "FAIL", extra))
        for n in notes[:3]:
            print("        %s" % n[:150])
        ok = ok and passed

    # The warm boot must actually be WARM.  Without this the gate would pass on
    # a cache that persists nothing at all -- which is precisely the state a
    # storage bug leaves behind.  Compared on the engine's own load clock.
    cold = next((l for n, p, s, l, _ in results if n.startswith("cold")), None)
    warm = next((l for n, p, s, l, _ in results if n.startswith("warm")), None)
    if cold and warm:
        faster = warm < cold * 0.5
        print("  %-42s %s  (cold load %.1f s -> warm load %.1f s)"
              % ("warm boot is really warm", "PASS" if faster else "FAIL",
                 cold, warm))
        ok = ok and faster
    else:
        print("  %-42s FAIL  (no B3_LOADPROF total found)"
              % "warm boot is really warm")
        ok = False

    print("\n%s" % ("PASS: the disc alone is enough, and a damaged cache "
                    "heals" if ok else "FAIL"))
    return 0 if ok else 1


sys.exit(asyncio.run(main()))
