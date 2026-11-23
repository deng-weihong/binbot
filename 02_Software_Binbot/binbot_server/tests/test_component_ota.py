import asyncio
import hashlib
from uuid import uuid4
from pathlib import Path
from typing import Any
from types import SimpleNamespace

import pytest
from esptool.bin_image import ESP32C3FirmwareImage, ImageSegment

from binbot_server.application.component_ota import ComponentOTAService, OTAError, Principal, parse_firmware
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import ComponentConfig

ADMIN = Principal("test", frozenset({"device:read", "firmware:read", "firmware:write", "component:ota:write"}))


def firmware_bytes(project: str = "binbot_round_screen", version: str = "25", payload_bytes: int = 1024) -> bytes:
    descriptor = bytearray(256)
    descriptor[:4] = (0xABCD5432).to_bytes(4, "little")
    descriptor[16:16 + len(version)] = version.encode()
    descriptor[48:48 + len(project)] = project.encode()
    descriptor[144:176] = hashlib.sha256((project + version).encode()).digest()
    image = ESP32C3FirmwareImage()
    image.chip_id = 5
    segment = ImageSegment(0x3FC80000, bytes(descriptor) + b"\x33" * payload_bytes)
    segment.name = ".data"
    image.segments = [segment]
    return image.save(None)


def component_info(version: str = "24", boot: str = "0000000000000001") -> dict[str, Any]:
    return {"profile": "round_screen", "state": 0, "healthy": True, "boot_id": boot,
            "capacity": 2 * 1024 * 1024, "version": version, "project": "binbot_round_screen",
            "elf_sha256": "00" * 32}


async def register(service: ComponentOTAService, data: bytes) -> dict[str, Any]:
    slot = service.prepare(ADMIN, len(data), hashlib.sha256(data).hexdigest())
    upload = slot["upload_id"]
    (service.directory / f"{upload}.upload").write_bytes(data)
    service.finish_upload(upload, True)
    return await service.register(ADMIN, upload)


class ProbeActor:
    def __init__(self, service: ComponentOTAService):
        self.service = service
        self.info = component_info()
        self.commands: list[dict[str, Any]] = []
        self.session = SimpleNamespace(session_id="test-session", phase=SimpleNamespace(value="idle"))

    async def send_device_command(self, event: dict[str, Any]) -> None:
        self.commands.append(event)
        if event["name"] == "component.probe":
            await self.service.event("test-device", {"request_id": event["command_id"], "status": "probe",
                                                     "info": self.info})


def test_image_validation() -> None:
    data = firmware_bytes()
    assert parse_firmware(data)["version"] == "25"
    broken = bytearray(data)
    broken[-1] ^= 1
    for image in (bytes(broken), data + b"extra", firmware_bytes("unknown"), firmware_bytes(version="v2"), b"bad"):
        with pytest.raises(OTAError, match="invalid ESP32"):
            parse_firmware(image)


def test_ambient_firmware_identity() -> None:
    data = firmware_bytes("binbot_ambient_light", "32")
    info = parse_firmware(data)
    assert info["profile"] == "ambient_light"
    assert info["project"] == "binbot_ambient_light"
    assert info["version"] == "32"
    assert info["sha256"] == hashlib.sha256(data).hexdigest()


def test_job_listing_is_device_scoped_persistent_and_redacted(tmp_path: Path) -> None:
    async def scenario() -> None:
        config = ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example")
        service = ComponentOTAService(config, DeviceSessionRegistry())
        target = await register(service, firmware_bytes())
        own = await service.start_job(ADMIN, "device-one", "component-1", target["firmware_id"], "one", wait_for_component=True)
        await service.start_job(ADMIN, "device-two", "component-1", target["firmware_id"], "two", wait_for_component=True)
        service.cancel(ADMIN, own["job_id"])
        scoped = Principal("scoped", ADMIN.scopes, frozenset({"device-one"}))
        result = service.list_jobs(scoped, "device-one")
        assert len(result["items"]) == 1
        assert result["items"][0]["status"] == "cancelled"
        assert "download_token" not in str(result)
        with pytest.raises(OTAError, match="permit"):
            service.list_jobs(scoped, "device-two")
        with pytest.raises(OTAError, match="required"):
            service.list_jobs(ADMIN, "", 0)
        await service.close()
        restored = ComponentOTAService(config, DeviceSessionRegistry())
        assert restored.list_jobs(scoped, "device-one") == result
        await restored.close()

    asyncio.run(scenario())


