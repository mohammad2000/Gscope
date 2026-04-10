/*
 * user/user.c — User creation inside scope rootfs
 *
 * Instead of calling `useradd` via subprocess, we directly write
 * to /etc/passwd, /etc/shadow, and create home directories.
 * This avoids dependency on shadow-utils and works in minimal rootfs.
 *
 * passwd format:  username:x:uid:gid:gecos:home:shell
 * shadow format:  username:hash:lastchg:min:max:warn:inactive:expire:
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <crypt.h>
#endif

/* Forward declarations */
gscope_err_t gscope_user_configure_sudo(gscope_scope_t *scope,
                                         const char *username,
                                         gscope_privilege_t privilege);

/* ─── Helpers ────────────────────────────────────────────────────── */

/*
 * Append a line to a file inside the rootfs.
 * Creates the file if it doesn't exist.
 * Returns 0 on success, -1 on error.
 */
static int append_line(const char *rootfs, const char *file, const char *line)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", rootfs, file);

    /* Check if line already exists (idempotent) */
    char buf[8192];
    if (gscope_read_file(path, buf, sizeof(buf)) >= 0) {
        if (strstr(buf, line))
            return 0;  /* Already present */
    }

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    size_t len = strlen(line);
    ssize_t written = write(fd, line, len);
    if (written > 0 && line[len - 1] != '\n')
        write(fd, "\n", 1);
    close(fd);

    return (written > 0) ? 0 : -1;
}

/*
 * Hash a password using SHA-512.
 * Returns static buffer — not thread-safe.
 */
