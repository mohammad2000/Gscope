/*
 * fs/rootfs.c — Scope directory structure creation
 *
 * Creates the directory tree for a scope under the base path:
 *   /opt/gritiva/scopes/{id}/
 *   ├── overlay/upper/
 *   ├── overlay/work/
 *   ├── rootfs/          (merged mount point)
 *   └── state.json
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ─── Constants ──────────────────────────────────────────────────── */

#define DEFAULT_ROOTFS_BASE "/opt/gritiva/scopes"

/* Essential directories inside the rootfs */
static const char *rootfs_essential_dirs[] = {
    "bin", "sbin", "usr/bin", "usr/sbin",
    "lib", "lib64",
    "etc", "etc/ssh", "etc/ssl/certs", "etc/sudoers.d",
    "var/log", "var/tmp", "var/run", "var/lib",
    "proc", "sys", "dev", "dev/pts", "dev/shm",
    "home", "root", "opt", "tmp",
    "run", "run/sshd",
    NULL
};

/* ─── Helpers ────────────────────────────────────────────────────── */

static int build_scope_paths(gscope_scope_t *scope)
{
    const char *base = scope->config.rootfs_base
                       ? scope->config.rootfs_base
                       : DEFAULT_ROOTFS_BASE;

    char scope_dir[4096];
    snprintf(scope_dir, sizeof(scope_dir), "%s/%u", base, scope->id);

    snprintf(scope->rootfs_lower, sizeof(scope->rootfs_lower),
             "%s", scope->config.template_path ? scope->config.template_path : "");
    snprintf(scope->rootfs_upper, sizeof(scope->rootfs_upper),
             "%s/overlay/upper", scope_dir);
    snprintf(scope->rootfs_work, sizeof(scope->rootfs_work),
             "%s/overlay/work", scope_dir);
    snprintf(scope->rootfs_merged, sizeof(scope->rootfs_merged),
             "%s/rootfs", scope_dir);
    snprintf(scope->rootfs_base, sizeof(scope->rootfs_base),
             "%s", scope_dir);

    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_rootfs_setup(gscope_scope_t *scope,
                                  const char *template_path)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    gscope_ctx_t *ctx = scope->ctx;

    /* Store template path in config if provided */
    if (template_path)
        scope->config.template_path = template_path;

    /* Build all paths */
    build_scope_paths(scope);

    GSCOPE_INFO(ctx, "setting up rootfs for scope %u", scope->id);
    GSCOPE_DEBUG(ctx, "  base:   %s", scope->rootfs_base);
    GSCOPE_DEBUG(ctx, "  lower:  %s", scope->rootfs_lower);
    GSCOPE_DEBUG(ctx, "  upper:  %s", scope->rootfs_upper);
    GSCOPE_DEBUG(ctx, "  work:   %s", scope->rootfs_work);
    GSCOPE_DEBUG(ctx, "  merged: %s", scope->rootfs_merged);

    /* Create directory structure */
    if (gscope_mkdir_p(scope->rootfs_upper, 0755) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_ROOTFS,
                                      "failed to create upper dir: %s",
                                      scope->rootfs_upper);

    if (gscope_mkdir_p(scope->rootfs_work, 0755) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_ROOTFS,
                                      "failed to create work dir: %s",
                                      scope->rootfs_work);

    if (gscope_mkdir_p(scope->rootfs_merged, 0755) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_ROOTFS,
                                      "failed to create merged dir: %s",
                                      scope->rootfs_merged);

    /* If template exists, mount overlay. Otherwise, create minimal rootfs. */
    struct stat st;
    bool has_template = (scope->rootfs_lower[0] != '\0' &&
                         stat(scope->rootfs_lower, &st) == 0 &&
                         S_ISDIR(st.st_mode));

    if (has_template) {
        GSCOPE_INFO(ctx, "template found: %s — will mount overlay", scope->rootfs_lower);
        /* Actual mount happens in overlay.c */
    } else {
        GSCOPE_INFO(ctx, "no template — creating essential dirs in merged rootfs");

        /* Create essential directories directly in merged */
        for (int i = 0; rootfs_essential_dirs[i]; i++) {
            char dir[4096];
            snprintf(dir, sizeof(dir), "%s/%s",
                     scope->rootfs_merged, rootfs_essential_dirs[i]);
            gscope_mkdir_p(dir, 0755);
        }

        /* Create /tmp with sticky bit */
        char tmp_dir[4096];
        snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", scope->rootfs_merged);
        chmod(tmp_dir, 01777);

        /* Create /var/tmp with sticky bit */
        snprintf(tmp_dir, sizeof(tmp_dir), "%s/var/tmp", scope->rootfs_merged);
        chmod(tmp_dir, 01777);
    }

    GSCOPE_INFO(ctx, "rootfs setup complete for scope %u", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_rootfs_info(gscope_scope_t *scope,
                                 gscope_rootfs_info_t *info)
{
    if (!scope || !info)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or info");

    memset(info, 0, sizeof(*info));

    gscope_strlcpy(info->lower, scope->rootfs_lower, sizeof(info->lower));
    gscope_strlcpy(info->upper, scope->rootfs_upper, sizeof(info->upper));
    gscope_strlcpy(info->work, scope->rootfs_work, sizeof(info->work));
    gscope_strlcpy(info->merged, scope->rootfs_merged, sizeof(info->merged));
    info->mounted = scope->rootfs_mounted;

    /* Calculate upper layer size (approximate) */
    /* TODO: du -sb equivalent */
    info->upper_bytes = 0;

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_rootfs_teardown(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "tearing down rootfs for scope %u", scope->id);

    /* Unmount overlay if mounted (done by overlay.c) */
    /* Then remove directories */
    if (scope->rootfs_base[0] != '\0') {
        GSCOPE_DEBUG(ctx, "removing rootfs tree: %s", scope->rootfs_base);
        gscope_rmdir_r(scope->rootfs_base);
    }

    scope->rootfs_mounted = false;
    scope->rootfs_lower[0] = '\0';
    scope->rootfs_upper[0] = '\0';
    scope->rootfs_work[0] = '\0';
    scope->rootfs_merged[0] = '\0';
    scope->rootfs_base[0] = '\0';

    GSCOPE_INFO(ctx, "rootfs teardown complete for scope %u", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}
