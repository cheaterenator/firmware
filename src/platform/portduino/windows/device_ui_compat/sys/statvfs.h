#pragma once

// PROTOTYPE for [env:native-windows-mui]: the slice of POSIX <sys/statvfs.h> that
// device-ui's PosixStorage::info() uses, which MinGW-w64 does not ship. Backed by the CRT's
// _getdiskfree() rather than GetDiskFreeSpaceEx(), to keep <windows.h> out of device-ui.
// Paths on a lettered drive only; UNC paths fail with ENOSYS.

#include <direct.h>
#include <errno.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct statvfs {
    unsigned long long f_bsize;
    unsigned long long f_frsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
};

static inline int statvfs(const char *path, struct statvfs *buf)
{
    char full[_MAX_PATH];
    if (!_fullpath(full, path, sizeof(full)))
        return -1;
    char letter = full[0];
    if (letter >= 'a' && letter <= 'z')
        letter -= 'a' - 'A';
    if (letter < 'A' || letter > 'Z' || full[1] != ':') {
        errno = ENOSYS;
        return -1;
    }
    struct _diskfree_t df;
    if (_getdiskfree((unsigned)(letter - 'A' + 1), &df) != 0) {
        errno = EIO;
        return -1;
    }
    unsigned long long cluster = (unsigned long long)df.sectors_per_cluster * df.bytes_per_sector;
    buf->f_bsize = cluster;
    buf->f_frsize = cluster;
    buf->f_blocks = df.total_clusters;
    buf->f_bfree = df.avail_clusters;
    buf->f_bavail = df.avail_clusters;
    return 0;
}

#ifdef __cplusplus
}
#endif
