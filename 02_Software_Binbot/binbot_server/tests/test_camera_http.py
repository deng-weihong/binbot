import asyncio
import json
from dataclasses import replace

import pytest
from aiohttp import ClientSession, CookieJar, web
from aiohttp.test_utils import TestServer

from binbot_server.bootstrap import create_application
from binbot_server.config import CameraConfig, Settings


@pytest.mark.integration
def test_camera_signaling_requires_admin_and_keeps_media_token_private(monkeypatch):
    async def scenario():
        viewer_token = "internal-viewer-token-" + "x" * 32
        monkeypatch.setenv("BINBOT_MEDIA_VIEWER_TOKEN", viewer_token)
        monkeypatch.setenv("BINBOT_ADMIN_PASSWORD", "test-admin-password")
        seen = []

        async def media(request):
            assert dict(request.query) == {
                "app": "live", "stream": "camera-1", "type": "play", "token": viewer_token,
            }
            assert await request.text() == "v=0\r\noffer"
            response = web.StreamResponse(headers={"Content-Type": "application/json"})
            await response.prepare(request)
            await response.write(b'{"code":0,')
            await asyncio.sleep(0.01)
            await response.write(b'"sdp":"v=0\\r\\nanswer"}')
            return response

        media_app = web.Application()
        media_app.router.add_post("/index/api/webrtc", media)
        async with TestServer(media_app) as media_server:
            defaults = Settings()
            settings = replace(defaults,
                camera=CameraConfig(enabled=True, api_url=str(media_server.make_url(""))),
                admin=replace(defaults.admin, allow_unauthenticated_loopback=False),
                vad=replace(defaults.vad, enabled=False),
            )
            application = create_application(settings)

            async def snapshot():
                return [{"id": "camera-1", "capabilities": ["camera_publish_v1"]}]

            async def send_event(device_id, event):
                seen.append((device_id, event))

            monkeypatch.setattr(application.registry, "snapshot", snapshot)
            monkeypatch.setattr(application.registry, "send_event", send_event)
            async with TestServer(application.http) as server, ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                path = str(server.make_url("/api/v1/admin/devices/camera-1/camera/sessions"))
                for method, suffix in [("POST", ""), ("PUT", "/missing"), ("DELETE", "/missing"), ("POST", "/missing/webrtc")]:
                    async with client.request(method, path + suffix) as response:
                        assert response.status == 401
                async with client.post(server.make_url("/api/v1/admin/session"), json={
                    "username": "admin", "password": "test-admin-password",
                }) as response:
                    assert response.status == 200
                async with client.post(path) as response:
                    assert response.status == 201
                    body = await response.text()
                    assert viewer_token not in body and "token" not in body
                    session_id = json.loads(body)["session_id"]
                session_path = path + "/" + session_id
                async with client.post(session_path + "/webrtc", data="v=0\r\noffer") as response:
                    assert response.status == 200
                    assert await response.text() == "v=0\r\nanswer"
                async with client.post(session_path + "/webrtc", data="v=0" + "x" * 65536) as response:
                    assert response.status == 413
                async with client.put(session_path.replace("camera-1", "camera-2")) as response:
                    assert response.status == 404
                async with client.delete(session_path) as response:
                    assert response.status == 204
                async with client.put(session_path) as response:
                    assert response.status == 404
                assert seen[0] == ("camera-1", {"type": "camera.publish", "lease_seconds": 45})
    asyncio.run(scenario())
