/*
 * net/firewall.c — Firewall management (NAT/MASQUERADE)
 *
 * For Phase 7, we use iptables exec as a starting point.
 * Migration to nftables via nfnetlink is planned for a later phase.
 *
 * Rules managed:
 *   - MASQUERADE for scope subnet (outbound NAT)
 *   - FORWARD rules for scope traffic
 *   - Port forwarding (DNAT)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── Helper: run iptables command ───────────────────────────────── */

#ifdef __linux__
static int run_iptables(const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "iptables %s 2>/dev/null", args);
    return system(cmd);
}
#endif

/* ─── NAT / MASQUERADE ───────────────────────────────────────────── */

/*
 * Enable IP forwarding and setup MASQUERADE for scope subnet.
 */
gscope_err_t gscope_fw_setup_nat(gscope_ctx_t *ctx,
                                  const char *subnet,
                                  const char *bridge)
{
#ifdef __linux__
    if (!ctx || !subnet || !bridge)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL args");

    GSCOPE_INFO(ctx, "setting up NAT for %s via %s", subnet, bridge);

    /* Enable IP forwarding */
    gscope_write_file("/proc/sys/net/ipv4/ip_forward", "1");

    /* MASQUERADE: scope subnet → host's default route */
    char rule[256];
    snprintf(rule, sizeof(rule),
             "-t nat -C POSTROUTING -s %s ! -o %s -j MASQUERADE",
             subnet, bridge);
    if (run_iptables(rule) != 0) {
        /* Rule doesn't exist — add it */
        snprintf(rule, sizeof(rule),
                 "-t nat -A POSTROUTING -s %s ! -o %s -j MASQUERADE",
                 subnet, bridge);
        run_iptables(rule);
    }

    /* FORWARD: allow established connections back */
    snprintf(rule, sizeof(rule),
             "-C FORWARD -i %s -j ACCEPT", bridge);
    if (run_iptables(rule) != 0) {
        snprintf(rule, sizeof(rule),
                 "-A FORWARD -i %s -j ACCEPT", bridge);
        run_iptables(rule);
    }

    snprintf(rule, sizeof(rule),
             "-C FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT",
             bridge);
    if (run_iptables(rule) != 0) {
        snprintf(rule, sizeof(rule),
                 "-A FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT",
                 bridge);
        run_iptables(rule);
    }

    GSCOPE_INFO(ctx, "NAT setup complete for %s", subnet);
    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)subnet; (void)bridge;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Port Forwarding (DNAT) ─────────────────────────────────────── */

gscope_err_t gscope_fw_port_forward(gscope_ctx_t *ctx,
                                     const gscope_port_map_t *map,
                                     const char *scope_ip)
{
#ifdef __linux__
    if (!ctx || !map || !scope_ip)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL args");

    const char *proto = (map->protocol == 17) ? "udp" : "tcp";

    GSCOPE_INFO(ctx, "port forward: host:%u → %s:%u (%s)",
                map->host_port, scope_ip, map->scope_port, proto);

    char rule[512];

    /* DNAT: incoming traffic on host_port → scope_ip:scope_port */
    snprintf(rule, sizeof(rule),
             "-t nat -A PREROUTING -p %s --dport %u -j DNAT --to-destination %s:%u",
             proto, map->host_port, scope_ip, map->scope_port);
    run_iptables(rule);

    /* Also for locally-generated traffic */
    snprintf(rule, sizeof(rule),
             "-t nat -A OUTPUT -p %s --dport %u -j DNAT --to-destination %s:%u",
             proto, map->host_port, scope_ip, map->scope_port);
    run_iptables(rule);

    /* FORWARD: allow forwarded traffic to scope */
    snprintf(rule, sizeof(rule),
             "-A FORWARD -p %s -d %s --dport %u -j ACCEPT",
             proto, scope_ip, map->scope_port);
    run_iptables(rule);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)map; (void)scope_ip;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Remove Port Forward ────────────────────────────────────────── */

gscope_err_t gscope_fw_port_remove(gscope_ctx_t *ctx,
                                    const gscope_port_map_t *map,
                                    const char *scope_ip)
{
#ifdef __linux__
    if (!ctx || !map || !scope_ip)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL args");

    const char *proto = (map->protocol == 17) ? "udp" : "tcp";

    char rule[512];

    snprintf(rule, sizeof(rule),
             "-t nat -D PREROUTING -p %s --dport %u -j DNAT --to-destination %s:%u",
             proto, map->host_port, scope_ip, map->scope_port);
    run_iptables(rule);

    snprintf(rule, sizeof(rule),
             "-t nat -D OUTPUT -p %s --dport %u -j DNAT --to-destination %s:%u",
             proto, map->host_port, scope_ip, map->scope_port);
    run_iptables(rule);

    snprintf(rule, sizeof(rule),
             "-D FORWARD -p %s -d %s --dport %u -j ACCEPT",
             proto, scope_ip, map->scope_port);
    run_iptables(rule);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)map; (void)scope_ip;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Cleanup All NAT ────────────────────────────────────────────── */

gscope_err_t gscope_fw_cleanup(gscope_ctx_t *ctx, const char *bridge)
{
#ifdef __linux__
    if (!ctx || !bridge)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL args");

    GSCOPE_INFO(ctx, "cleaning up firewall rules for %s", bridge);

    char rule[256];

    /* Remove FORWARD rules for bridge */
    snprintf(rule, sizeof(rule),
             "-D FORWARD -i %s -j ACCEPT", bridge);
    run_iptables(rule);

    snprintf(rule, sizeof(rule),
             "-D FORWARD -o %s -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT",
             bridge);
    run_iptables(rule);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)bridge;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}