def test_jobs_idempotency_events_and_recovery(tmp_path: Path) -> None:
    async def scenario() -> None:
        config = ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example")
        registry = DeviceSessionRegistry()
        service = ComponentOTAService(config, registry)
        actor = ProbeActor(service)
        await registry.register("test-device", actor, capabilities={"component_ota_v1"})
        target = await register(service, firmware_bytes())
        assert (await register(service, firmware_bytes()))["firmware_id"] == target["firmware_id"]
        job = await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "release-25")
        duplicate = await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "release-25")
        assert duplicate["job_id"] == job["job_id"]
        with pytest.raises(OTAError) as error:
            await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "release-25", True)
        assert error.value.code == "IDEMPOTENCY_CONFLICT"
        with pytest.raises(OTAError) as error:
            await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "another")
        assert error.value.code == "COMPONENT_BUSY"
        event = {"request_id": job["job_id"], "component_id": "component-1", "event_seq": 1,
                 "status": "transferring", "bytes_written": 512, "total_bytes": target["size_bytes"]}
        with pytest.raises(OTAError):
            await service.event("other-device", event)
        await service.event("test-device", event)
        await service.event("test-device", {**event, "event_seq": 1, "bytes_written": 1024})
        assert service.job(ADMIN, job["job_id"])["bytes_written"] == 512
        await service.close()
        service = ComponentOTAService(config, registry)
        actor.service = service
        service.start()
        await asyncio.sleep(0.05)
        assert any(c["name"] == "component.ota" for c in actor.commands)
        assert service.download(target["firmware_id"], job["job_id"], service.job(ADMIN, job["job_id"])["download_token"]).exists()
        assert "download_token" not in service.public_job(service.job(ADMIN, job["job_id"]))
        await service.event("test-device", {**event, "event_seq": 2, "status": "activating"})
        with pytest.raises(OTAError):
            await service.event("test-device", {**event, "event_seq": 3, "status": "transferring"})
        with pytest.raises(OTAError) as error:
            service.cancel(ADMIN, job["job_id"])
        assert error.value.code == "CANCEL_TOO_LATE"
        info = {**actor.info, "version": target["version"], "elf_sha256": target["elf_sha256"]}
        await service.event("test-device", {**event, "event_seq": 3, "status": "succeeded", "info": info})
        assert service.job(ADMIN, job["job_id"])["status"] == "reconciling"
        info["boot_id"] = "0000000000000002"
        await service.event("test-device", {**event, "event_seq": 4, "status": "succeeded", "info": info})
        assert service.job(ADMIN, job["job_id"])["status"] == "succeeded"
        await service.event("test-device", {**event, "event_seq": 5, "status": "failed"})
        assert service.job(ADMIN, job["job_id"])["status"] == "succeeded"
        with pytest.raises(OTAError):
            service.download(target["firmware_id"], job["job_id"], "wrong-token")
        await service.close()
    asyncio.run(scenario())


def test_preflight_and_permissions(tmp_path: Path) -> None:
    async def scenario() -> None:
        registry = DeviceSessionRegistry()
        service = ComponentOTAService(ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example"), registry)
        actor = ProbeActor(service)
        target = await register(service, firmware_bytes())
        for capabilities, code in ((None, "DEVICE_OFFLINE"), ({"remote_control_v1"}, "CAPABILITY_REQUIRED")):
            if capabilities:
                await registry.register("test-device", actor, capabilities=capabilities)
            with pytest.raises(OTAError) as error:
                await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "key")
            assert error.value.code == code
        await registry.unregister("test-device", actor)
        await registry.register("test-device", actor, capabilities={"component_ota_v1"})
        actor.info["version"] = "26"
        with pytest.raises(OTAError) as error:
            await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "key")
        assert error.value.code == "DOWNGRADE_REJECTED"
        job = await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "key", True)
        assert service.cancel(ADMIN, job["job_id"])["status"] == "cancel_requested"
        reader = Principal("limited", frozenset({"device:read"}), frozenset({"other-device"}))
        with pytest.raises(OTAError):
            service.job(reader, job["job_id"])
        await service.close()
    asyncio.run(scenario())


def test_principal_requires_token_unless_unauthenticated_allowed(tmp_path: Path) -> None:
    strict = ComponentOTAService(ComponentConfig(directory=str(tmp_path / "strict")), DeviceSessionRegistry())
    with pytest.raises(OTAError) as error:
        strict.principal("")
    assert error.value.code == "UNAUTHORIZED"
    open_service = ComponentOTAService(ComponentConfig(directory=str(tmp_path / "open"), allow_unauthenticated=True),
                                       DeviceSessionRegistry())
    open_service.principal("").require("component:ota:write", "binbot-001")


