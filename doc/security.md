# Security Model

gscope implements defense-in-depth with 7 isolation layers.

## Layer 1: Linux Namespaces

| Namespace | Flag | Isolation |
|-----------|------|-----------|
| PID | `CLONE_NEWPID` | Separate process tree (PID 1 inside scope) |
| NET | `CLONE_NEWNET` | Own network interfaces, IP, routing |
| MNT | `CLONE_NEWNS` | Own mount table, private mounts |
| UTS | `CLONE_NEWUTS` | Own hostname |
| IPC | `CLONE_NEWIPC` | Own semaphores, message queues, shared memory |
| USER | `CLONE_NEWUSER` | UID/GID mapping (rootless support) |
| CGROUP | `CLONE_NEWCGROUP` | Own cgroup root view |

## Layer 2: Cgroup v2 Resource Limits

All scopes are placed under `/sys/fs/cgroup/gscope.slice/scope-{id}/`:

| Control | File | Description |
|---------|------|-------------|
| CPU | `cpu.max` | Quota/period in microseconds |
| CPU weight | `cpu.weight` | Proportional sharing (1-10000) |
| Memory hard | `memory.max` | OOM kill threshold |
| Memory soft | `memory.high` | Throttle threshold (90% of max) |
| Swap | `memory.swap.max` | Swap limit |
| PIDs | `pids.max` | Fork bomb protection |
| I/O weight | `io.weight` | I/O priority |
| OOM | `memory.oom.group` | Kill entire cgroup on OOM |

## Layer 3: Filesystem Isolation

- **OverlayFS**: Read-only template + per-scope writable layer
- **pivot_root**: Old root completely unmounted (unlike chroot which is escapable)
- **/dev allowlist**: Only null, zero, full, random, urandom, tty
- **Path masking**: /proc/kcore, /proc/keys, /proc/sched_debug hidden
- **Read-only**: /proc/sys, /proc/bus, /proc/irq remounted RO

## Layer 4: seccomp-bpf Syscall Filter

BPF program generated at runtime. Architecture validated (blocks 32-bit bypass on x86_64).

### Blocked Syscalls (DEFAULT profile)

**System control:** reboot, swapon/off, settimeofday, clock_settime, clock_adjtime, adjtimex, acct

**Kernel modules:** init_module, delete_module, finit_module, kexec_load, kexec_file_load

**io_uring (seccomp bypass):** io_uring_setup, io_uring_enter, io_uring_register

**New mount API:** mount_setattr, move_mount, open_tree, fsopen, fsconfig, fsmount, fspick

**Escape vectors:** open_by_handle_at, name_to_handle_at, userfaultfd

**Memory access:** process_vm_readv, process_vm_writev

**eBPF:** bpf

**Info leaks:** perf_event_open, syslog, lookup_dcookie

**NUMA:** mbind, set_mempolicy, migrate_pages, move_pages

**Other:** personality, add_key, request_key, keyctl, vm86, vm86old

### STRICT profile (additional)

mount, umount2, pivot_root, chroot, mknod, mknodat, ptrace, kcmp, unshare, setns, clone, clone3, ioctl

## Layer 5: Linux Capabilities

Capabilities retained depend on isolation level:

| Capability | MINIMAL | STANDARD | HIGH | MAXIMUM |
|------------|---------|----------|------|---------|
| CAP_CHOWN | Yes | Yes | Yes | No |
| CAP_DAC_OVERRIDE | Yes | Yes | Yes | No |
| CAP_FOWNER | Yes | Yes | Yes | No |
| CAP_KILL | Yes | Yes | Yes | Yes |
| CAP_SETUID | Yes | Yes | Yes | Yes |
| CAP_SETGID | Yes | Yes | Yes | Yes |
| CAP_NET_BIND_SERVICE | Yes | Yes | Yes | Yes |
| CAP_NET_RAW | Yes | No | No | No |
| CAP_SYS_CHROOT | No | Yes | No | No |
| CAP_MKNOD | No | Yes | No | No |

## Layer 6: Privilege Prevention

- **PR_SET_NO_NEW_PRIVS**: Prevents setuid/setgid binaries from granting privileges
- **PR_SET_DUMPABLE=0**: Prevents core dumps and /proc/pid access by others
- **Supplementary groups cleared**: Only primary GID retained
- **Privilege drop verification**: After setuid, attempts setuid(0) to verify it fails

## Layer 7: File Descriptor Isolation

Before exec, the child process closes ALL inherited file descriptors except stdin/stdout/stderr and the PTY slave. This prevents:
- Leaked host sockets from being used
- Host file handles from being accessed
- Parent's netlink socket from being reused

## Security Execution Order

The order in `child_exec()` is critical:

```
1. Close fds           ← prevent host resource leaks
2. Enter namespaces    ← requires CAP_SYS_ADMIN
3. Mount filesystems   ← requires CAP_SYS_ADMIN
4. pivot_root          ← requires CAP_SYS_ADMIN (last privileged op)
5. Drop capabilities   ← AFTER all privileged operations
6. NO_NEW_PRIVS        ← BEFORE seccomp
7. seccomp filter      ← LAST security step (restricts everything after)
8. Drop to user        ← setuid is irreversible
9. exec                ← new process image
```

Reordering any of these steps creates security vulnerabilities.
