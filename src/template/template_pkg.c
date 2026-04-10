/*
 * template/template_pkg.c — Package manager handlers
 *
 * Supports: apt, pip, npm, cargo, gem
 * Each manager has: update, install (batch or individual), verify
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"

#include <stdio.h>
#include <string.h>

/* ─── Timeouts (seconds) ─────────────────────────────────────────── */

#define TIMEOUT_UPDATE    300   /* 5 min for apt-get update */
#define TIMEOUT_INSTALL   600   /* 10 min per package */
#define TIMEOUT_BATCH    1800   /* 30 min for batch install */
#define TIMEOUT_VERIFY     30

/* ─── Progress Helper ────────────────────────────────────────────── */

static void report(gscope_tmpl_progress_fn fn, void *ud,
                    const char *msg, int progress, bool is_err)
{
    if (!fn) return;
    gscope_tmpl_progress_t p = {
        .phase = GSCOPE_TMPL_PHASE_PACKAGES,
        .message = msg,
        .progress = progress,
        .is_error = is_err,
    };
    fn(&p, ud);
}

/* ─── APT ────────────────────────────────────────────────────────── */

static int install_apt(gscope_scope_t *scope,
                        const tmpl_package_t *pkgs, int count,
                        const tmpl_var_t *env, int env_count,
                        gscope_tmpl_progress_fn fn, void *ud,
                        int *installed)
{
    /* apt-get update first */
    report(fn, ud, "Updating package lists (apt)...", 5, false);
    tmpl_exec_in_scope(scope, "apt-get update -qq",
                        env, env_count, TIMEOUT_UPDATE, NULL, 0);

    /* Build batch install command */
    char cmd[8192] = "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends";
    int batch_count = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(pkgs[i].manager, "apt") != 0) continue;

        char pkg[256];
        if (pkgs[i].version[0])
            snprintf(pkg, sizeof(pkg), " %s=%s", pkgs[i].name, pkgs[i].version);
        else
            snprintf(pkg, sizeof(pkg), " %s", pkgs[i].name);

        if (strlen(cmd) + strlen(pkg) < sizeof(cmd) - 1) {
            strcat(cmd, pkg);
            batch_count++;
        }
    }

    if (batch_count == 0) return 0;

    char msg[256];
    snprintf(msg, sizeof(msg), "Installing %d apt package(s)...", batch_count);
    report(fn, ud, msg, 20, false);

    int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                  TIMEOUT_BATCH, NULL, 0);
    if (ret == 0) {
        *installed += batch_count;
        report(fn, ud, "APT packages installed successfully", 50, false);
    } else {
        /* Batch failed — try one by one */
        report(fn, ud, "Batch install failed, trying individually...", 25, false);

        for (int i = 0; i < count; i++) {
            if (strcmp(pkgs[i].manager, "apt") != 0) continue;

            char single[512];
            if (pkgs[i].version[0])
                snprintf(single, sizeof(single),
                         "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq %s=%s",
                         pkgs[i].name, pkgs[i].version);
            else
                snprintf(single, sizeof(single),
                         "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq %s",
                         pkgs[i].name);

            snprintf(msg, sizeof(msg), "Installing %s...", pkgs[i].name);
            report(fn, ud, msg, -1, false);

            ret = tmpl_exec_in_scope(scope, single, env, env_count,
                                      TIMEOUT_INSTALL, NULL, 0);
            if (ret == 0) {
                (*installed)++;
            } else if (pkgs[i].required) {
                snprintf(msg, sizeof(msg), "Required package %s failed", pkgs[i].name);
                report(fn, ud, msg, -1, true);
                return -1;
            }
        }
    }

    return 0;
}

/* ─── PIP ────────────────────────────────────────────────────────── */

static int install_pip(gscope_scope_t *scope,
                        const tmpl_package_t *pkgs, int count,
                        const tmpl_var_t *env, int env_count,
                        gscope_tmpl_progress_fn fn, void *ud,
                        int *installed)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(pkgs[i].manager, "pip") != 0 &&
            strcmp(pkgs[i].manager, "pip3") != 0) continue;

        char cmd[512];
        if (pkgs[i].version[0])
            snprintf(cmd, sizeof(cmd), "pip3 install -q '%s==%s'",
                     pkgs[i].name, pkgs[i].version);
        else
            snprintf(cmd, sizeof(cmd), "pip3 install -q '%s'", pkgs[i].name);

        char msg[256];
        snprintf(msg, sizeof(msg), "pip: Installing %s...", pkgs[i].name);
        report(fn, ud, msg, -1, false);

        int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                      TIMEOUT_INSTALL, NULL, 0);
        if (ret == 0) (*installed)++;
        else if (pkgs[i].required) return -1;
    }
    return 0;
}

