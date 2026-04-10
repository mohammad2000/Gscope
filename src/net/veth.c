/*
 * net/veth.c — VETH pair management via netlink
 *
 * Creates virtual ethernet pairs connecting the host bridge to scope
 * network namespaces.
 *
 * Naming: gs{id}h (host side), gs{id}s → renamed to eth0 inside scope.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/veth.h>
#include <net/if.h>

extern int gscope_nl_send(int fd, struct nlmsghdr *nlh);
extern int gscope_nl_recv_ack(int fd, int *errmsg);
extern void *gscope_nl_add_attr_str(struct nlmsghdr *nlh, int maxlen,
                                     int type, const char *str);
extern void *gscope_nl_add_attr_u32(struct nlmsghdr *nlh, int maxlen,
                                     int type, uint32_t value);
extern void *gscope_nl_add_attr(struct nlmsghdr *nlh, int maxlen,
                                 int type, const void *data, int len);
extern struct nlattr *gscope_nl_nest_start(struct nlmsghdr *nlh, int maxlen, int type);
extern void gscope_nl_nest_end(struct nlmsghdr *nlh, struct nlattr *start);
extern int gscope_nl_if_nametoindex(const char *name);
#endif

/* ─── Create VETH Pair ───────────────────────────────────────────── */

gscope_err_t gscope_veth_create(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

#ifdef __linux__
    gscope_ctx_t *ctx = scope->ctx;

    /* Build names: gs{id}h (host), gs{id}s (scope) */
    snprintf(scope->veth_host, sizeof(scope->veth_host),
             "gs%uh", scope->id);
    snprintf(scope->veth_scope, sizeof(scope->veth_scope),
             "gs%us", scope->id);

    /* Check if already exists */
    if (if_nametoindex(scope->veth_host) > 0) {
        GSCOPE_DEBUG(ctx, "veth %s already exists", scope->veth_host);
        gscope_clear_error();
        return GSCOPE_OK;
    }

    GSCOPE_INFO(ctx, "creating veth pair: %s <-> %s",
                scope->veth_host, scope->veth_scope);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    /*
     * Build RTM_NEWLINK for veth pair.
     *
     * Structure:
     *   IFLA_IFNAME = "gs{id}h"     (host side name)
     *   IFLA_LINKINFO
     *     IFLA_INFO_KIND = "veth"
     *     IFLA_INFO_DATA
     *       VETH_INFO_PEER
     *         ifinfomsg
     *         IFLA_IFNAME = "gs{id}s" (peer/scope side name)
     */
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
        char attrbuf[1024];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;

    /* Host side name */
    gscope_nl_add_attr_str(&req.nlh, (int)sizeof(req),
                            IFLA_IFNAME, scope->veth_host);

    /* IFLA_LINKINFO */
    struct nlattr *linkinfo = gscope_nl_nest_start(&req.nlh, (int)sizeof(req),
                                                    IFLA_LINKINFO);
    gscope_nl_add_attr_str(&req.nlh, (int)sizeof(req),
                            IFLA_INFO_KIND, "veth");

    /* IFLA_INFO_DATA → VETH_INFO_PEER */
    struct nlattr *infodata = gscope_nl_nest_start(&req.nlh, (int)sizeof(req),
                                                    IFLA_INFO_DATA);
    struct nlattr *peer = gscope_nl_nest_start(&req.nlh, (int)sizeof(req),
                                                VETH_INFO_PEER);

    /* Peer needs its own ifinfomsg */
    struct ifinfomsg *peer_ifi = (struct ifinfomsg *)((char *)&req.nlh + req.nlh.nlmsg_len);
    req.nlh.nlmsg_len += NLA_ALIGN(sizeof(struct ifinfomsg));
    memset(peer_ifi, 0, sizeof(*peer_ifi));
    peer_ifi->ifi_family = AF_UNSPEC;

    /* Peer name */
    gscope_nl_add_attr_str(&req.nlh, (int)sizeof(req),
                            IFLA_IFNAME, scope->veth_scope);

    gscope_nl_nest_end(&req.nlh, peer);
    gscope_nl_nest_end(&req.nlh, infodata);
    gscope_nl_nest_end(&req.nlh, linkinfo);

    /* Send */
    if (gscope_nl_send(fd, &req.nlh) < 0) {
        close(fd);
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK,
                                      "send veth create failed");
    }

    int kerr = 0;
    if (gscope_nl_recv_ack(fd, &kerr) < 0 && kerr != EEXIST) {
        close(fd);
        return gscope_set_error(GSCOPE_ERR_NETLINK,
                                "create veth: kernel error %d (%s)",
                                kerr, strerror(kerr));
    }

    close(fd);

    GSCOPE_INFO(ctx, "veth pair created: %s <-> %s",
                scope->veth_host, scope->veth_scope);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "veth requires Linux");
