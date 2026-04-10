/*
 * proc/pty.c — PTY (pseudo-terminal) management
 *
 * A PTY consists of a master/slave pair:
 *   master_fd — held by the parent (gscope), used for I/O
 *   slave_fd  — connected to child's stdin/stdout/stderr
 *
 * Data flow:
 *   User types → write(master_fd) → appears on child's stdin
 *   Child prints → write(stdout=slave) → readable from master_fd
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <termios.h>
#include <pty.h>
#endif

/*
 * Create a PTY pair.
 *
 * master_fd: parent's end — read output, write input
 * slave_fd:  child's end — becomes stdin/stdout/stderr
 *
 * Returns 0 on success, -1 on failure.
 */
int gscope_pty_create(int *master_fd, int *slave_fd,
                       uint16_t rows, uint16_t cols)
{
    if (!master_fd || !slave_fd)
        return -1;

#ifdef __linux__
    if (openpty(master_fd, slave_fd, NULL, NULL, NULL) != 0)
        return -1;

    /* Set initial window size */
    if (rows == 0) rows = 24;
    if (cols == 0) cols = 80;

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };
    ioctl(*master_fd, TIOCSWINSZ, &ws);

    /* Set master to non-blocking for async I/O */
    int flags = fcntl(*master_fd, F_GETFL);
    if (flags >= 0)
        fcntl(*master_fd, F_SETFL, flags | O_NONBLOCK);

    /* Set close-on-exec */
    fcntl(*master_fd, F_SETFD, FD_CLOEXEC);

    return 0;
#else
    (void)rows; (void)cols;
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * Resize a PTY's window.
 *
 * Sends SIGWINCH to the process group of the slave terminal.
 */
int gscope_pty_resize(int master_fd, uint16_t rows, uint16_t cols)
{
#ifdef __linux__
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    return ioctl(master_fd, TIOCSWINSZ, &ws);
#else
    (void)master_fd; (void)rows; (void)cols;
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * Get current PTY window size.
 */
int gscope_pty_get_size(int master_fd, uint16_t *rows, uint16_t *cols)
{
#ifdef __linux__
    struct winsize ws;
    if (ioctl(master_fd, TIOCGWINSZ, &ws) != 0)
        return -1;

    if (rows) *rows = ws.ws_row;
    if (cols) *cols = ws.ws_col;
    return 0;
#else
    (void)master_fd; (void)rows; (void)cols;
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * Setup the slave PTY in the child process.
 *
 * Must be called after fork() in the child:
 *   1. Create new session (setsid)
 *   2. Set slave as controlling terminal
 *   3. Redirect stdin/stdout/stderr to slave
 *   4. Close original slave fd
 *
 * After this, master_fd should be closed in the child.
 */
int gscope_pty_setup_child(int slave_fd)
{
#ifdef __linux__
    /* New session — detach from parent's terminal */
    if (setsid() < 0)
        return -1;

    /* Set controlling terminal */
    if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
        /* Non-fatal on some systems */
    }

    /* Redirect stdio */
    if (dup2(slave_fd, STDIN_FILENO) < 0)
        return -1;
    if (dup2(slave_fd, STDOUT_FILENO) < 0)
        return -1;
    if (dup2(slave_fd, STDERR_FILENO) < 0)
        return -1;

    /* Close original fd if it's not 0, 1, or 2 */
    if (slave_fd > STDERR_FILENO)
        close(slave_fd);

    return 0;
#else
    (void)slave_fd;
    errno = ENOSYS;
    return -1;
#endif
}
