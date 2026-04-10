/*
 * gscope/types.h — Core types, enums, and configuration structures
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GSCOPE_TYPES_H
#define GSCOPE_TYPES_H

#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Opaque Handles ─────────────────────────────────────────────── */

typedef struct gscope_ctx     gscope_ctx_t;
typedef struct gscope_scope   gscope_scope_t;
typedef struct gscope_process gscope_process_t;

typedef uint32_t gscope_id_t;

/* ─── Scope State ────────────────────────────────────────────────── */

typedef enum {
    GSCOPE_STATE_CREATING  = 0,
    GSCOPE_STATE_STOPPED   = 1,
    GSCOPE_STATE_STARTING  = 2,
    GSCOPE_STATE_RUNNING   = 3,
    GSCOPE_STATE_STOPPING  = 4,
    GSCOPE_STATE_ERROR     = 5,
    GSCOPE_STATE_DELETING  = 6,
    GSCOPE_STATE_DELETED   = 7,
} gscope_state_t;

typedef enum {
    GSCOPE_HEALTH_UNKNOWN   = 0,
    GSCOPE_HEALTH_HEALTHY   = 1,
    GSCOPE_HEALTH_DEGRADED  = 2,
    GSCOPE_HEALTH_UNHEALTHY = 3,
} gscope_health_t;

/* ─── Isolation Level ────────────────────────────────────────────── */

typedef enum {
    GSCOPE_ISOLATION_MINIMAL   = 0,   /* NET only */
    GSCOPE_ISOLATION_STANDARD  = 1,   /* NET + PID + MNT + UTS */
    GSCOPE_ISOLATION_HIGH      = 2,   /* + IPC + cgroup + seccomp */
    GSCOPE_ISOLATION_MAXIMUM   = 3,   /* + USER namespace + strict seccomp */
} gscope_isolation_t;

/* ─── Network Mode ───────────────────────────────────────────────── */

typedef enum {
    GSCOPE_NET_ISOLATED = 0,   /* No external network */
    GSCOPE_NET_BRIDGE   = 1,   /* Connected to host bridge */
    GSCOPE_NET_HOST     = 2,   /* Share host network namespace */
} gscope_netmode_t;

/* ─── Privilege Level ────────────────────────────────────────────── */

typedef enum {
    GSCOPE_PRIV_RESTRICTED = 0,   /* Minimal capabilities */
    GSCOPE_PRIV_STANDARD   = 1,   /* Default set */
    GSCOPE_PRIV_ELEVATED   = 2,   /* Package install, service mgmt */
    GSCOPE_PRIV_ROOT       = 3,   /* Full root inside scope */
} gscope_privilege_t;

/* ─── Seccomp Profile ────────────────────────────────────────────── */

typedef enum {
    GSCOPE_SECCOMP_DEFAULT    = 0,   /* Block dangerous syscalls */
    GSCOPE_SECCOMP_STRICT     = 1,   /* Whitelist-only */
    GSCOPE_SECCOMP_PERMISSIVE = 2,   /* Allow most, block critical */
    GSCOPE_SECCOMP_DISABLED   = 3,   /* No filter (insecure) */
    GSCOPE_SECCOMP_CUSTOM     = 4,   /* Load from file */
} gscope_seccomp_t;

/* ─── Namespace Flags (bitmask) ──────────────────────────────────── */

#define GSCOPE_NS_PID    (1U << 0)   /* PID namespace */
#define GSCOPE_NS_NET    (1U << 1)   /* Network namespace */
#define GSCOPE_NS_MNT    (1U << 2)   /* Mount namespace */
#define GSCOPE_NS_UTS    (1U << 3)   /* UTS (hostname) namespace */
#define GSCOPE_NS_IPC    (1U << 4)   /* IPC namespace */
#define GSCOPE_NS_USER   (1U << 5)   /* User namespace */
#define GSCOPE_NS_CGROUP (1U << 6)   /* Cgroup namespace */

#define GSCOPE_NS_ALL    (0x7FU)

