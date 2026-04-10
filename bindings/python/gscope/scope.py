"""
gscope.scope — Pythonic wrapper for gscope C library

Provides Context and Scope classes with context manager support.

Usage:
    from gscope import Context, Scope

    with Context() as ctx:
        scope = ctx.create(scope_id=1, cpu_cores=2.0, memory_mb=1024)
        scope.start()
        result = scope.exec("/bin/bash", pty=True)
        scope.stop()
        scope.delete()

Copyright 2026 Gritiva
SPDX-License-Identifier: Apache-2.0
"""

import ctypes
from typing import Optional, List, Dict, Any

from ._ffi import (
    get_lib,
    gscope_ctx_t,
    gscope_scope_t,
    gscope_config_t,
    gscope_status_t,
    gscope_metrics_t,
    gscope_exec_config_t,
    gscope_exec_result_t,
)
from .errors import check_error


# ─── Enums ───────────────────────────────────────────────────────────

class Isolation:
    MINIMAL = 0
    STANDARD = 1
    HIGH = 2
    MAXIMUM = 3


class NetMode:
    ISOLATED = 0
    BRIDGE = 1
    HOST = 2


class Privilege:
    RESTRICTED = 0
    STANDARD = 1
    ELEVATED = 2
    ROOT = 3


class State:
    CREATING = 0
    STOPPED = 1
    STARTING = 2
    RUNNING = 3
    STOPPING = 4
    ERROR = 5
    DELETING = 6
    DELETED = 7

    _NAMES = {
        0: "creating", 1: "stopped", 2: "starting", 3: "running",
        4: "stopping", 5: "error", 6: "deleting", 7: "deleted",
    }

    @classmethod
    def name(cls, state: int) -> str:
        return cls._NAMES.get(state, "unknown")


# ─── ExecResult ──────────────────────────────────────────────────────

class ExecResult:
    """Result of executing a command inside a scope."""

    def __init__(self, result: gscope_exec_result_t, lib):
        self._result = result
        self._lib = lib
        self.pid: int = result.pid
        self.pidfd: int = result.pidfd
        self.pty_fd: int = result.pty_fd
        self.has_pty: bool = result.has_pty

    def release(self):
        """Release resources (close fds)."""
        if self._result is not None:
            self._lib.gscope_process_release(ctypes.byref(self._result))
            self._result = None

    def __del__(self):
        self.release()

    def __repr__(self):
        return f"ExecResult(pid={self.pid}, pty={self.has_pty})"


# ─── Scope ───────────────────────────────────────────────────────────

class Scope:
    """Represents an isolated scope."""

    def __init__(self, handle: gscope_scope_t, ctx: 'Context'):
        self._handle = handle
        self._ctx = ctx
        self._lib = get_lib()
        self._deleted = False

    @property
    def id(self) -> int:
        return self._lib.gscope_scope_id(self._handle)

    @property
    def state(self) -> int:
        return self._lib.gscope_scope_state(self._handle)

    @property
    def state_name(self) -> str:
        return State.name(self.state)

    @property
    def is_running(self) -> bool:
        return self.state == State.RUNNING

    def status(self) -> Dict[str, Any]:
        """Get scope status as dictionary."""
        st = gscope_status_t()
        err = self._lib.gscope_scope_status(self._handle, ctypes.byref(st))
        check_error(err, self._lib)

        return {
            "id": st.id,
            "state": State.name(st.state),
            "health": st.health,
            "pid": st.init_pid if st.init_pid > 0 else None,
            "ip": st.ip_address.decode().rstrip("\x00") or None,
            "hostname": st.hostname.decode().rstrip("\x00") or None,
            "rootfs": st.rootfs_path.decode().rstrip("\x00") or None,
            "cgroup": st.cgroup_path.decode().rstrip("\x00") or None,
            "created_at": st.created_at,
            "started_at": st.started_at if st.started_at > 0 else None,
        }

    def metrics(self) -> Dict[str, Any]:
        """Get scope metrics as dictionary."""
        m = gscope_metrics_t()
        err = self._lib.gscope_scope_metrics(self._handle, ctypes.byref(m))
        check_error(err, self._lib)

        return {
            "cpu_usage_us": m.cpu_usage_us,
            "cpu_percent": m.cpu_percent,
            "memory_current": m.memory_current,
            "memory_limit": m.memory_limit,
            "memory_percent": m.memory_percent,
            "pids_current": m.pids_current,
            "pids_limit": m.pids_limit,
            "net_rx_bytes": m.net_rx_bytes,
            "net_tx_bytes": m.net_tx_bytes,
        }

    def start(self, command: Optional[str] = None):
        """Start the scope."""
        cmd = command.encode() if command else None
        err = self._lib.gscope_scope_start(self._handle, cmd)
        check_error(err, self._lib)

    def stop(self, timeout: int = 10):
        """Stop the scope."""
        err = self._lib.gscope_scope_stop(self._handle, timeout)
        check_error(err, self._lib)

    def delete(self, force: bool = False):
        """Delete the scope and free all resources."""
        err = self._lib.gscope_scope_delete(self._handle, force)
        check_error(err, self._lib)
        self._deleted = True

    def exec(self, command: str, argv: Optional[List[str]] = None,
             pty: bool = True, rows: int = 24, cols: int = 80,
             env: Optional[Dict[str, str]] = None,
             work_dir: Optional[str] = None) -> ExecResult:
        """Execute a command inside the scope."""

        # Build argv
        if argv is None:
            argv = [command]
        c_argv = (ctypes.c_char_p * (len(argv) + 1))()
        for i, arg in enumerate(argv):
            c_argv[i] = arg.encode() if isinstance(arg, str) else arg
        c_argv[len(argv)] = None

        # Build envp
        c_envp = None
        if env:
            env_strs = [f"{k}={v}" for k, v in env.items()]
            c_envp = (ctypes.c_char_p * (len(env_strs) + 1))()
            for i, s in enumerate(env_strs):
                c_envp[i] = s.encode()
            c_envp[len(env_strs)] = None

        cfg = gscope_exec_config_t(
            command=command.encode(),
            argv=c_argv,
            envp=c_envp,
            work_dir=work_dir.encode() if work_dir else None,
            allocate_pty=pty,
            pty_rows=rows,
            pty_cols=cols,
        )

        result = gscope_exec_result_t()
        err = self._lib.gscope_exec(self._handle, ctypes.byref(cfg),
                                     ctypes.byref(result))
        check_error(err, self._lib)

        return ExecResult(result, self._lib)

    def __repr__(self):
        return f"Scope(id={self.id}, state={self.state_name})"


