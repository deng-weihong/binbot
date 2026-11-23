"""Minimal Feishu tenant-token and message client."""

from __future__ import annotations

import asyncio
import importlib.util
import json
import logging
import os
import secrets
import threading
import time
from collections.abc import Awaitable, Callable, Mapping
from pathlib import Path
from typing import Any, cast
from urllib.parse import urlencode
from uuid import uuid4

import aiohttp

from binbot_server.application.feishu import FeishuMessenger
from binbot_server.application.calendar import CalendarEvent, CalendarEventDraft


class FeishuClientError(RuntimeError):
    pass


_LOG = logging.getLogger("binbot.feishu")


class FeishuTokenStore:
    """Small file-backed store for the server-owned Feishu user grant."""

    def __init__(self, path: str):
        self._path = Path(path).expanduser()

    def load(self) -> dict[str, object]:
        try:
            value = json.loads(self._path.read_text(encoding="utf-8"))
        except (FileNotFoundError, OSError, ValueError):
            return {}
        return dict(value) if isinstance(value, dict) else {}

    def save(self, value: Mapping[str, object]) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._path.with_name(f".{self._path.name}-{uuid4().hex}.tmp")
        try:
            with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
                json.dump(dict(value), stream, ensure_ascii=False)
                stream.flush()
                os.fsync(stream.fileno())
            temporary.replace(self._path)
        finally:
            temporary.unlink(missing_ok=True)

    def clear(self) -> None:
        try:
            self._path.unlink()
        except FileNotFoundError:
            pass


class FeishuOAuthManager:
    """Create local OAuth links and apply the resulting user grant."""

    def __init__(
        self,
        client: "HttpFeishuClient",
        token_store: FeishuTokenStore,
        app_id: str,
        scopes: tuple[str, ...],
        apply_token: Callable[[str], Awaitable[None]],
    ):
        self._client = client
        self._token_store = token_store
        self._app_id = app_id
        self._scopes = scopes
        self._apply_token = apply_token
        self._pending: dict[str, tuple[str, float]] = {}
        self._refresh_lock = asyncio.Lock()
        self._applied_token = ""

    def status(self) -> dict[str, object]:
        value = self._token_store.load()
        return {
            "authorized": bool(value.get("access_token")),
            "display_name": str(value.get("name") or value.get("en_name") or ""),
            "open_id": str(value.get("open_id") or ""),
            "tenant_key": str(value.get("tenant_key") or ""),
            "expires_at": str(value.get("expires_at") or ""),
            "scopes": list(self._scopes),
        }

    def authorization_url(self, redirect_uri: str) -> str:
        state = secrets.token_urlsafe(24)
        self._pending[state] = (redirect_uri, time.time() + 600)
        query = urlencode(
            {
                "app_id": self._app_id,
                "redirect_uri": redirect_uri,
                "state": state,
                "scope": " ".join(self._scopes),
            }
        )
        return f"{self._client.base_url}/open-apis/authen/v1/authorize?{query}"

    async def handle_callback(self, code: str, state: str) -> dict[str, object]:
        pending = self._pending.pop(state, None)
        if pending is None or pending[1] < time.time():
            raise FeishuClientError("Feishu OAuth state is invalid or expired")
        data = await self._client.exchange_user_code(code)
        access_token = str(data.get("access_token") or "")
        if not access_token:
            raise FeishuClientError("Feishu OAuth response did not contain an access token")
        expires_in = int(cast(Any, data.get("expires_in") or 0))
        value = dict(data)
        value["expires_at"] = str(int(time.time()) + max(0, expires_in))
        refresh_expires_in = int(cast(Any, data.get("refresh_expires_in") or 0))
        if refresh_expires_in > 0:
            value["refresh_expires_at"] = str(
                int(time.time()) + refresh_expires_in
            )
        async with self._refresh_lock:
            self._token_store.save(value)
            await self._apply_token(access_token)
            self._applied_token = access_token
        return {
            "display_name": str(value.get("name") or value.get("en_name") or ""),
            "open_id": str(value.get("open_id") or ""),
            "tenant_key": str(value.get("tenant_key") or ""),
        }

    async def restore(self) -> bool:
        """Restore the saved grant, refreshing it when the access token expired."""
        try:
            return await self.ensure_fresh_token()
        except FeishuClientError:
            _LOG.exception("failed to restore saved Feishu user grant")
            return False

    async def ensure_fresh_token(self) -> bool:
        async with self._refresh_lock:
            return await self._ensure_fresh_token()

    async def _ensure_fresh_token(self) -> bool:
        value = self._token_store.load()
        access_token = str(value.get("access_token") or "")
        if not access_token:
            return False
        try:
            expires_at = int(str(value.get("expires_at") or "0"))
        except ValueError:
            expires_at = 0
        if access_token and expires_at > int(time.time()) + 60:
            if access_token != self._applied_token:
                await self._apply_token(access_token)
                self._applied_token = access_token
            return True
        refresh_token = str(value.get("refresh_token") or "")
        if not refresh_token:
            raise FeishuClientError("飞书授权已过期，请重新连接飞书账号")
        try:
            refresh_expires_at = int(str(value.get("refresh_expires_at") or "0"))
        except ValueError:
            refresh_expires_at = 0
        if refresh_expires_at and refresh_expires_at <= int(time.time()):
            raise FeishuClientError("飞书授权已过期，请重新连接飞书账号")
        try:
            data = await self._client.refresh_user_token(refresh_token)
        except Exception as exc:
            raise FeishuClientError(f"飞书授权续期失败，请重新连接飞书账号：{exc}") from exc
        if not data.get("access_token"):
            raise FeishuClientError("飞书授权续期未返回令牌，请重新连接飞书账号")
        refreshed = dict(value)
        refreshed.update({key: item for key, item in data.items() if item is not None})
        now = int(time.time())
        refreshed["expires_at"] = str(now + int(cast(Any, data.get("expires_in") or 0)))
        if data.get("refresh_expires_in"):
            refreshed["refresh_expires_at"] = str(
                now + int(cast(Any, data["refresh_expires_in"] or 0))
            )
        self._token_store.save(refreshed)
        new_access_token = str(refreshed.get("access_token") or "")
        await self._apply_token(new_access_token)
        self._applied_token = new_access_token
        return bool(new_access_token)

    async def disconnect(self) -> None:
        async with self._refresh_lock:
            self._pending.clear()
            self._token_store.clear()
            await self._apply_token("")
            self._applied_token = ""


