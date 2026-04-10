/*
 * cgroup/cgroup_stats.c — Read cgroup v2 statistics
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Parse cpu.stat ─────────────────────────────────────────────── */

static uint64_t parse_cpu_stat_usage(const char *cgroup_path)
{
    char file[4096];
    char buf[1024];

    snprintf(file, sizeof(file), "%s/cpu.stat", cgroup_path);
    if (gscope_read_file(file, buf, sizeof(buf)) < 0)
        return 0;

    /* Format: "usage_usec <value>\n..." */
    const char *p = strstr(buf, "usage_usec");
    if (!p) return 0;

    p += strlen("usage_usec");
    while (*p == ' ' || *p == '\t') p++;

    return (uint64_t)strtoull(p, NULL, 10);
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_cgroup_stats(gscope_scope_t *scope,
                                  gscope_cgroup_stats_t *stats)
{
    if (!scope || !stats)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or stats");

    if (scope->cgroup_path[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "cgroup not created");

    memset(stats, 0, sizeof(*stats));

    const char *path = scope->cgroup_path;
    char file[4096];

    /* CPU usage */
    stats->cpu_usage_us = parse_cpu_stat_usage(path);

    /* Memory current */
    snprintf(file, sizeof(file), "%s/memory.current", path);
    gscope_read_uint64(file, &stats->memory_current);

    /* Memory max */
    snprintf(file, sizeof(file), "%s/memory.max", path);
    gscope_read_uint64(file, &stats->memory_max);

    /* Memory swap */
    snprintf(file, sizeof(file), "%s/memory.swap.current", path);
    gscope_read_uint64(file, &stats->memory_swap);

    /* PIDs current */
    snprintf(file, sizeof(file), "%s/pids.current", path);
    gscope_read_uint32(file, &stats->pids_current);

    /* PIDs max */
    snprintf(file, sizeof(file), "%s/pids.max", path);
    gscope_read_uint32(file, &stats->pids_max);

    gscope_clear_error();
    return GSCOPE_OK;
}
