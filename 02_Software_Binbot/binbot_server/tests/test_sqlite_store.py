import asyncio
from pathlib import Path
from binbot_server.infrastructure.sqlite import (
    SqliteConversationStore,
    SqliteDeviceControlStore,
    SqliteMemoryStore,
)
from binbot_server.transport.protocol import DeviceCommandResultMessage, DeviceStateMessage


def test_sqlite_conversation_survives_store_restart(tmp_path: Path) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "binbot.sqlite3")
        first = SqliteConversationStore(path, max_messages=3)
        await first.append_turn("device-a", "第一句", "第一答")
        await first.append_turn("device-a", "第二句", "第二答")
        await first.close()

        second = SqliteConversationStore(path, max_messages=3)
        messages = await second.load("device-a")
        assert [(message.role, message.content) for message in messages] == [
            ("assistant", "第一答"),
            ("user", "第二句"),
            ("assistant", "第二答"),
        ]
        entries = await second.list_conversations("device-")
        assert entries[0].conversation_key == "device-a"
        assert entries[0].message_count == 3
        await second.close()

    asyncio.run(scenario())


def test_sqlite_device_control_persists_state_and_commands(tmp_path: Path) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "binbot.sqlite3")
        first = SqliteDeviceControlStore(path, max_commands=10)
        await first.remember_device({
            "id": "device-a", "remote_ip": "192.0.2.1", "connected_at": "2026-10-08T00:00:00+00:00",
            "last_seen_at": "2026-10-08T00:00:00+00:00", "capabilities": ["remote_control_v1"],
            "online": True, "phase": "idle", "session_id": "old-session",
        })
        await first.update_state(
            "device-a",
            DeviceStateMessage(type="device.state", revision=3),
        )
        record = await first.create_command("device-a", "music.play", {})
        await first.mark_delivered(record.command_id)
        await first.complete(
            "device-a",
            DeviceCommandResultMessage(
                type="device.command.result",
                command_id=record.command_id,
                success=True,
                message="playing",
            ),
        )
        await first.close()

        second = SqliteDeviceControlStore(path, max_commands=10)
        known = await second.known_devices()
        assert len(known) == 1
        assert known[0]["id"] == "device-a" and known[0]["online"] is False
        assert known[0]["session_id"] == "" and known[0]["phase"] == "offline"
        assert known[0]["capabilities"] == ["remote_control_v1"]
        state = await second.state("device-a")
        assert state is not None
        assert state["revision"] == 3
        command = await second.command(record.command_id)
        assert command is not None
        assert command["status"] == "completed"
        assert command["message"] == "playing"
        duplicate, created = await second.get_or_create_command(
            "device-a", "music.play", {}, "request-1"
        )
        retry, was_created = await second.get_or_create_command(
            "device-a", "music.play", {}, "request-1"
        )
        assert created is True
        assert was_created is False
        assert duplicate.command_id == retry.command_id
        recent_ids = [
            item["command_id"] for item in await second.recent_commands("device-a")
        ]
        assert recent_ids[:2] == [str(retry.command_id), str(record.command_id)]
        await second.device_connected("device-a")
        assert await second.state("device-a") is None
        await second.close()

    asyncio.run(scenario())


def test_sqlite_discovers_devices_from_existing_state_and_commands(tmp_path: Path) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "binbot.sqlite3")
        first = SqliteDeviceControlStore(path)
        await first.update_state(
            "state-device", DeviceStateMessage(type="device.state", revision=1)
        )
        await first.create_command("command-device", "music.play", {})
        await first.close()
        second = SqliteDeviceControlStore(path)
        known = await second.known_devices()
        assert {item["id"] for item in known} == {"state-device", "command-device"}
        assert all(item["online"] is False for item in known)
        await second.close()

    asyncio.run(scenario())


def test_sqlite_command_history_is_bounded(tmp_path: Path) -> None:
    async def scenario() -> None:
        store = SqliteDeviceControlStore(str(tmp_path / "binbot.sqlite3"), max_commands=2)
        first = await store.create_command("device-a", "music.play", {})
        second = await store.create_command("device-a", "music.pause", {})
        third = await store.create_command("device-a", "music.stop", {})
        assert await store.command(first.command_id) is None
        assert [item["command_id"] for item in await store.recent_commands("device-a")] == [
            str(third.command_id),
            str(second.command_id),
        ]
        await store.close()

    asyncio.run(scenario())


def test_sqlite_memory_survives_store_restart(tmp_path: Path) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "binbot.sqlite3")
        first = SqliteMemoryStore(path)
        item = await first.remember("device-a", "用户常用英语回答")
        await first.close()
        second = SqliteMemoryStore(path)
        matches = await second.search("device-a", "英语")
        assert [match.memory_id for match in matches] == [item.memory_id]
        assert await second.forget("device-a", item.memory_id) is True
        await second.close()

    asyncio.run(scenario())
