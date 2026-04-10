/*
 * net/netlink.c — Netlink socket abstraction
 *
 * Netlink is the Linux kernel's IPC mechanism for network configuration.
 * Instead of spawning `ip` commands as subprocess, we talk directly
 * to the kernel via AF_NETLINK sockets.
 *
 * Message format:
 *   struct nlmsghdr — header (type, flags, seq, pid)
 *   payload        — depends on message type (RTM_NEWLINK, etc.)
 *   attributes     — TLV (type-length-value) chain via struct nlattr
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>
#endif

/* ─── Netlink Socket ─────────────────────────────────────────────── */

int gscope_nl_open(void)
{
#ifdef __linux__
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,      /* Let kernel assign */
        .nl_groups = 0,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
#else
    errno = ENOSYS;
    return -1;
#endif
}

void gscope_nl_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* ─── Send / Receive ─────────────────────────────────────────────── */

#ifdef __linux__

int gscope_nl_send(int fd, struct nlmsghdr *nlh)
{
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
    };

    struct iovec iov = {
        .iov_base = nlh,
        .iov_len = nlh->nlmsg_len,
    };

    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    ssize_t n = sendmsg(fd, &msg, 0);
    return (n < 0) ? -1 : 0;
}

/*
 * Receive netlink response and check for errors.
 * Returns 0 on success (ACK), -1 on error.
 * If errmsg is provided, kernel error code is stored there.
 */
int gscope_nl_recv_ack(int fd, int *errmsg)
{
    char buf[4096];
    struct sockaddr_nl addr;
    struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    ssize_t n = recvmsg(fd, &msg, 0);
    if (n < 0)
        return -1;

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;

    for (; NLMSG_OK(nlh, (unsigned int)n); nlh = NLMSG_NEXT(nlh, n)) {
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = NLMSG_DATA(nlh);
            if (errmsg)
                *errmsg = -err->error;
            if (err->error == 0)
                return 0;   /* ACK — success */
            errno = -err->error;
            return -1;
        }
        if (nlh->nlmsg_type == NLMSG_DONE)
            return 0;
    }

    return 0;
}

/* ─── Attribute Helpers ──────────────────────────────────────────── */

/*
 * Add a netlink attribute (NLA) to a message.
 * Returns pointer past the added attribute.
 */
void *gscope_nl_add_attr(struct nlmsghdr *nlh, int maxlen,
                          int type, const void *data, int len)
{
    int attr_len = NLA_HDRLEN + len;
    int aligned_len = NLA_ALIGN(attr_len);

    if ((int)nlh->nlmsg_len + aligned_len > maxlen)
        return NULL;

    struct nlattr *nla = (struct nlattr *)((char *)nlh + nlh->nlmsg_len);
    nla->nla_type = (unsigned short)type;
    nla->nla_len = (unsigned short)attr_len;

    if (data && len > 0)
        memcpy((char *)nla + NLA_HDRLEN, data, (size_t)len);

    /* Zero padding */
    if (aligned_len > attr_len)
        memset((char *)nla + attr_len, 0, (size_t)(aligned_len - attr_len));

    nlh->nlmsg_len += (unsigned int)aligned_len;
    return (char *)nla + aligned_len;
}

/*
 * Add a string attribute.
 */
void *gscope_nl_add_attr_str(struct nlmsghdr *nlh, int maxlen,
                              int type, const char *str)
{
    return gscope_nl_add_attr(nlh, maxlen, type, str, (int)strlen(str) + 1);
}

/*
 * Add a 32-bit integer attribute.
 */
void *gscope_nl_add_attr_u32(struct nlmsghdr *nlh, int maxlen,
                              int type, uint32_t value)
{
    return gscope_nl_add_attr(nlh, maxlen, type, &value, 4);
}

/*
 * Begin a nested attribute. Returns the nla pointer for closing.
 */
struct nlattr *gscope_nl_nest_start(struct nlmsghdr *nlh, int maxlen, int type)
{
    struct nlattr *nla = (struct nlattr *)((char *)nlh + nlh->nlmsg_len);
    int space = NLA_HDRLEN;

    if ((int)nlh->nlmsg_len + space > maxlen)
        return NULL;

    nla->nla_type = (unsigned short)type;
    nla->nla_len = (unsigned short)space;  /* Will be updated in nest_end */
    nlh->nlmsg_len += (unsigned int)space;

    return nla;
}

/*
 * Close a nested attribute (updates the length).
 */
void gscope_nl_nest_end(struct nlmsghdr *nlh, struct nlattr *start)
{
    start->nla_len = (unsigned short)((char *)nlh + nlh->nlmsg_len - (char *)start);
}

/*
 * Get interface index by name.
 */
int gscope_nl_if_nametoindex(const char *name)
{
    unsigned int idx = if_nametoindex(name);
    return (idx > 0) ? (int)idx : -1;
}

#endif /* __linux__ */
