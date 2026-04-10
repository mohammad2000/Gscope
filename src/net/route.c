/*
 * net/route.c — Route management via netlink (RTM_NEWROUTE)
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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

extern int gscope_nl_send(int fd, struct nlmsghdr *nlh);
extern int gscope_nl_recv_ack(int fd, int *errmsg);
extern void *gscope_nl_add_attr(struct nlmsghdr *nlh, int maxlen,
                                 int type, const void *data, int len);
#endif

/*
 * Add a default route (0.0.0.0/0) via a gateway.
 *
 * gateway: e.g. "10.50.0.1"
 */
gscope_err_t gscope_route_add_default(const char *gateway)
{
#ifdef __linux__
    if (!gateway)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL gateway");

    struct in_addr gw;
    if (inet_pton(AF_INET, gateway, &gw) != 1)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "invalid gateway: %s", gateway);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    struct {
        struct nlmsghdr nlh;
        struct rtmsg rtm;
        char attrbuf[256];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nlh.nlmsg_type = RTM_NEWROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;

    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_dst_len = 0;         /* 0.0.0.0/0 = default */
    req.rtm.rtm_table = RT_TABLE_MAIN;
    req.rtm.rtm_protocol = RTPROT_STATIC;
    req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    req.rtm.rtm_type = RTN_UNICAST;

    /* RTA_GATEWAY */
    gscope_nl_add_attr(&req.nlh, (int)sizeof(req),
                        RTA_GATEWAY, &gw, sizeof(gw));

    gscope_nl_send(fd, &req.nlh);
    int kerr = 0;
    int ret = gscope_nl_recv_ack(fd, &kerr);
    close(fd);

    if (ret < 0 && kerr != EEXIST)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "add default route via %s: error %d",
                                gateway, kerr);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)gateway;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}
