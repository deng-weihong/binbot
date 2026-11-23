import asyncio
import json
import socket
from dataclasses import replace
from uuid import UUID, uuid4

import pytest
from amqtt.client import MQTTClient
from amqtt.errors import ConnectError

from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.tools import CommandDispatchError, DeviceCommandService
from binbot_server.config import AuthConfig, MQTTConfig, Settings
from binbot_server.domain.errors import StateConflict
from binbot_server.infrastructure.sqlite import SqliteDeviceControlStore
from binbot_server.transport.mqtt import MQTTControlRuntime


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


async def until(predicate, timeout=4):
    async with asyncio.timeout(timeout):
        while not await predicate():
            await asyncio.sleep(0.02)


def state(boot=None, revision=1, brightness=70):
    return {
        "type": "device.control.state",
        "schema_version": 2,
        "boot_id": boot or uuid4().hex,
        "revision": revision,
        "ready": True,
        "brightness": brightness,
        "volume": 40,
        "capabilities": ["control_v2", "brightness_v1", "volume_v1"],
    }


def test_control_state_survives_restart_and_voice_reconnect(tmp_path):
    async def scenario():
        path = str(tmp_path / "state.sqlite")
        store = SqliteDeviceControlStore(path, 10)
        first = state()
        await store.update_control_state("device-a", first)
        await store.device_connected("device-a")
        await store.close()
        store = SqliteDeviceControlStore(path, 10)
        assert (await store.state("device-a"))["control"]["brightness"] == 70
        assert (await store.known_devices())[0]["control_online"] is False
        await store.update_control_state("device-a", {**first, "revision": 0, "brightness": 20})
        assert (await store.state("device-a"))["control"]["brightness"] == 70
        await store.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_real_broker_device_isolation_execution_receipts_and_reconnect():
    async def scenario():
        port = unused_port()
        settings = replace(
            Settings(),
            mqtt=MQTTConfig(enabled=True, port=port, command_timeout_seconds=2),
            auth=AuthConfig(tokens={"device-a": "test-a", "device-b": "test-b"}),
        )
        registry, store = DeviceSessionRegistry(), DeviceControlStore()
        runtime = MQTTControlRuntime(settings, registry, store)
        service = DeviceCommandService(registry, store)
        a = MQTTClient(client_id="bb-device-a:first", config={"auto_reconnect": False})
        b = MQTTClient(client_id="bb-device-b:first", config={"auto_reconnect": False})
        await runtime.start()
        try:
            wrong = MQTTClient(client_id="bb-device-a:wrong", config={"auto_reconnect": False})
            with pytest.raises(ConnectError):
                await wrong.connect(f"mqtt://device-a:wrong@127.0.0.1:{port}/", cleansession=True)
            await a.connect(f"mqtt://device-a:test-a@127.0.0.1:{port}/", cleansession=True)
            await b.connect(f"mqtt://device-b:test-b@127.0.0.1:{port}/", cleansession=True)
            assert await a.subscribe([("binbot/v2/devices/device-b/command", 1)]) == [128]
            assert await a.subscribe([("binbot/v2/devices/device-a/command", 1)]) == [1]
            assert await b.subscribe([("binbot/v2/devices/device-b/command", 1)]) == [1]
            sa, sb = state(), state()
            await a.publish("binbot/v2/devices/device-a/state", json.dumps(sa).encode(), qos=1)
            await b.publish("binbot/v2/devices/device-b/state", json.dumps(sb).encode(), qos=1)

            async def ready():
                return len([r for r in await registry.snapshot() if r.get("control_ready")]) == 2

            await until(ready)
            assert all(not row["voice_online"] for row in await registry.snapshot())
            result = await service.send(
                "device-a", "screen.brightness.set", {"brightness": 40}, "request-one"
            )
            assert result["status"] == "delivered"
            command = json.loads((await a.deliver_message(timeout_duration=2)).data)
            assert command["payload"] == {"brightness": 40}
            assert command["boot_id"] == sa["boot_id"]
            receipt = {
                "type": "device.control.result",
                "schema_version": 2,
                "boot_id": sa["boot_id"],
                "command_id": command["command_id"],
                "success": True,
                "message": "applied",
            }
            # Neither publishing to A's topic nor using A's command ID on B's topic may complete A.
            await b.publish(
                "binbot/v2/devices/device-a/result", json.dumps(receipt).encode(), qos=1
            )
            await b.publish(
                "binbot/v2/devices/device-b/result",
                json.dumps({**receipt, "boot_id": sb["boot_id"]}).encode(),
                qos=1,
            )
            await asyncio.sleep(0.05)
            assert (await service.command(uuid4())) is None
            assert (await service.command(UUID(command["command_id"])))["status"] == "delivered"
            await a.publish(
                "binbot/v2/devices/device-a/result", json.dumps(receipt).encode(), qos=1
            )

            async def complete():
                return (await service.command(UUID(command["command_id"])))["status"] == "completed"

            await until(complete)
            duplicate = await service.send(
                "device-a", "screen.brightness.set", {"brightness": 40}, "request-one"
            )
            assert duplicate["command_id"] == command["command_id"]
            with pytest.raises(StateConflict):
                await service.send(
                    "device-a", "screen.brightness.set", {"brightness": 41}, "request-one"
                )
            with pytest.raises(asyncio.TimeoutError):
                await b.deliver_message(timeout_duration=0.1)
            assert (await store.state("device-b"))["control"]["brightness"] == 70
            pending = await service.send("device-a", "audio.volume.set", {"volume": 50})
            await a.disconnect()

            async def failed():
                return (await service.command(UUID(pending["command_id"])))["status"] == "failed"

            await until(failed)
            with pytest.raises(CommandDispatchError):
                await service.send("device-a", "audio.volume.set", {"volume": 60})
            assert (await store.state("device-a"))["control"]["brightness"] == 70
            expired = await service.send("device-b", "audio.volume.set", {"volume": 65})
            await b.deliver_message(timeout_duration=2)

            async def timed_out():
                return (await service.command(UUID(expired["command_id"])))["status"] == "failed"

            await until(timed_out)
            await b.publish(
                "binbot/v2/devices/device-b/result",
                json.dumps(
                    {
                        **receipt,
                        "boot_id": sb["boot_id"],
                        "command_id": expired["command_id"],
                    }
                ).encode(),
                qos=1,
            )
            assert (await service.command(UUID(expired["command_id"])))["status"] == "failed"
        finally:
            await asyncio.gather(a.disconnect(), b.disconnect(), return_exceptions=True)
            await runtime.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("persistent", [False, True])
