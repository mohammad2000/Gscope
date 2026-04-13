/*
 * ns/namespace.c — Linux namespace management
 *
 * Provides clone3/clone/unshare/setns wrappers with graceful fallbacks.
 *
 * Namespace types managed:
 *   PID  — Process isolation (separate PID tree)
 *   NET  — Network isolation (own interfaces, routing)
 *   MNT  — Mount isolation (own mount table)
 *   UTS  — Hostname isolation
 *   IPC  — IPC isolation (semaphores, message queues)
 *   USER — UID/GID mapping (rootless scopes)
 *   CGROUP — Cgroup isolation
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "../compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#include <sys/mount.h>
#endif

/* ─── Namespace type to CLONE flag mapping ───────────────────────── */

static unsigned long ns_flag_to_clone(uint32_t ns_type)
{
    switch (ns_type) {
    case GSCOPE_NS_PID:    return CLONE_NEWPID;
    case GSCOPE_NS_NET:    return CLONE_NEWNET;
    case GSCOPE_NS_MNT:    return CLONE_NEWNS;
    case GSCOPE_NS_UTS:    return CLONE_NEWUTS;
    case GSCOPE_NS_IPC:    return CLONE_NEWIPC;
    case GSCOPE_NS_USER:   return CLONE_NEWUSER;
    case GSCOPE_NS_CGROUP: return CLONE_NEWCGROUP;
    default:               return 0;
    }
}

/* Namespace type name for /proc/<pid>/ns/<name> */
static const char *ns_type_name(uint32_t ns_type)
{
    switch (ns_type) {
    case GSCOPE_NS_PID:    return "pid";
    case GSCOPE_NS_NET:    return "net";
    case GSCOPE_NS_MNT:    return "mnt";
    case GSCOPE_NS_UTS:    return "uts";
    case GSCOPE_NS_IPC:    return "ipc";
    case GSCOPE_NS_USER:   return "user";
    case GSCOPE_NS_CGROUP: return "cgroup";
    default:               return NULL;
    }
}

/* Index in scope->ns_fds[] array */
static int ns_type_index(uint32_t ns_type)
{
    switch (ns_type) {
    case GSCOPE_NS_PID:    return 0;
    case GSCOPE_NS_NET:    return 1;
    case GSCOPE_NS_MNT:    return 2;
    case GSCOPE_NS_UTS:    return 3;
    case GSCOPE_NS_IPC:    return 4;
    case GSCOPE_NS_USER:   return 5;
    case GSCOPE_NS_CGROUP: return 6;
    default:               return -1;
    }
}

/* ─── Network Namespace (named, persistent) ──────────────────────── */

#ifdef __linux__
/*
 * Create a named network namespace at /var/run/netns/<name>.
 * This is equivalent to `ip netns add <name>` but done via syscalls:
 *   1. Create /var/run/netns/<name> as a regular file
 *   2. unshare(CLONE_NEWNET)
 *   3. Bind-mount /proc/self/ns/net onto the file
 *   4. setns back to the original namespace
 *
 * This approach avoids spawning a subprocess.
 */
