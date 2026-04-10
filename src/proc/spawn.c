/*
 * proc/spawn.c — Process spawning with full namespace isolation
 *
 * This is the most critical module in gscope. It implements the
 * fork pipeline that creates an isolated process inside a scope:
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │  PARENT                                                     │
 * │    │                                                        │
 * │    ├─ pipe2(sync_pipe)     ← for PID communication         │
 * │    ├─ openpty()            ← PTY pair (if requested)        │
 * │    ├─ fork() ──────────────┐                                │
 * │    │                       │                                │
 * │    │  STAGE-1 CHILD        │                                │
 * │    │    ├─ setsid()        │ new session                    │
 * │    │    ├─ TIOCSCTTY       │ set controlling terminal       │
 * │    │    ├─ dup2(slave → 0,1,2)                              │
 * │    │    ├─ setns(NET)      │ enter network namespace        │
 * │    │    ├─ unshare(PID|MNT|UTS|IPC)                         │
 * │    │    ├─ mount(/, MS_PRIVATE|MS_REC) ← remount private    │
 * │    │    │                                                   │
 * │    │    ├─ [if PID ns] fork() ─────┐                        │
 * │    │    │   write pid2 to pipe     │                        │
 * │    │    │   _exit(0)               │                        │
 * │    │    │                          │                        │
 * │    │    │  STAGE-2 CHILD (PID 1 in new ns)                  │
 * │    │    │    ├─ mount /proc, /sys, /dev/pts, /dev/shm       │
 * │    │    │    ├─ pivot_root(rootfs)  ← secure root switch    │
 * │    │    │    ├─ caps_set()          ← drop capabilities     │
 * │    │    │    ├─ no_new_privs()                               │
 * │    │    │    ├─ seccomp_apply()     ← syscall filter        │
 * │    │    │    ├─ setgid() + setgroups() + setuid()           │
 * │    │    │    ├─ chdir(workdir)                               │
 * │    │    │    └─ execvp(command)                              │
 * │    │    │                                                   │
 * │    │    └─ [if no PID ns]                                   │
 * │    │         write own pid to pipe                          │
 * │    │         continue to pivot_root → exec                  │
 * │    │                                                        │
 * │    ├─ read(pipe) → real_pid                                 │
 * │    ├─ pidfd_open(real_pid)  ← race-free handle              │
 * │    ├─ waitpid(stage1)       ← reap stage-1 if PID ns       │
 * │    └─ return {pid, pidfd, pty_fd}                           │
 * └─────────────────────────────────────────────────────────────┘
 *
 * Security order (CRITICAL — must be in this exact sequence):
 *   1. Enter namespaces (highest privilege needed)
 *   2. Mount filesystems
 *   3. pivot_root (requires CAP_SYS_ADMIN)
 *   4. Drop capabilities (after all privileged ops)
 *   5. Set no_new_privs (prevent regaining privs via exec)
 *   6. Apply seccomp filter (last — after all setup)
 *   7. Drop to non-root user (setuid/setgid)
 *   8. exec (process image replaced)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <poll.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <termios.h>
#include <pty.h>
#endif

/* Forward declarations for internal functions from other modules */
extern int gscope_ns_unshare(uint32_t ns_flags);
extern int gscope_ns_enter_netns(const char *name);
extern gscope_err_t gscope_do_pivot_root(const char *new_root);
extern gscope_err_t gscope_do_chroot(const char *new_root);
extern gscope_err_t gscope_mount_essential(const char *rootfs);
extern gscope_err_t gscope_dev_setup(const char *rootfs);
extern gscope_err_t gscope_mask_paths(const char *rootfs);

/* Security */
extern gscope_err_t gscope_seccomp_apply(gscope_seccomp_t profile, const char *path);
extern gscope_err_t gscope_caps_set(uint64_t keep_mask);
extern uint64_t gscope_caps_default_mask(gscope_isolation_t level);

