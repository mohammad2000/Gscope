/*
 * state.c — JSON state persistence for scopes
 *
 * Each scope's state is saved to a JSON file:
 *   /opt/gritiva/scopes/{id}/state.json
 *
 * On gscope_init with GSCOPE_INIT_RESTORE, existing state files
 * are scanned and scopes are restored.
 *
 * Writes are atomic: write to .tmp, then rename.
 * We use a minimal hand-written JSON serializer (no dependency).
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "internal.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_DIR "/opt/gritiva/scopes"

/* ─── Save State ─────────────────────────────────────────────────── */

gscope_err_t gscope_state_save(gscope_scope_t *scope)
{
    if (!scope)
        return gscope_set_error(GSCOPE_ERR_INVAL, "NULL scope");

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/%u", STATE_DIR, scope->id);
    gscope_mkdir_p(dir, 0755);

    char path[4096];
    snprintf(path, sizeof(path), "%s/state.json", dir);

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.state.json.tmp", dir);

    /* Build JSON manually (no library dependency) */
    char json[8192];
    int n = snprintf(json, sizeof(json),
        "{\n"
        "  \"id\": %u,\n"
        "  \"state\": %d,\n"
        "  \"health\": %d,\n"
        "  \"init_pid\": %d,\n"
        "  \"ns_active\": %u,\n"
        "  \"netns_name\": \"%s\",\n"
        "  \"cgroup_path\": \"%s\",\n"
        "  \"ip_address\": \"%s\",\n"
        "  \"veth_host\": \"%s\",\n"
        "  \"veth_scope\": \"%s\",\n"
        "  \"rootfs_lower\": \"%s\",\n"
        "  \"rootfs_upper\": \"%s\",\n"
        "  \"rootfs_work\": \"%s\",\n"
        "  \"rootfs_merged\": \"%s\",\n"
        "  \"rootfs_base\": \"%s\",\n"
        "  \"rootfs_mounted\": %s,\n"
        "  \"username\": \"%s\",\n"
        "  \"uid\": %u,\n"
        "  \"gid\": %u,\n"
        "  \"created_at\": %lu,\n"
        "  \"started_at\": %lu,\n"
        "  \"config\": {\n"
        "    \"isolation\": %d,\n"
        "    \"net_mode\": %d,\n"
        "    \"privilege\": %d,\n"
        "    \"ns_flags\": %u,\n"
        "    \"cpu_cores\": %.2f,\n"
        "    \"memory_bytes\": %lu,\n"
        "    \"max_pids\": %u,\n"
        "    \"seccomp\": %d\n"
        "  }\n"
        "}\n",
        scope->id,
        scope->state,
        scope->health,
        (int)scope->init_pid,
        scope->ns_active,
        scope->netns_name,
        scope->cgroup_path,
        scope->ip_address,
        scope->veth_host,
        scope->veth_scope,
        scope->rootfs_lower,
        scope->rootfs_upper,
        scope->rootfs_work,
        scope->rootfs_merged,
        scope->rootfs_base,
        scope->rootfs_mounted ? "true" : "false",
        scope->username,
        (unsigned)scope->uid,
        (unsigned)scope->gid,
        (unsigned long)scope->created_at,
        (unsigned long)scope->started_at,
        scope->config.isolation,
        scope->config.net_mode,
        scope->config.privilege,
        scope->config.ns_flags,
        (double)scope->config.cpu_cores,
        (unsigned long)scope->config.memory_bytes,
        scope->config.max_pids,
        scope->config.seccomp
    );

    if (n < 0 || (size_t)n >= sizeof(json))
        return gscope_set_error(GSCOPE_ERR_IO, "state JSON too large");

    /* Atomic write: tmp → rename */
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return gscope_set_error_errno(GSCOPE_ERR_IO,
                                      "cannot create %s", tmp_path);

    ssize_t written = write(fd, json, (size_t)n);
    close(fd);

    if (written != n) {
        unlink(tmp_path);
        return gscope_set_error_errno(GSCOPE_ERR_IO, "write failed");
    }

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return gscope_set_error_errno(GSCOPE_ERR_IO, "rename failed");
    }

    GSCOPE_DEBUG(scope->ctx, "state saved: %s", path);
    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Delete State ───────────────────────────────────────────────── */

gscope_err_t gscope_state_delete(uint32_t scope_id)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%u/state.json", STATE_DIR, scope_id);
    unlink(path);
    gscope_clear_error();
    return GSCOPE_OK;
}

/* ─── Minimal JSON Parser Helpers ────────────────────────────────── */

static const char *json_find_key(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n') p++;
    return p;
}

static int json_read_int(const char *json, const char *key, int def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    return atoi(p);
}

static unsigned long json_read_ulong(const char *json, const char *key, unsigned long def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    return strtoul(p, NULL, 10);
}

static float json_read_float(const char *json, const char *key, float def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    return (float)atof(p);
}

