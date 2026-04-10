# gscope

**Lightweight Linux scope isolation library** — a production-grade C library for creating isolated execution environments using Linux namespaces, cgroups v2, OverlayFS, and seccomp-bpf.

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/language-C11-brightgreen.svg)]()

## What is gscope?

gscope sits between simple `unshare(1)` and full container runtimes like Docker:

```
Lighter ←─────────────────────────────────────→ Heavier

unshare    gscope    systemd-nspawn    LXC    Docker
```

It provides a clean C API (with Python bindings) for:
- **Namespace isolation** — PID, NET, MNT, UTS, IPC, USER
- **Resource limits** — CPU, memory, I/O, PIDs via cgroup v2
- **Filesystem isolation** — OverlayFS with `pivot_root` (not chroot)
- **Security** — seccomp-bpf syscall filtering, Linux capabilities, `PR_SET_NO_NEW_PRIVS`
- **Networking** — bridges, veth pairs, IP allocation, NAT — all via netlink (no subprocess)
- **PTY support** — full interactive terminal for exec'd processes
- **Race-free process management** — `pidfd_open(2)` with graceful fallbacks

## Quick Start

### Build

```bash
meson setup build
ninja -C build
sudo ninja -C build test
```

### CLI

```bash
sudo gscopectl create --id 1 --cpu 2 --mem 1024
sudo gscopectl start  --id 1
sudo gscopectl exec   --id 1 -- /bin/bash
sudo gscopectl list
sudo gscopectl stop   --id 1
sudo gscopectl delete --id 1
```

### C API

```c
#include <gscope/gscope.h>

gscope_ctx_t *ctx;
gscope_init(&ctx, 0);

gscope_config_t config;
gscope_config_init(&config);
config.id = 1;
config.cpu_cores = 2.0;
config.memory_bytes = 1024ULL * 1024 * 1024;

gscope_scope_t *scope;
gscope_scope_create(ctx, &config, &scope);
gscope_scope_start(scope, NULL);

// Execute inside scope
gscope_exec_config_t exec = { .command = "/bin/bash", .allocate_pty = true };
gscope_exec_result_t result;
gscope_exec(scope, &exec, &result);

gscope_scope_stop(scope, 10);
gscope_scope_delete(scope, false);
gscope_destroy(ctx);
```

### Python

```python
from gscope import Context

with Context() as ctx:
    scope = ctx.create(1, cpu_cores=2.0, memory_mb=1024)
    scope.start()
    scope.exec("/bin/bash", pty=True)
    scope.stop()
    scope.delete()
```

## Architecture

```
include/gscope/        Public API headers (11 files)
src/
├── scope.c            Lifecycle orchestrator
├── cgroup/            cgroup v2 management
├── fs/                OverlayFS, mount, pivot_root
├── ns/                Namespace management (clone3/unshare/setns)
├── proc/              Process spawning, PTY, pidfd
├── sec/               seccomp-bpf, capabilities, privilege drop
├── net/               Netlink networking (bridge, veth, addr, route)
├── user/              User management (direct passwd/shadow writes)
└── state.c            JSON state persistence

cli/                   gscopectl CLI tool
bindings/python/       Python ctypes bindings
tests/                 Unit test suite
```

## Requirements

- Linux 5.3+ (for `pidfd_open`, `clone3`)
- cgroup v2 (unified hierarchy)
- Root privileges (CAP_SYS_ADMIN)
- C11 compiler (gcc/clang)
- Meson + Ninja (build system)

## License

Apache License 2.0 — see [LICENSE](LICENSE).

Copyright 2026 Gritiva.