def test_deferred_ota_survives_absence_restart_and_wrong_component(tmp_path: Path) -> None:
    async def scenario() -> None:
        config = ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example")
        registry = DeviceSessionRegistry()
        service = ComponentOTAService(config, registry)
        target = await register(service, firmware_bytes())
        job = await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"],
                                      "wait-25", wait_for_component=True)
        assert job["status"] == "waiting_for_component" and job["before"] is None
        assert not service.db.execute("SELECT * FROM component_outbox").fetchall()
        assert (await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"],
                                       "wait-25", wait_for_component=True))["job_id"] == job["job_id"]
        with pytest.raises(OTAError) as error:
            await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"], "wait-25")
        assert error.value.code == "IDEMPOTENCY_CONFLICT"
        await service.close()
        service = ComponentOTAService(config, registry)
        actor = ProbeActor(service)
        actor.info = None
        await registry.register("test-device", actor, capabilities={"component_ota_v1"})
        service.start()
        await asyncio.sleep(0.05)
        assert service.job(ADMIN, job["job_id"])["status"] == "waiting_for_component"
        assert not any(c["name"] == "component.ota" for c in actor.commands)
        notice = {"request_id": str(uuid4()), "component_id": "component-1", "status": "probe"}
        await service.event("test-device", {**notice, "info": {
            **component_info(), "profile": "ambient_light", "project": "binbot_ambient_light"}})
        assert service.job(ADMIN, job["job_id"])["waiting_reason"] == "waiting_for_matching_profile"
        await service.event("test-device", {**notice, "info": {**component_info(), "healthy": False}})
        assert service.job(ADMIN, job["job_id"])["waiting_reason"] == "component_initializing"
        await service.event("test-device", {**notice, "info": component_info()})
        ready = service.job(ADMIN, job["job_id"])
        assert ready["status"] == "preparing" and ready["before"]["version"] == "24"
        assert service.db.execute("SELECT job FROM component_outbox").fetchone()["job"] == job["job_id"]
        await service.event("test-device", {"request_id": str(uuid4()), "status": "succeeded"})
        await service.event("test-device", {**notice, "info": component_info(version="23")})
        assert service.job(ADMIN, job["job_id"])["before"]["version"] == "24"
        await service.close()
        service = ComponentOTAService(config, registry)
        actor.service = service
        service.start()
        await asyncio.sleep(0.05)
        commands = [c for c in actor.commands if c["name"] == "component.ota"]
        assert len(commands) == 1
        assert commands[0]["payload"]["before_version"] == "24"
        assert commands[0]["payload"]["firmware_id"] == target["firmware_id"]
        await service.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("mode", ["cancel", "current", "newer", "capacity"])
def test_deferred_ota_preflight_and_cancellation(tmp_path: Path, mode: str) -> None:
    async def scenario() -> None:
        service = ComponentOTAService(ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example"),
                                      DeviceSessionRegistry())
        target = await register(service, firmware_bytes())
        job = await service.start_job(ADMIN, "test-device", "component-1", target["firmware_id"],
                                      mode, wait_for_component=True)
        if mode == "cancel":
            assert service.cancel(ADMIN, job["job_id"])["status"] == "cancelled"
            info = component_info()
            expected = "cancelled"
        elif mode == "current":
            info = {**component_info(version="25"), "elf_sha256": target["elf_sha256"]}
            expected = "succeeded"
        elif mode == "newer":
            info = component_info(version="26")
            expected = "succeeded"
        else:
            info = {**component_info(), "capacity": 1}
            expected = "failed"
        await service.event("test-device", {"request_id": str(uuid4()), "component_id": "component-1",
                                            "status": "probe", "info": info})
        assert service.job(ADMIN, job["job_id"])["status"] == expected
        assert not service.db.execute("SELECT * FROM component_outbox").fetchall()
        await service.close()
    asyncio.run(scenario())


def test_http_ota_defaults_to_waiting_when_device_offline(tmp_path: Path) -> None:
    from aiohttp import web
    from aiohttp.test_utils import TestClient, TestServer
    from binbot_server.transport.component_http import component_errors, register_routes

    async def scenario() -> None:
        service = ComponentOTAService(ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example",
                                                       allow_unauthenticated=True), DeviceSessionRegistry())
        target = await register(service, firmware_bytes())
        app = web.Application(middlewares=[component_errors])
        register_routes(app, service)
        async with TestClient(TestServer(app)) as client:
            response = await client.post("/api/v1/admin/devices/test-device/components/component-1/ota", json={
                "firmware_id": target["firmware_id"], "idempotency_key": "http-deferred"})
            assert response.status == 202
            assert (await response.json())["status"] == "waiting_for_component"
            response = await client.post("/api/v1/admin/devices/test-device/components/component-1/ota", json={
                "firmware_id": target["firmware_id"], "idempotency_key": "http-now", "wait_for_component": False})
            assert response.status == 409 and (await response.json())["code"] == "DEVICE_OFFLINE"
        await service.close()
    asyncio.run(scenario())


def test_gif_upload_without_idempotency_key_reaches_service(tmp_path: Path) -> None:
    from aiohttp import web
    from aiohttp.test_utils import TestClient, TestServer

    from binbot_server.transport.component_http import component_errors, register_routes

    async def scenario() -> None:
        service = ComponentOTAService(ComponentConfig(directory=str(tmp_path), allow_unauthenticated=True),
                                      DeviceSessionRegistry())
        app = web.Application(middlewares=[component_errors])
        register_routes(app, service)
        async with TestClient(TestServer(app)) as client:
            response = await client.put("/api/v1/admin/devices/binbot-001/components/component-1/gif?name=holiday.png",
                                       data=b"not a gif")
            assert response.status != 500
            assert (await response.json())["code"]

    asyncio.run(scenario())
