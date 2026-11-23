"""Authenticated device workspace endpoints; device effects require receipts."""

from __future__ import annotations
from typing import Any
from aiohttp import web
from binbot_server.application.workspace import WorkspaceService
from binbot_server.domain.errors import StateConflict


def register_workspace_routes(app: web.Application, service: WorkspaceService) -> None:
    def response(value: dict[str, Any]) -> web.Response:
        return web.json_response(value, headers={"Cache-Control": "no-store"})

    def options(request: web.Request) -> dict[str, Any]:
        return {
            "compact": request.query.get("compact") == "1",
            "wordbooks_revision": request.query.get("wordbooks_revision", ""),
        }

    async def snapshot(request: web.Request) -> web.Response:
        try:
            value = await service.snapshot(
                request.match_info["device_id"],
                **options(request),
            )
        except ValueError as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=400)
        return response(value)

    async def save(request: web.Request) -> web.Response:
        try:
            value = await service.save(
                request.match_info["device_id"],
                await request.json(),
                **options(request),
            )
        except StateConflict as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=409)
        except (ValueError, TypeError, KeyError) as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=400)
        return response(value)

    async def action(request: web.Request) -> web.Response:
        try:
            body: Any = await request.json()
            if (
                not isinstance(body, dict)
                or set(body) != {"name", "payload", "event_id"}
                or not isinstance(body["event_id"], str)
                or not 1 <= len(body["event_id"]) <= 96
            ):
                raise ValueError("设备操作请求不完整")
            device_id = service.validate_id(request.match_info["device_id"])
            command = await service.apply(
                device_id, body["name"], body["payload"], "action:" + body["event_id"]
            )
            return response(
                {
                    "command": command,
                    "workspace": await service.snapshot(
                        device_id,
                        **options(request),
                    ),
                }
            )
        except StateConflict as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=409)
        except (ValueError, TypeError, KeyError) as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=400)

    async def schedule(request: web.Request) -> web.Response:
        try:
            value = await service.create_schedule(
                request.match_info["device_id"],
                await request.json(),
                **options(request),
            )
        except StateConflict as exc:
            return web.json_response({"error": str(exc), "code": "CALENDAR_SYNC_ERROR"}, status=409)
        except (ValueError, TypeError, KeyError) as exc:
            return web.json_response({"error": str(exc), "code": "CALENDAR_SYNC_ERROR"}, status=400)
        return response(value)

    async def review(request: web.Request) -> web.Response:
        try:
            value = await service.review_word(
                request.match_info["device_id"],
                await request.json(),
                **options(request),
            )
        except StateConflict as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=409)
        except (ValueError, TypeError, KeyError) as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=400)
        return response(value)

    async def learning(request: web.Request) -> web.Response:
        try:
            return response(await service.study_batch(
                request.match_info["device_id"], request.query.get("book_id", ""),
            ))
        except StateConflict as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=409)
        except ValueError as exc:
            return web.json_response({"error": str(exc), "code": "WORKSPACE_ERROR"}, status=400)

    base = "/api/v1/admin/devices/{device_id}/workspace"
    app.router.add_get(base, snapshot)
    app.router.add_put(base, save)
    app.router.add_post(base + "/actions", action)
    app.router.add_post(base + "/schedules", schedule)
    app.router.add_post(base + "/reviews", review)
    app.router.add_get(base + "/learning", learning)
