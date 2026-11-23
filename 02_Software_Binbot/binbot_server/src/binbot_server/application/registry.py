from __future__ import annotations

import asyncio
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime

from binbot_server.domain.errors import StateConflict

from .session_actor import SessionActor
from .device_commands import WORKSPACE_COMMANDS


@dataclass(frozen=True, slots=True)
class DeviceSession:
    actor: SessionActor
    remote_ip: str
    camera_url: str | None
    capabilities: frozenset[str]
    connected_at: str


@dataclass(slots=True)
class ControlSession:
    connection_id: str
    send: Callable[[dict[str, object]], Awaitable[None]]
    connected_at: str
    last_seen: float
    capabilities: frozenset[str] = frozenset()
    ready: bool = False
    boot_id: str = ""


class DeviceSessionRegistry:
    """Owns the one-device/one-connection invariant."""

    def __init__(self) -> None:
        self._sessions: dict[str, DeviceSession] = {}
        self._controls: dict[str, ControlSession] = {}
        self._workspace_devices: set[str] = set()
        self._lock = asyncio.Lock()

    async def register(
        self,
        device_id: str,
        actor: SessionActor,
        *,
        remote_ip: str = "",
        camera_url: str | None = None,
        capabilities: set[str] | frozenset[str] = frozenset(),
    ) -> None:
        async with self._lock:
            if device_id in self._sessions:
                raise StateConflict("device already has an active connection")
            self._sessions[device_id] = DeviceSession(
                actor=actor,
                remote_ip=remote_ip,
                camera_url=camera_url,
                capabilities=frozenset(capabilities),
                connected_at=datetime.now(UTC).isoformat(),
            )

    async def unregister(self, device_id: str, actor: SessionActor) -> None:
        async with self._lock:
            registration = self._sessions.get(device_id)
            if registration is not None and registration.actor is actor:
                del self._sessions[device_id]

    async def count(self) -> int:
        async with self._lock:
            return len(self._sessions.keys() | self._controls.keys())

    async def register_control(
        self,
        device_id: str,
        connection_id: str,
        send: Callable[[dict[str, object]], Awaitable[None]],
    ) -> None:
        async with self._lock:
            self._controls[device_id] = ControlSession(
                connection_id, send, datetime.now(UTC).isoformat(), time.monotonic()
            )

    async def control_seen(
        self,
        device_id: str,
        connection_id: str,
        *,
        ready: bool,
        boot_id: str,
        capabilities: Sequence[str],
    ) -> bool:
        async with self._lock:
            row = self._controls.get(device_id)
            if row is None or row.connection_id != connection_id:
                return False
            row.last_seen = time.monotonic()
            row.ready, row.boot_id = ready, boot_id
            row.capabilities = frozenset(capabilities)
            if "workspace_v1" in row.capabilities:
                self._workspace_devices.add(device_id)
            else:
                self._workspace_devices.discard(device_id)
            return True

    async def unregister_control(self, device_id: str, connection_id: str) -> None:
        async with self._lock:
            row = self._controls.get(device_id)
            if row is not None and row.connection_id == connection_id:
                del self._controls[device_id]

    async def snapshot(self) -> list[dict[str, object]]:
        """Return a point-in-time view for the local operations console."""
        async with self._lock:
            devices: dict[str, dict[str, object]] = {
                device_id: {
                    "id": device_id,
                    "online": True,
                    "session_id": str(registration.actor.session.session_id),
                    "phase": registration.actor.session.phase.value,
                    "remote_ip": registration.remote_ip,
                    "connected_at": registration.connected_at,
                    "capabilities": sorted(registration.capabilities),
                    "video_available": registration.camera_url is not None,
                    "voice_online": True,
                    "control_online": False,
                    "control_ready": False,
                }
                for device_id, registration in self._sessions.items()
            }
            for device_id, control in self._controls.items():
                row = devices.setdefault(
                    device_id,
                    {
                        "id": device_id,
                        "online": True,
                        "session_id": "",
                        "phase": "idle",
                        "remote_ip": "",
                        "connected_at": control.connected_at,
                        "capabilities": [],
                        "video_available": False,
                        "voice_online": False,
                    },
                )
                existing = row["capabilities"]
                assert isinstance(existing, list)
                row.update(
                    control_online=True,
                    control_ready=control.ready,
                    capabilities=sorted(set(existing) | control.capabilities),
                )
            return list(devices.values())

    async def send_command(self, device_id: str, event: dict[str, object]) -> None:
        async with self._lock:
            registration = self._sessions.get(device_id)
            control = self._controls.get(device_id)
        name = str(event.get("name", ""))
        control_capabilities = {
            "screen.brightness.set": "brightness_v1",
            "audio.volume.set": "volume_v1",
        }
        if name in WORKSPACE_COMMANDS and control and "workspace_v1" in control.capabilities:
            control_capabilities[name] = "workspace_v1"
        if name in control_capabilities:
            if (
                control is None
                or not control.ready
                or control_capabilities[name] not in control.capabilities
            ):
                raise StateConflict("device control is offline or capability is unavailable")
            await control.send(event)
            return
        if registration is None:
            raise StateConflict("device is not connected")
        capability = (
            "component_ota_v1"
            if str(event.get("name", "")).startswith("component.")
            else "remote_control_v1"
        )
        if capability not in registration.capabilities:
            raise StateConflict("device does not support remote control")
        await registration.actor.send_device_command(event)

    async def workspace_enabled(self, device_id: str) -> bool:
        async with self._lock:
            control = self._controls.get(device_id)
            return device_id in self._workspace_devices or bool(
                control and "workspace_v1" in control.capabilities
            )

    async def send_event(self, device_id: str, event: dict[str, object]) -> None:
        async with self._lock:
            registration = self._sessions.get(device_id)
            control = self._controls.get(device_id)
            workspace = device_id in self._workspace_devices
        if event.get("type") == "device.calendar" and (
            workspace or (control and "workspace_v1" in control.capabilities)
        ):
            # The workspace owns the merged Feishu/local calendar for this App.
            return
        if registration is None:
            raise StateConflict("device is not connected")
        await registration.actor.send_device_event(event)

    async def camera_url(self, device_id: str) -> str | None:
        async with self._lock:
            registration = self._sessions.get(device_id)
            return registration.camera_url if registration is not None else None

    async def close_all(self) -> None:
        async with self._lock:
            actors = [registration.actor for registration in self._sessions.values()]
            self._sessions.clear()
            self._controls.clear()
            self._workspace_devices.clear()
        await asyncio.gather(*(actor.close() for actor in actors), return_exceptions=True)
