#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifndef PROT_READ
#define PROT_READ  1
#define PROT_WRITE 2
#endif

#ifndef MAP_SHARED
#define MAP_SHARED  1
#define MAP_PRIVATE 2
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    (void)addr; (void)prot; (void)flags;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return MAP_FAILED;
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m)
        return MAP_FAILED;
    uint64_t off64 = (uint64_t)offset;
    void *p = MapViewOfFile(m, FILE_MAP_READ, (DWORD)(off64 >> 32), (DWORD)(off64 & 0xFFFFFFFFu), len);
    CloseHandle(m);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void *addr, size_t len)
{
    (void)len;
    return UnmapViewOfFile(addr) ? 0 : -1;
}

#endif /* _WIN32 */
