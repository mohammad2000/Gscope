/*
 * test_network.c — Tests for networking subsystem
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/network.h>
#include <gscope/types.h>
#include <gscope/error.h>

#include "../src/internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Internal functions */
extern gscope_err_t gscope_ip_alloc(gscope_ctx_t *ctx, char *out, size_t size);
extern gscope_err_t gscope_ip_alloc_specific(gscope_ctx_t *ctx, const char *ip);
extern gscope_err_t gscope_ip_free(gscope_ctx_t *ctx, const char *ip);
extern void gscope_ip_gateway(gscope_ctx_t *ctx, char *out, size_t size);
extern void gscope_ip_stats(gscope_ctx_t *ctx, int *allocated, int *total);

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

static void test_ip_alloc_basic(void)
{
    TEST(network_ip_alloc_basic);

    /* Create a context with initialized IP allocator */
    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log_level = GSCOPE_LOG_OFF;

    /* Init allocator (same as scope.c init_ip_allocator) */
    ctx.ip_alloc.base = (10U << 24) | (50U << 16) | 0U;
    ctx.ip_alloc.gateway = ctx.ip_alloc.base | 1U;
    ctx.ip_alloc.prefix_len = 24;
    ctx.ip_alloc.first_host = 10;
    ctx.ip_alloc.last_host = 254;
    memset(ctx.ip_alloc.bitmap, 0, sizeof(ctx.ip_alloc.bitmap));
    pthread_mutex_init(&ctx.ip_alloc.lock, NULL);

    /* Allocate first IP — should be .10 */
    char ip[16];
    gscope_err_t err = gscope_ip_alloc(&ctx, ip, sizeof(ip));
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }
    if (strcmp(ip, "10.50.0.10") != 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 10.50.0.10, got %s", ip);
        FAIL(msg); return;
    }

    /* Allocate second — should be .11 */
    err = gscope_ip_alloc(&ctx, ip, sizeof(ip));
    if (err != GSCOPE_OK) { FAIL(gscope_strerror()); return; }
    if (strcmp(ip, "10.50.0.11") != 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 10.50.0.11, got %s", ip);
        FAIL(msg); return;
    }

    /* Stats */
    int allocated, total;
    gscope_ip_stats(&ctx, &allocated, &total);
    if (allocated != 2) { FAIL("expected 2 allocated"); return; }
    if (total != 245) { FAIL("expected 245 total (10..254)"); return; }

    /* Free .10, allocate again — should get .10 */
    gscope_ip_free(&ctx, "10.50.0.10");
    err = gscope_ip_alloc(&ctx, ip, sizeof(ip));
    if (err != GSCOPE_OK) { FAIL("alloc after free failed"); return; }
    if (strcmp(ip, "10.50.0.10") != 0) { FAIL("should reuse freed IP"); return; }

    /* Gateway */
    char gw[16];
    gscope_ip_gateway(&ctx, gw, sizeof(gw));
    if (strcmp(gw, "10.50.0.1") != 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "gateway: expected 10.50.0.1, got %s", gw);
        FAIL(msg); return;
    }

    pthread_mutex_destroy(&ctx.ip_alloc.lock);
    PASS();
}

static void test_ip_alloc_specific(void)
{
    TEST(network_ip_alloc_specific);

    gscope_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ip_alloc.base = (10U << 24) | (50U << 16) | 0U;
    ctx.ip_alloc.gateway = ctx.ip_alloc.base | 1U;
    ctx.ip_alloc.first_host = 10;
    ctx.ip_alloc.last_host = 254;
    memset(ctx.ip_alloc.bitmap, 0, sizeof(ctx.ip_alloc.bitmap));
    pthread_mutex_init(&ctx.ip_alloc.lock, NULL);

    /* Allocate specific IP */
    gscope_err_t err = gscope_ip_alloc_specific(&ctx, "10.50.0.100");
    if (err != GSCOPE_OK) { FAIL("specific alloc failed"); return; }

    /* Try to allocate same — should fail */
    err = gscope_ip_alloc_specific(&ctx, "10.50.0.100");
    if (err != GSCOPE_ERR_EXIST) { FAIL("duplicate should return EXIST"); return; }

    /* Out of range */
    err = gscope_ip_alloc_specific(&ctx, "10.50.0.5");
    if (err != GSCOPE_ERR_INVAL) { FAIL("out of range should return INVAL"); return; }

    pthread_mutex_destroy(&ctx.ip_alloc.lock);
    PASS();
}

static void test_null_args(void)
{
    TEST(network_null_args);

    gscope_err_t err;
    err = gscope_net_setup(NULL, GSCOPE_NET_BRIDGE);
    if (err != GSCOPE_ERR_INVAL) { FAIL("should return INVAL"); return; }

    err = gscope_net_info(NULL, NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("info NULL should return INVAL"); return; }

    err = gscope_net_teardown(NULL);
    if (err != GSCOPE_ERR_INVAL) { FAIL("teardown NULL should return INVAL"); return; }

    PASS();
}

int main(void)
{
    printf("\n\033[1mgscope test suite — networking\033[0m\n\n");

    test_ip_alloc_basic();
    test_ip_alloc_specific();
    test_null_args();

    printf("\n  Results: %d passed, %d failed\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
