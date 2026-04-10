# gscope Architecture

## Module Dependency Graph

```
                    ┌──────────┐
                    │ scope.c  │  ← Orchestrator (ties everything together)
                    └────┬─────┘
          ┌──────────────┼──────────────────────────────┐
          │              │              │                │
     ┌────┴────┐   ┌────┴────┐   ┌────┴────┐    ┌─────┴─────┐
     │ cgroup/ │   │  fs/    │   │   ns/   │    │   net/    │
     │         │   │         │   │         │    │           │
     │ cgroup  │   │ rootfs  │   │namespace│    │  netlink  │
     │ stats   │   │ overlay │   │ userns  │    │  bridge   │
     └─────────┘   │ mount   │   │ pidns   │    │  veth     │
                   │ pivot   │   └─────────┘    │  addr     │
                   └─────────┘                  │  route    │
                                                │  firewall │
     ┌─────────┐   ┌─────────┐                  │  ip_alloc │
     │ proc/   │   │  sec/   │                  └───────────┘
     │         │   │         │
     │ spawn   │   │ seccomp │   ┌──────────┐   ┌──────────┐
     │ pty     │   │ caps    │   │ user/    │   │template/ │
     │ pidfd   │   │ priv    │   │ user.c   │   │ template │
     │ wait    │   └─────────┘   └──────────┘   │ vars     │
     └─────────┘                                │ exec     │
                                                │ pkg      │
     ┌─────────┐   ┌─────────┐                  │ file     │
     │ error.c │   │ state.c │                  │ verify   │
     │ log.c   │   └─────────┘                  └──────────┘
     │ util.c  │
     └─────────┘
```

## Syscall Usage by Module

| Module | Syscalls Used |
|--------|--------------|
| ns/namespace.c | `unshare(2)`, `setns(2)`, `open(2)`, `mount(2)` (bind), `close(2)` |
| ns/userns.c | `write(2)` to /proc/pid/{uid_map,gid_map,setgroups} |
| ns/pidns.c | `fork(2)`, `waitpid(2)`, `setsid(2)` |
| proc/spawn.c | `fork(2)`, `execvp(3)`, `dup2(2)`, `setuid(2)`, `setgid(2)`, `setgroups(2)`, `chdir(2)`, `prctl(2)`, `pipe2(2)`, `close(2)`, `setsid(2)` |
| proc/pty.c | `openpty(3)`, `ioctl(TIOCSWINSZ)`, `ioctl(TIOCSCTTY)`, `fcntl(2)` |
| proc/pidfd.c | `pidfd_open(2)`, `pidfd_send_signal(2)`, `kill(2)` |
| proc/wait.c | `waitpid(2)`, `poll(2)`, `usleep(3)` |
| fs/overlay.c | `mount(2)` type=overlay, `umount2(2)` with MNT_DETACH |
| fs/mount.c | `mount(2)` type=proc/sysfs/devpts/tmpfs, `mknod(2)`, `symlink(2)` |
| fs/pivot.c | `pivot_root(2)`, `mount(2)` MS_BIND, `umount2(2)`, `chroot(2)` |
| sec/seccomp.c | `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER)` with BPF |
| sec/caps.c | `capset(2)`, `capget(2)`, `prctl(PR_CAP_AMBIENT)` |
| sec/priv.c | `prctl(PR_SET_NO_NEW_PRIVS)`, `prctl(PR_SET_DUMPABLE)`, `setgroups(2)`, `setuid(2)`, `setgid(2)` |
| net/netlink.c | `socket(AF_NETLINK)`, `bind(2)`, `sendmsg(2)`, `recvmsg(2)` |
| net/bridge.c | netlink RTM_NEWLINK (IFLA_INFO_KIND=bridge) |
| net/veth.c | netlink RTM_NEWLINK (VETH_INFO_PEER), IFLA_NET_NS_FD |
| net/addr.c | netlink RTM_NEWADDR |
| net/route.c | netlink RTM_NEWROUTE |
| cgroup/ | `mkdir(2)`, `open(2)`, `write(2)`, `read(2)`, `rmdir(2)` on cgroupfs |
| user/user.c | `open(2)`, `write(2)`, `mkdir(2)`, `chmod(2)`, `chown(2)` |

## Thread Safety Model

- **Global context** (`gscope_ctx_t`): protected by `ctx->lock` mutex
- **Per-scope** (`gscope_scope_t`): protected by `scope->lock` mutex
- **Error state**: `__thread` TLS (thread-local storage)
- **IP allocator**: separate `ctx->ip_alloc.lock` mutex
- **Netlink sequence numbers**: atomic increment via `ctx->nl_seq`

## State Persistence

Each scope saves its state to `/opt/gritiva/scopes/{id}/state.json`:
- Atomic writes: write to `.state.json.tmp`, then `rename(2)`
- Restored on `gscope_init()` with `GSCOPE_INIT_RESTORE`
- Scopes restored in STOPPED state (processes not restarted)
