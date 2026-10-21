/* SPDX-License-Identifier: BSD-3-Clause */
#include <sys/mman.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "rmtfs.h"

struct rmtfs_mem {
    uint64_t address;
    size_t size;
    void *base;
    int fd;
};

static int read_value(const char *path, uint64_t *value)
{
    char text[80] = {0};
    char *end;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    errno = 0;
    *value = strtoull(text, &end, 0);
    return errno || end == text ? -1 : 0;
}

struct rmtfs_mem *rmtfs_mem_open(void)
{
    char name[32] = {0};
    char sysfs[256] = {0};
    char device[64] = {0};
    DIR *dir = opendir("/sys/class/uio");
    if (!dir)
        return NULL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "uio", 3) ||
            strspn(entry->d_name + 3, "0123456789") != strlen(entry->d_name + 3) ||
            !entry->d_name[3] || strlen(entry->d_name) > 20)
            continue;
        char path[256];
        snprintf(path, sizeof(path), "/sys/class/uio/%s/name", entry->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        memset(name, 0, sizeof(name));
        ssize_t count = fd < 0 ? -1 : read(fd, name, sizeof(name) - 1);
        if (fd >= 0) close(fd);
        if (count > 0 && !strcmp(name, "rmtfs\n")) {
            snprintf(sysfs, sizeof(sysfs), "/sys/class/uio/%s", entry->d_name);
            snprintf(device, sizeof(device), "/dev/%s", entry->d_name);
            break;
        }
    }
    closedir(dir);
    if (!device[0]) return NULL;
    char path[300];
    snprintf(path, sizeof(path), "%s/name", sysfs);
    int namefd = open(path, O_RDONLY | O_CLOEXEC);
    if (namefd < 0)
        return NULL;
    ssize_t n = read(namefd, name, sizeof(name) - 1);
    close(namefd);
    if (n <= 0 || strcmp(name, "rmtfs\n")) {
        fprintf(stderr, "Refusing UIO device not named rmtfs\n");
        return NULL;
    }
    uint64_t address, size;
    snprintf(path, sizeof(path), "%s/maps/map0/addr", sysfs);
    if (read_value(path, &address)) return NULL;
    snprintf(path, sizeof(path), "%s/maps/map0/size", sysfs);
    if (read_value(path, &size) ||
        !size || size > 64 * 1024 * 1024 || address > UINT64_MAX - size)
        return NULL;
    extern char aera_rmtfs_uio_device[64];
    strcpy(aera_rmtfs_uio_device, device);
    int fd = open(device, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    struct rmtfs_mem *mem = calloc(1, sizeof(*mem));
    if (!mem) {
        munmap(base, size);
        close(fd);
        return NULL;
    }
    mem->address = address;
    mem->size = size;
    mem->base = base;
    mem->fd = fd;
    fprintf(stderr, "RMTFS exchange RAM mapped: address=0x%llx size=%zu\n",
            (unsigned long long)address, mem->size);
    return mem;
}

int64_t rmtfs_mem_alloc(struct rmtfs_mem *mem, size_t size)
{
    return size <= mem->size ? (int64_t)mem->address : -EINVAL;
}

void rmtfs_mem_free(struct rmtfs_mem *mem) { (void)mem; }

static void *checked_pointer(struct rmtfs_mem *mem, unsigned long address, ssize_t len)
{
    if (len < 0 || address < mem->address)
        return NULL;
    uint64_t offset = address - mem->address;
    if (offset > mem->size || (uint64_t)len > mem->size - offset)
        return NULL;
    return (char *)mem->base + offset;
}

ssize_t rmtfs_mem_read(struct rmtfs_mem *mem, unsigned long address, void *buf, ssize_t len)
{
    void *ptr = checked_pointer(mem, address, len);
    if (!ptr)
        return -EINVAL;
    memcpy(buf, ptr, len);
    return len;
}

ssize_t rmtfs_mem_write(struct rmtfs_mem *mem, unsigned long address, const void *buf, ssize_t len)
{
    void *ptr = checked_pointer(mem, address, len);
    if (!ptr)
        return -EINVAL;
    memcpy(ptr, buf, len);
    return len;
}

void rmtfs_mem_close(struct rmtfs_mem *mem)
{
    munmap(mem->base, mem->size);
    close(mem->fd);
    free(mem);
}
