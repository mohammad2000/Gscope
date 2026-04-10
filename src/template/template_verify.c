/*
 * template/template_verify.c — Verification engine
 *
 * Checks: command exit codes, file existence, port listening
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"

#include <stdio.h>
#include <string.h>

int tmpl_verify_run(gscope_scope_t *scope,
                     const tmpl_verify_t *checks, int count,
                     const tmpl_var_t *env, int env_count,
                     int *passed_out, int *failed_out)
{
    int passed = 0, failed = 0;

    for (int i = 0; i < count; i++) {
        bool ok = false;

        if (checks[i].command[0]) {
            /* Command verification */
            int ret = tmpl_exec_in_scope(scope, checks[i].command,
                                          env, env_count, 30, NULL, 0);
            ok = (ret == 0);
        } else if (checks[i].file_path[0]) {
            /* File existence verification */
            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "test -e '%s'", checks[i].file_path);
            int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                          10, NULL, 0);
            ok = (ret == 0);
        } else if (checks[i].port > 0) {
            /* Port listening verification */
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "ss -tlnp | grep -q ':%d ' || netstat -tlnp | grep -q ':%d '",
                     checks[i].port, checks[i].port);
            int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                          10, NULL, 0);
            ok = (ret == 0);
        }

        if (ok) passed++;
        else failed++;
    }

    if (passed_out) *passed_out = passed;
    if (failed_out) *failed_out = failed;

    return 0;
}
