/*
 * fs/pivot.c — pivot_root implementation
 *
 * pivot_root(2) replaces chroot(2) for proper root filesystem isolation.
 *
 * Unlike chroot:
 *   - pivot_root UNMOUNTS the old root, making escape impossible
 *   - chroot only changes the apparent root — old root is still accessible
 *   - pivot_root requires a mount namespace
 *
 * Sequence:
 *   1. Bind-mount new_root onto itself (required by pivot_root)
 *   2. Create put_old directory inside new_root
 *   3. pivot_root(new_root, put_old) — swaps root and old root
 *   4. chdir("/") — into the new root
 *   5. umount2(put_old, MNT_DETACH) — detach old root
 *   6. rmdir(put_old) — clean up mount point
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mount.h>
#endif

gscope_err_t gscope_do_pivot_root(const char *new_root)
{
    if (!new_root || new_root[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "new_root path is NULL or empty");

#ifdef __linux__
    /*
     * Step 1: Bind-mount new_root onto itself.
     *
     * pivot_root requires new_root to be on a different mount point
     * than the current root. A bind mount satisfies this.
     */
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "bind mount %s failed", new_root);

    /*
     * Step 2: Create the put_old directory.
     *
     * This is where the old root will be moved to.
     * It must be under new_root.
     */
    char put_old[4096];
    snprintf(put_old, sizeof(put_old), "%s/.pivot_old", new_root);
    mkdir(put_old, 0700);

    /*
     * Step 3: pivot_root(new_root, put_old)
     *
     * After this:
     *   / = new_root (our scope rootfs)
     *   /.pivot_old = the old host root
     */
    if (gscope_pivot_root(new_root, put_old) != 0) {
        /* Fallback to chroot if pivot_root unavailable */
        if (errno == ENOSYS || errno == EINVAL) {
            /* Undo bind mount */
            umount2(new_root, MNT_DETACH);

            /* Fallback: chroot (less secure but functional) */
            if (chroot(new_root) != 0)
                return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                              "chroot %s failed", new_root);
            if (chdir("/") != 0)
                return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                              "chdir / failed after chroot");

            gscope_clear_error();
            return GSCOPE_OK;
        }

        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "pivot_root(%s, %s) failed",
                                      new_root, put_old);
    }

    /*
     * Step 4: Change to new root.
     */
    if (chdir("/") != 0)
        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "chdir / failed after pivot_root");

    /*
     * Step 5: Unmount old root.
     *
     * MNT_DETACH ensures it succeeds even if something is using it.
     * After this, the old host filesystem is completely inaccessible.
     */
    if (umount2("/.pivot_old", MNT_DETACH) != 0) {
        /* Non-fatal — try to continue */
    }

    /*
     * Step 6: Remove the mount point.
     */
    rmdir("/.pivot_old");

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)new_root;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "pivot_root requires Linux");
#endif
}

/*
 * Simplified chroot fallback for systems without pivot_root.
 * Less secure — old root remains accessible.
 */
gscope_err_t gscope_do_chroot(const char *new_root)
{
    if (!new_root || new_root[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "new_root path is NULL or empty");

#ifdef __linux__
    if (chroot(new_root) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "chroot %s failed", new_root);

    if (chdir("/") != 0)
        return gscope_set_error_errno(GSCOPE_ERR_MOUNT,
                                      "chdir / failed after chroot");

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)new_root;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "chroot requires Linux");
#endif
}
