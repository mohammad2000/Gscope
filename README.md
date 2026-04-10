# gscope

<p align="center">
  <strong>Lightweight Linux Scope Isolation Library</strong><br>
  A production-grade C library for creating isolated execution environments<br>
  using Linux namespaces, cgroups v2, OverlayFS, seccomp-bpf, and netlink networking.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue.svg" alt="License"></a>
  <img src="https://img.shields.io/badge/language-C11-brightgreen.svg" alt="C11">
  <img src="https://img.shields.io/badge/kernel-5.3%2B-orange.svg" alt="Linux 5.3+">
  <img src="https://img.shields.io/badge/cgroup-v2-purple.svg" alt="cgroup v2">
  <img src="https://img.shields.io/badge/build-meson%20%2B%20ninja-yellow.svg" alt="Meson">
</p>

---

## What is gscope?

gscope creates **lightweight isolated environments** (scopes) on Linux. Think of it as a container runtime that sits between simple `unshare(1)` and full Docker:

```
Lighter ────────────────────────────────────── Heavier

unshare(1)    gscope    systemd-nspawn    LXC    Docker
   |            |             |            |        |
 1 syscall   C library    boot needed   images   daemon
 no API      clean API    systemd dep   complex  heavy
```

**Why gscope?**

- **Zero dependencies** — only libc and Linux kernel headers
- **Single library** — `libgscope.so` (~200KB) with clean C API
- **No daemon** — direct syscall-based, no background service needed
- **Template system** — JSON config to provision scopes automatically
- **Production security** — seccomp-bpf, capabilities, pivot_root, path masking

---

## Quick Start

### Build

```bash
# Requirements: Linux, GCC/Clang, Meson, Ninja
meson setup build
ninja -C build
sudo ninja -C build test    # 33 tests
```

### Create Your First Scope

```bash
# Create a base filesystem template
sudo debootstrap --variant=minbase noble /opt/templates/base http://archive.ubuntu.com/ubuntu

# Create an isolated scope
sudo gscopectl create --id 1 --cpu 2 --mem 1024 --template /opt/templates/base

# Start it
sudo gscopectl start --id 1

# Execute commands inside
sudo gscopectl exec --id 1 -- /bin/bash
sudo gscopectl exec --id 1 -- python3 -c "print('Hello from scope!')"

# Check status
sudo gscopectl status --id 1
sudo gscopectl list

# Clean up
sudo gscopectl stop --id 1
sudo gscopectl delete --id 1
```

### Provision with Templates

```bash
# Create a template JSON
cat > python-app.json << 'EOF'
{
  "name": "Python Web App",
  "variables": { "project": "myapp", "port": "8080" },
  "packages": [
    {"name": "python3-pip", "manager": "apt"},
    {"name": "sqlite3", "manager": "apt"}
  ],
  "files": [
    {"path": "/opt/app/server.py", "type": "template",
     "content": "# ${project}\nfrom http.server import *\nHTTPServer(('0.0.0.0',${port}),SimpleHTTPRequestHandler).serve_forever()"}
  ],
  "setup_script": "python3 --version && echo '${project} ready on port ${port}'",
  "verification": {
    "commands": [{"name": "python", "command": "python3 --version"}],
    "files": [{"path": "/opt/app/server.py"}]
  }
}
EOF

# Apply to running scope
sudo gscopectl template --id 1 --file python-app.json
```

---

## Architecture

```
gscope/
├── include/gscope/           Public C API (12 headers)
│   ├── gscope.h              Umbrella header
│   ├── types.h               Core types, enums, config structs
│   ├── error.h               Error codes and retrieval
│   ├── scope.h               Scope lifecycle (create/start/stop/delete)
│   ├── namespace.h            Namespace management
│   ├── cgroup.h              Cgroup v2 resource limits
│   ├── network.h             Networking (bridge, veth, IP)
│   ├── process.h             Process spawning with PTY
│   ├── rootfs.h              OverlayFS and root filesystem
│   ├── security.h            Seccomp and capabilities
│   ├── user.h                User management
│   └── template.h            Template executor
│
├── src/                      Implementation (37 C files)
│   ├── scope.c               Lifecycle orchestrator
│   ├── state.c               JSON state persistence
│   ├── error.c / log.c / util.c   Foundation
│   ├── cgroup/               Cgroup v2 (limits, stats, freeze, kill)
│   ├── fs/                   OverlayFS, mount, pivot_root, /dev setup
│   ├── ns/                   Namespaces (clone3, setns, user ns mapping)
│   ├── proc/                 Process spawning, PTY, pidfd, wait
│   ├── sec/                  Seccomp BPF, capabilities, privilege drop
│   ├── net/                  Netlink (bridge, veth, addr, route, firewall)
│   ├── user/                 User/group/sudo (direct file writes)
│   └── template/             Template executor (JSON, vars, packages)
│
├── cli/                      gscopectl command-line tool
├── bindings/python/          Python ctypes bindings
├── tests/                    Unit test suite (8 suites, 33 tests)
├── meson.build               Build system
└── gscope.sym                Symbol versioning for ABI stability
```

