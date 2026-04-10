/*
 * template/template.c — Main template executor orchestrator
 *
 * Parses JSON template configs and executes them in 9 phases:
 *   PREFLIGHT → VARIABLES → PRE_INSTALL → PACKAGES →
 *   POST_INSTALL → FILES → SETUP → VERIFICATION → COMPLETE
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ─── JSON Parsing ───────────────────────────────────────────────── */

static char *json_get_string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsString(item)) return NULL;
    return item->valuestring;
}

static char *json_strdup(cJSON *obj, const char *key)
{
    const char *s = json_get_string(obj, key);
    return s ? strdup(s) : NULL;
}

static int parse_variables(cJSON *vars_obj, tmpl_var_t **vars,
                            int *count, int *capacity)
{
    if (!vars_obj || !cJSON_IsObject(vars_obj)) return 0;

    cJSON *item;
    cJSON_ArrayForEach(item, vars_obj) {
        if (cJSON_IsString(item)) {
            tmpl_var_set(vars, count, capacity,
                         item->string, item->valuestring);
        } else if (cJSON_IsNumber(item)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", item->valuedouble);
            tmpl_var_set(vars, count, capacity, item->string, buf);
        } else if (cJSON_IsBool(item)) {
            tmpl_var_set(vars, count, capacity,
                         item->string,
                         cJSON_IsTrue(item) ? "true" : "false");
        }
    }
    return 0;
}

static int parse_packages(cJSON *pkgs_arr, tmpl_package_t **pkgs, int *count)
{
    if (!pkgs_arr || !cJSON_IsArray(pkgs_arr)) return 0;

    int n = cJSON_GetArraySize(pkgs_arr);
    *pkgs = calloc((size_t)n, sizeof(tmpl_package_t));
    if (!*pkgs) return -1;

    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, pkgs_arr) {
        if (!cJSON_IsObject(item)) continue;

        const char *name = json_get_string(item, "name");
        if (!name) continue;

        gscope_strlcpy((*pkgs)[idx].name, name, sizeof((*pkgs)[idx].name));

        const char *ver = json_get_string(item, "version");
        if (ver) gscope_strlcpy((*pkgs)[idx].version, ver, sizeof((*pkgs)[idx].version));

        const char *mgr = json_get_string(item, "manager");
        gscope_strlcpy((*pkgs)[idx].manager, mgr ? mgr : "apt",
                        sizeof((*pkgs)[idx].manager));

        cJSON *req = cJSON_GetObjectItemCaseSensitive(item, "required");
        (*pkgs)[idx].required = req ? cJSON_IsTrue(req) : true;

        (*pkgs)[idx].pre_script = json_strdup(item, "pre_install_script");
        (*pkgs)[idx].post_script = json_strdup(item, "post_install_script");

        idx++;
    }

    *count = idx;
    return 0;
}

static int parse_files(cJSON *files_arr, tmpl_file_t **files, int *count)
{
    if (!files_arr || !cJSON_IsArray(files_arr)) return 0;

    int n = cJSON_GetArraySize(files_arr);
    *files = calloc((size_t)n, sizeof(tmpl_file_t));
    if (!*files) return -1;

    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, files_arr) {
        if (!cJSON_IsObject(item)) continue;

        const char *path = json_get_string(item, "path");
        if (!path) continue;

        gscope_strlcpy((*files)[idx].path, path, sizeof((*files)[idx].path));

        const char *content = json_get_string(item, "content");
        (*files)[idx].content = content ? strdup(content) : NULL;

        const char *mode_str = json_get_string(item, "mode");
        (*files)[idx].mode = mode_str ? (mode_t)strtol(mode_str, NULL, 8) : 0644;

        const char *type = json_get_string(item, "type");
        (*files)[idx].is_template = (!type || strcmp(type, "template") == 0);

        idx++;
    }

    *count = idx;
    return 0;
}

