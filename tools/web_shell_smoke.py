#!/usr/bin/env python3
"""web_shell_smoke.py -- drive THE REAL SHELL (web/index.html) headlessly and
prove that game pixels reach the canvas.

tools/web_smoke.py exercises the engine through a bare test page and gates on
LOG LINES.  That is exactly how a black screen got shipped: every gate passed
while nothing was ever presented.  This one gates on PIXELS -- it screenshots
the canvas and refuses to pass on a uniformly black image.

It drives the real page the way a player does: click PLAY, hand the file input
a genuine File through CDP DOM.setFileInputFiles, then wait and look.

Headless Chromium only; never opens a visible browser.

    python3 tools/web_shell_smoke.py --iso <path> [--shot out.png] [--seconds 40]
"""
import argparse
import asyncio
import base64
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


def find_chrome():
    for c in ("google-chrome", "chromium", "chromium-browser", "google-chrome-stable"):
        p = shutil.which(c)
        if p:
            return p
    sys.exit("no chromium/chrome found")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=None,
                    help="disc image; defaults to build/iso_path.txt, then "
                         "$B3_ISO")
    ap.add_argument("--seconds", type=float, default=40.0,
                    help="how long to let the game run before looking")
    ap.add_argument("--shot", default="/tmp/b3_shell.png")
    ap.add_argument("--log", default="/tmp/b3_shell.log")
    ap.add_argument("--port", type=int, default=8744)
    ap.add_argument("--cdp", type=int, default=9744)
    ap.add_argument("--timeout", type=float, default=600.0)
    a = ap.parse_args()

    if not a.iso:
        a.iso = default_iso()
    if not a.iso:
        sys.exit("no disc image: pass --iso, or boot once in iso mode so "
                 "build/iso_path.txt is written, or set $B3_ISO")
    iso = os.path.abspath(a.iso)
    if not os.path.exists(iso):
        sys.exit("no such iso: " + iso)
    print("[shell] disc image: %s" % iso)

    # the repo root is the docroot, exactly as `make serve` does: index.html
    # reaches the engine at ../build/web/burnout3.js
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "webserve.py"),
                            "--port", str(a.port), "--root", ROOT],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    profile = tempfile.mkdtemp(prefix="b3shell-")
    chrome = subprocess.Popen([
        find_chrome(), "--headless=new", "--mute-audio",
        "--remote-debugging-port=%d" % a.cdp, "--user-data-dir=" + profile,
        "--no-sandbox", "--disable-gpu-sandbox",
        "--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
        "--window-size=1400,900",
        "--enable-features=SharedArrayBuffer",
        "--autoplay-policy=no-user-gesture-required",
        "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    lines, verdict = [], {}
    try:
        ws_url = None
        for _ in range(150):
            try:
                with urllib.request.urlopen(
                        "http://127.0.0.1:%d/json/version" % a.cdp, timeout=1) as r:
                    ws_url = json.load(r)["webSocketDebuggerUrl"]
                break
            except Exception:
                time.sleep(0.2)
        if not ws_url:
            sys.exit("chrome never came up")

        async with websockets.connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            n = [0]
            pending = {}

            def note(msg):
                m = msg.get("method")
                if m == "Runtime.consoleAPICalled":
                    t = " ".join(str(x.get("value", "")) for x in msg["params"]["args"])
                    lines.append("C|" + t)
                elif m == "Runtime.exceptionThrown":
                    d = msg["params"]["exceptionDetails"]
                    lines.append("X|" + str(d.get("text")) + " " +
                                 str((d.get("exception") or {}).get("description", "")))

            async def cmd(method, params=None, sess=None):
                n[0] += 1
                msg = {"id": n[0], "method": method, "params": params or {}}
                if sess:
                    msg["sessionId"] = sess
                await ws.send(json.dumps(msg))
                while True:
                    r = json.loads(await ws.recv())
                    note(r)
                    if r.get("id") == n[0]:
                        if "error" in r:
                            raise RuntimeError(method + ": " + json.dumps(r["error"]))
                        return r.get("result", {})

            async def pump(seconds):
                end = time.time() + seconds
                while time.time() < end:
                    try:
                        note(json.loads(await asyncio.wait_for(ws.recv(), timeout=0.5)))
                    except asyncio.TimeoutError:
                        pass

            t = await cmd("Target.createTarget", {"url": "about:blank"})
            sess = (await cmd("Target.attachToTarget",
                              {"targetId": t["targetId"], "flatten": True}))["sessionId"]
            await cmd("Runtime.enable", {}, sess)
            await cmd("Page.enable", {}, sess)
            await cmd("DOM.enable", {}, sess)

            url = "http://127.0.0.1:%d/web/index.html" % a.port
            await cmd("Page.navigate", {"url": url}, sess)
            await pump(3.0)

            # click PLAY to reach the picker
            await cmd("Runtime.evaluate",
                      {"expression": "document.getElementById('play').click()"}, sess)
            await pump(1.5)

            # hand the file input a REAL File, the same object a picker yields
            doc = await cmd("DOM.getDocument", {"depth": -1}, sess)
            node = await cmd("DOM.querySelector",
                             {"nodeId": doc["root"]["nodeId"],
                              "selector": "#file-input"}, sess)
            if not node.get("nodeId"):
                sys.exit("shell has no #file-input")
            await cmd("DOM.setFileInputFiles",
                      {"files": [iso], "nodeId": node["nodeId"]}, sess)
            lines.append("H|handed over %s (%d bytes)" % (os.path.basename(iso),
                                                          os.path.getsize(iso)))

            # the shell verifies the image first, then enables "Start game"
            for _ in range(60):
                await pump(1.0)
                r = await cmd("Runtime.evaluate",
                              {"expression": "(function(){var b="
                                             "document.getElementById('start-btn');"
                                             "return b && !b.disabled ? 1 : 0})()",
                               "returnByValue": True}, sess)
                if r["result"].get("value"):
                    break
            await cmd("Runtime.evaluate",
                      {"expression": "document.getElementById('start-btn').click()"},
                      sess)
            lines.append("H|clicked Start game")
            await pump(a.seconds)

            # the shell routes the engine's stdout to its own on-page console,
            # not to console.log, so scrape that panel
            r = await cmd("Runtime.evaluate",
                          {"expression": "(document.getElementById('log')||{})"
                                         ".textContent || ''",
                           "returnByValue": True}, sess)
            for ln in (r["result"].get("value") or "").splitlines():
                if ln.strip():
                    lines.append("S|" + ln)

            # ---- THE GATE THAT MATTERS: are there pixels? ----
            probe = r"""
            (function () {
              var c = document.getElementById('canvas');
              if (!c) return JSON.stringify({err: 'no canvas'});
              var out = {w: c.width, h: c.height,
                         transferred: false, phase: (window.S && window.S.phase) || '?'};
              // A canvas whose control was transferred cannot be read here at
              // all -- that itself is the answer we need.
              try {
                var t = document.createElement('canvas');
                t.width = c.width; t.height = c.height;
                var x = t.getContext('2d');
                x.drawImage(c, 0, 0);
                var d = x.getImageData(0, 0, c.width, c.height).data;
                var nonblack = 0, distinct = {}, i;
                for (i = 0; i < d.length; i += 4 * 97) {
                  var k = (d[i] << 16) | (d[i+1] << 8) | d[i+2];
                  if (k !== 0) nonblack++;
                  distinct[k] = 1;
                }
                out.sampled = Math.floor(d.length / (4 * 97));
                out.nonblack = nonblack;
                out.distinct = Object.keys(distinct).length;
              } catch (e) {
                out.transferred = true;
                out.readErr = String(e);
              }
              return JSON.stringify(out);
            })()
            """
            r = await cmd("Runtime.evaluate",
                          {"expression": probe, "returnByValue": True}, sess)
            verdict = json.loads(r["result"]["value"])
            lines.append("H|canvas probe " + json.dumps(verdict))

            shot = await cmd("Page.captureScreenshot", {"format": "png"}, sess)
            with open(a.shot, "wb") as f:
                f.write(base64.b64decode(shot["data"]))
            lines.append("H|screenshot -> " + a.shot)
    finally:
        for p in (chrome, srv):
            try:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=5)
            except Exception:
                p.kill()
        shutil.rmtree(profile, ignore_errors=True)

    with open(a.log, "w") as f:
        f.write("\n".join(lines) + "\n")

    blob = "\n".join(lines)
    # "the engine ran" is necessary but NOT sufficient -- pixels decide
    gates = [
        ("engine booted",     "image bridge up" in blob),
        # the compatibility layer is gone; see tools/web_smoke.py
        ("direct WebGL",      "direct WebGL" in blob),
        ("no compat layer",   "LIBGL: Initialising gl4es" not in blob),
        ("reached a screen",  "track select" in blob or "font metrics" in blob),
        ("canvas readable",   not verdict.get("transferred", True)),
        ("PIXELS ON CANVAS",  verdict.get("nonblack", 0) > 0 and
                              verdict.get("distinct", 0) > 3),
    ]
    print("\n== gates ==")
    for name, ok in gates:
        print("  %-22s %s" % (name, "PASS" if ok else "FAIL"))
    print("\nlog: %s (%d lines)   shot: %s" % (a.log, len(lines), a.shot))
    return 0 if all(ok for _, ok in gates) else 1


sys.exit(asyncio.run(main()))
