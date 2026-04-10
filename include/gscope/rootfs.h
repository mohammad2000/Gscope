/*
 * gscope/rootfs.h — Rootfs and OverlayFS API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_ROOTFS_H
#define GSCOPE_ROOTFS_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

gscope_err_t gscope_rootfs_setup(gscope_scope_t *scope,
                                  const char *template_path);
gscope_err_t gscope_rootfs_info(gscope_scope_t *scope,
                                 gscope_rootfs_info_t *info);
gscope_err_t gscope_rootfs_teardown(gscope_scope_t *scope);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_ROOTFS_H */
