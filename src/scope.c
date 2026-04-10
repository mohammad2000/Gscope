/*
 * scope.c — Scope lifecycle orchestrator
 *
 * This is the integration layer that ties all subsystems together.
 * It orchestrates the creation, starting, stopping, and deletion
 * of isolated scopes by calling into the individual modules.
 *
 * Create order (with rollback on failure):
 *   1. Allocate scope struct + add to context list
 *   2. Create directory structure (rootfs)
 *   3. Mount OverlayFS (if template)
 *   4. Create cgroup + set resource limits
 *   5. Create namespaces (NET is persistent/named)
 *   6. Setup networking (bridge + veth + IP + route)
 *   7. Create user in rootfs
 *
 * Delete order (reverse):
 *   7. Delete user
 *   6. Teardown networking
 *   5. Delete namespaces
 *   4. Delete cgroup
 *   3. Unmount OverlayFS
 *   2. Remove directories
 *   1. Remove from context list + free struct
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "internal.h"
#include "compat.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define GSCOPE_VERSION "0.1.0"

/* Forward declarations for own functions used before definition */
gscope_scope_t *gscope_scope_get(gscope_ctx_t *ctx, gscope_id_t id);

/* Forward declarations for subsystem functions */
extern gscope_err_t gscope_cgroup_create(gscope_scope_t *, const gscope_cgroup_limits_t *);
extern gscope_err_t gscope_cgroup_update(gscope_scope_t *, const gscope_cgroup_limits_t *);
extern gscope_err_t gscope_cgroup_add_pid(gscope_scope_t *, pid_t);
extern gscope_err_t gscope_cgroup_stats(gscope_scope_t *, gscope_cgroup_stats_t *);
extern gscope_err_t gscope_cgroup_kill(gscope_scope_t *, int);
extern gscope_err_t gscope_cgroup_delete(gscope_scope_t *);

extern gscope_err_t gscope_rootfs_setup(gscope_scope_t *, const char *);
extern gscope_err_t gscope_rootfs_teardown(gscope_scope_t *);

extern gscope_err_t gscope_overlay_mount(gscope_scope_t *);
extern gscope_err_t gscope_overlay_unmount(gscope_scope_t *);

extern gscope_err_t gscope_ns_create(gscope_scope_t *, uint32_t);
extern gscope_err_t gscope_ns_delete(gscope_scope_t *);

extern gscope_err_t gscope_bridge_create(gscope_ctx_t *, const char *);
extern gscope_err_t gscope_veth_create(gscope_scope_t *);
extern gscope_err_t gscope_veth_move_to_ns(gscope_scope_t *);
extern gscope_err_t gscope_veth_attach_bridge(gscope_scope_t *, const char *);
extern gscope_err_t gscope_veth_delete(gscope_scope_t *);
extern gscope_err_t gscope_addr_add(const char *, const char *, int);
extern gscope_err_t gscope_fw_setup_nat(gscope_ctx_t *, const char *, const char *);
extern gscope_err_t gscope_fw_cleanup(gscope_ctx_t *, const char *);
extern gscope_err_t gscope_fw_port_forward(gscope_ctx_t *, const gscope_port_map_t *, const char *);
extern gscope_err_t gscope_fw_port_remove(gscope_ctx_t *, const gscope_port_map_t *, const char *);

extern gscope_err_t gscope_ip_alloc(gscope_ctx_t *, char *, size_t);
extern gscope_err_t gscope_ip_alloc_specific(gscope_ctx_t *, const char *);
extern gscope_err_t gscope_ip_free(gscope_ctx_t *, const char *);
extern void gscope_ip_gateway(gscope_ctx_t *, char *, size_t);

extern gscope_err_t gscope_user_create(gscope_scope_t *, const char *, uid_t, gid_t, gscope_privilege_t);
extern gscope_err_t gscope_user_delete(gscope_scope_t *, const char *);

