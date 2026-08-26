#!/bin/sh
# web/fetch_deps.sh -- THE WEB PORT NO LONGER HAS A THIRD-PARTY DEPENDENCY.
#
# It had exactly one, gl4es, and this script fetched it.  gl4es was here
# because the harness drew through the GL 2.1 COMPATIBILITY surface -- immediate
# mode, display lists, GL_QUADS, glPushAttrib, glAlphaFunc,
# glTexEnvi(GL_COMBINE), fog, the matrix stack -- and WebGL has none of that.
# gl4es re-implemented the whole surface on GLES2 and rewrote the GLSL 1.10
# shaders in postfx/carfx/trackmesh on the way through.
#
# The retained-renderer wave removed every one of those things from the engine.
# What it emits now is a strict subset of GLES 2.0 -- static VBOs, generic
# vertex attributes, GLSL, and the small set of state calls the CPU shadow in
# burnout3_render.h owns -- under the same entry-point names, so Emscripten's
# own WebGL bindings satisfy the link with nothing in between.  <GL/gl.h>
# resolves to web/GL/gl.h, which is GLES2; see that file and web/b3_web.c.
#
# The script is kept rather than deleted because `bash web/fetch_deps.sh` is in
# the build instructions, in the CI recipe and in several handoff documents,
# and a "no such file" there reads like a broken checkout rather than a
# dependency that went away.  It is now a no-op that says so.
#
# THE ANDROID PORT STILL USES gl4es and still fetches it, through its own
# android/fetch_deps.sh -- its GLES driver is real hardware, its bring-up is
# tied to the EGL rebind dance, and nothing there is call-count bound.  That is
# the port to look at if this one ever needs a compatibility layer again.
set -e

echo "== web deps: none.  gl4es was retired when the renderer stopped needing"
echo "   a GL 2.1 compatibility layer; the web build talks to WebGL directly."
echo "== now: make wasm"