### How It Works

```
┌─────────────────────────────────────────────────────────────┐
│  gscope_scope_create()                                      │
│                                                             │
│  [1] Create directories    /opt/gritiva/scopes/{id}/        │
│  [2] Mount OverlayFS       template (RO) + upper (RW)       │
│  [3] Create cgroup         cpu.max, memory.max, pids.max    │
│  [4] Create namespace      Named NET ns + PID/MNT/UTS/IPC   │
│  [5] Setup networking      Bridge + veth + IP + NAT          │
│  [6] Create user           /etc/passwd + sudo + .bashrc      │
│  [7] Save state            state.json (atomic write)         │
│                                                             │
│  On ANY failure → rollback in reverse order                 │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  gscope_exec() — Fork Pipeline (child process)             │
│                                                             │
│   0. Close all inherited fds     (fd leak prevention)       │
│   1. setsid + PTY setup          (new session)              │
│   2. setns(NET)                  (enter network namespace)  │
│   3. unshare(PID|MNT|UTS|IPC)   (create new namespaces)    │
│   4. mount(/, MS_PRIVATE)        (isolate mount events)     │
│   5. [PID ns] fork()            (become PID 1)              │
│   6. dev_setup (mknod)          (create /dev nodes)         │
│   7. mount_essential             (/proc, /sys, /dev/pts)    │
│   8. mask_paths                  (/proc/kcore, /proc/keys)  │
│   9. pivot_root                  (unmount old root)         │
│  10. caps_set(keep_mask)         (drop capabilities)        │
│  11. PR_SET_NO_NEW_PRIVS         (prevent priv escalation)  │
│  12. seccomp_apply(DEFAULT)      (syscall filter)           │
│  13. setgid → setgroups → setuid (drop to non-root)        │
│  14. execvp(command)             (replace process image)    │
└─────────────────────────────────────────────────────────────┘
```

---

## C API Reference

### Library Lifecycle

```c
#include <gscope/gscope.h>

gscope_ctx_t *ctx;
gscope_init(&ctx, GSCOPE_INIT_RESTORE | GSCOPE_INIT_VERBOSE);

// ... use library ...

gscope_destroy(ctx);
```

### Scope Lifecycle

```c
// Configure
gscope_config_t config;
gscope_config_init(&config);     // Safe defaults
config.id = 1;
config.cpu_cores = 2.0;
config.memory_bytes = 1024ULL * 1024 * 1024;  // 1 GB
config.max_pids = 500;
config.template_path = "/opt/templates/base";
config.net_mode = GSCOPE_NET_BRIDGE;
config.isolation = GSCOPE_ISOLATION_HIGH;
config.seccomp = GSCOPE_SECCOMP_DEFAULT;

// Create
gscope_scope_t *scope;
gscope_scope_create(ctx, &config, &scope);

// Start (spawns init process)
gscope_scope_start(scope, NULL);  // NULL = sleep infinity

// Execute inside
gscope_exec_config_t exec = {
    .command = "/bin/bash",
    .allocate_pty = true,
    .pty_rows = 30,
    .pty_cols = 120,
};
gscope_exec_result_t result;
gscope_exec(scope, &exec, &result);
// result.pid, result.pty_fd, result.pidfd available

// Query
gscope_status_t status;
gscope_scope_status(scope, &status);

gscope_metrics_t metrics;
gscope_scope_metrics(scope, &metrics);

// Lifecycle
gscope_scope_stop(scope, 10);    // 10s graceful timeout
gscope_scope_delete(scope, false);
```

### Template Execution

```c
// Parse template from JSON
gscope_template_t *tmpl;
gscope_template_parse_file("app-template.json", &tmpl);

// Override variables
gscope_template_set_var(tmpl, "port", "3000");
gscope_template_set_var(tmpl, "db_host", "10.50.0.20");

// Execute on running scope
gscope_tmpl_result_t result;
gscope_template_execute(scope, tmpl, my_progress_callback, NULL, &result);

printf("Packages: %d, Files: %d, Duration: %.1fs\n",
       result.packages_installed, result.files_created, result.duration_sec);

gscope_template_free(tmpl);
```

