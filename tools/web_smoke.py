#!/usr/bin/env python3
"""web_smoke.py -- boot the wasm engine in HEADLESS Chromium against a real
Xbox image and gate on what it prints.

Nothing here ever opens a visible browser: Chrome runs --headless=new in a
throwaway profile, with SwiftShader for WebGL so the GL path is really
exercised rather than skipped.

The image is handed over the way a player would hand it over -- as a File on
a file input, set through CDP's DOM.setFileInputFiles.  That matters: a File
is read lazily off disk, so the 2.4 GB image never enters the page's memory,
and the engine's WORKERFS + synchronous-pread path is the one under test.

    tools/web_smoke.py --iso "<path to .xiso.iso>" [--seconds 30]

Exit status 0 only if every gate below passes.
"""
import argparse
import asyncio
import base64
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

import websockets

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# WHICH DISC IMAGE, WHEN --iso IS NOT GIVEN.
#
# This has now cost two separate agents most of an hour, the same way both
# times: a plausible-looking "Burnout 3 - Takedown (USA).iso" sitting in a
# media folder that is NOT an XISO.  The engine answers
#
#     iso: /iso/game.xiso is neither an XISO image nor a game directory
#          -- falling back to the pre-extracted build/ tree
#
# and then dies on the first missing artefact ("FATAL: build/frontend/font.bin
# -- no such file"), several minutes into a boot, with every browser-side gate
# still looking healthy up to that point.
#
# The engine already REMEMBERS the right one: src/burnout3_isodata.c writes
# build/iso_path.txt on the first successful --iso run and resolves
# CLI > $B3_ISO > build/iso_path.txt itself.  So the harnesses use the same
# ladder rather than making the operator paste a path.  tools/validate_wma.py
# has done this for a while; the web smokes had not, which is why the trap was
# still live.
def default_iso():
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

# Where --pin-frame parks the BMP inside the module filesystem. /app is the
# engine's CWD there (web/pre.js), and it is MEMFS, so nothing touches disk.
PIN_PATH = "/app/pinned.bmp"


# A gate that stays green through a crash is worse than no gate. The engine's
# own word is "FATAL", but the ways this target dies are mostly Emscripten's:
# an abort() from a bad runtime call, an uncaught TypeError on a worker, a
# pthread taking itself down. All of them count.
FATAL_MARKERS = ("FATAL", "Aborted(", "abort(", "RuntimeError",
                 "TypeError", "sent an error", "uncaught exception",
                 "was not exported")


def is_fatal(text):
    return any(m in text for m in FATAL_MARKERS)


# WHICH WebGL IMPLEMENTATION THE RUN GETS.  The default is unchanged and is
# what every gate in this repo is calibrated against: SwiftShader, a real
# (software) WebGL, so the GL path runs for real instead of falling back to
# "no context".  It is not a frame-rate oracle -- see web/README.md.
#
# `--gl hw` reaches the REAL GPU from --headless=new, which turns out to be
# possible and is how the port's frame time can be measured honestly without
# ever opening a visible browser.  MEASURED on an RTX 3090 / driver 595.84:
#
#     --use-angle=vulkan  ->  ANGLE (NVIDIA, Vulkan 1.4.329 (RTX 3090), NVIDIA)
#     --use-angle=gl      ->  NO CONTEXT AT ALL   (headless has no EGL display)
#     (no flag)           ->  SwiftShader, silently
#
# so the Vulkan backend is not a preference here, it is the only one that
# works.  A machine with no usable GPU falls back to SwiftShader by itself and
# the run still completes -- which is why the boot line that names the
# renderer, "[Burnout3] web: gpu = ...", is worth reading before any number
# from a --gl hw run is believed.
GL_FLAGS = {
    "swiftshader": ["--use-gl=angle", "--use-angle=swiftshader",
                    "--enable-unsafe-swiftshader"],
    "hw": ["--use-gl=angle", "--use-angle=vulkan",
           "--enable-features=Vulkan", "--ignore-gpu-blocklist"],
}


