/*
 * gscope/cgroup.h — Cgroup v2 management API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_CGROUP_H
#define GSCOPE_CGROUP_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

gscope_err_t gscope_cgroup_create(gscope_scope_t *scope,
                                   const gscope_cgroup_limits_t *limits);
gscope_err_t gscope_cgroup_update(gscope_scope_t *scope,
                                   const gscope_cgroup_limits_t *limits);
gscope_err_t gscope_cgroup_add_pid(gscope_scope_t *scope, pid_t pid);
gscope_err_t gscope_cgroup_stats(gscope_scope_t *scope,
                                  gscope_cgroup_stats_t *stats);
gscope_err_t gscope_cgroup_freeze(gscope_scope_t *scope);
gscope_err_t gscope_cgroup_thaw(gscope_scope_t *scope);
gscope_err_t gscope_cgroup_kill(gscope_scope_t *scope, int signal);
gscope_err_t gscope_cgroup_delete(gscope_scope_t *scope);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_CGROUP_H */
