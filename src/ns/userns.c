/*
 * ns/userns.c — User namespace UID/GID mapping
 *
 * User namespaces allow mapping container UIDs to different host UIDs.
 * For example, container UID 0 (root) can map to host UID 100000,
 * providing rootless isolation.
 *
 * Mapping files:
 *   /proc/<pid>/uid_map  — UID mapping
 *   /proc/<pid>/gid_map  — GID mapping
 *   /proc/<pid>/setgroups — must write "deny" before gid_map
 *
 * Format: "<container_id> <host_id> <count>"
 * Example: "0 100000 65536" maps container UIDs 0-65535 to host 100000-165535
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ─── Write to /proc/<pid> file ──────────────────────────────────── */

static int write_proc_file(pid_t pid, const char *file, const char *value)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, file);

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t len = strlen(value);
    ssize_t written = write(fd, value, len);
    close(fd);

    if (written < 0 || (size_t)written != len)
        return -1;

    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_ns_map_uid(gscope_scope_t *scope,
                                uid_t host_uid, uid_t container_uid,
                                uint32_t count)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (count == 0)
        return gscope_set_error(GSCOPE_ERR_INVAL, "count must be > 0");

    pid_t pid = scope->init_pid;
    if (pid <= 0)
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "scope has no init process");

    if (!(scope->ns_active & GSCOPE_NS_USER))
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "user namespace not active");

    char mapping[128];
    snprintf(mapping, sizeof(mapping), "%u %u %u",
             (unsigned)container_uid, (unsigned)host_uid, count);

    GSCOPE_DEBUG(scope->ctx, "writing uid_map for PID %d: %s",
                 (int)pid, mapping);

    if (write_proc_file(pid, "uid_map", mapping) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "failed to write uid_map for PID %d",
                                      (int)pid);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_ns_map_gid(gscope_scope_t *scope,
                                gid_t host_gid, gid_t container_gid,
                                uint32_t count)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (count == 0)
        return gscope_set_error(GSCOPE_ERR_INVAL, "count must be > 0");

    pid_t pid = scope->init_pid;
    if (pid <= 0)
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "scope has no init process");

    if (!(scope->ns_active & GSCOPE_NS_USER))
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "user namespace not active");

    /*
     * IMPORTANT: Must write "deny" to setgroups BEFORE writing gid_map.
     * Otherwise gid_map write fails with EPERM.
     * See user_namespaces(7).
     */
    GSCOPE_DEBUG(scope->ctx, "writing setgroups=deny for PID %d", (int)pid);

    if (write_proc_file(pid, "setgroups", "deny") != 0) {
        /* Non-fatal on some kernels where setgroups is already denied */
        GSCOPE_DEBUG(scope->ctx, "setgroups write failed (may be already denied)");
    }

    char mapping[128];
    snprintf(mapping, sizeof(mapping), "%u %u %u",
             (unsigned)container_gid, (unsigned)host_gid, count);

    GSCOPE_DEBUG(scope->ctx, "writing gid_map for PID %d: %s",
                 (int)pid, mapping);

    if (write_proc_file(pid, "gid_map", mapping) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "failed to write gid_map for PID %d",
                                      (int)pid);

    gscope_clear_error();
    return GSCOPE_OK;
}
