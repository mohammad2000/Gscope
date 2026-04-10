/*
 * cgroup/cgroup.c — Cgroup v2 unified hierarchy management
 *
 * Creates, configures, and destroys cgroup scopes under
 * /sys/fs/cgroup/gscope.slice/scope-{id}/
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* ─── Constants ──────────────────────────────────────────────────── */

#define CGROUP_ROOT       "/sys/fs/cgroup"
#define CGROUP_BASE       CGROUP_ROOT "/gscope.slice"
#define CGROUP_NAME_FMT   "scope-%u"
#define CGROUP_PATH_FMT   CGROUP_BASE "/scope-%u"

/* ─── Internal Helpers ───────────────────────────────────────────── */

static int cgroup_path(gscope_scope_t *scope, char *buf, size_t size)
{
    int n = snprintf(buf, size, CGROUP_PATH_FMT, scope->id);
    if (n < 0 || (size_t)n >= size)
        return -1;
    return 0;
}

/*
 * Enable controllers in the parent's subtree_control.
 * Must be done before the child cgroup can use those controllers.
 */
static void enable_controllers(gscope_ctx_t *ctx)
{
    const char *controllers[] = {"cpu", "memory", "io", "pids", NULL};

    /* Enable in root subtree */
    char available[256];
    if (gscope_read_file(CGROUP_ROOT "/cgroup.controllers",
                         available, sizeof(available)) < 0)
        return;

    char enable_str[256] = {0};
    int pos = 0;

    for (int i = 0; controllers[i]; i++) {
        if (strstr(available, controllers[i])) {
            int wrote = snprintf(enable_str + pos,
                                 sizeof(enable_str) - (size_t)pos,
                                 "%s+%s", pos > 0 ? " " : "",
                                 controllers[i]);
            if (wrote > 0) pos += wrote;
        }
    }

    if (pos > 0) {
        gscope_write_file(CGROUP_ROOT "/cgroup.subtree_control", enable_str);
    }

    /* Ensure base directory exists */
    gscope_mkdir_p(CGROUP_BASE, 0755);

    /* Enable in gscope.slice subtree */
    char base_subtree[512];
    snprintf(base_subtree, sizeof(base_subtree),
             CGROUP_BASE "/cgroup.subtree_control");
    gscope_write_file(base_subtree, enable_str);

    GSCOPE_DEBUG(ctx, "cgroup controllers enabled: %s", enable_str);
}

static int write_cpu_limits(const char *path, const gscope_cgroup_limits_t *lim)
{
    char file[4096];
    char val[64];

    /* cpu.max: "quota period" in microseconds */
    if (lim->cpu_cores > 0.0f) {
        int period = 100000;  /* 100ms */
        int quota = (int)(lim->cpu_cores * (float)period);
        snprintf(val, sizeof(val), "%d %d", quota, period);
        snprintf(file, sizeof(file), "%s/cpu.max", path);
        if (gscope_write_file(file, val) < 0)
            return -1;
    }

    /* cpu.weight: 1-10000 (default 100) */
    if (lim->cpu_weight > 0) {
        snprintf(val, sizeof(val), "%u", lim->cpu_weight);
        snprintf(file, sizeof(file), "%s/cpu.weight", path);
        gscope_write_file(file, val);  /* non-critical */
    }

    return 0;
}

static int write_memory_limits(const char *path, const gscope_cgroup_limits_t *lim)
{
    char file[4096];
    char val[64];

    if (lim->memory_bytes > 0) {
        /* memory.max — hard limit */
        snprintf(val, sizeof(val), "%lu", (unsigned long)lim->memory_bytes);
        snprintf(file, sizeof(file), "%s/memory.max", path);
        if (gscope_write_file(file, val) < 0)
            return -1;

        /* memory.high — soft limit (90% of max) */
        uint64_t high = lim->memory_bytes * 9 / 10;
        snprintf(val, sizeof(val), "%lu", (unsigned long)high);
        snprintf(file, sizeof(file), "%s/memory.high", path);
        gscope_write_file(file, val);

        /* memory.swap.max */
        uint64_t swap = lim->memory_swap_bytes > 0
                        ? lim->memory_swap_bytes
                        : lim->memory_bytes / 2;
        snprintf(val, sizeof(val), "%lu", (unsigned long)swap);
        snprintf(file, sizeof(file), "%s/memory.swap.max", path);
        gscope_write_file(file, val);

        /* Enable OOM group kill */
        snprintf(file, sizeof(file), "%s/memory.oom.group", path);
        gscope_write_file(file, "1");
    }

    return 0;
}

static int write_pids_limit(const char *path, const gscope_cgroup_limits_t *lim)
{
    if (lim->pids_max > 0) {
        char file[4096];
        char val[32];
        snprintf(val, sizeof(val), "%u", lim->pids_max);
        snprintf(file, sizeof(file), "%s/pids.max", path);
        return gscope_write_file(file, val);
    }
    return 0;
}

static int write_io_limits(const char *path, const gscope_cgroup_limits_t *lim)
{
    if (lim->io_weight > 0) {
        char file[4096];
        char val[32];
        snprintf(val, sizeof(val), "%u", lim->io_weight);
        snprintf(file, sizeof(file), "%s/io.weight", path);
        gscope_write_file(file, val);  /* non-critical */
    }
    return 0;
}

/* ─── Kill All Processes in Cgroup ───────────────────────────────── */