static int parse_verification(cJSON *ver_obj, tmpl_verify_t **checks, int *count)
{
    if (!ver_obj || !cJSON_IsObject(ver_obj)) return 0;

    int total = 0;
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(ver_obj, "commands");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(ver_obj, "files");
    cJSON *ports = cJSON_GetObjectItemCaseSensitive(ver_obj, "ports");

    if (cmds) total += cJSON_GetArraySize(cmds);
    if (files) total += cJSON_GetArraySize(files);
    if (ports) total += cJSON_GetArraySize(ports);

    if (total == 0) return 0;

    *checks = calloc((size_t)total, sizeof(tmpl_verify_t));
    if (!*checks) return -1;

    int idx = 0;
    cJSON *item;

    if (cmds) {
        cJSON_ArrayForEach(item, cmds) {
            const char *name = json_get_string(item, "name");
            const char *cmd = json_get_string(item, "command");
            if (name) gscope_strlcpy((*checks)[idx].name, name, 128);
            if (cmd) gscope_strlcpy((*checks)[idx].command, cmd, 4096);
            idx++;
        }
    }

    if (files) {
        cJSON_ArrayForEach(item, files) {
            const char *path = json_get_string(item, "path");
            if (path) {
                snprintf((*checks)[idx].name, 128, "file:%s", path);
                gscope_strlcpy((*checks)[idx].file_path, path, 4096);
            }
            idx++;
        }
    }

    if (ports) {
        cJSON_ArrayForEach(item, ports) {
            cJSON *port = cJSON_GetObjectItemCaseSensitive(item, "port");
            if (port && cJSON_IsNumber(port)) {
                snprintf((*checks)[idx].name, 128, "port:%d", port->valueint);
                (*checks)[idx].port = port->valueint;
            }
            idx++;
        }
    }

    *count = idx;
    return 0;
}

/* ─── Public: Parse ──────────────────────────────────────────────── */

gscope_err_t gscope_template_parse(const char *json,
                                    gscope_template_t **out)
{
    if (!json || !out)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL json or out");

    cJSON *root = cJSON_Parse(json);
    if (!root)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "JSON parse error near: %.30s",
                                cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");

    gscope_template_t *tmpl = calloc(1, sizeof(*tmpl));
    if (!tmpl) {
        cJSON_Delete(root);
        return gscope_set_error(GSCOPE_ERR_NOMEM, "alloc failed");
    }

    /* Identity */
    const char *s;
    if ((s = json_get_string(root, "name")) || (s = json_get_string(root, "template_name")))
        gscope_strlcpy(tmpl->name, s, sizeof(tmpl->name));
    if ((s = json_get_string(root, "version")) || (s = json_get_string(root, "template_version")))
        gscope_strlcpy(tmpl->version, s, sizeof(tmpl->version));
    if ((s = json_get_string(root, "template_id")))
        gscope_strlcpy(tmpl->template_id, s, sizeof(tmpl->template_id));

    /* Variables */
    cJSON *vars = cJSON_GetObjectItemCaseSensitive(root, "variables");
    if (!vars) vars = cJSON_GetObjectItemCaseSensitive(root, "template_variables");
    parse_variables(vars, &tmpl->vars, &tmpl->var_count, &tmpl->var_capacity);

    /* Environment */
    cJSON *env = cJSON_GetObjectItemCaseSensitive(root, "environment");
    parse_variables(env, &tmpl->env, &tmpl->env_count, &tmpl->env_capacity);

    /* Packages */
    cJSON *pkgs = cJSON_GetObjectItemCaseSensitive(root, "packages");
    parse_packages(pkgs, &tmpl->packages, &tmpl->pkg_count);

    /* Files */
    cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    parse_files(files, &tmpl->files, &tmpl->file_count);

    /* Scripts */
    tmpl->pre_install_script = json_strdup(root, "pre_install_script");
    tmpl->post_install_script = json_strdup(root, "post_install_script");
    tmpl->setup_script = json_strdup(root, "setup_script");
    tmpl->startup_script = json_strdup(root, "startup_script");
    tmpl->health_check_script = json_strdup(root, "health_check_script");

    /* Verification */
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "verification");
    parse_verification(ver, &tmpl->verifications, &tmpl->verify_count);

    cJSON_Delete(root);

    *out = tmpl;
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_template_parse_file(const char *path,
                                         gscope_template_t **out)
{
    if (!path || !out)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL path or out");

    /* Read file */
    struct stat st;
    if (stat(path, &st) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_NOENT,
                                      "template file not found: %s", path);

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf)
        return gscope_set_error(GSCOPE_ERR_NOMEM, "alloc failed");

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(buf); return gscope_set_error_errno(GSCOPE_ERR_IO, "open %s", path); }

    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);

    if (n < 0) { free(buf); return gscope_set_error_errno(GSCOPE_ERR_IO, "read %s", path); }
    buf[n] = '\0';

    gscope_err_t err = gscope_template_parse(buf, out);
    free(buf);
    return err;
}

void gscope_template_free(gscope_template_t *tmpl)
{
    if (!tmpl) return;

    free(tmpl->vars);
    free(tmpl->env);

    for (int i = 0; i < tmpl->pkg_count; i++) {
        free(tmpl->packages[i].pre_script);
        free(tmpl->packages[i].post_script);
    }
    free(tmpl->packages);

    for (int i = 0; i < tmpl->file_count; i++)
        free(tmpl->files[i].content);
    free(tmpl->files);

    free(tmpl->verifications);

    free(tmpl->pre_install_script);
    free(tmpl->post_install_script);
    free(tmpl->setup_script);
    free(tmpl->startup_script);
    free(tmpl->health_check_script);

    free(tmpl);
}

