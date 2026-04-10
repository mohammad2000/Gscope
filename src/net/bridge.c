/*
 * net/bridge.c — Linux bridge management via netlink
 *
 * Creates and manages the host bridge (br-gscope) that connects
 * all scope veth interfaces for inter-scope and external communication.
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
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>

/* Internal netlink helpers */
extern int gscope_nl_send(int fd, struct nlmsghdr *nlh);
extern int gscope_nl_recv_ack(int fd, int *errmsg);
extern void *gscope_nl_add_attr_str(struct nlmsghdr *nlh, int maxlen,
                                     int type, const char *str);
extern struct nlattr *gscope_nl_nest_start(struct nlmsghdr *nlh, int maxlen, int type);
extern void gscope_nl_nest_end(struct nlmsghdr *nlh, struct nlattr *start);

/* Forward declaration */
gscope_err_t gscope_bridge_set_up(gscope_ctx_t *ctx, const char *name);
#endif

/* ─── Create Bridge ──────────────────────────────────────────────── */

gscope_err_t gscope_bridge_create(gscope_ctx_t *ctx, const char *name)
{
    if (!ctx || !name)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx or name");

#ifdef __linux__
    /* Check if already exists */
    if (if_nametoindex(name) > 0) {
        GSCOPE_DEBUG(ctx, "bridge %s already exists", name);
        gscope_clear_error();
        return GSCOPE_OK;
    }

    int fd = ctx->nl_sock;
    if (fd < 0) {
        /* Open temporary socket */
        fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd < 0)
            return gscope_set_error_errno(GSCOPE_ERR_NETLINK,
                                          "socket(NETLINK_ROUTE) failed");
        struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
        bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    }

    GSCOPE_INFO(ctx, "creating bridge: %s", name);

    /* Build RTM_NEWLINK message */
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
        char attrbuf[512];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;

    /* IFLA_IFNAME = bridge name */
    gscope_nl_add_attr_str(&req.nlh, (int)sizeof(req), IFLA_IFNAME, name);

    /* IFLA_LINKINFO → IFLA_INFO_KIND = "bridge" */
    struct nlattr *linkinfo = gscope_nl_nest_start(&req.nlh, (int)sizeof(req), IFLA_LINKINFO);
    if (!linkinfo) {
        if (fd != ctx->nl_sock) close(fd);
        return gscope_set_error(GSCOPE_ERR_NETLINK, "buffer overflow building msg");
    }
    gscope_nl_add_attr_str(&req.nlh, (int)sizeof(req), IFLA_INFO_KIND, "bridge");
    gscope_nl_nest_end(&req.nlh, linkinfo);

    /* Send */
    if (gscope_nl_send(fd, &req.nlh) < 0) {
        if (fd != ctx->nl_sock) close(fd);
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK,
                                      "send RTM_NEWLINK failed");
    }

    int kerr = 0;
    if (gscope_nl_recv_ack(fd, &kerr) < 0) {
        if (fd != ctx->nl_sock) close(fd);
        if (kerr == EEXIST) {
            GSCOPE_DEBUG(ctx, "bridge %s created concurrently", name);
            gscope_clear_error();
            return GSCOPE_OK;
        }
        return gscope_set_error(GSCOPE_ERR_NETLINK,
                                "create bridge %s: kernel error %d (%s)",
                                name, kerr, strerror(kerr));
    }

    if (fd != ctx->nl_sock) close(fd);

    /* Bring bridge up */
    gscope_bridge_set_up(ctx, name);

    GSCOPE_INFO(ctx, "bridge %s created and up", name);
    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)name;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "bridge requires Linux");
#endif
}

/* ─── Set Interface Up/Down ──────────────────────────────────────── */

gscope_err_t gscope_bridge_set_up(gscope_ctx_t *ctx, const char *name)
{
#ifdef __linux__
    unsigned int idx = if_nametoindex(name);
    if (idx == 0)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "interface %s not found", name);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_NEWLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = (int)idx;
    req.ifi.ifi_change = IFF_UP;
    req.ifi.ifi_flags = IFF_UP;

    gscope_nl_send(fd, &req.nlh);
    gscope_nl_recv_ack(fd, NULL);
    close(fd);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)ctx; (void)name;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}

/* ─── Delete Bridge ──────────────────────────────────────────────── */

gscope_err_t gscope_bridge_delete(gscope_ctx_t *ctx, const char *name)
{
#ifdef __linux__
    unsigned int idx = if_nametoindex(name);
    if (idx == 0) {
        gscope_clear_error();
        return GSCOPE_OK;  /* Already gone */
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nlh.nlmsg_type = RTM_DELLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.nlh.nlmsg_seq = ++ctx->nl_seq;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = (int)idx;

    gscope_nl_send(fd, &req.nlh);
    gscope_nl_recv_ack(fd, NULL);
    close(fd);

    GSCOPE_INFO(ctx, "bridge %s deleted", name);
    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)ctx; (void)name;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}
