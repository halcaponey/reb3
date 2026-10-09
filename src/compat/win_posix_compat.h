#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Undefine 16-bit legacy near/far macros from windows.h */
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

/* sysconf(_SC_NPROCESSORS_ONLN) */
#define _SC_NPROCESSORS_ONLN 1
static inline long sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return 1;
}

/* mkdir() compatibility: MinGW _mkdir takes 1 parameter */
#ifndef mkdir
#define mkdir(p, m) _mkdir(p)
#endif

/* lstat() and S_ISLNK compatibility for Windows */
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
static inline int lstat(const char *path, struct stat *buf)
{
    return stat(path, buf);
}

/* setenv() compatibility: implement via _putenv_s */
static inline int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name) return -1;
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value ? value : "");
}

/* realpath() compatibility: implement via _fullpath */
static inline char *realpath(const char *path, char *resolved_path)
{
    return _fullpath(resolved_path, path, resolved_path ? 4096 : 0);
}

/* symlink() compatibility: CreateSymbolicLinkA with directory flag */
static inline int symlink(const char *target, const char *linkpath)
{
    DWORD flags = 0;
    DWORD attr = GetFileAttributesA(target);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        flags |= 1; /* SYMBOLIC_LINK_FLAG_DIRECTORY */
    }
#if defined(SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
    if (CreateSymbolicLinkA(linkpath, target, flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        return 0;
#endif
    if (CreateSymbolicLinkA(linkpath, target, flags))
        return 0;
    return -1;
}

/* fmemopen() compatibility for Windows: memory buffer backed temporary FILE* */
static inline FILE *win_fmemopen(void *buf, size_t size, const char *mode)
{
    (void)mode;
    FILE *f = tmpfile();
    if (!f) return NULL;
    if (buf && size > 0) {
        fwrite(buf, 1, size, f);
        rewind(f);
    }
    return f;
}
#ifndef fmemopen
#define fmemopen(b, s, m) win_fmemopen((b), (s), (m))
#endif

/* Common OpenGL tokens not present in Windows 1.1 gl.h */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif

#ifndef GL_FUNC_ADD
#define GL_FUNC_ADD 0x8006
#endif
#ifndef GL_FUNC_SUBTRACT
#define GL_FUNC_SUBTRACT 0x800A
#endif
#ifndef GL_FUNC_REVERSE_SUBTRACT
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#endif
#ifndef GL_MIN
#define GL_MIN 0x8007
#endif
#ifndef GL_MAX
#define GL_MAX 0x8008
#endif

#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#endif
#ifndef GL_BLEND_DST_RGB
#define GL_BLEND_DST_RGB 0x80C8
#endif
#ifndef GL_BLEND_EQUATION_RGB
#define GL_BLEND_EQUATION_RGB 0x8009
#endif

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
