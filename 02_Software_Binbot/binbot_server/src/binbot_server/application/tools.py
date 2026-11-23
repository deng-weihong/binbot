"""Application tools exposed to agents and external channel adapters.

Tools deliberately depend on application services instead of aiohttp request
objects.  This keeps the same actions reusable from voice, console, and Feishu.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import Any
from uuid import UUID

from binbot_server.domain.errors import StateConflict

from .device_control import DeviceControlStore
from .ports import MemoryStore
from .reports import ActivityJournal
from .registry import DeviceSessionRegistry
from .tasks import TaskService
from .device_commands import WORKSPACE_COMMANDS

ToolHandler = Callable[["ToolContext", dict[str, Any]], Awaitable[dict[str, Any]]]
ArgumentValidator = Callable[[Mapping[str, Any]], dict[str, Any]]


class ToolError(ValueError):
    """A user-correctable tool invocation error."""


class CommandDispatchError(StateConflict):
    """A command was recorded but could not be delivered to the device."""

    def __init__(self, message: str, command: dict[str, object]):
        super().__init__(message)
        self.command = command


@dataclass(frozen=True, slots=True)
class ToolContext:
    """Invocation metadata shared by channel adapters and agent runtime."""

    device_id: str | None = None
    session_id: str | None = None
    source: str = "unknown"
    approved: bool = False


@dataclass(frozen=True, slots=True)
class ToolSpec:
    name: str
    description: str
    parameters: dict[str, object]
    permission: str
    execute: ToolHandler
    validate: ArgumentValidator | None = None
    timeout_seconds: float = 10.0

    def public_schema(self) -> dict[str, object]:
        return {
            "name": self.name,
            "description": self.description,
            "parameters": dict(self.parameters),
            "permission": self.permission,
        }


class ToolRegistry:
    """Register, describe, validate, and execute agent tools."""

    def __init__(self, journal: ActivityJournal | None = None) -> None:
        self._tools: dict[str, ToolSpec] = {}
        self._journal = journal

    def register(self, spec: ToolSpec) -> None:
        if not spec.name or "." not in spec.name:
            raise ValueError("tool name must use a namespaced form such as device.list")
        if spec.name in self._tools:
            raise ValueError(f"tool already registered: {spec.name}")
        if spec.timeout_seconds <= 0:
            raise ValueError("tool timeout must be positive")
        self._tools[spec.name] = spec

    def get(self, name: str) -> ToolSpec:
        try:
            return self._tools[name]
        except KeyError as exc:
            raise ToolError(f"unknown tool: {name}") from exc

    def describe(self) -> list[dict[str, object]]:
        return [self._tools[name].public_schema() for name in sorted(self._tools)]

    async def execute(
        self,
        name: str,
        context: ToolContext,
        arguments: Mapping[str, Any] | None = None,
        *,
        authorize: Callable[[ToolSpec, ToolContext], Awaitable[bool]] | None = None,
    ) -> dict[str, Any]:
        try:
            result = await self._execute(name, context, arguments, authorize=authorize)
        except Exception as exc:
            if self._journal is not None and not name.startswith("report."):
                await self._journal.record(
                    "tool",
                    context.source,
                    context.device_id,
                    name,
                    "failed",
                    {
                        "arguments": dict(arguments) if isinstance(arguments, Mapping) else {},
                        "error": str(exc),
                    },
                )
            raise
        if self._journal is not None and not name.startswith("report."):
            state = str(result.get("status", ""))
            status = (
                "failed"
                if (
                    "error" in result
                    or state == "failed"
                    or (name == "task.cancel" and result.get("cancelled") is False)
                )
                else (
                    "submitted"
                    if state in {"pending", "queued", "delivered", "running"}
                    else "completed"
                )
            )
            reference_id = result.get("command_id") or result.get("task_id") or ""
            if name == "task.cancel" and result.get("cancelled") is True:
                reference_id = (arguments or {}).get("task_id", "")
            await self._journal.record(
                "tool",
                context.source,
                context.device_id,
                self.get(name).description,
                status,
                {"tool": name, "arguments": dict(arguments or {}), "result": result},
                str(reference_id),
            )
        return result

    async def _execute(
        self,
        name: str,
        context: ToolContext,
        arguments: Mapping[str, Any] | None = None,
        *,
        authorize: Callable[[ToolSpec, ToolContext], Awaitable[bool]] | None = None,
    ) -> dict[str, Any]:
        spec = self.get(name)
        if authorize is not None and not await authorize(spec, context):
            raise ToolError(f"permission denied for tool: {name}")
        raw = arguments or {}
        if not isinstance(raw, Mapping):
            raise ToolError("tool arguments must be an object")
        try:
            validated = spec.validate(raw) if spec.validate else dict(raw)
        except (TypeError, ValueError) as exc:
            raise ToolError(str(exc)) from exc
        try:
            return await asyncio.wait_for(
                spec.execute(context, validated), timeout=spec.timeout_seconds
            )
        except asyncio.TimeoutError as exc:
            raise ToolError(f"tool timed out: {name}") from exc


class DeviceCommandService:
    """Application boundary for sending commands to a connected device."""

    def __init__(self, registry: DeviceSessionRegistry, controls: DeviceControlStore):
        self._registry = registry
        self._controls = controls

    async def list_devices(self) -> list[dict[str, object]]:
        return await self._registry.snapshot()

    async def get_state(self, device_id: str) -> dict[str, object] | None:
        return await self._controls.state(device_id)

    async def command(self, command_id: UUID) -> dict[str, object] | None:
        return await self._controls.command(command_id)

    async def send(
        self,
        device_id: str,
        name: str,
        payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> dict[str, object]:
        record, created = await self._controls.get_or_create_command(
            device_id, name, payload, idempotency_key
        )
        if not created:
            # Legacy media retries mint fresh transfer URLs for the same upload.
            # Enforce exact payload reuse for the new immediate control protocol.
            strict = {"screen.brightness.set", "audio.volume.set"} | (WORKSPACE_COMMANDS - {"display.gif", "modules.image"})
            if (record.name in strict or name in strict) and (
                record.name != name or record.payload != payload
            ):
                raise StateConflict("Idempotency-Key 已用于不同的设备操作")
            return record.snapshot()
        event: dict[str, object] = {
            "type": "device.command",
            "command_id": str(record.command_id),
            "name": name,
            "payload": dict(payload),
        }
        try:
            await self._registry.send_command(device_id, event)
        except StateConflict as exc:
            await self._controls.mark_failed(record.command_id, str(exc))
            snapshot = await self._controls.command(record.command_id)
            assert snapshot is not None
            raise CommandDispatchError(str(snapshot["message"]), snapshot) from exc
        await self._controls.mark_delivered(record.command_id)
        snapshot = await self._controls.command(record.command_id)
        assert snapshot is not None
        return snapshot


class MemoryService:
    """Application boundary for device-scoped long-term memory."""

    def __init__(self, store: MemoryStore):
        self._store = store

    async def remember(self, device_id: str, content: str) -> dict[str, object]:
        item = await self._store.remember(device_id, content)
        return {
            "memory_id": str(item.memory_id),
            "device_id": item.device_id,
            "content": item.content,
            "tags": list(item.tags),
            "created_at": item.created_at,
        }

    async def search(self, device_id: str, query: str) -> dict[str, object]:
        items = await self._store.search(device_id, query)
        return {
            "memories": [
                {
                    "memory_id": str(item.memory_id),
                    "content": item.content,
                    "tags": list(item.tags),
                    "created_at": item.created_at,
                }
                for item in items
            ]
        }

    async def forget(self, device_id: str, memory_id: str) -> dict[str, object]:
        try:
            parsed = UUID(memory_id)
        except ValueError as exc:
            raise ToolError("memory_id must be a UUID") from exc
        return {"deleted": await self._store.forget(device_id, parsed)}


def _require_device(context: ToolContext) -> str:
    if not context.device_id:
        raise ToolError("this tool requires a device context")
    return context.device_id


def _empty(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if arguments:
        raise ValueError("this tool does not accept arguments")
    return {}


def _text(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"text"} or not isinstance(arguments["text"], str):
        raise ValueError("text is required")
    value = arguments["text"].strip()
    if not value:
        raise ValueError("text must be non-empty")
    return {"text": value}


def _index(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"index"}:
        raise ValueError("index is required")
    value = arguments["index"]
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < 8:
        raise ValueError("index must be an integer in 0..7")
    return {"index": value}


def _seek(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"position_seconds"}:
        raise ValueError("position_seconds is required")
    value = arguments["position_seconds"]
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 86_400:
        raise ValueError("position_seconds must be an integer in 0..86400")
    return {"position_seconds": value}


def _select(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"track_index"}:
        raise ValueError("track_index is required")
    value = arguments["track_index"]
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 255:
        raise ValueError("track_index must be an integer in 0..255")
    return {"track_index": value}


def _edit(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"index", "text"}:
        raise ValueError("index and text are required")
    normalized = _index({"index": arguments["index"]})
    text = _text({"text": arguments["text"]})
    normalized.update(text)
    return normalized


def _set_done(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"index", "done"}:
        raise ValueError("index and done are required")
    normalized = _index({"index": arguments["index"]})
    done = arguments["done"]
    if not isinstance(done, bool):
        raise ValueError("done must be a boolean")
    normalized["done"] = done
    return normalized


def _memory_content(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"content"} or not isinstance(arguments["content"], str):
        raise ValueError("content is required")
    value = arguments["content"].strip()
    if not value or len(value) > 2_000:
        raise ValueError("content must be 1..2000 characters")
    return {"content": value}


def _memory_query(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"query"} or not isinstance(arguments["query"], str):
        raise ValueError("query is required")
    value = arguments["query"].strip()
    if not value:
        raise ValueError("query must be non-empty")
    return {"query": value}


def _memory_id(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"memory_id"} or not isinstance(arguments["memory_id"], str):
        raise ValueError("memory_id is required")
    try:
        UUID(arguments["memory_id"])
    except ValueError as exc:
        raise ValueError("memory_id must be a UUID") from exc
    return {"memory_id": arguments["memory_id"]}


def _task_create(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"name", "run_at", "payload"}:
        raise ValueError("name, run_at and payload are required")
    name, run_at, payload = arguments["name"], arguments["run_at"], arguments["payload"]
    if not isinstance(name, str) or not name.strip() or len(name.strip()) > 100:
        raise ValueError("name must be 1..100 characters")
    if not isinstance(run_at, str) or not run_at.strip():
        raise ValueError("run_at must be an ISO-8601 timestamp")
    if not isinstance(payload, dict):
        raise ValueError("payload must be an object")
    return {"name": name.strip(), "run_at": run_at.strip(), "payload": dict(payload)}


def _task_list(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) not in (set(), {"status"}):
        raise ValueError("only status is accepted")
    if "status" not in arguments:
        return {}
    status = arguments["status"]
    if status not in {"pending", "running", "completed", "failed", "cancelled"}:
        raise ValueError("invalid task status")
    return {"status": status}


def _task_id(arguments: Mapping[str, Any]) -> dict[str, Any]:
    if set(arguments) != {"task_id"} or not isinstance(arguments["task_id"], str):
        raise ValueError("task_id is required")
    try:
        UUID(arguments["task_id"])
    except ValueError as exc:
        raise ValueError("task_id must be a UUID") from exc
    return {"task_id": arguments["task_id"]}


def register_device_tools(
    tools: ToolRegistry,
    service: DeviceCommandService,
    memories: MemoryService | None = None,
    tasks: TaskService | None = None,
) -> None:
    """Register the first stable tool surface for the main agent."""

    async def list_devices(context: ToolContext, __: dict[str, Any]) -> dict[str, Any]:
        devices = await service.list_devices()
        if context.source == "web" and context.device_id:
            devices = [item for item in devices if item["id"] == context.device_id]
        return {"devices": devices}

    async def get_state(context: ToolContext, _: dict[str, Any]) -> dict[str, Any]:
        device_id = _require_device(context)
        return {"device_id": device_id, "state": await service.get_state(device_id)}

    async def command(name: str, context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
        device_id = _require_device(context)
        result = await service.send(device_id, name, args)
        if context.source in {'web', 'voice', 'feishu'}:
            deadline = asyncio.get_running_loop().time() + 7
            while result['status'] in {'queued', 'delivered'} and asyncio.get_running_loop().time() < deadline:
                await asyncio.sleep(0.05)
                result = await service.command(UUID(str(result['command_id']))) or result
        return {"command": result}

    def command_handler(name: str) -> ToolHandler:
        async def handler(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await command(name, context, args)

        return handler

    tools.register(
        ToolSpec(
            "device.list",
            "List connected devices.",
            {"type": "object"},
            "device.read",
            list_devices,
            _empty,
        )
    )
    tools.register(
        ToolSpec(
            "device.get_state",
            "Read the current device state.",
            {"type": "object"},
            "device.read",
            get_state,
            _empty,
        )
    )
    for name, description in (
        ("music.play", "Start music playback."),
        ("music.pause", "Pause music playback."),
        ("music.stop", "Stop music playback."),
        ("music.previous", "Go to the previous track."),
        ("music.next", "Skip to the next track."),
    ):
        tools.register(
            ToolSpec(
                name,
                description,
                {"type": "object"},
                "device.control",
                command_handler(name),
                _empty,
            )
        )
    tools.register(
        ToolSpec(
            "music.seek",
            "Seek within the current track.",
            {
                "type": "object",
                "properties": {"position_seconds": {"type": "integer"}},
                "required": ["position_seconds"],
            },
            "device.control",
            command_handler("music.seek"),
            _seek,
        )
    )
    tools.register(
        ToolSpec(
            "music.select",
            "Select a track by index.",
            {
                "type": "object",
                "properties": {"track_index": {"type": "integer"}},
                "required": ["track_index"],
            },
            "device.control",
            command_handler("music.select"),
            _select,
        )
    )
    tools.register(
        ToolSpec(
            "todo.add",
            "Add a todo item on the device.",
            {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]},
            "device.control",
            lambda context, args: command("todo.add", context, args),
            _text,
        )
    )
    tools.register(
        ToolSpec(
            "todo.edit",
            "Edit a todo item on the device.",
            {
                "type": "object",
                "properties": {"index": {"type": "integer"}, "text": {"type": "string"}},
                "required": ["index", "text"],
            },
            "device.control",
            command_handler("todo.edit"),
            _edit,
        )
    )
    tools.register(
        ToolSpec(
            "todo.set_done",
            "Set a todo item's completion state.",
            {
                "type": "object",
                "properties": {"index": {"type": "integer"}, "done": {"type": "boolean"}},
                "required": ["index", "done"],
            },
            "device.control",
            command_handler("todo.set_done"),
            _set_done,
        )
    )
    tools.register(
        ToolSpec(
            "todo.delete",
            "Delete a todo item from the device.",
            {"type": "object", "properties": {"index": {"type": "integer"}}, "required": ["index"]},
            "device.control",
            command_handler("todo.delete"),
            _index,
        )
    )
    tools.register(
        ToolSpec(
            "voice.wake",
            "Wake the device voice interface.",
            {"type": "object"},
            "device.control",
            command_handler("voice.wake"),
            _empty,
        )
    )
    if memories is not None:

        async def memory_context(context: ToolContext) -> str:
            return _require_device(context)

        async def remember(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await memories.remember(await memory_context(context), args["content"])

        async def search(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await memories.search(await memory_context(context), args["query"])

        async def forget(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await memories.forget(await memory_context(context), args["memory_id"])

        tools.register(
            ToolSpec(
                "memory.remember",
                "Save a fact for future conversations.",
                {
                    "type": "object",
                    "properties": {"content": {"type": "string"}},
                    "required": ["content"],
                },
                "memory.write",
                remember,
                _memory_content,
            )
        )
        tools.register(
            ToolSpec(
                "memory.search",
                "Search saved facts.",
                {
                    "type": "object",
                    "properties": {"query": {"type": "string"}},
                    "required": ["query"],
                },
                "memory.read",
                search,
                _memory_query,
            )
        )
        tools.register(
            ToolSpec(
                "memory.forget",
                "Delete a saved fact.",
                {
                    "type": "object",
                    "properties": {"memory_id": {"type": "string"}},
                    "required": ["memory_id"],
                },
                "memory.write",
                forget,
                _memory_id,
            )
        )
    if tasks is not None:

        async def task_context(context: ToolContext) -> str:
            return _require_device(context)

        async def create_task(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await tasks.create(
                await task_context(context), args["name"], args["payload"], args["run_at"]
            )

        async def list_tasks(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            statuses = (args["status"],) if "status" in args else ()
            return await tasks.list(await task_context(context), statuses=statuses)

        async def cancel_task(context: ToolContext, args: dict[str, Any]) -> dict[str, Any]:
            return await tasks.cancel(await task_context(context), args["task_id"])

        tools.register(
            ToolSpec(
                "task.create",
                "Schedule a one-time task for this device.",
                {
                    "type": "object",
                    "properties": {
                        "name": {"type": "string"},
                        "run_at": {"type": "string", "format": "date-time"},
                        "payload": {"type": "object"},
                    },
                    "required": ["name", "run_at", "payload"],
                },
                "task.write",
                create_task,
                _task_create,
            )
        )
        tools.register(
            ToolSpec(
                "task.list",
                "List scheduled tasks for this device.",
                {"type": "object", "properties": {"status": {"type": "string"}}},
                "task.read",
                list_tasks,
                _task_list,
            )
        )
        tools.register(
            ToolSpec(
                "task.cancel",
                "Cancel a pending task for this device.",
                {
                    "type": "object",
                    "properties": {"task_id": {"type": "string", "format": "uuid"}},
                    "required": ["task_id"],
                },
                "task.write",
                cancel_task,
                _task_id,
            )
        )