/* State persistence */
extern gscope_err_t gscope_state_save(gscope_scope_t *);
extern gscope_err_t gscope_state_delete(uint32_t);
extern int gscope_state_restore_all(gscope_ctx_t *);

extern gscope_err_t gscope_exec(gscope_scope_t *, const gscope_exec_config_t *, gscope_exec_result_t *);
extern void gscope_process_release(gscope_exec_result_t *);
extern int gscope_stop_process(int, pid_t, int);

/* ─── Feature Detection ──────────────────────────────────────────── */

static void detect_features(gscope_ctx_t *ctx)
{
    ctx->features.has_clone3 = gscope_probe_syscall(__NR_clone3);
    ctx->features.has_pidfd_open = gscope_probe_syscall(__NR_pidfd_open);
    ctx->features.has_pivot_root = true;

    struct stat st;
    ctx->features.has_seccomp = (stat("/proc/sys/kernel/seccomp", &st) == 0);

    struct stat cg2;
    if (stat("/sys/fs/cgroup/cgroup.controllers", &cg2) == 0)
        ctx->features.cgroup_version = 2;
    else
        ctx->features.cgroup_version = 1;

    ctx->features.has_cgroup_freeze = (ctx->features.cgroup_version == 2);
    ctx->features.has_cgroup_kill = (ctx->features.cgroup_version == 2);
}

/* ─── IP Allocator Init ──────────────────────────────────────────── */

static void init_ip_allocator(gscope_ctx_t *ctx)
{
    ctx->ip_alloc.base = (10U << 24) | (50U << 16) | 0U;
    ctx->ip_alloc.gateway = ctx->ip_alloc.base | 1U;
    ctx->ip_alloc.prefix_len = 24;
    ctx->ip_alloc.first_host = 10;
    ctx->ip_alloc.last_host = 254;
    memset(ctx->ip_alloc.bitmap, 0, sizeof(ctx->ip_alloc.bitmap));
    pthread_mutex_init(&ctx->ip_alloc.lock, NULL);
    ctx->ip_alloc.bitmap[0] |= (1ULL << 0) | (1ULL << 1);
}

/* ─── Scope List Management ──────────────────────────────────────── */

static void ctx_add_scope(gscope_ctx_t *ctx, gscope_scope_t *scope)
{
    pthread_mutex_lock(&ctx->lock);
    scope->next = ctx->scopes;
    ctx->scopes = scope;
    ctx->scope_count++;
    pthread_mutex_unlock(&ctx->lock);
}

static void ctx_remove_scope(gscope_ctx_t *ctx, gscope_scope_t *scope)
{
    pthread_mutex_lock(&ctx->lock);
    gscope_scope_t **pp = &ctx->scopes;
    while (*pp) {
        if (*pp == scope) {
            *pp = scope->next;
            ctx->scope_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&ctx->lock);
}

/* ─── Library Lifecycle ──────────────────────────────────────────── */

const char *gscope_version(void)
{
    return GSCOPE_VERSION;
}

gscope_err_t gscope_init(gscope_ctx_t **out, unsigned int flags)
{
    if (!out)
        return gscope_set_error(GSCOPE_ERR_INVAL, "out pointer is NULL");

#ifdef __linux__
    if (geteuid() != 0)
        return gscope_set_error(GSCOPE_ERR_PERM,
                                "gscope requires root privileges");
#endif

    gscope_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return gscope_set_error(GSCOPE_ERR_NOMEM, "failed to allocate context");

    pthread_mutex_init(&ctx->lock, NULL);
    ctx->nl_sock = -1;
    gscope_strlcpy(ctx->bridge_name, "br-gscope", sizeof(ctx->bridge_name));

    ctx->log_level = (flags & GSCOPE_INIT_VERBOSE)
                     ? GSCOPE_LOG_DEBUG : GSCOPE_LOG_INFO;

    detect_features(ctx);

    GSCOPE_INFO(ctx, "gscope %s initializing", GSCOPE_VERSION);
    GSCOPE_INFO(ctx, "features: clone3=%s pidfd=%s seccomp=%s cgroup=v%d",
                ctx->features.has_clone3 ? "yes" : "no",
                ctx->features.has_pidfd_open ? "yes" : "no",
                ctx->features.has_seccomp ? "yes" : "no",
                ctx->features.cgroup_version);

    init_ip_allocator(ctx);
    gscope_mkdir_p("/opt/gritiva/scopes", 0755);

    /* Restore existing scopes from state files */
    if (flags & GSCOPE_INIT_RESTORE) {
        int restored = gscope_state_restore_all(ctx);
        if (restored > 0)
            GSCOPE_INFO(ctx, "restored %d scope(s) from state files", restored);
    }

    GSCOPE_INFO(ctx, "gscope initialized successfully");

    *out = ctx;
    gscope_clear_error();
    return GSCOPE_OK;
}

void gscope_destroy(gscope_ctx_t *ctx)
{
    if (!ctx) return;
    GSCOPE_INFO(ctx, "gscope shutting down (%u scopes active)", ctx->scope_count);
    if (ctx->nl_sock >= 0) close(ctx->nl_sock);
    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->ip_alloc.lock);
    free(ctx);
}