# Read the canvas back and count non-black, distinct sampled pixels.  Drawn
# into a scratch 2D canvas because the game's own drawing buffer is not
# preserved; a transferred canvas throws here, which is itself informative.
PIXEL_PROBE = r"""
(function () {
  var c = document.getElementById('canvas');
  if (!c) return JSON.stringify({err: 'no canvas'});
  var out = {w: c.width, h: c.height};
  try {
    var t = document.createElement('canvas');
    t.width = c.width; t.height = c.height;
    var x = t.getContext('2d');
    x.drawImage(c, 0, 0);
    var d = x.getImageData(0, 0, c.width, c.height).data;
    var nonblack = 0, distinct = {}, i;
    for (i = 0; i < d.length; i += 4 * 97) {
      var k = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
      if (k !== 0) nonblack++;
      distinct[k] = 1;
    }
    out.sampled = Math.floor(d.length / (4 * 97));
    out.nonblack = nonblack;
    out.distinct = Object.keys(distinct).length;
  } catch (e) {
    out.readErr = String(e);
  }
  return JSON.stringify(out);
})()
"""


# THE AUDIO BRIDGE'S OWN LINE (web/b3_web.c, b3_web_audio_stats).  There is no
# way to listen to a headless run, and rendering the mix into an
# OfflineAudioContext is not an option either -- an OfflineAudioContext is a
# different context from the live one, and the worklet under test is attached
# to the live one.  So the engine is INSTRUMENTED instead and the gate reads
# the numbers it prints: the ring's two cursors say whether audio is moving,
# the underrun counter says whether it is moving fast enough, and the RMS the
# worklet measures over its OWN OUTPUT says whether what moved was audible.
AUDIO_RE = re.compile(
    r"\[audio\] ring (-?\d+)/(\d+) frames \(([\d.]+) ms\) \| "
    r"mixed (-?\d+), played (-?\d+) in [\d.]+s \| "
    r"underruns (\d+) \(\+(\d+)\), silence (\d+) frames \| "
    r"rms ([\d.]+) peak ([\d.]+) over (\d+) windows \| "
    r"worklet (\d+) Hz, quantum (\d+), live (\d+)")


def audio_rows(lines):
    out = []
    for ln in lines:
        m = AUDIO_RE.search(ln)
        if m:
            g = m.groups()
            out.append({
                "fill": int(g[0]), "cap": int(g[1]), "ms": float(g[2]),
                "mixed": int(g[3]), "played": int(g[4]),
                "under": int(g[5]), "under_d": int(g[6]),
                "silence": int(g[7]),
                "rms": float(g[8]), "peak": float(g[9]),
                "windows": int(g[10]),
                "rate": int(g[11]), "quantum": int(g[12]), "live": int(g[13]),
            })
    return out


def find_chrome():
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("no chromium/chrome on PATH")


