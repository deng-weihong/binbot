import asyncio
from uuid import uuid4

from binbot_server.application.device_control import DeviceControlStore
from binbot_server.transport.protocol import (
    DeviceCommandResultMessage,
    DeviceStateMessage,
)


def test_device_control_tracks_state_revision_and_command_result() -> None:
    async def scenario() -> None:
        store = DeviceControlStore(max_commands=2)
        await store.update_state(
            "device-a",
            DeviceStateMessage(type="device.state", revision=2),
        )
        await store.update_state(
            "device-a",
            DeviceStateMessage(type="device.state", revision=1),
        )
        state = await store.state("device-a")
        assert state is not None
        assert state["revision"] == 2
        await store.remember_device({
            "id": "device-a", "online": True, "phase": "idle",
            "capabilities": ["remote_control_v1"],
        })
        known = await store.known_devices()
        assert known[0]["online"] is False
        assert known[0]["capabilities"] == ["remote_control_v1"]
        known[0]["capabilities"] = []
        assert (await store.known_devices())[0]["capabilities"] == ["remote_control_v1"]
        await store.device_connected("device-a")
        assert await store.state("device-a") is None
        await store.update_state(
            "device-a",
            DeviceStateMessage(type="device.state", revision=1),
        )
        state = await store.state("device-a")
        assert state is not None
        assert state["revision"] == 1

        record = await store.create_command("device-a", "music.play", {})
        await store.mark_delivered(record.command_id)
        await store.complete(
            "device-a",
            DeviceCommandResultMessage(
                type="device.command.result",
                command_id=record.command_id,
                success=True,
                message="playing",
            ),
        )
        command = await store.command(record.command_id)
        assert command is not None
        assert command["status"] == "completed"
        assert command["message"] == "playing"

        first, created = await store.get_or_create_command(
            "device-a", "todo.add", {"text": "same"}, "request-1"
        )
        second, duplicate = await store.get_or_create_command(
            "device-a", "todo.add", {"text": "same"}, "request-1"
        )
        assert created is True
        assert duplicate is False
        assert first.command_id == second.command_id

        unrelated = uuid4()
        await store.complete(
            "device-a",
            DeviceCommandResultMessage(
                type="device.command.result",
                command_id=unrelated,
                success=False,
            ),
        )

    asyncio.run(scenario())
