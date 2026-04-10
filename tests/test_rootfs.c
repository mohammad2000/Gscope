/*
 * test_rootfs.c — Tests for rootfs/overlay/mount/pivot subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/rootfs.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

/* mkdtemp needs _DEFAULT_SOURCE on some platforms */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Forward declare mkdtemp in case headers miss it */
extern char *mkdtemp(char *template);

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
    TEST(rootfs_null_args);

    gscope_err_t err;
    err = gscope_rootfs_setup(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL"); return; }

    err = gscope_rootfs_info(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("info NULL should return INVAL"); return; }

    err = gscope_rootfs_teardown(NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("teardown NULL should return INVAL"); return; }

    PASS();
}

static void test_rootfs_setup_no_template(void)
{
    TEST(rootfs_setup_no_template);

    /* Use /tmp as base to avoid needing root */
    char tmpdir[] = "/tmp/gscope_test_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) { SKIP("mkdtemp failed"); return; }

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 12345;
    scope.ctx = &ctx;
    scope.config.rootfs_base = tmpdir;
    scope.config.template_path = NULL;

    gscope_err_t err = gscope_rootfs_setup(&scope, NULL);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); gscope_rmdir_r(tmpdir); return; }

    /* Verify essential dirs were created */
    struct stat st;
    char path[4096];

    snprintf(path, sizeof(path), "%s/12345/rootfs/proc", tmpdir);
    if (stat(path, &st) != 0) { FAIL("proc dir missing"); gscope_rmdir_r(tmpdir); return; }

    snprintf(path, sizeof(path), "%s/12345/rootfs/dev/pts", tmpdir);
    if (stat(path, &st) != 0) { FAIL("dev/pts dir missing"); gscope_rmdir_r(tmpdir); return; }

    snprintf(path, sizeof(path), "%s/12345/rootfs/etc", tmpdir);
    if (stat(path, &st) != 0) { FAIL("etc dir missing"); gscope_rmdir_r(tmpdir); return; }

    snprintf(path, sizeof(path), "%s/12345/rootfs/home", tmpdir);
    if (stat(path, &st) != 0) { FAIL("home dir missing"); gscope_rmdir_r(tmpdir); return; }

    /* Verify overlay dirs */
    snprintf(path, sizeof(path), "%s/12345/overlay/upper", tmpdir);
    if (stat(path, &st) != 0) { FAIL("upper dir missing"); gscope_rmdir_r(tmpdir); return; }

    snprintf(path, sizeof(path), "%s/12345/overlay/work", tmpdir);
    if (stat(path, &st) != 0) { FAIL("work dir missing"); gscope_rmdir_r(tmpdir); return; }

    /* Test rootfs_info */
    gscope_rootfs_info_t info;
    err = gscope_rootfs_info(&scope, &info);
    if (err != GSCOPE_OK) { FAIL("info failed"); gscope_rmdir_r(tmpdir); return; }
    if (info.mounted) { FAIL("should not be mounted"); gscope_rmdir_r(tmpdir); return; }

    /* Test teardown */
    err = gscope_rootfs_teardown(&scope);
    if (err != GSCOPE_OK) { FAIL("teardown failed"); gscope_rmdir_r(tmpdir); return; }

    /* Verify cleanup */
    snprintf(path, sizeof(path), "%s/12345", tmpdir);
    if (stat(path, &st) == 0) { FAIL("scope dir should be removed"); gscope_rmdir_r(tmpdir); return; }

    gscope_rmdir_r(tmpdir);
    PASS();
}

static void test_rootfs_info_fields(void)
{
    TEST(rootfs_info_fields);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 42;
    scope.ctx = &ctx;
    gscope_strlcpy(scope.rootfs_lower, "/lower", sizeof(scope.rootfs_lower));
    gscope_strlcpy(scope.rootfs_upper, "/upper", sizeof(scope.rootfs_upper));
    gscope_strlcpy(scope.rootfs_work, "/work", sizeof(scope.rootfs_work));
    gscope_strlcpy(scope.rootfs_merged, "/merged", sizeof(scope.rootfs_merged));
    scope.rootfs_mounted = true;

    gscope_rootfs_info_t info;
    gscope_err_t err = gscope_rootfs_info(&scope, &info);
    if (err != GSCOPE_OK) { FAIL("info failed"); return; }
    if (strcmp(info.lower, "/lower") != 0) { FAIL("lower mismatch"); return; }
    if (strcmp(info.upper, "/upper") != 0) { FAIL("upper mismatch"); return; }
    if (!info.mounted) { FAIL("should be mounted"); return; }

    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — rootfs/overlay\033[0m\n\n");

    test_null_args();
    test_rootfs_setup_no_template();
    test_rootfs_info_fields();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
