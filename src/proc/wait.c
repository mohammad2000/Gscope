/*
 * proc/wait.c — Process waiting with pidfd and timeout
 *
 * Provides robust process waiting using:
 *   1. pidfd + poll (preferred — exact timeout, no races)
 *   2. waitpid + WNOHANG loop (fallback)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <poll.h>
#endif

/*
 * Wait for a process to exit with timeout.
 *
 * pidfd:    file descriptor from pidfd_open (-1 if unavailable)
 * pid:      process ID (used as fallback if pidfd == -1)
 * status:   output exit status (NULL to ignore)
 * timeout_ms: milliseconds to wait (0 = indefinite, -1 = non-blocking)
 *
 * Returns:
 *   0  — process exited, status filled
 *  -1  — error (check errno)
 *  -2  — timeout
 */
int gscope_wait_pid(int pidfd, pid_t pid, int *status, int timeout_ms)
{
#ifdef __linux__
    /*
     * Strategy 1: poll on pidfd
     * This gives us exact timeout semantics without busy-waiting.
     */
    if (pidfd >= 0 && timeout_ms != 0) {
        struct pollfd pfd = {
            .fd = pidfd,
            .events = POLLIN,  /* pidfd becomes readable when process exits */
        };

        int poll_timeout = (timeout_ms > 0) ? timeout_ms : -1;
        int ret = poll(&pfd, 1, poll_timeout);

        if (ret == 0)
            return -2;  /* Timeout */

        if (ret < 0) {
            if (errno == EINTR)
                return -2;  /* Treat interrupted as timeout */
            return -1;
        }

        /* Process exited — fall through to waitpid */
    }

    /*
     * Strategy 2: waitpid
     *
     * If we have a pidfd, the poll above already told us the process
     * is dead, so WNOHANG will succeed immediately.
     *
     * If no pidfd, we busy-wait with WNOHANG.
     */
    if (pidfd >= 0 || timeout_ms == -1) {
        /* Non-blocking waitpid */
        int wstatus;
        pid_t ret = waitpid(pid, &wstatus, WNOHANG);
        if (ret > 0) {
            if (status) {
                if (WIFEXITED(wstatus))
                    *status = WEXITSTATUS(wstatus);
                else if (WIFSIGNALED(wstatus))
                    *status = 128 + WTERMSIG(wstatus);
                else
                    *status = -1;
            }
            return 0;
        }
        if (ret == 0)
            return -2;  /* Still running */
        if (errno == ECHILD) {
            /* Already reaped */
            if (status) *status = 0;
            return 0;
        }
        return -1;
    }

    /*
     * Strategy 3: Busy-wait with increasing sleep intervals
     * Only used when no pidfd and timeout > 0.
     */
    int elapsed_us = 0;
    int total_us = timeout_ms * 1000;
    int sleep_us = 10000;  /* Start at 10ms */

    while (elapsed_us < total_us) {
        int wstatus;
        pid_t ret = waitpid(pid, &wstatus, WNOHANG);

        if (ret > 0) {
            if (status) {
                if (WIFEXITED(wstatus))
                    *status = WEXITSTATUS(wstatus);
                else if (WIFSIGNALED(wstatus))
                    *status = 128 + WTERMSIG(wstatus);
                else
                    *status = -1;
            }
            return 0;
        }

        if (ret < 0) {
            if (errno == ECHILD) {
                if (status) *status = 0;
                return 0;
            }
            return -1;
        }

        /* ret == 0: still running */
        usleep((useconds_t)sleep_us);
        elapsed_us += sleep_us;

        /* Increase interval: 10ms → 20ms → 50ms → 100ms (max) */
        if (sleep_us < 100000)
            sleep_us = sleep_us * 3 / 2;
        if (sleep_us > 100000)
            sleep_us = 100000;
    }

    return -2;  /* Timeout */

#else
    (void)pidfd; (void)pid; (void)status; (void)timeout_ms;
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * Stop a process gracefully: SIGTERM → wait → SIGKILL
 *
 * Returns 0 when process is confirmed dead.
 */
int gscope_stop_process(int pidfd, pid_t pid, int timeout_sec)
{
    /* Send SIGTERM */
    if (pidfd >= 0) {
        gscope_pidfd_send_signal(pidfd, SIGTERM, NULL, 0);
    } else if (pid > 0) {
        kill(pid, SIGTERM);
    } else {
        return -1;
    }

    /* Wait for graceful exit */
    int status;
    int ret = gscope_wait_pid(pidfd, pid, &status,
                               timeout_sec > 0 ? timeout_sec * 1000 : 5000);

    if (ret == 0)
        return 0;  /* Exited gracefully */

    /* Still alive — SIGKILL */
    if (pidfd >= 0) {
        gscope_pidfd_send_signal(pidfd, SIGKILL, NULL, 0);
    } else {
        kill(pid, SIGKILL);
    }

    /* Wait for SIGKILL to take effect */
    ret = gscope_wait_pid(pidfd, pid, &status, 3000);
    return (ret == 0) ? 0 : -1;
}
