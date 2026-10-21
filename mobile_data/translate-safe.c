/* SPDX-License-Identifier: Apache-2.0 */
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/statfs.h>
#include "translate.h"

#define RAM_ROOT "/tmp/aera-cellular-rfs"

int sanitize_path(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) >= 2048)
        return -1;
    const char *part = path + 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') ||
            (size == 2 && part[0] == '.' && part[1] == '.'))
            return -1;
        if (!end)
            return 0;
        part = end + 1;
    }
    return -1;
}

// Resolve every component without following symlinks; writes are RAM-only.
static int open_beneath(const char *root, const char *relative, int flags, int create_dirs)
{
    char copy[2048];
    if (!*relative || strlen(relative) >= sizeof(copy)) {
        errno = EINVAL;
        return -1;
    }
    strcpy(copy, relative);
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0)
        return -1;
    char *part = copy;
    char *slash;
    while ((slash = strchr(part, '/'))) {
        *slash = 0;
        if (create_dirs && mkdirat(dir, part, 0700) && errno != EEXIST) {
            close(dir);
            return -1;
        }
        int next = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        if (next < 0)
            return -1;
        dir = next;
        part = slash + 1;
    }
    int fd = openat(dir, part, flags | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    return fd;
}

int translate_open(const char *path, int flags)
{
    if (sanitize_path(path)) {
        errno = EACCES;
        return -1;
    }
    const char *rw = "/readwrite/";
    if (!strncmp(path, rw, strlen(rw))) {
        struct statfs fs;
        if (statfs("/tmp", &fs) ||
            ((unsigned long)fs.f_type != 0x01021994UL &&
             (unsigned long)fs.f_type != 0x858458f6UL)) {
            errno = EROFS;
            return -1;
        }
        if (mkdir(RAM_ROOT, 0700) && errno != EEXIST)
            return -1;
        return open_beneath(RAM_ROOT, path + strlen(rw), flags, 1);
    }
    const char *prefixes[] = {
        "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/",
        "/readonly/vendor/firmware/", "/readonly/firmware/",
    };
    const char *configured = getenv("AERA_CELLULAR_FIRMWARE");
    const char *roots[] = { configured ? configured : "/firmware/image",
                           "/vendor/firmware_mnt/image", "/vendor/firmware",
                           "/mnt/aera-stock/vendor/firmware" };
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p) {
        if (strncmp(path, prefixes[p], strlen(prefixes[p])))
            continue;
        if ((flags & O_ACCMODE) != O_RDONLY || flags & (O_CREAT | O_TRUNC | O_APPEND)) {
            errno = EROFS;
            return -1;
        }
        for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
            int fd = open_beneath(roots[r], path + strlen(prefixes[p]), O_RDONLY, 0);
            if (fd >= 0)
                return fd;
        }
        return -1;
    }
    errno = ENOENT;
    return -1;
}
