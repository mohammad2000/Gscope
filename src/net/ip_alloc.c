/*
 * net/ip_alloc.c — Bitmap-based IP address allocator
 *
 * Manages IP allocation for the 10.50.0.0/24 subnet:
 *   .0   = network address (reserved)
 *   .1   = gateway / bridge IP (reserved)
 *   .2-9 = reserved for future use
 *   .10-.254 = allocatable for scopes
 *   .255 = broadcast (reserved)
 *
 * Uses a 256-bit bitmap (4 × uint64_t) for O(1) allocation.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <arpa/inet.h>
#endif

/* ─── Bitmap Operations ──────────────────────────────────────────── */

static bool bitmap_test(const uint64_t *bm, int bit)
{
    return (bm[bit / 64] & (1ULL << (bit % 64))) != 0;
}

static void bitmap_set(uint64_t *bm, int bit)
{
    bm[bit / 64] |= (1ULL << (bit % 64));
}

static void bitmap_clear(uint64_t *bm, int bit)
{
    bm[bit / 64] &= ~(1ULL << (bit % 64));
}

/* ─── IP String Helpers ──────────────────────────────────────────── */

static void ip_to_str(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF,
             (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF,
             ip & 0xFF);
}

/* ─── Public API ─────────────────────────────────────────────────── */

/*
 * Allocate the next available IP from the pool.
 *
 * ctx:    library context (contains IP allocator state)
 * out_ip: output buffer for IP string (at least 16 bytes)
 *
 * Returns GSCOPE_OK and fills out_ip, or GSCOPE_ERR_QUOTA if exhausted.
 * Thread-safe (uses ctx->ip_alloc.lock).
 */
gscope_err_t gscope_ip_alloc(gscope_ctx_t *ctx, char *out_ip, size_t ip_size)
{
    if (!ctx || !out_ip || ip_size < 16)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx or out_ip");

    pthread_mutex_lock(&ctx->ip_alloc.lock);

    /* Scan bitmap for first free IP in allocatable range */
    for (int offset = ctx->ip_alloc.first_host;
         offset <= ctx->ip_alloc.last_host;
         offset++) {

        if (!bitmap_test(ctx->ip_alloc.bitmap, offset)) {
            /* Found free IP */
            bitmap_set(ctx->ip_alloc.bitmap, offset);

            uint32_t ip = ctx->ip_alloc.base | (uint32_t)offset;
            ip_to_str(ip, out_ip, ip_size);

            pthread_mutex_unlock(&ctx->ip_alloc.lock);

            gscope_clear_error();
            return GSCOPE_OK;
        }
    }

    pthread_mutex_unlock(&ctx->ip_alloc.lock);

    return gscope_set_error(GSCOPE_ERR_QUOTA,
                            "IP address pool exhausted (no free IPs in subnet)");
}

/*
 * Allocate a specific IP address.
 *
 * Returns GSCOPE_OK if the IP was free and is now allocated.
 * Returns GSCOPE_ERR_EXIST if already allocated.
 */
gscope_err_t gscope_ip_alloc_specific(gscope_ctx_t *ctx,
                                       const char *ip_str)
{
    if (!ctx || !ip_str)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx or ip");

    /* Parse IP to get the last octet */
    int a, b, c, d;
    if (sscanf(ip_str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return gscope_set_error(GSCOPE_ERR_INVAL, "invalid IP: %s", ip_str);

    int offset = d;  /* Last octet = offset in /24 */

    if (offset < ctx->ip_alloc.first_host || offset > ctx->ip_alloc.last_host)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "IP %s outside allocatable range (.%d-.%d)",
                                ip_str,
                                ctx->ip_alloc.first_host,
                                ctx->ip_alloc.last_host);

    pthread_mutex_lock(&ctx->ip_alloc.lock);

    if (bitmap_test(ctx->ip_alloc.bitmap, offset)) {
        pthread_mutex_unlock(&ctx->ip_alloc.lock);
        return gscope_set_error(GSCOPE_ERR_EXIST,
                                "IP %s already allocated", ip_str);
    }

    bitmap_set(ctx->ip_alloc.bitmap, offset);
    pthread_mutex_unlock(&ctx->ip_alloc.lock);

    gscope_clear_error();
    return GSCOPE_OK;
}

/*
 * Release an allocated IP back to the pool.
 */
gscope_err_t gscope_ip_free(gscope_ctx_t *ctx, const char *ip_str)
{
    if (!ctx || !ip_str)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx or ip");

    int a, b, c, d;
    if (sscanf(ip_str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4)
        return gscope_set_error(GSCOPE_ERR_INVAL, "invalid IP: %s", ip_str);

    int offset = d;

    pthread_mutex_lock(&ctx->ip_alloc.lock);
    bitmap_clear(ctx->ip_alloc.bitmap, offset);
    pthread_mutex_unlock(&ctx->ip_alloc.lock);

    gscope_clear_error();
    return GSCOPE_OK;
}

/*
 * Get the gateway IP string for the subnet.
 */
void gscope_ip_gateway(gscope_ctx_t *ctx, char *out, size_t size)
{
    if (!ctx || !out) return;
    ip_to_str(ctx->ip_alloc.gateway, out, size);
}

/*
 * Get number of allocated / total IPs.
 */
void gscope_ip_stats(gscope_ctx_t *ctx, int *allocated, int *total)
{
    if (!ctx) return;

    int count = 0;
    pthread_mutex_lock(&ctx->ip_alloc.lock);
    for (int i = ctx->ip_alloc.first_host; i <= ctx->ip_alloc.last_host; i++) {
        if (bitmap_test(ctx->ip_alloc.bitmap, i))
            count++;
    }
    pthread_mutex_unlock(&ctx->ip_alloc.lock);

    if (allocated) *allocated = count;
    if (total) *total = ctx->ip_alloc.last_host - ctx->ip_alloc.first_host + 1;
}