### Error Handling

```c
gscope_err_t err = gscope_scope_create(ctx, &config, &scope);
if (err != GSCOPE_OK) {
    fprintf(stderr, "Error [%s]: %s (errno=%d)\n",
            gscope_err_name(err),    // "GSCOPE_ERR_NAMESPACE"
            gscope_strerror(),        // "setns(NET) failed: No such file"
            gscope_last_errno());     // 2 (ENOENT)
}
```

---

## Python API

```python
from gscope import Context, Isolation, Privilege

# Context manager for automatic cleanup
with Context(verbose=True) as ctx:
    # Create scope
    scope = ctx.create(
        scope_id=1,
        cpu_cores=2.0,
        memory_mb=1024,
        template_path="/opt/templates/base",
        isolation=Isolation.HIGH,
        privilege=Privilege.STANDARD,
    )

    # Start
    scope.start()

    # Execute
    result = scope.exec("/bin/bash", pty=True, rows=30, cols=120)
    print(f"PID: {result.pid}, PTY: {result.has_pty}")

    # Query
    print(scope.status())    # dict with state, pid, ip, etc.
    print(scope.metrics())   # dict with cpu, memory, pids

    # Stop & delete
    scope.stop(timeout=10)
    scope.delete()
```

### GritivaCore Drop-in Adapter

```python
# Replace Python scope system with C library:
# from scope.manager import ScopeManager
from gscope.adapter import GscopeScopeManager as ScopeManager

manager = ScopeManager()
await manager.initialize()
result = await manager.create_scope(1, {"cpu_cores": 2, "memory_mb": 1024})
await manager.start_scope(1)
await manager.delete_scope(1)
```

---

## CLI Reference

### `gscopectl create`

```
gscopectl create [options]

  --id N            Scope ID (required)
  --cpu N           CPU cores (default: 1.0)
  --mem N           Memory in MB (default: 512)
  --pids N          Max processes (default: 1024)
  --template PATH   Base rootfs template path
  --user NAME       Username inside scope (default: root)
  --hostname NAME   Hostname (default: scope-{id})
  --ip ADDR         Request specific IP address
  --net MODE        bridge|host|none (default: bridge)
  --isolation LVL   minimal|standard|high|maximum (default: standard)
```

### `gscopectl template`

```
gscopectl template --id N --file TEMPLATE.json

Executes a template on a running scope:
  - Installs packages (apt, pip, npm, cargo, gem)
  - Creates config files with variable substitution
  - Runs setup/startup scripts
  - Verifies installation (commands, files, ports)
```

### Other Commands

```
gscopectl start   --id N [--command CMD]
gscopectl stop    --id N [--timeout SEC]
gscopectl delete  --id N [--force]
gscopectl list
gscopectl status  --id N
gscopectl exec    --id N [--no-pty] -- COMMAND [ARGS...]
gscopectl version
```

---

## Template System

Templates are JSON configs that provision scopes automatically.

### Template Format

```json
{
  "name": "My Application",
  "version": "1.0.0",
  "variables": {
    "project_name": "myapp",
    "port": "8080",
    "db_path": "/opt/data/app.db"
  },
  "packages": [
    {"name": "python3-pip", "manager": "apt", "required": true},
    {"name": "flask", "manager": "pip"},
    {"name": "express", "manager": "npm"}
  ],
  "files": [
    {
      "path": "/opt/app/config.ini",
      "type": "template",
      "mode": "0644",
      "content": "[app]\nname = ${project_name}\nport = ${port}\ndb = ${db_path}\n"
    }
  ],
  "pre_install_script": "mkdir -p /opt/app /opt/data",
  "setup_script": "cd /opt/app && python3 -m venv venv",
  "startup_script": "cd /opt/app && python3 app.py",
  "health_check_script": "curl -sf http://localhost:${port}/health",
  "verification": {
    "commands": [{"name": "python", "command": "python3 --version"}],
    "files": [{"path": "/opt/app/config.ini"}],
    "ports": [{"port": 8080}]
  }
}
```

### Variable Substitution

| Syntax | Description | Example |
|--------|-------------|---------|
| `${var}` | Basic substitution | `${port}` → `8080` |
| `$var` | Bash-style | `$port` → `8080` |
| `{{var}}` | Jinja2-style | `{{port}}` → `8080` |
| `${var:-default}` | Default if unset | `${mode:-prod}` → `prod` |
| `${var:+alt}` | Alternate if set | `${port:+--port $port}` |
| `${var^^}` | Uppercase | `${name^^}` → `MYAPP` |
| `${var,,}` | Lowercase | `${NAME,,}` → `myapp` |

