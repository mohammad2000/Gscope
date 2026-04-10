/*
 * sec/priv.c — Privilege management
 *
 * Handles privilege-related security settings:
 *
 *   PR_SET_NO_NEW_PRIVS:
 *     Prevents the process (and its children) from gaining new
 *     privileges via exec of setuid/setgid binaries. Once set,
 *     cannot be unset. Required before seccomp filter installation.
 *
 *   PR_SET_DUMPABLE:
 *     Controls whether core dumps are generated and whether
 *     /proc/<pid>/ is accessible by non-owner processes.
 *     Setting to 0 prevents information leaks.
 *
 *   Supplementary groups:
 *     Clear all supplementary groups except the primary GID.
 *     Prevents inheriting host user's group memberships.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <unistd.h>

#ifdef __linux__
#include <grp.h>
#else
/* macOS: setgroups is in unistd.h but needs explicit decl with strict POSIX */
extern int setgroups(int, const gid_t *);
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_no_new_privs(void)
{
#ifdef __linux__
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                      "prctl(PR_SET_NO_NEW_PRIVS) failed");

    gscope_clear_error();
    return GSCOPE_OK;
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "PR_SET_NO_NEW_PRIVS requires Linux");
#endif
}

/*
 * Set PR_SET_DUMPABLE to 0.
 * Prevents core dumps and restricts /proc/<pid> access.
 */
gscope_err_t gscope_set_no_dumpable(void)
{
#ifdef __linux__
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                      "prctl(PR_SET_DUMPABLE) failed");

    gscope_clear_error();
    return GSCOPE_OK;
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "PR_SET_DUMPABLE requires Linux");
#endif
}

/*
 * Drop all supplementary groups, keeping only the primary GID.
 * Must be called BEFORE setuid (otherwise EPERM).
 */
gscope_err_t gscope_clear_groups(gid_t primary_gid)
{
    gid_t groups[] = { primary_gid };

    if (setgroups(1, groups) != 0) {
        /* EPERM is expected if not root — non-fatal */
        if (errno == EPERM) {
            gscope_clear_error();
            return GSCOPE_OK;
        }
        return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                      "setgroups() failed");
    }

    gscope_clear_error();
    return GSCOPE_OK;
}

/*
 * Complete privilege drop sequence.
 *
 * Call order (CRITICAL):
 *   1. gscope_clear_groups(gid)   — while still root
 *   2. setgid(gid)                — drop group first
 *   3. setuid(uid)                — drop user LAST (irreversible)
 *   4. verify                     — confirm we can't re-escalate
 *
 * After this, the process runs as (uid, gid) with no way back to root.
 */
gscope_err_t gscope_drop_privileges(uid_t uid, gid_t gid)
{
    if (uid == 0 && gid == 0) {
        /* Running as root inside scope — no drop needed */
        gscope_clear_error();
        return GSCOPE_OK;
    }

    /* Step 1: Clear supplementary groups */
    gscope_clear_groups(gid);

    /* Step 2: Set GID (must be before setuid) */
    if (gid > 0) {
        if (setgid(gid) != 0)
            return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                          "setgid(%d) failed", (int)gid);

        if (setegid(gid) != 0)
            return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                          "setegid(%d) failed", (int)gid);
    }

    /* Step 3: Set UID (LAST — irreversible) */
    if (uid > 0) {
        if (setuid(uid) != 0)
            return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                          "setuid(%d) failed", (int)uid);

        if (seteuid(uid) != 0)
            return gscope_set_error_errno(GSCOPE_ERR_SECURITY,
                                          "seteuid(%d) failed", (int)uid);
    }

    /* Step 4: Verify we can't re-escalate */
    if (uid > 0) {
        /* Try to set UID back to 0 — should fail */
        if (setuid(0) == 0) {
            /* This should NOT succeed — something is wrong */
            return gscope_set_error(GSCOPE_ERR_SECURITY,
                                    "CRITICAL: setuid(0) succeeded after drop!");
        }
    }

    gscope_clear_error();
    return GSCOPE_OK;
}
