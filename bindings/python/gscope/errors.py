"""
gscope.errors — Exception classes for gscope Python bindings

Maps C error codes to Python exceptions.

Copyright 2026 Gritiva
SPDX-License-Identifier: Apache-2.0
"""


class GscopeError(Exception):
    """Base exception for all gscope errors."""

    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message
        super().__init__(f"[{code}] {message}")


class InvalidArgumentError(GscopeError):
    """Invalid argument passed to gscope function."""
    pass


class PermissionError(GscopeError):
    """Permission denied (usually needs root)."""
    pass


class NotFoundError(GscopeError):
    """Scope or resource not found."""
    pass


class AlreadyExistsError(GscopeError):
    """Scope or resource already exists."""
    pass


class InvalidStateError(GscopeError):
    """Operation invalid for current scope state."""
    pass


class NamespaceError(GscopeError):
    """Namespace operation failed."""
    pass


class CgroupError(GscopeError):
    """Cgroup operation failed."""
    pass


class NetworkError(GscopeError):
    """Network operation failed."""
    pass


class RootfsError(GscopeError):
    """Rootfs/overlay operation failed."""
    pass


class ProcessError(GscopeError):
    """Process spawn/signal failed."""
    pass


class SecurityError(GscopeError):
    """Security (seccomp/caps) error."""
    pass


class TimeoutError(GscopeError):
    """Operation timed out."""
    pass


class UnsupportedError(GscopeError):
    """Feature not supported on this kernel."""
    pass


# Error code to exception class mapping
_ERROR_MAP = {
    -1: InvalidArgumentError,
    -2: GscopeError,          # NOMEM
    -3: PermissionError,
    -4: AlreadyExistsError,
    -5: NotFoundError,
    -6: InvalidStateError,
    -7: GscopeError,          # BUSY
    -8: TimeoutError,
    -10: GscopeError,         # SYSCALL
    -20: NamespaceError,
    -21: CgroupError,
    -22: NetworkError,
    -24: RootfsError,
    -26: ProcessError,
    -27: SecurityError,
    -29: SecurityError,
    -40: UnsupportedError,
}


def check_error(code: int, lib=None):
    """Raise appropriate exception if code is not OK (0)."""
    if code == 0:
        return

    message = "unknown error"
    if lib is not None:
        try:
            msg = lib.gscope_strerror()
            if msg:
                message = msg.decode("utf-8", errors="replace")
        except Exception:
            pass

    exc_class = _ERROR_MAP.get(code, GscopeError)
    raise exc_class(code, message)
