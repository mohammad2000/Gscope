/*
 * template/template_file.c — Create config files inside scope rootfs
 *
 * Supports variable substitution in file content and paths.
 * Writes atomically: .tmp → rename.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int tmpl_file_create(gscope_scope_t *scope,
                      const tmpl_file_t *files, int count,
                      const tmpl_var_t *vars, int var_count,
                      gscope_tmpl_progress_fn fn, void *ud,
                      int *created_out)
{
    int created = 0;

    for (int i = 0; i < count; i++) {
        /* Resolve path variables */
        char resolved_path[4096];
        tmpl_vars_substitute(files[i].path, resolved_path,
                              sizeof(resolved_path), vars, var_count);

        /* Build full path inside rootfs */
        char full_path[8192];
        snprintf(full_path, sizeof(full_path), "%s%s",
                 scope->rootfs_merged, resolved_path);

        /* Ensure parent directory exists */
        char *last_slash = strrchr(full_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            gscope_mkdir_p(full_path, 0755);
            *last_slash = '/';
        }

        /* Resolve content variables if template */
        const char *content = files[i].content;
        char *resolved_content = NULL;

        if (files[i].is_template && content) {
            size_t content_len = strlen(content);
            size_t buf_size = content_len * 2 + 4096;
            if (buf_size > 1024 * 1024) buf_size = 1024 * 1024; /* 1MB max */
            resolved_content = malloc(buf_size);
            if (resolved_content) {
                tmpl_vars_substitute(content, resolved_content,
                                      buf_size, vars, var_count);
                content = resolved_content;
            }
        }

        /* Progress */
        if (fn) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Creating file: %s", resolved_path);
            gscope_tmpl_progress_t p = {
                .phase = GSCOPE_TMPL_PHASE_FILES,
                .message = msg,
                .progress = count > 0 ? (i * 100 / count) : -1,
                .is_error = false,
            };
            fn(&p, ud);
        }

        /* Atomic write: .tmp → rename */
        char tmp_path[8192];
        snprintf(tmp_path, sizeof(tmp_path), "%s.gscope.tmp", full_path);

        int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                       files[i].mode ? files[i].mode : 0644);
        if (fd < 0) {
            free(resolved_content);
            continue;
        }

        if (content) {
            size_t len = strlen(content);
            write(fd, content, len);
            /* Ensure trailing newline */
            if (len > 0 && content[len - 1] != '\n')
                write(fd, "\n", 1);
        }

        close(fd);

        if (rename(tmp_path, full_path) != 0) {
            unlink(tmp_path);
            free(resolved_content);
            continue;
        }

        /* Set permissions */
        if (files[i].mode)
            chmod(full_path, files[i].mode);

        free(resolved_content);
        created++;
    }

    if (created_out) *created_out = created;
    return 0;
}
