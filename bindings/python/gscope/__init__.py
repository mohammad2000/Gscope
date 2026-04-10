"""
gscope — Python bindings for libgscope

Lightweight Linux scope isolation library.

Usage:
    from gscope import Context

    with Context() as ctx:
        scope = ctx.create(1, cpu_cores=2.0, memory_mb=1024)
        scope.start()
        scope.exec("/bin/bash", pty=True)
        scope.stop()
        scope.delete()

Copyright 2026 Gritiva
SPDX-License-Identifier: Apache-2.0
"""

from .scope import Context, Scope, ExecResult
from .scope import Isolation, NetMode, Privilege, State
from .errors import (
    GscopeError,
    InvalidArgumentError,
    PermissionError,
    NotFoundError,
    AlreadyExistsError,
    InvalidStateError,
    NamespaceError,
    CgroupError,
    NetworkError,
    RootfsError,
    ProcessError,
    SecurityError,
    TimeoutError,
    UnsupportedError,
)

__version__ = Context.version() if False else "0.1.0"
__all__ = [
    "Context", "Scope", "ExecResult",
    "Isolation", "NetMode", "Privilege", "State",
    "GscopeError", "InvalidArgumentError", "PermissionError",
    "NotFoundError", "AlreadyExistsError", "InvalidStateError",
    "NamespaceError", "CgroupError", "NetworkError",
    "RootfsError", "ProcessError", "SecurityError",
    "TimeoutError", "UnsupportedError",
]
