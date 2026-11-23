from __future__ import annotations

import asyncio
import base64
import binascii
import ipaddress
import hmac
import logging
import os
import shutil
import subprocess
import time
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, cast
from uuid import UUID, uuid4
from urllib.parse import urlsplit

from aiohttp import ClientError, WSMsgType, WSCloseCode, web

from binbot_server import __version__
from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.calendar import CalendarEvent, CalendarEventDraft, CalendarSyncService
from binbot_server.application.device_control import DeviceControlStore, offline_device_snapshot
from binbot_server.application.feishu import FeishuError, FeishuGateway
from binbot_server.application.tasks import TaskWorker
from binbot_server.application.reports import ActivityJournal, DailyReportService, DailyReportWorker
from binbot_server.application.web import WebAgentError, WebAgentGateway
from binbot_server.application.ports import MemoryStore, TaskStore
from binbot_server.application.tools import CommandDispatchError, DeviceCommandService, ToolRegistry
from binbot_server.application.ports import (
    ConversationStore,
    EventSink,
    OutboundAudioFrame,
    SpeechEndpointDetector,
    ProviderBundle, AgentModel,
)
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.session_actor import SessionActor
from binbot_server.application.realtime_session import RealtimeSessionActor
from binbot_server.config import Settings
from binbot_server.domain.errors import (
    AuthenticationFailed,
    BinBotError,
    ProtocolViolation,
    ProviderFailure,
    StateConflict,
)
from binbot_server.domain.model import AudioFormat, ChatMessage, TurnContext, VoiceSession
from binbot_server.infrastructure.audio import OpusDecoder, OpusEncoder
from binbot_server.infrastructure.feishu import FeishuLongConnection, FeishuOAuthManager
from binbot_server.infrastructure.device_feishu import DeviceFeishuError, DeviceFeishuManager
from binbot_server.infrastructure.security import DeviceAuthenticator
from binbot_server.infrastructure import provider_settings

from .protocol import (
    ComponentEventMessage,
    AgentResponseMessage,
    AudioEndMessage,
    AudioFrame,
    AudioKind,
    DeviceCommandResultMessage,
    DeviceStateMessage,
    HelloMessage,
    PingMessage,
    TextInputMessage,
    TurnCancelMessage,
    TurnStartMessage,
    decode_audio_frame,
    encode_audio_frame,
    parse_control,
    parse_hello,
)
from .component_http import component_errors, is_component_path, register_routes
from .weather_http import register_weather_routes
from .camera_http import register_camera_routes
from .gallery_http import register_gallery_routes
from .workspace_http import register_workspace_routes
from .mqtt import MQTTControlRuntime
from binbot_server.application.device_commands import WORKSPACE_COMMANDS, validate_device_payload
from binbot_server.application.workspace import WorkspaceService
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.application.component_ota import ComponentOTAService
from binbot_server.application.module_images import image_name, prepare_module_image

_LOG = logging.getLogger("binbot.gateway")


@dataclass(slots=True)
class GatewayContext:
    settings: Settings
    providers: ProviderBundle
    registry: DeviceSessionRegistry
    authenticator: DeviceAuthenticator
    conversations: ConversationStore
    controls: DeviceControlStore
    command_service: DeviceCommandService
    tools: ToolRegistry
    agent: AgentRuntime
    web_agent: WebAgentGateway
    memories: MemoryStore
    tasks: TaskStore
    feishu: FeishuGateway | None
    feishu_oauth: FeishuOAuthManager | None
    feishu_connection: FeishuLongConnection | None
    calendar: CalendarSyncService | None
    worker: TaskWorker
    reports: DailyReportService
    report_worker: DailyReportWorker
    journal: ActivityJournal
    component_ota: ComponentOTAService
    device_feishu: DeviceFeishuManager | None = None
    workspace_store: SqliteWorkspaceStore | None = None
    workspace: WorkspaceService | None = None
    mqtt: MQTTControlRuntime | None = None
    endpoint_factory: Callable[[], SpeechEndpointDetector] | None = None
    configure_providers: Callable[[Settings], ProviderBundle] | None = None
    provider_lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    retired_providers: list[ProviderBundle] = field(default_factory=list)


_CONTEXT_KEY = web.AppKey("gateway_context", GatewayContext)
_WEBSOCKETS_KEY = web.AppKey(
    "active_websockets", set[web.WebSocketResponse]
)
_ADMIN_COOKIE = "binbot_admin"


def _admin_password(settings: Settings) -> str:
    # BINBOT_ADMIN_TOKEN is retained only as a migration fallback. New
    # deployments should set BINBOT_ADMIN_USERNAME and BINBOT_ADMIN_PASSWORD.
    return os.environ.get(settings.admin.password_env, "") or os.environ.get(
        settings.admin.token_env, ""
    )


def _admin_username(settings: Settings) -> str:
    return os.environ.get(settings.admin.username_env, "admin").strip() or "admin"


def _session_cookie(settings: Settings, username: str, remember: bool) -> str:
    expires = int(time.time()) + settings.admin.remember_days * 86400 if remember else 0
    payload = f"{username}\n{expires}".encode("utf-8")
    encoded = base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")
    signature = hmac.new(
        _admin_password(settings).encode("utf-8"), encoded.encode("ascii"), "sha256"
    ).hexdigest()
    return f"{encoded}.{signature}"


def _valid_session_cookie(settings: Settings, value: str) -> bool:
    if not value or "." not in value:
        return False
    encoded, signature = value.rsplit(".", 1)
    password = _admin_password(settings)
    if not password or not hmac.compare_digest(
        signature,
        hmac.new(password.encode("utf-8"), encoded.encode("ascii"), "sha256").hexdigest(),
    ):
        return False
    try:
        padded = encoded + "=" * (-len(encoded) % 4)
        username, expires_text = base64.urlsafe_b64decode(padded).decode("utf-8").split("\n", 1)
        expires = int(expires_text)
    except (ValueError, UnicodeDecodeError, binascii.Error):
        return False
    return bool(username and username == _admin_username(settings) and (not expires or expires > int(time.time())))


@web.middleware
async def admin_authentication(
    request: web.Request,
    handler: Callable[[web.Request], Awaitable[web.StreamResponse]],
) -> web.StreamResponse:
    # The Web Console uses admin sessions; machine clients retain scoped tokens.
    if is_component_path(request.path):
        is_web_component_upload = (
            request.path.startswith("/api/v1/admin/devices/") and
            request.path.endswith("/gif")
        )
        is_web_component_job = request.path.startswith("/api/v1/admin/ota/jobs/")
        if request.path.startswith("/api/v1/admin/"):
            context = request.app[_CONTEXT_KEY]
            if (is_web_component_upload or is_web_component_job) and \
                    context.settings.admin.allow_unauthenticated_loopback and request.remote:
                try:
                    if ipaddress.ip_address(request.remote).is_loopback:
                        request["admin_authenticated"] = True
                        return await handler(request)
                except ValueError:
                    pass
            cookie = request.cookies.get(_ADMIN_COOKIE, "")
            authorization = request.headers.get("Authorization", "")
            supplied = authorization[7:] if authorization.startswith("Bearer ") else ""
            if _valid_session_cookie(context.settings, cookie) or (
                _admin_password(context.settings) and
                hmac.compare_digest(_admin_password(context.settings), supplied)
            ):
                request["admin_authenticated"] = True
                return await handler(request)
        return await handler(request)
    if request.path == "/api/v1/admin/devices" and bool(request.headers.get("Authorization")):
        context = request.app[_CONTEXT_KEY]
        token = request.headers.get("Authorization", "").removeprefix("Bearer ")
        if token != _admin_password(context.settings):
            p = context.component_ota.principal(token)
            p.require("device:read")
            request["component_principal"] = p
            return await handler(request)
    protected = request.path.startswith("/api/v1/admin/") or request.path.startswith(
        "/api/v1/agent/"
    )
    if not protected or request.path in {"/api/v1/admin/session", "/api/v1/admin/logout"}:
        return await handler(request)
    context = request.app[_CONTEXT_KEY]
    admin = context.settings.admin
    if admin.allow_unauthenticated_loopback and request.remote:
        try:
            if ipaddress.ip_address(request.remote).is_loopback:
                request["admin_authenticated"] = True
                return await handler(request)
        except ValueError:
            pass
    cookie = request.cookies.get(_ADMIN_COOKIE, "")
    # Bearer support is migration-only; the console now uses signed cookies.
    authorization = request.headers.get("Authorization", "")
    supplied = authorization[7:] if authorization.startswith("Bearer ") else ""
    legacy_password = _admin_password(context.settings)
    if _valid_session_cookie(context.settings, cookie) or (
        legacy_password and hmac.compare_digest(legacy_password, supplied)
    ):
        request["admin_authenticated"] = True
        return await handler(request)
    return web.json_response(
        {"error": "administrator credentials were rejected"},
        status=401,
    )