static gscope_err_t create_named_netns(gscope_scope_t *scope)
{
    gscope_ctx_t *ctx = scope->ctx;
    const char *name = scope->netns_name;

    char netns_dir[] = "/var/run/netns";
    char netns_path[256];
    snprintf(netns_path, sizeof(netns_path), "%s/%s", netns_dir, name);

    /* Ensure /var/run/netns exists */
    gscope_mkdir_p(netns_dir, 0755);

    /* Save current net namespace fd */
    int orig_ns_fd = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    if (orig_ns_fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "cannot open /proc/self/ns/net");

    /* Create the netns file */
    int ns_file_fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0);
    if (ns_file_fd < 0) {
        if (errno == EEXIST) {
            /*
             * File exists — verify it's a REAL isolated namespace.
             * Stale bind mounts (from crashes/restarts) point to
             * the host network namespace, which is dangerous.
             * Detect by checking if the namespace has host interfaces.
             */
            char check_cmd[256];
            snprintf(check_cmd, sizeof(check_cmd),
                     "ip netns exec %s ip link show enp6s18 >/dev/null 2>&1"
                     " || ip netns exec %s ip link show eth0 >/dev/null 2>&1",
                     name, name);
            int has_host_iface = (system(check_cmd) == 0);

            if (has_host_iface) {
                /* Stale namespace — delete and recreate */
                GSCOPE_INFO(ctx, "netns %s is stale (has host interfaces), recreating", name);
                umount2(netns_path, MNT_DETACH);
                unlink(netns_path);
                /* Fall through to create fresh namespace below */
                ns_file_fd = open(netns_path, O_RDONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0);
                if (ns_file_fd < 0) {
                    close(orig_ns_fd);
                    return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                                  "cannot recreate %s", netns_path);
                }
            } else {
                /* Namespace is functional — reuse it */
                close(orig_ns_fd);
                GSCOPE_DEBUG(ctx, "netns %s already exists and functional, reusing", name);
                gscope_clear_error();
                return GSCOPE_OK;
            }
        } else {
            close(orig_ns_fd);
            return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                          "cannot create %s", netns_path);
        }
    }
    close(ns_file_fd);

    /* Create new network namespace */
    if (unshare(CLONE_NEWNET) != 0) {
        close(orig_ns_fd);
        unlink(netns_path);
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "unshare(CLONE_NEWNET) failed");
    }

    /* Bind-mount /proc/self/ns/net onto the named file */
    if (mount("/proc/self/ns/net", netns_path, "none", MS_BIND, NULL) != 0) {
        /* Restore original namespace before returning */
        setns(orig_ns_fd, CLONE_NEWNET);
        close(orig_ns_fd);
        unlink(netns_path);
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "bind mount netns failed");
    }

    /* Switch back to original namespace */
    if (setns(orig_ns_fd, CLONE_NEWNET) != 0) {
        close(orig_ns_fd);
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "setns to original netns failed");
    }

    close(orig_ns_fd);

    /* Open the new namespace fd for later use */
    int ns_fd = open(netns_path, O_RDONLY | O_CLOEXEC);
    if (ns_fd >= 0) {
        int idx = ns_type_index(GSCOPE_NS_NET);
        if (idx >= 0)
            scope->ns_fds[idx] = ns_fd;
    }

    /* Enable loopback inside the new namespace */
    int new_ns_fd = open(netns_path, O_RDONLY | O_CLOEXEC);
    if (new_ns_fd >= 0) {
        /* We'll configure loopback when we actually enter the ns
         * (during spawn or via netlink). For now, just save the fd. */
        close(new_ns_fd);
    }

    scope->ns_active |= GSCOPE_NS_NET;

    GSCOPE_INFO(ctx, "named netns created: %s", netns_path);
    return GSCOPE_OK;
}

static gscope_err_t delete_named_netns(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "/var/run/netns/%s", name);

    /* Unmount the bind mount */
    umount2(path, MNT_DETACH);

    /* Remove the file */
    if (unlink(path) != 0 && errno != ENOENT)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "unlink %s failed", path);

    return GSCOPE_OK;
}
#endif /* __linux__ */

/* ─── Build combined CLONE flags ─────────────────────────────────── */

static unsigned long build_clone_flags(uint32_t ns_flags)
{
    unsigned long flags = 0;

    for (int bit = 0; bit < 7; bit++) {
        uint32_t ns = (1U << bit);
        if (ns_flags & ns) {
            /* NET namespace is handled separately (named) */
            if (ns == GSCOPE_NS_NET)
                continue;
            flags |= ns_flag_to_clone(ns);
        }
    }

    return flags;
}

/* ─── Public API ─────────────────────────────────────────────────── */

gscope_err_t gscope_ns_create(gscope_scope_t *scope, uint32_t ns_flags)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

#ifdef __linux__
    gscope_ctx_t *ctx = scope->ctx;

    /* Initialize all ns_fds to -1 */
    for (int i = 0; i < GSCOPE_NS_COUNT; i++) {
        if (scope->ns_fds[i] == 0)
            scope->ns_fds[i] = -1;
    }

    /* If ns_flags is 0, derive from isolation level */
    if (ns_flags == 0) {
        switch (scope->config.isolation) {
        case GSCOPE_ISOLATION_MINIMAL:
            ns_flags = GSCOPE_NS_NET;
            break;
        case GSCOPE_ISOLATION_STANDARD:
            ns_flags = GSCOPE_NS_NET | GSCOPE_NS_PID | GSCOPE_NS_MNT | GSCOPE_NS_UTS;
            break;
        case GSCOPE_ISOLATION_HIGH:
            ns_flags = GSCOPE_NS_NET | GSCOPE_NS_PID | GSCOPE_NS_MNT |
                       GSCOPE_NS_UTS | GSCOPE_NS_IPC | GSCOPE_NS_CGROUP;
            break;
        case GSCOPE_ISOLATION_MAXIMUM:
            ns_flags = GSCOPE_NS_ALL;
            break;
        }
    }

    /* Build netns name */
    snprintf(scope->netns_name, sizeof(scope->netns_name),
             "gscope-%u", scope->id);

    GSCOPE_INFO(ctx, "creating namespaces for scope %u (flags=0x%x)",
                scope->id, ns_flags);

    /* 1. Create named network namespace (persistent, visible to `ip netns`) */
    if (ns_flags & GSCOPE_NS_NET) {
        gscope_err_t err = create_named_netns(scope);
        if (err != GSCOPE_OK)
            return err;
        GSCOPE_DEBUG(ctx, "  NET namespace: %s", scope->netns_name);
    }

    /*
     * 2. Other namespaces (PID, MNT, UTS, IPC, USER, CGROUP)
     *    are created at spawn time via unshare() in the child process.
     *    We just record which ones are requested.
     */
    scope->ns_active |= ns_flags;
    scope->config.ns_flags = ns_flags;

    GSCOPE_INFO(ctx, "namespace setup complete for scope %u "
                "(active=0x%x, netns=%s)",
                scope->id, scope->ns_active, scope->netns_name);

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)ns_flags;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "namespaces require Linux");
#endif
}

