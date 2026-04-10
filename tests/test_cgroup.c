/*
 * test_cgroup.c — Tests for cgroup v2 subsystem
 *
 * Note: Most cgroup tests require root on Linux.
 * On non-Linux or non-root, tests verify API behavior
 * with expected error handling.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/cgroup.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

#include <assert.h>
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
    TEST(cgroup_null_args);

    gscope_err_t err;

    err = gscope_cgroup_create(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL for NULL"); return; }

    err = gscope_cgroup_add_pid(NULL, 1);
    if (err != GSCOPE_ERR_INVAL) { FAIL("add_pid NULL should return INVAL"); return; }

    err = gscope_cgroup_stats(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("stats NULL should return INVAL"); return; }

    err = gscope_cgroup_freeze(NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("freeze NULL should return INVAL"); return; }

    err = gscope_cgroup_delete(NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("delete NULL should return INVAL"); return; }

    PASS();
}

static void test_no_cgroup_state(void)
{
    TEST(cgroup_no_state);

    /* Create a scope struct with empty cgroup_path */
    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 999;
    scope.ctx = &ctx;
    scope.cgroup_path[0] = '\0';

    gscope_err_t err;

    err = gscope_cgroup_add_pid(&scope, 1);
    if (err != GSCOPE_ERR_STATE) { FAIL("should return STATE when no cgroup"); return; }

    gscope_cgroup_stats_t stats;
    err = gscope_cgroup_stats(&scope, &stats);
    if (err != GSCOPE_ERR_STATE) { FAIL("stats should return STATE"); return; }

    /* Delete of empty path should succeed (no-op) */
    err = gscope_cgroup_delete(&scope);
    if (err != GSCOPE_OK) { FAIL("delete of empty should be OK"); return; }

    PASS();
}

static void test_cgroup_create_linux(void)
{
    TEST(cgroup_create_on_linux);

#ifndef __linux__
    SKIP("not Linux");
    return;
#endif

    if (geteuid() != 0) {
        SKIP("requires root");
        return;
    }

    /* This test creates a real cgroup on Linux */
    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_INFO;
    ctx.features.cgroup_version = 2;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.id = 99999;  /* Use high ID to avoid conflicts */
    scope.ctx = &ctx;

    gscope_cgroup_limits_t lim = {
        .cpu_cores = 0.5f,
        .cpu_weight = 100,
        .memory_bytes = 128ULL * 1024 * 1024,
        .memory_swap_bytes = 64ULL * 1024 * 1024,
        .pids_max = 64,
        .io_weight = 100,
    };

    gscope_err_t err = gscope_cgroup_create(&scope, &lim);
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }

    /* Verify cgroup_path was set */
    if (scope.cgroup_path[0] == '\0') { FAIL("cgroup_path not set"); return; }

    /* Read stats */
    gscope_cgroup_stats_t stats;
    err = gscope_cgroup_stats(&scope, &stats);
    if (err != GSCOPE_OK) { FAIL("stats failed"); return; }

    /* Delete */
    err = gscope_cgroup_delete(&scope);
    if (err != GSCOPE_OK) { FAIL("delete failed"); return; }

    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — cgroup v2\033[0m\n\n");

    test_null_args();
    test_no_cgroup_state();
    test_cgroup_create_linux();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
