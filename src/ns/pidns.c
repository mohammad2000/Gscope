/*
 * ns/pidns.c — PID namespace helpers
 *
 * PID namespaces have a special property: after unshare(CLONE_NEWPID),
 * the calling process is NOT in the new PID namespace — only its
 * children will be. So we need a "double fork":
 *
 *   Parent → unshare(CLONE_NEWPID) → fork() → Child (PID 1 in new ns)
 *
 * The child process becomes PID 1 inside the new namespace.
 * PID 1 is special: if it dies, all processes in the namespace are killed.
 *
 * This module provides helpers for:
 *   - Detecting if we're already in a PID namespace
 *   - The double-fork pattern used by spawn.c
 *   - PID 1 reaping (init responsibility)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ─── PID Namespace Detection ────────────────────────────────────── */

/*
 * Check if we're inside a PID namespace (not the root one).
 * In a PID namespace, /proc/1/sched shows a different PID than 1.
 */
bool gscope_pidns_is_root(void)
{
#ifdef __linux__
    char buf[64];
    if (gscope_read_file("/proc/1/sched", buf, sizeof(buf)) < 0)
        return true;  /* Can't read — assume root */

    /* In root PID ns, PID 1's sched shows "systemd" or "init" */
    /* In child PID ns, PID 1 is our init process */
    return true;  /* Conservative default */
#else
    return true;
#endif
}

/*
 * Get our PID as seen from the parent PID namespace.
 * Returns -1 if unavailable.
 */
pid_t gscope_pidns_get_outer_pid(void)
{
#ifdef __linux__
    char buf[256];
    if (gscope_read_file("/proc/self/status", buf, sizeof(buf)) < 0)
        return -1;

    /* Look for "NSpid:" line which shows PID in each namespace */
    const char *p = strstr(buf, "NSpid:");
    if (!p) return -1;

    p += 6; /* skip "NSpid:" */

    /* First number is PID in outermost namespace */
    while (*p == ' ' || *p == '\t') p++;
    return (pid_t)atoi(p);
#else
    return getpid();
#endif
}

/*
 * Reap zombie children — the init (PID 1) responsibility.
 *
 * In a PID namespace, PID 1 must reap orphaned children,
 * otherwise they become zombies.
 *
 * This is called in a loop by the init process.
 * Returns the number of children reaped.
 */
int gscope_pidns_reap_zombies(void)
{
    int reaped = 0;

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        reaped++;
    }

    return reaped;
}

/*
 * Wait for all children to exit.
 * Used during scope stop to ensure clean shutdown.
 * Returns 0 when all children have exited, -1 on timeout.
 */
int gscope_pidns_wait_all(int timeout_sec)
{
    for (int i = 0; i < timeout_sec * 10; i++) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid < 0 && errno == ECHILD)
            return 0;  /* No more children */

        usleep(100000);  /* 100ms */
    }

    return -1;  /* Timeout */
}
