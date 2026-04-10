/*
 * compat.h — Kernel feature detection and syscall wrappers
 *
 * Provides fallback implementations for newer Linux syscalls
 * that may not be available in older glibc or kernel versions.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_COMPAT_H
#define GSCOPE_COMPAT_H

#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/types.h>
#else
/* macOS / non-Linux stubs for cross-platform compilation.
 * Actual runtime requires Linux. */
typedef uint64_t __aligned_u64;
typedef uint32_t __u32;
#ifndef SYS_clone3
#define __NR_clone3 -1
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open -1
#endif
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal -1
#endif
#ifndef __NR_pivot_root
#define __NR_pivot_root -1
#endif
#ifndef __NR_capget
#define __NR_capget -1
#define __NR_capset -1
#endif
#endif /* __linux__ */

/* ─── Namespace clone flags ──────────────────────────────────────── */

#ifndef CLONE_NEWNS
#define CLONE_NEWNS     0x00020000U
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS    0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC    0x08000000
#endif
#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER   0x10000000
#endif
#ifndef CLONE_NEWPID
#define CLONE_NEWPID    0x20000000
#endif
#ifndef CLONE_NEWNET
#define CLONE_NEWNET    0x40000000
#endif
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

/* ─── clone3 (Linux 5.3+) ───────────────────────────────────────── */

#ifdef __linux__
#  ifndef __NR_clone3
#    if defined(__x86_64__)
#      define __NR_clone3 435
#    elif defined(__aarch64__)
#      define __NR_clone3 435
#    else
#      define __NR_clone3 -1
#    endif
#  endif
#endif

#ifndef HAVE_CLONE3
struct clone_args {
    __aligned_u64 flags;
    __aligned_u64 pidfd;
    __aligned_u64 child_tid;
    __aligned_u64 parent_tid;
    __aligned_u64 exit_signal;
    __aligned_u64 stack;
    __aligned_u64 stack_size;
    __aligned_u64 tls;
    __aligned_u64 set_tid;
    __aligned_u64 set_tid_size;
    __aligned_u64 cgroup;
};

static inline long gscope_clone3(struct clone_args *args, size_t size)
{
#if defined(__linux__) && defined(__NR_clone3) && __NR_clone3 > 0
    return syscall(__NR_clone3, args, size);
#else
    (void)args; (void)size;
    errno = ENOSYS;
    return -1;
#endif
}
#endif /* HAVE_CLONE3 */

/* ─── pidfd_open (Linux 5.3+) ────────────────────────────────────── */

#ifdef __linux__
#  ifndef __NR_pidfd_open
#    if defined(__x86_64__)
#      define __NR_pidfd_open 434
#    elif defined(__aarch64__)
#      define __NR_pidfd_open 434
#    else
#      define __NR_pidfd_open -1
#    endif
#  endif
#endif

static inline int gscope_pidfd_open(pid_t pid, unsigned int flags)
{
#if defined(__linux__) && defined(__NR_pidfd_open) && __NR_pidfd_open > 0
    return (int)syscall(__NR_pidfd_open, pid, flags);
#else
    (void)pid; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

/* ─── pidfd_send_signal (Linux 5.1+) ─────────────────────────────── */

#ifdef __linux__
#  ifndef __NR_pidfd_send_signal
#    if defined(__x86_64__)
#      define __NR_pidfd_send_signal 424
#    elif defined(__aarch64__)
#      define __NR_pidfd_send_signal 424
#    else
#      define __NR_pidfd_send_signal -1
#    endif
#  endif
#endif

static inline int gscope_pidfd_send_signal(int pidfd, int sig,
                                            siginfo_t *info,
                                            unsigned int flags)
{
#if defined(__linux__) && defined(__NR_pidfd_send_signal) && __NR_pidfd_send_signal > 0
    return (int)syscall(__NR_pidfd_send_signal, pidfd, sig, info, flags);
#else
    (void)pidfd; (void)sig; (void)info; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

/* ─── pivot_root (available since Linux 2.3.41) ──────────────────── */

#ifdef __linux__
#  ifndef __NR_pivot_root
#    if defined(__x86_64__)
#      define __NR_pivot_root 155
#    elif defined(__aarch64__)
#      define __NR_pivot_root 41
#    endif
#  endif
#endif

static inline int gscope_pivot_root(const char *new_root, const char *put_old)
{
#if defined(__linux__) && defined(__NR_pivot_root) && __NR_pivot_root > 0
    return (int)syscall(__NR_pivot_root, new_root, put_old);
#else
    (void)new_root; (void)put_old;
    errno = ENOSYS;
    return -1;
#endif
}

/* ─── Capability syscalls ────────────────────────────────────────── */

#ifdef __linux__
#  ifndef __NR_capget
#    if defined(__x86_64__)
#      define __NR_capget 125
#      define __NR_capset 126
#    elif defined(__aarch64__)
#      define __NR_capget 90
#      define __NR_capset 91
#    endif
#  endif
#endif

struct gscope_cap_header {
    __u32 version;
    int   pid;
};

struct gscope_cap_data {
    __u32 effective;
    __u32 permitted;
    __u32 inheritable;
};

/* Version 3 header: two data structs for caps > 31 */
#define GSCOPE_LINUX_CAPABILITY_VERSION_3 0x20080522

static inline int gscope_capget(struct gscope_cap_header *hdr,
                                 struct gscope_cap_data *data)
{
#if defined(__linux__) && defined(__NR_capget) && __NR_capget > 0
    return (int)syscall(__NR_capget, hdr, data);
#else
    (void)hdr; (void)data;
    errno = ENOSYS;
    return -1;
#endif
}

static inline int gscope_capset(struct gscope_cap_header *hdr,
                                 const struct gscope_cap_data *data)
{
#if defined(__linux__) && defined(__NR_capset) && __NR_capset > 0
    return (int)syscall(__NR_capset, hdr, data);
#else
    (void)hdr; (void)data;
    errno = ENOSYS;
    return -1;
#endif
}

/* ─── Feature probing ────────────────────────────────────────────── */

/* Check if a syscall number is supported (returns true if it doesn't give ENOSYS) */
static inline bool gscope_probe_syscall(long nr)
{
#ifdef __linux__
    if (nr < 0) return false;
    /* Call with invalid args — we just want to check if ENOSYS is returned */
    long ret = syscall(nr, NULL, 0);
    if (ret == -1 && errno == ENOSYS)
        return false;
    /* Any other error (EFAULT, EINVAL, etc.) means the syscall exists */
    return true;
#else
    (void)nr;
    return false;
#endif
}

#endif /* GSCOPE_COMPAT_H */