static int kill_all_procs(const char *cgroup_path, int sig)
{
    char file[4096];
    char buf[8192];

    /* Try cgroup.kill first (Linux 5.14+) */
    snprintf(file, sizeof(file), "%s/cgroup.kill", cgroup_path);
    if (gscope_write_file(file, "1") == 0)
        return 0;

    /* Fallback: iterate cgroup.procs */
    snprintf(file, sizeof(file), "%s/cgroup.procs", cgroup_path);
    if (gscope_read_file(file, buf, sizeof(buf)) < 0)
        return 0;  /* No procs or can't read */

    char *line = strtok(buf, "\n");
    int killed = 0;
    while (line) {
        pid_t pid = (pid_t)atoi(line);
        if (pid > 0) {
            kill(pid, sig);
            killed++;
        }
        line = strtok(NULL, "\n");
    }

    return killed;
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_cgroup_create(gscope_scope_t *scope,
                                   const gscope_cgroup_limits_t *limits)
{
    if (!scope || !limits)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or limits");

    gscope_ctx_t *ctx = scope->ctx;

    /* Ensure controllers are enabled */
    enable_controllers(ctx);

    /* Build cgroup path */
    char path[256];
    if (cgroup_path(scope, path, sizeof(path)) < 0)
        return gscope_set_error(GSCOPE_ERR_INVAL, "scope ID too large");

    GSCOPE_INFO(ctx, "creating cgroup: %s", path);

    /* Create directory */
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return gscope_set_error_errno(GSCOPE_ERR_CGROUP,
                                      "mkdir %s failed", path);

    /* Set limits */
    if (write_cpu_limits(path, limits) < 0) {
        GSCOPE_WARN(ctx, "failed to set CPU limits for scope %u", scope->id);
    }

    if (write_memory_limits(path, limits) < 0) {
        GSCOPE_WARN(ctx, "failed to set memory limits for scope %u", scope->id);
    }

    if (write_pids_limit(path, limits) < 0) {
        GSCOPE_WARN(ctx, "failed to set PID limits for scope %u", scope->id);
    }

    write_io_limits(path, limits);

    /* Save path in scope */
    gscope_strlcpy(scope->cgroup_path, path, sizeof(scope->cgroup_path));

    GSCOPE_INFO(ctx, "cgroup created: cpu=%.1f cores, mem=%lu MB, pids=%u",
                limits->cpu_cores,
                (unsigned long)(limits->memory_bytes / (1024 * 1024)),
                limits->pids_max);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_update(gscope_scope_t *scope,
                                   const gscope_cgroup_limits_t *limits)
{
    if (!scope || !limits)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or limits");

    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    const char *path = scope->cgroup_path;

    write_cpu_limits(path, limits);
    write_memory_limits(path, limits);
    write_pids_limit(path, limits);
    write_io_limits(path, limits);

    GSCOPE_DEBUG(scope->ctx, "cgroup updated for scope %u", scope->id);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_add_pid(gscope_scope_t *scope, pid_t pid)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");
    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    char file[4096];
    char val[32];
    snprintf(file, sizeof(file), "%s/cgroup.procs", scope->cgroup_path);
    snprintf(val, sizeof(val), "%d", (int)pid);

    if (gscope_write_file(file, val) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_CGROUP,
                                      "failed to add PID %d to cgroup", (int)pid);

    GSCOPE_DEBUG(scope->ctx, "added PID %d to cgroup scope-%u",
                 (int)pid, scope->id);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_freeze(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");
    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    char file[4096];
    snprintf(file, sizeof(file), "%s/cgroup.freeze", scope->cgroup_path);

    if (gscope_write_file(file, "1") < 0)
        return gscope_set_error_errno(GSCOPE_ERR_CGROUP,
                                      "failed to freeze scope %u", scope->id);

    GSCOPE_INFO(scope->ctx, "scope %u frozen", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_thaw(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");
    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    char file[4096];
    snprintf(file, sizeof(file), "%s/cgroup.freeze", scope->cgroup_path);

    if (gscope_write_file(file, "0") < 0)
        return gscope_set_error_errno(GSCOPE_ERR_CGROUP,
                                      "failed to thaw scope %u", scope->id);

    GSCOPE_INFO(scope->ctx, "scope %u thawed", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_kill(gscope_scope_t *scope, int signal)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");
    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    int killed = kill_all_procs(scope->cgroup_path, signal);
    GSCOPE_DEBUG(scope->ctx, "sent signal %d to %d procs in scope %u",
                 signal, killed, scope->id);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_cgroup_delete(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");
    if (scope->cgroup_path[0] == '\0') {
        gscope_clear_error();
        return GSCOPE_OK;  /* Nothing to delete */
    }

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "deleting cgroup: %s", scope->cgroup_path);

    /* Kill all remaining processes */
    kill_all_procs(scope->cgroup_path, SIGKILL);

    /* Wait briefly for processes to exit */
    struct timespec ts = {0, 100000000};  /* 100ms */
    nanosleep(&ts, NULL);

    /* rmdir (cgroup dirs can only be removed when empty of processes) */
    if (rmdir(scope->cgroup_path) != 0 && errno != ENOENT) {
        /* Retry after another kill + wait */
        kill_all_procs(scope->cgroup_path, SIGKILL);
        ts.tv_nsec = 500000000;  /* 500ms */
        nanosleep(&ts, NULL);

        if (rmdir(scope->cgroup_path) != 0 && errno != ENOENT) {
            GSCOPE_WARN(ctx, "failed to remove cgroup dir %s: %s",
                        scope->cgroup_path, strerror(errno));
            /* Non-fatal — the kernel will clean up eventually */
        }
    }

    scope->cgroup_path[0] = '\0';

    GSCOPE_INFO(ctx, "cgroup deleted for scope %u", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}
