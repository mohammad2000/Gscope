/*
 * template/template_exec.c — Execute commands/scripts inside scope
 *
 * Two execution strategies:
 *   1. nsenter (preferred) — enters all namespaces of init process
 *   2. ip netns exec + chroot (fallback) — for when init isn't running
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ─── Build environment array for execve ─────────────────────────── */

static char **build_env_array(const tmpl_var_t *env, int env_count)
{
    /* Base env + custom + NULL terminator */
    static const char *base_env[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=xterm-256color",
        "LANG=en_US.UTF-8",
        "DEBIAN_FRONTEND=noninteractive",
        "HOME=/root",
        NULL
    };

    int base_count = 0;
    while (base_env[base_count]) base_count++;

    int total = base_count + env_count + 1;
    char **envp = calloc((size_t)total, sizeof(char *));
    if (!envp) return NULL;

    int idx = 0;
    for (int i = 0; i < base_count; i++)
        envp[idx++] = strdup(base_env[i]);

    for (int i = 0; i < env_count; i++) {
        char buf[4224];
        snprintf(buf, sizeof(buf), "%s=%s", env[i].key, env[i].value);
        envp[idx++] = strdup(buf);
    }

    envp[idx] = NULL;
    return envp;
}

static void free_env_array(char **envp)
{
    if (!envp) return;
    for (int i = 0; envp[i]; i++)
        free(envp[i]);
    free(envp);
}

/* ─── Execute command with timeout ───────────────────────────────── */

int tmpl_exec_in_scope(gscope_scope_t *scope,
                        const char *command,
                        const tmpl_var_t *env, int env_count,
                        int timeout_sec,
                        char *output, size_t output_size)
{
    if (!scope || !command)
        return -1;

    if (output && output_size > 0)
        output[0] = '\0';

    /* Build command line */
    char cmd[8192];

    if (scope->init_pid > 0) {
        /* Strategy 1: nsenter into init's namespaces */
        snprintf(cmd, sizeof(cmd),
                 "nsenter -t %d -m -p -n -u -i -- /bin/bash -lc '%s'",
                 (int)scope->init_pid, command);
    } else if (scope->netns_name[0] && scope->rootfs_merged[0]) {
        /* Strategy 2: ip netns exec + chroot */
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s chroot %s /bin/bash -lc '%s'",
                 scope->netns_name, scope->rootfs_merged, command);
    } else if (scope->rootfs_merged[0]) {
        /* Strategy 3: chroot only */
        snprintf(cmd, sizeof(cmd),
                 "chroot %s /bin/bash -lc '%s'",
                 scope->rootfs_merged, command);
    } else {
        return -1;
    }

    /* Create pipe for output capture */
    int pipefd[2] = {-1, -1};
    if (output && output_size > 0) {
        if (pipe(pipefd) < 0)
            return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
        return -1;
    }

    if (pid == 0) {
        /* Child */
        if (pipefd[1] >= 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }

        /* Set environment */
        char **envp = build_env_array(env, env_count);

        if (envp)
            execle("/bin/bash", "bash", "-c", cmd, (char *)NULL, envp);
        else
            execlp("/bin/bash", "bash", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    if (pipefd[1] >= 0) close(pipefd[1]);

    /* Read output with timeout */
    if (output && output_size > 0 && pipefd[0] >= 0) {
        size_t total = 0;
        /* Non-blocking read */
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

        int elapsed = 0;
        while (elapsed < timeout_sec * 1000) {
            ssize_t n = read(pipefd[0], output + total,
                             output_size - total - 1);
            if (n > 0) {
                total += (size_t)n;
            } else if (n == 0) {
                break;  /* EOF */
            } else if (errno != EAGAIN && errno != EINTR) {
                break;
            }

            /* Check if child exited */
            int status;
            pid_t ret = waitpid(pid, &status, WNOHANG);
            if (ret > 0) {
                /* Drain remaining output */
                while ((n = read(pipefd[0], output + total,
                                  output_size - total - 1)) > 0)
                    total += (size_t)n;

                output[total] = '\0';
                close(pipefd[0]);
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }

            usleep(50000); /* 50ms */
            elapsed += 50;
        }

        output[total] = '\0';
        close(pipefd[0]);
    }

    /* Wait for child */
    int status;
    int wait_ms = timeout_sec * 1000;
    int waited = 0;

    while (waited < wait_ms) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret > 0)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (ret < 0)
            return (errno == ECHILD) ? 0 : -1;
        usleep(100000);
        waited += 100;
    }

    /* Timeout — kill */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* ─── Run a script (writes to temp file, executes, cleans up) ────── */

int tmpl_run_script_in_scope(gscope_scope_t *scope,
                              const char *script_name,
                              const char *script_content,
                              const tmpl_var_t *env, int env_count,
                              int timeout_sec)
{
    if (!scope || !script_content || !script_content[0])
        return 0;  /* Empty script = success */

    /* Write script to temp file inside rootfs */
    char script_path[4096];
    snprintf(script_path, sizeof(script_path),
             "%s/tmp/gscope_%s.sh", scope->rootfs_merged,
             script_name ? script_name : "script");

    /* Wrap with error handling */
    char wrapped[32768];
    snprintf(wrapped, sizeof(wrapped),
             "#!/bin/bash\nset -e\nset -o pipefail\n\n%s\n",
             script_content);

    int fd = open(script_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0) return -1;
    write(fd, wrapped, strlen(wrapped));
    close(fd);

    /* Execute inside scope */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/tmp/gscope_%s.sh",
             script_name ? script_name : "script");

    int ret = tmpl_exec_in_scope(scope, cmd, env, env_count,
                                  timeout_sec, NULL, 0);

    /* Cleanup temp file */
    unlink(script_path);

    return ret;
}
