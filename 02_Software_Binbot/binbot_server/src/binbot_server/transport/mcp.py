from __future__ import annotations

import argparse
import hmac
import os
from typing import Any, cast
from collections.abc import Sequence
from urllib.parse import quote, urlsplit

import httpx
from mcp.server.fastmcp import FastMCP
from mcp.server.transport_security import TransportSecuritySettings
from mcp.types import ToolAnnotations
from mcp.types import ContentBlock, Tool
from jsonschema import Draft202012Validator


class StrictFastMCP(FastMCP):
    async def list_tools(self) -> list[Tool]:
        tools = await super().list_tools()
        for tool in tools:
            tool.inputSchema["additionalProperties"] = False
        return tools

    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Sequence[ContentBlock] | dict[str, Any]:
        tool = next((tool for tool in await self.list_tools() if tool.name == name), None)
        if tool is None:
            raise ValueError("unknown tool")
        Draft202012Validator(tool.inputSchema).validate(arguments)
        return await super().call_tool(name, arguments)


def create_mcp(server_url: str, service_token: str, public_origin: str = "") -> FastMCP:
    if not service_token:
        raise ValueError("BINBOT_MCP_SERVICE_TOKEN is required")
    allowed_hosts = ["127.0.0.1:*", "localhost:*", "[::1]:*"]
    allowed_origins = ["http://127.0.0.1:*", "http://localhost:*", "http://[::1]:*"]
    if public_origin:
        origin = urlsplit(public_origin)
        if origin.scheme not in {"http", "https"} or not origin.netloc or origin.username:
            raise ValueError("invalid MCP public origin")
        allowed_hosts.append(origin.netloc)
        allowed_origins.append(public_origin.rstrip("/"))
    mcp = StrictFastMCP("BinBot", stateless_http=True, json_response=True,
                  transport_security=TransportSecuritySettings(
                      enable_dns_rebinding_protection=True, allowed_hosts=allowed_hosts,
                      allowed_origins=allowed_origins))
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, idempotentHint=True)
    write = ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=True)

    async def call(method: str, path: str, **kwargs: Any) -> dict[str, Any]:
        async with httpx.AsyncClient(base_url=server_url.rstrip("/"), timeout=10,
                                     headers={"Authorization": f"Bearer {service_token}"}) as client:
            response = await client.request(method, path, **kwargs)
            data = cast(dict[str, Any], response.json())
            if response.is_error:
                raise ValueError(f'{data.get("code", "HTTP_ERROR")}: {data.get("error", "request rejected")}')
            return data

    def component_path(device_id: str, component_id: str) -> str:
        return f"/api/v1/admin/devices/{quote(device_id, safe='')}/components/{quote(component_id, safe='')}"

    @mcp.tool(annotations=read)
    async def binbot_list_devices(online_only: bool = False) -> dict[str, Any]:
        """List known devices and their negotiated capabilities."""
        data = await call("GET", "/api/v1/admin/devices")
        if online_only:
            data["devices"] = [d for d in data["devices"] if d["online"]]
        return data

    @mcp.tool(annotations=read)
    async def binbot_get_component(device_id: str, component_id: str,
                                   fresh: bool = True) -> dict[str, Any]:
        """Probe UART component (up to five seconds), or explicitly read its cached snapshot."""
        return await call("GET", component_path(device_id, component_id), params={"fresh": str(fresh).lower()})

    @mcp.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False))
    async def binbot_prepare_firmware_upload(size_bytes: int, sha256: str) -> dict[str, Any]:
        """Prepare a bounded HTTP PUT upload; firmware bytes stay outside MCP."""
        return await call("POST", "/api/v1/admin/firmware/uploads", json={"size_bytes": size_bytes, "sha256": sha256})

    @mcp.tool(annotations=write)
    async def binbot_register_firmware(upload_id: str) -> dict[str, Any]:
        """Validate uploaded ESP32-C3 image and register immutable firmware metadata."""
        return await call("POST", "/api/v1/admin/firmware", json={"upload_id": upload_id})

    @mcp.tool(annotations=read)
    async def binbot_list_firmware(profile: str | None = None, cursor: str | None = None,
                                   limit: int = 50) -> dict[str, Any]:
        """List verified firmware, optionally filtered by component profile."""
        return await call("GET", "/api/v1/admin/firmware", params={"profile": profile or "", "cursor": cursor or "", "limit": limit})

    @mcp.tool(annotations=write)
    async def binbot_start_component_ota(device_id: str, component_id: str, firmware_id: str,
                                         idempotency_key: str, allow_downgrade: bool = False,
                                         wait_for_component: bool = True) -> dict[str, Any]:
        """Queue OTA until the matching component connects. Reuse the same key when retrying."""
        return await call("POST", component_path(device_id, component_id) + "/ota",
                          json={"firmware_id": firmware_id, "idempotency_key": idempotency_key,
                                "allow_downgrade": allow_downgrade,
                                "wait_for_component": wait_for_component})

    @mcp.tool(annotations=read)
    async def binbot_get_ota_job(job_id: str) -> dict[str, Any]:
        """Read durable OTA progress and its confirmed result."""
        return await call("GET", f"/api/v1/admin/ota/jobs/{quote(job_id, safe='')}")

    @mcp.tool(annotations=write)
    async def binbot_cancel_ota_job(job_id: str) -> dict[str, Any]:
        """Request cancellation before activation. Poll the job for confirmed cancellation."""
        return await call("POST", f"/api/v1/admin/ota/jobs/{quote(job_id, safe='')}/cancel")

    return mcp


class AuthenticatedMCP:
    def __init__(self, app: Any, token: str):
        if not token:
            raise ValueError("BINBOT_MCP_TOKEN is required")
        self.app, self.token = app, token

    async def __call__(self, scope: Any, receive: Any, send: Any) -> None:
        if scope["type"] == "http":
            supplied = dict(scope["headers"]).get(b"authorization", b"").decode("latin1")
            if not hmac.compare_digest(supplied, f"Bearer {self.token}"):
                await send({"type": "http.response.start", "status": 401,
                            "headers": [(b"content-type", b"application/json"), (b"www-authenticate", b"Bearer")]})
                await send({"type": "http.response.body", "body": b'{"error":"MCP credential required"}'})
                return
        await self.app(scope, receive, send)


def main() -> None:
    import uvicorn
    parser = argparse.ArgumentParser(description="BinBot MCP Streamable HTTP adapter")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--server-url", default="http://127.0.0.1:8765")
    parser.add_argument("--public-origin", default="")
    args = parser.parse_args()
    mcp = create_mcp(args.server_url, os.environ.get("BINBOT_MCP_SERVICE_TOKEN", ""), args.public_origin)
    app = AuthenticatedMCP(mcp.streamable_http_app(), os.environ.get("BINBOT_MCP_TOKEN", ""))
    uvicorn.run(app, host=args.host, port=args.port, access_log=False)


if __name__ == "__main__":
    main()
