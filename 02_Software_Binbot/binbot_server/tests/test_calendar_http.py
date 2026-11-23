import asyncio
from dataclasses import replace
from unittest.mock import AsyncMock

import pytest
from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer, make_mocked_request

from binbot_server.application.calendar import CalendarEvent, CalendarSyncService, InMemoryCalendarStore
from binbot_server.bootstrap import create_application
from binbot_server.config import Settings
from binbot_server.transport.http import _CONTEXT_KEY, feishu_oauth_callback


@pytest.mark.parametrize("sync_fails", [False, True])
def test_authorization_syncs_calendar_without_polling_and_preserves_success(sync_fails: bool) -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        context = application.http[_CONTEXT_KEY]
        oauth, calendar = AsyncMock(), AsyncMock()
        oauth.handle_callback.return_value = {"display_name": "Test account"}
        if sync_fails:
            calendar.sync_once.side_effect = RuntimeError("calendar unavailable")
        context.feishu_oauth = oauth
        context.calendar = calendar
        request = make_mocked_request(
            "GET", "/api/v1/feishu/oauth/callback?code=test-code&state=test-state",
            app=application.http,
        )
        response = await feishu_oauth_callback(request)
        assert response.status == 200 and "Test account" in response.text
        oauth.handle_callback.assert_awaited_once_with("test-code", "test-state")
        calendar.sync_once.assert_awaited_once()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_calendar_requires_login_and_lists_creates_syncs(monkeypatch: pytest.MonkeyPatch) -> None:
    async def scenario() -> None:
        monkeypatch.setenv("BINBOT_CALENDAR_TEST_PASSWORD", "calendar-password")
        monkeypatch.setenv("BINBOT_CALENDAR_TEST_USERNAME", "calendar-operator")
        defaults = Settings()
        settings = replace(defaults, feishu=replace(defaults.feishu, device_id="test"), admin=replace(
            defaults.admin, allow_unauthenticated_loopback=False,
            password_env="BINBOT_CALENDAR_TEST_PASSWORD",
            username_env="BINBOT_CALENDAR_TEST_USERNAME",
        ))
        application = create_application(settings)
        provider, notify = AsyncMock(), AsyncMock()
        provider.calendar_primary.return_value = "user-calendar"
        event = CalendarEvent("user-calendar", "event-1", "会议", start_date="2026-10-19", end_date="2026-10-20")
        provider.calendar_events.return_value = ([event], "sync-token")
        provider.calendar_instances.return_value = [event]
        provider.create_calendar_event.return_value = event
        service = CalendarSyncService(provider, InMemoryCalendarStore(), subscribe=False, notify=notify)
        context = application.http[_CONTEXT_KEY]
        context.calendar = service
        server = TestServer(application.http)
        await server.start_server()
        await service.stop()
        path = "/api/v1/admin/calendar/events"
        query = path + "?start_date=2026-10-01&end_date=2026-11-01"
        valid = {"summary": "会议", "all_day": True, "start_time": "2026-10-19", "end_time": "2026-10-20"}
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                for method, route in [("get", query), ("post", path), ("post", "/api/v1/admin/calendar/sync")]:
                    async with client.request(method, server.make_url(route), json=valid) as response:
                        assert response.status == 401
                async with client.post(server.make_url("/api/v1/admin/session"), json={
                    "username": "calendar-operator", "password": "calendar-password",
                }) as response:
                    assert response.status == 200
                async with client.get(server.make_url(query)) as response:
                    assert response.status == 200
                    body = await response.json()
                    assert body["calendar_id"] == "user-calendar"
                    assert body["events"] == [event.snapshot()]
                    assert body["timezone"] == "Asia/Shanghai" and body["fetched_at"]
                async with client.get(server.make_url(path)) as response:
                    assert response.status == 400
                for body in [{}, {**valid, "end_time": "2026-10-18"}, [], {**valid, "all_day": "true"}]:
                    async with client.post(server.make_url(path), json=body) as response:
                        assert response.status == 400
                async with client.post(server.make_url(path), data="{", headers={"Content-Type": "application/json"}) as response:
                    assert response.status == 400
                async with client.post(server.make_url(path), json=valid, headers={"Idempotency-Key": " "}) as response:
                    assert response.status == 400
                provider.create_calendar_event.assert_not_awaited()
                async with client.post(server.make_url(path), json=valid, headers={"Idempotency-Key": "calendar-key"}) as response:
                    assert response.status == 201
                    assert (await response.json())["event"] == event.snapshot()
                assert provider.create_calendar_event.await_args.args[2] == "calendar-key"
                assert (await service.snapshot())[0] == event
                async with client.get(server.make_url("/api/v1/admin/devices/test/calendar")) as response:
                    assert response.status == 200
                    assert "description" not in (await response.json())["events"][0]
                async with client.get(server.make_url("/api/v1/admin/devices/other/calendar")) as response:
                    assert response.status == 404
                async with client.post(server.make_url("/api/v1/admin/calendar/sync")) as response:
                    assert response.status == 200
                provider.create_calendar_event.side_effect = RuntimeError("permission denied")
                async with client.post(server.make_url(path), json=valid) as response:
                    assert response.status == 502
                    assert "permission denied" in (await response.json())["error"]
                provider.calendar_instances.side_effect = RuntimeError("authorization expired")
                async with client.get(server.make_url(query)) as response:
                    assert response.status == 502
                context.calendar = None
                async with client.get(server.make_url(query)) as response:
                    assert response.status == 404
                async with client.post(server.make_url(path), json=valid) as response:
                    assert response.status == 404
        finally:
            context.calendar = service
            await server.close()

    asyncio.run(scenario())