/* ─── NPM ────────────────────────────────────────────────────────── */

static int install_npm(gscope_scope_t *scope,
                        const tmpl_package_t *pkgs, int count,
                        const tmpl_var_t *env, int env_count,
                        gscope_tmpl_progress_fn fn, void *ud,
                        int *installed)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(pkgs[i].manager, "npm") != 0) continue;

        char cmd[512];
        if (pkgs[i].version[0])
            snprintf(cmd, sizeof(cmd), "npm install -g '%s@%s'",
                     pkgs[i].name, pkgs[i].version);
        else
            snprintf(cmd, sizeof(cmd), "npm install -g '%s'", pkgs[i].name);

        char msg[256];
        snprintf(msg, sizeof(msg), "npm: Installing %s...", pkgs[i].name);
        report(fn, ud, msg, -1, false);

        int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                      TIMEOUT_INSTALL, NULL, 0);
        if (ret == 0) (*installed)++;
        else if (pkgs[i].required) return -1;
    }
    return 0;
}

/* ─── CARGO ──────────────────────────────────────────────────────── */

static int install_cargo(gscope_scope_t *scope,
                          const tmpl_package_t *pkgs, int count,
                          const tmpl_var_t *env, int env_count,
                          gscope_tmpl_progress_fn fn, void *ud,
                          int *installed)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(pkgs[i].manager, "cargo") != 0) continue;

        char cmd[512];
        if (pkgs[i].version[0])
            snprintf(cmd, sizeof(cmd), "cargo install '%s' --version '%s'",
                     pkgs[i].name, pkgs[i].version);
        else
            snprintf(cmd, sizeof(cmd), "cargo install '%s'", pkgs[i].name);

        char msg[256];
        snprintf(msg, sizeof(msg), "cargo: Installing %s...", pkgs[i].name);
        report(fn, ud, msg, -1, false);

        int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                      TIMEOUT_INSTALL, NULL, 0);
        if (ret == 0) (*installed)++;
        else if (pkgs[i].required) return -1;
    }
    return 0;
}

/* ─── GEM ────────────────────────────────────────────────────────── */

static int install_gem(gscope_scope_t *scope,
                        const tmpl_package_t *pkgs, int count,
                        const tmpl_var_t *env, int env_count,
                        gscope_tmpl_progress_fn fn, void *ud,
                        int *installed)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(pkgs[i].manager, "gem") != 0) continue;

        char cmd[512];
        if (pkgs[i].version[0])
            snprintf(cmd, sizeof(cmd), "gem install '%s' -v '%s' --no-document",
                     pkgs[i].name, pkgs[i].version);
        else
            snprintf(cmd, sizeof(cmd), "gem install '%s' --no-document",
                     pkgs[i].name);

        char msg[256];
        snprintf(msg, sizeof(msg), "gem: Installing %s...", pkgs[i].name);
        report(fn, ud, msg, -1, false);

        int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                      TIMEOUT_INSTALL, NULL, 0);
        if (ret == 0) (*installed)++;
        else if (pkgs[i].required) return -1;
    }
    return 0;
}

/* ─── Public: Install All Packages ───────────────────────────────── */

int tmpl_pkg_install(gscope_scope_t *scope,
                      const tmpl_package_t *packages, int count,
                      const tmpl_var_t *env, int env_count,
                      gscope_tmpl_progress_fn fn, void *ud,
                      int *installed_out)
{
    int installed = 0;
    int ret;

    /* Run pre-install scripts for packages that have them */
    for (int i = 0; i < count; i++) {
        if (packages[i].pre_script && packages[i].pre_script[0]) {
            tmpl_run_script_in_scope(scope, "pkg_pre",
                                      packages[i].pre_script,
                                      env, env_count, 120);
        }
    }

    /* Install by manager type */
    ret = install_apt(scope, packages, count, env, env_count, fn, ud, &installed);
    if (ret < 0) goto done;

    ret = install_pip(scope, packages, count, env, env_count, fn, ud, &installed);
    if (ret < 0) goto done;

    ret = install_npm(scope, packages, count, env, env_count, fn, ud, &installed);
    if (ret < 0) goto done;

    ret = install_cargo(scope, packages, count, env, env_count, fn, ud, &installed);
    if (ret < 0) goto done;

    ret = install_gem(scope, packages, count, env, env_count, fn, ud, &installed);

done:
    /* Run post-install scripts */
    for (int i = 0; i < count; i++) {
        if (packages[i].post_script && packages[i].post_script[0]) {
            tmpl_run_script_in_scope(scope, "pkg_post",
                                      packages[i].post_script,
                                      env, env_count, 120);
        }
    }

    if (installed_out) *installed_out = installed;
    return ret;
}
