/*
 * fs/overlay.c — OverlayFS mount/unmount
 *
 * Mounts an overlay filesystem combining a read-only template (lower)
 * with a per-scope writable layer (upper).
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

#ifdef __linux__
#include <sys/mount.h>
#endif

/* ─── Mount OverlayFS ────────────────────────────────────────────── */

gscope_err_t gscope_overlay_mount(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (scope->rootfs_mounted) {
        gscope_clear_error();
        return GSCOPE_OK;  /* Already mounted */
    }

    /* Validate paths */
    if (scope->rootfs_lower[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_ROOTFS,
                                "no template (lower) path set");

    struct stat st;
    if (stat(scope->rootfs_lower, &st) != 0 || !S_ISDIR(st.st_mode))
        return gscope_set_error(GSCOPE_ERR_ROOTFS,
                                "template path does not exist: %s",
                                scope->rootfs_lower);

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "mounting overlay for scope %u", scope->id);
    GSCOPE_DEBUG(ctx, "  lowerdir=%s", scope->rootfs_lower);
    GSCOPE_DEBUG(ctx, "  upperdir=%s", scope->rootfs_upper);
    GSCOPE_DEBUG(ctx, "  workdir=%s", scope->rootfs_work);
    GSCOPE_DEBUG(ctx, "  merged=%s", scope->rootfs_merged);

#ifdef __linux__
    /* Build mount options string */
    char options[8192];
    int n = snprintf(options, sizeof(options),
                     "lowerdir=%s,upperdir=%s,workdir=%s",
                     scope->rootfs_lower,
                     scope->rootfs_upper,
                     scope->rootfs_work);
    if (n < 0 || (size_t)n >= sizeof(options))
        return gscope_set_error(GSCOPE_ERR_ROOTFS,
                                "overlay options string too long");

    /* mount(2) */
    if (mount("overlay", scope->rootfs_merged, "overlay", 0, options) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "mount overlay failed at %s",
                                      scope->rootfs_merged);

    scope->rootfs_mounted = true;
    GSCOPE_INFO(ctx, "overlay mounted at %s", scope->rootfs_merged);
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "OverlayFS requires Linux");
#endif

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Unmount OverlayFS ──────────────────────────────────────────── */

gscope_err_t gscope_overlay_unmount(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (!scope->rootfs_mounted) {
        gscope_clear_error();
        return GSCOPE_OK;  /* Not mounted */
    }

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "unmounting overlay for scope %u at %s",
                scope->id, scope->rootfs_merged);

#ifdef __linux__
    /* Try normal unmount first */
    if (umount(scope->rootfs_merged) == 0) {
        scope->rootfs_mounted = false;
        GSCOPE_INFO(ctx, "overlay unmounted cleanly");
        gscope_clear_error();
        return GSCOPE_OK;
    }

    /* If EBUSY, try lazy unmount (MNT_DETACH) */
    if (errno == EBUSY) {
        GSCOPE_WARN(ctx, "overlay busy, trying lazy unmount (MNT_DETACH)");
        if (umount2(scope->rootfs_merged, MNT_DETACH) == 0) {
            scope->rootfs_mounted = false;
            GSCOPE_INFO(ctx, "overlay lazy-unmounted");
            gscope_clear_error();
            return GSCOPE_OK;
        }
    }

    /* If still failing, try force unmount */
    if (umount2(scope->rootfs_merged, MNT_FORCE | MNT_DETACH) == 0) {
        scope->rootfs_mounted = false;
        GSCOPE_WARN(ctx, "overlay force-unmounted");
        gscope_clear_error();
        return GSCOPE_OK;
    }

    return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                  "failed to unmount overlay at %s",
                                  scope->rootfs_merged);
#else
    scope->rootfs_mounted = false;
    gscope_clear_error();
    return GSCOPE_OK;
#endif
}
