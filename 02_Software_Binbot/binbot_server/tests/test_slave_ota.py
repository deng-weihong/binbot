import asyncio
import hashlib
import struct
import zlib
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from binbot_server.application.component_ota import ComponentOTAService, OTAError, parse_firmware
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import ComponentConfig
from binbot_server.transport.protocol import ComponentEventMessage
from test_component_ota import ADMIN, register


def package(version: int = 2, image_size: int = 512) -> bytes:
    images = (b"\x17" * image_size, b"\x42" * image_size)
    digests = b"".join(hashlib.sha256(image).digest() for image in images)
    header = bytearray(160)
    header[:8] = b"BBSOTA1\0"
    struct.pack_into("<6I", header, 8, 1, 0x582, version, 0x35000, image_size, image_size)
    header[32:64] = hashlib.sha256(struct.pack("<I", version) + digests).digest()
    header[64:128] = digests
    struct.pack_into("<2I", header, 128, *[zlib.crc32(image) for image in images])
    struct.pack_into("<I", header, 156, zlib.crc32(header[:156]))
    return bytes(header) + b"".join(images)


def info() -> dict[str, Any]:
    return {"profile": "slave_mcu", "project": "binbot_slave", "version": "1",
            "state": 0, "healthy": True, "boot_id": "0000000000000001",
            "capacity": 0x35000, "elf_sha256": "00" * 32}


def test_package_integrity_and_boundaries() -> None:
    data = package()
    assert parse_firmware(data)["profile"] == "slave_mcu"
    for offset in (8, 12, 20, 24, 32, 64, 96, 128, 136, 156, 160, len(data) - 1):
        broken = bytearray(data)
        broken[offset] ^= 1
        with pytest.raises(OTAError):
            parse_firmware(bytes(broken))
    for broken in (data[:159], data[:-4], data + b"\xff" * 4, package(image_size=0x35004)):
        with pytest.raises(OTAError):
            parse_firmware(broken)
    assert parse_firmware(package(image_size=0x35000))["image_size_bytes"] == 0x35000


def test_slave_job_dispatch_and_confirmed_result(tmp_path: Path) -> None:
    async def scenario() -> None:
        registry = DeviceSessionRegistry()
        config = ComponentConfig(directory=str(tmp_path), public_origin="https://binbot.example")
        service = ComponentOTAService(config, registry)
        commands: list[dict[str, Any]] = []

        class Actor:
            session = SimpleNamespace(session_id="slave-test", phase=SimpleNamespace(value="idle"))

            async def send_device_command(self, event: dict[str, Any]) -> None:
                commands.append(event)
                if event["name"] == "component.probe":
                    await service.event("test-device", {
                        "request_id": event["command_id"], "status": "probe",
                        "component_id": "slave-1", "info": info(),
                    })

        await registry.register("test-device", Actor(), capabilities={"component_ota_v1"})
        target = await register(service, package(image_size=0x35000))
        # Package contains both images; capacity applies to one selected slot.
        job = await service.start_job(
            ADMIN, "test-device", "slave-1", target["firmware_id"], "slave-release")
        cached = await service.component(ADMIN, "test-device", "slave-1", False)
        assert cached["project"] == "binbot_slave"
        with pytest.raises(OTAError, match="not been observed"):
            await service.component(ADMIN, "test-device", "component-1", False)
        service.start()
        await asyncio.sleep(0.02)
        dispatched = next(command for command in commands if command["name"] == "component.ota")
        assert dispatched["payload"]["component_id"] == "slave-1"
        assert dispatched["payload"]["sha256"] == target["sha256"]
        event = {"type": "component.event", "request_id": job["job_id"], "component_id": "slave-1",
                 "event_seq": 1, "status": "succeeded", "total_bytes": target["size_bytes"],
                 "bytes_written": target["size_bytes"], "info": {
                 **info(), "version": target["version"],
                 "elf_sha256": target["elf_sha256"]}}
        ComponentEventMessage.model_validate(event)
        await service.event("test-device", event)
        assert service.job(ADMIN, job["job_id"])["status"] == "reconciling"
        event["event_seq"] = 2
        event["info"]["boot_id"] = "0000000000000002"
        await service.event("test-device", event)
        assert service.job(ADMIN, job["job_id"])["status"] == "succeeded"
        await service.close()

    asyncio.run(scenario())
