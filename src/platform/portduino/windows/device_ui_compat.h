#pragma once

// PROTOTYPE for [env:native-windows-mui], force-included ahead of every translation unit:
// the POSIX/glibc bits device-ui and its dependencies take for granted and MinGW-w64
// leaves out. Each belongs upstream in the library that uses it.
#ifdef _WIN32

// localtime_r() and gmtime_r(): MinGW declares them only when asked, before <time.h>.
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif

#include <string.h>

// glibc's <sys/types.h> typedef; PNGdec's inflate.c uses it.
typedef unsigned int uint;

// GNU extension used by device-ui's node and message filters.
static inline char *device_ui_compat_strcasestr(const char *haystack, const char *needle)
{
    size_t len = strlen(needle);
    if (len == 0)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, len) == 0)
            return (char *)haystack;
    }
    return NULL;
}
#define strcasestr device_ui_compat_strcasestr

#ifdef __cplusplus
#include <io.h>
// POSIX mkdir() takes a mode; MinGW's has no such parameter (PosixFileSystem.cpp).
inline int mkdir(const char *path, unsigned int)
{
    return ::mkdir(path);
}
#endif

#endif // _WIN32