class HttpFeishuClient(FeishuMessenger):
    def __init__(
        self,
        base_url: str,
        app_id: str,
        app_secret: str,
        request_timeout_seconds: float = 10.0,
        user_access_token: str = "",
    ):
        self._base_url = base_url.rstrip("/")
        self._app_id = app_id
        self._app_secret = app_secret
        self._timeout = aiohttp.ClientTimeout(total=request_timeout_seconds)
        self._session: aiohttp.ClientSession | None = None
        self._token: str | None = None
        self._token_expires_at = 0.0
        self._lock = asyncio.Lock()
        self._sdk_client: Any = None
        self._user_access_token = user_access_token.strip()

    @property
    def base_url(self) -> str:
        return self._base_url

    def set_user_access_token(self, value: str) -> None:
        self._user_access_token = value.strip()

    def _sdk(self) -> Any:
        if self._sdk_client is None:
            try:
                import lark_oapi as lark
            except ImportError as exc:  # pragma: no cover - optional dependency
                raise FeishuClientError("lark-oapi is required for Feishu SDK access") from exc
            self._sdk_client = (
                lark.Client.builder()
                .app_id(self._app_id)
                .app_secret(self._app_secret)
                .enable_set_token(True)
                .domain(self._base_url)
                .timeout(self._timeout.total)
                .log_level(lark.LogLevel.WARNING)
                .build()
            )
        return self._sdk_client

    def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(timeout=self._timeout)
        return self._session

    async def _tenant_token(self) -> str:
        async with self._lock:
            if self._token and time.monotonic() < self._token_expires_at:
                return self._token
            session = self._get_session()
            async with session.post(
                f"{self._base_url}/open-apis/auth/v3/tenant_access_token/internal",
                json={"app_id": self._app_id, "app_secret": self._app_secret},
            ) as response:
                body: Any = await response.json(content_type=None)
            if response.status != 200 or body.get("code") != 0:
                raise FeishuClientError(f"Feishu token request failed: {body}")
            token = body.get("tenant_access_token")
            expires = body.get("expire", 7_200)
            if not isinstance(token, str) or not token:
                raise FeishuClientError("Feishu token response did not contain a token")
            self._token = token
            self._token_expires_at = time.monotonic() + max(60, int(expires) - 60)
            return token

    async def send_text(self, receive_id: str, text: str, receive_id_type: str) -> None:
        try:
            from lark_oapi.api.im.v1 import (
                CreateMessageRequest,
                CreateMessageRequestBody,
            )

            request = (
                CreateMessageRequest.builder()
                .receive_id_type(receive_id_type)
                .request_body(
                    CreateMessageRequestBody.builder()
                    .receive_id(receive_id)
                    .msg_type("text")
                    .content(json.dumps({"text": text}, ensure_ascii=False))
                    .uuid(str(uuid4()))
                    .build()
                )
                .build()
            )
            response = await self._sdk().im.v1.message.acreate(request)
        except Exception as exc:
            if isinstance(exc, FeishuClientError):
                raise
            raise FeishuClientError(f"Feishu message request failed: {exc}") from exc
        if not response.success():
            raise FeishuClientError(
                f"Feishu message request failed: code={response.code} msg={response.msg}"
            )

    async def exchange_user_code(self, code: str) -> dict[str, object]:
        try:
            from lark_oapi.api.authen.v1 import (
                CreateAccessTokenRequest,
                CreateAccessTokenRequestBody,
            )

            request = (
                CreateAccessTokenRequest.builder()
                .request_body(
                    CreateAccessTokenRequestBody.builder()
                    .grant_type("authorization_code")
                    .code(code)
                    .build()
                )
                .build()
            )
            response = await self._sdk().authen.v1.access_token.acreate(request)
        except Exception as exc:
            raise FeishuClientError(f"Feishu OAuth token exchange failed: {exc}") from exc
        if not response.success() or response.data is None:
            raise FeishuClientError(
                f"Feishu OAuth token exchange failed: code={response.code} msg={response.msg}"
            )
        data = response.data
        fields = (
            "access_token", "refresh_token", "expires_in", "name", "en_name",
            "open_id", "union_id", "user_id", "tenant_key", "refresh_expires_in",
        )
        return {field: getattr(data, field, None) for field in fields}

    async def refresh_user_token(self, refresh_token: str) -> dict[str, object]:
        try:
            from lark_oapi.api.authen.v1 import (
                CreateRefreshAccessTokenRequest,
                CreateRefreshAccessTokenRequestBody,
            )

            request = (
                CreateRefreshAccessTokenRequest.builder()
                .request_body(
                    CreateRefreshAccessTokenRequestBody.builder()
                    .grant_type("refresh_token")
                    .refresh_token(refresh_token)
                    .build()
                )
                .build()
            )
            response = await self._sdk().authen.v1.refresh_access_token.acreate(request)
        except Exception as exc:
            raise FeishuClientError(f"Feishu OAuth token refresh failed: {exc}") from exc
        if not response.success() or response.data is None:
            raise FeishuClientError(
                f"Feishu OAuth token refresh failed: code={response.code} msg={response.msg}"
            )
        data = response.data
        fields = (
            "access_token", "refresh_token", "expires_in", "refresh_expires_in",
        )
        return {field: getattr(data, field, None) for field in fields}

    async def calendar_primary(self) -> str:
        from lark_oapi.api.calendar.v4 import PrimaryCalendarRequest

        response = await self._sdk().calendar.v4.calendar.aprimary(
            PrimaryCalendarRequest.builder().user_id_type("open_id").build(),
            self._user_option(),
        )
        if not response.success() or response.data is None:
            raise FeishuClientError(
                f"Feishu primary calendar request failed: code={response.code} msg={response.msg}"
            )
        calendars = response.data.calendars or []
        if not calendars or calendars[0].calendar is None:
            raise FeishuClientError("Feishu primary calendar response was empty")
        calendar_id = calendars[0].calendar.calendar_id
        if not isinstance(calendar_id, str) or not calendar_id:
            raise FeishuClientError("Feishu primary calendar did not contain an id")
        return calendar_id

    @staticmethod
    def _event(calendar_id: str, item: Any) -> CalendarEvent | None:
        event_id = getattr(item, "event_id", None)
        if not isinstance(event_id, str) or not event_id:
            return None
        start = getattr(item, "start_time", None)
        end = getattr(item, "end_time", None)
        location = getattr(item, "location", None)
        vchat = getattr(item, "vchat", None)
        location_text = " ".join(
            value for value in (
                getattr(location, "name", None),
                getattr(location, "address", None),
            ) if isinstance(value, str) and value
        )
        meeting_url = getattr(vchat, "meeting_url", "") if vchat is not None else ""
        deleted = bool(getattr(item, "is_deleted", False)) or getattr(item, "status", "") == "cancelled"
        return CalendarEvent(
            calendar_id=calendar_id,
            event_id=event_id,
            summary=str(getattr(item, "summary", None) or "无标题日程"),
            start_timestamp=str(getattr(start, "timestamp", None) or ""),
            end_timestamp=str(getattr(end, "timestamp", None) or ""),
            start_date=str(getattr(start, "date", None) or ""),
            end_date=str(getattr(end, "date", None) or ""),
            timezone=str(getattr(start, "timezone", None) or getattr(end, "timezone", None) or "Asia/Shanghai"),
            location=location_text,
            meeting_url=str(meeting_url or ""),
            status=str(getattr(item, "status", None) or "confirmed"),
            app_link=str(getattr(item, "app_link", None) or ""),
            recurrence=str(getattr(item, "recurrence", None) or ""),
            is_deleted=deleted,
            description=str(getattr(item, "description", None) or ""),
        )

    async def calendar_instances(
        self, calendar_id: str, start_time: str, end_time: str,
    ) -> list[CalendarEvent]:
        from lark_oapi.api.calendar.v4 import InstanceViewCalendarEventRequest

        request = (InstanceViewCalendarEventRequest.builder().calendar_id(calendar_id)
                   .start_time(start_time).end_time(end_time).user_id_type("open_id").build())
        response = await self._sdk().calendar.v4.calendar_event.ainstance_view(
            request, self._user_option()
        )
        if not response.success() or response.data is None:
            raise FeishuClientError(
                f"飞书日程读取失败：code={response.code} msg={response.msg}"
            )
        return [event for item in response.data.items or []
                if (event := self._event(calendar_id, item)) is not None]

    async def create_calendar_event(
        self, calendar_id: str, draft: CalendarEventDraft, idempotency_key: str,
    ) -> CalendarEvent:
        from lark_oapi.api.calendar.v4 import (
            CalendarEvent as FeishuCalendarEvent,
            CreateCalendarEventRequest, EventLocation, TimeInfo,
        )

        start, end = TimeInfo.builder(), TimeInfo.builder()
        if draft.start_date:
            start.date(draft.start_date)
            end.date(draft.end_date)
        else:
            start.timestamp(draft.start_timestamp).timezone(draft.timezone)
            end.timestamp(draft.end_timestamp).timezone(draft.timezone)
        body = (FeishuCalendarEvent.builder().summary(draft.summary)
                .description(draft.description).start_time(start.build()).end_time(end.build()))
        if draft.location:
            body.location(EventLocation.builder().name(draft.location).build())
        request = (CreateCalendarEventRequest.builder().calendar_id(calendar_id)
                   .idempotency_key(idempotency_key).user_id_type("open_id")
                   .request_body(body.build()).build())
        response = await self._sdk().calendar.v4.calendar_event.acreate(request, self._user_option())
        if not response.success() or response.data is None:
            raise FeishuClientError(
                f"飞书日程创建失败：code={response.code} msg={response.msg}"
            )
        event = self._event(calendar_id, response.data.event)
        if event is None:
            raise FeishuClientError("飞书创建响应未包含日程")
        return event

    async def calendar_events(
        self,
        calendar_id: str,
        sync_token: str = "",
        start_time: str = "",
        end_time: str = "",
    ) -> tuple[list[CalendarEvent], str]:
        from lark_oapi.api.calendar.v4 import ListCalendarEventRequest

        page_token = ""
        events: list[CalendarEvent] = []
        next_sync_token = sync_token
        while True:
            builder = (
                ListCalendarEventRequest.builder()
                .calendar_id(calendar_id)
                .page_size(100)
                .user_id_type("open_id")
            )
            if sync_token:
                builder = builder.sync_token(sync_token)
            else:
                if start_time:
                    builder = builder.start_time(start_time)
                if end_time:
                    builder = builder.end_time(end_time)
            if page_token:
                builder = builder.page_token(page_token)
            response = await self._sdk().calendar.v4.calendar_event.alist(
                builder.build(), self._user_option()
            )
            if not response.success() or response.data is None:
                raise FeishuClientError(
                    f"Feishu calendar list failed: code={response.code} msg={response.msg}"
                )
            for item in response.data.items or []:
                event = self._event(calendar_id, item)
                if event is not None:
                    events.append(event)
            next_sync_token = str(response.data.sync_token or next_sync_token)
            if not response.data.has_more:
                break
            page_token = str(response.data.page_token or "")
            if not page_token:
                break
        return events, next_sync_token

    async def subscribe_calendar(self, calendar_id: str, user_access_token: str) -> None:
        from lark_oapi.api.calendar.v4 import SubscriptionCalendarEventRequest
        import lark_oapi as lark

        request = SubscriptionCalendarEventRequest.builder().calendar_id(calendar_id).build()
        option = lark.RequestOption.builder().user_access_token(user_access_token).build()
        response = await self._sdk().calendar.v4.calendar_event.asubscription(request, option)
        if not response.success():
            raise FeishuClientError(
                f"Feishu calendar subscription failed: code={response.code} msg={response.msg}"
            )

    def _user_option(self) -> Any:
        if not self._user_access_token:
            return None
        import lark_oapi as lark

        return lark.RequestOption.builder().user_access_token(
            self._user_access_token
        ).build()

    async def close(self) -> None:
        if self._session is not None and not self._session.closed:
            await self._session.close()


