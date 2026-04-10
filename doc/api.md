# gscope C API Reference

Complete reference for all public functions in `libgscope`.

---

## Headers

```c
#include <gscope/gscope.h>    // Everything (umbrella)
#include <gscope/types.h>     // Types only
#include <gscope/scope.h>     // Scope lifecycle only
#include <gscope/template.h>  // Template executor only
```

---

## Library Lifecycle

### `gscope_init`
```c
gscope_err_t gscope_init(gscope_ctx_t **ctx, unsigned int flags);
```
Initialize the library. Must be called before any other function. Detects kernel features, initializes IP allocator, creates base directories.

**Flags:**
- `GSCOPE_INIT_DEFAULT` (0) — Normal init
- `GSCOPE_INIT_RESTORE` (1) — Restore existing scopes from state files
- `GSCOPE_INIT_VERBOSE` (2) — Enable debug logging

**Returns:** `GSCOPE_OK` or `GSCOPE_ERR_PERM` (not root), `GSCOPE_ERR_NOMEM`

### `gscope_destroy`
```c
void gscope_destroy(gscope_ctx_t *ctx);
```
Destroy the library context. Does NOT stop running scopes.

### `gscope_version`
```c
const char *gscope_version(void);
```
Returns version string (e.g. `"0.1.0"`).

---

## Scope Lifecycle

### `gscope_scope_create`
```c
gscope_err_t gscope_scope_create(gscope_ctx_t *ctx,
                                  const gscope_config_t *config,
                                  gscope_scope_t **scope);
```
Create a new scope. Orchestrates: directories → overlay → cgroup → namespace → networking → user. Rolls back on any failure.

### `gscope_scope_start`
```c
gscope_err_t gscope_scope_start(gscope_scope_t *scope,
                                 const char *init_command);
```
Start a stopped scope. Spawns init process. Pass `NULL` for `sleep infinity`.

### `gscope_scope_stop`
```c
gscope_err_t gscope_scope_stop(gscope_scope_t *scope,
                                unsigned int timeout_sec);
```
Stop a running scope. SIGTERM → wait → SIGKILL.

### `gscope_scope_delete`
```c
gscope_err_t gscope_scope_delete(gscope_scope_t *scope, bool force);
```
Delete scope and free all resources (reverse of create).

### `gscope_scope_status`
```c
gscope_err_t gscope_scope_status(gscope_scope_t *scope,
                                  gscope_status_t *status);
```

### `gscope_scope_metrics`
```c
gscope_err_t gscope_scope_metrics(gscope_scope_t *scope,
                                   gscope_metrics_t *metrics);
```
Read cgroup stats: CPU usage, memory current/limit, PID count.

### `gscope_scope_get` / `gscope_scope_list`
```c
gscope_scope_t *gscope_scope_get(gscope_ctx_t *ctx, gscope_id_t id);
int gscope_scope_list(gscope_ctx_t *ctx, gscope_id_t *ids, int max);
```

---

## Process Execution

### `gscope_exec`
```c
gscope_err_t gscope_exec(gscope_scope_t *scope,
                          const gscope_exec_config_t *config,
                          gscope_exec_result_t *result);
```
Execute a command inside a scope with full isolation pipeline. Supports PTY allocation.

### `gscope_process_signal` / `gscope_process_wait`
```c
gscope_err_t gscope_process_signal(gscope_exec_result_t *result, int sig);
gscope_err_t gscope_process_wait(gscope_exec_result_t *result,
                                  int *exit_status, int timeout_ms);
```

### `gscope_process_resize_pty`
```c
gscope_err_t gscope_process_resize_pty(gscope_exec_result_t *result,
                                        uint16_t rows, uint16_t cols);
```

### `gscope_process_release`
```c
void gscope_process_release(gscope_exec_result_t *result);
```
Close pidfd and pty_fd.

---

## Template Execution

### `gscope_template_parse` / `gscope_template_parse_file`
```c
gscope_err_t gscope_template_parse(const char *json, gscope_template_t **tmpl);
gscope_err_t gscope_template_parse_file(const char *path, gscope_template_t **tmpl);
```

### `gscope_template_execute`
```c
gscope_err_t gscope_template_execute(gscope_scope_t *scope,
                                      const gscope_template_t *tmpl,
                                      gscope_tmpl_progress_fn progress_fn,
                                      void *userdata,
                                      gscope_tmpl_result_t *result);
```

### `gscope_template_set_var` / `gscope_template_get_var`
```c
gscope_err_t gscope_template_set_var(gscope_template_t *tmpl,
                                      const char *key, const char *value);
const char *gscope_template_get_var(const gscope_template_t *tmpl,
                                     const char *key);
```

---

## Error Handling

All functions return `gscope_err_t`. Thread-safe error details:

```c
gscope_err_t gscope_last_error(void);     // Error code
const char  *gscope_strerror(void);        // Human-readable message
int          gscope_last_errno(void);      // Captured errno
const char  *gscope_err_name(gscope_err_t); // "GSCOPE_ERR_NAMESPACE"
```

### Error Codes

| Code | Name | Meaning |
|------|------|---------|
| 0 | `GSCOPE_OK` | Success |
| -1 | `GSCOPE_ERR_INVAL` | Invalid argument |
| -3 | `GSCOPE_ERR_PERM` | Permission denied |
| -4 | `GSCOPE_ERR_EXIST` | Already exists |
| -5 | `GSCOPE_ERR_NOENT` | Not found |
| -6 | `GSCOPE_ERR_STATE` | Wrong state for operation |
| -8 | `GSCOPE_ERR_TIMEOUT` | Timed out |
| -20 | `GSCOPE_ERR_NAMESPACE` | Namespace error |
| -21 | `GSCOPE_ERR_CGROUP` | Cgroup error |
| -22 | `GSCOPE_ERR_NETWORK` | Network error |
| -24 | `GSCOPE_ERR_ROOTFS` | Rootfs/overlay error |
| -26 | `GSCOPE_ERR_PROCESS` | Process error |
| -27 | `GSCOPE_ERR_SECCOMP` | Seccomp error |
| -40 | `GSCOPE_ERR_UNSUPPORTED` | Feature not available |
