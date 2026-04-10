/*
 * gscopectl — CLI tool for gscope
 *
 * Usage: gscopectl <command> [options]
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gscope/gscope.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Command handlers (defined in cmd_*.c or below) */
static int cmd_create(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_start(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_stop(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_delete(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_list(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_status(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_exec(gscope_ctx_t *ctx, int argc, char **argv);
static int cmd_template(gscope_ctx_t *ctx, int argc, char **argv);

/* ─── Usage ──────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "gscopectl %s — Lightweight Linux scope isolation\n"
        "\n"
        "Usage: gscopectl <command> [options]\n"
        "\n"
        "Lifecycle:\n"
        "  create    Create a new scope\n"
        "  start     Start a stopped scope\n"
        "  stop      Stop a running scope\n"
        "  delete    Delete a scope and free resources\n"
        "\n"
        "Query:\n"
        "  list      List all scopes\n"
        "  status    Show scope status\n"
        "\n"
        "Execution:\n"
        "  exec      Execute command inside a scope\n"
        "\n"
        "Other:\n"
        "  version   Show version\n"
        "  help      Show this help\n"
        "\n"
        "Examples:\n"
        "  gscopectl create --id 1 --cpu 2 --mem 1024 --template /opt/templates/base\n"
        "  gscopectl start  --id 1\n"
        "  gscopectl exec   --id 1 -- /bin/bash\n"
        "  gscopectl stop   --id 1\n"
        "  gscopectl delete --id 1\n"
        "\n",
        gscope_version()
    );
}

/* ─── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("gscopectl %s\n", gscope_version());
        return 0;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    /* Commands that need library init */
    gscope_ctx_t *ctx = NULL;
    gscope_err_t err = gscope_init(&ctx, GSCOPE_INIT_RESTORE);

    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl: init failed: %s\n", gscope_strerror());
        return 1;
    }

    int rc = 1;

    if (strcmp(cmd, "create") == 0)       rc = cmd_create(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "start") == 0)   rc = cmd_start(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "stop") == 0)    rc = cmd_stop(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "delete") == 0)  rc = cmd_delete(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "list") == 0)    rc = cmd_list(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "status") == 0)  rc = cmd_status(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "exec") == 0)    rc = cmd_exec(ctx, argc - 1, argv + 1);
    else if (strcmp(cmd, "template") == 0) rc = cmd_template(ctx, argc - 1, argv + 1);
    else {
        fprintf(stderr, "gscopectl: unknown command '%s'\n", cmd);
        fprintf(stderr, "Run 'gscopectl help' for usage.\n");
    }

    gscope_destroy(ctx);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: create
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_create(gscope_ctx_t *ctx, int argc, char **argv)
{
    gscope_config_t config;
    gscope_config_init(&config);

    static struct option long_opts[] = {
        {"id",       required_argument, 0, 'i'},
        {"cpu",      required_argument, 0, 'c'},
        {"mem",      required_argument, 0, 'm'},
        {"pids",     required_argument, 0, 'p'},
        {"template", required_argument, 0, 't'},
        {"user",     required_argument, 0, 'u'},
        {"hostname", required_argument, 0, 'H'},
        {"ip",       required_argument, 0, 'I'},
        {"net",      required_argument, 0, 'n'},
        {"isolation",required_argument, 0, 'L'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:c:m:p:t:u:H:I:n:L:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': config.id = (uint32_t)atoi(optarg); break;
        case 'c': config.cpu_cores = (float)atof(optarg); break;
        case 'm': config.memory_bytes = (uint64_t)atol(optarg) * 1024 * 1024; break;
        case 'p': config.max_pids = (uint32_t)atoi(optarg); break;
        case 't': config.template_path = optarg; break;
        case 'u': config.username = optarg; break;
        case 'H': config.hostname = optarg; break;
        case 'I': config.requested_ip = optarg; break;
        case 'n':
            if (strcmp(optarg, "bridge") == 0)   config.net_mode = GSCOPE_NET_BRIDGE;
            else if (strcmp(optarg, "host") == 0) config.net_mode = GSCOPE_NET_HOST;
            else if (strcmp(optarg, "none") == 0) config.net_mode = GSCOPE_NET_ISOLATED;
            break;
        case 'L':
            if (strcmp(optarg, "minimal") == 0)      config.isolation = GSCOPE_ISOLATION_MINIMAL;
            else if (strcmp(optarg, "standard") == 0) config.isolation = GSCOPE_ISOLATION_STANDARD;
            else if (strcmp(optarg, "high") == 0)     config.isolation = GSCOPE_ISOLATION_HIGH;
            else if (strcmp(optarg, "maximum") == 0)  config.isolation = GSCOPE_ISOLATION_MAXIMUM;
            break;
        case 'h':
            fprintf(stderr,
                "Usage: gscopectl create [options]\n"
                "\n"
                "Options:\n"
                "  --id N          Scope ID (required)\n"
                "  --cpu N         CPU cores (default: 1.0)\n"
                "  --mem N         Memory in MB (default: 512)\n"
                "  --pids N        Max processes (default: 1024)\n"
                "  --template PATH Template rootfs path\n"
                "  --user NAME     Username (default: root)\n"
                "  --hostname NAME Hostname\n"
                "  --ip ADDR       Request specific IP\n"
                "  --net MODE      bridge|host|none (default: bridge)\n"
                "  --isolation LVL minimal|standard|high|maximum\n"
            );
            return 0;
        default:
            return 1;
        }
    }

    if (config.id == 0) {
        fprintf(stderr, "gscopectl create: --id is required\n");
        return 1;
    }

    gscope_scope_t *scope;
    gscope_err_t err = gscope_scope_create(ctx, &config, &scope);
    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl create: %s\n", gscope_strerror());
        return 1;
    }

    gscope_status_t st;
    gscope_scope_status(scope, &st);

    printf("Scope %u created\n", config.id);
    printf("  rootfs:   %s\n", st.rootfs_path);
    printf("  cgroup:   %s\n", st.cgroup_path);
    printf("  ip:       %s\n", st.ip_address[0] ? st.ip_address : "(none)");
    printf("  hostname: %s\n", st.hostname);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: start
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_start(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;
    const char *command = NULL;

    static struct option long_opts[] = {
        {"id",      required_argument, 0, 'i'},
        {"command", required_argument, 0, 'c'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 'c': command = optarg; break;
        case 'h':
            fprintf(stderr, "Usage: gscopectl start --id N [--command CMD]\n");
            return 0;
        default: return 1;
        }
    }

    if (id == 0) { fprintf(stderr, "gscopectl start: --id required\n"); return 1; }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl start: scope %u not found\n", id); return 1; }

    gscope_err_t err = gscope_scope_start(scope, command);
    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl start: %s\n", gscope_strerror());
        return 1;
    }

    gscope_status_t st;
    gscope_scope_status(scope, &st);
    printf("Scope %u started (pid=%d)\n", id, (int)st.init_pid);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: stop
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_stop(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;
    unsigned int timeout = 10;

    static struct option long_opts[] = {
        {"id",      required_argument, 0, 'i'},
        {"timeout", required_argument, 0, 't'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:t:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 't': timeout = (unsigned)atoi(optarg); break;
        case 'h':
            fprintf(stderr, "Usage: gscopectl stop --id N [--timeout SEC]\n");
            return 0;
        default: return 1;
        }
    }

    if (id == 0) { fprintf(stderr, "gscopectl stop: --id required\n"); return 1; }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl stop: scope %u not found\n", id); return 1; }

    gscope_err_t err = gscope_scope_stop(scope, timeout);
    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl stop: %s\n", gscope_strerror());
        return 1;
    }

    printf("Scope %u stopped\n", id);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: delete
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_delete(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;
    bool force = false;

    static struct option long_opts[] = {
        {"id",    required_argument, 0, 'i'},
        {"force", no_argument,       0, 'f'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:fh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 'f': force = true; break;
        case 'h':
            fprintf(stderr, "Usage: gscopectl delete --id N [--force]\n");
            return 0;
        default: return 1;
        }
    }

    if (id == 0) { fprintf(stderr, "gscopectl delete: --id required\n"); return 1; }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl delete: scope %u not found\n", id); return 1; }

    gscope_err_t err = gscope_scope_delete(scope, force);
    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl delete: %s\n", gscope_strerror());
        return 1;
    }

    printf("Scope %u deleted\n", id);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: list
 * ═══════════════════════════════════════════════════════════════════ */