static void json_read_str(const char *json, const char *key,
                           char *out, size_t out_size)
{
    out[0] = '\0';
    const char *p = json_find_key(json, key);
    if (!p || *p != '"') return;
    p++;  /* skip opening quote */

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static int json_read_bool(const char *json, const char *key, int def)
{
    const char *p = json_find_key(json, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

/* ─── Load / Restore ─────────────────────────────────────────────── */

/*
 * Scan STATE_DIR for scope directories containing state.json,
 * parse them, and re-register the scopes in the context.
 *
 * NOTE: This restores metadata only — processes are NOT restarted.
 * The scope will be in STOPPED state after restore.
 */
int gscope_state_restore_all(gscope_ctx_t *ctx)
{
    if (!ctx) return 0;

    DIR *d = opendir(STATE_DIR);
    if (!d) return 0;

    int restored = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Check if it's a numeric directory */
        char *endp;
        unsigned long id_val = strtoul(entry->d_name, &endp, 10);
        if (*endp != '\0' || id_val == 0) continue;

        /* Read state.json */
        char state_path[4096];
        snprintf(state_path, sizeof(state_path),
                 "%s/%s/state.json", STATE_DIR, entry->d_name);

        char json[8192];
        if (gscope_read_file(state_path, json, sizeof(json)) < 0)
            continue;

        /* Allocate scope */
        gscope_scope_t *scope = calloc(1, sizeof(*scope));
        if (!scope) continue;

        scope->ctx = ctx;
        pthread_mutex_init(&scope->lock, NULL);
        for (int i = 0; i < GSCOPE_NS_COUNT; i++)
            scope->ns_fds[i] = -1;

        /* Parse JSON fields */
        scope->id = (uint32_t)json_read_int(json, "id", 0);
        scope->state = GSCOPE_STATE_STOPPED;  /* Always stopped after restore */
        scope->health = (gscope_health_t)json_read_int(json, "health", 0);
        scope->init_pid = -1;  /* Process not running */
        scope->pidfd = -1;
        scope->ns_active = (uint32_t)json_read_int(json, "ns_active", 0);

        json_read_str(json, "netns_name", scope->netns_name, sizeof(scope->netns_name));
        json_read_str(json, "cgroup_path", scope->cgroup_path, sizeof(scope->cgroup_path));
        json_read_str(json, "ip_address", scope->ip_address, sizeof(scope->ip_address));
        json_read_str(json, "veth_host", scope->veth_host, sizeof(scope->veth_host));
        json_read_str(json, "veth_scope", scope->veth_scope, sizeof(scope->veth_scope));
        json_read_str(json, "rootfs_lower", scope->rootfs_lower, sizeof(scope->rootfs_lower));
        json_read_str(json, "rootfs_upper", scope->rootfs_upper, sizeof(scope->rootfs_upper));
        json_read_str(json, "rootfs_work", scope->rootfs_work, sizeof(scope->rootfs_work));
        json_read_str(json, "rootfs_merged", scope->rootfs_merged, sizeof(scope->rootfs_merged));
        json_read_str(json, "rootfs_base", scope->rootfs_base, sizeof(scope->rootfs_base));
        scope->rootfs_mounted = json_read_bool(json, "rootfs_mounted", 0);
        json_read_str(json, "username", scope->username, sizeof(scope->username));
        scope->uid = (uid_t)json_read_int(json, "uid", 0);
        scope->gid = (gid_t)json_read_int(json, "gid", 0);
        scope->created_at = json_read_ulong(json, "created_at", 0);
        scope->started_at = json_read_ulong(json, "started_at", 0);

        /* Config section */
        scope->config.id = scope->id;
        scope->config.isolation = (gscope_isolation_t)json_read_int(json, "isolation", 1);
        scope->config.net_mode = (gscope_netmode_t)json_read_int(json, "net_mode", 1);
        scope->config.privilege = (gscope_privilege_t)json_read_int(json, "privilege", 1);
        scope->config.ns_flags = (uint32_t)json_read_int(json, "ns_flags", 0);
        scope->config.cpu_cores = json_read_float(json, "cpu_cores", 1.0f);
        scope->config.memory_bytes = json_read_ulong(json, "memory_bytes", 512UL * 1024 * 1024);
        scope->config.max_pids = (uint32_t)json_read_int(json, "max_pids", 1024);
        scope->config.seccomp = (gscope_seccomp_t)json_read_int(json, "seccomp", 0);

        /* Add to context */
        pthread_mutex_lock(&ctx->lock);
        scope->next = ctx->scopes;
        ctx->scopes = scope;
        ctx->scope_count++;
        pthread_mutex_unlock(&ctx->lock);

        restored++;
        GSCOPE_INFO(ctx, "restored scope %u (state=stopped, ip=%s)",
                    scope->id, scope->ip_address);
    }

    closedir(d);
    return restored;
}
