/*
 * gscope/namespace.h — Namespace management API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_NAMESPACE_H
#define GSCOPE_NAMESPACE_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create namespaces for a scope.
 * ns_flags: GSCOPE_NS_* bitmask specifying which namespaces to create.
 */
gscope_err_t gscope_ns_create(gscope_scope_t *scope, uint32_t ns_flags);

/*
 * Enter (setns) a specific namespace of a scope.
 * ns_type: a single GSCOPE_NS_* flag.
 * Affects the calling thread.
 */
gscope_err_t gscope_ns_enter(gscope_scope_t *scope, uint32_t ns_type);

/*
 * Get the file descriptor for a scope's namespace.
 * ns_type: a single GSCOPE_NS_* flag.
 * Returns fd >= 0 on success, -1 if not available.
 */
int gscope_ns_fd(gscope_scope_t *scope, uint32_t ns_type);

/*
 * Configure UID mapping for user namespace.
 * Writes to /proc/<pid>/uid_map.
 */
gscope_err_t gscope_ns_map_uid(gscope_scope_t *scope,
                                uid_t host_uid, uid_t container_uid,
                                uint32_t count);

/*
 * Configure GID mapping for user namespace.
 * Writes "deny" to setgroups, then writes /proc/<pid>/gid_map.
 */
gscope_err_t gscope_ns_map_gid(gscope_scope_t *scope,
                                gid_t host_gid, gid_t container_gid,
                                uint32_t count);

/*
 * Verify that a namespace is functional.
 * ns_type: a single GSCOPE_NS_* flag.
 */
bool gscope_ns_verify(gscope_scope_t *scope, uint32_t ns_type);

/*
 * Delete a scope's network namespace by name.
 */
gscope_err_t gscope_ns_delete(gscope_scope_t *scope);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_NAMESPACE_H */
