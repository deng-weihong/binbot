from __future__ import annotations

import asyncio
import time
from collections.abc import Callable, Awaitable
from typing import Any
from dataclasses import dataclass
from uuid import uuid4

from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import CameraConfig
from binbot_server.domain.errors import StateConflict


@dataclass(slots=True)
class Viewer:
    device_id: str
    expires_at: float


@dataclass(slots=True)
class Demand:
    next_renewal: float = 0.0
    stop_at: float | None = None


class CameraSessions:
    """Shares one publisher between viewers, with leases on both sides of the network."""

    def __init__(
        self,
        config: CameraConfig,
        registry: DeviceSessionRegistry,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.config = config
        self.registry = registry
        self.clock = clock
        self.viewers: dict[str, Viewer] = {}
        self.demands: dict[str, Demand] = {}
        self._lock = asyncio.Lock()
        self.publisher: (
            Callable[[str, str, dict[str, Any], str], Awaitable[dict[str, Any]]] | None
        ) = None

    async def _check_device(self, device_id: str) -> None:
        devices = await self.registry.snapshot()
        device = next((item for item in devices if item["id"] == device_id), None)
        if device is None:
            raise StateConflict("设备离线")
        capabilities = device.get("capabilities")
        if not isinstance(capabilities, list) or not {
            "camera_publish_v1",
            "camera_publish_v2",
        }.intersection(capabilities):
            raise StateConflict("设备尚未配置云端摄像头")

    async def _publish(self, device_id: str, seconds: int) -> None:
        try:
            devices = await self.registry.snapshot()
            current = next((item for item in devices if item["id"] == device_id), {})
            capabilities = current.get("capabilities")
            if (
                self.publisher
                and isinstance(capabilities, list)
                and "camera_publish_v2" in capabilities
            ):
                await self.publisher(
                    device_id, "camera.publish", {"lease_seconds": seconds}, "camera:" + uuid4().hex
                )
                return
            await asyncio.wait_for(
                self.registry.send_event(
                    device_id,
                    {
                        "type": "camera.publish",
                        "lease_seconds": seconds,
                    },
                ),
                timeout=5,
            )
        except (ConnectionError, TimeoutError):
            raise StateConflict("设备连接暂不可用") from None

    async def open(self, device_id: str) -> str:
        await self._check_device(device_id)
        async with self._lock:
            now = self.clock()
            self._expire(now)
            if (
                len(self.viewers) >= 256
                or sum(viewer.device_id == device_id for viewer in self.viewers.values()) >= 16
            ):
                raise StateConflict("观看连接过多")
            demand = self.demands.get(device_id) or Demand()
            if now >= demand.next_renewal:
                await self._publish(device_id, 45)
                demand.next_renewal = now + 10
            demand.stop_at = None
            self.demands[device_id] = demand
            session_id = str(uuid4())
            self.viewers[session_id] = Viewer(device_id, now + self.config.viewer_ttl_seconds)
            return session_id

    async def refresh(self, device_id: str, session_id: str) -> None:
        async with self._lock:
            viewer = self.viewers.get(session_id)
            if viewer is None or viewer.device_id != device_id or viewer.expires_at <= self.clock():
                raise KeyError("观看会话已过期")
            await self._check_device(device_id)
            viewer.expires_at = self.clock() + self.config.viewer_ttl_seconds

    async def release(self, device_id: str, session_id: str) -> None:
        async with self._lock:
            viewer = self.viewers.get(session_id)
            if viewer is not None and viewer.device_id == device_id:
                del self.viewers[session_id]
                self._schedule_stop(device_id, self.clock())

    def _schedule_stop(self, device_id: str, now: float) -> None:
        demand = self.demands.get(device_id)
        if (
            demand is not None
            and demand.stop_at is None
            and not any(viewer.device_id == device_id for viewer in self.viewers.values())
        ):
            demand.stop_at = now + self.config.stop_delay_seconds

    def _expire(self, now: float) -> None:
        for session_id, viewer in list(self.viewers.items()):
            if viewer.expires_at <= now:
                del self.viewers[session_id]
                self._schedule_stop(viewer.device_id, now)

    async def tick(self) -> None:
        async with self._lock:
            now = self.clock()
            self._expire(now)
            for device_id, demand in list(self.demands.items()):
                if demand.stop_at is not None and now >= demand.stop_at:
                    try:
                        await self._publish(device_id, 0)
                    except StateConflict:
                        pass  # The device lease also expires if its connection disappears.
                    del self.demands[device_id]
                elif now >= demand.next_renewal:
                    try:
                        await self._publish(device_id, 45)
                    except StateConflict:
                        pass
                    demand.next_renewal = now + 10

    async def close(self) -> None:
        async with self._lock:
            for device_id in self.demands:
                try:
                    await self._publish(device_id, 0)
                except StateConflict:
                    pass
            self.viewers.clear()
            self.demands.clear()