/* ═══════════════════════════════════════════════════════════════════
 * SCOPE CREATE — the main orchestration function
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_scope_create(gscope_ctx_t *ctx,
                                  const gscope_config_t *config,
                                  gscope_scope_t **out)
{
    if (!ctx || !config || !out)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx, config, or out");

    if (config->id == 0)
        return gscope_set_error(GSCOPE_ERR_INVAL, "scope ID cannot be 0");

    /* Check if already exists */
    if (gscope_scope_get(ctx, config->id))
        return gscope_set_error(GSCOPE_ERR_EXIST,
                                "scope %u already exists", config->id);

    GSCOPE_INFO(ctx, "═══ creating scope %u ═══", config->id);

    /* Step 0: Allocate scope struct */
    gscope_scope_t *scope = calloc(1, sizeof(*scope));
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_NOMEM, "failed to allocate scope");

    scope->id = config->id;
    scope->state = GSCOPE_STATE_CREATING;
    scope->health = GSCOPE_HEALTH_UNKNOWN;
    scope->config = *config;
    scope->ctx = ctx;
    scope->init_pid = -1;
    scope->pidfd = -1;
    scope->created_at = gscope_now();
    pthread_mutex_init(&scope->lock, NULL);

    for (int i = 0; i < GSCOPE_NS_COUNT; i++)
        scope->ns_fds[i] = -1;

    /* Track rollback progress */
    int step = 0;
    gscope_err_t err;

    /* Step 1: Create directory structure + rootfs */
    GSCOPE_INFO(ctx, "  [1/7] rootfs setup");
    err = gscope_rootfs_setup(scope, config->template_path);
    if (err != GSCOPE_OK) goto rollback;
    step = 1;

    /* Step 2: Mount OverlayFS (if template available) */
    if (scope->rootfs_lower[0] != '\0') {
        GSCOPE_INFO(ctx, "  [2/7] overlay mount");
        err = gscope_overlay_mount(scope);
        if (err != GSCOPE_OK) goto rollback;
    } else {
        GSCOPE_DEBUG(ctx, "  [2/7] overlay skip (no template)");
    }
    step = 2;

    /* Step 3: Create cgroup */
    GSCOPE_INFO(ctx, "  [3/7] cgroup create");
    gscope_cgroup_limits_t limits = {
        .cpu_cores = config->cpu_cores,
        .cpu_weight = config->cpu_weight,
        .memory_bytes = config->memory_bytes,
        .memory_swap_bytes = config->memory_swap_bytes,
        .pids_max = config->max_pids,
        .io_weight = config->io_weight,
    };
    err = gscope_cgroup_create(scope, &limits);
    if (err != GSCOPE_OK) goto rollback;
    step = 3;

    /* Step 4: Create namespaces */
    GSCOPE_INFO(ctx, "  [4/7] namespace create");
    err = gscope_ns_create(scope, config->ns_flags);
    if (err != GSCOPE_OK) goto rollback;
    step = 4;

    /* Step 5: Setup networking */
    if (config->net_mode == GSCOPE_NET_BRIDGE) {
        GSCOPE_INFO(ctx, "  [5/7] network setup (bridge mode)");

        /* Ensure bridge exists */
        gscope_bridge_create(ctx, ctx->bridge_name);

        /* Bridge needs an IP (gateway) */
        char gw[16];
        gscope_ip_gateway(ctx, gw, sizeof(gw));
        gscope_addr_add(ctx->bridge_name, gw, 24);

        /* Create veth pair */
        err = gscope_veth_create(scope);
        if (err != GSCOPE_OK) goto rollback;

        /* Move scope side to namespace */
        err = gscope_veth_move_to_ns(scope);
        if (err != GSCOPE_OK) goto rollback;

        /* Attach host side to bridge */
        gscope_veth_attach_bridge(scope, ctx->bridge_name);

        /* Allocate IP */
        if (config->requested_ip) {
            gscope_ip_alloc_specific(ctx, config->requested_ip);
            gscope_strlcpy(scope->ip_address, config->requested_ip,
                           sizeof(scope->ip_address));
        } else {
            err = gscope_ip_alloc(ctx, scope->ip_address,
                                   sizeof(scope->ip_address));
            if (err != GSCOPE_OK) goto rollback;
        }

        /* Setup NAT */
        gscope_fw_setup_nat(ctx, "10.50.0.0/24", ctx->bridge_name);

        GSCOPE_INFO(ctx, "  network: ip=%s bridge=%s",
                    scope->ip_address, ctx->bridge_name);
    } else if (config->net_mode == GSCOPE_NET_ISOLATED) {
        GSCOPE_DEBUG(ctx, "  [5/7] network: isolated (no external)");
    } else {
        GSCOPE_DEBUG(ctx, "  [5/7] network: host mode");
    }
    step = 5;

    /* Step 6: Create user in rootfs */
    GSCOPE_INFO(ctx, "  [6/7] user create");
    {
        const char *username = config->username ? config->username : "root";
        uid_t uid = config->uid > 0 ? config->uid : 0;
        gid_t gid = config->gid > 0 ? config->gid : 0;

        err = gscope_user_create(scope, username, uid, gid, config->privilege);
        if (err != GSCOPE_OK) goto rollback;
    }
    step = 6;

    /* Step 7: Finalize */
    GSCOPE_INFO(ctx, "  [7/7] finalize");
    scope->state = GSCOPE_STATE_STOPPED;
    scope->health = GSCOPE_HEALTH_HEALTHY;

    ctx_add_scope(ctx, scope);

    /* Save state to disk */
    gscope_state_save(scope);

    GSCOPE_INFO(ctx, "═══ scope %u created successfully ═══", scope->id);
    GSCOPE_INFO(ctx, "  rootfs: %s", scope->rootfs_merged);
    GSCOPE_INFO(ctx, "  cgroup: %s", scope->cgroup_path);
    GSCOPE_INFO(ctx, "  netns:  %s", scope->netns_name);
    GSCOPE_INFO(ctx, "  ip:     %s", scope->ip_address);
    GSCOPE_INFO(ctx, "  user:   %s (uid=%d)", scope->username, (int)scope->uid);

    *out = scope;
    gscope_clear_error();
    return GSCOPE_OK;