async def admin_session(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if request.method == "GET":
        # Browser session restoration must check the cookie itself, even when
        # local development requests are allowed through the legacy exemption.
        if not _valid_session_cookie(context.settings, request.cookies.get(_ADMIN_COOKIE, "")):
            return web.json_response(
                {"error": "administrator session required"}, status=401,
                headers={"Cache-Control": "no-store"},
            )
        return web.json_response(
            {"authenticated": True, "username": _admin_username(context.settings)},
            headers={"Cache-Control": "no-store"},
        )
    try:
        value = await request.json()
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "invalid JSON body")
    if not isinstance(value, dict):
        return _error(400, "invalid credentials")
    username = value.get("username")
    password = value.get("password")
    # Accept the old token body during migration; it is not used by the UI.
    if isinstance(value.get("token"), str) and not username and not password:
        username, password = _admin_username(context.settings), value["token"]
    expected_username = _admin_username(context.settings)
    expected_password = _admin_password(context.settings)
    if (
        not expected_password
        or not isinstance(username, str)
        or not isinstance(password, str)
        or not hmac.compare_digest(expected_username, username.strip())
        or not hmac.compare_digest(expected_password, password)
    ):
        return _error(401, "administrator credentials were rejected")
    remember = bool(value.get("remember", False))
    response = web.json_response({"status": "ok"}, headers={"Cache-Control": "no-store"})
    response.set_cookie(
        _ADMIN_COOKIE,
        _session_cookie(context.settings, expected_username, remember),
        max_age=context.settings.admin.remember_days * 86400 if remember else None,
        httponly=True,
        secure=request.secure,
        samesite="Strict",
        path="/api/v1/",
    )
    return response


async def admin_logout(request: web.Request) -> web.Response:
    response = web.json_response({"status": "ok"}, headers={"Cache-Control": "no-store"})
    response.del_cookie(_ADMIN_COOKIE, path="/api/v1/")
    return response


class WebSocketEventSink(EventSink):
    def __init__(self, websocket: web.WebSocketResponse):
        self._websocket = websocket
        self._lock = asyncio.Lock()

    async def send_control(self, event: dict[str, object]) -> None:
        async with self._lock:
            if not self._websocket.closed:
                await self._websocket.send_json(event)

    async def send_audio(self, frame: OutboundAudioFrame) -> None:
        packet = encode_audio_frame(
            AudioFrame(
                kind=AudioKind.DOWNLINK_OPUS,
                stream_id=UUID(frame.stream_id),
                sequence=frame.sequence,
                timestamp_ms=frame.timestamp_ms,
                payload=frame.payload,
            )
        )
        async with self._lock:
            if not self._websocket.closed:
                await self._websocket.send_bytes(packet)


def _audio_format(spec: object) -> AudioFormat:
    return AudioFormat(
        codec=getattr(spec, "codec"),
        sample_rate=getattr(spec, "sample_rate"),
        channels=getattr(spec, "channels"),
        frame_ms=getattr(spec, "frame_ms"),
    )


def _validate_audio_negotiation(context: GatewayContext, hello: HelloMessage) -> None:
    audio = context.settings.audio
    expected_uplink = (audio.uplink_sample_rate, audio.uplink_frame_ms)
    actual_uplink = (hello.uplink.sample_rate, hello.uplink.frame_ms)
    expected_downlink = (audio.downlink_sample_rate, audio.downlink_frame_ms)
    actual_downlink = (hello.downlink.sample_rate, hello.downlink.frame_ms)
    if hello.uplink.codec != "opus" or actual_uplink != expected_uplink:
        raise ProtocolViolation(
            "unsupported uplink format; server requires "
            f"opus/{expected_uplink[0]}/{expected_uplink[1]}ms"
        )
    if hello.downlink.codec != "opus" or actual_downlink != expected_downlink:
        raise ProtocolViolation(
            "unsupported downlink format; server requires "
            f"opus/{expected_downlink[0]}/{expected_downlink[1]}ms"
        )


async def _send_error(
    websocket: web.WebSocketResponse,
    error: Exception,
    *,
    retryable: bool = False,
) -> None:
    code = getattr(error, "code", "internal_error")
    if not websocket.closed:
        await websocket.send_json(
            {
                "type": "error",
                "code": code,
                "message": str(error),
                "retryable": retryable,
            }
        )


