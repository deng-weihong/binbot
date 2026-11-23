from __future__ import annotations

import asyncio
import contextlib
import json
import os
from collections.abc import AsyncIterator, Awaitable, Callable
from typing import Any

from aiohttp import ClientError, ClientSession, ClientTimeout, StreamReader, web

from binbot_server.application.camera import CameraSessions
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import CameraConfig
from binbot_server.domain.errors import StateConflict


async def _read_limited(stream: StreamReader) -> bytes:
    data = bytearray()
    async for chunk in stream.iter_chunked(16384):
        data.extend(chunk)
        if len(data) > 65536:
            raise ValueError("SDP exceeds size limit")
    return bytes(data)


def register_camera_routes(
    app: web.Application,
    config: CameraConfig,
    registry: DeviceSessionRegistry,
    publisher: Callable[[str, str, dict[str, Any], str], Awaitable[dict[str, Any]]] | None = None,
) -> None:
    sessions = CameraSessions(config, registry)
    sessions.publisher = publisher
    client: ClientSession | None = None
    token = ""

    async def lifecycle(_app: web.Application) -> AsyncIterator[None]:
        nonlocal client, token
        if not config.enabled:
            yield
            return
        token = os.environ.get(config.viewer_token_env, "").strip()
        if len(token) < 32:
            raise ValueError(f"{config.viewer_token_env} is required for cloud camera playback")
        client = ClientSession(timeout=ClientTimeout(total=20))

        async def worker() -> None:
            while True:
                await sessions.tick()
                await asyncio.sleep(2)

        task = asyncio.create_task(worker(), name="camera-publish-leases")
        try:
            yield
        finally:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
            await sessions.close()
            await client.close()

    def available() -> None:
        if not config.enabled or client is None:
            raise web.HTTPServiceUnavailable(text="云端摄像头未启用")

    async def open_session(request: web.Request) -> web.Response:
        available()
        try:
            session_id = await sessions.open(request.match_info["device_id"])
        except StateConflict as exc:
            return web.json_response({"error": str(exc)}, status=409)
        return web.json_response(
            {"session_id": session_id, "heartbeat_seconds": 10},
            headers={"Cache-Control": "no-store"},
            status=201,
        )

    async def refresh_session(request: web.Request) -> web.Response:
        available()
        try:
            await sessions.refresh(
                request.match_info["device_id"], request.match_info["session_id"]
            )
        except KeyError:
            return web.json_response({"error": "观看会话已过期"}, status=404)
        except StateConflict as exc:
            return web.json_response({"error": str(exc)}, status=409)
        return web.json_response({"status": "ok"}, headers={"Cache-Control": "no-store"})

    async def release_session(request: web.Request) -> web.Response:
        await sessions.release(request.match_info["device_id"], request.match_info["session_id"])
        return web.Response(status=204)

    async def negotiate(request: web.Request) -> web.Response:
        available()
        device_id = request.match_info["device_id"]
        try:
            await sessions.refresh(device_id, request.match_info["session_id"])
        except KeyError:
            return web.json_response({"error": "观看会话已过期"}, status=404)
        except StateConflict as exc:
            return web.json_response({"error": str(exc)}, status=409)
        if request.content_length is not None and request.content_length > 65536:
            raise web.HTTPRequestEntityTooLarge(max_size=65536, actual_size=request.content_length)
        try:
            offer_bytes = await _read_limited(request.content)
        except ValueError:
            raise web.HTTPRequestEntityTooLarge(max_size=65536, actual_size=65537) from None
        try:
            offer = offer_bytes.decode("utf-8")
        except UnicodeDecodeError:
            raise web.HTTPBadRequest(text="无效的视频协商请求") from None
        if not offer.startswith("v=0"):
            raise web.HTTPBadRequest(text="无效的视频协商请求")
        assert client is not None
        try:
            async with client.post(
                config.api_url.rstrip("/") + "/index/api/webrtc",
                params={"app": "live", "stream": device_id, "type": "play", "token": token},
                data=offer,
                headers={"Content-Type": "application/sdp"},
                allow_redirects=False,
            ) as response:
                data = await _read_limited(response.content)
                if response.status != 200:
                    raise ValueError("media response rejected")
                answer = json.loads(data)
                if (
                    not isinstance(answer, dict)
                    or answer.get("code") != 0
                    or not isinstance(answer.get("sdp"), str)
                ):
                    raise ValueError("stream unavailable")
        except (ClientError, TimeoutError, ValueError):
            return web.json_response({"error": "摄像头连接暂不可用，请重试"}, status=502)
        # Only SDP crosses the admin boundary; media credentials stay on the server.
        return web.Response(
            text=answer["sdp"],
            content_type="application/sdp",
            headers={"Cache-Control": "no-store"},
        )

    path = "/api/v1/admin/devices/{device_id}/camera/sessions"
    app.router.add_post(path, open_session)
    app.router.add_put(path + "/{session_id}", refresh_session)
    app.router.add_delete(path + "/{session_id}", release_session)
    app.router.add_post(path + "/{session_id}/webrtc", negotiate)
    app.cleanup_ctx.append(lifecycle)
