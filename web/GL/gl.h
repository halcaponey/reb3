/* web/GL/gl.h -- <GL/gl.h> FOR THE WEB BUILD, WITHOUT gl4es.
 *
 * ==================================================== WHY THIS REPLACED gl4es
 *
 * gl4es was here to re-implement the GL 1.x/2.1 COMPATIBILITY surface -- the
 * matrix stack, glBegin/glEnd, display lists, glTexEnv, the fixed-function
 * pipeline -- on top of GLES2, because that is what this harness drew through.
 * The retained-renderer wave deleted all of it.  What the engine emits now is
 * a strict subset of GLES 2.0: static VBOs, generic vertex attributes, GLSL,
 * and the handful of state calls in burnout3_render.h's shadow.  Every entry
 * point it still calls is core GLES2 under the SAME NAME, so Emscripten's own
 * WebGL bindings satisfy the link directly and the compatibility layer has
 * nothing left to do.
 *
 * Dropping it is not only dead weight.  gl4es sits BETWEEN the engine and
 * WebGL and does real per-draw work there: it keeps one scratch VBO per
 * attribute, re-points them per batch, and re-checks the fixed-function state
 * against the bound program to decide whether to compile a shader VARIANT
 * (fpe_ReleventState / fpe_CustomShader, src/gl/fpe.c:1090).  Now that no
 * fixed-function state is ever set, all of that is pure overhead on a path
 * whose cost is counted in WebGL calls per frame.
 *
 * WHAT THIS FILE IS.  The sources say `#include <GL/gl.h>`; -Iweb puts this
 * ahead of the sysroot, so on the web that resolves HERE, and here it means
 * GLES2.  Emscripten ships its own <GL/gl.h>, and it is deliberately NOT what
 * we want: for __EMSCRIPTEN__ it defines USE_MGL_NAMESPACE and renames every
 * entry point to mgl*, which is the legacy GL-emulation path.
 *
 * The desktop and Android builds do not see this file at all -- they keep
 * their own <GL/gl.h> (the system's, and gl4es' respectively).  Android stays
 * on gl4es for now: its GLES driver is real, its bring-up sequence is tied to
 * the EGL rebind dance, and nothing there is call-count bound.
 */
#ifndef B3_WEB_GL_GL_H
#define B3_WEB_GL_GL_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/* GLES2 spells the double-precision-free subset of GL, so a few names the
 * desktop headers carry are simply absent.  These are the ones this harness
 * mentions; each is the SAME VALUE the desktop header gives it.  Nothing here
 * enables a feature -- the tokens are used in code that is compiled on every
 * target and reaches them only on paths GLES2 also supports. */

/* GL_TEXTURE_MAX_ANISOTROPY_EXT / GL_MAX_... come from gl2ext.h when the
 * EXT_texture_filter_anisotropic block is present; the engine also defines
 * them defensively, so nothing is needed here. */

/* glGetFloatv / glGetIntegerv / glGetBooleanv, glReadPixels, glFinish,
 * glLineWidth, glColorMask, glFrontFace, glBlendEquation, glCopyTexImage2D,
 * glCopyTexSubImage2D, glGenerateMipmap, the whole shader and buffer API and
 * every framebuffer call this engine makes are all core GLES2 and come
 * straight from <GLES2/gl2.h> above. */

/* GLES2 has no GLdouble-based entry points and no `GLclampd`; nothing in this
 * tree uses them.  It also has no `APIENTRY` decoration -- provide the name so
 * any stray declaration still parses. */
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

#endif /* B3_WEB_GL_GL_H */
