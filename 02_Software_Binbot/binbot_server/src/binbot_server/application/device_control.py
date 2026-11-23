from __future__ import annotations

import asyncio
from collections import deque
from copy import deepcopy
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any
from uuid import UUID, uuid4

from .device_commands import WORKSPACE_COMMANDS

from binbot_server.transport.protocol import (
    DeviceCommandResultMessage,
    DeviceStateMessage,
)


def _now() -> str:
    return datetime.now(UTC).isoformat()


def offline_device_snapshot(
    device_id: str,
    metadata: dict[str, object] | None = None,
) -> dict[str, object]:
    snapshot: dict[str, object] = {
        "id": device_id,
        "remote_ip": "",
        "connected_at": "",
        "last_seen_at": "",
        "capabilities": [],
        "video_available": False,
    }
    snapshot.update(deepcopy(metadata or {}))
    snapshot.update({"id": device_id, "online": False, "phase": "offline", "session_id": ""})
    snapshot.update(control_online=False, control_ready=False, voice_online=False)
    return snapshot


@dataclass(slots=True)
class CommandRecord:
    command_id: UUID
    device_id: str
    name: str
    payload: dict[str, object]
    status: str
    created_at: str
    updated_at: str
    message: str = ""
    idempotency_key: str | None = None

    def snapshot(self) -> dict[str, object]:
        return {
            "command_id": str(self.command_id),
            "device_id": self.device_id,
            "name": self.name,
            "payload": dict(self.payload),
            "status": self.status,
            "message": self.message,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "idempotency_key": self.idempotency_key,
        }


class DeviceControlStore:
    """Process-local device state and bounded command history for the admin API."""

    def __init__(self, max_commands: int = 500):
        self._max_commands = max_commands
        self._states: dict[str, dict[str, Any]] = {}
        self._control_states: dict[str, dict[str, Any]] = {}
        self._commands: dict[UUID, CommandRecord] = {}
        self._command_order: deque[UUID] = deque()
        self._idempotency: dict[tuple[str, str], UUID] = {}
        self._devices: dict[str, dict[str, object]] = {}
        self._lock = asyncio.Lock()

    async def remember_device(self, snapshot: dict[str, object]) -> None:
        async with self._lock:
            self._devices[str(snapshot["id"])] = offline_device_snapshot(
                str(snapshot["id"]), snapshot
            )

    async def known_devices(self) -> list[dict[str, object]]:
        async with self._lock:
            identifiers = set(self._devices) | set(self._states) | set(self._control_states)
            identifiers.update(record.device_id for record in self._commands.values())
            result = []
            for device_id in sorted(identifiers):
                snapshot = offline_device_snapshot(device_id, self._devices.get(device_id))
                if device_id in self._states:
                    snapshot["last_seen_at"] = self._states[device_id]["updated_at"]
                result.append(snapshot)
            return result

    async def update_state(self, device_id: str, state: DeviceStateMessage) -> None:
        snapshot = state.model_dump(exclude={"type"})
        snapshot["updated_at"] = _now()
        async with self._lock:
            previous = self._states.get(device_id)
            if previous is not None and int(previous.get("revision", -1)) > state.revision:
                return
            self._states[device_id] = snapshot

    async def device_connected(self, device_id: str) -> None:
        """Allow a restarted device to begin its revision counter again."""
        async with self._lock:
            self._states.pop(device_id, None)

    async def state(self, device_id: str) -> dict[str, object] | None:
        async with self._lock:
            state = self._states.get(device_id)
            control = self._control_states.get(device_id)
            if control is not None:
                return {
                    **deepcopy(state or {}),
                    "control": deepcopy(control),
                    "updated_at": control["updated_at"],
                }
            return deepcopy(state) if state is not None else None

    async def update_control_state(self, device_id: str, state: dict[str, Any]) -> None:
        async with self._lock:
            previous = self._control_states.get(device_id)
            if (
                previous
                and previous["boot_id"] == state["boot_id"]
                and previous["revision"] > state["revision"]
            ):
                return
            self._control_states[device_id] = {**deepcopy(state), "updated_at": _now()}

    async def create_command(
        self,
        device_id: str,
        name: str,
        payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> CommandRecord:
        record, _ = await self.get_or_create_command(device_id, name, payload, idempotency_key)
        return record

    async def get_or_create_command(
        self,
        device_id: str,
        name: str,
        payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> tuple[CommandRecord, bool]:
        async with self._lock:
            if idempotency_key is not None:
                existing_id = self._idempotency.get((device_id, idempotency_key))
                if existing_id is not None:
                    existing = self._commands.get(existing_id)
                    if existing is not None:
                        return existing, False
            timestamp = _now()
            record = CommandRecord(
                command_id=uuid4(),
                device_id=device_id,
                name=name,
                payload=dict(payload),
                status="queued",
                created_at=timestamp,
                updated_at=timestamp,
                idempotency_key=idempotency_key,
            )
            self._commands[record.command_id] = record
            self._command_order.append(record.command_id)
            if idempotency_key is not None:
                self._idempotency[(device_id, idempotency_key)] = record.command_id
            while len(self._command_order) > self._max_commands:
                expired = self._command_order.popleft()
                removed = self._commands.pop(expired, None)
                if removed is not None and removed.idempotency_key is not None:
                    self._idempotency.pop((removed.device_id, removed.idempotency_key), None)
            return record, True

    async def mark_delivered(self, command_id: UUID) -> None:
        async with self._lock:
            record = self._commands.get(command_id)
            if record is not None and record.status == "queued":
                record.status = "delivered"
                record.updated_at = _now()

    async def fail_unconfirmed_control_commands(self) -> None:
        async with self._lock:
            for record in self._commands.values():
                if record.name in ({
                    "screen.brightness.set",
                    "audio.volume.set",
                } | WORKSPACE_COMMANDS) and record.status in {"queued", "delivered"}:
                    record.status = "failed"
                    record.message = "控制服务已重启，执行结果未确认"
                    record.updated_at = _now()

    async def mark_failed(self, command_id: UUID, message: str) -> None:
        async with self._lock:
            record = self._commands.get(command_id)
            if record is not None:
                record.status = "failed"
                record.message = message[:256]
                record.updated_at = _now()

    async def complete(self, device_id: str, result: DeviceCommandResultMessage) -> None:
        async with self._lock:
            record = self._commands.get(result.command_id)
            if record is None or record.device_id != device_id:
                return
            record.status = "completed" if result.success else "failed"
            record.message = result.message
            record.updated_at = _now()

    async def command(self, command_id: UUID) -> dict[str, object] | None:
        async with self._lock:
            record = self._commands.get(command_id)
            return record.snapshot() if record is not None else None

    async def recent_commands(self, device_id: str, limit: int = 20) -> list[dict[str, object]]:
        async with self._lock:
            records = (
                self._commands[command_id]
                for command_id in reversed(self._command_order)
                if command_id in self._commands
            )
            return [record.snapshot() for record in records if record.device_id == device_id][
                :limit
            ]

    async def close(self) -> None:
        """Match persistent stores so application cleanup is uniform."""