static const char *hash_password(const char *password)
{
    if (!password || password[0] == '\0')
        return "!";  /* Locked account */

#ifdef __linux__
    /* Generate salt */
    char salt[32];
    snprintf(salt, sizeof(salt), "$6$%.16lx$", (unsigned long)time(NULL) ^ (unsigned long)getpid());

    struct crypt_data data;
    memset(&data, 0, sizeof(data));
    char *hash = crypt_r(password, salt, &data);
    if (!hash)
        return "!";

    /* Copy to static buffer since crypt_r uses data */
    static __thread char result[256];
    gscope_strlcpy(result, hash, sizeof(result));
    return result;
#else
    (void)password;
    return "!";
#endif
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_user_create(gscope_scope_t *scope,
                                 const char *username,
                                 uid_t uid, gid_t gid,
                                 gscope_privilege_t privilege)
{
    if (!scope || !username || username[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or username");

    const char *rootfs = scope->rootfs_merged;
    if (rootfs[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "rootfs not set up");

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "creating user '%s' (uid=%d, gid=%d) in scope %u",
                username, (int)uid, (int)gid, scope->id);

    /* Determine home and shell */
    char home[256];
    const char *shell = "/bin/bash";

    if (strcmp(username, "root") == 0) {
        snprintf(home, sizeof(home), "/root");
    } else {
        snprintf(home, sizeof(home), "/home/%s", username);
    }

    /* 1. Create group entry in /etc/group */
    char group_line[512];
    snprintf(group_line, sizeof(group_line), "%s:x:%u:", username, (unsigned)gid);
    if (append_line(rootfs, "etc/group", group_line) < 0)
        GSCOPE_WARN(ctx, "failed to write /etc/group");

    /* 2. Create passwd entry */
    char passwd_line[512];
    snprintf(passwd_line, sizeof(passwd_line), "%s:x:%u:%u::%s:%s",
             username, (unsigned)uid, (unsigned)gid, home, shell);
    if (append_line(rootfs, "etc/passwd", passwd_line) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_USER,
                                      "failed to write /etc/passwd");

    /* 3. Create shadow entry (locked by default) */
    long days_since_epoch = (long)(time(NULL) / 86400);
    char shadow_line[512];
    snprintf(shadow_line, sizeof(shadow_line), "%s:!:%ld:0:99999:7:::",
             username, days_since_epoch);

    char shadow_path[4096];
    snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", rootfs);

    /* Ensure shadow file exists with correct permissions */
    int sfd = open(shadow_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0640);
    if (sfd >= 0) {
        char buf[8192];
        if (gscope_read_file(shadow_path, buf, sizeof(buf)) < 0 ||
            !strstr(buf, username)) {
            size_t len = strlen(shadow_line);
            write(sfd, shadow_line, len);
            write(sfd, "\n", 1);
        }
        close(sfd);
    }

    /* 4. Create home directory */
    char home_path[4096];
    snprintf(home_path, sizeof(home_path), "%s%s", rootfs, home);
    gscope_mkdir_p(home_path, 0755);
    chown(home_path, uid, gid);

    /* 5. Create .bashrc */
    char bashrc_path[4096];
    snprintf(bashrc_path, sizeof(bashrc_path), "%s/.bashrc", home_path);

    char bashrc[1024];
    snprintf(bashrc, sizeof(bashrc),
             "export PS1='\\u@scope-%u:\\w\\$ '\n"
             "export TERM=xterm-256color\n"
             "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
             "alias ls='ls --color=auto'\n"
             "alias ll='ls -la'\n",
             scope->id);

    int bfd = open(bashrc_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (bfd >= 0) {
        write(bfd, bashrc, strlen(bashrc));
        close(bfd);
        chown(bashrc_path, uid, gid);
    }

    /* 6. Create .profile */
    char profile_path[4096];
    snprintf(profile_path, sizeof(profile_path), "%s/.profile", home_path);
    int pfd = open(profile_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (pfd >= 0) {
        const char *profile = "if [ -f ~/.bashrc ]; then . ~/.bashrc; fi\n";
        write(pfd, profile, strlen(profile));
        close(pfd);
        chown(profile_path, uid, gid);
    }

    /* Save in scope state */
    gscope_strlcpy(scope->username, username, sizeof(scope->username));
    scope->uid = uid;
    scope->gid = gid;

    GSCOPE_INFO(ctx, "user '%s' created (uid=%d, gid=%d, home=%s)",
                username, (int)uid, (int)gid, home);

    /* Setup sudo if needed */
    if (privilege >= GSCOPE_PRIV_ELEVATED)
        gscope_user_configure_sudo(scope, username, privilege);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_user_configure_sudo(gscope_scope_t *scope,
                                         const char *username,
                                         gscope_privilege_t privilege)
{
    if (!scope || !username)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or username");

    const char *rootfs = scope->rootfs_merged;
    if (rootfs[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "rootfs not set up");

    gscope_ctx_t *ctx = scope->ctx;

    /* Create /etc/sudoers.d directory */
    char sudoers_dir[4096];
    snprintf(sudoers_dir, sizeof(sudoers_dir), "%s/etc/sudoers.d", rootfs);
    gscope_mkdir_p(sudoers_dir, 0755);

    /* Write sudoers file */
    char sudoers_path[4096];
    snprintf(sudoers_path, sizeof(sudoers_path), "%s/%s", sudoers_dir, username);

    char content[1024];
    if (privilege >= GSCOPE_PRIV_ROOT) {
        /* Full sudo access */
        snprintf(content, sizeof(content),
                 "# gscope sudo rules for %s (privilege: ROOT)\n"
                 "%s ALL=(ALL:ALL) NOPASSWD: ALL\n",
                 username, username);
    } else {
        /* Limited sudo — package management and service control */
        snprintf(content, sizeof(content),
                 "# gscope sudo rules for %s (privilege: ELEVATED)\n"
                 "%s ALL=(ALL) NOPASSWD: /usr/bin/apt, /usr/bin/apt-get\n"
                 "%s ALL=(ALL) NOPASSWD: /usr/bin/pip, /usr/bin/pip3\n"
                 "%s ALL=(ALL) NOPASSWD: /usr/bin/npm\n"
                 "%s ALL=(ALL) NOPASSWD: /usr/bin/systemctl --user *\n"
                 "# Deny dangerous commands\n"
                 "%s ALL=(ALL) !!/usr/sbin/reboot, !/usr/sbin/shutdown\n"
                 "%s ALL=(ALL) !!/usr/sbin/iptables, !/usr/sbin/ip6tables\n",
                 username, username, username, username,
                 username, username, username);
    }

    int fd = open(sudoers_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0440);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_USER,
                                      "failed to create sudoers for %s", username);

    write(fd, content, strlen(content));
    close(fd);

    GSCOPE_DEBUG(ctx, "sudo configured for '%s' (privilege=%d)", username, privilege);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_user_set_password(gscope_scope_t *scope,
                                       const char *username,
                                       const char *password)
{
    if (!scope || !username)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or username");

    const char *rootfs = scope->rootfs_merged;
    if (rootfs[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "rootfs not set up");

    const char *hash = hash_password(password);

    /* Read current shadow file */
    char shadow_path[4096];
    snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", rootfs);

    char buf[8192];
    if (gscope_read_file(shadow_path, buf, sizeof(buf)) < 0)
        return gscope_set_error_errno(GSCOPE_ERR_USER,
                                      "cannot read shadow file");

    /* Find and replace the user's hash */
    char search[128];
    snprintf(search, sizeof(search), "%s:", username);

    char *line_start = strstr(buf, search);
    if (!line_start)
        return gscope_set_error(GSCOPE_ERR_NOENT,
                                "user %s not in shadow file", username);

    /* Rebuild shadow file with new hash */
    char new_shadow[8192];
    int pos = 0;

    /* Copy lines before this user */
    char *p = buf;
    while (p < line_start) {
        char *eol = strchr(p, '\n');
        if (!eol) break;
        int len = (int)(eol - p + 1);
        memcpy(new_shadow + pos, p, (size_t)len);
        pos += len;
        p = eol + 1;
    }

    /* Write new entry */
    long days = (long)(time(NULL) / 86400);
    pos += snprintf(new_shadow + pos, sizeof(new_shadow) - (size_t)pos,
                    "%s:%s:%ld:0:99999:7:::\n", username, hash, days);

    /* Skip old entry */
    char *eol = strchr(line_start, '\n');
    if (eol) p = eol + 1;

    /* Copy remaining lines */
    while (*p) {
        char *next_eol = strchr(p, '\n');
        if (!next_eol) {
            int len = (int)strlen(p);
            memcpy(new_shadow + pos, p, (size_t)len);
            pos += len;
            break;
        }
        int len = (int)(next_eol - p + 1);
        memcpy(new_shadow + pos, p, (size_t)len);
        pos += len;
        p = next_eol + 1;
    }
    new_shadow[pos] = '\0';

    /* Write back atomically */
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", shadow_path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_USER, "cannot write shadow");
    write(fd, new_shadow, (size_t)pos);
    close(fd);
    rename(tmp_path, shadow_path);

    GSCOPE_DEBUG(scope->ctx, "password set for '%s'", username);
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_user_info(gscope_scope_t *scope,
                               const char *username,
                               gscope_user_info_t *info)
{
    if (!scope || !username || !info)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL args");

    memset(info, 0, sizeof(*info));
    gscope_strlcpy(info->username, username, sizeof(info->username));
    info->uid = scope->uid;
    info->gid = scope->gid;
    info->privilege = scope->config.privilege;

    if (strcmp(username, "root") == 0)
        snprintf(info->home, sizeof(info->home), "/root");
    else
        snprintf(info->home, sizeof(info->home), "/home/%s", username);

    gscope_strlcpy(info->shell, "/bin/bash", sizeof(info->shell));

    /* Check if sudo is configured */
    char sudoers_path[4096];
    snprintf(sudoers_path, sizeof(sudoers_path),
             "%s/etc/sudoers.d/%s", scope->rootfs_merged, username);
    struct stat st;
    info->sudo_enabled = (stat(sudoers_path, &st) == 0);

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_user_delete(gscope_scope_t *scope,
                                 const char *username)
{
    if (!scope || !username)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or username");

    gscope_ctx_t *ctx = scope->ctx;
    const char *rootfs = scope->rootfs_merged;

    GSCOPE_INFO(ctx, "deleting user '%s' from scope %u", username, scope->id);

    /* Remove sudoers file */
    char path[4096];
    snprintf(path, sizeof(path), "%s/etc/sudoers.d/%s", rootfs, username);
    unlink(path);

    /* Remove home directory */
    if (strcmp(username, "root") != 0) {
        snprintf(path, sizeof(path), "%s/home/%s", rootfs, username);
        gscope_rmdir_r(path);
    }

    /*
     * Note: We don't remove passwd/shadow/group entries because:
     * 1. The rootfs overlay will be deleted when the scope is deleted
     * 2. Editing these files safely requires careful locking
     * 3. There's no security benefit since the scope is isolated
     */

    GSCOPE_INFO(ctx, "user '%s' deleted from scope %u", username, scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}
