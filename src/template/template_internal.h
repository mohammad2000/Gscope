/*
 * template/template_internal.h — Shared internal types for template system
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_TEMPLATE_INTERNAL_H
#define GSCOPE_TEMPLATE_INTERNAL_H

#include "../internal.h"
#include <gscope/template.h>
#include <stdbool.h>
#include <sys/types.h>

/* ─── Internal Data Structures ───────────────────────────────────── */

typedef struct {
    char key[128];
    char value[4096];
} tmpl_var_t;

typedef struct {
    char  name[128];
    char  version[64];
    char  manager[16];     /* "apt", "pip", "npm", "cargo", "gem", "custom" */
    bool  required;
    char *pre_script;      /* Heap, nullable */
    char *post_script;     /* Heap, nullable */
} tmpl_package_t;

typedef struct {
    char     path[4096];
    char    *content;       /* Heap allocated */
    mode_t   mode;
    bool     is_template;   /* Apply variable substitution */
} tmpl_file_t;

typedef struct {
    char name[128];
    char command[4096];
    char file_path[4096];
    int  port;
} tmpl_verify_t;

struct gscope_template {
    char name[256];
    char version[64];
    char template_id[128];

    tmpl_var_t     *vars;
    int             var_count;
    int             var_capacity;

    tmpl_package_t *packages;
    int             pkg_count;

    tmpl_file_t    *files;
    int             file_count;

    tmpl_verify_t  *verifications;
    int             verify_count;

    char *pre_install_script;
    char *post_install_script;
    char *setup_script;
    char *startup_script;
    char *health_check_script;

    tmpl_var_t     *env;
    int             env_count;
    int             env_capacity;
};

/* ─── Variable Engine ────────────────────────────────────────────── */

const char *tmpl_var_get(const tmpl_var_t *vars, int count, const char *key);
int tmpl_var_set(tmpl_var_t **vars, int *count, int *capacity,
                  const char *key, const char *value);
int tmpl_vars_substitute(const char *input, char *output, size_t output_size,
                          const tmpl_var_t *vars, int var_count);
void tmpl_vars_add_builtins(tmpl_var_t **vars, int *count, int *capacity,
                             gscope_scope_t *scope);

/* ─── Command Execution ──────────────────────────────────────────── */

int tmpl_exec_in_scope(gscope_scope_t *scope,
                        const char *command,
                        const tmpl_var_t *env, int env_count,
                        int timeout_sec,
                        char *output, size_t output_size);

int tmpl_run_script_in_scope(gscope_scope_t *scope,
                              const char *script_name,
                              const char *script_content,
                              const tmpl_var_t *env, int env_count,
                              int timeout_sec);

/* ─── Package Manager ────────────────────────────────────────────── */

int tmpl_pkg_install(gscope_scope_t *scope,
                      const tmpl_package_t *packages, int count,
                      const tmpl_var_t *env, int env_count,
                      gscope_tmpl_progress_fn progress_fn, void *userdata,
                      int *installed_out);

/* ─── File Creation ──────────────────────────────────────────────── */

int tmpl_file_create(gscope_scope_t *scope,
                      const tmpl_file_t *files, int count,
                      const tmpl_var_t *vars, int var_count,
                      gscope_tmpl_progress_fn progress_fn, void *userdata,
                      int *created_out);

/* ─── Verification ───────────────────────────────────────────────── */

int tmpl_verify_run(gscope_scope_t *scope,
                     const tmpl_verify_t *checks, int count,
                     const tmpl_var_t *env, int env_count,
                     int *passed_out, int *failed_out);

#endif /* GSCOPE_TEMPLATE_INTERNAL_H */
