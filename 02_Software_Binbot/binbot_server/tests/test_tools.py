import asyncio
from typing import cast

import pytest

from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.tools import (
    CommandDispatchError,
    DeviceCommandService,
    ToolContext,
    ToolError,
    ToolRegistry,
    ToolSpec,
    register_device_tools,
)
from binbot_server.domain.errors import StateConflict


def test_registry_validates_and_executes_with_metadata() -> None:
    async def scenario() -> None:
        registry = ToolRegistry()

        async def execute(context: ToolContext, args: dict[str, object]) -> dict[str, object]:
            return {"device": context.device_id, **args}

        registry.register(
            ToolSpec(
                "test.echo", "Echo", {"type": "object"}, "test.read", execute,
                lambda value: (
                    {"text": str(value["text"])}
                    if "text" in value
                    else (_ for _ in ()).throw(ValueError("text required"))
                ),
            )
        )
        result = await registry.execute(
            "test.echo", ToolContext(device_id="dev-1"), {"text": "hi"}
        )
        assert result == {"device": "dev-1", "text": "hi"}
        assert registry.describe()[0]["permission"] == "test.read"
        with pytest.raises(ToolError, match="text required"):
            await registry.execute("test.echo", ToolContext(), {})

    asyncio.run(scenario())


def test_registry_authorization_and_unknown_tool() -> None:
    async def scenario() -> None:
        registry = ToolRegistry()

        async def execute(_: ToolContext, __: dict[str, object]) -> dict[str, object]:
            return {}

        registry.register(ToolSpec("test.noop", "Noop", {"type": "object"}, "test.run", execute))
        with pytest.raises(ToolError, match="permission denied"):
            await registry.execute(
                "test.noop", ToolContext(),
                authorize=lambda *_: asyncio.sleep(0, result=False),
            )
        with pytest.raises(ToolError, match="unknown tool"):
            await registry.execute("test.missing", ToolContext())

    asyncio.run(scenario())


def test_device_command_service_preserves_failed_command_snapshot() -> None:
    async def scenario() -> None:
        class OfflineRegistry:
            async def send_command(
                self, _: str, __: dict[str, object]
            ) -> None:
                raise StateConflict("device is not connected")

        service = DeviceCommandService(
            cast(DeviceSessionRegistry, OfflineRegistry()), DeviceControlStore()
        )
        with pytest.raises(CommandDispatchError) as raised:
            await service.send("offline", "music.play", {})
        assert raised.value.command["status"] == "failed"
        assert raised.value.command["message"] == "device is not connected"

    asyncio.run(scenario())


def test_device_command_service_does_not_redeliver_idempotent_retry() -> None:
    async def scenario() -> None:
        class CountingRegistry:
            sends = 0

            async def send_command(
                self, _: str, __: dict[str, object]
            ) -> None:
                self.sends += 1

        registry = CountingRegistry()
        service = DeviceCommandService(
            cast(DeviceSessionRegistry, registry), DeviceControlStore()
        )
        first = await service.send("device-a", "music.play", {}, "request-1")
        second = await service.send("device-a", "music.play", {}, "request-1")
        assert first["command_id"] == second["command_id"]
        assert registry.sends == 1

    asyncio.run(scenario())


def test_device_tools_expose_existing_device_command_surface() -> None:
    tools = ToolRegistry()
    register_device_tools(
        tools, DeviceCommandService(DeviceSessionRegistry(), DeviceControlStore())
    )
    assert [item["name"] for item in tools.describe()] == [
        "device.get_state",
        "device.list",
        "music.next",
        "music.pause",
        "music.play",
        "music.previous",
        "music.seek",
        "music.select",
        "music.stop",
        "todo.add",
        "todo.delete",
        "todo.edit",
        "todo.set_done",
        "voice.wake",
    ]