### Built-in Variables

| Variable | Value |
|----------|-------|
| `${scope_id}` | Scope numeric ID |
| `${rootfs}` | Rootfs mount path |
| `${USER}` | Username inside scope |
| `${HOME}` | Home directory |
| `${APP_DIR}` | `/opt/app` |
| `${DATA_DIR}` | `/opt/data` |
| `${CONFIG_DIR}` | `/etc/app` |
| `${LOG_DIR}` | `/var/log/app` |
| `${HOST}` | Scope IP address |
| `${LANG}` | `en_US.UTF-8` |

### Package Managers

| Manager | Install Command | Batch |
|---------|----------------|-------|
| `apt` | `apt-get install -y` | Yes |
| `pip` | `pip3 install` | No |
| `npm` | `npm install -g` | No |
| `cargo` | `cargo install` | No |
| `gem` | `gem install --no-document` | No |

### Execution Phases

```
1. PREFLIGHT      Verify scope is running, rootfs accessible
2. VARIABLES      Resolve all variables (built-in + user)
3. PRE_INSTALL    Run pre_install_script
4. PACKAGES       Install packages by manager type
5. POST_INSTALL   Run post_install_script
6. FILES          Create config files with variable substitution
7. SETUP          Run setup_script
8. VERIFICATION   Check commands, files, and ports
9. COMPLETE       Save startup/health scripts, report result
```

---

## Security

### Isolation Layers

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **Namespaces** | PID, NET, MNT, UTS, IPC, USER | Process, network, filesystem isolation |
| **Cgroups v2** | cpu.max, memory.max, pids.max | Resource limits |
| **OverlayFS** | Copy-on-write filesystem | Template sharing, write isolation |
| **pivot_root** | Root filesystem switch | Old root completely unmounted |
| **seccomp-bpf** | Syscall filter (BPF) | Block dangerous syscalls |
| **Capabilities** | capset/capget | Fine-grained privilege control |
| **Path masking** | Bind mount /dev/null | Hide sensitive /proc, /sys paths |

### Seccomp Profile (DEFAULT)

Blocks 40+ dangerous syscalls including:

- **System control**: `reboot`, `swapon/off`, `settimeofday`, `clock_settime`
- **Kernel modules**: `init_module`, `delete_module`, `finit_module`
- **io_uring**: `io_uring_setup/enter/register` (bypasses seccomp!)
- **New mount API**: `fsopen`, `fsmount`, `move_mount`, `open_tree`
- **Escape vectors**: `open_by_handle_at`, `name_to_handle_at`, `userfaultfd`
- **Memory access**: `process_vm_readv/writev`
- **eBPF**: `bpf` (powerful kernel access)
- **Info leaks**: `perf_event_open`, `syslog`, `lookup_dcookie`
- **32-bit bypass**: AUDIT_ARCH_I386 blocked on x86_64

### Capability Levels

| Isolation | Capabilities Kept |
|-----------|------------------|
| MINIMAL | Full default + NET_RAW + DAC_READ_SEARCH |
| STANDARD | CHOWN, DAC_OVERRIDE, FOWNER, KILL, SETUID/GID, NET_BIND, MKNOD, ... |
| HIGH | Reduced: no MKNOD, no NET_RAW |
| MAXIMUM | Minimal: only KILL, SETUID, SETGID, NET_BIND_SERVICE |

### /dev Device Allowlist (OCI-compliant)

Only these devices are created inside scopes:

| Device | Type | Permissions |
|--------|------|-------------|
| `/dev/null` | char 1:3 | 0666 |
| `/dev/zero` | char 1:5 | 0666 |
| `/dev/full` | char 1:7 | 0666 |
| `/dev/random` | char 1:8 | 0666 |
| `/dev/urandom` | char 1:9 | 0666 |
| `/dev/tty` | char 5:0 | 0666 |
| `/dev/ptmx` | symlink → /dev/pts/ptmx | |
| `/dev/fd` | symlink → /proc/self/fd | |

### Masked Paths

These host paths are hidden from scopes (bind-mounted with /dev/null):

```
/proc/kcore, /proc/keys, /proc/timer_list, /proc/sched_debug,
/proc/acpi, /proc/scsi, /sys/firmware, /sys/devices/virtual/powercap
```

Read-only: `/proc/bus`, `/proc/fs`, `/proc/irq`, `/proc/sys`

---

## Networking

