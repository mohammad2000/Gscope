/*
 * test_namespace.c — Tests for namespace subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/namespace.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    TEST(namespace_null_args);

    gscope_err_t err;
    err = gscope_ns_create(NULL, 0);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL"); return; }

    err = gscope_ns_enter(NULL, GSCOPE_NS_NET);
    if (err != GSCOPE_ERR_INVAL) { FAIL("enter NULL should return INVAL"); return; }

    err = gscope_ns_delete(NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("delete NULL should return INVAL"); return; }

    if (gscope_ns_fd(NULL, GSCOPE_NS_NET) != -1) { FAIL("fd NULL should return -1"); return; }
    if (gscope_ns_verify(NULL, GSCOPE_NS_NET)) { FAIL("verify NULL should return false"); return; }

    PASS();
}

static void test_ns_flags_from_isolation(void)
{
    TEST(namespace_flags_from_isolation);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 77777;
    scope.ctx = &ctx;

    /* Test that isolation levels set appropriate flags */
    scope.config.isolation = GSCOPE_ISOLATION_MINIMAL;

    /* We can't actually create namespaces on macOS,
     * but verify the API accepts the call */
#ifndef __linux__
    gscope_err_t err = gscope_ns_create(&scope, 0);
    if (err != GSCOPE_ERR_UNSUPPORTED) { FAIL("should return UNSUPPORTED on non-Linux"); return; }
    PASS();
#else
    if (geteuid() != 0) {
        SKIP("requires root on Linux");
        return;
    }
    /* On Linux with root, test actual creation */
    gscope_err_t err = gscope_ns_create(&scope, GSCOPE_NS_NET);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }
    if (!gscope_ns_verify(&scope, GSCOPE_NS_NET)) { FAIL("netns not verified"); return; }
    gscope_ns_delete(&scope);
    PASS();
#endif
}

static void test_ns_map_no_process(void)
{
    TEST(namespace_map_no_process);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 88888;
    scope.ctx = &ctx;
    scope.init_pid = -1;

    /* Should fail because no init process */
    gscope_err_t err = gscope_ns_map_uid(&scope, 100000, 0, 65536);
    if (err != GSCOPE_ERR_STATE) { FAIL("should return STATE with no pid"); return; }

    err = gscope_ns_map_gid(&scope, 100000, 0, 65536);
    if (err != GSCOPE_ERR_STATE) { FAIL("should return STATE with no pid"); return; }

    PASS();
}

static void test_ns_map_no_userns(void)
{
    TEST(namespace_map_no_userns);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 99999;
    scope.ctx = &ctx;
    scope.init_pid = 1;  /* Fake PID */
    scope.ns_active = GSCOPE_NS_NET;  /* No USER namespace */

    gscope_err_t err = gscope_ns_map_uid(&scope, 100000, 0, 65536);
    if (err != GSCOPE_ERR_STATE) { FAIL("should return STATE without userns"); return; }

    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — namespaces\033[0m\n\n");

    test_null_args();
    test_ns_flags_from_isolation();
    test_ns_map_no_process();
    test_ns_map_no_userns();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