async def health(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    calendar_status = await context.calendar.status() if context.calendar is not None else None
    return web.json_response(
        {
            "status": "ok",
            "service": "binbot-voice-server",
            "version": __version__,
            "connected_devices": await context.registry.count(),
            "feishu": {
                "enabled": context.feishu is not None or bool(context.device_feishu and context.device_feishu.documents),
                "scope": "device",
                "connected": (
                    context.feishu_connection is not None
                    and context.feishu_connection.running
                ),
                "account": (
                    context.feishu_oauth.status()
                    if context.feishu_oauth is not None
                    else {
                        "authorized": False,
                        "display_name": "",
                        "open_id": "",
                        "tenant_key": "",
                        "expires_at": "",
                        "scopes": [],
                    }
                ),
                "calendar": calendar_status,
            },
            "task_worker": context.worker.running,
            "daily_report_worker": context.report_worker.running,
        }
    )


async def feishu_events(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if context.device_feishu is not None and context.feishu is None:
        try:
            return web.json_response(await context.device_feishu.webhook(await request.json()))
        except DeviceFeishuError as exc:
            return _error(exc.status, str(exc))
        except (ValueError, TypeError, web.HTTPException):
            return _error(400, "飞书事件格式无效。")
        except Exception:
            _LOG.exception("device Feishu webhook failed")
            return _error(502, "飞书事件处理失败。")
    if context.feishu is None:
        return _error(404, "Feishu integration is disabled")
    try:
        payload = await request.json()
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "invalid JSON body")
    if not isinstance(payload, dict):
        return _error(400, "JSON body must be an object")
    if payload.get("type") == "url_verification":
        token = payload.get("token")
        expected = context.settings.feishu.verification_token()
        if not expected or not isinstance(token, str) or not hmac.compare_digest(expected, token):
            return _error(401, "Feishu verification token was rejected")
        challenge = payload.get("challenge")
        if not isinstance(challenge, str) or not challenge:
            return _error(400, "challenge is required")
        return web.json_response({"challenge": challenge})
    expected = context.settings.feishu.verification_token()
    header = payload.get("header")
    supplied = header.get("token") if isinstance(header, dict) else None
    if expected and (not isinstance(supplied, str) or not hmac.compare_digest(expected, supplied)):
        return _error(401, "Feishu event token was rejected")
    if (
        context.calendar is not None
        and isinstance(header, dict)
        and header.get("event_type") == "calendar.calendar.event.changed_v4"
    ):
        await context.calendar.handle_event(payload)
        return web.json_response({"status": "ok", "calendar": "synchronized"})
    try:
        result = await context.feishu.handle(payload)
    except FeishuError as exc:
        return _error(400, str(exc))
    except Exception:
        _LOG.exception("Feishu event handling failed")
        return _error(502, "Feishu event handling failed")
    return web.json_response(result)


async def admin_devices(request: web.Request) -> web.Response:
    """Keep known devices visible independently of their current connection."""
    context: GatewayContext = request.app[_CONTEXT_KEY]
    devices = {str(item["id"]): item for item in await context.controls.known_devices()}
    for device_id in context.settings.auth.tokens:
        devices.setdefault(device_id, offline_device_snapshot(device_id))
    for conversation in await context.conversations.list_conversations(limit=1000):
        device_id = conversation.conversation_key
        if not device_id.startswith(("web:", "feishu:")):
            devices.setdefault(device_id, offline_device_snapshot(
                device_id, {"last_seen_at": conversation.updated_at}
            ))
    for item in await context.registry.snapshot():
        devices[str(item["id"])] = item
    if context.workspace is not None:
        for device_id, item in devices.items():
            profile = (await context.workspace.document(device_id))["data"]
            item.update(name=profile["deviceName"], location=profile["location"], timezone=profile["timezone"])
    p = request.get("component_principal")
    if p is not None and p.device_ids:
        devices = {k: v for k, v in devices.items() if k in p.device_ids}
    return web.json_response({
        "devices": sorted(devices.values(), key=lambda item: (not item["online"], str(item["id"])))
    })


async def admin_providers(request: web.Request) -> web.Response:
    """Return provider configuration without exposing secret values."""
    context: GatewayContext = request.app[_CONTEXT_KEY]
    selected = context.settings.providers
    openai = selected.openai
    volcengine = selected.volcengine
    asr_api_key_env = volcengine.asr_api_key_env or volcengine.api_key_env
    tts_api_key_env = volcengine.tts_api_key_env or volcengine.api_key_env
    if selected.asr == "volcengine":
        asr = {
            "provider": selected.asr,
            "endpoint": volcengine.asr_endpoint,
            "model": volcengine.asr_resource_id,
            "api_key_configured": bool(os.environ.get(asr_api_key_env)),
        }
    elif selected.asr == "openai":
        asr = {
            "provider": selected.asr,
            "endpoint": openai.base_url,
            "model": openai.asr_model,
            "api_key_configured": bool(os.environ.get(openai.api_key_env)),
        }
    else:
        asr = {
            "provider": selected.asr,
            "endpoint": None,
            "model": "mock",
            "api_key_configured": False,
        }
    if selected.tts == "volcengine":
        tts = {
            "provider": selected.tts,
            "endpoint": volcengine.tts_endpoint,
            "model": volcengine.tts_model,
            "voice": volcengine.tts_speaker,
            "api_key_configured": bool(os.environ.get(tts_api_key_env)),
        }
    elif selected.tts == "openai":
        tts = {
            "provider": selected.tts,
            "endpoint": openai.base_url,
            "model": openai.tts_model,
            "voice": openai.tts_voice,
            "api_key_configured": bool(os.environ.get(openai.api_key_env)),
        }
    else:
        tts = {
            "provider": selected.tts,
            "endpoint": None,
            "model": "mock",
            "voice": "",
            "api_key_configured": False,
        }
    return web.json_response(
        {
            "asr": asr,
            "llm": {
                "provider": selected.llm,
                "model": openai.llm_model if selected.llm == "openai" else "mock",
                "base_url": openai.base_url if selected.llm == "openai" else None,
                "api": openai.llm_api if selected.llm == "openai" else None,
                "thinking": openai.llm_thinking if selected.llm == "openai" else None,
                "api_key_configured": bool(os.environ.get(openai.api_key_env))
                if selected.llm == "openai"
                else True,
            },
            "tts": tts,
            "voice": {
                "provider": selected.voice,
                "endpoint": selected.seeduplex.endpoint if selected.voice == "seeduplex" else None,
                "model": selected.seeduplex.model if selected.voice == "seeduplex" else None,
                "voice": selected.seeduplex.voice if selected.voice == "seeduplex" else None,
                "api_key_configured": bool(os.environ.get(selected.seeduplex.api_key_env)),
            },
            "feishu": {
                "enabled": False,
                "scope": "device",
                "app_id": "",
            },
        }
    )


async def admin_provider_configuration(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    return web.json_response(provider_settings.snapshot(context.settings), headers={"Cache-Control": "no-store"})


async def save_provider_configuration(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if context.configure_providers is None:
        return _error(503, "provider reconfiguration is unavailable")
    async with context.provider_lock:
        previous_env: dict[str, str | None] = {}
        bundle: ProviderBundle | None = None
        try:
            body = await request.json()
            if not isinstance(body, dict) or set(body) - {"revision", "config", "secrets"}:
                raise ValueError("invalid provider configuration request")
            previous = provider_settings.read_document(context.settings)
            if body.get("revision") != previous["revision"]:
                return _error(409, "配置已被其他窗口修改，请重新加载后保存")
            config_values = body.get("config")
            if not isinstance(config_values, dict):
                raise ValueError("provider config must be an object")
            updated = provider_settings.validate_values(context.settings, config_values)
            new_secrets = body.get("secrets", {})
            provider_settings.validate_secrets(new_secrets)
            secrets = {**previous["secrets"], **new_secrets}
            names = provider_settings.secret_names(updated)
            previous_env = {name: os.environ.get(name) for name in names.values()}
            provider_settings.apply_secrets(updated, secrets)
            bundle = context.configure_providers(updated)
            if bundle.realtime is not None and (
                context.settings.providers.voice != updated.providers.voice
                or context.settings.providers.seeduplex != updated.providers.seeduplex
                or "seeduplex" in new_secrets
            ):
                probe = await bundle.realtime.open_session(
                    TurnContext(uuid4(), uuid4(), "configuration-test"), (), ()
                )
                await probe.close()
            provider_settings.persist(updated, provider_settings.provider_values(updated), secrets)
        except (ValueError, TypeError, ProviderFailure, TimeoutError, ClientError) as exc:
            for name, value in previous_env.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
            if bundle is not None:
                await bundle.close()
            return _error(400, str(exc) or "实时语音服务连接超时，请检查服务开通状态和 API Key")
        except OSError:
            for name, value in previous_env.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value
            if bundle is not None:
                await bundle.close()
            return _error(500, "配置写入失败，请检查服务器存储")
        # Keep adapters used by active streams alive until server shutdown.
        reconnect_voice = (
            context.settings.providers.voice != updated.providers.voice
            or context.settings.providers.seeduplex != updated.providers.seeduplex
            or (updated.providers.voice == "seeduplex" and "seeduplex" in new_secrets)
        )
        context.retired_providers.append(ProviderBundle(
            context.providers.asr, context.providers.llm, context.providers.tts, context.providers.realtime
        ))
        context.providers.asr, context.providers.llm, context.providers.tts = bundle.asr, bundle.llm, bundle.tts
        context.providers.realtime = bundle.realtime
        context.settings = updated
        context.agent.set_model(cast(AgentModel, bundle.llm) if updated.agent.enabled else None)
        context.reports.set_model(bundle.llm if updated.providers.llm != "mock" else None)
        if reconnect_voice:
            sockets = request.app[_WEBSOCKETS_KEY]
            await asyncio.gather(*(socket.close(code=WSCloseCode.SERVICE_RESTART, message=b"voice configuration updated")
                                   for socket in tuple(sockets)))
    return await admin_provider_configuration(request)


async def admin_provider_secrets(request: web.Request) -> web.Response:
    """Return provider secrets only when an authenticated admin explicitly asks."""
    context: GatewayContext = request.app[_CONTEXT_KEY]
    selected = context.settings.providers
    openai = selected.openai
    volcengine = selected.volcengine
    asr_env = volcengine.asr_api_key_env or volcengine.api_key_env
    tts_env = volcengine.tts_api_key_env or volcengine.api_key_env
    llm_key = os.environ.get(openai.api_key_env, "") if selected.llm == "openai" else ""
    asr_key = (
        os.environ.get(asr_env, "") if selected.asr == "volcengine"
        else llm_key if selected.asr == "openai" else ""
    )
    tts_key = (
        os.environ.get(tts_env, "") if selected.tts == "volcengine"
        else llm_key if selected.tts == "openai" else ""
    )
    return web.json_response(
        {
            "llm": {"api_key": llm_key},
            "asr": {"api_key": asr_key},
            "tts": {"api_key": tts_key},
            "feishu": {
                "scope": "device",
                "app_id": "",
                "app_secret": "",
                "verification_token": "",
                "encrypt_key": "",
            },
        }
    )


def _error(status: int, message: str) -> web.Response:
    return web.json_response({"error": message}, status=status)


async def admin_device_state(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.match_info["device_id"]
    state = await context.controls.state(device_id)
    device = next((item for item in await context.registry.snapshot() if item["id"] == device_id), {})
    online = bool(device)
    return web.json_response(
        {"device_id": device_id, "online": online, "state": state,
         "control_online": bool(device.get("control_online")),
         "control_ready": bool(device.get("control_ready")),
         "voice_online": bool(device.get("voice_online"))}
    )


def _calendar_event_payload(event: CalendarEvent) -> dict[str, object]:
    return event.snapshot(include_description=False)


async def _calendar_message(
    service: CalendarSyncService,
) -> dict[str, object]:
    status = await service.status()
    events = await service.snapshot()
    return {
        "type": "device.calendar",
        "calendar_id": str(status.get("calendar_id", "")),
        "revision": int(cast(Any, status.get("revision", 0))),
        "events": [_calendar_event_payload(event) for event in events],
    }


async def admin_device_calendar(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    service = _device_calendar(context, request.match_info["device_id"])
    if service is None:
        return _error(404, "Feishu calendar integration is disabled")
    message = await _calendar_message(service)
    message["device_id"] = request.match_info["device_id"]
    return web.json_response(message)


def _device_calendar(context: GatewayContext, device_id: str) -> CalendarSyncService | None:
    if context.device_feishu is not None:
        service = context.device_feishu.calendar(device_id)
        if service is not None:
            return service
    # Only an explicitly configured legacy target may receive the old calendar.
    return context.calendar if device_id == context.settings.feishu.device_id.strip() else None


def _request_calendar(request: web.Request) -> CalendarSyncService | None:
    context = request.app[_CONTEXT_KEY]
    device_id = request.match_info.get("device_id") or request.query.get("device_id", "")
    return _device_calendar(context, device_id) if device_id else context.calendar


async def admin_calendar_sync(request: web.Request) -> web.Response:
    service = _request_calendar(request)
    if service is None:
        return _error(404, "Feishu calendar integration is disabled")
    try:
        result = await service.sync_once()
    except Exception as exc:
        _LOG.exception("manual Feishu calendar sync failed")
        return _error(502, str(exc))
    return web.json_response({**result, **({"device_id": request.match_info["device_id"]} if "device_id" in request.match_info else {})})


async def admin_calendar_events(request: web.Request) -> web.Response:
    service = _request_calendar(request)
    if service is None:
        return _error(404, "飞书日程集成未启用")
    start, end = request.query.get("start_date", ""), request.query.get("end_date", "")
    try:
        events = await service.events_between(start, end)
    except ValueError as exc:
        return _error(400, str(exc))
    except Exception as exc:
        _LOG.exception("failed to read Feishu calendar events")
        return _error(502, str(exc))
    return web.json_response({
        "calendar_id": service.calendar_id,
        **({"device_id": request.match_info["device_id"]} if "device_id" in request.match_info else {}),
        "start_date": start, "end_date": end, "timezone": "Asia/Shanghai",
        "events": [event.snapshot() for event in events],
        "fetched_at": datetime.now(UTC).isoformat(),
        "sync": dict(await service.status()),
    })


async def admin_calendar_create(request: web.Request) -> web.Response:
    service = _request_calendar(request)
    if service is None:
        return _error(404, "飞书日程集成未启用")
    try:
        draft = CalendarEventDraft.from_payload(await request.json())
        key = _idempotency_key(request) or str(uuid4())
    except (ValueError, TypeError, web.HTTPException) as exc:
        return _error(400, str(exc))
    try:
        event = await service.create_event(draft, key)
    except Exception as exc:
        _LOG.exception("failed to create Feishu calendar event")
        return _error(502, str(exc))
    return web.json_response({"event": event.snapshot()}, status=201)


async def admin_feishu_oauth_url(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if context.feishu_oauth is None:
        return _error(404, "Feishu integration is disabled")
    redirect_uri = request.query.get("redirect_uri", "").strip()
    if not redirect_uri or len(redirect_uri) > 2_000:
        return _error(400, "redirect_uri is required")
    try:
        authorize_url = context.feishu_oauth.authorization_url(redirect_uri)
    except Exception as exc:
        _LOG.exception("failed to create Feishu OAuth URL")
        return _error(500, str(exc))
    return web.json_response({"authorize_url": authorize_url, "redirect_uri": redirect_uri})


async def feishu_oauth_callback(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if context.device_feishu is not None and context.device_feishu.owns_state(request.query.get("state", "")):
        try:
            if request.query.get("error") or not request.query.get("code"):
                return _oauth_html("飞书授权未完成", "请返回当前设备页面重新连接。", error=True)
            device_id, account = await context.device_feishu.callback(request.query["code"], request.query["state"])
            return _oauth_html("飞书授权成功", f"设备 {device_id} 已连接账号：{account.get('display_name') or '飞书账号'}。可以关闭此窗口返回设备页面。")
        except Exception:
            _LOG.exception("device Feishu authorization failed")
            return _oauth_html("飞书授权失败", "授权已失效或暂不可用，请返回当前设备页面重新连接。", error=True)
    if context.device_feishu is not None and context.feishu_oauth is None:
        return _oauth_html("飞书授权已失效", "请从对应设备的飞书设置重新发起连接。", error=True)
    if context.feishu_oauth is None:
        return _oauth_html("飞书未启用", "当前服务没有启用飞书集成。", error=True)
    error = request.query.get("error", "").strip()
    if error:
        description = request.query.get("error_description", error).strip()
        return _oauth_html("飞书授权未完成", description or "用户取消了授权。", error=True)
    code = request.query.get("code", "").strip()
    state = request.query.get("state", "").strip()
    if not code or not state:
        return _oauth_html("飞书授权失败", "回调缺少 code 或 state 参数。", error=True)
    try:
        account = await context.feishu_oauth.handle_callback(code, state)
    except Exception as exc:
        _LOG.exception("Feishu OAuth callback failed")
        return _oauth_html("飞书授权失败", str(exc), error=True)
    if context.calendar is not None:
        try:
            await context.calendar.sync_once()
        except Exception:
            _LOG.exception("Feishu calendar sync after authorization failed")
    name = str(account.get("display_name") or "飞书账号")
    return _oauth_html("飞书授权成功", f"已连接账号：{name}。可以关闭此页面返回后台。")


async def admin_feishu_oauth_disconnect(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    if context.feishu_oauth is None:
        return _error(404, "Feishu integration is disabled")
    await context.feishu_oauth.disconnect()
    return web.json_response({"status": "ok"})


async def admin_device_feishu(request: web.Request) -> web.Response:
    manager = request.app[_CONTEXT_KEY].device_feishu
    if manager is None:
        return _error(503, "当前后端尚不支持设备飞书配置。")
    device_id = request.match_info["device_id"]
    try:
        if request.method == "PUT":
            result = await manager.save(device_id, await request.json())
        else:
            result = await manager.snapshot(device_id)
        return web.json_response(result, headers={"Cache-Control": "no-store"})
    except DeviceFeishuError as exc:
        return _feishu_error(exc)
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "飞书配置请求无效。")
    except Exception:
        _LOG.exception("device Feishu configuration failed")
        return _error(500, "设备飞书配置未能保存，请检查服务端存储。")


async def admin_device_feishu_authorize(request: web.Request) -> web.Response:
    manager = request.app[_CONTEXT_KEY].device_feishu
    if manager is None:
        return _error(503, "当前后端尚不支持设备飞书授权。")
    redirect_uri = request.query.get("redirect_uri", "")
    try:
        redirect = urlsplit(redirect_uri)
        if redirect.scheme not in {"http", "https"} or redirect.netloc != request.host or redirect.path != "/api/v1/feishu/oauth/callback" or redirect.query or redirect.fragment or redirect.username or len(redirect_uri) > 2000:
            return _error(400, "授权回调必须为当前后台的飞书回调地址。")
        url = await manager.authorize(request.match_info["device_id"], redirect_uri)
        return web.json_response({"device_id": request.match_info["device_id"], "authorize_url": url}, headers={"Cache-Control": "no-store"})
    except DeviceFeishuError as exc:
        return _feishu_error(exc)
    except ValueError:
        return _error(400, "授权回调地址无效。")


async def admin_device_feishu_disconnect(request: web.Request) -> web.Response:
    manager = request.app[_CONTEXT_KEY].device_feishu
    if manager is None:
        return _error(503, "当前后端尚不支持设备飞书授权。")
    try:
        await manager.disconnect(request.match_info["device_id"])
        return web.json_response(await manager.snapshot(request.match_info["device_id"]), headers={"Cache-Control": "no-store"})
    except DeviceFeishuError as exc:
        return _feishu_error(exc)


def _feishu_error(error: DeviceFeishuError) -> web.Response:
    return web.json_response({"code": "DEVICE_FEISHU_ERROR", "error": str(error)},
                             status=error.status, headers={"Cache-Control": "no-store"})


def _oauth_html(title: str, message: str, *, error: bool = False) -> web.Response:
    import html

    color = "#b42318" if error else "#067647"
    body = (
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        f"<title>{html.escape(title)}</title><style>body{{font-family:system-ui,sans-serif;"
        "margin:0;min-height:100vh;display:grid;place-items:center;background:#f6f7f9;color:#1d2939}}"
        f"main{{max-width:460px;padding:32px;background:#fff;border:1px solid #e4e7ec;"
        f"border-radius:12px;box-shadow:0 8px 30px #10182814}}h1{{margin:0 0 12px;color:{color};"
        "font-size:22px}}p{margin:0;line-height:1.6}</style>"
        f"<main><h1>{html.escape(title)}</h1><p>{html.escape(message)}</p></main></html>"
    )
    return web.Response(text=body, content_type="text/html", charset="utf-8")


async def admin_device_conversations(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.match_info["device_id"]
    messages = await context.conversations.load(device_id)
    return web.json_response(
        {
            "device_id": device_id,
            "messages": [
                {"role": message.role, "content": message.content}
                for message in messages
            ],
        }
    )


async def admin_device_commands(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.match_info["device_id"]
    try:
        limit = min(100, max(1, int(request.query.get("limit", "20"))))
    except ValueError:
        return _error(400, "limit must be an integer")
    commands = await context.controls.recent_commands(device_id, limit)
    return web.json_response({"device_id": device_id, "commands": commands})


def _conversation_messages(messages: Sequence[ChatMessage]) -> list[dict[str, str]]:
    return [
        {"role": message.role, "content": message.content}
        for message in messages
        if message.role in {"user", "assistant"}
    ]


async def admin_device_chat(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    headers = {"Cache-Control": "no-store"}
    try:
        device_id = WorkspaceService.validate_id(request.match_info["device_id"])
        if device_id.startswith(("web:", "feishu:")):
            raise ValueError("设备编号无效")
    except ValueError as exc:
        return _error(400, str(exc))
    try:
        if request.method in {"GET", "HEAD"}:
            messages = await context.conversations.load_device_history(device_id)
        else:
            try:
                body = await request.json()
            except (ValueError, TypeError, web.HTTPBadRequest):
                return _error(400, "invalid JSON body")
            if not isinstance(body, dict) or not isinstance(body.get("text"), str):
                return _error(400, "text is required")
            request_id = body.get("request_id")
            if not isinstance(request_id, str) or not 1 <= len(request_id) <= 64:
                return _error(400, "request_id must be 1..64 characters")
            reply = await context.web_agent.handle(
                device_id, body["text"], request_id=request_id, approved=True
            )
            messages = reply.messages
    except WebAgentError as exc:
        return _error(400, str(exc))
    except StateConflict as exc:
        return web.json_response(
            {"error": str(exc), "code": "CHAT_REQUEST_UNCERTAIN"}, status=409, headers=headers
        )
    except Exception:
        _LOG.exception("Device chat request failed")
        return _error(502, "Device chat request failed")
    return web.json_response(
        {"device_id": device_id, "messages": _conversation_messages(messages)},
        headers=headers,
    )


async def agent_tools(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    return web.json_response({"tools": context.tools.describe()})


async def agent_reports(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        limit = min(100, max(1, int(request.query.get("limit", "31"))))
    except ValueError:
        return _error(400, "limit must be an integer")
    return web.json_response({
        "reports": list(await context.reports.store.list_reports(limit)),
        "timezone": context.reports.timezone.key,
        "today": context.reports.today(),
        "generate_at": context.settings.daily_report.generate_at,
        "automatic": context.settings.daily_report.enabled,
        "recording_started_at": await context.reports.store.started_at(),
    })


async def agent_report(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    day = request.match_info["date"]
    try:
        start, end = context.reports.bounds(day)
    except ValueError:
        return _error(400, "date must use YYYY-MM-DD")
    batch = await context.reports.store.activities(start, end, 1000)
    return web.json_response({
        "report": await context.reports.store.get_report(day),
        "activities": [item.snapshot() for item in batch.items],
        "activity_count": batch.total,
    })


async def agent_generate_report(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        body = await request.json()
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "invalid JSON body")
    if not isinstance(body, dict) or not isinstance(body.get("date"), str):
        return _error(400, "date is required")
    force = body.get("force", False)
    if not isinstance(force, bool):
        return _error(400, "force must be boolean")
    try:
        report = await context.reports.generate(body["date"], force=force)
    except ValueError as exc:
        return _error(400, str(exc))
    except Exception:
        _LOG.exception("Daily report generation failed")
        return _error(503, "daily report generation failed")
    return web.json_response({"report": report})


async def agent_conversations(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.query.get("device_id", "")
    try:
        limit = min(100, max(1, int(request.query.get("limit", "50"))))
        conversations = await context.web_agent.list(device_id, limit)
    except (ValueError, WebAgentError) as exc:
        return _error(400, str(exc))
    return web.json_response(
        {
            "device_id": device_id,
            "conversations": [
                {
                    "conversation_id": item.conversation_id,
                    "message_count": item.message_count,
                    "updated_at": item.updated_at,
                }
                for item in conversations
            ],
        }
    )


async def agent_create_conversation(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        body = await request.json()
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "invalid JSON body")
    device_id = body.get("device_id") if isinstance(body, dict) else None
    if not isinstance(device_id, str):
        return _error(400, "device_id is required")
    try:
        conversation_id = await context.web_agent.create(device_id)
    except WebAgentError as exc:
        return _error(400, str(exc))
    return web.json_response(
        {"device_id": device_id.strip(), "conversation_id": conversation_id},
        status=201,
    )


async def agent_memories(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.query.get("device_id", "").strip()
    if not device_id:
        return _error(400, "device_id is required")
    try:
        limit = min(100, max(1, int(request.query.get("limit", "50"))))
    except ValueError:
        return _error(400, "limit must be an integer")
    items = await context.memories.search(device_id, request.query.get("query", ""), limit)
    return web.json_response(
        {
            "device_id": device_id,
            "memories": [
                {
                    "memory_id": str(item.memory_id),
                    "content": item.content,
                    "tags": list(item.tags),
                    "created_at": item.created_at,
                }
                for item in items
            ],
        }
    )


async def agent_delete_memory(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.query.get("device_id", "").strip()
    if not device_id:
        return _error(400, "device_id is required")
    try:
        memory_id = UUID(request.match_info["memory_id"])
    except ValueError:
        return _error(400, "memory_id must be a UUID")
    deleted = await context.memories.forget(device_id, memory_id)
    return web.json_response(
        {"device_id": device_id, "memory_id": str(memory_id), "deleted": deleted}
    )


async def agent_tasks(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.query.get("device_id", "").strip()
    if not device_id:
        return _error(400, "device_id is required")
    try:
        limit = min(100, max(1, int(request.query.get("limit", "50"))))
    except ValueError:
        return _error(400, "limit must be an integer")
    statuses = tuple(value for value in request.query.get("status", "").split(",") if value)
    items = await context.tasks.list(device_id, limit, statuses)
    return web.json_response(
        {"device_id": device_id, "tasks": [item.snapshot() for item in items]}
    )


async def agent_cancel_task(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.query.get("device_id", "").strip()
    if not device_id:
        return _error(400, "device_id is required")
    try:
        task_id = UUID(request.match_info["task_id"])
    except ValueError:
        return _error(400, "task_id must be a UUID")
    cancelled = await context.tasks.cancel(device_id, task_id)
    if cancelled:
        await context.journal.record(
            "task", "web", device_id, "取消计划任务", "completed",
            {"task_id": str(task_id), "cancelled": True},
        )
    return web.json_response(
        {"device_id": device_id, "task_id": str(task_id), "cancelled": cancelled}
    )


async def agent_conversation(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    conversation_id = request.match_info["conversation_id"]
    device_id = request.query.get("device_id", "")
    try:
        messages = await context.web_agent.load(device_id, conversation_id)
    except WebAgentError as exc:
        return _error(400, str(exc))
    return web.json_response(
        {
            "device_id": device_id,
            "conversation_id": conversation_id,
            "messages": _conversation_messages(messages),
        }
    )


async def agent_chat(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        body = await request.json()
    except (ValueError, TypeError, web.HTTPException):
        return _error(400, "invalid JSON body")
    if not isinstance(body, dict):
        return _error(400, "JSON body must be an object")
    device_id = body.get("device_id")
    text = body.get("text")
    conversation_id = body.get("conversation_id")
    session_id = body.get("session_id", "")
    approved = body.get("approved", False)
    request_id = body.get("request_id", "")
    if not isinstance(device_id, str) or not isinstance(text, str):
        return _error(400, "device_id and text are required strings")
    if not isinstance(conversation_id, str):
        return _error(400, "conversation_id is required")
    if not isinstance(session_id, str) or not isinstance(approved, bool):
        return _error(400, "session_id must be a string and approved must be boolean")
    if not isinstance(request_id, str) or len(request_id) > 64:
        return _error(400, "request_id must be a string of at most 64 characters")
    try:
        reply = await context.web_agent.handle(
            device_id,
            text,
            conversation_id=conversation_id,
            session_id=session_id,
            approved=approved,
            request_id=request_id,
        )
    except WebAgentError as exc:
        return _error(400, str(exc))
    except StateConflict as exc:
        return web.json_response({"error": str(exc), "code": "CHAT_REQUEST_UNCERTAIN"}, status=409)
    except Exception:
        _LOG.exception("Web Agent request failed")
        return _error(502, "Web Agent request failed")
    return web.json_response(
        {
            "device_id": reply.device_id,
            "conversation_id": reply.conversation_id,
            "session_id": reply.session_id,
            "text": reply.text,
            "messages": _conversation_messages(reply.messages),
        }
    )


def _validate_command(value: Any) -> tuple[str, dict[str, object]]:
    if not isinstance(value, dict) or set(value) != {"name", "payload"}:
        raise ValueError("body must contain exactly name and payload")
    name = value.get("name")
    payload = value.get("payload")
    if not isinstance(name, str) or not isinstance(payload, dict):
        raise ValueError("name must be a string and payload must be an object")
    if name in WORKSPACE_COMMANDS:
        return name, validate_device_payload(name, payload)

    if name in {"screen.brightness.set", "audio.volume.set"}:
        key = "brightness" if name == "screen.brightness.set" else "volume"
        minimum = 1 if key == "brightness" else 0
        number = payload.get(key)
        if set(payload) != {key} or type(number) is not int or not minimum <= number <= 100:
            raise ValueError(f"{key} must be an integer in {minimum}..100")
        return name, {key: number}

    empty_commands = {
        "voice.wake",
        "music.play",
        "music.pause",
        "music.stop",
        "music.previous",
        "music.next",
    }
    if name in empty_commands:
        if payload:
            raise ValueError(f"{name} does not accept payload fields")
        return name, {}
    if name == "music.seek":
        if set(payload) != {"position_seconds"}:
            raise ValueError("music.seek requires position_seconds")
        position = payload["position_seconds"]
        if (
            not isinstance(position, int)
            or isinstance(position, bool)
            or not 0 <= position <= 86_400
        ):
            raise ValueError("position_seconds must be an integer in 0..86400")
        return name, {"position_seconds": position}
    if name == "music.select":
        if set(payload) != {"track_index"}:
            raise ValueError("music.select requires track_index")
        index = payload["track_index"]
        if (
            not isinstance(index, int)
            or isinstance(index, bool)
            or not 0 <= index <= 255
        ):
            raise ValueError("track_index must be an integer in 0..255")
        return name, {"track_index": index}
    if name == "todo.add":
        if set(payload) != {"text"}:
            raise ValueError("todo.add requires text")
        text = payload["text"]
        if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) >= 72:
            raise ValueError("text must be non-empty and shorter than 72 UTF-8 bytes")
        return name, {"text": text.strip()}
    if name in {"todo.edit", "todo.set_done", "todo.delete"}:
        expected = {
            "todo.edit": {"index", "text"},
            "todo.set_done": {"index", "done"},
            "todo.delete": {"index"},
        }[name]
        if set(payload) != expected:
            raise ValueError(f"{name} requires {', '.join(sorted(expected))}")
        index = payload["index"]
        if not isinstance(index, int) or isinstance(index, bool) or not 0 <= index < 8:
            raise ValueError("index must be an integer in 0..7")
        normalized: dict[str, object] = {"index": index}
        if name == "todo.edit":
            text = payload["text"]
            if (
                not isinstance(text, str)
                or not text.strip()
                or len(text.encode("utf-8")) >= 72
            ):
                raise ValueError("text must be non-empty and shorter than 72 UTF-8 bytes")
            normalized["text"] = text.strip()
        elif name == "todo.set_done":
            done = payload["done"]
            if not isinstance(done, bool):
                raise ValueError("done must be a boolean")
            normalized["done"] = done
        return name, normalized
    if name == "display.gif":
        raise ValueError("use the display GIF upload endpoint")
    raise ValueError("unsupported command name")


def _idempotency_key(request: web.Request) -> str | None:
    value = request.headers.get("Idempotency-Key")
    if value is None:
        return None
    value = value.strip()
    if not value or len(value) > 128:
        raise ValueError("Idempotency-Key must be 1..128 characters")
    return value


async def _send_command(
    service: DeviceCommandService,
    device_id: str,
    name: str,
    payload: dict[str, object],
    idempotency_key: str | None = None,
) -> tuple[dict[str, object], int]:
    try:
        command = await service.send(device_id, name, payload, idempotency_key)
    except CommandDispatchError as exc:
        return exc.command, 409
    except StateConflict as exc:
        raise web.HTTPConflict(text=str(exc)) from exc
    return command, 202


async def admin_send_device_command(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        raw = await request.json()
        name, payload = _validate_command(raw)
        idempotency_key = _idempotency_key(request)
    except (ValueError, TypeError, web.HTTPException) as exc:
        return _error(400, str(exc) or "invalid JSON body")
    command, status = await _send_command(
        context.command_service, request.match_info["device_id"], name, payload,
        idempotency_key,
    )
    return web.json_response(command, status=status)


async def admin_command(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        command_id = UUID(request.match_info["command_id"])
    except ValueError:
        return _error(400, "invalid command id")
    command = await context.controls.command(command_id)
    if command is None:
        return _error(404, "command not found")
    return web.json_response(command)


def _write_bytes(path: Path, content: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def _is_supported_image(content: bytes) -> bool:
    return content.startswith((
        b"GIF87a",
        b"GIF89a",
        b"\x89PNG\r\n\x1a\n",
        b"\xff\xd8\xff",
    )) or (content.startswith(b"RIFF") and content[8:12] == b"WEBP")


def _normalize_gif(ffmpeg_path: str, source: Path, destination: Path) -> None:
    """Render an uploaded image in the exact GIF format expected by the device UI."""
    filter_graph = (
        "fps=12.5,"
        "scale=216:216:force_original_aspect_ratio=decrease:flags=lanczos,"
        "pad=216:216:(ow-iw)/2:(oh-ih)/2:color=black,"
        "split[s0][s1];"
        "[s0]palettegen=reserve_transparent=0[p];"
        "[s1][p]paletteuse=dither=sierra2_4a"
    )
    try:
        result = subprocess.run(
            [
                ffmpeg_path,
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(source),
                "-filter_complex",
                filter_graph,
                "-gifflags",
                "-offsetting-transdiff",
                "-f",
                "gif",
                str(destination),
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError("GIF conversion did not complete") from exc
    if result.returncode != 0 or not destination.is_file() or destination.stat().st_size < 6:
        detail = result.stderr.strip()[-240:] if result.stderr else "unknown FFmpeg error"
        raise RuntimeError(f"GIF conversion failed: {detail}")


async def admin_upload_gif(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    settings = context.settings.server
    if request.content_length is not None and request.content_length > settings.max_gif_bytes:
        return _error(413, "image exceeds configured size limit")
    try:
        content = await request.read()
    except web.HTTPRequestEntityTooLarge:
        return _error(413, "image exceeds configured size limit")
    if not _is_supported_image(content):
        return _error(415, "uploaded file is not a supported GIF, PNG, JPEG, or WebP image")
    if len(content) > settings.max_gif_bytes:
        return _error(413, "image exceeds configured size limit")
    if not Path(settings.ffmpeg_path).is_file():
        return _error(503, "FFmpeg is not installed at the configured path")

    asset_id = uuid4()
    directory = Path(settings.asset_directory)
    source = directory / f".{asset_id}.input.gif"
    normalized = directory / f".{asset_id}.normalized.gif"
    final = directory / f"{asset_id}.gif"
    try:
        await asyncio.to_thread(directory.mkdir, parents=True, exist_ok=True)
        await asyncio.to_thread(_write_bytes, source, content)
        await asyncio.to_thread(_normalize_gif, settings.ffmpeg_path, source, normalized)
        if normalized.stat().st_size > settings.max_gif_bytes:
            raise RuntimeError("normalized GIF exceeds configured size limit")
        await asyncio.to_thread(os.replace, normalized, final)
    except (OSError, RuntimeError) as exc:
        await asyncio.to_thread(source.unlink, missing_ok=True)
        await asyncio.to_thread(normalized.unlink, missing_ok=True)
        _LOG.exception("failed to store GIF asset")
        return _error(500, f"could not process image: {exc}")
    finally:
        await asyncio.to_thread(source.unlink, missing_ok=True)
        await asyncio.to_thread(normalized.unlink, missing_ok=True)

    payload: dict[str, object] = {
        "asset_path": f"/v1/device/assets/{asset_id}.gif",
        "asset_id": str(asset_id),
    }
    command, status = await _send_command(
        context.command_service, request.match_info["device_id"], "display.gif", payload
    )
    if status != 202:
        await asyncio.to_thread(final.unlink, missing_ok=True)
    return web.json_response(
        {
            "asset_id": str(asset_id),
            "asset_path": payload["asset_path"],
            "command": command,
        },
        status=status,
    )


async def device_asset(request: web.Request) -> web.StreamResponse:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        asset_id = UUID(request.match_info["asset_id"])
    except ValueError:
        return _error(400, "invalid asset id")
    path = Path(context.settings.server.asset_directory) / f"{asset_id}.gif"
    if not path.is_file():
        return _error(404, "asset not found")
    return web.FileResponse(
        path,
        headers={"Content-Type": "image/gif", "Cache-Control": "private, max-age=86400"},
    )


def _prepare_module_image(context: GatewayContext, content: bytes, name: str) -> tuple[str, Path]:
    return prepare_module_image(context.component_ota,
                                Path(context.settings.server.asset_directory) / "module-images",
                                content, name)


async def admin_upload_module_image(request: web.Request) -> web.Response:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        idempotency_key = _idempotency_key(request)
    except ValueError as exc:
        return _error(400, str(exc))
    limit = context.settings.server.max_gif_bytes
    if request.content_length is not None and request.content_length > limit:
        return _error(413, "image exceeds configured size limit")
    try:
        content = await request.read()
    except web.HTTPRequestEntityTooLarge:
        return _error(413, "image exceeds configured size limit")
    if len(content) > limit:
        return _error(413, "image exceeds configured size limit")
    if not _is_supported_image(content):
        return _error(415, "uploaded file is not a supported GIF, PNG, JPEG, or WebP image")
    name = image_name(request.query.get("name", "图片"))
    try:
        asset_id, directory = await asyncio.to_thread(_prepare_module_image, context, content, name)
    except Exception as exc:
        _LOG.exception("failed to prepare module gallery image")
        return _error(400, f"could not process image: {exc}")
    asset_path = f"/v1/device/module-images/{asset_id}"
    command, status = await _send_command(
        context.command_service, request.match_info["device_id"], "modules.image",
        {"asset_id": asset_id, "asset_path": asset_path},
        idempotency_key,
    )
    # A retried request can return the original command. Keep its bundle and
    # remove the newly normalized duplicate instead of sending a second image.
    payload = command.get("payload")
    accepted_id = payload.get("asset_id") if isinstance(payload, dict) else None
    if status != 202 or accepted_id != asset_id:
        await asyncio.to_thread(shutil.rmtree, directory, True)
    if status == 202 and (command.get("name") != "modules.image" or not accepted_id):
        return _error(409, "idempotency key already used for a different operation")
    if isinstance(accepted_id, str):
        asset_id = accepted_id
        asset_path = f"/v1/device/module-images/{asset_id}"
    return web.json_response({"asset_id": asset_id, "asset_path": asset_path,
                              "name": name, "command": command}, status=status)


async def module_image_asset(request: web.Request) -> web.StreamResponse:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    try:
        asset_id = UUID(request.match_info["asset_id"])
    except ValueError:
        return _error(400, "invalid asset id")
    filename = request.match_info["filename"]
    if filename not in {"manifest.json", "thumbnail.png", "round.gif", "holo.gif"}:
        return _error(404, "asset not found")
    path = (Path(context.settings.server.asset_directory)
            / "module-images" / str(asset_id) / filename)
    if not path.is_file():
        return _error(404, "asset not found")
    return web.FileResponse(path, headers={"Cache-Control": "private, max-age=86400"})


async def admin_device_video(request: web.Request) -> web.StreamResponse:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    device_id = request.match_info["device_id"]
    camera_url = await context.registry.camera_url(device_id)
    if camera_url is None:
        return _error(404, "device is offline or did not advertise a camera")
    ffmpeg_path = context.settings.server.ffmpeg_path
    if not Path(ffmpeg_path).is_file():
        return _error(503, "FFmpeg is not installed at the configured path")
    try:
        process = await asyncio.create_subprocess_exec(
            ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "error",
            "-rtsp_transport",
            "tcp",
            "-i",
            camera_url,
            "-an",
            "-r",
            "8",
            "-q:v",
            "5",
            "-f",
            "mpjpeg",
            "-boundary_tag",
            "binbotframe",
            "pipe:1",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
    except OSError as exc:
        return _error(503, f"could not start FFmpeg: {exc.strerror or 'I/O error'}")

    assert process.stdout is not None
    try:
        first = await asyncio.wait_for(
            process.stdout.read(64 * 1024),
            timeout=context.settings.server.video_start_timeout_seconds,
        )
        if not first:
            return _error(502, "camera stream did not produce video")
        response = web.StreamResponse(
            headers={
                "Content-Type": "multipart/x-mixed-replace; boundary=binbotframe",
                "Cache-Control": "no-store, no-cache, must-revalidate",
            }
        )
        await response.prepare(request)
        await response.write(first)
        while chunk := await process.stdout.read(64 * 1024):
            await response.write(chunk)
        return response
    except TimeoutError:
        return _error(502, "camera stream did not produce video before the startup timeout")
    except (ConnectionResetError, asyncio.CancelledError):
        raise
    finally:
        if process.returncode is None:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), timeout=2.0)
            except TimeoutError:
                process.kill()
                await process.wait()


def _camera_url(remote: str | None, hello: HelloMessage) -> str | None:
    if hello.camera is None or remote is None:
        return None
    try:
        address = ipaddress.ip_address(remote)
    except ValueError:
        return None
    host = f"[{address}]" if address.version == 6 else str(address)
    return f"rtsp://{host}:{hello.camera.port}{hello.camera.path}"


async def websocket_handler(request: web.Request) -> web.WebSocketResponse:
    context: GatewayContext = request.app[_CONTEXT_KEY]
    settings = context.settings
    websocket = web.WebSocketResponse(
        protocols=("binbot-bbvp",),
        heartbeat=30.0,
        max_msg_size=max(
            settings.server.max_control_bytes,
            settings.server.max_audio_payload_bytes + 40,
        ),
        compress=False,
    )
    await websocket.prepare(request)
    request.app[_WEBSOCKETS_KEY].add(websocket)

    actor: SessionActor | None = None
    device_id: str | None = None
    try:
        first = await asyncio.wait_for(
            websocket.receive(),
            timeout=settings.server.handshake_timeout_seconds,
        )
        if first.type != WSMsgType.TEXT:
            raise ProtocolViolation("the first WebSocket message must be a hello control event")
        hello = parse_hello(first.data, settings.server.max_control_bytes)
        context.authenticator.verify(hello.device_id, hello.token)
        _validate_audio_negotiation(context, hello)
        device_id = hello.device_id
        realtime = context.providers.realtime is not None
        if realtime and "realtime_voice_v1" not in hello.capabilities:
            raise ProtocolViolation("Seeduplex requires a device supporting realtime_voice_v1; update binbot_dialog")

        session = VoiceSession(
            device_id=device_id,
            uplink=_audio_format(hello.uplink),
            downlink=_audio_format(hello.downlink),
            max_history_messages=settings.conversation.max_history_messages,
        )
        sink = WebSocketEventSink(websocket)
        endpoint = (
            context.endpoint_factory()
            if not realtime and "server_vad_v1" in hello.capabilities and context.endpoint_factory is not None
            and hello.uplink.sample_rate == 16000
            else None
        )
        actor_type = RealtimeSessionActor if realtime else SessionActor
        realtime_options: dict[str, Any] = (
            {"idle_timeout_seconds": settings.providers.seeduplex.idle_timeout_seconds,
             "allow_interruption": settings.providers.seeduplex.allow_interruption,
             "max_tool_rounds": settings.agent.max_tool_rounds,
             "interruption_asr": context.providers.asr
             if settings.providers.asr == "volcengine" else None} if realtime else {}
        )
        actor = actor_type(
            session=session,
            providers=context.providers,
            decoder=OpusDecoder(
                session.uplink.sample_rate,
                session.uplink.channels,
                session.uplink.frame_ms,
            ),
            encoder=OpusEncoder(
                session.downlink.sample_rate,
                session.downlink.channels,
                session.downlink.frame_ms,
                settings.audio.downlink_bitrate,
            ),
            sink=sink,
            first_sentence_chars=settings.conversation.first_sentence_chars,
            conversation_store=context.conversations,
            max_turn_audio_seconds=settings.pipeline.max_turn_audio_seconds,
            asr_timeout_seconds=settings.pipeline.asr_timeout_seconds,
            llm_timeout_seconds=settings.pipeline.llm_timeout_seconds,
            tts_timeout_seconds=settings.pipeline.tts_timeout_seconds,
            tts_concurrency=settings.pipeline.tts_concurrency,
            max_tts_segments=settings.pipeline.max_tts_segments,
            max_tts_segment_bytes=settings.pipeline.max_tts_segment_bytes,
            device_agent="device_agent_v1" in hello.capabilities,
            agent_runtime=context.agent if not realtime or settings.agent.enabled else None,
            endpoint_detector=endpoint,
            **realtime_options,
        )
        await context.registry.register(
            device_id,
            actor,
            remote_ip=request.remote or "",
            camera_url=_camera_url(request.remote, hello),
            capabilities=hello.capabilities,
        )
        await context.controls.device_connected(device_id)
        for snapshot in await context.registry.snapshot():
            if snapshot["id"] == device_id:
                snapshot["last_seen_at"] = snapshot["connected_at"]
                await context.controls.remember_device(snapshot)
                break
        await sink.send_control(
            {
                "type": "hello.ack",
                "protocol_version": 1,
                "session_id": str(session.session_id),
                "server_version": __version__,
                "input_endpoint": "realtime_voice_v1" if realtime else (
                    "server_vad_v1" if endpoint is not None else "client"
                ),
                "uplink": hello.uplink.model_dump(),
                "downlink": hello.downlink.model_dump(),
            }
        )
        if context.workspace is not None:
            context.workspace.invalidate_calendar(hello.device_id)
        service = _device_calendar(context, hello.device_id)
        if service is not None and not await context.registry.workspace_enabled(hello.device_id):
            await sink.send_control(await _calendar_message(service))
        elif context.device_feishu is not None and (hello.device_id in context.device_feishu.documents or context.settings.feishu.enabled) and not await context.registry.workspace_enabled(hello.device_id):
            await sink.send_control({"type": "device.calendar", "calendar_id": "", "revision": 0, "events": []})
        await actor.start()
        _LOG.info("device connected device_id=%s remote=%s", device_id, request.remote)

        async for message in websocket:
            try:
                if message.type == WSMsgType.TEXT:
                    control = parse_control(message.data, settings.server.max_control_bytes)
                    if isinstance(control, TurnStartMessage):
                        await actor.begin_turn(
                            control.turn_id,
                            control.stream_id,
                            control.wake_word,
                        )
                    elif isinstance(control, AudioEndMessage):
                        await actor.end_audio(control.turn_id, control.stream_id)
                    elif isinstance(control, TurnCancelMessage):
                        await actor.cancel_turn(control.turn_id, control.reason)
                    elif isinstance(control, TextInputMessage):
                        await actor.submit_text(control.turn_id, control.text)
                    elif isinstance(control, AgentResponseMessage):
                        await actor.submit_agent_response(control.turn_id, control.text)
                    elif isinstance(control, PingMessage):
                        await sink.send_control({"type": "pong", "nonce": control.nonce})
                    elif isinstance(control, DeviceStateMessage):
                        await context.controls.update_state(device_id, control)
                    elif isinstance(control, ComponentEventMessage):
                        if "component_ota_v1" not in hello.capabilities:
                            raise ProtocolViolation("component events require component_ota_v1")
                        await context.component_ota.event(device_id, control.model_dump(mode="json"))
                    elif isinstance(control, DeviceCommandResultMessage):
                        await context.controls.complete(device_id, control)
                        await context.journal.command_result(
                            device_id, str(control.command_id), control.success, control.message,
                        )
                elif message.type == WSMsgType.BINARY:
                    frame = decode_audio_frame(
                        message.data,
                        settings.server.max_audio_payload_bytes,
                    )
                    if frame.kind != AudioKind.UPLINK_OPUS:
                        raise ProtocolViolation("device may only send uplink Opus audio frames")
                    await actor.push_audio(
                        frame.stream_id,
                        frame.sequence,
                        frame.timestamp_ms,
                        frame.payload,
                    )
                elif message.type in {WSMsgType.CLOSE, WSMsgType.CLOSED, WSMsgType.ERROR}:
                    break
            except StateConflict as exc:
                # A semantic race (for example a late audio.end after cancel) does not
                # corrupt framing, so report it without throwing away the connection.
                await _send_error(websocket, exc)
    except TimeoutError:
        await _send_error(websocket, ProtocolViolation("hello handshake timed out"))
        await websocket.close(code=WSCloseCode.POLICY_VIOLATION, message=b"hello timeout")
        _LOG.warning("connection handshake timed out remote=%s", request.remote)
    except ProtocolViolation as exc:
        await _send_error(websocket, exc)
        await websocket.close(
            code=WSCloseCode.POLICY_VIOLATION, message=b"protocol violation"
        )
        _LOG.warning("protocol violation device_id=%s error=%s", device_id, exc)
    except AuthenticationFailed as exc:
        await _send_error(websocket, exc)
        await websocket.close(
            code=WSCloseCode.POLICY_VIOLATION, message=b"authentication failed"
        )
        _LOG.warning("authentication failed device_id=%s", device_id)
    except BinBotError as exc:
        await _send_error(websocket, exc)
        _LOG.warning("connection error device_id=%s error=%s", device_id, exc)
    except Exception as exc:
        _LOG.exception("unexpected connection error device_id=%s", device_id)
        await _send_error(websocket, exc, retryable=True)
    finally:
        request.app[_WEBSOCKETS_KEY].discard(websocket)
        if actor is not None and device_id is not None:
            await context.registry.unregister(device_id, actor)
        if actor is not None:
            await actor.close()
        if not websocket.closed:
            await websocket.close()
        if device_id:
            _LOG.info("device disconnected device_id=%s", device_id)
    return websocket


async def _shutdown(app: web.Application) -> None:
    active_websockets = app[_WEBSOCKETS_KEY]
    websockets = tuple(active_websockets)
    if websockets:
        await asyncio.gather(
            *(
                websocket.close(
                    code=WSCloseCode.GOING_AWAY,
                    message=b"server shutdown",
                )
                for websocket in websockets
            ),
            return_exceptions=True,
        )


async def _cleanup(app: web.Application) -> None:
    context: GatewayContext = app[_CONTEXT_KEY]
    if context.workspace is not None:
        await context.workspace.close()
    if context.mqtt is not None:
        await context.mqtt.close()
    if context.device_feishu is not None:
        await context.device_feishu.close()
    await context.component_ota.close()
    await context.worker.stop()
    await context.report_worker.stop()
    await context.registry.close_all()
    await context.conversations.close()
    if context.workspace_store is not None:
        await context.workspace_store.close()
    await context.controls.close()
    await context.memories.close()
    await context.tasks.close()
    await context.reports.store.close()
    if context.calendar is not None:
        await context.calendar.stop()
        await context.calendar.close()
    if context.feishu_connection is not None:
        await context.feishu_connection.stop()
    if context.feishu is not None:
        await context.feishu.close()
    await context.providers.close()
    for providers in context.retired_providers:
        await providers.close()


async def _startup(app: web.Application) -> None:
    context = app[_CONTEXT_KEY]
    if context.mqtt is not None:
        await context.mqtt.start()
    context.component_ota.start()
    context.worker.start()
    context.report_worker.start()
    if context.device_feishu is not None:
        await context.device_feishu.start()
    if context.feishu_oauth is not None:
        await context.feishu_oauth.restore()
    if context.feishu_connection is not None:
        context.feishu_connection.start()
    if context.calendar is not None:
        context.calendar.start()


def create_http_app(context: GatewayContext) -> web.Application:
    app = web.Application(
        client_max_size=max(
            context.settings.server.max_control_bytes,
            context.settings.server.max_gif_bytes,
        ),
        middlewares=(component_errors, admin_authentication),
    )
    app[_CONTEXT_KEY] = context
    register_routes(app, context.component_ota)
    app[_WEBSOCKETS_KEY] = set()
    app.router.add_get("/healthz", health)
    app.router.add_get("/readyz", health)
    app.router.add_post("/api/v1/feishu/events", feishu_events)
    app.router.add_get("/api/v1/feishu/oauth/callback", feishu_oauth_callback)
    app.router.add_get("/api/v1/agent/tools", agent_tools)
    app.router.add_get("/api/v1/agent/reports", agent_reports)
    app.router.add_post("/api/v1/agent/reports/generate", agent_generate_report)
    app.router.add_get("/api/v1/agent/reports/{date}", agent_report)
    app.router.add_get("/api/v1/agent/conversations", agent_conversations)
    app.router.add_post("/api/v1/agent/conversations", agent_create_conversation)
    app.router.add_get(
        "/api/v1/agent/conversations/{conversation_id}", agent_conversation
    )
    app.router.add_post("/api/v1/agent/chat", agent_chat)
    app.router.add_get("/api/v1/agent/memories", agent_memories)
    app.router.add_delete(
        "/api/v1/agent/memories/{memory_id}", agent_delete_memory
    )
    app.router.add_get("/api/v1/agent/tasks", agent_tasks)
    app.router.add_post(
        "/api/v1/agent/tasks/{task_id}/cancel", agent_cancel_task
    )
    app.router.add_get("/api/v1/admin/devices", admin_devices)
    app.router.add_get("/api/v1/admin/providers", admin_providers)
    app.router.add_get("/api/v1/admin/providers/configuration", admin_provider_configuration)
    app.router.add_put("/api/v1/admin/providers/configuration", save_provider_configuration)
    app.router.add_get("/api/v1/admin/providers/secrets", admin_provider_secrets)
    app.router.add_get("/api/v1/admin/session", admin_session)
    app.router.add_post("/api/v1/admin/session", admin_session)
    app.router.add_post("/api/v1/admin/logout", admin_logout)
    app.router.add_get(
        "/api/v1/admin/devices/{device_id}/state", admin_device_state
    )
    app.router.add_get(
        "/api/v1/admin/devices/{device_id}/calendar", admin_device_calendar
    )
    app.router.add_get("/api/v1/admin/devices/{device_id}/feishu", admin_device_feishu)
    app.router.add_put("/api/v1/admin/devices/{device_id}/feishu", admin_device_feishu)
    app.router.add_get("/api/v1/admin/devices/{device_id}/feishu/oauth/url", admin_device_feishu_authorize)
    app.router.add_post("/api/v1/admin/devices/{device_id}/feishu/oauth/disconnect", admin_device_feishu_disconnect)
    app.router.add_post("/api/v1/admin/devices/{device_id}/calendar/sync", admin_calendar_sync)
    app.router.add_get("/api/v1/admin/devices/{device_id}/calendar/events", admin_calendar_events)
    app.router.add_post("/api/v1/admin/devices/{device_id}/calendar/events", admin_calendar_create)
    app.router.add_post("/api/v1/admin/calendar/sync", admin_calendar_sync)
    app.router.add_get("/api/v1/admin/calendar/events", admin_calendar_events)
    app.router.add_post("/api/v1/admin/calendar/events", admin_calendar_create)
    app.router.add_get("/api/v1/admin/feishu/oauth/url", admin_feishu_oauth_url)
    app.router.add_post(
        "/api/v1/admin/feishu/oauth/disconnect", admin_feishu_oauth_disconnect
    )
    app.router.add_get(
        "/api/v1/admin/devices/{device_id}/conversations",
        admin_device_conversations,
    )
    app.router.add_get("/api/v1/admin/devices/{device_id}/chat", admin_device_chat)
    app.router.add_post("/api/v1/admin/devices/{device_id}/chat", admin_device_chat)
    app.router.add_get(
        "/api/v1/admin/devices/{device_id}/commands", admin_device_commands
    )
    app.router.add_post(
        "/api/v1/admin/devices/{device_id}/commands", admin_send_device_command
    )
    app.router.add_get("/api/v1/admin/commands/{command_id}", admin_command)
    app.router.add_put(
        "/api/v1/admin/devices/{device_id}/display/gif", admin_upload_gif
    )
    app.router.add_put(
        "/api/v1/admin/devices/{device_id}/modules/images", admin_upload_module_image
    )
    app.router.add_get(
        "/api/v1/admin/devices/{device_id}/video.mjpeg", admin_device_video
    )
    app.router.add_get("/v1/device/assets/{asset_id}.gif", device_asset)
    app.router.add_get("/v1/device/module-images/{asset_id}/{filename}", module_image_asset)
    app.router.add_get("/v1/device/ws", websocket_handler)
    register_weather_routes(app)
    assert context.workspace_store is not None
    register_gallery_routes(app, context.workspace_store, context.component_ota, Path(context.settings.server.asset_directory) / "module-images", context.settings.server.max_gif_bytes)
    register_camera_routes(app, context.settings.camera, context.registry, context.workspace.apply if context.workspace else None)
    if context.workspace is not None:
        register_workspace_routes(app, context.workspace)
        async def start_workspace(_app: web.Application) -> None:
            assert context.workspace is not None
            context.workspace.start()
        app.on_startup.append(start_workspace)
    app.on_shutdown.append(_shutdown)
    app.on_startup.append(_startup)
    app.on_cleanup.append(_cleanup)
    return app
