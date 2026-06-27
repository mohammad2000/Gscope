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
#include <stdlib.h>
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

/* ─── Live-state reservation (collision guard) ───────────────────────
 *
 * The in-memory bitmap only tracks IPs THIS allocator handed out. But our
 * /24 (10.50.0.0/24) is NOT exclusively ours at runtime: a co-tenant daemon
 * (gmeshd) installs per-scope /30 mesh veths inside the SAME /24. A /30 route
 * is more-specific than the bridge /24, so if we hand a scope an IP that falls
 * inside a live /30 (e.g. .12 inside 10.50.0.12/30 dev vh-sN), the kernel
 * routes that IP out the foreign veth and the scope becomes unreachable from
 * the gateway ("No route to host" to its backing DB).
 *
 * Before every allocation we therefore scan the LIVE host networking — every
 * assigned address and every more-specific route inside our /24 — and reserve
 * those bits so we never select an IP another subsystem already owns. This is
 * the C, in-library version of the collision guard; it makes gscope correct
 * regardless of what gmeshd (or anything else) does in the shared /24.
 * Best-effort: parse failures simply leave the bitmap unchanged.
 */
static void reserve_token(gscope_ctx_t *ctx, const char *tok, bool is_route)
{
    unsigned a, b, c, d, pfx = 32;
    int m = sscanf(tok, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &pfx);
    if (m < 4) return;
    /* Only our /24 (base's first three octets). */
    if (a != ((ctx->ip_alloc.base >> 24) & 0xFF) ||
        b != ((ctx->ip_alloc.base >> 16) & 0xFF) ||
        c != ((ctx->ip_alloc.base >> 8) & 0xFF))
        return;
    if (d > 255) return;
    if (is_route && pfx >= 25 && pfx <= 31) {
        /* A more-specific sub-/24 block (e.g. a /30 mesh veth) — reserve the
         * WHOLE block so we never land inside a foreign point-to-point range. */
        unsigned block = 1u << (32 - pfx);
        unsigned start = d & ~(block - 1);
        for (unsigned o = start; o < start + block && o <= 255; o++)
            bitmap_set(ctx->ip_alloc.bitmap, (int)o);
    } else {
        /* Assigned address or host route — reserve just that host. (A bare /24
         * route is our own subnet; it only marks .0, which is reserved anyway.) */
        bitmap_set(ctx->ip_alloc.bitmap, (int)d);
    }
}

/* Public (internal) entry — called by the scope-create path right before
 * allocation, NOT from the gscope_ip_alloc primitive itself (keeps that a pure,
 * environment-independent bitmap op that unit tests can exercise in isolation). */
void gscope_ip_reserve_live(gscope_ctx_t *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->ip_alloc.lock);
    const char *cmds[3] = {
        "ip -o -4 addr show 2>/dev/null",
        "ip -o -4 route show 2>/dev/null",
        /* Per-scope IPs live INSIDE each network namespace and are invisible to
         * the host's `ip addr`/`ip route`. During coexistence with the legacy
         * Python scope path (whose veths sit in `scope-<id>` netns) we MUST scan
         * every netns too, or we hand out an address already held by a live
         * legacy scope (e.g. 10.50.0.10) and collide on the shared bridge. */
        "for ns in $(ip netns list 2>/dev/null | awk '{print $1}'); do "
        "ip netns exec \"$ns\" ip -o -4 addr show 2>/dev/null; done",
    };
    for (int ci = 0; ci < 3; ci++) {
        FILE *fp = popen(cmds[ci], "r");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof(line), fp) != NULL) {
            for (char *tok = strtok(line, " \t\n"); tok; tok = strtok(NULL, " \t\n"))
                reserve_token(ctx, tok, ci == 1);
        }
        pclose(fp);
    }
    pthread_mutex_unlock(&ctx->ip_alloc.lock);
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
