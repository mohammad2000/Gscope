/*
 * test_user.c — Tests for user management subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <gscope/user.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char *mkdtemp(char *template);
extern gscope_err_t gscope_user_set_password(gscope_scope_t *scope,
                                              const char *username,
                                              const char *password);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  %-50s ", #name); } while(0)
#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); tests_passed++; } while(0)
#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m: %s\n", msg); tests_failed++; } while(0)
#define SKIP(msg) \
    do { printf("\033[33mSKIP\033[0m: %s\n", msg); tests_passed++; } while(0)

static void test_null_args(void)
{
    TEST(user_null_args);

    gscope_err_t err;
    err = gscope_user_create(NULL, NULL, 0, 0, GSCOPE_PRIV_STANDARD);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL"); return; }

    err = gscope_user_info(NULL, NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("info should return INVAL"); return; }

    err = gscope_user_delete(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("delete should return INVAL"); return; }

    PASS();
}

static void test_user_create_in_rootfs(void)
{
    TEST(user_create_in_rootfs);

    /* Create temp rootfs */
    char tmpdir[] = "/tmp/gscope_user_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) { SKIP("mkdtemp failed"); return; }

    /* Create etc directory */
    char etc[4096];
    snprintf(etc, sizeof(etc), "%s/etc", tmpdir);
    gscope_mkdir_p(etc, 0755);

    /* Create minimal /etc/passwd with root entry */
    char passwd_path[4096];
    snprintf(passwd_path, sizeof(passwd_path), "%s/etc/passwd", tmpdir);
    int fd = open(passwd_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *root_entry = "root:x:0:0:root:/root:/bin/bash\n";
        write(fd, root_entry, strlen(root_entry));
        close(fd);
    }

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 42;
    scope.ctx = &ctx;
    gscope_strlcpy(scope.rootfs_merged, tmpdir, sizeof(scope.rootfs_merged));

    /* Create user */
    gscope_err_t err = gscope_user_create(&scope, "testuser", 1000, 1000,
                                           GSCOPE_PRIV_STANDARD);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); gscope_rmdir_r(tmpdir); return; }

    /* Verify passwd entry was written */
    char buf[4096];
    if (gscope_read_file(passwd_path, buf, sizeof(buf)) < 0) {
        FAIL("cannot read passwd"); gscope_rmdir_r(tmpdir); return;
    }
    if (!strstr(buf, "testuser:x:1000:1000::/home/testuser:/bin/bash")) {
        FAIL("passwd entry wrong"); gscope_rmdir_r(tmpdir); return;
    }

    /* Verify home directory created */
    struct stat st;
    char home[4096];
    snprintf(home, sizeof(home), "%s/home/testuser", tmpdir);
    if (stat(home, &st) != 0) { FAIL("home dir not created"); gscope_rmdir_r(tmpdir); return; }

    /* Verify .bashrc */
    char bashrc[4096];
    snprintf(bashrc, sizeof(bashrc), "%s/home/testuser/.bashrc", tmpdir);
    if (stat(bashrc, &st) != 0) { FAIL("bashrc not created"); gscope_rmdir_r(tmpdir); return; }

    /* Verify scope state updated */
    if (strcmp(scope.username, "testuser") != 0) { FAIL("username not saved"); gscope_rmdir_r(tmpdir); return; }
    if (scope.uid != 1000) { FAIL("uid not saved"); gscope_rmdir_r(tmpdir); return; }

    /* Test user_info */
    gscope_user_info_t info;
    err = gscope_user_info(&scope, "testuser", &info);
    if (err != GSCOPE_OK) { FAIL("user_info failed"); gscope_rmdir_r(tmpdir); return; }
    if (strcmp(info.home, "/home/testuser") != 0) { FAIL("home mismatch"); gscope_rmdir_r(tmpdir); return; }

    /* Test delete */
    err = gscope_user_delete(&scope, "testuser");
    if (err != GSCOPE_OK) { FAIL("delete failed"); gscope_rmdir_r(tmpdir); return; }

    /* Home should be gone */
    if (stat(home, &st) == 0) { FAIL("home should be removed"); gscope_rmdir_r(tmpdir); return; }

    gscope_rmdir_r(tmpdir);
    PASS();
}

static void test_user_sudo_elevated(void)
{
    TEST(user_sudo_elevated);

    char tmpdir[] = "/tmp/gscope_sudo_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) { SKIP("mkdtemp failed"); return; }

    char etc[4096];
    snprintf(etc, sizeof(etc), "%s/etc", tmpdir);
    gscope_mkdir_p(etc, 0755);

    /* Create passwd for append_line to work */
    char pp[4096];
    snprintf(pp, sizeof(pp), "%s/etc/passwd", tmpdir);
    int fd = open(pp, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 99;
    scope.ctx = &ctx;
    gscope_strlcpy(scope.rootfs_merged, tmpdir, sizeof(scope.rootfs_merged));

    /* Create user with ELEVATED privilege — should auto-configure sudo */
    gscope_err_t err = gscope_user_create(&scope, "admin", 2000, 2000,
                                           GSCOPE_PRIV_ELEVATED);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); gscope_rmdir_r(tmpdir); return; }

    /* Verify sudoers file exists */
    struct stat st;
    char sudoers[4096];
    snprintf(sudoers, sizeof(sudoers), "%s/etc/sudoers.d/admin", tmpdir);
    if (stat(sudoers, &st) != 0) { FAIL("sudoers file not created"); gscope_rmdir_r(tmpdir); return; }

    /* Verify it contains apt rules */
    char buf[4096];
    if (gscope_read_file(sudoers, buf, sizeof(buf)) < 0) { FAIL("cannot read sudoers"); gscope_rmdir_r(tmpdir); return; }
    if (!strstr(buf, "apt")) { FAIL("sudoers should mention apt"); gscope_rmdir_r(tmpdir); return; }

    /* Verify permissions (should be 0440) */
    if ((st.st_mode & 0777) != 0440) { FAIL("sudoers permissions should be 0440"); gscope_rmdir_r(tmpdir); return; }

    gscope_rmdir_r(tmpdir);
    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — user management\033[0m\n\n");

    test_null_args();
    test_user_create_in_rootfs();
    test_user_sudo_elevated();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
