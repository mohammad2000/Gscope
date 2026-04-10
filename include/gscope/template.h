/*
 * gscope/template.h — Template executor API
 *
 * Executes template configs inside scopes: installs packages,
 * creates config files with variable substitution, runs scripts,
 * and verifies the result.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_TEMPLATE_H
#define GSCOPE_TEMPLATE_H

#include <gscope/types.h>
#include <gscope/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Opaque Template Handle ─────────────────────────────────────── */

typedef struct gscope_template gscope_template_t;

/* ─── Parse ──────────────────────────────────────────────────────── */

/*
 * Parse a template from a JSON string.
 *
 * JSON format:
 * {
 *   "name": "PostgreSQL",
 *   "version": "1.0",
 *   "variables": { "db_user": "postgres", ... },
 *   "environment": { "PGDATA": "/var/lib/postgresql/data" },
 *   "packages": [
 *     { "name": "postgresql", "manager": "apt", "required": true }
 *   ],
 *   "files": [
 *     { "path": "/etc/config.conf", "content": "key=${var}", "mode": "0644" }
 *   ],
 *   "pre_install_script": "#!/bin/bash\n...",
 *   "post_install_script": "...",
 *   "setup_script": "...",
 *   "startup_script": "...",
 *   "health_check_script": "...",
 *   "verification": {
 *     "commands": [{"name": "check", "command": "pg_isready"}],
 *     "files": [{"path": "/var/lib/postgresql/data"}],
 *     "ports": [{"port": 5432}]
 *   }
 * }
 */
gscope_err_t gscope_template_parse(const char *json,
                                    gscope_template_t **tmpl);

/* Parse from a JSON file path */
gscope_err_t gscope_template_parse_file(const char *path,
                                         gscope_template_t **tmpl);

/* Free a parsed template */
void gscope_template_free(gscope_template_t *tmpl);

/* ─── Variable Management ────────────────────────────────────────── */

/* Set or override a variable value */
gscope_err_t gscope_template_set_var(gscope_template_t *tmpl,
                                      const char *key, const char *value);

/* Get a variable value (NULL if not set) */
const char *gscope_template_get_var(const gscope_template_t *tmpl,
                                     const char *key);

/* ─── Execution Phases ───────────────────────────────────────────── */

typedef enum {
    GSCOPE_TMPL_PHASE_PREFLIGHT    = 0,
    GSCOPE_TMPL_PHASE_VARIABLES    = 1,
    GSCOPE_TMPL_PHASE_PRE_INSTALL  = 2,
    GSCOPE_TMPL_PHASE_PACKAGES     = 3,
    GSCOPE_TMPL_PHASE_POST_INSTALL = 4,
    GSCOPE_TMPL_PHASE_FILES        = 5,
    GSCOPE_TMPL_PHASE_SETUP        = 6,
    GSCOPE_TMPL_PHASE_VERIFICATION = 7,
    GSCOPE_TMPL_PHASE_COMPLETE     = 8,
} gscope_tmpl_phase_t;

/* ─── Progress Callback ──────────────────────────────────────────── */

typedef struct {
    gscope_tmpl_phase_t phase;
    const char         *message;
    int                 progress;    /* 0-100, -1 = unknown */
    bool                is_error;
} gscope_tmpl_progress_t;

/*
 * Progress callback function.
 * Called during template execution to report status.
 * The callback should return quickly (non-blocking).
 */
typedef void (*gscope_tmpl_progress_fn)(const gscope_tmpl_progress_t *progress,
                                         void *userdata);

/* ─── Execution Result ───────────────────────────────────────────── */

typedef struct {
    bool    success;
    float   duration_sec;
    int     packages_installed;
    int     files_created;
    int     scripts_executed;
    int     verifications_passed;
    int     verifications_failed;
    char    error[512];
} gscope_tmpl_result_t;

/* ─── Execute ────────────────────────────────────────────────────── */

/*
 * Execute a template on a running scope.
 *
 * The scope MUST be started (init process running) before calling this.
 * The template will install packages, create files, and run scripts
 * inside the scope's isolated environment.
 *
 * progress_fn: optional callback for progress reporting (can be NULL)
 * userdata:    passed to progress_fn
 * result:      output — filled with execution results
 *
 * Returns GSCOPE_OK on success (even if some non-required packages fail).
 * Returns error if a required operation fails.
 */
gscope_err_t gscope_template_execute(gscope_scope_t *scope,
                                      const gscope_template_t *tmpl,
                                      gscope_tmpl_progress_fn progress_fn,
                                      void *userdata,
                                      gscope_tmpl_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_TEMPLATE_H */
