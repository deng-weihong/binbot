import asyncio
import time
from dataclasses import replace
from types import SimpleNamespace
from urllib.parse import parse_qs, urlsplit
from unittest.mock import AsyncMock

import pytest

from binbot_server.application.calendar import CalendarEvent
from binbot_server.config import Settings
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.device_feishu import DeviceFeishuError, DeviceFeishuManager
from binbot_server.infrastructure.feishu import FeishuTokenStore


class FakeClient:
    def __init__(self, base_url, app_id, secret, timeout):
        self.base_url, self.app_id, self.secret = base_url, app_id, secret
        self.token, self.sent = "", []

    def set_user_access_token(self, token):
        self.token = token

    async def exchange_user_code(self, code):
        return {"access_token": f"access-{code}", "refresh_token": f"refresh-{code}", "expires_in": 7200, "name": code}

    async def refresh_user_token(self, token):
        return {"access_token": f"refreshed-{token}", "expires_in": 7200}

    async def calendar_primary(self):
        return "same-calendar-id"

    async def calendar_events(self, calendar_id, **kwargs):
        return [CalendarEvent(calendar_id, "same-event", self.token, start_date="2026-11-18", end_date="2026-11-19")], f"cursor-{self.token}"

    async def calendar_instances(self, calendar_id, start, end):
        return (await self.calendar_events(calendar_id))[0]

    async def create_calendar_event(self, calendar_id, draft, key):
        return CalendarEvent(calendar_id, key, draft.summary, start_date="2026-11-18", end_date="2026-11-19")

    async def send_text(self, receive_id, text, kind):
        self.sent.append((receive_id, text))

    async def close(self):
        pass


class FakeConnection:
    def __init__(self, app_id, secret, event, calendar_event):
        self.app_id, self.event, self.running = app_id, event, False

    def start(self):
        self.running = True

    async def stop(self):
        self.running = False


def make_manager(tmp_path, monkeypatch):
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.HttpFeishuClient", FakeClient)
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.FeishuLongConnection", FakeConnection)
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.importlib.util.find_spec", lambda name: True)
    defaults = Settings()
    settings = replace(defaults, component=replace(defaults.component, directory=str(tmp_path)),
                       feishu=replace(defaults.feishu, calendar=replace(defaults.feishu.calendar, sync_interval_seconds=0)))
    registry, agent = AsyncMock(), AsyncMock()
    agent.build_context.side_effect = lambda device_id, text, **kwargs: SimpleNamespace(conversation_id=kwargs["conversation_id"])
    agent.run.return_value = SimpleNamespace(text="已收到")
    return DeviceFeishuManager(settings, agent, InMemoryConversationStore(10), registry)


async def configure(manager, device_id, app_id="cli_shared", chat_id="", calendar=True):
    snapshot = await manager.snapshot(device_id)
    config = {**snapshot["config"], "enabled": True, "app_id": app_id,
              "messages_enabled": bool(chat_id), "chat_id": chat_id, "calendar_enabled": calendar}
    return await manager.save(device_id, {"revision": snapshot["revision"], "config": config,
                                          "secrets": {"app_secret": f"secret-{app_id}"}})


async def authorize(manager, device_id, name):
    url = await manager.authorize(device_id, "https://console.test/api/v1/feishu/oauth/callback")
    state = parse_qs(urlsplit(url).query)["state"][0]
    result = await manager.callback(name, state)
    await manager.runtime(device_id).calendar.stop()
    return result


def event(chat_id, event_id="same-event"):
    return {"header": {"event_type": "im.message.receive_v1", "event_id": event_id},
            "event": {"message": {"message_id": "msg", "chat_id": chat_id, "chat_type": "p2p", "message_type": "text", "content": '{"text":"hello"}'},
                      "sender": {"sender_id": {"open_id": "same-user"}}}}