def test_restart_marks_uncertain_commands_failed_without_replay(tmp_path, persistent):
    async def scenario():
        path = str(tmp_path / "commands.sqlite")
        store = SqliteDeviceControlStore(path, 10) if persistent else DeviceControlStore()
        pending = await store.create_command("a", "audio.volume.set", {"volume": 20}, "pending")
        await store.mark_delivered(pending.command_id)
        complete = await store.create_command("a", "screen.brightness.set", {"brightness": 30})
        from binbot_server.transport.protocol import DeviceCommandResultMessage

        await store.complete(
            "a",
            DeviceCommandResultMessage(
                type="device.command.result",
                command_id=complete.command_id,
                success=True,
                message="ok",
            ),
        )
        legacy = await store.create_command("a", "todo.add", {"text": "test"})
        if persistent:
            await store.close()
            store = SqliteDeviceControlStore(path, 10)
        await store.fail_unconfirmed_control_commands()
        assert (await store.command(pending.command_id))["status"] == "failed"
        assert (await store.command(complete.command_id))["status"] == "completed"
        assert (await store.command(legacy.command_id))["status"] == "failed"
        registry = DeviceSessionRegistry()
        service = DeviceCommandService(registry, store)
        assert (await service.send("a", "audio.volume.set", {"volume": 20}, "pending"))[
            "status"
        ] == "failed"
        with pytest.raises(StateConflict):
            await service.send("a", "audio.volume.set", {"volume": 21}, "pending")
        await store.close()

    asyncio.run(scenario())


def test_stale_states_do_not_regress_readiness_and_heartbeat_can_recover():
    from types import SimpleNamespace

    async def scenario():
        registry, store = DeviceSessionRegistry(), DeviceControlStore()
        runtime = MQTTControlRuntime(Settings(), registry, store)
        await runtime.connected("client", "a", object())

        async def receive(value):
            await runtime.received(
                "client",
                SimpleNamespace(
                    topic="binbot/v2/devices/a/state",
                    data=json.dumps(value).encode(),
                    publish_packet=None,
                ),
            )

        first = state(revision=4)
        await receive(first)
        await receive({**first, "revision": 3, "ready": False, "capabilities": ["control_v2"]})
        assert (await registry.snapshot())[0]["control_ready"] is True
        await registry.unregister_control("a", "client")
        await receive(first)
        assert (await registry.snapshot())[0]["control_ready"] is True
        second = state(revision=1, brightness=25)
        await receive(second)
        await receive(first)
        assert (await store.state("a"))["control"]["boot_id"] == second["boot_id"]

    asyncio.run(scenario())