async def run(args):
    log_path = args.log
    lines = []          # every console line, in order
    fatals = []
    pixels = {}

    # ---------------------------------------------------------------- server
    serve = subprocess.Popen(
        [sys.executable, os.path.join(REPO, "tools", "webserve.py"),
         "--port", str(args.port), "--root", REPO],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env={**os.environ, "B3_WEBSERVE_QUIET": "1"})
    # ------------------------------------------------------------ the browser
    profile = tempfile.mkdtemp(prefix="b3-smoke-")
    chrome = subprocess.Popen([
        find_chrome(),
        "--headless=new", "--mute-audio",
        "--remote-debugging-port=%d" % args.cdp,
        "--user-data-dir=" + profile,
        "--no-first-run", "--no-default-browser-check",
        "--disable-gpu-sandbox", "--no-sandbox",
        # THE AUDIO GATE NEEDS THIS.  Chrome starts an AudioContext
        # `suspended` until a user gesture, and a suspended context never
        # pulls its AudioWorklet -- so without this flag the port's audio
        # bridge is wired up correctly and produces exactly nothing, and the
        # gate below would be measuring the autoplay policy rather than the
        # port.  MEASURED, same page, both ways: 0 quanta vs 480 in 1.5 s.
        # There is no gesture to synthesise from --headless=new.
        "--autoplay-policy=no-user-gesture-required",
    ] + GL_FLAGS[args.gl] + [
        "--enable-features=SharedArrayBuffer",
        "--window-size=1400,900",
        "about:blank",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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

            def handle_event(msg):
                m = msg.get("method")
                if m == "Runtime.consoleAPICalled":
                    parts = [a.get("value", "") for a in
                             msg["params"].get("args", [])]
                    text = " ".join(str(p) for p in parts)
                    lines.append(text)
                    if is_fatal(text):
                        fatals.append(text)
                elif m == "Log.entryAdded":
                    # THE BROWSER'S OWN LOG, which is where WebGL errors go --
                    # "WebGL: INVALID_OPERATION: drawArrays: ..." is an entry
                    # here, NOT a console.log, so subscribing only to
                    # Runtime.consoleAPICalled made a whole class of defect
                    # invisible to every gate this tool runs.  A rejected draw
                    # is a silent no-op on the web and fine on the desktop.
                    e = msg["params"]["entry"]
                    text = "%s|%s" % (e.get("level", "?"), e.get("text", ""))
                    lines.append(text)
                elif m == "Runtime.exceptionThrown":
                    d = msg["params"]["exceptionDetails"]
                    text = "EXCEPTION|" + json.dumps(d.get("text", "")) + " " + \
                           json.dumps(d.get("exception", {}).get("description", ""))
                    lines.append(text)
                    fatals.append(text)

            # one tab, attached
            t = await cmd("Target.createTarget", {"url": "about:blank"})
            s = await cmd("Target.attachToTarget",
                          {"targetId": t["targetId"], "flatten": True})
            sess = s["sessionId"]
            await cmd("Runtime.enable", {}, sess)
            await cmd("Log.enable", {}, sess)      # WebGL errors live here

            await cmd("Page.enable", {}, sess)
            await cmd("DOM.enable", {}, sess)

            env = {"B3_AUTODRIVE": "1", "B3_EXIT_AT": str(args.seconds),
                   "B3_MSAA": "0", "B3_NO_VSYNC": "1",
                   # the audio ring reports every 5 s rather than its default
                   # 10, so a short run still produces rows to gate on
                   "B3_WEB_AUDIO_STATS": "5"}
            # Caller-set B3_* vars reach the page too, and WIN over the
            # defaults above -- so `B3_RT=1 tools/web_smoke.py` tests what it
            # says it tests instead of the settings-file default.
            env.update({k: v for k, v in os.environ.items()
                        if k.startswith("B3_") and k != "B3_ISO"})
            # THE PINNED WEB FRAME.  --pin-frame turns this run into the web's
            # answer to the native offscreen screenshot the desktop gates use:
            # a fixed sim step and a fixed frame number, so the same build
            # produces the same pixels every time and two builds can be
            # bit-compared.  The engine's own B3_SHOT path writes the BMP into
            # the module filesystem and stops the loop; it is read back out
            # through FS afterwards, because the canvas itself cannot be
            # sampled at an exact frame from out here.
            if args.pin_frame:
                env.update({"B3_FIXED_DT": "0.0166667",
                            "B3_PACE_MAX_TICKS": "1",
                            "B3_SHOT": PIN_PATH,
                            "B3_SHOT_FRAME": str(args.pin_frame)})
            env.update(json.loads(os.environ.get("B3_EXTRA_ENV","{}")))
            if args.track:
                env["B3_TRACK"] = args.track
            await cmd("Page.addScriptToEvaluateOnNewDocument",
                      {"source": "window.B3_ENV=%s;window.B3_CACHE_MODE=%s;"
                                 % (json.dumps(env), json.dumps(args.cache))},
                      sess)

            url = "http://127.0.0.1:%d/web/smoke.html" % args.port
            await cmd("Page.navigate", {"url": url}, sess)
            await asyncio.sleep(2.0)

            # hand over the image exactly as a file picker would
            doc = await cmd("DOM.getDocument", {}, sess)
            node = await cmd("DOM.querySelector",
                             {"nodeId": doc["root"]["nodeId"],
                              "selector": "#iso"}, sess)
            await cmd("DOM.setFileInputFiles",
                      {"files": [os.path.abspath(args.iso)],
                       "nodeId": node["nodeId"]}, sess)

            # ------------------------------------------------------- watch it
            deadline = time.time() + args.timeout
            done = False
            # The canvas must be read WHILE THE RACE IS RUNNING: the drawing
            # buffer is not preserved, so after B3_EXIT_AT it reads black and
            # would fail a gate the engine actually passed.  Sample once the
            # race is properly under way, and keep the liveliest frame seen.
            sampled_shot = False
            while time.time() < deadline and not done:
                try:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), timeout=2.0))
                    handle_event(msg)
                except asyncio.TimeoutError:
                    pass
                if not sampled_shot and any("FPS:" in ln for ln in lines):
                    sampled_shot = True
                    r = await cmd("Runtime.evaluate",
                                  {"expression": PIXEL_PROBE,
                                   "returnByValue": True}, sess)
                    try:
                        pixels = json.loads(r["result"]["value"])
                    except Exception:
                        pixels = {}
                    lines.append("smoke: canvas (mid-race) " + json.dumps(pixels))
                    shot = await cmd("Page.captureScreenshot",
                                     {"format": "png"}, sess)
                    with open(args.shot, "wb") as fh:
                        fh.write(base64.b64decode(shot["data"]))
                    lines.append("smoke: screenshot -> " + args.shot)
                for ln in lines:
                    if "smoke: exit" in ln:
                        done = True
                        break
                if len(lines) % 200 == 0:
                    sys.stderr.write("\r[smoke] %d lines" % len(lines))
                    sys.stderr.flush()
            sys.stderr.write("\r[smoke] %d lines\n" % len(lines))

            # ---- the pinned frame, read back out of the module filesystem
            if args.pin_frame:
                r = await cmd("Runtime.evaluate",
                              {"expression": "(function(){try{"
                                             "var d=window.b3.module.FS.readFile(%s);"
                                             "var s='',i;for(i=0;i<d.length;i++)"
                                             "s+=String.fromCharCode(d[i]);"
                                             "return btoa(s);}catch(e){"
                                             "return 'ERR '+e;}})()"
                                             % json.dumps(PIN_PATH),
                               "returnByValue": True}, sess)
                v = r.get("result", {}).get("value") or ""
                if v.startswith("ERR") or not v:
                    lines.append("smoke: PINNED FRAME NOT WRITTEN ("
                                 + str(v)[:160] + ")")
                else:
                    with open(args.pin_out, "wb") as fh:
                        fh.write(base64.b64decode(v))
                    lines.append("smoke: pinned frame %d -> %s (%d bytes)"
                                 % (args.pin_frame, args.pin_out,
                                    os.path.getsize(args.pin_out)))

            # frames actually presented, if the page is still alive
            try:
                r = await cmd("Runtime.evaluate",
                              {"expression": "JSON.stringify({e:window.b3.exited,"
                                             "err:window.b3.error,n:window.b3.log.length,"
                                             "heapPeak:window.b3.heapPeak})",
                               "returnByValue": True}, sess)
                lines.append("smoke: final " + str(r.get("result", {}).get("value")))
                try:
                    peak = json.loads(r["result"]["value"]).get("heapPeak") or 0
                    if peak:
                        lines.append("smoke: peak wasm heap %.1f MiB (%d bytes)"
                                     % (peak / (1024.0 * 1024.0), peak))
                except Exception:
                    pass
            except Exception:
                pass
    finally:
        for p in (chrome, serve):
            try:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=5)
            except Exception:
                p.kill()
        shutil.rmtree(profile, ignore_errors=True)

    with open(log_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    # ------------------------------------------------------------- the gates
    blob = "\n".join(lines)
    aud = audio_rows(lines)
    last = aud[-1] if aud else None
    # SILENCE AS A SHARE OF WHAT THE WORKLET ACTUALLY EMITTED.  An absolute
    # underrun count means nothing without knowing how long the run was, and a
    # run under SwiftShader is not a fixed length.
    #
    # The two numbers are in different units and the conversion is the whole
    # point of the bridge: `played` counts INPUT frames taken out of the 44100
    # ring (it is the READ cursor, reported per window), while `silence` counts
    # OUTPUT frames at the context's rate and is cumulative.  So the input
    # total is summed across the windows and scaled up to output frames before
    # the two can be added.
    aud_in = sum(r["played"] for r in aud)
    aud_rate = last["rate"] if last and last["rate"] else 44100
    aud_out = aud_in * (aud_rate / 44100.0)
    aud_frames = aud_out + (last["silence"] if last else 0)
    aud_silent_pct = (100.0 * last["silence"] / aud_frames) if aud_frames else 100.0
    if aud:
        print("\n== audio ==")
        for r in aud[-4:]:
            print("  ring %5d fr (%5.1f ms) | mixed %8d played %8d | "
                  "underruns %5d silence %7d | rms %.4f peak %.4f | "
                  "%d Hz q%d live %d"
                  % (r["fill"], r["ms"], r["mixed"], r["played"], r["under"],
                     r["silence"], r["rms"], r["peak"], r["rate"],
                     r["quantum"], r["live"]))
        print("  %d windows | %.1f s of audio emitted | silence share %.2f%%"
              % (len(aud), aud_frames / float(aud_rate), aud_silent_pct))
    gates = [
        ("module booted",        "Burnout 3: Takedown - RE harness" in blob),
        ("image bridge up",      "web: image bridge up" in blob),
        ("iso source banner",    "[Burnout3] iso:" in blob),
        # WAS ("gl4es up", "web: gl4es up on" in blob).  The compatibility
        # layer is gone -- the engine talks to WebGL directly -- so the gate
        # that mattered is the INVERSE: that nothing pulled it back in.
        ("direct WebGL",         "web: direct WebGL" in blob),
        ("no compat layer",      "LIBGL: Initialising gl4es" not in blob),
        ("global materialise",   ("global" in blob and "->" in blob)),
        ("track geometry",       ("track" in blob.lower() and
                                  ("collision" in blob.lower() or
                                   "trackmesh" in blob.lower() or
                                   "traffic" in blob.lower()))),
        ("no FATALs",            not fatals),
        # The gate that was missing when a black screen shipped.
        ("PIXELS ON CANVAS",     pixels.get("nonblack", 0) > 0 and
                                 pixels.get("distinct", 0) > 3),
        # ---- and the four that were missing while it shipped SILENT.
        ("audio bridge up",      "web: audio bridge up" in blob),
        ("audio worklet pulling", bool(last) and last["live"] == 1
                                  and last["played"] > 0),
        # THE ONE THAT MATTERS: the worklet measured its own output and it was
        # not zero.  A ring that moves while every sample in it is 0 is what a
        # correctly wired bridge over a broken mixer looks like.
        ("AUDIO IS AUDIBLE",     bool(last) and last["peak"] > 0.0
                                  and last["windows"] > 0),
        ("underruns bounded",    bool(last) and aud_silent_pct < 25.0),
    ]
    print("\n== gates ==")
    ok = True
    for name, passed in gates:
        print("  %-22s %s" % (name, "PASS" if passed else "FAIL"))
        ok = ok and passed
    if fatals:
        print("\n== fatals ==")
        for f_ in fatals[:20]:
            print("  " + f_[:300])
    print("\nlog: %s (%d lines)" % (log_path, len(lines)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=None,
                    help="disc image; defaults to build/iso_path.txt, then "
                         "$B3_ISO (see default_iso)")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="B3_EXIT_AT, in simulated race seconds")
    ap.add_argument("--timeout", type=float, default=900.0,
                    help="wall-clock ceiling for the whole run")
    ap.add_argument("--track", default=None)
    ap.add_argument("--cache", default="opfs",
                    choices=("opfs", "idbfs", "memfs"))
    ap.add_argument("--gl", default="swiftshader",
                    choices=tuple(GL_FLAGS),
                    help="swiftshader (the default, and what every gate is "
                         "calibrated on) or hw, which reaches the real GPU "
                         "from --headless=new through ANGLE's Vulkan backend "
                         "-- the only way to measure this port's frame time "
                         "honestly without opening a browser")
    # Port 0 = OS-assigned.  A FIXED default here once made every concurrent
    # agent's smoke silently load whichever build was already serving 8731 --
    # all 8 gates passed against someone else's binary.  Never pin it again.
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--cdp", type=int, default=0)
    ap.add_argument("--log", default="/tmp/b3_web_smoke.log")
    ap.add_argument("--shot", default="/tmp/b3_web_smoke.png")
    ap.add_argument("--pin-frame", type=int, default=0,
                    help="capture THIS render frame deterministically "
                         "(B3_FIXED_DT plus the engine's own B3_SHOT path) "
                         "and read the BMP back out of the module filesystem")
    ap.add_argument("--pin-out", default="/tmp/b3_web_pinned.bmp")
    args = ap.parse_args()
    if not args.iso:
        args.iso = default_iso()
    if not args.iso or not os.path.exists(args.iso):
        sys.exit("no disc image: pass --iso, or boot once in iso mode so "
                 "build/iso_path.txt is written, or set $B3_ISO")
    print("[smoke] disc image: %s" % args.iso)
    # Resolve port 0 to a real free port ourselves: webserve and chrome both
    # receive the literal number, so the OS-assigned port must be grabbed
    # here, not inside them.
    import socket
    for attr in ("port", "cdp"):
        if getattr(args, attr) == 0:
            s = socket.socket()
            s.bind(("127.0.0.1", 0))
            setattr(args, attr, s.getsockname()[1])
            s.close()
    sys.exit(asyncio.run(run(args)))


if __name__ == "__main__":
    main()