rollback:
    GSCOPE_ERROR(ctx, "scope %u creation failed at step %d: %s",
                 config->id, step + 1, gscope_strerror());

    /* Reverse order cleanup */
    if (step >= 5 && scope->ip_address[0])
        gscope_ip_free(ctx, scope->ip_address);
    if (step >= 5)
        gscope_veth_delete(scope);
    if (step >= 4)
        gscope_ns_delete(scope);
    if (step >= 3)
        gscope_cgroup_delete(scope);
    if (step >= 2)
        gscope_overlay_unmount(scope);
    if (step >= 1)
        gscope_rootfs_teardown(scope);

    scope->state = GSCOPE_STATE_ERROR;
    pthread_mutex_destroy(&scope->lock);
    free(scope);

    return err;
}

/* ═══════════════════════════════════════════════════════════════════
 * SCOPE START
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_scope_start(gscope_scope_t *scope, const char *init_command)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (scope->state != GSCOPE_STATE_STOPPED)
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "scope %u not in STOPPED state (state=%d)",
                                scope->id, scope->state);

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "starting scope %u", scope->id);
    scope->state = GSCOPE_STATE_STARTING;

    /* Build exec config for init process */
    const char *cmd = init_command ? init_command : "/bin/sleep";
    const char *argv_sleep[] = { "/bin/sleep", "infinity", NULL };
    const char *argv_custom[] = { cmd, NULL };

    gscope_exec_config_t exec_cfg = {
        .command = cmd,
        .argv = init_command ? argv_custom : argv_sleep,
        .envp = NULL,
        .work_dir = "/",
        .allocate_pty = false,
        .uid = scope->uid,
        .gid = scope->gid,
    };

    gscope_exec_result_t result;
    gscope_err_t err = gscope_exec(scope, &exec_cfg, &result);

    if (err != GSCOPE_OK) {
        scope->state = GSCOPE_STATE_ERROR;
        return err;
    }

    scope->init_pid = result.pid;
    scope->pidfd = result.pidfd;
    scope->started_at = gscope_now();

    /* Add init process to cgroup */
    if (scope->cgroup_path[0])
        gscope_cgroup_add_pid(scope, result.pid);

    scope->state = GSCOPE_STATE_RUNNING;
    scope->health = GSCOPE_HEALTH_HEALTHY;

    GSCOPE_INFO(ctx, "scope %u started (pid=%d, pidfd=%d)",
                scope->id, (int)result.pid, result.pidfd);

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * SCOPE STOP
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_scope_stop(gscope_scope_t *scope, unsigned int timeout_sec)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (scope->state != GSCOPE_STATE_RUNNING)
        return gscope_set_error(GSCOPE_ERR_STATE,
                                "scope %u not running", scope->id);

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "stopping scope %u (timeout=%us)", scope->id, timeout_sec);
    scope->state = GSCOPE_STATE_STOPPING;

    /* Kill all processes in cgroup first */
    if (scope->cgroup_path[0])
        gscope_cgroup_kill(scope, SIGTERM);

    /* Stop init process */
    if (scope->init_pid > 0) {
        int ret = gscope_stop_process(scope->pidfd, scope->init_pid,
                                       (int)timeout_sec);
        if (ret != 0)
            GSCOPE_WARN(ctx, "init process %d did not exit cleanly",
                        (int)scope->init_pid);
    }

    /* Force-kill any remaining processes */
    if (scope->cgroup_path[0])
        gscope_cgroup_kill(scope, SIGKILL);

    /* Clean up PID state */
    if (scope->pidfd >= 0) {
        close(scope->pidfd);
        scope->pidfd = -1;
    }
    scope->init_pid = -1;

    scope->state = GSCOPE_STATE_STOPPED;
    GSCOPE_INFO(ctx, "scope %u stopped", scope->id);

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * SCOPE DELETE
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_scope_delete(gscope_scope_t *scope, bool force)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    gscope_ctx_t *ctx = scope->ctx;

    GSCOPE_INFO(ctx, "═══ deleting scope %u ═══", scope->id);
    scope->state = GSCOPE_STATE_DELETING;

    /* Stop if running */
    if (scope->init_pid > 0) {
        GSCOPE_INFO(ctx, "  stopping running scope...");
        gscope_scope_stop(scope, 10);
    }

    /* Reverse order teardown */
    GSCOPE_INFO(ctx, "  [1] user delete");
    if (scope->username[0])
        gscope_user_delete(scope, scope->username);

    GSCOPE_INFO(ctx, "  [2] network teardown");
    if (scope->ip_address[0])
        gscope_ip_free(ctx, scope->ip_address);
    gscope_veth_delete(scope);

    GSCOPE_INFO(ctx, "  [3] namespace delete");
    gscope_ns_delete(scope);

    GSCOPE_INFO(ctx, "  [4] cgroup delete");
    gscope_cgroup_delete(scope);

    GSCOPE_INFO(ctx, "  [5] overlay unmount");
    gscope_overlay_unmount(scope);

    GSCOPE_INFO(ctx, "  [6] rootfs teardown");
    gscope_rootfs_teardown(scope);

    /* Remove from context list */
    ctx_remove_scope(ctx, scope);

    /* Remove state file */
    gscope_state_delete(scope->id);

    scope->state = GSCOPE_STATE_DELETED;
    GSCOPE_INFO(ctx, "═══ scope %u deleted ═══", scope->id);

    pthread_mutex_destroy(&scope->lock);
    free(scope);

    gscope_clear_error();
    return GSCOPE_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * QUERY FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_scope_status(gscope_scope_t *scope, gscope_status_t *status)
{
    if (!scope || !status)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or status");

    memset(status, 0, sizeof(*status));
    status->id = scope->id;
    status->state = scope->state;
    status->health = scope->health;
    status->init_pid = scope->init_pid;
    status->pidfd = scope->pidfd;
    gscope_strlcpy(status->ip_address, scope->ip_address, sizeof(status->ip_address));
    gscope_strlcpy(status->rootfs_path, scope->rootfs_merged, sizeof(status->rootfs_path));
    gscope_strlcpy(status->cgroup_path, scope->cgroup_path, sizeof(status->cgroup_path));

    if (scope->config.hostname)
        gscope_strlcpy(status->hostname, scope->config.hostname, sizeof(status->hostname));
    else
        snprintf(status->hostname, sizeof(status->hostname), "scope-%u", scope->id);

    status->created_at = scope->created_at;
    status->started_at = scope->started_at;

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_scope_metrics(gscope_scope_t *scope, gscope_metrics_t *metrics)
{
    if (!scope || !metrics)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or metrics");

    memset(metrics, 0, sizeof(*metrics));

    /* Read cgroup stats */
    gscope_cgroup_stats_t cg;
    if (gscope_cgroup_stats(scope, &cg) == GSCOPE_OK) {
        metrics->cpu_usage_us = cg.cpu_usage_us;
        metrics->memory_current = cg.memory_current;
        metrics->memory_limit = cg.memory_max;
        metrics->pids_current = cg.pids_current;
        metrics->pids_limit = cg.pids_max;

        if (cg.memory_max > 0 && cg.memory_max != UINT64_MAX)
            metrics->memory_percent = (float)cg.memory_current / (float)cg.memory_max * 100.0f;
    }

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_scope_t *gscope_scope_get(gscope_ctx_t *ctx, gscope_id_t id)
{
    if (!ctx) return NULL;

    pthread_mutex_lock(&ctx->lock);
    gscope_scope_t *s = ctx->scopes;
    while (s) {
        if (s->id == id) {
            pthread_mutex_unlock(&ctx->lock);
            return s;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

int gscope_scope_list(gscope_ctx_t *ctx, gscope_id_t *ids, int max_count)
{
    if (!ctx) return 0;

    pthread_mutex_lock(&ctx->lock);
    int count = 0;
    gscope_scope_t *s = ctx->scopes;
    while (s) {
        if (ids && count < max_count)
            ids[count] = s->id;
        count++;
        s = s->next;
    }
    pthread_mutex_unlock(&ctx->lock);
    return count;
}

gscope_id_t gscope_scope_id(gscope_scope_t *scope)
{
    return scope ? scope->id : 0;
}

gscope_state_t gscope_scope_state(gscope_scope_t *scope)
{
    return scope ? scope->state : GSCOPE_STATE_DELETED;
}

gscope_err_t gscope_scope_update(gscope_scope_t *scope, const gscope_config_t *config)
{
    if (!scope || !config)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or config");

    gscope_cgroup_limits_t limits = {
        .cpu_cores = config->cpu_cores,
        .cpu_weight = config->cpu_weight,
        .memory_bytes = config->memory_bytes,
        .memory_swap_bytes = config->memory_swap_bytes,
        .pids_max = config->max_pids,
        .io_weight = config->io_weight,
    };

    return gscope_cgroup_update(scope, &limits);
}

/* ═══════════════════════════════════════════════════════════════════
 * NETWORK API (delegates to net/* modules)
 * ═══════════════════════════════════════════════════════════════════ */

gscope_err_t gscope_net_setup(gscope_scope_t *scope, gscope_netmode_t mode)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    /* Already done in scope_create. This is for post-create setup. */
    (void)mode;
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_net_info(gscope_scope_t *scope, gscope_net_info_t *info)
{
    if (!scope || !info)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or info");

    memset(info, 0, sizeof(*info));
    gscope_strlcpy(info->ip_address, scope->ip_address, sizeof(info->ip_address));
    gscope_strlcpy(info->veth_host, scope->veth_host, sizeof(info->veth_host));
    gscope_strlcpy(info->veth_scope, scope->veth_scope, sizeof(info->veth_scope));
    gscope_strlcpy(info->netns_name, scope->netns_name, sizeof(info->netns_name));

    if (scope->ctx)
        gscope_strlcpy(info->bridge, scope->ctx->bridge_name, sizeof(info->bridge));

    char gw[16];
    if (scope->ctx) {
        gscope_ip_gateway(scope->ctx, gw, sizeof(gw));
        gscope_strlcpy(info->gateway, gw, sizeof(info->gateway));
    }

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_net_port_add(gscope_scope_t *scope, const gscope_port_map_t *map)
{
    if (!scope || !map)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or map");
    if (scope->ip_address[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "scope has no IP");

    return gscope_fw_port_forward(scope->ctx, map, scope->ip_address);
}

gscope_err_t gscope_net_port_remove(gscope_scope_t *scope, const gscope_port_map_t *map)
{
    if (!scope || !map)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or map");
    if (scope->ip_address[0] == '\0')
        return gscope_set_error(GSCOPE_ERR_STATE, "scope has no IP");

    return gscope_fw_port_remove(scope->ctx, map, scope->ip_address);
}

gscope_err_t gscope_net_stats(gscope_scope_t *scope, gscope_net_stats_t *stats)
{
    if (!scope || !stats)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope or stats");

    /* TODO: read from /sys/class/net/{veth}/statistics/ */
    memset(stats, 0, sizeof(*stats));
    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_net_teardown(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    if (scope->ip_address[0] && scope->ctx)
        gscope_ip_free(scope->ctx, scope->ip_address);
    gscope_veth_delete(scope);
    scope->ip_address[0] = '\0';

    gscope_clear_error();
    return GSCOPE_OK;
}

gscope_err_t gscope_net_ensure_bridge(gscope_ctx_t *ctx, const char *bridge_name)
{
    if (!ctx)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL ctx");

    const char *name = bridge_name ? bridge_name : ctx->bridge_name;
    return gscope_bridge_create(ctx, name);
}
