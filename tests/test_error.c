/*
 * test_error.c — Tests for error subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/error.h>
#include <gscope/types.h>
#include <gscope/scope.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  %-50s ", #name); } while(0)

#define PASS() \
    do { printf("\033[32mPASS\033[0m\n"); tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("\033[31mFAIL\033[0m: %s\n", msg); tests_failed++; } while(0)

#define ASSERT(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_initial_state(void)
{
    TEST(initial_state);
    ASSERT(gscope_last_error() == GSCOPE_OK, "initial error should be OK");
    ASSERT(strcmp(gscope_strerror(), "success") == 0, "initial message should be 'success'");
    ASSERT(gscope_last_errno() == 0, "initial errno should be 0");
    PASS();
}

static void test_err_names(void)
{
    TEST(err_names);
    ASSERT(strcmp(gscope_err_name(GSCOPE_OK), "GSCOPE_OK") == 0, "OK name");
    ASSERT(strcmp(gscope_err_name(GSCOPE_ERR_INVAL), "GSCOPE_ERR_INVAL") == 0, "INVAL name");
    ASSERT(strcmp(gscope_err_name(GSCOPE_ERR_NAMESPACE), "GSCOPE_ERR_NAMESPACE") == 0, "NS name");
    ASSERT(strcmp(gscope_err_name(GSCOPE_ERR_UNSUPPORTED), "GSCOPE_ERR_UNSUPPORTED") == 0, "UNSUP name");
    ASSERT(strcmp(gscope_err_name((gscope_err_t)-999), "GSCOPE_ERR_UNKNOWN") == 0, "unknown name");
    PASS();
}

static void test_version(void)
{
    TEST(version);
    const char *v = gscope_version();
    ASSERT(v != NULL, "version not NULL");
    ASSERT(strlen(v) > 0, "version not empty");
    ASSERT(strstr(v, "0.1.0") != NULL, "version is 0.1.0");
    PASS();
}

static void test_config_defaults(void)
{
    TEST(config_defaults);
    gscope_config_t c;
    gscope_config_init(&c);
    ASSERT(c.isolation == GSCOPE_ISOLATION_STANDARD, "default isolation");
    ASSERT(c.net_mode == GSCOPE_NET_BRIDGE, "default net_mode");
    ASSERT(c.privilege == GSCOPE_PRIV_STANDARD, "default privilege");
    ASSERT(c.cpu_cores > 0.9f && c.cpu_cores < 1.1f, "default cpu_cores");
    ASSERT(c.memory_bytes == 512ULL * 1024 * 1024, "default memory");
    ASSERT(c.max_pids == 1024, "default max_pids");
    ASSERT(c.seccomp == GSCOPE_SECCOMP_DEFAULT, "default seccomp");
    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — error & types\033[0m\n\n");

    test_initial_state();
    test_err_names();
    test_version();
    test_config_defaults();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