#endif
}

/* ─── Move VETH to Namespace ─────────────────────────────────────── */

gscope_err_t gscope_veth_move_to_ns(gscope_scope_t *scope)
{
#ifdef __linux__
    if (!scope || scope->veth_scope[0] == '\0' || scope->netns_name[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_INVAL, "missing veth or netns");

    gscope_ctx_t *ctx = scope->ctx;

    /* Get scope-side interface index */
    int ifidx = gscope_nl_if_nametoindex(scope->veth_scope);
    if (ifidx < 0)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "veth %s not found", scope->veth_scope);

    /* Open namespace fd */
    char ns_path[256];
    snprintf(ns_path, sizeof(ns_path), "/var/run/netns/%s", scope->netns_name);
    int ns_fd = open(ns_path, O_RDONLY | O_CLOEXEC);
    if (ns_fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "cannot open netns %s", scope->netns_name);

    /* RTM_NEWLINK with IFLA_NET_NS_FD */
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        close(ns_fd);
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");
    }

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
        char attrbuf[256];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = ifidx;

    /* Move to namespace */
    gscope_nl_add_attr(&req.nlh, (int)sizeof(req),
                        IFLA_NET_NS_FD, &ns_fd, sizeof(ns_fd));

    gscope_nl_send(fd, &req.nlh);
    int kerr = 0;
    int ret = gscope_nl_recv_ack(fd, &kerr);

    close(fd);
    close(ns_fd);

    if (ret < 0)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "move %s to netns %s: error %d",
                                scope->veth_scope, scope->netns_name, kerr);

    GSCOPE_INFO(ctx, "moved %s to netns %s",
                scope->veth_scope, scope->netns_name);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Attach Host Side to Bridge ─────────────────────────────────── */

gscope_err_t gscope_veth_attach_bridge(gscope_scope_t *scope,
                                        const char *bridge_name)
{
#ifdef __linux__
    if (!scope || !bridge_name)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or bridge");

    gscope_ctx_t *ctx = scope->ctx;

    int veth_idx = gscope_nl_if_nametoindex(scope->veth_host);
    int br_idx = gscope_nl_if_nametoindex(bridge_name);

    if (veth_idx < 0 || br_idx < 0)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "interface not found: veth=%s bridge=%s",
                                scope->veth_host, bridge_name);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
        char attrbuf[128];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = veth_idx;

    /* IFLA_MASTER = bridge index */
    gscope_nl_add_attr_u32(&req.nlh, (int)sizeof(req),
                            IFLA_MASTER, (uint32_t)br_idx);

    gscope_nl_send(fd, &req.nlh);
    gscope_nl_recv_ack(fd, NULL);

    /* Bring host side up */
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = veth_idx;
    req.ifi.ifi_change = IFF_UP;
    req.ifi.ifi_flags = IFF_UP;

    gscope_nl_send(fd, &req.nlh);
    gscope_nl_recv_ack(fd, NULL);

    close(fd);

    GSCOPE_INFO(ctx, "attached %s to bridge %s",
                scope->veth_host, bridge_name);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)bridge_name;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Delete VETH ────────────────────────────────────────────────── */

gscope_err_t gscope_veth_delete(gscope_scope_t *scope)
{
#ifdef __linux__
    if (!scope) return gscope_set_error(GSCOPE_ERR_INVAL, "NULL");

    /* Deleting one side deletes both */
    int idx = gscope_nl_if_nametoindex(scope->veth_host);
    if (idx < 0) {
        gscope_clear_error();
        return GSCOPE_OK;  /* Already gone */
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {0};

    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_DELLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = idx;

    gscope_nl_send(fd, &req.nlh);
    gscope_nl_recv_ack(fd, NULL);
    close(fd);

    GSCOPE_DEBUG(scope->ctx, "veth %s deleted", scope->veth_host);
    scope->veth_host[0] = '\0';
    scope->veth_scope[0] = '\0';

    gscope_clear_error();
    return GSCOPE_OK;
#else
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}