# ─── Context ─────────────────────────────────────────────────────────

class Context:
    """
    gscope library context.

    Usage as context manager:
        with Context() as ctx:
            scope = ctx.create(1)
    """

    def __init__(self, verbose: bool = False, restore: bool = False):
        self._lib = get_lib()
        self._handle = gscope_ctx_t()

        flags = 0
        if verbose:
            flags |= 1  # GSCOPE_INIT_VERBOSE
        if restore:
            flags |= 2  # GSCOPE_INIT_RESTORE

        err = self._lib.gscope_init(ctypes.byref(self._handle), flags)
        check_error(err, self._lib)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.destroy()

    def destroy(self):
        """Destroy the context."""
        if self._handle:
            self._lib.gscope_destroy(self._handle)
            self._handle = None

    @staticmethod
    def version() -> str:
        """Get library version."""
        return get_lib().gscope_version().decode()

    def create(self, scope_id: int, *,
               cpu_cores: float = 1.0,
               memory_mb: int = 512,
               max_pids: int = 1024,
               template_path: Optional[str] = None,
               username: Optional[str] = None,
               hostname: Optional[str] = None,
               ip: Optional[str] = None,
               net_mode: int = NetMode.BRIDGE,
               isolation: int = Isolation.STANDARD,
               privilege: int = Privilege.STANDARD) -> Scope:
        """Create a new scope."""

        config = gscope_config_t()
        ctypes.memset(ctypes.byref(config), 0, ctypes.sizeof(config))

        config.id = scope_id
        config.isolation = isolation
        config.net_mode = net_mode
        config.privilege = privilege
        config.cpu_cores = cpu_cores
        config.cpu_weight = 100
        config.memory_bytes = memory_mb * 1024 * 1024
        config.max_pids = max_pids
        config.io_weight = 100
        config.seccomp = 0  # DEFAULT

        if template_path:
            config.template_path = template_path.encode()
        if username:
            config.username = username.encode()
        if hostname:
            config.hostname = hostname.encode()
        if ip:
            config.requested_ip = ip.encode()

        scope_handle = gscope_scope_t()
        err = self._lib.gscope_scope_create(
            self._handle,
            ctypes.byref(config),
            ctypes.byref(scope_handle),
        )
        check_error(err, self._lib)

        return Scope(scope_handle, self)

    def get(self, scope_id: int) -> Optional[Scope]:
        """Get an existing scope by ID."""
        handle = self._lib.gscope_scope_get(self._handle, scope_id)
        if not handle:
            return None
        return Scope(handle, self)

    def list(self) -> List[int]:
        """List all scope IDs."""
        ids = (ctypes.c_uint32 * 256)()
        count = self._lib.gscope_scope_list(self._handle, ids, 256)
        return [ids[i] for i in range(min(count, 256))]

    def __repr__(self):
        return f"Context(scopes={self.list()})"