/* ─── Log Level ──────────────────────────────────────────────────── */

typedef enum {
    GSCOPE_LOG_TRACE = 0,
    GSCOPE_LOG_DEBUG = 1,
    GSCOPE_LOG_INFO  = 2,
    GSCOPE_LOG_WARN  = 3,
    GSCOPE_LOG_ERROR = 4,
    GSCOPE_LOG_FATAL = 5,
    GSCOPE_LOG_OFF   = 6,
} gscope_log_level_t;

/* Log callback signature */
typedef void (*gscope_log_fn)(gscope_log_level_t level,
                               const char *file,
                               int line,
                               const char *fmt,
                               va_list args,
                               void *userdata);

/* ─── Scope Configuration ────────────────────────────────────────── */

typedef struct {
    gscope_id_t        id;
    gscope_isolation_t isolation;
    gscope_netmode_t   net_mode;
    gscope_privilege_t privilege;
    uint32_t           ns_flags;       /* GSCOPE_NS_* bitmask, 0 = auto from isolation */

    /* Resource limits */
    float    cpu_cores;                /* 0.0 = unlimited */
    uint32_t cpu_weight;               /* 1-10000, 0 = default (100) */
    uint64_t memory_bytes;             /* 0 = unlimited */
    uint64_t memory_swap_bytes;        /* 0 = same as memory_bytes */
    uint32_t max_pids;                 /* 0 = unlimited */
    uint32_t io_weight;                /* 1-10000, 0 = default (100) */

    /* Rootfs */
    const char *template_path;         /* Lower layer for OverlayFS */
    const char *rootfs_base;           /* Base dir, NULL = /opt/gritiva/scopes */

    /* Network */
    const char *bridge_name;           /* NULL = "br-gscope" */
    const char *requested_ip;          /* NULL = auto-allocate */
    const char *gateway;               /* NULL = auto (bridge IP) */

    /* User */
    const char *username;              /* NULL = "root" */
    uid_t       uid;                   /* 0 = auto-allocate */
    gid_t       gid;                   /* 0 = auto-allocate */
    const char *password;              /* NULL = no password */

    /* Security */
    gscope_seccomp_t seccomp;          /* Seccomp profile */
    const char *seccomp_profile_path;  /* For GSCOPE_SECCOMP_CUSTOM */
    uint64_t    cap_keep;              /* Capabilities to keep (bitmask) */
    uint64_t    cap_drop;              /* Capabilities to drop (bitmask) */

    /* Misc */
    const char *hostname;              /* NULL = "scope-{id}" */
} gscope_config_t;

/* Initialize config with safe defaults */
static inline void gscope_config_init(gscope_config_t *c)
{
    /* Zero-fill then set defaults */
    __builtin_memset(c, 0, sizeof(*c));
    c->isolation  = GSCOPE_ISOLATION_STANDARD;
    c->net_mode   = GSCOPE_NET_BRIDGE;
    c->privilege  = GSCOPE_PRIV_STANDARD;
    c->cpu_cores  = 1.0f;
    c->cpu_weight = 100;
    c->memory_bytes = 512ULL * 1024 * 1024;  /* 512 MB */
    c->max_pids   = 1024;
    c->io_weight  = 100;
    c->seccomp    = GSCOPE_SECCOMP_DEFAULT;
}

/* ─── Scope Status ───────────────────────────────────────────────── */

typedef struct {
    gscope_id_t    id;
    gscope_state_t state;
    gscope_health_t health;
    pid_t          init_pid;           /* -1 if not running */
    int            pidfd;              /* -1 if unavailable */
    char           ip_address[16];     /* e.g. "10.50.0.10" */
    char           hostname[64];
    char           rootfs_path[4096];
    char           cgroup_path[256];
    uint64_t       created_at;         /* Unix timestamp */
    uint64_t       started_at;         /* 0 if never started */
} gscope_status_t;

/* ─── Metrics ────────────────────────────────────────────────────── */

