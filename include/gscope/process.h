/*
 * gscope/process.h — Process spawning and PTY API
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_PROCESS_H
#define GSCOPE_PROCESS_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execute a command inside a running scope.
 *
 * The process runs in all of the scope's namespaces with
 * security policies applied. If allocate_pty is true in config,
 * a PTY is created and result->pty_fd is the master fd.
 */
gscope_err_t gscope_exec(gscope_scope_t *scope,
                          const gscope_exec_config_t *config,
                          gscope_exec_result_t *result);

gscope_err_t gscope_process_signal(gscope_exec_result_t *result, int sig);
gscope_err_t gscope_process_wait(gscope_exec_result_t *result,
                                  int *exit_status, int timeout_ms);
gscope_err_t gscope_process_resize_pty(gscope_exec_result_t *result,
                                        uint16_t rows, uint16_t cols);

/*
 * Release resources associated with an exec result.
 * Closes pidfd and pty_fd if open.
 */
void gscope_process_release(gscope_exec_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_PROCESS_H */