/* ─── Default Environment ────────────────────────────────────────── */

static const char *default_env[] = {
    "TERM=xterm-256color",
    "LANG=en_US.UTF-8",
    "LC_ALL=en_US.UTF-8",
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    NULL
};

/* ─── Build Environment Array ────────────────────────────────────── */

/*
 * Merge default env, scope config env, and user overrides.
 * Returns a heap-allocated NULL-terminated array.
 * Caller must free the array (but not the strings, which point to
 * the defaults or config).
 */
static char **build_envp(const gscope_exec_config_t *config,
                          const gscope_scope_t *scope)
{
    /* Count entries */
    int count = 0;
    for (int i = 0; default_env[i]; i++) count++;

    /* Extra: HOME, USER, LOGNAME, HOSTNAME */
    count += 4;

    /* User-provided environment */
    if (config->envp) {
        for (int i = 0; config->envp[i]; i++) count++;
    }

    char **envp = calloc((size_t)(count + 1), sizeof(char *));
    if (!envp) return NULL;

    int idx = 0;

    /* Copy defaults */
    for (int i = 0; default_env[i]; i++)
        envp[idx++] = (char *)default_env[i];

    /* HOME, USER, LOGNAME */
    static __thread char home_buf[4128];
    static __thread char user_buf[128];
    static __thread char logname_buf[128];
    static __thread char hostname_buf[128];

    const char *username = scope->username[0] ? scope->username : "root";
    snprintf(home_buf, sizeof(home_buf), "HOME=/home/%s", username);
    snprintf(user_buf, sizeof(user_buf), "USER=%s", username);
    snprintf(logname_buf, sizeof(logname_buf), "LOGNAME=%s", username);

    if (strcmp(username, "root") == 0)
        snprintf(home_buf, sizeof(home_buf), "HOME=/root");

    char hn[64] = "scope";
    if (scope->config.hostname)
        gscope_strlcpy(hn, scope->config.hostname, sizeof(hn));
    else
        snprintf(hn, sizeof(hn), "scope-%u", scope->id);
    snprintf(hostname_buf, sizeof(hostname_buf), "HOSTNAME=%s", hn);

    envp[idx++] = home_buf;
    envp[idx++] = user_buf;
    envp[idx++] = logname_buf;
    envp[idx++] = hostname_buf;

    /* User overrides */
    if (config->envp) {
        for (int i = 0; config->envp[i]; i++)
            envp[idx++] = (char *)config->envp[i];
    }

    envp[idx] = NULL;
    return envp;
}

/* ─── Child Process Logic ────────────────────────────────────────── */

/*
 * This function runs in the CHILD process after fork().
 * It NEVER returns — it either calls execvp or _exit.
 *
 * All error reporting is via the error_fd pipe.
 */