```
┌──── HOST ──────────────────────────────────────────────┐
│                                                        │
│  br-gscope (10.50.0.1/24)  ← Linux bridge             │
│       │                                                │
│       ├── gs1h ←──veth──→ eth0 (10.50.0.10)  Scope 1  │
│       ├── gs2h ←──veth──→ eth0 (10.50.0.11)  Scope 2  │
│       └── gs3h ←──veth──→ eth0 (10.50.0.12)  Scope 3  │
│                                                        │
│  NAT: iptables MASQUERADE (scope → internet)           │
│  DNAT: port forwarding (host:port → scope:port)        │
└────────────────────────────────────────────────────────┘
```

- **Bridge mode**: Each scope gets its own IP on 10.50.0.0/24
- **Host mode**: Shares host network namespace
- **Isolated mode**: No external network access
- All networking via **netlink sockets** (no subprocess spawning)
- IP allocation: bitmap-based O(1) allocator (253 addresses)

---

## Design Principles

1. **Zero external dependencies** — only libc + kernel headers
2. **Opaque pointers** — callers never see struct internals (ABI stable)
3. **Symbol versioning** — `gscope.sym` ensures binary compatibility
4. **Graceful degradation** — clone3→clone, pidfd→kill, pivot_root→chroot
5. **Thread-safe** — per-scope mutex, TLS error state
6. **Every function returns `gscope_err_t`** — detailed error via `gscope_strerror()`
7. **Atomic state writes** — write to .tmp, rename
8. **Rollback on failure** — scope creation with full undo

---

## Requirements

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| Linux kernel | 5.3+ | 6.0+ |
| cgroup | v2 (unified) | v2 |
| Privileges | root (CAP_SYS_ADMIN) | root |
| Compiler | GCC 10+ / Clang 12+ | GCC 13+ |
| Build system | Meson 0.60+ | Meson 1.0+ |
| Architecture | x86_64, aarch64 | x86_64 |

### Feature Detection

gscope probes kernel features at init and degrades gracefully:

| Feature | Syscall | Fallback |
|---------|---------|----------|
| clone3 | `__NR_clone3` (5.3+) | `clone(2)` |
| pidfd_open | `__NR_pidfd_open` (5.3+) | `kill(pid, 0)` |
| pivot_root | `__NR_pivot_root` | `chroot(2)` |
| cgroup.kill | cgroup v2 (5.14+) | iterate cgroup.procs |
| cgroup.freeze | cgroup v2 | SIGSTOP |
| seccomp | `PR_SET_SECCOMP` | skip filter |

---

## Building

```bash
# Standard build
meson setup build
ninja -C build

# Build options
meson setup build -Dtests=true -Dcli=true -Dpython_bindings=false

# Install system-wide
sudo ninja -C build install

# Run tests (some require root)
sudo ninja -C build test

# Development (debug build)
meson setup build --buildtype=debug -Db_sanitize=address
ninja -C build
```

### Build Outputs

| Output | Description |
|--------|-------------|
| `build/libgscope.so` | Shared library |
| `build/libgscope_static.a` | Static library |
| `build/cli/gscopectl` | CLI tool |
| `build/tests/test_*` | Test executables |

---

## Comparison with Alternatives

| Feature | gscope | Docker | LXC | systemd-nspawn | bubblewrap |
|---------|--------|--------|-----|----------------|-----------|
| Language | C | Go | C | C | C |
| Dependencies | libc only | containerd, runc | liblxc | systemd | libcap |
| Daemon | No | Yes | No | No | No |
| Image format | OverlayFS dir | OCI layers | LXC images | Directory | None |
| Startup time | ~50ms | ~1s | ~1s | ~300ms | ~10ms |
| Binary size | ~200KB | ~50MB+ | ~5MB | ~2MB | ~100KB |
| Template system | Built-in | Dockerfile | Cloud-init | No | No |
| seccomp | Built-in BPF | Via runc | Via config | Limited | Basic |
| Networking | Netlink native | CNI plugins | lxc-net | systemd-networkd | None |
| cgroup version | v2 only | v1/v2 | v1/v2 | v2 | None |
| Rootless | Planned | Yes | Yes | Yes | Yes |
| API | C + Python | REST + CLI | CLI | CLI | CLI |

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Copyright 2026 Gritiva.

---

## Acknowledgments

- [cJSON](https://github.com/DaveGamble/cJSON) — MIT licensed JSON parser (vendored)
- Linux kernel developers — namespaces, cgroups, seccomp, netlink
- [OCI Runtime Spec](https://github.com/opencontainers/runtime-spec) — security reference
- [Docker default seccomp profile](https://docs.docker.com/engine/security/seccomp/) — syscall blocklist reference
