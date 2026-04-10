/*
 * gscope/network.h — Networking API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_NETWORK_H
#define GSCOPE_NETWORK_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

gscope_err_t gscope_net_setup(gscope_scope_t *scope, gscope_netmode_t mode);
gscope_err_t gscope_net_info(gscope_scope_t *scope, gscope_net_info_t *info);
gscope_err_t gscope_net_port_add(gscope_scope_t *scope,
                                  const gscope_port_map_t *map);
gscope_err_t gscope_net_port_remove(gscope_scope_t *scope,
                                     const gscope_port_map_t *map);
gscope_err_t gscope_net_stats(gscope_scope_t *scope, gscope_net_stats_t *stats);
gscope_err_t gscope_net_teardown(gscope_scope_t *scope);
gscope_err_t gscope_net_ensure_bridge(gscope_ctx_t *ctx,
                                       const char *bridge_name);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_NETWORK_H */