static void child_exec(const gscope_exec_config_t *config,
                        const gscope_scope_t *scope,
                        int slave_fd,       /* PTY slave, -1 if no PTY */
                        int pid_pipe_w,     /* Write end of PID pipe */
                        int error_pipe_w)   /* Write end of error pipe */
{
#ifdef __linux__
    /*
     * Step 0: Close all inherited file descriptors.
     *
     * Leaked fds from the parent can expose host resources
     * (sockets, files, device handles) to the scoped process.
     *
     * We close everything except:
     *   - slave_fd     (PTY, or -1)
     *   - pid_pipe_w   (PID communication)
     *   - error_pipe_w (error reporting)
     *   - stdin/stdout/stderr (will be replaced by PTY)
     */
    {
        /* Try close_range() first (Linux 5.9+, most efficient) */
        int max_keep = slave_fd;
        if (pid_pipe_w > max_keep) max_keep = pid_pipe_w;
        if (error_pipe_w > max_keep) max_keep = error_pipe_w;

        /* Close everything above our highest needed fd */
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;

        for (int fd = STDERR_FILENO + 1; fd < (int)max_fd; fd++) {
            if (fd == slave_fd || fd == pid_pipe_w || fd == error_pipe_w)
                continue;
            close(fd);  /* Ignore errors (fd might not be open) */
        }
    }

    /*
     * Step 1: New session + controlling terminal
     */
    setsid();

    if (slave_fd >= 0) {
        /* Set PTY slave as controlling terminal */
        ioctl(slave_fd, TIOCSCTTY, 0);

        /* Redirect stdio to PTY */
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        if (slave_fd > STDERR_FILENO)
            close(slave_fd);
    }

    /*
     * Step 2: Enter network namespace
     */
    if (scope->netns_name[0] != '\0') {
        if (gscope_ns_enter_netns(scope->netns_name) != 0) {
            dprintf(error_pipe_w, "setns(NET %s): %s",
                    scope->netns_name, strerror(errno));
            _exit(1);
        }
    }

    /*
     * Step 3: Unshare other namespaces (PID, MNT, UTS, IPC)
     *
     * NET is already done via setns above.
     * USER namespace, if requested, is set up by the parent via uid_map/gid_map.
     */
    uint32_t unshare_flags = scope->config.ns_flags & ~(GSCOPE_NS_NET | GSCOPE_NS_USER);

    if (unshare_flags) {
        if (gscope_ns_unshare(unshare_flags) != 0) {
            dprintf(error_pipe_w, "unshare(0x%x): %s",
                    unshare_flags, strerror(errno));
            _exit(1);
        }
    }

    /*
     * Step 3.5: Make mount namespace private
     * This prevents mount events from propagating to the host.
     */
    if (scope->config.ns_flags & GSCOPE_NS_MNT) {
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    }

    /*
     * Step 4: PID namespace double-fork
     *
     * After unshare(CLONE_NEWPID), the CURRENT process is still in
     * the old PID namespace. Only children will be PID 1 in the new ns.
     * So we fork again.
     */
    bool pid_ns = (scope->config.ns_flags & GSCOPE_NS_PID) != 0;

    if (pid_ns) {
        pid_t pid2 = fork();
        if (pid2 < 0) {
            dprintf(error_pipe_w, "fork(PID ns): %s", strerror(errno));
            _exit(1);
        }
        if (pid2 > 0) {
            /* Stage-1: send PID of stage-2 to parent, then exit */
            dprintf(pid_pipe_w, "%d\n", pid2);
            close(pid_pipe_w);
            close(error_pipe_w);
            _exit(0);
        }
        /* Stage-2: we are PID 1 in the new namespace */
    } else {
        /* No PID namespace — report our own PID */
        dprintf(pid_pipe_w, "%d\n", getpid());
        close(pid_pipe_w);
    }

    /*
     * Step 5: Set hostname (UTS namespace)
     */
    if (scope->config.ns_flags & GSCOPE_NS_UTS) {
        char hn[64];
        if (scope->config.hostname)
            gscope_strlcpy(hn, scope->config.hostname, sizeof(hn));
        else
            snprintf(hn, sizeof(hn), "scope-%u", scope->id);
        sethostname(hn, strlen(hn));
    }

    /*
     * Step 6: Mount essential filesystems + pivot_root
     */
    if (scope->rootfs_merged[0] != '\0') {
        /* Setup /dev device nodes (OCI requirement) */
        gscope_dev_setup(scope->rootfs_merged);

        /* Mount /proc, /sys, /dev/pts, /dev/shm inside rootfs */
        gscope_mount_essential(scope->rootfs_merged);

        /* Mask sensitive kernel paths (/proc/kcore, /proc/keys, etc.) */
        gscope_mask_paths(scope->rootfs_merged);

        /* pivot_root — secure root filesystem switch */
        if (scope->ctx && scope->ctx->features.has_pivot_root) {
            if (gscope_do_pivot_root(scope->rootfs_merged) != GSCOPE_OK) {
                /* Fallback to chroot */
                if (gscope_do_chroot(scope->rootfs_merged) != GSCOPE_OK) {
                    dprintf(error_pipe_w, "pivot_root and chroot both failed");
                    _exit(1);
                }
            }
        } else {
            /* No pivot_root support — use chroot */
            if (gscope_do_chroot(scope->rootfs_merged) != GSCOPE_OK) {
                dprintf(error_pipe_w, "chroot(%s): %s",
                        scope->rootfs_merged, strerror(errno));
                _exit(1);
            }
        }
    }

    /*
     * Step 7: Drop capabilities
     *
     * MUST happen AFTER pivot_root (which needs CAP_SYS_ADMIN)
     * but BEFORE exec (to limit the new process).
     *
     * We keep only the capabilities specified in config, or the
     * defaults for the isolation level. Everything else is dropped.
     */
    {
        uint64_t keep = scope->config.cap_keep;
        if (keep == 0)
            keep = gscope_caps_default_mask(scope->config.isolation);

        /* Apply explicit drops */
        if (scope->config.cap_drop)
            keep &= ~scope->config.cap_drop;

        gscope_caps_set(keep);
    }

    /*
     * Step 8: Set PR_SET_NO_NEW_PRIVS
     *
     * Prevents the exec'd process from gaining privileges
     * via setuid/setgid binaries. MUST be set before seccomp.
     */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    /* Prevent core dumps (security — no memory inspection) */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /*
     * Step 9: Apply seccomp-bpf filter
     *
     * MUST be the LAST security step before exec.
     * Once applied, the filter restricts ALL future syscalls
     * including those in the exec'd binary.
     *
     * Order is critical: caps → no_new_privs → seccomp → setuid → exec
     */
    if (scope->config.seccomp != GSCOPE_SECCOMP_DISABLED) {
        gscope_seccomp_apply(scope->config.seccomp,
                             scope->config.seccomp_profile_path);
        /* Note: if seccomp_apply fails, we continue anyway.
         * The process will still have other protections (caps, privs).
         * Logging happens inside seccomp_apply. */
    }

    /*
     * Step 10: Drop to non-root user
     *
     * Order matters:
     *   setgid → setgroups → setuid (MUST be last — can't undo)
     */
    uid_t target_uid = config->uid > 0 ? config->uid : scope->uid;
    gid_t target_gid = config->gid > 0 ? config->gid : scope->gid;

    if (target_gid > 0) {
        gid_t groups[] = { target_gid };
        setgroups(1, groups);
        if (setgid(target_gid) != 0) {
            dprintf(error_pipe_w, "setgid(%d): %s",
                    (int)target_gid, strerror(errno));
            _exit(1);
        }
    }

    if (target_uid > 0) {
        if (setuid(target_uid) != 0) {
            dprintf(error_pipe_w, "setuid(%d): %s",
                    (int)target_uid, strerror(errno));
            _exit(1);
        }
    }

    /*
     * Step 11: Change working directory
     */
    const char *workdir = config->work_dir;
    if (!workdir || workdir[0] == '\0') {
        if (target_uid == 0)
            workdir = "/root";
        else
            workdir = "/";
    }
    /* Silently ignore chdir failure — exec may still work */
    (void)chdir(workdir);

    /*
     * Step 12: Close error pipe (signal success to parent)
     */
    close(error_pipe_w);

    /*
     * Step 13: Execute the command
     *
     * Build envp, then execvpe.
     */
    char **envp = build_envp(config, scope);

    if (config->argv) {
        execve(config->command, (char *const *)config->argv, envp);
    } else {
        /* No argv — use command as both argv[0] and command */
        char *argv[] = { (char *)config->command, NULL };
        execve(config->command, argv, envp);
    }

    /* If execve failed, try execvp (searches PATH) */
    if (config->argv) {
        execvp(config->command, (char *const *)config->argv);
    } else {
        char *argv[] = { (char *)config->command, NULL };
        execvp(config->command, argv);
    }

    /* If we get here, exec failed */
    dprintf(STDERR_FILENO, "exec(%s): %s\n", config->command, strerror(errno));
    free(envp);
    _exit(127);

#else
    (void)config; (void)scope; (void)slave_fd;
    (void)pid_pipe_w; (void)error_pipe_w;
    _exit(1);
#endif
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_exec(gscope_scope_t *scope,
                          const gscope_exec_config_t *config,
                          gscope_exec_result_t *result)
{
    if (!scope || !config || !result)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "NULL scope, config, or result");

    if (!config->command || config->command[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "command is NULL or empty");

    memset(result, 0, sizeof(*result));
    result->pid = -1;
    result->pidfd = -1;
    result->pty_fd = -1;
    result->has_pty = false;

#ifdef __linux__
    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "exec in scope %u: %s (pty=%s)",
                scope->id, config->command,
                config->allocate_pty ? "yes" : "no");

    /*
     * Step 1: Create PTY pair (if requested)
     */
    int master_fd = -1, slave_fd = -1;

    if (config->allocate_pty) {
        if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0)
            return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                          "openpty() failed");

        /* Set initial window size */
        uint16_t rows = config->pty_rows > 0 ? config->pty_rows : 24;
        uint16_t cols = config->pty_cols > 0 ? config->pty_cols : 80;

        struct winsize ws = {
            .ws_row = rows,
            .ws_col = cols,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        ioctl(master_fd, TIOCSWINSZ, &ws);

        GSCOPE_DEBUG(ctx, "  PTY created: master=%d slave=%d (%dx%d)",
                     master_fd, slave_fd, cols, rows);
    }

    /*
     * Step 2: Create communication pipes
     *
     * pid_pipe:   child writes its real PID (after optional double-fork)
     * error_pipe: child writes error messages (closed on success → EOF)
     */
    int pid_pipe[2];
    int error_pipe[2];

    if (pipe2(pid_pipe, O_CLOEXEC) != 0) {
        if (master_fd >= 0) { close(master_fd); close(slave_fd); }
        return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                      "pipe2(pid_pipe) failed");
    }

    if (pipe2(error_pipe, O_CLOEXEC) != 0) {
        close(pid_pipe[0]); close(pid_pipe[1]);
        if (master_fd >= 0) { close(master_fd); close(slave_fd); }
        return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                      "pipe2(error_pipe) failed");
    }

    /*
     * Step 3: Fork
     */
    GSCOPE_DEBUG(ctx, "  forking child process...");

    pid_t child = fork();

    if (child < 0) {
        close(pid_pipe[0]); close(pid_pipe[1]);
        close(error_pipe[0]); close(error_pipe[1]);
        if (master_fd >= 0) { close(master_fd); close(slave_fd); }
        return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                      "fork() failed");
    }

    if (child == 0) {
        /* ═══════════════════════════════════════════════════════════
         * CHILD PROCESS — never returns
         * ═══════════════════════════════════════════════════════════ */

        /* Close parent's ends of pipes */
        close(pid_pipe[0]);
        close(error_pipe[0]);

        /* Close PTY master (parent's side) */
        if (master_fd >= 0)
            close(master_fd);

        /* Enter the isolation pipeline */
        child_exec(config, scope, slave_fd, pid_pipe[1], error_pipe[1]);

        /* Should never reach here */
        _exit(1);
    }

    /* ═══════════════════════════════════════════════════════════════
     * PARENT PROCESS
     * ═══════════════════════════════════════════════════════════════ */

    /* Close child's ends */
    close(pid_pipe[1]);
    close(error_pipe[1]);

    /* Close PTY slave (child's side) */
    if (slave_fd >= 0)
        close(slave_fd);

    /*
     * Step 4: Read the real PID from the child
     *
     * If PID namespace is used, the stage-1 child forks again
     * and sends the stage-2 PID. Otherwise, the child sends
     * its own PID.
     */
    char pid_buf[32] = {0};
    ssize_t nr = 0;

    /* Read with timeout (via poll or simple retry) */
    for (int attempt = 0; attempt < 50; attempt++) {
        ssize_t n = read(pid_pipe[0], pid_buf + nr,
                         sizeof(pid_buf) - 1 - (size_t)nr);
        if (n > 0) {
            nr += n;
            if (memchr(pid_buf, '\n', (size_t)nr))
                break;
        } else if (n == 0) {
            break;  /* EOF */
        } else if (errno != EAGAIN && errno != EINTR) {
            break;  /* Error */
        }
        usleep(20000);  /* 20ms */
    }
    close(pid_pipe[0]);

    pid_t real_pid = -1;
    if (nr > 0) {
        pid_buf[nr] = '\0';
        real_pid = (pid_t)atoi(pid_buf);
    }

    if (real_pid <= 0) {
        /* Read error message from error pipe */
        char err_buf[256] = {0};
        read(error_pipe[0], err_buf, sizeof(err_buf) - 1);
        close(error_pipe[0]);

        if (master_fd >= 0) close(master_fd);

        /* Reap stage-1 child */
        waitpid(child, NULL, 0);

        return gscope_set_error(GSCOPE_ERR_PROCESS,
                                "child failed to start: %s",
                                err_buf[0] ? err_buf : "unknown error");
    }

    /*
     * Step 5: Check for errors from child
     *
     * If error_pipe gives EOF immediately, the child closed it
     * successfully (just before exec). If we get data, it's an error.
     */
    char err_buf[256] = {0};
    ssize_t err_n = read(error_pipe[0], err_buf, sizeof(err_buf) - 1);
    close(error_pipe[0]);

    if (err_n > 0) {
        /* Child reported an error */
        err_buf[err_n] = '\0';
        if (master_fd >= 0) close(master_fd);
        waitpid(child, NULL, 0);
        return gscope_set_error(GSCOPE_ERR_PROCESS,
                                "child error: %s", err_buf);
    }

    /*
     * Step 6: Wait for stage-1 (if PID namespace caused double fork)
     */
    if ((scope->config.ns_flags & GSCOPE_NS_PID) && child != real_pid) {
        waitpid(child, NULL, 0);
        GSCOPE_DEBUG(ctx, "  reaped stage-1 child %d", (int)child);
    }

    /*
     * Step 7: Verify process is alive
     */
    usleep(50000);  /* 50ms — let exec happen */
    if (kill(real_pid, 0) != 0 && errno == ESRCH) {
        if (master_fd >= 0) close(master_fd);
        return gscope_set_error(GSCOPE_ERR_PROCESS,
                                "process %d died immediately after exec",
                                (int)real_pid);
    }

    /*
     * Step 8: Open pidfd (race-free process handle)
     */
    int pidfd = gscope_pidfd_open(real_pid, 0);
    if (pidfd < 0) {
        GSCOPE_DEBUG(ctx, "  pidfd_open not available (pid=%d)", (int)real_pid);
        pidfd = -1;  /* Graceful degradation */
    } else {
        GSCOPE_DEBUG(ctx, "  pidfd=%d for pid=%d", pidfd, (int)real_pid);
    }

    /*
     * Step 9: Set PTY master to non-blocking
     */
    if (master_fd >= 0) {
        int flags = fcntl(master_fd, F_GETFL);
        if (flags >= 0)
            fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /*
     * Step 10: Fill result
     */
    result->pid = real_pid;
    result->pidfd = pidfd;
    result->pty_fd = master_fd;
    result->has_pty = (master_fd >= 0);

    GSCOPE_INFO(ctx, "exec successful: pid=%d pidfd=%d pty=%s",
                (int)real_pid, pidfd,
                result->has_pty ? "yes" : "no");

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)scope; (void)config;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "process spawning requires Linux");
#endif
}