def test_device_configuration_persists_privately_and_never_inherits_another_devices_secrets(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            first = await configure(manager, "device-a", "cli_a")
            second = await configure(manager, "device-b", "cli_b")
            assert first["config"]["app_id"] == "cli_a"
            assert second["config"]["app_id"] == "cli_b"
            assert "secret-cli" not in str(first) + str(second)
            assert (manager.directory("device-a") / "configuration.json").stat().st_mode & 0o777 == 0o600
            with pytest.raises(DeviceFeishuError, match="已被修改"):
                await manager.save("device-a", {"revision": "initial", "config": first["config"]})
            with pytest.raises(DeviceFeishuError, match="App Secret"):
                await manager.save("device-a", {"revision": first["revision"], "config": {**first["config"], "app_id": "cli_other"}})
            restored = make_manager(tmp_path, monkeypatch)
            assert (await restored.snapshot("device-b"))["config"]["app_id"] == "cli_b"
            assert (await manager.snapshot("unconfigured"))["credentials_configured"]["app_secret"] is False
            with pytest.raises(DeviceFeishuError):
                await manager.snapshot("../outside")
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_grants_same_named_calendars_and_disconnect_are_isolated_per_device(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            await configure(manager, "device-a")
            await configure(manager, "device-b")
            assert (await authorize(manager, "device-a", "Alice"))[0] == "device-a"
            assert (await authorize(manager, "device-b", "Bob"))[0] == "device-b"
            await manager.calendar("device-a").sync_once()
            await manager.calendar("device-b").sync_once()
            assert (await manager.calendar("device-a").snapshot())[0].summary == "access-Alice"
            assert (await manager.calendar("device-b").snapshot())[0].summary == "access-Bob"
            assert (await manager.snapshot("device-a"))["account"]["display_name"] == "Alice"
            assert "access-Alice" not in str(await manager.snapshot("device-a"))
            await manager.disconnect("device-a")
            assert manager.calendar("device-a") is None
            assert (await manager.snapshot("device-b"))["account"]["authorized"]
            assert (await manager.calendar("device-b").snapshot())[0].summary == "access-Bob"
            await authorize(manager, "device-a", "Carol")
            assert list(await manager.calendar("device-a").snapshot()) == []
            await manager.calendar("device-a").sync_once()
            assert (await manager.calendar("device-a").snapshot())[0].summary == "access-Carol"
            manager.registry.send_event.reset_mock()
            await authorize(manager, "device-a", "Dora")
            assert list(await manager.calendar("device-a").snapshot()) == []
            manager.registry.send_event.assert_awaited_once_with(
                "device-a", {"type": "device.calendar", "calendar_id": "", "revision": 0, "events": []}
            )
            assert (await manager.calendar("device-b").snapshot())[0].summary == "access-Bob"
            await manager.close()
            restored = make_manager(tmp_path, monkeypatch)
            try:
                await restored.start()
                await restored.runtime("device-a").calendar.stop()
                await restored.runtime("device-b").calendar.stop()
                assert (await restored.snapshot("device-b"))["account"]["display_name"] == "Bob"
            finally:
                await restored.close()
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_shared_app_has_one_connection_and_routes_only_the_bound_chat_with_separate_conversations(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            await configure(manager, "device-a", chat_id="chat-a", calendar=False)
            await configure(manager, "device-b", chat_id="chat-b", calendar=False)
            await manager.start()
            assert len(manager.connections) == 1
            assert (await manager.dispatch("cli_shared", event("unbound")))["status"] == "ignored"
            assert (await manager.dispatch("cli_shared", event("chat-a")))["status"] == "ok"
            assert (await manager.dispatch("cli_shared", event("chat-a")))["status"] == "duplicate"
            assert (await manager.dispatch("cli_shared", event("chat-b")))["status"] == "ok"
            calls = manager.agent.build_context.call_args_list
            assert [call.args[0] for call in calls] == ["device-a", "device-b"]
            assert calls[0].kwargs["conversation_id"] != calls[1].kwargs["conversation_id"]
            first = await manager.snapshot("device-b")
            with pytest.raises(DeviceFeishuError, match="绑定其他设备"):
                await manager.save("device-b", {"revision": first["revision"], "config": {**first["config"], "chat_id": "chat-a"}})
            with pytest.raises(DeviceFeishuError, match="凭证"):
                await manager.save("device-b", {"revision": first["revision"], "config": first["config"], "secrets": {"app_secret": "different"}})
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_reconfigured_or_disconnected_device_cannot_accept_an_old_oauth_callback(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            first = await configure(manager, "device-a")
            url = await manager.authorize("device-a", "https://console.test/api/v1/feishu/oauth/callback")
            state = parse_qs(urlsplit(url).query)["state"][0]
            await manager.save("device-a", {"revision": first["revision"], "config": {**first["config"], "calendar_id": "other"}})
            with pytest.raises(DeviceFeishuError, match="配置已变化"):
                await manager.callback("old-account", state)
            assert not (await manager.snapshot("device-a"))["account"]["authorized"]
            await authorize(manager, "device-a", "new-account")
            with pytest.raises(DeviceFeishuError):
                await manager.callback("replay", state)
            await manager.disconnect("device-a")
            assert not (await manager.snapshot("device-a"))["account"]["authorized"]
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_legacy_grant_migrates_only_to_its_explicit_target_and_never_to_all_devices(tmp_path, monkeypatch):
    manager = make_manager(tmp_path, monkeypatch)
    monkeypatch.setenv("BINBOT_FEISHU_MIGRATION_TEST_SECRET", "migration-secret")
    grant_path = tmp_path / "legacy-grant.json"
    FeishuTokenStore(str(grant_path)).save({"access_token": "legacy-access", "refresh_token": "legacy-refresh", "name": "Legacy account", "expires_at": str(int(time.time()) + 7200)})
    legacy = replace(manager.settings.feishu, enabled=True, app_id="cli_legacy", app_secret_env="BINBOT_FEISHU_MIGRATION_TEST_SECRET",
                     device_id="device-a", calendar=replace(manager.settings.feishu.calendar, token_file=str(grant_path)))
    settings = replace(manager.settings, feishu=legacy)

    async def scenario():
        migrated = DeviceFeishuManager(settings, manager.agent, manager.conversations, manager.registry)
        try:
            await migrated.start()
            assert (await migrated.snapshot("device-a"))["account"]["display_name"] == "Legacy account"
            assert not (await migrated.snapshot("device-b"))["account"]["authorized"]
            assert not (await migrated.snapshot("device-b"))["credentials_configured"]["app_secret"]
            assert not (await migrated.snapshot("device-a"))["config"]["messages_enabled"]
        finally:
            await migrated.close()
        unbound = DeviceFeishuManager(replace(settings, component=replace(settings.component, directory=str(tmp_path / "unbound")), feishu=replace(legacy, device_id="")), manager.agent, manager.conversations, manager.registry)
        assert unbound.documents == {}

    asyncio.run(scenario())


def test_two_apps_have_independent_message_routes_and_webhook_credentials(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            for device_id, app_id in [("a", "cli_a"), ("b", "cli_b")]:
                snapshot = await manager.snapshot(device_id)
                await manager.save(device_id, {"revision": snapshot["revision"], "config": {**snapshot["config"], "enabled": True, "app_id": app_id,
                    "mode": "webhook", "messages_enabled": True, "chat_id": "same-chat"}, "secrets": {"app_secret": f"secret-{device_id}", "verification_token": f"verify-{device_id}"}})
            with pytest.raises(DeviceFeishuError):
                await manager.webhook({**event("same-chat"), "header": {"app_id": "cli_b", "token": "verify-a"}})
            payload = event("same-chat")
            payload["header"].update({"app_id": "cli_a", "token": "verify-a"})
            assert (await manager.webhook(payload))["status"] == "ok"
            assert manager.agent.build_context.await_args.args[0] == "a"
            payload["header"].update({"app_id": "cli_b", "token": "verify-b"})
            assert (await manager.webhook(payload))["status"] == "ok"
            assert manager.agent.build_context.await_args.args[0] == "b"
            assert await manager.webhook({"type": "url_verification", "token": "verify-a", "challenge": "test-challenge"}) == {"challenge": "test-challenge"}
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_reauthorization_commit_failure_keeps_the_original_account_and_cache(tmp_path, monkeypatch):
    async def scenario():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            await configure(manager, "a")
            await authorize(manager, "a", "Original")
            await manager.calendar("a").sync_once()
            url = await manager.authorize("a", "https://console.test/api/v1/feishu/oauth/callback")
            state = parse_qs(urlsplit(url).query)["state"][0]
            with monkeypatch.context() as failure:
                failure.setattr(manager, "_persist", lambda doc: (_ for _ in ()).throw(OSError("disk full")))
                with pytest.raises(OSError):
                    await manager.callback("Replacement", state)
            assert (await manager.snapshot("a"))["account"]["display_name"] == "Original"
            assert (await manager.calendar("a").snapshot())[0].summary == "access-Original"
            assert len(list(manager.directory("a").glob("grant-*.json"))) == 1
        finally:
            await manager.close()
    asyncio.run(scenario())


def test_multiple_long_connections_keep_their_own_loops_and_stop_cleanly(monkeypatch):
    pytest.importorskip("lark_oapi")
    from lark_oapi import ws
    from lark_oapi.ws import client as module
    from binbot_server.infrastructure.feishu import FeishuLongConnection
    original_loop = module.loop
    connections = []

    class Socket:
        def __init__(self):
            self.loop = asyncio.get_running_loop()
            self.closed = False

        async def recv(self):
            await asyncio.Future()

        async def send(self, message):
            assert asyncio.get_running_loop() is self.loop

        async def close(self):
            assert asyncio.get_running_loop() is self.loop
            self.closed = True

    async def connect(*args, **kwargs):
        socket = Socket()
        connections.append(socket)
        return socket

    monkeypatch.setattr(module.websockets, "connect", connect)
    monkeypatch.setattr(ws.Client, "_get_conn_url", lambda self: f"wss://example.invalid?{module.DEVICE_ID}=connection&{module.SERVICE_ID}=1")

    async def scenario():
        one = FeishuLongConnection("app-a", "secret-a", AsyncMock())
        two = FeishuLongConnection("app-b", "secret-b", AsyncMock())
        one.start()
        two.start()
        try:
            for _ in range(100):
                if len(connections) == 2:
                    break
                await asyncio.sleep(0.01)
            assert len(connections) == 2
            assert connections[0].loop is not connections[1].loop
            assert module.loop is original_loop
        finally:
            await one.stop()
            await two.stop()
        assert all(socket.closed for socket in connections)
        assert not one.running and not two.running

    asyncio.run(scenario())