class FeishuLongConnection:
    """Run the official lark-oapi WebSocket client in a worker thread."""

    def __init__(
        self,
        app_id: str,
        app_secret: str,
        on_event: Callable[[Mapping[str, object]], Awaitable[object]],
        on_calendar_event: Callable[[Mapping[str, object]], Awaitable[object]] | None = None,
    ):
        self._app_id = app_id
        self._app_secret = app_secret
        self._on_event = on_event
        self._on_calendar_event = on_calendar_event
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._client: Any = None
        self._sdk_loop: Any = None
        self._stopping = False

    @staticmethod
    def _event_payload(data: Any) -> dict[str, object]:
        """Convert SDK generated models into the common event envelope."""
        header = getattr(data, "header", None)
        event = getattr(data, "event", None)
        message = getattr(event, "message", None)
        sender = getattr(event, "sender", None)
        sender_id = getattr(sender, "sender_id", None)
        values: dict[str, object] = {
            "header": {
                "event_id": getattr(header, "event_id", ""),
                "event_type": "im.message.receive_v1",
            },
            "event": {
                "sender": {
                    "sender_id": {
                        "open_id": getattr(sender_id, "open_id", ""),
                        "user_id": getattr(sender_id, "user_id", ""),
                        "union_id": getattr(sender_id, "union_id", ""),
                    }
                },
                "message": {
                    "message_id": getattr(message, "message_id", ""),
                    "chat_id": getattr(message, "chat_id", ""),
                    "chat_type": getattr(message, "chat_type", "p2p"),
                    "message_type": getattr(message, "message_type", ""),
                    "content": getattr(message, "content", ""),
                },
            },
        }
        return values

    @staticmethod
    def _calendar_event_payload(data: Any) -> dict[str, object]:
        header = getattr(data, "header", None)
        event = getattr(data, "event", None)
        return {
            "header": {
                "event_id": getattr(header, "event_id", ""),
                "event_type": "calendar.calendar.event.changed_v4",
                "token": getattr(header, "token", ""),
            },
            "event": {
                "calendar_id": getattr(event, "calendar_id", ""),
                "calendar_event_id": getattr(event, "calendar_event_id", ""),
                "change_type": getattr(event, "change_type", ""),
            },
        }

    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            return
        if importlib.util.find_spec("lark_oapi") is None:
            raise FeishuClientError(
                "lark-oapi is required for Feishu long_connection; "
                "install binbot-voice-server[feishu]"
            )
        # Import the SDK on the event-loop thread before the worker starts.
        # Calendar polling and the WebSocket worker may otherwise import the
        # generated calendar package concurrently and deadlock on its module lock.
        import lark_oapi  # noqa: F401
        self._loop = asyncio.get_running_loop()
        self._stopping = False
        self._thread = threading.Thread(
            target=self._run_client,
            name="binbot-feishu-long-connection",
            daemon=True,
        )
        self._thread.start()

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def _run_client(self) -> None:
        import lark_oapi as lark
        from lark_oapi import ws
        from lark_oapi.ws import client as ws_module

        # lark-oapi's WebSocket client calls ``loop.run_until_complete`` on a
        # module-level loop. The server already owns its main asyncio loop, so
        # the SDK must get a private loop in this worker thread.
        sdk_loop = asyncio.new_event_loop()
        asyncio.set_event_loop(sdk_loop)
        self._sdk_loop = sdk_loop
        loop = self._loop
        if loop is None:
            sdk_loop.close()
            return

        def callback(data: Any) -> None:
            payload = self._event_payload(data)
            async def dispatch() -> object:
                return await self._on_event(payload)

            future = asyncio.run_coroutine_threadsafe(dispatch(), loop)
            try:
                future.result(timeout=30)
            except Exception:
                _LOG.exception("Feishu long-connection event handling failed")

        def calendar_callback(data: Any) -> None:
            if self._on_calendar_event is None:
                return
            payload = self._calendar_event_payload(data)
            calendar_handler = self._on_calendar_event

            async def dispatch() -> object:
                return await calendar_handler(payload)

            future = asyncio.run_coroutine_threadsafe(dispatch(), loop)
            try:
                future.result(timeout=30)
            except Exception:
                _LOG.exception("Feishu calendar event handling failed")

        handler_builder = (
            lark.EventDispatcherHandler.builder("", "")
            .register_p2_im_message_receive_v1(callback)
        )
        if self._on_calendar_event is not None:
            handler_builder = handler_builder.register_p2_calendar_calendar_event_changed_v4(
                calendar_callback
            )
        handler = handler_builder.build()
        # The SDK stores its loop at module scope. Keep receive and connect
        # tasks on this instance's loop so different apps cannot replace it.
        class IsolatedClient(ws.Client):  # type: ignore[misc]
            async def _connect(client_self: Any) -> None:
                from urllib.parse import parse_qs, urlparse
                async with client_self._lock:
                    if client_self._conn is not None:
                        return
                    try:
                        conn_url = client_self._get_conn_url()
                        query = parse_qs(urlparse(conn_url).query)
                        client_self._conn = await ws_module.websockets.connect(
                            conn_url, **ws_module._ws_connect_kwargs()
                        )
                        client_self._conn_url = conn_url
                        client_self._conn_id = query[ws_module.DEVICE_ID][0]
                        client_self._service_id = query[ws_module.SERVICE_ID][0]
                        sdk_loop.create_task(client_self._receive_message_loop())
                    except ws_module.InvalidHandshake as exc:
                        ws_module._parse_ws_conn_exception(exc)

            async def _receive_message_loop(client_self: Any) -> None:
                try:
                    while client_self._conn is not None:
                        message = await client_self._conn.recv()
                        sdk_loop.create_task(client_self._handle_message(message))
                except asyncio.CancelledError:
                    raise
                except Exception:
                    await client_self._disconnect()
                    if client_self._auto_reconnect:
                        await client_self._reconnect()

        self._client = IsolatedClient(
            self._app_id,
            self._app_secret,
            event_handler=handler,
            log_level=lark.LogLevel.INFO,
            auto_reconnect=True,
        )
        ws_client = self._client
        try:
            async def run() -> None:
                try:
                    await ws_client._connect()
                except Exception:
                    await ws_client._disconnect()
                    await ws_client._reconnect()
                sdk_loop.create_task(ws_client._ping_loop())
                await asyncio.Future()

            sdk_loop.run_until_complete(run())
        except (RuntimeError, asyncio.CancelledError):
            if not self._stopping:
                _LOG.exception("Feishu long connection stopped")
        except Exception:
            if not self._stopping:
                _LOG.exception("Feishu long connection failed")
        finally:
            if not sdk_loop.is_closed():
                ws_client._auto_reconnect = False
                for task in asyncio.all_tasks(sdk_loop):
                    task.cancel()
                sdk_loop.run_until_complete(asyncio.gather(*asyncio.all_tasks(sdk_loop), return_exceptions=True))
                sdk_loop.run_until_complete(ws_client._disconnect())
                sdk_loop.close()

    async def stop(self) -> None:
        thread = self._thread
        self._stopping = True
        self._client = None
        sdk_loop = self._sdk_loop
        if sdk_loop is not None and not sdk_loop.is_closed():
            def request_stop() -> None:
                pending = [task for task in asyncio.all_tasks() if not task.done()]
                for pending_task in pending:
                    pending_task.cancel()
                sdk_loop.call_later(0.1, sdk_loop.stop)

            sdk_loop.call_soon_threadsafe(request_stop)
        if thread is not None:
            await asyncio.to_thread(thread.join, 2.0)
        self._thread = None
        self._sdk_loop = None
