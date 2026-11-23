"""Persistent, device-scoped gallery independent of device availability."""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
from pathlib import Path
import re
import time
from uuid import UUID, uuid4
from typing import Any

from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.application.module_images import ImageNormalizer

from aiohttp import web

from binbot_server.application.module_images import image_name, prepare_module_image


def register_gallery_routes(
    app: web.Application,
    store: SqliteWorkspaceStore,
    normalizer: ImageNormalizer,
    root: Path,
    maximum: int,
) -> None:
    locks: dict[str, asyncio.Lock] = {}

    def identifiers(request: web.Request) -> tuple[str, str]:
        device = request.match_info["device_id"]
        image = request.match_info.get("image_id", "")
        if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", device) or device in {".", ".."}:
            raise web.HTTPBadRequest(text="invalid device")
        if image and (not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", image) or image in {".", ".."}):
            raise web.HTTPBadRequest(text="invalid image")
        return device, image

    async def find(request: web.Request) -> tuple[str, str, dict[str, Any]]:
        device, image = identifiers(request)
        row = next((item for item in await store.gallery(device) if item["id"] == image), None)
        if row is None:
            raise web.HTTPNotFound(text="image not found")
        return device, image, row

    async def listing(request: web.Request) -> web.Response:
        device, _ = identifiers(request)
        return web.json_response(
            {"device_id": device, "images": await store.gallery(device)},
            headers={"Cache-Control": "no-store"},
        )

    async def upload(request: web.Request) -> web.Response:
        device, image = identifiers(request)
        if request.content_length and request.content_length > maximum:
            raise web.HTTPRequestEntityTooLarge(
                max_size=maximum, actual_size=request.content_length
            )
        content = await request.read()
        if not content or len(content) > maximum:
            raise web.HTTPBadRequest(text="image exceeds size limit")
        signature = hashlib.sha256(content).hexdigest()
        supported = content.startswith(
            (b"GIF87a", b"GIF89a", b"\x89PNG\r\n\x1a\n", b"\xff\xd8\xff")
        ) or (content.startswith(b"RIFF") and content[8:12] == b"WEBP")
        if not supported:
            raise web.HTTPBadRequest(text="unsupported image type")
        async with locks.setdefault(device, asyncio.Lock()):
            rows = await store.gallery(device)
            # A client-generated image ID makes network retries safe.
            existing = next((item for item in rows if item["id"] == image), None)
            if existing:
                if existing.get("sourceSha256") != signature:
                    raise web.HTTPConflict(text="image ID already used for different content")
                return web.json_response(existing)
            if len(rows) >= 200:
                raise web.HTTPConflict(text="gallery is full")
            name = image_name(request.query.get("name", "图片"))
            try:
                asset, directory = await asyncio.to_thread(
                    prepare_module_image, normalizer, root, content, name
                )
            except (ValueError, RuntimeError, OSError):
                raise web.HTTPBadRequest(text="image could not be processed") from None
            source = directory / "original"
            source.write_bytes(content)
            row: dict[str, Any] = {
                "id": image or str(uuid4()),
                "assetId": asset,
                "name": name,
                "createdAt": int(time.time() * 1000),
                "sourceSha256": signature,
            }
            await store.gallery_put(device, row["id"], row)
        return web.json_response(row, status=201)

    async def patch(request: web.Request) -> web.Response:
        device, image, row = await find(request)
        body = await request.json()
        if not isinstance(body, dict) or set(body) - {"name", "delivery", "push"}:
            raise web.HTTPBadRequest(text="invalid image metadata")
        async with locks.setdefault(device, asyncio.Lock()):
            _, _, row = await find(request)
            if "name" in body:
                if not isinstance(body["name"], str) or not body["name"].strip():
                    raise web.HTTPBadRequest(text="invalid image name")
                row["name"] = image_name(body["name"])
                manifest = root / str(UUID(row["assetId"])) / "manifest.json"
                if manifest.is_file():
                    document = json.loads(manifest.read_text())
                    document["name"] = row["name"]
                    temporary = manifest.with_suffix(".tmp")
                    temporary.write_text(json.dumps(document, ensure_ascii=False))
                    os.replace(temporary, manifest)
            # Delivery and OTA progress remain recoverable across browsers.
            for key in ("delivery", "push"):
                if key in body:
                    if not isinstance(body[key], dict) or len(str(body[key])) > 2048:
                        raise web.HTTPBadRequest(text="invalid transfer metadata")
                    row[key] = body[key]
            await store.gallery_put(device, image, row)
        return web.json_response(row)

    async def remove(request: web.Request) -> web.Response:
        device, image, _ = await find(request)
        await store.gallery_delete(device, image)
        # Keep transfer bundles used by active jobs or by a device's local gallery.
        return web.Response(status=204)

    async def source(request: web.Request) -> web.StreamResponse:
        _, _, row = await find(request)
        asset = str(UUID(row["assetId"]))
        path = root / asset / "original"
        if not path.is_file():
            path = root / asset / "round.gif"
        return web.FileResponse(path, headers={"Cache-Control": "private, max-age=86400"})

    base = "/api/v1/admin/devices/{device_id}/gallery"
    app.router.add_get(base, listing)
    app.router.add_put(base + "/{image_id}", upload)
    app.router.add_patch(base + "/{image_id}", patch)
    app.router.add_delete(base + "/{image_id}", remove)
    app.router.add_get(base + "/{image_id}/source", source)
