"""AI operations scoped to the current workspace with confirmed results."""

from __future__ import annotations
from uuid import uuid4
from typing import Any
from .workspace import WorkspaceService

from .tools import ToolRegistry, ToolSpec, ToolError, ToolContext


def register_workspace_tools(tools: ToolRegistry, workspace: WorkspaceService) -> None:
    operations = [
        "screen.brightness.set",
        "audio.volume.set",
        "watchface.set",
        "gestures.set",
        "microphone.set",
        "noise.set",
        "focus.set",
        "module.motor.set",
        "module.light.set",
        "module.pen.set",
        "module.rotation.set",
        "module.display.set",
        "device.wake",
    ]

    async def read(context: ToolContext, arguments: dict[str, Any]) -> dict[str, Any]:
        if not context.device_id:
            raise ToolError("device context required")
        snapshot = await workspace.snapshot(context.device_id)
        books = snapshot["data"]["wordbooks"]
        snapshot["data"]["wordbooks"] = {
            "activeBookId": books["activeBookId"],
            "books": [
                {
                    "id": book["id"],
                    "name": book["name"],
                    "words": len(book["words"]),
                    "remembered": sum(word["remembered"] for word in book["words"]),
                }
                for book in books["books"]
            ],
        }
        return snapshot

    async def apply(context: ToolContext, arguments: dict[str, Any]) -> dict[str, Any]:
        if (
            not context.device_id
            or arguments.get("name") not in operations
            or not isinstance(arguments.get("payload"), dict)
        ):
            raise ToolError("unsupported operation")
        return {
            "command": await workspace.apply(
                context.device_id,
                arguments["name"],
                arguments["payload"],
                "agent:" + uuid4().hex,
            )
        }

    tools.register(
        ToolSpec(
            "workspace.read",
            "Read this device settings, todos, calendar and wordbook progress.",
            {"type": "object", "properties": {}, "additionalProperties": False},
            "device.read",
            read,
        )
    )
    tools.register(
        ToolSpec(
            "device.apply",
            (
                "Apply a setting and wait for its device receipt. "
                "noise.set: playing(bool), volume(0..100), timerMinutes(0..180), "
                "levels(9 integers: rain/waves/fire/forest/cafe/wind/storm/stream/typing). "
                "focus.set: action(start/pause/reset/configure), optional minutes(5..120), "
                "mode(focus/break). microphone.set: enabled(bool). "
                "screen.brightness.set: brightness(1..100). audio.volume.set: volume(0..100). "
                "watchface.set and gestures.set: use the complete settings from workspace.read. "
                "Module settings require a connected component."
            ),
            {
                "type": "object",
                "properties": {
                    "name": {"type": "string", "enum": operations},
                    "payload": {"type": "object"},
                },
                "required": ["name", "payload"],
                "additionalProperties": False,
            },
            "device.control",
            apply,
            timeout_seconds=22,
        )
    )