gscope_err_t gscope_ns_enter(gscope_scope_t *scope, uint32_t ns_type)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

#ifdef __linux__
    int idx = ns_type_index(ns_type);
    if (idx < 0)
        return gscope_set_error(GSCOPE_ERR_INVAL,
                                "invalid namespace type 0x%x", ns_type);

    int fd = scope->ns_fds[idx];

    /* If we don't have a stored fd, try to open it */
    if (fd < 0 && ns_type == GSCOPE_NS_NET) {
        char path[256];
        snprintf(path, sizeof(path), "/var/run/netns/%s", scope->netns_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            scope->ns_fds[idx] = fd;
    }

    if (fd < 0)
        return gscope_set_error(GSCOPE_ERR_NAMESPACE,
                                "no fd for %s namespace",
                                ns_type_name(ns_type));

    unsigned long clone_flag = ns_flag_to_clone(ns_type);
    if (setns(fd, (int)clone_flag) != 0)
        return gscope_set_error_errno(GSCOPE_ERR_NAMESPACE,
                                      "setns(%s) failed",
                                      ns_type_name(ns_type));

    gscope_clear_error();
    return GSCOPE_OK;

#else
    (void)ns_type;
    return gscope_set_error(GSCOPE_ERR_UNSUPPORTED,
                            "namespaces require Linux");
#endif
}

int gscope_ns_fd(gscope_scope_t *scope, uint32_t ns_type)
{
    if (!scope) return -1;

    int idx = ns_type_index(ns_type);
    if (idx < 0) return -1;

    return scope->ns_fds[idx];
}

bool gscope_ns_verify(gscope_scope_t *scope, uint32_t ns_type)
{
    if (!scope)
        return false;

#ifdef __linux__
    if (ns_type == GSCOPE_NS_NET && scope->netns_name[0] != '\0') {
        char path[256];
        snprintf(path, sizeof(path), "/var/run/netns/%s", scope->netns_name);

        /* Check file exists */
        struct stat st;
        if (stat(path, &st) != 0)
            return false;

        /* Check if we can open it */
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        close(fd);
        return true;
    }

    /* For non-NET namespaces, check if the process has the namespace fd */
    int idx = ns_type_index(ns_type);
    if (idx < 0) return false;

    return scope->ns_fds[idx] >= 0;
#else
    (void)ns_type;
    return false;
#endif
}

gscope_err_t gscope_ns_delete(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    gscope_ctx_t *ctx = scope->ctx;

    /* Close all stored namespace fds */
    for (int i = 0; i < GSCOPE_NS_COUNT; i++) {
        if (scope->ns_fds[i] >= 0) {
            close(scope->ns_fds[i]);
            scope->ns_fds[i] = -1;
        }
    }

#ifdef __linux__
    /* Delete named network namespace */
    if (scope->netns_name[0] != '\0') {
        GSCOPE_INFO(ctx, "deleting netns: %s", scope->netns_name);
        delete_named_netns(scope->netns_name);
        scope->netns_name[0] = '\0';
    }
#endif

    scope->ns_active = 0;

    GSCOPE_INFO(ctx, "namespaces deleted for scope %u", scope->id);
    gscope_clear_error();
    return GSCOPE_OK;
}

/*
 * Internal: apply unshare for non-NET namespaces.
 * Called from spawn.c in the child process.
 */
int gscope_ns_unshare(uint32_t ns_flags)
{
#ifdef __linux__
    unsigned long flags = build_clone_flags(ns_flags);
    if (flags == 0)
        return 0;

    if (unshare((int)flags) != 0)
        return -1;

    return 0;
#else
    (void)ns_flags;
    errno = ENOSYS;
    return -1;
#endif
}

/*
 * Internal: enter the NET namespace by name.
 * Called from spawn.c in the child process before exec.
 */
int gscope_ns_enter_netns(const char *name)
{
#ifdef __linux__
    char path[256];
    snprintf(path, sizeof(path), "/var/run/netns/%s", name);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int ret = setns(fd, CLONE_NEWNET);
    close(fd);
    return ret;
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}