static const char *state_str(gscope_state_t s)
{
    switch (s) {
    case GSCOPE_STATE_CREATING: return "creating";
    case GSCOPE_STATE_STOPPED:  return "stopped";
    case GSCOPE_STATE_STARTING: return "starting";
    case GSCOPE_STATE_RUNNING:  return "running";
    case GSCOPE_STATE_STOPPING: return "stopping";
    case GSCOPE_STATE_ERROR:    return "error";
    case GSCOPE_STATE_DELETING: return "deleting";
    case GSCOPE_STATE_DELETED:  return "deleted";
    default: return "unknown";
    }
}

static int cmd_list(gscope_ctx_t *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;

    gscope_id_t ids[256];
    int count = gscope_scope_list(ctx, ids, 256);

    if (count == 0) {
        printf("No scopes\n");
        return 0;
    }

    printf("%-6s  %-10s  %-8s  %-16s  %-20s\n",
           "ID", "STATE", "PID", "IP", "HOSTNAME");
    printf("%-6s  %-10s  %-8s  %-16s  %-20s\n",
           "──", "─────", "───", "──", "────────");

    for (int i = 0; i < count && i < 256; i++) {
        gscope_scope_t *scope = gscope_scope_get(ctx, ids[i]);
        if (!scope) continue;

        gscope_status_t st;
        gscope_scope_status(scope, &st);

        char pid_str[16];
        if (st.init_pid > 0)
            snprintf(pid_str, sizeof(pid_str), "%d", (int)st.init_pid);
        else
            snprintf(pid_str, sizeof(pid_str), "-");

        printf("%-6u  %-10s  %-8s  %-16s  %-20s\n",
               st.id, state_str(st.state), pid_str,
               st.ip_address[0] ? st.ip_address : "-",
               st.hostname[0] ? st.hostname : "-");
    }

    printf("\n%d scope(s) total\n", count);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: status
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_status(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;

    static struct option long_opts[] = {
        {"id",   required_argument, 0, 'i'},
        {"help", no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 'h':
            fprintf(stderr, "Usage: gscopectl status --id N\n");
            return 0;
        default: return 1;
        }
    }

    if (id == 0) { fprintf(stderr, "gscopectl status: --id required\n"); return 1; }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl status: scope %u not found\n", id); return 1; }

    gscope_status_t st;
    gscope_scope_status(scope, &st);

    printf("Scope %u\n", st.id);
    printf("  State:    %s\n", state_str(st.state));
    printf("  PID:      %d\n", (int)st.init_pid);
    printf("  IP:       %s\n", st.ip_address[0] ? st.ip_address : "(none)");
    printf("  Hostname: %s\n", st.hostname);
    printf("  Rootfs:   %s\n", st.rootfs_path);
    printf("  Cgroup:   %s\n", st.cgroup_path);

    /* Metrics */
    gscope_metrics_t m;
    if (gscope_scope_metrics(scope, &m) == GSCOPE_OK) {
        printf("  CPU:      %lu us (%.1f%%)\n", (unsigned long)m.cpu_usage_us, m.cpu_percent);
        printf("  Memory:   %lu / %lu MB (%.1f%%)\n",
               (unsigned long)(m.memory_current / (1024*1024)),
               (unsigned long)(m.memory_limit / (1024*1024)),
               m.memory_percent);
        printf("  PIDs:     %u / %u\n", m.pids_current, m.pids_limit);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: exec
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_exec(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;
    bool pty = true;

    /* Find "--" separator */
    int cmd_start_idx = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            cmd_start_idx = i + 1;
            break;
        }
    }

    /* Parse options before "--" */
    static struct option long_opts[] = {
        {"id",     required_argument, 0, 'i'},
        {"no-pty", no_argument,       0, 'P'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    int stop_at = cmd_start_idx > 0 ? cmd_start_idx - 1 : argc;

    /* Temporarily truncate argv for getopt */
    char *saved = NULL;
    if (cmd_start_idx > 0 && cmd_start_idx - 1 < argc) {
        saved = argv[cmd_start_idx - 1];
        argv[cmd_start_idx - 1] = NULL;
    }

    while ((opt = getopt_long(stop_at, argv, "i:Ph", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 'P': pty = false; break;
        case 'h':
            fprintf(stderr,
                "Usage: gscopectl exec --id N [--no-pty] -- COMMAND [ARGS...]\n"
                "\n"
                "Execute a command inside a running scope.\n"
                "  --id N      Scope ID\n"
                "  --no-pty    Don't allocate PTY\n"
                "  --          Separator before command\n"
            );
            return 0;
        default: return 1;
        }
    }

    if (saved) argv[cmd_start_idx - 1] = saved;

    if (id == 0) { fprintf(stderr, "gscopectl exec: --id required\n"); return 1; }

    if (cmd_start_idx < 0 || cmd_start_idx >= argc) {
        fprintf(stderr, "gscopectl exec: no command specified (use -- CMD)\n");
        return 1;
    }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl exec: scope %u not found\n", id); return 1; }

    /* Build argv for the command */
    const char **cmd_argv = (const char **)&argv[cmd_start_idx];

    gscope_exec_config_t cfg = {
        .command = cmd_argv[0],
        .argv = cmd_argv,
        .allocate_pty = pty,
        .pty_rows = 24,
        .pty_cols = 80,
    };

    gscope_exec_result_t result;
    gscope_err_t err = gscope_exec(scope, &cfg, &result);
    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl exec: %s\n", gscope_strerror());
        return 1;
    }

    printf("Process started: pid=%d pty=%s\n",
           (int)result.pid, result.has_pty ? "yes" : "no");

    /* Wait for process to exit */
    int exit_status = 0;
    gscope_process_wait(&result, &exit_status, 0);

    printf("Process exited: status=%d\n", exit_status);
    gscope_process_release(&result);

    return exit_status;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND: template
 * ═══════════════════════════════════════════════════════════════════ */

static void template_progress_cb(const gscope_tmpl_progress_t *p, void *ud)
{
    (void)ud;
    const char *phase_names[] = {
        "PREFLIGHT", "VARIABLES", "PRE_INSTALL", "PACKAGES",
        "POST_INSTALL", "FILES", "SETUP", "VERIFICATION", "COMPLETE"
    };
    const char *phase = (p->phase >= 0 && p->phase <= 8)
                        ? phase_names[p->phase] : "?";

    if (p->progress >= 0)
        printf("  [%s] %3d%% %s%s\n", phase, p->progress, p->message,
               p->is_error ? " (ERROR)" : "");
    else
        printf("  [%s]      %s%s\n", phase, p->message,
               p->is_error ? " (ERROR)" : "");
}

static int cmd_template(gscope_ctx_t *ctx, int argc, char **argv)
{
    uint32_t id = 0;
    const char *file = NULL;
    const char *inline_json = NULL;

    static struct option long_opts[] = {
        {"id",     required_argument, 0, 'i'},
        {"file",   required_argument, 0, 'f'},
        {"inline", required_argument, 0, 'j'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:f:j:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i': id = (uint32_t)atoi(optarg); break;
        case 'f': file = optarg; break;
        case 'j': inline_json = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: gscopectl template --id N --file TEMPLATE.json\n"
                "       gscopectl template --id N --inline '{...}'\n"
                "\n"
                "Execute a template on a running scope.\n"
                "Installs packages, creates files, runs scripts.\n"
            );
            return 0;
        default: return 1;
        }
    }

    if (id == 0) { fprintf(stderr, "gscopectl template: --id required\n"); return 1; }
    if (!file && !inline_json) {
        fprintf(stderr, "gscopectl template: --file or --inline required\n");
        return 1;
    }

    gscope_scope_t *scope = gscope_scope_get(ctx, id);
    if (!scope) { fprintf(stderr, "gscopectl template: scope %u not found\n", id); return 1; }

    /* Parse template */
    gscope_template_t *tmpl = NULL;
    gscope_err_t err;

    if (file) {
        err = gscope_template_parse_file(file, &tmpl);
    } else {
        err = gscope_template_parse(inline_json, &tmpl);
    }

    if (err != GSCOPE_OK) {
        fprintf(stderr, "gscopectl template: parse error: %s\n", gscope_strerror());
        return 1;
    }

    const char *tname = gscope_template_get_var(tmpl, "template_name");
    printf("Template: %s\n", tname ? tname : (file ? file : "inline"));
    printf("Executing on scope %u...\n\n", id);

    /* Execute */
    gscope_tmpl_result_t result;
    err = gscope_template_execute(scope, tmpl, template_progress_cb, NULL, &result);

    printf("\n");
    if (result.success) {
        printf("✅ Template executed successfully (%.1fs)\n", result.duration_sec);
        printf("   Packages: %d installed\n", result.packages_installed);
        printf("   Files:    %d created\n", result.files_created);
        printf("   Scripts:  %d executed\n", result.scripts_executed);
        if (result.verifications_passed + result.verifications_failed > 0)
            printf("   Verify:   %d/%d passed\n",
                   result.verifications_passed,
                   result.verifications_passed + result.verifications_failed);
    } else {
        printf("❌ Template execution failed: %s\n", result.error);
    }

    gscope_template_free(tmpl);
    return result.success ? 0 : 1;
}
