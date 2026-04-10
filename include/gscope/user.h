/*
 * gscope/user.h — User management API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_USER_H
#define GSCOPE_USER_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

gscope_err_t gscope_user_create(gscope_scope_t *scope,
                                 const char *username,
                                 uid_t uid, gid_t gid,
                                 gscope_privilege_t privilege);

gscope_err_t gscope_user_configure_sudo(gscope_scope_t *scope,
                                         const char *username,
                                         gscope_privilege_t privilege);

gscope_err_t gscope_user_info(gscope_scope_t *scope,
                               const char *username,
                               gscope_user_info_t *info);

gscope_err_t gscope_user_delete(gscope_scope_t *scope,
                                 const char *username);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_USER_H */
