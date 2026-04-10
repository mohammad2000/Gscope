/*
 * test_spawn.c — Tests for process spawning subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/process.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Internal functions we test */
extern int gscope_pty_create(int *master_fd, int *slave_fd,
                              uint16_t rows, uint16_t cols);
extern int gscope_pty_resize(int master_fd, uint16_t rows, uint16_t cols);
extern int gscope_pty_get_size(int master_fd, uint16_t *rows, uint16_t *cols);
extern int gscope_pidfd_try_open(pid_t pid);
extern bool gscope_pidfd_is_alive(int pidfd, pid_t fallback_pid);

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
    TEST(spawn_null_args);

    gscope_err_t err;
    gscope_exec_result_t result;

    err = gscope_exec(NULL, NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL"); return; }

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    gscope_scope_t scope;
    memset(&scope, 0, sizeof(scope));
    scope.ctx = &ctx;

    gscope_exec_config_t config;
    memset(&config, 0, sizeof(config));

    err = gscope_exec(&scope, &config, &result);
    if (err != GSCOPE_ERR_INVAL) { FAIL("empty command should return INVAL"); return; }

    PASS();
}

static void test_signal_null(void)
{
    TEST(spawn_signal_null);

    gscope_err_t err;
    err = gscope_process_signal(NULL, 0);
    if (err != GSCOPE_ERR_INVAL) { FAIL("NULL should return INVAL"); return; }

    gscope_exec_result_t result;
    memset(&result, 0, sizeof(result));
    result.pid = -1;

    err = gscope_process_signal(&result, 0);
    if (err != GSCOPE_ERR_STATE) { FAIL("no PID should return STATE"); return; }

    PASS();
}

static void test_wait_null(void)
{
    TEST(spawn_wait_null);

    gscope_err_t err;
    err = gscope_process_wait(NULL, NULL, 0);
    if (err != GSCOPE_ERR_INVAL) { FAIL("NULL should return INVAL"); return; }

    PASS();
}

static void test_pty_resize_null(void)
{
    TEST(spawn_pty_resize_null);

    gscope_err_t err;
    err = gscope_process_resize_pty(NULL, 24, 80);
    if (err != GSCOPE_ERR_INVAL) { FAIL("NULL should return INVAL"); return; }

    gscope_exec_result_t result;
    memset(&result, 0, sizeof(result));
    result.pty_fd = -1;

    err = gscope_process_resize_pty(&result, 24, 80);
    if (err != GSCOPE_ERR_STATE) { FAIL("no PTY should return STATE"); return; }

    PASS();
}

static void test_release(void)
{
    TEST(spawn_release);

    /* Release should handle NULL gracefully */
    gscope_process_release(NULL);

    /* Release with no fds */
    gscope_exec_result_t result;
    memset(&result, 0, sizeof(result));
    result.pid = -1;
    result.pidfd = -1;
    result.pty_fd = -1;
    gscope_process_release(&result);

    if (result.pid != -1) { FAIL("pid should be -1 after release"); return; }
    if (result.pidfd != -1) { FAIL("pidfd should be -1 after release"); return; }

    PASS();
}

static void test_pty_create(void)
{
    TEST(spawn_pty_create);

#ifndef __linux__
    SKIP("PTY requires Linux");
    return;
#else
    int master, slave;
    if (gscope_pty_create(&master, &slave, 30, 120) != 0) {
        SKIP("openpty failed (may need /dev/ptmx)");
        return;
    }

    if (master < 0 || slave < 0) { FAIL("invalid fds"); return; }

    /* Verify we can get the size */
    uint16_t rows = 0, cols = 0;
    if (gscope_pty_get_size(master, &rows, &cols) != 0) {
        FAIL("get_size failed");
        close(master); close(slave);
        return;
    }

    if (rows != 30 || cols != 120) {
        char msg[64];
        snprintf(msg, sizeof(msg), "size mismatch: %dx%d", cols, rows);
        FAIL(msg);
        close(master); close(slave);
        return;
    }

    /* Resize */
    if (gscope_pty_resize(master, 50, 200) != 0) {
        FAIL("resize failed");
        close(master); close(slave);
        return;
    }

    gscope_pty_get_size(master, &rows, &cols);
    if (rows != 50 || cols != 200) {
        FAIL("resize verification failed");
        close(master); close(slave);
        return;
    }

    close(master);
    close(slave);
    PASS();
#endif
}

static void test_pidfd(void)
{
    TEST(spawn_pidfd_open);

#ifndef __linux__
    SKIP("pidfd requires Linux");
    return;
#else
    /* Try to open pidfd for our own process */
    int fd = gscope_pidfd_try_open(getpid());
    if (fd < 0) {
        SKIP("pidfd_open not available (kernel < 5.3?)");
        return;
    }

    /* Verify we're alive */
    bool alive = gscope_pidfd_is_alive(fd, getpid());
    if (!alive) {
        FAIL("should report ourselves as alive");
        close(fd);
        return;
    }

    close(fd);
    PASS();
#endif
}

int main(void)
{
    printf("\n\033[1mgscope test suite — process spawning\033[0m\n\n");

    test_null_args();
    test_signal_null();
    test_wait_null();
    test_pty_resize_null();
    test_release();
    test_pty_create();
    test_pidfd();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
