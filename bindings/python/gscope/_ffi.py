"""
gscope._ffi — CFFI ABI-mode bindings to libgscope.so

Loads the shared library via dlopen (no compilation needed at install time).
All C types and functions are declared here for FFI access.

Copyright 2026 Gritiva
SPDX-License-Identifier: Apache-2.0
"""

import os
import ctypes
import ctypes.util

# ─── Find and load libgscope ─────────────────────────────────────────

_lib = None

def _find_library():
    """Find libgscope.so in standard locations."""
    # 1. Environment variable
    path = os.environ.get("GSCOPE_LIB_PATH")
    if path and os.path.exists(path):
        return path

    # 2. Next to this file (development)
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in [
        os.path.join(here, "..", "..", "..", "build", "libgscope.so"),
        os.path.join(here, "..", "..", "..", "build", "libgscope.so.0"),
        os.path.join(here, "..", "..", "..", "build", "libgscope.0.dylib"),
    ]:
        if os.path.exists(candidate):
            return os.path.abspath(candidate)

    # 3. System library path
    found = ctypes.util.find_library("gscope")
    if found:
        return found

    # 4. Standard locations
    for p in ["/usr/lib/libgscope.so", "/usr/local/lib/libgscope.so",
              "/usr/lib/libgscope.so.0", "/usr/local/lib/libgscope.so.0"]:
        if os.path.exists(p):
            return p

    return None


def get_lib():
    """Get the loaded library, loading it if necessary."""
    global _lib
    if _lib is not None:
        return _lib

    path = _find_library()
    if not path:
        raise RuntimeError(
            "libgscope not found. Set GSCOPE_LIB_PATH or install libgscope.\n"
            "Build with: meson setup build && ninja -C build"
        )

    _lib = ctypes.CDLL(path)
    _setup_prototypes(_lib)
    return _lib


# ─── C Type Definitions ──────────────────────────────────────────────

class gscope_ctx_t(ctypes.c_void_p):
    """Opaque context handle."""
    pass

class gscope_scope_t(ctypes.c_void_p):
    """Opaque scope handle."""
    pass


class gscope_config_t(ctypes.Structure):
    """Scope configuration struct."""
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("isolation", ctypes.c_int),
        ("net_mode", ctypes.c_int),
        ("privilege", ctypes.c_int),
        ("ns_flags", ctypes.c_uint32),
        # Resource limits
        ("cpu_cores", ctypes.c_float),
        ("cpu_weight", ctypes.c_uint32),
        ("memory_bytes", ctypes.c_uint64),
        ("memory_swap_bytes", ctypes.c_uint64),
        ("max_pids", ctypes.c_uint32),
        ("io_weight", ctypes.c_uint32),
        # Rootfs
        ("template_path", ctypes.c_char_p),
        ("rootfs_base", ctypes.c_char_p),
        # Network
        ("bridge_name", ctypes.c_char_p),
        ("requested_ip", ctypes.c_char_p),
        ("gateway", ctypes.c_char_p),
        # User
        ("username", ctypes.c_char_p),
        ("uid", ctypes.c_uint32),
        ("gid", ctypes.c_uint32),
        ("password", ctypes.c_char_p),
        # Security
        ("seccomp", ctypes.c_int),
        ("seccomp_profile_path", ctypes.c_char_p),
        ("cap_keep", ctypes.c_uint64),
        ("cap_drop", ctypes.c_uint64),
        # Misc
        ("hostname", ctypes.c_char_p),
    ]


class gscope_status_t(ctypes.Structure):
    """Scope status struct."""
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("state", ctypes.c_int),
        ("health", ctypes.c_int),
        ("init_pid", ctypes.c_int),
        ("pidfd", ctypes.c_int),
        ("ip_address", ctypes.c_char * 16),
        ("hostname", ctypes.c_char * 64),
        ("rootfs_path", ctypes.c_char * 4096),
        ("cgroup_path", ctypes.c_char * 256),
        ("created_at", ctypes.c_uint64),
        ("started_at", ctypes.c_uint64),
    ]


