"""HTTP -> aMQTT -> compiled device daemon -> App IPC -> execution receipt.

Uses the C App fixture rather than RV1106 hardware. Build control-host first.
"""

import asyncio
import json
import os
from dataclasses import replace
from pathlib import Path

import pytest
from aiohttp import ClientSession
from aiohttp.test_utils import TestServer

from binbot_server.bootstrap import create_application
from binbot_server.config import AuthConfig, MQTTConfig, Settings
from test_mqtt_control import unused_port, until


@pytest.mark.integration
def test_http_mqtt_and_c_app_execution_isolated_for_two_devices(tmp_path):
    build = Path(__file__).resolve().parents[2] / "binbot_app/build/control-host"
    daemon, fixture = build / "binbot_control", build / "control_ipc/test_control_service"
    if not daemon.is_file() or not fixture.is_file():
        pytest.skip("build app/services/binbot_control into app/build/control-host first")

    async def scenario():
        port = unused_port()
        defaults = Settings()
        settings = replace(
            defaults,
            mqtt=MQTTConfig(enabled=True, port=port, command_timeout_seconds=3),
            auth=AuthConfig(tokens={"device-a": "test-a", "device-b": "test-b"}),
            server=replace(defaults.server, asset_directory=str(tmp_path / "assets")),
            component=replace(defaults.component, directory=str(tmp_path / "components")),
        )
        server = TestServer(create_application(settings).http)
        processes = []

        async def start_app(device):
            # AF_UNIX paths must fit in 108 bytes, including under pytest's tmp path.
            process = await asyncio.create_subprocess_exec(
                str(fixture),
                "--serve",
                str(tmp_path / f"{device}.sock"),
                str(tmp_path / f"{device}.json"),
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
            processes.append(process)
            return process

        async def stop(process):
            if process.returncode is None:
                process.terminate()
                await asyncio.wait_for(process.wait(), 3)

        try:
            await server.start_server()
            apps = {device: await start_app(device) for device in ("device-a", "device-b")}
            for device, token in (("device-a", "test-a"), ("device-b", "test-b")):
                process = await asyncio.create_subprocess_exec(
                    str(daemon),
                    env={
                        **os.environ,
                        "BINBOT_DEVICE_ID": device,
                        "BINBOT_DEVICE_TOKEN": token,
                        "BINBOT_MQTT_HOST": "127.0.0.1",
                        "BINBOT_MQTT_PORT": str(port),
                        "BINBOT_MQTT_CA_FILE": "",
                        "BINBOT_CONTROL_SOCKET": str(tmp_path / f"{device}.sock"),
                    },
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                )
                processes.append(process)
            async with ClientSession() as client:

                async def snapshot(device):
                    async with client.get(
                        server.make_url(f"/api/v1/admin/devices/{device}/state")
                    ) as response:
                        assert response.status == 200
                        return await response.json()

                async def ready():
                    return all(
                        [
                            (await snapshot(device)).get("control_ready")
                            for device in ("device-a", "device-b")
                        ]
                    )

                await until(ready, timeout=8)
                assert all(
                    [not (await snapshot(d))["voice_online"] for d in ("device-a", "device-b")]
                )

                async def send(name, value, key=None, expected=202):
                    field = "brightness" if name == "screen.brightness.set" else "volume"
                    async with client.post(
                        server.make_url("/api/v1/admin/devices/device-a/commands"),
                        json={"name": name, "payload": {field: value}},
                        headers={"Idempotency-Key": key} if key else {},
                    ) as response:
                        assert response.status == expected, await response.text()
                        if expected != 202:
                            return None
                        command = await response.json()

                    async def completed():
                        async with client.get(
                            server.make_url(f"/api/v1/admin/commands/{command['command_id']}")
                        ) as response:
                            command.update(await response.json())
                        return command["status"] in {"completed", "failed"}

                    await until(completed)
                    return command

                first = await send("screen.brightness.set", 40, "brightness-once")
                assert first["status"] == "completed"

                async def changed():
                    return (await snapshot("device-a"))["state"]["control"]["brightness"] == 40

                await until(changed)
                assert (await snapshot("device-b"))["state"]["control"]["brightness"] == 70
                assert (await send("screen.brightness.set", 13))["status"] == "failed"
                assert (await send("audio.volume.set", 61))["status"] == "completed"
                duplicate = await send("screen.brightness.set", 40, "brightness-once")
                assert duplicate["command_id"] == first["command_id"]
                await send("screen.brightness.set", 42, "brightness-once", expected=409)
                journal = json.loads((tmp_path / "device-a.json").read_text())
                assert len([r for r in journal if r["id"] == first["command_id"]]) == 1
                old_boot = (await snapshot("device-a"))["state"]["control"]["boot_id"]
                await stop(apps["device-a"])

                async def unavailable():
                    return not (await snapshot("device-a"))["control_ready"]

                await until(unavailable)
                await send("screen.brightness.set", 45, expected=409)
                apps["device-a"] = await start_app("device-a")
                await until(ready, timeout=8)
                assert (await snapshot("device-a"))["state"]["control"]["boot_id"] != old_boot
                assert (await snapshot("device-b"))["state"]["control"]["brightness"] == 70
        finally:
            for process in reversed(processes):
                await stop(process)
            await asyncio.wait_for(server.close(), 5)

    asyncio.run(scenario())
