"""
gscope.adapter — Drop-in adapter for GritivaCore's ScopeManager

This adapter matches the existing async ScopeManager interface used by
the GritivaCore agent, so it can replace the Python scope system with
zero changes to the rest of the codebase.

Usage in GritivaCore agent:
    # Replace:
    #   from scope.manager import ScopeManager
    # With:
    from gscope.adapter import GscopeScopeManager as ScopeManager

Copyright 2026 Gritiva
SPDX-License-Identifier: Apache-2.0
"""

import asyncio
import logging
from typing import Dict, Any, Optional, List

from .scope import Context, Scope, Isolation, NetMode, Privilege

logger = logging.getLogger("gscope.adapter")


class GscopeScopeManager:
    """
    Async adapter matching GritivaCore's ScopeManager interface.

    The C library is synchronous, so we wrap calls in
    asyncio.to_thread() to avoid blocking the event loop.
    """

    def __init__(self):
        self._ctx: Optional[Context] = None
        self._scopes: Dict[int, Scope] = {}
        self._initialized = False

    async def initialize(self):
        """Initialize the gscope library."""
        if self._initialized:
            return

        self._ctx = await asyncio.to_thread(Context, verbose=False, restore=True)
        self._initialized = True
        logger.info("GscopeScopeManager initialized (libgscope %s)", Context.version())

    async def shutdown(self):
        """Shutdown and cleanup."""
        if self._ctx:
            await asyncio.to_thread(self._ctx.destroy)
            self._ctx = None
        self._scopes.clear()
        self._initialized = False

    async def create_scope(self, scope_id: int, config: dict) -> Dict[str, Any]:
        """
        Create a scope. Matches GritivaCore orchestrator.create_scope().

        Args:
            scope_id: Scope numeric ID
            config: Dict with keys matching GritivaCore's ScopeConfigModel

        Returns:
            Dict with 'success', 'user', 'rootfs', 'network' info
        """
        if not self._ctx:
            await self.initialize()

        try:
            # Map GritivaCore config to gscope config
            cpu = config.get("cpu_cores", 1.0)
            mem_mb = config.get("memory_mb", 512)
            if "memory_bytes" in config:
                mem_mb = config["memory_bytes"] // (1024 * 1024)

            max_pids = config.get("max_pids", 1024)
            template = config.get("template_path")
            username = config.get("username", "root")
            hostname = config.get("hostname")
            ip = config.get("mesh_ip") or config.get("ip_address")

            # Map isolation level
            iso_map = {
                "minimal": Isolation.MINIMAL,
                "standard": Isolation.STANDARD,
                "high": Isolation.HIGH,
                "maximum": Isolation.MAXIMUM,
            }
            isolation = iso_map.get(
                str(config.get("isolation", "standard")).lower(),
                Isolation.STANDARD,
            )

            # Map privilege
            priv_map = {
                "restricted": Privilege.RESTRICTED,
                "standard": Privilege.STANDARD,
                "elevated": Privilege.ELEVATED,
                "root": Privilege.ROOT,
            }
            privilege = priv_map.get(
                str(config.get("privilege", "standard")).lower(),
                Privilege.STANDARD,
            )

            scope = await asyncio.to_thread(
                self._ctx.create,
                scope_id,
                cpu_cores=cpu,
                memory_mb=mem_mb,
                max_pids=max_pids,
                template_path=template,
                username=username,
                hostname=hostname,
                ip=ip,
                isolation=isolation,
                privilege=privilege,
            )

            self._scopes[scope_id] = scope
            status = await asyncio.to_thread(scope.status)

            return {
                "success": True,
                "user": {
                    "username": username,
                    "uid": config.get("uid", 0),
                    "gid": config.get("gid", 0),
                },
                "rootfs": {
                    "rootfs_path": status.get("rootfs", ""),
                },
                "network": {
                    "ip_address": status.get("ip", ""),
                    "scope_ip": status.get("ip", ""),
                },
            }

        except Exception as e:
            logger.error("create_scope %d failed: %s", scope_id, e)
            return {"success": False, "error": str(e)}

    async def start_scope(self, scope_id: int, command: Optional[str] = None) -> Dict[str, Any]:
        """Start a scope."""
        scope = self._scopes.get(scope_id)
        if not scope and self._ctx:
            scope = await asyncio.to_thread(self._ctx.get, scope_id)

        if not scope:
            return {"success": False, "error": f"Scope {scope_id} not found"}

        try:
            await asyncio.to_thread(scope.start, command)
            status = await asyncio.to_thread(scope.status)
            return {"success": True, "pid": status.get("pid")}
        except Exception as e:
            return {"success": False, "error": str(e)}

    async def stop_scope(self, scope_id: int, timeout: int = 10) -> Dict[str, Any]:
        """Stop a scope."""
        scope = self._scopes.get(scope_id)
        if not scope:
            return {"success": False, "error": f"Scope {scope_id} not found"}

        try:
            await asyncio.to_thread(scope.stop, timeout)
            return {"success": True}
        except Exception as e:
            return {"success": False, "error": str(e)}

    async def delete_scope(self, scope_id: int, force: bool = False) -> Dict[str, Any]:
        """Delete a scope."""
        scope = self._scopes.pop(scope_id, None)
        if not scope and self._ctx:
            scope = await asyncio.to_thread(self._ctx.get, scope_id)

        if not scope:
            return {"success": False, "error": f"Scope {scope_id} not found"}

        try:
            await asyncio.to_thread(scope.delete, force)
            return {"success": True}
        except Exception as e:
            return {"success": False, "error": str(e)}

    async def list_scopes(self) -> List[int]:
        """List all scope IDs."""
        if not self._ctx:
            return []
        return await asyncio.to_thread(self._ctx.list)

    async def get_scope_status(self, scope_id: int) -> Optional[Dict[str, Any]]:
        """Get scope status."""
        scope = self._scopes.get(scope_id)
        if not scope and self._ctx:
            scope = await asyncio.to_thread(self._ctx.get, scope_id)
        if not scope:
            return None
        return await asyncio.to_thread(scope.status)

    async def get_scope_metrics(self, scope_id: int) -> Optional[Dict[str, Any]]:
        """Get scope metrics."""
        scope = self._scopes.get(scope_id)
        if not scope:
            return None
        return await asyncio.to_thread(scope.metrics)
