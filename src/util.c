/*
 * util.c — Filesystem and utility helpers
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ─── File I/O ───────────────────────────────────────────────────── */

int gscope_write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);

    if (written < 0 || (size_t)written != len)
        return -1;

    return 0;
}

int gscope_read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ssize_t n = read(fd, buf, size - 1);
    close(fd);

    if (n < 0)
        return -1;

    buf[n] = '\0';

    /* Strip trailing newline */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';

    return (int)n;
}

int gscope_read_uint64(const char *path, uint64_t *value)
{
    char buf[64];
    if (gscope_read_file(path, buf, sizeof(buf)) < 0)
        return -1;

    /* Handle "max" (e.g. memory.max = "max") */
    if (strcmp(buf, "max") == 0) {
        *value = UINT64_MAX;
        return 0;
    }

    char *end;
    errno = 0;
    *value = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf)
        return -1;

    return 0;
}

int gscope_read_uint32(const char *path, uint32_t *value)
{
    uint64_t v;
    if (gscope_read_uint64(path, &v) < 0)
        return -1;
    if (v > UINT32_MAX)
        *value = UINT32_MAX;
    else
        *value = (uint32_t)v;
    return 0;
}

/* ─── Directory Operations ───────────────────────────────────────── */

int gscope_mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    gscope_strlcpy(tmp, path, sizeof(tmp));

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

int gscope_rmdir_r(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    struct dirent *entry;
    int ret = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) {
            ret = -1;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (gscope_rmdir_r(child) != 0)
                ret = -1;
        } else {
            if (unlink(child) != 0)
                ret = -1;
        }
    }

    closedir(d);

    if (rmdir(path) != 0)
        ret = -1;

    return ret;
}

/* ─── Time ───────────────────────────────────────────────────────── */

uint64_t gscope_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}