gscope_err_t gscope_template_set_var(gscope_template_t *tmpl,
                                      const char *key, const char *value)
{
    if (!tmpl || !key)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL tmpl or key");
    if (tmpl_var_set(&tmpl->vars, &tmpl->var_count, &tmpl->var_capacity,
                      key, value ? value : "") < 0)
        return gscope_set_error(GSCOPE_ERR_NOMEM, "var alloc failed");
    gscope_clear_error();
    return GSCOPE_OK;
}

const char *gscope_template_get_var(const gscope_template_t *tmpl,
                                     const char *key)
{
    if (!tmpl || !key) return NULL;
    return tmpl_var_get(tmpl->vars, tmpl->var_count, key);
}

/* ─── Progress Helper ────────────────────────────────────────────── */

static void progress(gscope_tmpl_progress_fn fn, void *ud,
                      gscope_tmpl_phase_t phase,
                      const char *msg, int pct, bool err)
{
    if (!fn) return;
    gscope_tmpl_progress_t p = {
        .phase = phase, .message = msg,
        .progress = pct, .is_error = err,
    };
    fn(&p, ud);
}

/* ─── Main Executor ──────────────────────────────────────────────── */

gscope_err_t gscope_template_execute(gscope_scope_t *scope,
                                      const gscope_template_t *tmpl,
                                      gscope_tmpl_progress_fn progress_fn,
                                      void *userdata,
                                      gscope_tmpl_result_t *result)
{
    if (!scope || !tmpl || !result)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope, tmpl, or result");

    memset(result, 0, sizeof(*result));
    gscope_ctx_t *ctx = scope->ctx;

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    GSCOPE_INFO(ctx, "template execute: '%s' v%s on scope %u",
                tmpl->name, tmpl->version, scope->id);

    /* Merge variables: copy template vars + add builtins */
    int var_count = 0, var_cap = 0;
    tmpl_var_t *vars = NULL;

    /* Copy template variables */
    for (int i = 0; i < tmpl->var_count; i++)
        tmpl_var_set(&vars, &var_count, &var_cap,
                      tmpl->vars[i].key, tmpl->vars[i].value);

    /* Add built-in variables */
    tmpl_vars_add_builtins(&vars, &var_count, &var_cap, scope);

    /* Merge template environment with scope env */
    int env_count = 0, env_cap = 0;
    tmpl_var_t *env = NULL;
    for (int i = 0; i < tmpl->env_count; i++)
        tmpl_var_set(&env, &env_count, &env_cap,
                      tmpl->env[i].key, tmpl->env[i].value);

    int scripts_run = 0;

    /* ═══ Phase 1: PREFLIGHT ═══ */
    progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_PREFLIGHT,
             "Running preflight checks...", 0, false);

    if (scope->rootfs_merged[0] == '\0') {
        snprintf(result->error, sizeof(result->error), "rootfs not set up");
        result->success = false;
        goto cleanup;
    }

    /* Verify we can execute commands in scope */
    char test_out[64];
    int test_ret = tmpl_exec_in_scope(scope, "echo ok",
                                       env, env_count, 10,
                                       test_out, sizeof(test_out));
    if (test_ret != 0) {
        snprintf(result->error, sizeof(result->error),
                 "Cannot execute commands in scope (is init running?)");
        result->success = false;
        goto cleanup;
    }

    progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_PREFLIGHT,
             "Preflight checks passed", 5, false);

    /* ═══ Phase 2: VARIABLES ═══ */
    progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_VARIABLES,
             "Setting up variables...", 8, false);

    GSCOPE_DEBUG(ctx, "  template has %d variables, %d packages, %d files",
                 var_count, tmpl->pkg_count, tmpl->file_count);

    /* ═══ Phase 3: PRE_INSTALL ═══ */
    if (tmpl->pre_install_script && tmpl->pre_install_script[0]) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_PRE_INSTALL,
                 "Running pre-install script...", 10, false);

        /* Substitute variables in script */
        char resolved[32768];
        tmpl_vars_substitute(tmpl->pre_install_script, resolved,
                              sizeof(resolved), vars, var_count);

        int ret = tmpl_run_script_in_scope(scope, "pre_install", resolved,
                                            env, env_count, 300);
        if (ret != 0) {
            snprintf(result->error, sizeof(result->error),
                     "Pre-install script failed (exit code %d)", ret);
            result->success = false;
            goto cleanup;
        }
        scripts_run++;
    }

    /* ═══ Phase 4: PACKAGES ═══ */
    if (tmpl->pkg_count > 0) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_PACKAGES,
                 "Installing packages...", 15, false);

        int installed = 0;
        int ret = tmpl_pkg_install(scope, tmpl->packages, tmpl->pkg_count,
                                    env, env_count,
                                    progress_fn, userdata, &installed);
        result->packages_installed = installed;

        if (ret < 0) {
            snprintf(result->error, sizeof(result->error),
                     "Required package installation failed");
            result->success = false;
            goto cleanup;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "%d package(s) installed", installed);
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_PACKAGES,
                 msg, 55, false);
    }

    /* ═══ Phase 5: POST_INSTALL ═══ */
    if (tmpl->post_install_script && tmpl->post_install_script[0]) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_POST_INSTALL,
                 "Running post-install script...", 60, false);

        char resolved[32768];
        tmpl_vars_substitute(tmpl->post_install_script, resolved,
                              sizeof(resolved), vars, var_count);

        tmpl_run_script_in_scope(scope, "post_install", resolved,
                                  env, env_count, 300);
        scripts_run++;
    }

    /* ═══ Phase 6: FILES ═══ */
    if (tmpl->file_count > 0) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_FILES,
                 "Creating configuration files...", 65, false);

        int created = 0;
        tmpl_file_create(scope, tmpl->files, tmpl->file_count,
                          vars, var_count,
                          progress_fn, userdata, &created);
        result->files_created = created;

        char msg[128];
        snprintf(msg, sizeof(msg), "%d file(s) created", created);
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_FILES,
                 msg, 75, false);
    }

    /* ═══ Phase 7: SETUP ═══ */
    if (tmpl->setup_script && tmpl->setup_script[0]) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_SETUP,
                 "Running setup script...", 78, false);

        char resolved[32768];
        tmpl_vars_substitute(tmpl->setup_script, resolved,
                              sizeof(resolved), vars, var_count);

        int ret = tmpl_run_script_in_scope(scope, "setup", resolved,
                                            env, env_count, 600);
        if (ret != 0) {
            snprintf(result->error, sizeof(result->error),
                     "Setup script failed (exit code %d)", ret);
            result->success = false;
            goto cleanup;
        }
        scripts_run++;
    }

    /* Save startup script for later use */
    if (tmpl->startup_script && tmpl->startup_script[0]) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/opt/gritiva/startup.sh",
                 scope->rootfs_merged);
        gscope_mkdir_p(path, 0755);
        /* Remove directory and write file */
        rmdir(path);

        char resolved[32768];
        tmpl_vars_substitute(tmpl->startup_script, resolved,
                              sizeof(resolved), vars, var_count);

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd >= 0) {
            char header[] = "#!/bin/bash\nset -e\n\n";
            write(fd, header, strlen(header));
            write(fd, resolved, strlen(resolved));
            close(fd);
        }
    }

    /* Save health check script */
    if (tmpl->health_check_script && tmpl->health_check_script[0]) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/opt/gritiva/health_check.sh",
                 scope->rootfs_merged);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd >= 0) {
            char header[] = "#!/bin/bash\n";
            write(fd, header, strlen(header));
            write(fd, tmpl->health_check_script,
                   strlen(tmpl->health_check_script));
            close(fd);
        }
    }

    /* ═══ Phase 8: VERIFICATION ═══ */
    if (tmpl->verify_count > 0) {
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_VERIFICATION,
                 "Running verification checks...", 90, false);

        tmpl_verify_run(scope, tmpl->verifications, tmpl->verify_count,
                         env, env_count,
                         &result->verifications_passed,
                         &result->verifications_failed);

        char msg[128];
        snprintf(msg, sizeof(msg), "%d/%d verifications passed",
                 result->verifications_passed,
                 result->verifications_passed + result->verifications_failed);
        progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_VERIFICATION,
                 msg, 95, result->verifications_failed > 0);
    }

    /* ═══ Phase 9: COMPLETE ═══ */
    result->success = true;
    result->scripts_executed = scripts_run;

    progress(progress_fn, userdata, GSCOPE_TMPL_PHASE_COMPLETE,
             "Template execution complete!", 100, false);

cleanup:
    /* Calculate duration */
    {
        struct timespec end_ts;
        clock_gettime(CLOCK_MONOTONIC, &end_ts);
        result->duration_sec = (float)(end_ts.tv_sec - start_ts.tv_sec) +
                               (float)(end_ts.tv_nsec - start_ts.tv_nsec) / 1e9f;
    }

    free(vars);
    free(env);

    GSCOPE_INFO(ctx, "template '%s' %s in %.1fs (%d pkgs, %d files)",
                tmpl->name, result->success ? "succeeded" : "FAILED",
                result->duration_sec,
                result->packages_installed,
                result->files_created);

    gscope_clear_error();
    return result->success ? GSCOPE_OK : GSCOPE_ERR_PROCESS;
}
