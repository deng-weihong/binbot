import asyncio
import ctypes as c
import hashlib
import json
import os
import socket
from dataclasses import replace
from pathlib import Path
from uuid import UUID

import httpx
import pytest
import uvicorn
from aiohttp import ClientSession
from aiohttp.test_utils import TestServer
from mcp import ClientSession as MCPSession
from mcp.client.streamable_http import streamablehttp_client

from binbot_server.bootstrap import create_application
from binbot_server.config import ComponentConfig, Settings
from binbot_server.transport.mcp import AuthenticatedMCP, create_mcp
from test_component_ota import firmware_bytes


class Command(c.Structure):
    _fields_ = [("request_id", c.c_ubyte * 16), ("action", c.c_uint32), ("size", c.c_uint32),
                ("before_boot_id", c.c_uint64), ("cancel", c.c_uint8), ("allow_downgrade", c.c_uint8),
                ("reconcile_only", c.c_uint8), ("reserved", c.c_uint8 * 5),
                ("version", c.c_char * 32), ("before_version", c.c_char * 32),
                ("sha256", c.c_char * 65), ("elf_sha256", c.c_char * 65),
                ("before_elf_sha256", c.c_char * 65), ("url", c.c_char * 768),
                ("file_name", c.c_char * 16)]


class Event(c.Structure):
    _fields_ = [("request_id", c.c_ubyte * 16), ("event_seq", c.c_uint64), ("status", c.c_uint32),
                ("bytes_written", c.c_uint32), ("total_bytes", c.c_uint32),
                ("has_info", c.c_uint8), ("profile", c.c_uint8), ("healthy", c.c_uint8),
                ("state", c.c_uint8), ("boot_id", c.c_uint64), ("capacity", c.c_uint32),
                ("version", c.c_char * 32), ("project", c.c_char * 32),
                ("elf_sha256", c.c_char * 65), ("message", c.c_char * 160)]


STATES = ["", "preparing", "downloading", "transferring", "validating", "activating",
          "verifying", "reconciling", "succeeded", "failed", "cancelled", "probe"]
PROFILES = ["", "round_screen", "holographic_screen", "showcase_turntable"]


