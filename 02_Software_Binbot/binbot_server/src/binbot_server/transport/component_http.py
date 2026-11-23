from __future__ import annotations

import hashlib
import os
from collections.abc import Awaitable, Callable
from uuid import UUID, uuid4

from aiohttp import web
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from binbot_server.application.component_ota import ComponentOTAService, OTAError, Principal

SERVICE_KEY = web.AppKey("component_ota", ComponentOTAService)


class UploadBody(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    size_bytes: int = Field(ge=168)
    sha256: str = Field(pattern=r"^[0-9a-fA-F]{64}$")


class RegisterBody(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    upload_id: str


class StartBody(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    firmware_id: str
    idempotency_key: str = Field(min_length=1, max_length=128)
    allow_downgrade: bool = False
    wait_for_component: bool = True


def principal(request: web.Request) -> Principal:
    if request.get("admin_authenticated"):
        return Principal("admin", frozenset({
            "device:read", "firmware:read", "firmware:write", "component:ota:write",
        }))
    token = request.headers.get("Authorization", "").removeprefix("Bearer ")
    return request.app[SERVICE_KEY].principal(token)


def is_component_path(path: str) -> bool:
    return (path.startswith("/api/v1/admin/firmware") or path.startswith("/api/v1/admin/ota/")
            or "/components/" in path or path.startswith("/v1/device/firmware/"))


@web.middleware
async def component_errors(request: web.Request, handler: Callable[[web.Request], Awaitable[web.StreamResponse]]) -> web.StreamResponse:
    try:
        return await handler(request)
    except OTAError as exc:
        return web.json_response({"error": str(exc), "code": exc.code}, status=exc.status)
    except (ValueError, ValidationError) as exc:
        if not is_component_path(request.path):
            raise
        return web.json_response({"error": str(exc), "code": "INVALID_ARGUMENT"}, status=400)


async def component(request: web.Request) -> web.Response:
    fresh = request.query.get("fresh", "true")
    if fresh not in {"true", "false"}:
        raise ValueError("fresh must be true or false")
    return web.json_response(await request.app[SERVICE_KEY].component(
        principal(request), request.match_info["device_id"], request.match_info["component_id"], fresh == "true"))


async def prepare(request: web.Request) -> web.Response:
    p = principal(request)
    body = UploadBody.model_validate(await request.json())
    return web.json_response(request.app[SERVICE_KEY].prepare(p, body.size_bytes, body.sha256), status=201)


async def upload(request: web.Request) -> web.Response:
    service = request.app[SERVICE_KEY]
    upload_id = str(UUID(request.match_info["upload_id"]))
    row = service.begin_upload(upload_id, request.query.get("token", ""))
    path = service.directory / f"{upload_id}.upload"
    valid = False
    try:
        size, digest = 0, hashlib.sha256()
        with path.open("wb") as stream:
            async for chunk in request.content.iter_chunked(65536):
                size += len(chunk)
                if size > row["size"]:
                    raise OTAError("SIZE_MISMATCH", "upload exceeds declared size", 413)
                digest.update(chunk)
                stream.write(chunk)
            stream.flush()
            os.fsync(stream.fileno())
        if size != row["size"] or digest.hexdigest() != row["sha"]:
            raise OTAError("HASH_MISMATCH", "upload size or digest mismatch", 400)
        valid = True
        return web.json_response({"upload_id": upload_id, "status": "uploaded"})
    finally:
        service.finish_upload(upload_id, valid)
        if not valid:
            path.unlink(missing_ok=True)


async def register(request: web.Request) -> web.Response:
    p = principal(request)
    body = RegisterBody.model_validate(await request.json())
    return web.json_response(await request.app[SERVICE_KEY].register(p, str(UUID(body.upload_id))))


async def catalog(request: web.Request) -> web.Response:
    return web.json_response(request.app[SERVICE_KEY].list_firmware(
        principal(request), request.query.get("profile", ""), request.query.get("cursor", ""),
        int(request.query.get("limit", "50"))))


async def start(request: web.Request) -> web.Response:
    p = principal(request)
    body = StartBody.model_validate(await request.json())
    service = request.app[SERVICE_KEY]
    result = await service.start_job(p, request.match_info["device_id"], request.match_info["component_id"],
                                     str(UUID(body.firmware_id)), body.idempotency_key,
                                     body.allow_downgrade, body.wait_for_component)
    return web.json_response(service.public_job(result), status=202)


async def job(request: web.Request) -> web.Response:
    service = request.app[SERVICE_KEY]
    return web.json_response(service.public_job(service.job(principal(request), request.match_info["job_id"])))


async def jobs(request: web.Request) -> web.Response:
    return web.json_response(request.app[SERVICE_KEY].list_jobs(
        principal(request), request.query.get("device_id", ""), int(request.query.get("limit", "30"))))


async def cancel(request: web.Request) -> web.Response:
    return web.json_response(request.app[SERVICE_KEY].cancel(principal(request), request.match_info["job_id"]))


async def download(request: web.Request) -> web.FileResponse:
    path = request.app[SERVICE_KEY].download(request.match_info["firmware_id"],
                                           request.query.get("job_id", ""), request.query.get("token", ""))
    return web.FileResponse(path, headers={"Cache-Control": "no-store", "Content-Type": "application/octet-stream"})


async def upload_component_gif(request: web.Request) -> web.Response:
    service = request.app[SERVICE_KEY]
    component = request.match_info.get("component_id", "component-1")
    data = await request.read()
    idem = request.headers.get("Idempotency-Key", "") or f"web-{uuid4().hex}"
    job = await service.start_file_job(
        principal(request), request.match_info["device_id"], component, request.query.get("name", ""), data, idem
    )
    return web.json_response(job, status=202)


async def restore_gallery(request: web.Request) -> web.Response:
    return web.json_response(await request.app[SERVICE_KEY].restore_file_gallery(
        principal(request), str(UUID(request.match_info["job_id"]))), status=202)


async def download_component_asset(request: web.Request) -> web.FileResponse:
    path = request.app[SERVICE_KEY].download_asset(
        request.match_info["asset_id"], request.query.get("job_id", ""),
        request.query.get("token", ""))
    return web.FileResponse(path, headers={"Cache-Control": "no-store", "Content-Type": "image/gif"})


def register_routes(app: web.Application, service: ComponentOTAService) -> None:
    app[SERVICE_KEY] = service
    app.router.add_get("/api/v1/admin/devices/{device_id}/components/{component_id}", component)
    app.router.add_post("/api/v1/admin/firmware/uploads", prepare)
    app.router.add_put("/api/v1/admin/firmware/uploads/{upload_id}", upload)
    app.router.add_post("/api/v1/admin/firmware", register)
    app.router.add_get("/api/v1/admin/firmware", catalog)
    app.router.add_post("/api/v1/admin/devices/{device_id}/components/{component_id}/ota", start)
    app.router.add_get("/api/v1/admin/ota/jobs/{job_id}", job)
    app.router.add_get("/api/v1/admin/ota/jobs", jobs)
    app.router.add_post("/api/v1/admin/ota/jobs/{job_id}/cancel", cancel)
    app.router.add_post("/api/v1/admin/ota/jobs/{job_id}/gallery", restore_gallery)
    app.router.add_get("/v1/device/firmware/{firmware_id}", download)
    app.router.add_put("/api/v1/admin/devices/{device_id}/component-gif", upload_component_gif)
    app.router.add_put("/api/v1/admin/devices/{device_id}/components/{component_id}/gif", upload_component_gif)
    app.router.add_get("/v1/device/component-assets/{asset_id}", download_component_asset)