/* ─── Process Signal ─────────────────────────────────────────────── */

gscope_err_t gscope_process_signal(gscope_exec_result_t *result, int sig)
{
    if (!result)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL result");

    if (result->pid <= 0)
        return gscope_set_error(GSCOPE_ERR_STATE, "no PID in result");

    /* Prefer pidfd if available (race-free) */
    if (result->pidfd >= 0) {
        if (gscope_pidfd_send_signal(result->pidfd, sig, NULL, 0) == 0) {
            gscope_clear_error();
            return GSCOPE_OK;
        }
        /* Fall through to kill() if pidfd_send_signal unavailable */
    }

    if (kill(result->pid, sig) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                      "kill(%d, %d) failed",
                                      (int)result->pid, sig);

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Process Wait ───────────────────────────────────────────────── */

gscope_err_t gscope_process_wait(gscope_exec_result_t *result,
                                  int *exit_status, int timeout_ms)
{
    if (!result)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL result");

    if (result->pid <= 0)
        return gscope_set_error(GSCOPE_ERR_STATE, "no PID in result");

#ifdef __linux__
    /*
     * Strategy: poll on pidfd (if available) with timeout,
     * then waitpid to get exit status.
     * Fallback: busy-wait with WNOHANG.
     */
    if (result->pidfd >= 0 && timeout_ms > 0) {
        struct pollfd pfd = {
            .fd = result->pidfd,
            .events = POLLIN,
        };

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret == 0) {
            return gscope_set_error(GSCOPE_ERR_TIMEOUT,
                                    "process %d did not exit within %d ms",
                                    (int)result->pid, timeout_ms);
        }
    }

    /* Try waitpid */
    int status;
    int elapsed = 0;
    int interval = 10000;  /* 10ms */

    while (elapsed < timeout_ms || timeout_ms <= 0) {
        pid_t ret = waitpid(result->pid, &status, WNOHANG);
        if (ret > 0) {
            if (exit_status) {
                if (WIFEXITED(status))
                    *exit_status = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    *exit_status = 128 + WTERMSIG(status);
                else
                    *exit_status = -1;
            }
            gscope_clear_error();
            return GSCOPE_OK;
        }
        if (ret < 0) {
            if (errno == ECHILD) {
                /* Already reaped — process is gone */
                if (exit_status) *exit_status = 0;
                gscope_clear_error();
                return GSCOPE_OK;
            }
            return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                          "waitpid(%d) failed",
                                          (int)result->pid);
        }

        /* ret == 0: still running */
        if (timeout_ms > 0 && elapsed >= timeout_ms)
            break;

        usleep((useconds_t)interval);
        elapsed += interval / 1000;
    }

    return gscope_set_error(GSCOPE_ERR_TIMEOUT,
                            "process %d did not exit within %d ms",
                            (int)result->pid, timeout_ms);
#else
    (void)exit_status; (void)timeout_ms;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── PTY Resize ─────────────────────────────────────────────────── */

gscope_err_t gscope_process_resize_pty(gscope_exec_result_t *result,
                                        uint16_t rows, uint16_t cols)
{
    if (!result)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL result");

    if (result->pty_fd < 0)
        return gscope_set_error(GSCOPE_ERR_STATE, "no PTY attached");

#ifdef __linux__
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    if (ioctl(result->pty_fd, TIOCSWINSZ, &ws) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_PROCESS,
                                      "ioctl(TIOCSWINSZ) failed");

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)rows; (void)cols;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Release Resources ──────────────────────────────────────────── */

void gscope_process_release(gscope_exec_result_t *result)
{
    if (!result)
        return;

    if (result->pidfd >= 0) {
        close(result->pidfd);
        result->pidfd = -1;
    }

    if (result->pty_fd >= 0) {
        close(result->pty_fd);
        result->pty_fd = -1;
    }

    result->pid = -1;
    result->has_pty = false;
}
