/*
 * proc/pidfd.c — pidfd helpers
 *
 * pidfd provides a race-free handle to a process. Unlike PIDs,
 * pidfds cannot be recycled — they remain valid even if the PID
 * is reused by another process.
 *
 * Available since Linux 5.3 (pidfd_open) and 5.1 (pidfd_send_signal).
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

/*
 * Open a pidfd for a running process.
 * Returns fd >= 0 on success, -1 if unavailable or failed.
 *
 * This is a thin wrapper around the syscall with graceful fallback.
 */
int gscope_pidfd_try_open(pid_t pid)
{
    if (pid <= 0)
        return -1;

    int fd = gscope_pidfd_open(pid, 0);
    if (fd < 0) {
        /* ENOSYS = kernel too old, not an error */
        if (errno == ENOSYS)
            return -1;
        /* ESRCH = process already dead */
        return -1;
    }

    return fd;
}

/*
 * Send a signal via pidfd (race-free).
 * Falls back to kill(2) if pidfd_send_signal is unavailable.
 *
 * Returns 0 on success, -1 on failure.
 */
int gscope_pidfd_signal(int pidfd, pid_t fallback_pid, int sig)
{
    if (pidfd >= 0) {
        int ret = gscope_pidfd_send_signal(pidfd, sig, NULL, 0);
        if (ret == 0)
            return 0;
        if (errno != ENOSYS)
            return -1;
        /* ENOSYS: fall through to kill() */
    }

    /* Fallback to kill(2) */
    if (fallback_pid > 0)
        return kill(fallback_pid, sig);

    errno = ESRCH;
    return -1;
}

/*
 * Check if a process is alive using pidfd or kill(pid, 0).
 * Returns true if alive, false if dead or error.
 */
bool gscope_pidfd_is_alive(int pidfd, pid_t fallback_pid)
{
    if (pidfd >= 0) {
        int ret = gscope_pidfd_send_signal(pidfd, 0, NULL, 0);
        if (ret == 0)
            return true;
        if (errno == ENOSYS)
            goto fallback;
        return false;
    }

fallback:
    if (fallback_pid > 0) {
        if (kill(fallback_pid, 0) == 0)
            return true;
    }

    return false;
}
