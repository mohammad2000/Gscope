/*
 * gscope/scope.h — Scope lifecycle API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_SCOPE_H
#define GSCOPE_SCOPE_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Library Lifecycle ──────────────────────────────────────────── */

/*
 * Initialize the gscope library context.
 *
 * Must be called before any other gscope function.
 * Detects kernel features, creates shared netlink socket,
 * initializes IP allocator.
 *
 * flags: GSCOPE_INIT_* bitmask
 *   GSCOPE_INIT_RESTORE  — scan state files and restore existing scopes
 *   GSCOPE_INIT_VERBOSE  — enable verbose init logging
 *
 * Returns GSCOPE_OK on success.
 */
gscope_err_t gscope_init(gscope_ctx_t **ctx, unsigned int flags);

/*
 * Destroy the library context and release all resources.
 * Does NOT stop or delete running scopes — caller must do that first.
 */
void gscope_destroy(gscope_ctx_t *ctx);

/*
 * Get library version string (e.g. "0.1.0").
 */
const char *gscope_version(void);

/* ─── Scope Lifecycle ────────────────────────────────────────────── */

/*
 * Create a new scope with the given configuration.
 *
 * Orchestration order:
 *   1. Create directory structure
 *   2. Create cgroup + set resource limits
 *   3. Mount OverlayFS rootfs
 *   4. Create network namespace
 *   5. Setup networking (bridge + veth + IP + route)
 *   6. Create user in rootfs
 *   7. Save state to disk
 *
 * On any failure, partially created resources are rolled back.
 * The scope is left in STOPPED state on success.
 *
 * Returns GSCOPE_OK on success, populates *scope.
 */
gscope_err_t gscope_scope_create(gscope_ctx_t *ctx,
                                  const gscope_config_t *config,
                                  gscope_scope_t **scope);

/*
 * Start a stopped scope by spawning its init process.
 *
 * If command is NULL, spawns "sleep infinity" as the init process.
 * The init process runs inside all configured namespaces with
 * security policies applied (seccomp, capabilities, pivot_root).
 *
 * Scope must be in STOPPED state.
 * Returns GSCOPE_OK on success, scope transitions to RUNNING.
 */
gscope_err_t gscope_scope_start(gscope_scope_t *scope, const char *init_command);

/*
 * Stop a running scope.
 *
 * Sends SIGTERM to the init process, waits up to timeout_sec seconds,
 * then sends SIGKILL if still alive. Also kills all processes in the
 * scope's cgroup.
 *
 * timeout_sec: seconds to wait after SIGTERM (0 = SIGKILL immediately)
 * Returns GSCOPE_OK when scope is fully stopped.
 */
gscope_err_t gscope_scope_stop(gscope_scope_t *scope, unsigned int timeout_sec);

/*
 * Delete a scope and release all its resources.
 *
 * Teardown order (reverse of creation):
 *   1. Stop if running
 *   2. Remove user from rootfs
 *   3. Teardown networking
 *   4. Delete network namespace
 *   5. Unmount OverlayFS
 *   6. Delete cgroup
 *   7. Remove directories
 *   8. Remove state file
 *
 * force: if true, proceeds even if some cleanup steps fail.
 * Returns GSCOPE_OK on success.
 */
gscope_err_t gscope_scope_delete(gscope_scope_t *scope, bool force);

/* ─── Query ──────────────────────────────────────────────────────── */

/*
 * Get the current status of a scope.
 */
gscope_err_t gscope_scope_status(gscope_scope_t *scope, gscope_status_t *status);

/*
 * Get real-time resource metrics for a scope.
 * Reads from cgroup stats and network counters.
 */
gscope_err_t gscope_scope_metrics(gscope_scope_t *scope, gscope_metrics_t *metrics);

/*
 * Look up a scope by ID within a context.
 * Returns NULL if not found (does not set error state).
 */
gscope_scope_t *gscope_scope_get(gscope_ctx_t *ctx, gscope_id_t id);

/*
 * List all scope IDs in this context.
 *
 * ids:       output array (caller-allocated)
 * max_count: size of ids array
 * Returns the number of scopes (may exceed max_count, meaning truncated).
 */
int gscope_scope_list(gscope_ctx_t *ctx, gscope_id_t *ids, int max_count);

/*
 * Get the scope's numeric ID.
 */
gscope_id_t gscope_scope_id(gscope_scope_t *scope);

/*
 * Get the scope's current state.
 */
gscope_state_t gscope_scope_state(gscope_scope_t *scope);

/* ─── Live Update ────────────────────────────────────────────────── */

/*
 * Update resource limits on a running or stopped scope.
 * Only the following fields from config are applied:
 *   cpu_cores, cpu_weight, memory_bytes, memory_swap_bytes,
 *   max_pids, io_weight
 *
 * Other fields are ignored (cannot change namespace, rootfs, etc. at runtime).
 */
gscope_err_t gscope_scope_update(gscope_scope_t *scope,
                                  const gscope_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_SCOPE_H */