@pytest.mark.integration
def test_mcp_http_websocket_worker_uart(tmp_path, monkeypatch) -> None:
    bridge_path = Path(__file__).parents[2] / "binbot_app/build/host-simulator/bin/test_component_ota"
    if not bridge_path.exists():
        pytest.skip("build test_component_ota with the App host-simulator preset first")
    monkeypatch.setenv("BINBOT_MCP_SERVICE_TOKEN", "integration-service-token")

    async def scenario() -> None:
        settings = replace(Settings(), component=ComponentConfig(directory=str(tmp_path / "server"),
                                                                  public_origin="http://127.0.0.1"))
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        origin = str(server.make_url("/")).rstrip("/")
        # TestServer chooses its port after service construction.
        from binbot_server.transport.component_http import SERVICE_KEY
        service = application.http[SERVICE_KEY]
        service.config = replace(service.config, public_origin=origin)
        bridge = await asyncio.create_subprocess_exec(str(bridge_path), "--bridge",
                    stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                    env={**os.environ, "BINBOT_COMPONENT_STATE_DIR": str(tmp_path / "worker")})
        mcp = create_mcp(origin, "integration-service-token")
        listen = socket.socket()
        listen.bind(("127.0.0.1", 0))
        mcp_port = listen.getsockname()[1]
        uvicorn_server = uvicorn.Server(uvicorn.Config(
            AuthenticatedMCP(mcp.streamable_http_app(), "integration-mcp-token"),
            log_level="error", access_log=False, lifespan="on"))
        mcp_task = asyncio.create_task(uvicorn_server.serve(sockets=[listen]))
        while not uvicorn_server.started:
            if mcp_task.done():
                await mcp_task
            await asyncio.sleep(0.01)
        workers = []
        try:
            async with ClientSession() as http:
                response = await http.get(origin + "/api/v1/admin/firmware")
                assert response.status == 401  # Loopback bypass cannot grant firmware access.
                async with http.ws_connect(origin + "/v1/device/ws") as ws:
                    await ws.send_json({"type": "hello", "protocol_version": 1, "device_id": "test-device",
                        "client_version": "worker-test", "capabilities": ["component_ota_v1"],
                        "uplink": {"codec": "opus", "sample_rate": 16000, "channels": 1, "frame_ms": 20},
                        "downlink": {"codec": "opus", "sample_rate": 24000, "channels": 1, "frame_ms": 20}})
                    async def commands() -> None:
                        async for message in ws:
                            value = message.json()
                            if value.get("type") != "device.command":
                                continue
                            command = Command()
                            command.request_id[:] = UUID(value["command_id"]).bytes
                            command.action = 1 if value["name"] == "component.probe" else 2
                            payload = value["payload"]
                            if command.action == 2:
                                command.size = payload["size_bytes"]
                                command.before_boot_id = int(payload["before_boot_id"], 16)
                                command.cancel = payload["cancel_requested"]
                                command.allow_downgrade = payload["allow_downgrade"]
                                command.reconcile_only = payload["reconcile_only"]
                                for field in ("version", "before_version", "sha256", "elf_sha256", "before_elf_sha256"):
                                    setattr(command, field, payload[field].encode())
                                command.url = (origin + payload["asset_path"]).encode()
                            bridge.stdin.write(bytes(command))
                            await bridge.stdin.drain()

                    resumed = False
                    async def events() -> None:
                        nonlocal resumed
                        while True:
                            raw = await bridge.stdout.readexactly(c.sizeof(Event))
                            event = Event.from_buffer_copy(raw)
                            info = None
                            if event.has_info:
                                info = {"profile": PROFILES[event.profile], "state": event.state,
                                    "healthy": bool(event.healthy), "boot_id": f"{event.boot_id:016x}",
                                    "capacity": event.capacity, "version": event.version.decode(),
                                    "project": event.project.decode(), "elf_sha256": event.elf_sha256.decode()}
                            await ws.send_json({"type": "component.event", "request_id": str(UUID(bytes=bytes(event.request_id))),
                                "component_id": "component-1", "event_seq": event.event_seq,
                                "status": STATES[event.status], "bytes_written": event.bytes_written,
                                "total_bytes": event.total_bytes, "info": info, "message": event.message.decode()})
                            if event.status == 3 and event.bytes_written > 0 and not resumed:
                                resumed = True
                                restart = Command()
                                restart.action = 3
                                bridge.stdin.write(bytes(restart))
                                await bridge.stdin.drain()

                    workers = [asyncio.create_task(commands()), asyncio.create_task(events())]
                    mcp_url = f"http://127.0.0.1:{mcp_port}/mcp"
                    async with httpx.AsyncClient() as client:
                        assert (await client.post(mcp_url)).status_code == 401
                    async with streamablehttp_client(mcp_url, headers={"Authorization": "Bearer integration-mcp-token"}) as (read, write, _):
                        async with MCPSession(read, write) as session:
                            await session.initialize()
                            discovered = await session.list_tools()
                            assert len(discovered.tools) == 8
                            invalid = await session.call_tool("binbot_list_devices", {"unexpected": True})
                            assert invalid.isError
                            async def tool(name: str, args: dict) -> dict:
                                result = await session.call_tool(name, args)
                                assert not result.isError, result
                                return result.structuredContent or json.loads(result.content[0].text)
                            devices = await tool("binbot_list_devices", {"online_only": True})
                            assert devices["devices"][0]["id"] == "test-device"
                            info = await tool("binbot_get_component", {"device_id": "test-device", "component_id": "component-1"})
                            assert info["version"] == "24" and info["fresh"]
                            data = firmware_bytes(payload_bytes=64 * 1024)
                            upload = await tool("binbot_prepare_firmware_upload", {"size_bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
                            assert (await http.put(upload["upload_url"], data=data)).status == 200
                            assert (await http.put(upload["upload_url"], data=data)).status == 409
                            firmware = await tool("binbot_register_firmware", {"upload_id": upload["upload_id"]})
                            absent = Command()
                            absent.action = 4
                            bridge.stdin.write(bytes(absent))
                            await bridge.stdin.drain()
                            await asyncio.sleep(0.1)
                            args = {"device_id": "test-device", "component_id": "component-1",
                                    "firmware_id": firmware["firmware_id"], "idempotency_key": "integration-25"}
                            job = await tool("binbot_start_component_ota", args)
                            assert job["status"] == "waiting_for_component"
                            assert (await tool("binbot_start_component_ota", args))["job_id"] == job["job_id"]
                            assert "download_token" not in job
                            await asyncio.sleep(1.2)
                            waiting = await tool("binbot_get_ota_job", {"job_id": job["job_id"]})
                            assert waiting["status"] == "waiting_for_component" and waiting["bytes_written"] == 0
                            inserted = Command()
                            inserted.action = 5
                            bridge.stdin.write(bytes(inserted))
                            await bridge.stdin.drain()
                            for _ in range(150):
                                snapshot = await tool("binbot_get_ota_job", {"job_id": job["job_id"]})
                                if snapshot["status"] in {"succeeded", "failed", "cancelled"}:
                                    break
                                for task in workers:
                                    if task.done():
                                        await task
                                await asyncio.sleep(0.1)
                            assert snapshot["status"] == "succeeded", snapshot
                            assert snapshot["bytes_written"] == len(data)
                            assert resumed
                            assert (tmp_path / "worker/job.state").exists()
                            restart = Command()
                            restart.action = 3
                            bridge.stdin.write(bytes(restart))
                            await bridge.stdin.drain()
                            await asyncio.sleep(0.1)
                            recovered = await tool("binbot_get_component", {"device_id": "test-device", "component_id": "component-1"})
                            assert recovered["version"] == "25" and recovered["boot_id"] == "0000000000000002"
                            data26 = firmware_bytes(version="26")
                            upload26 = await tool("binbot_prepare_firmware_upload", {"size_bytes": len(data26), "sha256": hashlib.sha256(data26).hexdigest()})
                            assert (await http.put(upload26["upload_url"], data=data26)).status == 200
                            target26 = await tool("binbot_register_firmware", {"upload_id": upload26["upload_id"]})
                            next_job = await tool("binbot_start_component_ota", {**args,
                                "firmware_id": target26["firmware_id"], "idempotency_key": "cancel-before-start"})
                            await tool("binbot_cancel_ota_job", {"job_id": next_job["job_id"]})
                            for _ in range(60):
                                cancelled = await tool("binbot_get_ota_job", {"job_id": next_job["job_id"]})
                                if cancelled["status"] in {"cancelled", "failed", "succeeded"}:
                                    break
                                await asyncio.sleep(0.1)
                            assert cancelled["status"] == "cancelled", cancelled
        finally:
            for task in workers:
                task.cancel()
            await asyncio.gather(*workers, return_exceptions=True)
            bridge.stdin.close()
            try:
                await asyncio.wait_for(bridge.wait(), 5)
            except TimeoutError:
                bridge.kill()
                await bridge.wait()
            assert bridge.returncode == 0
            uvicorn_server.should_exit = True
            await mcp_task
            listen.close()
            await server.close()
    asyncio.run(scenario())
