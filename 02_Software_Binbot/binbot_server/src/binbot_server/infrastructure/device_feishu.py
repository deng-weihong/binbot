"""Device-owned Feishu configuration, grants, calendar caches and message routes."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import hashlib
import hmac
import importlib.util
import json
import logging
import os
from pathlib import Path
import re
import time
from typing import Any
from uuid import UUID, uuid4

from binbot_server.application.calendar import CalendarSyncService
from binbot_server.application.feishu import FeishuGateway, parse_message
from binbot_server.config import Settings
from .feishu import FeishuClientError, FeishuLongConnection, FeishuOAuthManager, FeishuTokenStore, HttpFeishuClient
from .sqlite import SqliteCalendarStore

_LOG = logging.getLogger("binbot.device_feishu")
_DEFAULT = {
    "enabled": False, "app_id": "", "calendar_id": "", "calendar_enabled": False,
    "messages_enabled": False, "chat_id": "", "allow_control": False,
    "mode": "long_connection",
}


class DeviceFeishuError(ValueError):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


@dataclass
class DeviceFeishuRuntime:
    document: dict[str, Any]
    client: HttpFeishuClient
    gateway: FeishuGateway
    oauth: FeishuOAuthManager
    calendar: CalendarSyncService
    error: str = ""
    active: bool = True


class DeviceFeishuManager:
    def __init__(self, settings: Settings, agent: Any, conversations: Any, registry: Any):
        self.settings = settings
        self.agent = agent
        self.conversations = conversations
        self.registry = registry
        self.root = Path(settings.component.directory) / "device-feishu"
        self.documents: dict[str, dict[str, Any]] = {}
        self.runtimes: dict[str, DeviceFeishuRuntime] = {}
        self.connections: dict[str, FeishuLongConnection] = {}
        self._connection_keys: dict[str, str] = {}
        self._pending: dict[str, tuple[str, DeviceFeishuRuntime, float, str, FeishuOAuthManager]] = {}
        self._corrupt: set[str] = set()
        self._lock = asyncio.Lock()
        self._started = False
        self._load()
        self._migrate_legacy()

    def directory(self, device_id: str) -> Path:
        if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", device_id) or device_id in {".", ".."}:
            raise DeviceFeishuError(400, "设备编号无效。")
        return self.root / hashlib.sha256(device_id.encode()).hexdigest()

    def _empty(self, device_id: str) -> dict[str, Any]:
        return {"device_id": device_id, "revision": "initial", "config": dict(_DEFAULT),
                "secrets": {}, "grant_epoch": str(uuid4())}

    def _load(self) -> None:
        if not self.root.exists():
            return
        for path in self.root.glob("*/configuration.json"):
            try:
                document = json.loads(path.read_text(encoding="utf-8"))
                device_id = document["device_id"]
                if self.directory(device_id) / "configuration.json" != path:
                    raise ValueError("device directory mismatch")
                self._validate_config(document["config"])
                if not isinstance(document["revision"], str) or not isinstance(document["secrets"], dict):
                    raise ValueError("invalid stored configuration")
                UUID(document["grant_epoch"])
                if document["config"]["enabled"] and not isinstance(document["secrets"].get("app_secret"), str):
                    raise ValueError("missing device credentials")
                self.documents[device_id] = document
            except (ValueError, KeyError, TypeError, OSError):
                # One damaged device must not prevent the others from starting.
                self._corrupt.add(path.parent.name)
                _LOG.error("unreadable device Feishu configuration directory=%s", path.parent.name)

    def _persist(self, document: dict[str, Any]) -> None:
        directory = self.directory(document["device_id"])
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        directory.chmod(0o700)
        temporary = directory / f".configuration-{uuid4().hex}.tmp"
        try:
            with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
                json.dump(document, stream, ensure_ascii=False)
                stream.flush()
                os.fsync(stream.fileno())
            temporary.replace(directory / "configuration.json")
        finally:
            temporary.unlink(missing_ok=True)

    def _migrate_legacy(self) -> None:
        legacy = self.settings.feishu
        device_id = legacy.device_id.strip()
        # A global grant with no explicit target cannot be assigned to devices.
        if not legacy.enabled or not device_id or device_id in self.documents:
            return
        if self.directory(device_id).name in self._corrupt:
            return
        app_id, secret = legacy.credentials()
        document = self._empty(device_id)
        document["revision"] = str(uuid4())
        document["config"].update({
            "enabled": True, "app_id": app_id, "calendar_id": legacy.calendar.calendar_id,
            "calendar_enabled": legacy.calendar.enabled, "mode": legacy.mode,
            # Legacy conversations have no explicit chat binding; do not guess.
            "allow_control": legacy.allow_control,
        })
        document["secrets"] = {"app_secret": secret, "verification_token": legacy.verification_token()}
        grant = FeishuTokenStore(legacy.calendar.token_file).load()
        self.directory(device_id).mkdir(parents=True, exist_ok=True, mode=0o700)
        if grant:
            FeishuTokenStore(str(self.directory(device_id) / f"grant-{document['grant_epoch']}.json")).save(grant)
        self._persist(document)
        self.documents[device_id] = document

    @staticmethod
    def _validate_config(config: Any) -> dict[str, Any]:
        if not isinstance(config, dict) or set(config) != set(_DEFAULT):
            raise DeviceFeishuError(400, "飞书配置不完整，请重新读取后保存。")
        for key in ("enabled", "calendar_enabled", "messages_enabled", "allow_control"):
            if not isinstance(config[key], bool):
                raise DeviceFeishuError(400, "飞书开关配置无效。")
        for key in ("app_id", "calendar_id", "chat_id", "mode"):
            if not isinstance(config[key], str) or len(config[key]) > 256 or any(ord(c) < 32 for c in config[key]):
                raise DeviceFeishuError(400, "飞书配置格式无效。")
        if config["mode"] not in {"long_connection", "webhook"}:
            raise DeviceFeishuError(400, "不支持此飞书接入方式。")
        if config["app_id"] and not re.fullmatch(r"[A-Za-z0-9_-]+", config["app_id"]):
            raise DeviceFeishuError(400, "App ID 格式无效。")
        if config["enabled"] and not config["app_id"].strip():
            raise DeviceFeishuError(400, "请填写当前设备的飞书 App ID。")
        if config["messages_enabled"] and not config["chat_id"].strip():
            raise DeviceFeishuError(400, "请填写绑定到当前设备的飞书会话 ID。")
        return {key: value.strip() if isinstance(value, str) else value for key, value in config.items()}

    async def snapshot(self, device_id: str) -> dict[str, Any]:
        self.directory(device_id)
        if self.directory(device_id).name in self._corrupt:
            raise DeviceFeishuError(503, "当前设备的飞书配置无法读取，请检查服务端存储。")
        document = self.documents.get(device_id) or self._empty(device_id)
        runtime = self.runtimes.get(device_id)
        account = runtime.oauth.status() if runtime else {"authorized": False, "display_name": ""}
        calendar = await runtime.calendar.status() if runtime and account["authorized"] else None
        connection = self.connections.get(document["config"]["app_id"])
        return {
            "device_id": device_id, "revision": document["revision"],
            "config": document["config"],
            "credentials_configured": {key: bool(document["secrets"].get(key)) for key in ("app_secret", "verification_token")},
            "account": account, "connected": bool(connection and connection.running),
            "error": runtime.error if runtime else "", "calendar": calendar,
            "callback_path": "/api/v1/feishu/oauth/callback",
        }

    async def save(self, device_id: str, body: Any) -> dict[str, Any]:
        self.directory(device_id)
        if self.directory(device_id).name in self._corrupt:
            raise DeviceFeishuError(503, "当前设备的飞书配置无法读取，请检查服务端存储。")
        if not isinstance(body, dict) or set(body) - {"revision", "config", "secrets"}:
            raise DeviceFeishuError(400, "飞书配置请求无效。")
        async with self._lock:
            previous = self.documents.get(device_id) or self._empty(device_id)
            if body.get("revision") != previous["revision"]:
                raise DeviceFeishuError(409, "当前设备的飞书配置已被修改，请重新读取后保存。")
            config = self._validate_config(body.get("config"))
            secrets = body.get("secrets", {})
            if not isinstance(secrets, dict) or set(secrets) - {"app_secret", "verification_token"}:
                raise DeviceFeishuError(400, "飞书凭证格式无效。")
            if any(not isinstance(value, str) or not value.strip() or len(value) > 4096 for value in secrets.values()):
                raise DeviceFeishuError(400, "飞书凭证不能为空或超过 4096 字符。")
            changed_app = config["app_id"] != previous["config"]["app_id"]
            merged = {**({} if changed_app else previous["secrets"]), **{key: value.strip() for key, value in secrets.items()}}
            if config["enabled"] and not merged.get("app_secret"):
                raise DeviceFeishuError(400, "请填写当前应用的 App Secret。")
            if config["enabled"] and config["mode"] == "webhook" and not merged.get("verification_token"):
                raise DeviceFeishuError(400, "回调方式需要配置 Verification Token。")
            for other_id, other in self.documents.items():
                if other_id == device_id or not other["config"]["enabled"] or not config["enabled"]:
                    continue
                other_config = other["config"]
                if other_config["app_id"] != config["app_id"]:
                    continue
                if (other["secrets"].get("app_secret") != merged.get("app_secret") or
                        other_config["mode"] != config["mode"] or
                        (config["mode"] == "webhook" and other["secrets"].get("verification_token", "") != merged.get("verification_token", ""))):
                    raise DeviceFeishuError(409, "同一飞书应用的凭证和接入方式需保持一致。")
                if config["messages_enabled"] and other_config["messages_enabled"] and config["chat_id"] == other_config["chat_id"]:
                    raise DeviceFeishuError(409, "该飞书会话已绑定其他设备，请使用不同会话。")
            if config["enabled"] and config["mode"] == "long_connection" and importlib.util.find_spec("lark_oapi") is None:
                raise DeviceFeishuError(503, "服务端尚未安装飞书连接依赖。")
            invalidate = changed_app or merged.get("app_secret") != previous["secrets"].get("app_secret")
            document: dict[str, Any] = {"device_id": device_id, "revision": str(uuid4()), "config": config,
                        "secrets": merged, "grant_epoch": str(uuid4()) if invalidate else previous["grant_epoch"]}
            self._persist(document)
            old = self.runtimes.pop(device_id, None)
            if old:
                await self._close_runtime(old)
            if invalidate:
                self._discard_old_caches(device_id, document["grant_epoch"])
            self.documents[device_id] = document
            if config["enabled"]:
                self.runtimes[device_id] = self._build(document)
            if invalidate or not config["enabled"] or not config["calendar_enabled"]:
                await self.clear_calendar(device_id)
            await self._reconcile_connections()
            if self._started and device_id in self.runtimes:
                await self._restore(self.runtimes[device_id])
        return await self.snapshot(device_id)

    def _build(self, document: dict[str, Any]) -> DeviceFeishuRuntime:
        device_id, config = document["device_id"], document["config"]
        directory = self.directory(device_id)
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        client = HttpFeishuClient(self.settings.feishu.base_url, config["app_id"], document["secrets"]["app_secret"], self.settings.feishu.request_timeout_seconds)
        # A separate cache per device and grant prevents equal calendar IDs in
        # different apps/accounts from sharing events or sync cursors.
        cache = directory / f"calendar-{document['grant_epoch']}.sqlite3"
        store = SqliteCalendarStore(str(cache))
        cache.chmod(0o600)
        runtime: DeviceFeishuRuntime

        async def prepare() -> None:
            if not runtime.active or not await runtime.oauth.ensure_fresh_token():
                raise FeishuClientError("请先连接当前设备的飞书账号。")

        async def publish(calendar_id: str, events: Any, changes: Any) -> None:
            if not runtime.active or not runtime.oauth.status()["authorized"]:
                return
            status = await store.status(calendar_id)
            try:
                await self.registry.send_event(device_id, {
                    "type": "device.calendar", "calendar_id": calendar_id,
                    "revision": status.get("revision", 0), "changes": changes.snapshot(),
                    "events": [event.snapshot(include_description=False) for event in events],
                })
            except Exception:
                _LOG.debug("calendar snapshot pending for device=%s", device_id)

        calendar = CalendarSyncService(client, store, calendar_id=config["calendar_id"],
                                       # Device calendars do not subscribe to change events.
                                       # A legacy zero interval would otherwise make the
                                       # only pull stop after startup, leaving a stale cache.
                                       interval_seconds=self.settings.feishu.calendar.sync_interval_seconds or 60.0,
                                       max_events=self.settings.feishu.calendar.max_events,
                                       subscribe=False, prepare=prepare, notify=publish)

        async def apply_token(token: str) -> None:
            client.set_user_access_token(token)
            await calendar.set_user_access_token(token)

        oauth = FeishuOAuthManager(client, FeishuTokenStore(str(directory / f"grant-{document['grant_epoch']}.json")), config["app_id"],
                                  tuple(self.settings.feishu.calendar.oauth_scopes), apply_token)
        gateway = FeishuGateway(self.agent, self.conversations, client, "chat_id", device_id,
                                config["allow_control"], conversation_namespace=device_id)
        runtime = DeviceFeishuRuntime(document, client, gateway, oauth, calendar)
        return runtime

    async def _restore(self, runtime: DeviceFeishuRuntime) -> None:
        if await runtime.oauth.restore() and runtime.document["config"]["calendar_enabled"]:
            runtime.calendar.start()
        elif runtime.oauth.status()["authorized"] and not runtime.document["config"]["calendar_enabled"]:
            runtime.error = ""
        elif runtime.oauth.status()["authorized"]:
            runtime.error = "飞书授权暂不可用，请重新连接账号。"

    def runtime(self, device_id: str) -> DeviceFeishuRuntime:
        self.directory(device_id)
        runtime = self.runtimes.get(device_id)
        if not runtime or not runtime.active:
            raise DeviceFeishuError(404, "当前设备尚未启用飞书连接。")
        return runtime

    def calendar(self, device_id: str) -> CalendarSyncService | None:
        runtime = self.runtimes.get(device_id)
        if runtime and runtime.active and runtime.document["config"]["calendar_enabled"] and runtime.oauth.status()["authorized"]:
            return runtime.calendar
        return None

    async def calendar_status(self, device_id: str) -> dict[str, Any]:
        self.directory(device_id)
        document = self.documents.get(device_id)
        runtime = self.runtimes.get(device_id)
        enabled = bool(document and document["config"]["enabled"] and document["config"]["calendar_enabled"])
        authorized = bool(runtime and runtime.active and runtime.oauth.status()["authorized"])
        status = dict(await runtime.calendar.status()) if runtime and enabled else {}
        return {"enabled": enabled, "authorized": authorized,
                "last_sync_at": status.get("last_sync_at", ""),
                "poll_interval_seconds": status.get("poll_interval_seconds", 0),
                "polling": status.get("polling", False),
                "error": status.get("error") or (runtime.error if runtime else "")}

    def calendar_scope(self, device_id: str) -> str:
        runtime = self.runtime(device_id)
        document = runtime.document
        return hashlib.sha256(json.dumps([
            document["config"]["app_id"], document["grant_epoch"], document["config"]["calendar_id"],
        ]).encode()).hexdigest()

    async def authorize(self, device_id: str, redirect_uri: str) -> str:
        async with self._lock:
            runtime = self.runtime(device_id)
            self._pending = {key: row for key, row in self._pending.items() if row[2] > time.time()}
            epoch = str(uuid4())

            async def stage_token(token: str) -> None:
                pass  # Apply only after both the new grant and device config commit.

            staged = FeishuOAuthManager(runtime.client,
                FeishuTokenStore(str(self.directory(device_id) / f"grant-{epoch}.json")),
                runtime.document["config"]["app_id"], tuple(self.settings.feishu.calendar.oauth_scopes), stage_token)
            url = staged.authorization_url(redirect_uri)
            from urllib.parse import parse_qs, urlsplit
            state = parse_qs(urlsplit(url).query)["state"][0]
            self._pending[state] = (device_id, runtime, time.time() + 600, epoch, staged)
            return url

    def owns_state(self, state: str) -> bool:
        return state in self._pending

    async def callback(self, code: str, state: str) -> tuple[str, dict[str, object]]:
        async with self._lock:
            pending = self._pending.pop(state, None)
            if not pending or pending[2] <= time.time():
                raise DeviceFeishuError(400, "飞书授权已失效，请从设备页面重新连接。")
            device_id, runtime, _, epoch, staged = pending
            if self.runtimes.get(device_id) is not runtime or not runtime.active:
                raise DeviceFeishuError(409, "设备配置已变化，请重新连接飞书。")
            # Stage a replacement account in a new grant/cache namespace so a
            # failed commit or restart cannot attach it to the former calendar.
            try:
                account = await staged.handle_callback(code, state)
                document = {**runtime.document, "grant_epoch": epoch}
                self._persist(document)
            except Exception:
                FeishuTokenStore(str(self.directory(device_id) / f"grant-{epoch}.json")).clear()
                raise
            await self._close_runtime(runtime)
            self._discard_old_caches(device_id, document["grant_epoch"])
            self.documents[device_id] = document
            runtime = self._build(document)
            self.runtimes[device_id] = runtime
            # Do not leave the former account's events on the device if the
            # replacement account's first calendar sync fails.
            await self.clear_calendar(device_id)
            await self._restore(runtime)
            return device_id, account

    async def disconnect(self, device_id: str) -> None:
        async with self._lock:
            runtime = self.runtime(device_id)
            await runtime.calendar.stop()
            await runtime.oauth.disconnect()
            self._pending = {key: row for key, row in self._pending.items() if row[0] != device_id}
            document = {**runtime.document, "grant_epoch": str(uuid4())}
            self._persist(document)
            await self._close_runtime(runtime)
            self._discard_old_caches(device_id, document["grant_epoch"])
            self.documents[device_id] = document
            self.runtimes[device_id] = self._build(document)
            await self.clear_calendar(device_id)

    async def clear_calendar(self, device_id: str) -> None:
        try:
            await self.registry.send_event(device_id, {"type": "device.calendar", "calendar_id": "", "revision": 0, "events": []})
        except Exception:
            pass  # Device will receive its own snapshot after reconnecting.

    def _discard_old_caches(self, device_id: str, keep_epoch: str) -> None:
        for path in self.directory(device_id).glob("calendar-*.sqlite3*"):
            if not path.name.startswith(f"calendar-{keep_epoch}.sqlite3"):
                path.unlink(missing_ok=True)
        for path in self.directory(device_id).glob("grant-*.json"):
            if path.name != f"grant-{keep_epoch}.json":
                path.unlink(missing_ok=True)

    async def dispatch(self, app_id: str, payload: Any) -> dict[str, object]:
        message = parse_message(payload)
        if message is None:
            header = payload.get("header", {})
            if header.get("event_type") == "calendar.calendar.event.changed_v4":
                for device_id, runtime in list(self.runtimes.items()):
                    if runtime.document["config"]["app_id"] == app_id and self.calendar(device_id):
                        await runtime.calendar.handle_event(payload)
            return {"status": "ignored"}
        matches = [runtime for runtime in self.runtimes.values() if runtime.active and
                   runtime.document["config"]["app_id"] == app_id and
                   runtime.document["config"]["messages_enabled"] and
                   runtime.document["config"]["chat_id"] == message.chat_id]
        if len(matches) != 1:
            return {"status": "ignored"}
        return await matches[0].gateway.handle(payload)

    async def webhook(self, payload: Any) -> dict[str, object]:
        if not isinstance(payload, dict):
            raise DeviceFeishuError(400, "飞书事件格式无效。")
        header = payload.get("header") or {}
        if not isinstance(header, dict):
            raise DeviceFeishuError(400, "飞书事件格式无效。")
        supplied = payload.get("token") if payload.get("type") == "url_verification" else header.get("token")
        app_id = header.get("app_id") or payload.get("app_id")
        matches = [row for row in self.documents.values() if row["config"]["enabled"] and row["config"]["mode"] == "webhook" and
                   (not app_id or row["config"]["app_id"] == app_id) and isinstance(supplied, str) and
                   row["secrets"].get("verification_token") and hmac.compare_digest(row["secrets"]["verification_token"], supplied)]
        apps = {row["config"]["app_id"] for row in matches}
        if len(apps) != 1:
            raise DeviceFeishuError(401, "飞书事件凭证无效。")
        if payload.get("type") == "url_verification":
            challenge = payload.get("challenge")
            if not isinstance(challenge, str) or not challenge:
                raise DeviceFeishuError(400, "飞书回调缺少 challenge。")
            return {"challenge": challenge}
        return await self.dispatch(apps.pop(), payload)

    async def _reconcile_connections(self) -> None:
        if not self._started:
            return
        wanted = {row["config"]["app_id"]: row for row in self.documents.values()
                  if row["config"]["enabled"] and row["config"]["mode"] == "long_connection" and row["config"]["messages_enabled"]}
        for app_id in list(self.connections):
            if app_id not in wanted or self._connection_keys[app_id] != wanted[app_id]["secrets"]["app_secret"]:
                await self.connections.pop(app_id).stop()
                self._connection_keys.pop(app_id, None)
        for app_id, document in wanted.items():
            if app_id not in self.connections:
                async def on_event(payload: Any, app: str = app_id) -> object:
                    return await self.dispatch(app, payload)
                connection = FeishuLongConnection(app_id, document["secrets"]["app_secret"], on_event, on_event)
                try:
                    connection.start()
                except FeishuClientError:
                    for runtime in self.runtimes.values():
                        if runtime.document["config"]["app_id"] == app_id:
                            runtime.error = "飞书消息连接未启动，请检查服务端飞书依赖。"
                    continue
                self.connections[app_id] = connection
                self._connection_keys[app_id] = document["secrets"]["app_secret"]

    async def _close_runtime(self, runtime: DeviceFeishuRuntime) -> None:
        runtime.active = False
        await runtime.calendar.close()
        await runtime.gateway.close()

    async def start(self) -> None:
        self._started = True
        for device_id, document in self.documents.items():
            if document["config"]["enabled"]:
                runtime = self._build(document)
                self.runtimes[device_id] = runtime
                await self._restore(runtime)
        await self._reconcile_connections()

    async def close(self) -> None:
        self._started = False
        for connection in self.connections.values():
            await connection.stop()
        self.connections.clear()
        for runtime in self.runtimes.values():
            await self._close_runtime(runtime)
        self.runtimes.clear()
        self._pending.clear()