typedef struct {
    /* CPU */
    uint64_t cpu_usage_us;             /* Total CPU time in microseconds */
    float    cpu_percent;              /* Current utilization % */

    /* Memory */
    uint64_t memory_current;           /* Current usage in bytes */
    uint64_t memory_limit;             /* Hard limit in bytes */
    float    memory_percent;

    /* PIDs */
    uint32_t pids_current;
    uint32_t pids_limit;

    /* Network */
    uint64_t net_rx_bytes;
    uint64_t net_tx_bytes;
    uint64_t net_rx_packets;
    uint64_t net_tx_packets;

    /* Disk */
    uint64_t disk_usage_bytes;
} gscope_metrics_t;

/* ─── Process Exec Config ────────────────────────────────────────── */

typedef struct {
    const char  *command;              /* e.g. "/bin/bash" */
    const char **argv;                 /* NULL-terminated */
    const char **envp;                 /* NULL-terminated, NULL = default env */
    const char  *work_dir;             /* NULL = "/" */
    bool         allocate_pty;         /* Allocate PTY for interactive use */
    uint16_t     pty_rows;             /* 0 = default (24) */
    uint16_t     pty_cols;             /* 0 = default (80) */
    uid_t        uid;                  /* 0 = scope's default uid */
    gid_t        gid;                  /* 0 = scope's default gid */
} gscope_exec_config_t;

/* ─── Process Exec Result ────────────────────────────────────────── */

typedef struct {
    pid_t pid;
    int   pidfd;                       /* -1 if unavailable */
    int   pty_fd;                      /* PTY master fd, -1 if no PTY */
    bool  has_pty;
} gscope_exec_result_t;

/* ─── Network Info ───────────────────────────────────────────────── */

typedef struct {
    char ip_address[16];
    char gateway[16];
    char bridge[32];
    char veth_host[16];
    char veth_scope[16];
    char netns_name[32];
} gscope_net_info_t;

typedef struct {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
} gscope_net_stats_t;

typedef struct {
    uint16_t host_port;
    uint16_t scope_port;
    uint8_t  protocol;                 /* IPPROTO_TCP or IPPROTO_UDP */
} gscope_port_map_t;

/* ─── Cgroup Limits ──────────────────────────────────────────────── */

typedef struct {
    float    cpu_cores;                /* 0 = unlimited */
    uint32_t cpu_weight;               /* 1-10000, 0 = default */
    uint64_t memory_bytes;             /* 0 = unlimited */
    uint64_t memory_swap_bytes;        /* 0 = same as memory */
    uint32_t pids_max;                 /* 0 = unlimited */
    uint32_t io_weight;                /* 1-10000, 0 = default */
} gscope_cgroup_limits_t;

typedef struct {
    uint64_t cpu_usage_us;
    uint64_t memory_current;
    uint64_t memory_max;
    uint64_t memory_swap;
    uint32_t pids_current;
    uint32_t pids_max;
} gscope_cgroup_stats_t;

/* ─── Rootfs Info ────────────────────────────────────────────────── */

typedef struct {
    char     lower[4096];
    char     upper[4096];
    char     work[4096];
    char     merged[4096];
    uint64_t upper_bytes;              /* Size of writable layer */
    bool     mounted;
} gscope_rootfs_info_t;

/* ─── User Info ──────────────────────────────────────────────────── */

typedef struct {
    char  username[64];
    uid_t uid;
    gid_t gid;
    char  home[4096];
    char  shell[256];
    bool  sudo_enabled;
    gscope_privilege_t privilege;
} gscope_user_info_t;

/* ─── Init Flags ─────────────────────────────────────────────────── */

#define GSCOPE_INIT_DEFAULT     0U
#define GSCOPE_INIT_RESTORE     (1U << 0)  /* Restore existing scopes from state files */
#define GSCOPE_INIT_VERBOSE     (1U << 1)  /* Enable verbose logging at init */

#ifdef __cplusplus
}
#endif

#endif /* GSCOPE_TYPES_H */
