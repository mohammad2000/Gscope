/*
 * net/addr.c — IP address management via netlink (RTM_NEWADDR)
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
 * Add an IPv4 address to an interface.
 *
 * ifname:  interface name (e.g. "br-gscope", "eth0")
 * addr:    IP in dotted notation (e.g. "10.50.0.1")
 * prefix:  prefix length (e.g. 24)
 */
gscope_err_t gscope_addr_add(const char *ifname, const char *addr, int prefix)
{
#ifdef __linux__
    if (!ifname || !addr || prefix < 0 || prefix > 32)
        return gscope_set_error(GSCOPE_ERR_INVAL, "invalid args");

    unsigned int idx = if_nametoindex(ifname);
    if (idx == 0)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "interface %s not found", ifname);

    struct in_addr ip;
    if (inet_pton(AF_INET, addr, &ip) != 1)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "invalid IP address: %s", addr);

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NETLINK, "socket failed");

    struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
    bind(fd, (struct sockaddr *)&nladdr, sizeof(nladdr));

    struct {
        struct nlmsghdr nlh;
        struct ifaddrmsg ifa;
        char attrbuf[256];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nlh.nlmsg_type = RTM_NEWADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
    req.ifa.ifa_family = AF_INET;
    req.ifa.ifa_prefixlen = (unsigned char)prefix;
    req.ifa.ifa_index = (int)idx;
    req.ifa.ifa_scope = 253;  /* RT_SCOPE_LINK — bridge-local, prevents kernel default route */

    /* IFA_LOCAL = the IP address */
    gscope_nl_add_attr(&req.nlh, (int)sizeof(req),
                        IFA_LOCAL, &ip, sizeof(ip));

    /* IFA_ADDRESS = same for point-to-point */
    gscope_nl_add_attr(&req.nlh, (int)sizeof(req),
                        IFA_ADDRESS, &ip, sizeof(ip));

    gscope_nl_send(fd, &req.nlh);
    int kerr = 0;
    int ret = gscope_nl_recv_ack(fd, &kerr);
    close(fd);

    if (ret < 0 && kerr != EEXIST)
        return gscope_set_error(GSCOPE_ERR_NETWORK,
                                "add addr %s/%d to %s: error %d",
                                addr, prefix, ifname, kerr);

    gscope_clear_error();
    return GSCOPE_OK;
#else
    (void)ifname; (void)addr; (void)prefix;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED, "requires Linux");
#endif
}