class gscope_metrics_t(ctypes.Structure):
    """Scope metrics struct."""
    _fields_ = [
        ("cpu_usage_us", ctypes.c_uint64),
        ("cpu_percent", ctypes.c_float),
        ("memory_current", ctypes.c_uint64),
        ("memory_limit", ctypes.c_uint64),
        ("memory_percent", ctypes.c_float),
        ("pids_current", ctypes.c_uint32),
        ("pids_limit", ctypes.c_uint32),
        ("net_rx_bytes", ctypes.c_uint64),
        ("net_tx_bytes", ctypes.c_uint64),
        ("net_rx_packets", ctypes.c_uint64),
        ("net_tx_packets", ctypes.c_uint64),
        ("disk_usage_bytes", ctypes.c_uint64),
    ]


class gscope_exec_config_t(ctypes.Structure):
    """Process exec configuration."""
    _fields_ = [
        ("command", ctypes.c_char_p),
        ("argv", ctypes.POINTER(ctypes.c_char_p)),
        ("envp", ctypes.POINTER(ctypes.c_char_p)),
        ("work_dir", ctypes.c_char_p),
        ("allocate_pty", ctypes.c_bool),
        ("pty_rows", ctypes.c_uint16),
        ("pty_cols", ctypes.c_uint16),
        ("uid", ctypes.c_uint32),
        ("gid", ctypes.c_uint32),
    ]


class gscope_exec_result_t(ctypes.Structure):
    """Process exec result."""
    _fields_ = [
        ("pid", ctypes.c_int),
        ("pidfd", ctypes.c_int),
        ("pty_fd", ctypes.c_int),
        ("has_pty", ctypes.c_bool),
    ]


# ─── Setup Function Prototypes ───────────────────────────────────────

def _setup_prototypes(lib):
    """Declare C function signatures for type safety."""

    # Library lifecycle
    lib.gscope_version.restype = ctypes.c_char_p
    lib.gscope_version.argtypes = []

    lib.gscope_init.restype = ctypes.c_int
    lib.gscope_init.argtypes = [
        ctypes.POINTER(gscope_ctx_t),
        ctypes.c_uint,
    ]

    lib.gscope_destroy.restype = None
    lib.gscope_destroy.argtypes = [gscope_ctx_t]

    # Error
    lib.gscope_strerror.restype = ctypes.c_char_p
    lib.gscope_strerror.argtypes = []

    lib.gscope_last_error.restype = ctypes.c_int
    lib.gscope_last_error.argtypes = []

    # Scope lifecycle
    lib.gscope_scope_create.restype = ctypes.c_int
    lib.gscope_scope_create.argtypes = [
        gscope_ctx_t,
        ctypes.POINTER(gscope_config_t),
        ctypes.POINTER(gscope_scope_t),
    ]

    lib.gscope_scope_start.restype = ctypes.c_int
    lib.gscope_scope_start.argtypes = [gscope_scope_t, ctypes.c_char_p]

    lib.gscope_scope_stop.restype = ctypes.c_int
    lib.gscope_scope_stop.argtypes = [gscope_scope_t, ctypes.c_uint]

    lib.gscope_scope_delete.restype = ctypes.c_int
    lib.gscope_scope_delete.argtypes = [gscope_scope_t, ctypes.c_bool]

    lib.gscope_scope_status.restype = ctypes.c_int
    lib.gscope_scope_status.argtypes = [
        gscope_scope_t,
        ctypes.POINTER(gscope_status_t),
    ]

    lib.gscope_scope_metrics.restype = ctypes.c_int
    lib.gscope_scope_metrics.argtypes = [
        gscope_scope_t,
        ctypes.POINTER(gscope_metrics_t),
    ]

    lib.gscope_scope_get.restype = gscope_scope_t
    lib.gscope_scope_get.argtypes = [gscope_ctx_t, ctypes.c_uint32]

    lib.gscope_scope_list.restype = ctypes.c_int
    lib.gscope_scope_list.argtypes = [
        gscope_ctx_t,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_int,
    ]

    # Exec
    lib.gscope_exec.restype = ctypes.c_int
    lib.gscope_exec.argtypes = [
        gscope_scope_t,
        ctypes.POINTER(gscope_exec_config_t),
        ctypes.POINTER(gscope_exec_result_t),
    ]

    lib.gscope_process_release.restype = None
    lib.gscope_process_release.argtypes = [ctypes.POINTER(gscope_exec_result_t)]
