/*
 * internal.h — Shared internal declarations (not part of public API)
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_INTERNAL_H
#define GSCOPE_INTERNAL_H

#include <gscope/types.h>
#include <gscope/error.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ─── Internal Scope Structure ───────────────────────────────────── */

#define GSCOPE_NS_COUNT 7    /* PID, NET, MNT, UTS, IPC, USER, CGROUP */

struct gscope_scope {
    gscope_id_t        id;
    gscope_state_t     state;
    gscope_health_t    health;
    gscope_config_t    config;
    gscope_ctx_t      *ctx;

    /* Namespace state */
    pid_t    init_pid;              /* -1 if not running */
    int      pidfd;                 /* -1 if unavailable */
    int      ns_fds[GSCOPE_NS_COUNT]; /* One fd per namespace type, -1 if unused */
    uint32_t ns_active;             /* Bitmask of created namespaces */
    char     netns_name[32];        /* e.g. "gscope-42" */

    /* Cgroup */
    char     cgroup_path[256];      /* e.g. "/sys/fs/cgroup/gscope.slice/scope-42" */

    /* Network */
    char     ip_address[16];
    char     veth_host[16];         /* e.g. "gs42h" */
    char     veth_scope[16];        /* e.g. "gs42s" → renamed to "eth0" inside */
    uint32_t ip_alloc_idx;          /* Index in allocator bitmap */

    /* Rootfs */
    char     rootfs_base[4096];
    char     rootfs_lower[4096];
    char     rootfs_upper[4096];
    char     rootfs_work[4096];
    char     rootfs_merged[4096];
    bool     rootfs_mounted;

    /* User */
    char     username[64];
    uid_t    uid;
    gid_t    gid;

    /* Timestamps */
    uint64_t created_at;
    uint64_t started_at;

    /* Synchronization */
    pthread_mutex_t lock;

    /* Linked list for ctx->scopes */
    struct gscope_scope *next;
};

/* ─── Internal Context Structure ─────────────────────────────────── */

/* IP allocator for a /24 subnet — 256 bits = 4 * uint64_t */
#define GSCOPE_IP_BITMAP_SIZE 4

struct gscope_ctx {
    pthread_mutex_t   lock;
    gscope_scope_t   *scopes;          /* Linked list head */
    uint32_t          scope_count;

    /* Shared netlink socket (for networking operations) */
    int               nl_sock;          /* -1 if not initialized */
    uint32_t          nl_seq;           /* Netlink sequence number */

    /* Bridge */
    char              bridge_name[32];  /* Default: "br-gscope" */
    bool              bridge_created;

    /* IP allocator (10.50.0.0/24) */
    struct {
        uint32_t base;                  /* Network address (host order) */
        uint32_t gateway;               /* Gateway IP (host order) */
        uint8_t  prefix_len;
        uint8_t  first_host;            /* First allocatable offset (e.g. 10) */
        uint8_t  last_host;             /* Last allocatable offset (e.g. 254) */
        uint64_t bitmap[GSCOPE_IP_BITMAP_SIZE]; /* 1 = allocated */
        pthread_mutex_t lock;
    } ip_alloc;

    /* Feature detection (probed at init) */
    struct {
        bool has_clone3;
        bool has_pidfd_open;
        bool has_cgroup_kill;
        bool has_cgroup_freeze;
        bool has_pivot_root;
        bool has_seccomp;
        int  cgroup_version;            /* 1 or 2 */
    } features;

    /* Logging */
    gscope_log_fn    log_fn;
    void            *log_userdata;
    gscope_log_level_t log_level;
};

/* ─── Error Setting (internal) ───────────────────────────────────── */

/*
 * Set the thread-local error state.
 * Returns the error code for convenient "return gscope_set_error(...)".
 */
gscope_err_t gscope_set_error(gscope_err_t code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Set error with captured errno.
 */
gscope_err_t gscope_set_error_errno(gscope_err_t code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Clear the error state.
 */
void gscope_clear_error(void);

/* ─── Logging (internal) ─────────────────────────────────────────── */

void gscope_log(gscope_ctx_t *ctx, gscope_log_level_t level,
                const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

#define GSCOPE_LOG(ctx, level, ...) \
    gscope_log((ctx), (level), __FILE__, __LINE__, __VA_ARGS__)

#define GSCOPE_TRACE(ctx, ...) GSCOPE_LOG(ctx, GSCOPE_LOG_TRACE, __VA_ARGS__)
#define GSCOPE_DEBUG(ctx, ...) GSCOPE_LOG(ctx, GSCOPE_LOG_DEBUG, __VA_ARGS__)
#define GSCOPE_INFO(ctx, ...)  GSCOPE_LOG(ctx, GSCOPE_LOG_INFO,  __VA_ARGS__)
#define GSCOPE_WARN(ctx, ...)  GSCOPE_LOG(ctx, GSCOPE_LOG_WARN,  __VA_ARGS__)
#define GSCOPE_ERROR(ctx, ...) GSCOPE_LOG(ctx, GSCOPE_LOG_ERROR, __VA_ARGS__)
#define GSCOPE_FATAL(ctx, ...) GSCOPE_LOG(ctx, GSCOPE_LOG_FATAL, __VA_ARGS__)

/* ─── Utility ────────────────────────────────────────────────────── */

/* Safe string copy — always NUL-terminates */
static inline void gscope_strlcpy(char *dst, const char *src, size_t size)
{
    if (size == 0) return;
    size_t i;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Write a string to a file (for cgroup, sysfs, procfs) */
int gscope_write_file(const char *path, const char *value);

/* Read a file into a buffer */
int gscope_read_file(const char *path, char *buf, size_t size);

/* Read a uint64 from a file */
int gscope_read_uint64(const char *path, uint64_t *value);

/* Read a uint32 from a file */
int gscope_read_uint32(const char *path, uint32_t *value);

/* Recursive mkdir */
int gscope_mkdir_p(const char *path, mode_t mode);

/* Recursive rmdir */
int gscope_rmdir_r(const char *path);

/* Get monotonic timestamp in seconds */
uint64_t gscope_now(void);

#endif /* GSCOPE_INTERNAL_H */
