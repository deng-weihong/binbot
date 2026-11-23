import asyncio
import hashlib
import json
import shutil
from dataclasses import replace
from pathlib import Path
from uuid import UUID, uuid4

from aiohttp import ClientSession
from aiohttp.test_utils import TestServer
import pytest

from binbot_server.bootstrap import create_application
from binbot_server.config import Settings
from binbot_server.transport.http import _CONTEXT_KEY
from binbot_server.transport.protocol import DeviceCommandResultMessage
from test_component_ota import ProbeActor
from test_http_integration import send_hello


@pytest.mark.integration
def test_web_image_arrives_as_verified_gallery_bundle(tmp_path: Path) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            server=replace(defaults.server, asset_directory=str(tmp_path / "assets")),
            component=replace(defaults.component, directory=str(tmp_path / "components")),
        )
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        content = (Path(__file__).parents[2]
                   / "binbot_web/public/binbot-right-component.gif").read_bytes()
        try:
            async with ClientSession() as client:
                ws = await client.ws_connect(
                    str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
                )
                await send_hello(ws, "gallery-device", remote_control=True)
                endpoint = server.make_url("/api/v1/admin/devices/gallery-device/modules/images")
                async with client.put(
                    endpoint, params={"name": "../测试图片.gif"}, data=content,
                    headers={"Idempotency-Key": "gallery-upload-retry"},
                ) as response:
                    assert response.status == 202, await response.text()
                    uploaded = await response.json()
                    assert uploaded["name"] == "测试图片.gif"
                command = await ws.receive_json()
                assert command["name"] == "modules.image"
                async with client.put(
                    endpoint, params={"name": "../测试图片.gif"}, data=content,
                    headers={"Idempotency-Key": "gallery-upload-retry"},
                ) as response:
                    assert response.status == 202
                    retry = await response.json()
                    assert retry["asset_id"] == uploaded["asset_id"]
                    assert retry["command"]["command_id"] == uploaded["command"]["command_id"]
                with pytest.raises(asyncio.TimeoutError):
                    await asyncio.wait_for(ws.receive_json(), timeout=0.05)
                base = command["payload"]["asset_path"]
                async with client.get(server.make_url(base + "/manifest.json")) as response:
                    assert response.status == 200
                    manifest = json.loads(await response.text())
                assert manifest["id"] == uploaded["asset_id"]
                for key, filename, width, height in (
                    ("round", "round.gif", 240, 240), ("holo", "holo.gif", 320, 240),
                    ("thumbnail", "thumbnail.png", 0, 0),
                ):
                    async with client.get(server.make_url(base + "/" + filename)) as response:
                        assert response.status == 200
                        data = await response.read()
                    assert len(data) == manifest[key]["size"]
                    assert hashlib.sha256(data).hexdigest() == manifest[key]["sha256"]
                    if width:
                        assert data[:6] in (b"GIF87a", b"GIF89a")
                        assert data[6:10] == width.to_bytes(2, "little") + height.to_bytes(2, "little")
                        assert manifest[key]["width"] == width
                        assert manifest[key]["height"] == height
                        assert len(data) <= 512 * 1024
                    else:
                        assert data.startswith(b"\x89PNG\r\n\x1a\n")
                async with client.get(server.make_url(base + "/job.state")) as response:
                    assert response.status == 404
                missing_url = server.make_url(f"/v1/device/module-images/{uuid4()}/round.gif")
                async with client.get(missing_url) as response:
                    assert response.status == 404
                async with client.put(endpoint, data=b"broken image") as response:
                    assert response.status == 415
                await ws.close()
                async with client.put(endpoint, data=content) as response:
                    assert response.status == 409
                assert len(list((tmp_path / "assets/module-images").glob("*/manifest.json"))) == 1
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_direct_component_upload_is_synced_to_gallery_after_success(tmp_path: Path) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            server=replace(defaults.server, asset_directory=str(tmp_path / "assets")),
            component=replace(defaults.component, directory=str(tmp_path / "components"),
                              allow_unauthenticated=True),
        )
        application = create_application(settings)
        context = application.http[_CONTEXT_KEY]
        service = context.component_ota
        actor = ProbeActor(service)
        await context.registry.register("test-device", actor,
                                        capabilities={"component_ota_v1", "remote_control_v1"})
        server = TestServer(application.http)
        await server.start_server()
        content = (Path(__file__).parents[2]
                   / "binbot_web/public/binbot-right-component.gif").read_bytes()
        try:
            async with ClientSession() as client:
                endpoint = server.make_url("/api/v1/admin/devices/test-device/components/component-1/gif")
                async with client.put(endpoint, params={"name": "../测试动画.gif"}, data=content,
                                      headers={"Idempotency-Key": "direct-gallery-test"}) as response:
                    assert response.status == 202, await response.text()
                    uploaded = await response.json()
                asset_id, job_id = uploaded["gallery_asset_id"], uploaded["job_id"]
                assert uploaded["source_name"] == "测试动画.gif"
                assert not any(c["name"] == "modules.image" for c in actor.commands)
                directory = tmp_path / "assets/module-images" / asset_id
                manifest = json.loads((directory / "manifest.json").read_text())
                assert manifest["name"] == "测试动画.gif"
                assert manifest["round"]["sha256"] == uploaded["target"]["sha256"]
                assert manifest["holo"]["width"] == 320 and manifest["holo"]["height"] == 240
                async with client.put(endpoint, params={"name": "ignored.gif"}, data=content,
                                      headers={"Idempotency-Key": "direct-gallery-test"}) as response:
                    assert response.status == 202
                    assert (await response.json())["job_id"] == job_id
                assert len(list(directory.parent.glob("*/manifest.json"))) == 1
                await service.event("test-device", {
                    "request_id": job_id, "component_id": "component-1", "event_seq": 1,
                    "status": "succeeded", "bytes_written": uploaded["total_bytes"],
                    "total_bytes": uploaded["total_bytes"], "message": "display asset replaced",
                })
                for _ in range(100):
                    commands = [c for c in actor.commands if c["name"] == "modules.image"]
                    if commands:
                        break
                    await asyncio.sleep(.05)
                assert len(commands) == 1
                command = commands[0]
                assert command["payload"]["asset_id"] == asset_id
                assert command["payload"]["display_profile"] == 1
                await context.controls.complete("test-device", DeviceCommandResultMessage(
                    type="device.command.result", command_id=UUID(command["command_id"]),
                    success=True, message="module gallery image received"))
                for _ in range(100):
                    job = service.job(service.principal(""), job_id)
                    if job["gallery_status"] == "completed":
                        break
                    await asyncio.sleep(.05)
                assert job["gallery_status"] == "completed"
                assert len([c for c in actor.commands if c["name"] == "modules.image"]) == 1
                # Previously successful uploads can be recovered without
                # writing the display a second time.
                job.pop("gallery_asset_id")
                job.pop("gallery_status")
                job.pop("gallery_command_id")
                job.pop("source_name")
                service._save_job(job)
                shutil.rmtree(directory)
                async with client.post(server.make_url(f"/api/v1/admin/ota/jobs/{job_id}/gallery")) as response:
                    assert response.status == 202, await response.text()
                    restored = await response.json()
                assert restored["gallery_asset_id"] == asset_id
                assert restored["status"] == "succeeded" and restored["gallery_status"] == "pending"
                assert (directory / "manifest.json").is_file()
                async with client.post(server.make_url(f"/api/v1/admin/ota/jobs/{job_id}/gallery")) as response:
                    assert response.status == 202
                    assert (await response.json())["gallery_asset_id"] == asset_id
                assert len(list(directory.parent.glob("*/manifest.json"))) == 1
        finally:
            await context.registry.unregister("test-device", actor)
            await server.close()

    asyncio.run(scenario())
