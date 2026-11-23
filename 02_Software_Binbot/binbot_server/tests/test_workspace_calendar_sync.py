import asyncio
from datetime import UTC, datetime, timedelta
from unittest.mock import AsyncMock
from zoneinfo import ZoneInfo

import pytest
from aiohttp import ClientSession, web
from aiohttp.test_utils import TestServer

from binbot_server.application.calendar import CalendarEvent, CalendarSyncService, InMemoryCalendarStore
from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.workspace import WorkspaceService
from binbot_server.domain.errors import StateConflict
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.infrastructure.sqlite import SqliteCalendarStore
from binbot_server.application import workspace
from binbot_server.transport.workspace_http import register_workspace_routes
from test_device_feishu import make_manager, configure, authorize


def entry():
    return {"id": "stable-agenda-id", "name": "下午讨论", "date": "2026-11-18",
            "time": "19:00", "note": "准备资料", "enabled": True, "done": False}


def setup_service(path):
    store = SqliteWorkspaceStore(path)
    service = WorkspaceService(store, DeviceControlStore(), None, DeviceSessionRegistry())
    provider = AsyncMock()
    provider.calendar_primary.return_value = "device-one-calendar"
    provider.create_calendar_event.return_value = CalendarEvent(
        "device-one-calendar", "created-event", "下午讨论", start_timestamp="1794999600",
        end_timestamp="1795001400", description="准备资料",
    )
    calendar = CalendarSyncService(provider, InMemoryCalendarStore(), subscribe=False)
    service.calendar_provider = lambda device: calendar if device == "one" else None
    service.calendar_status_provider = AsyncMock(side_effect=lambda device: {"enabled": device == "one", "authorized": device == "one"})
    service.calendar_scope_provider = lambda device: "grant-one"
    return service, store, provider, calendar


def test_daily_calendar_polling_survives_legacy_zero_without_message_connection(tmp_path, monkeypatch):
    async def run():
        manager = make_manager(tmp_path, monkeypatch)
        try:
            await configure(manager, "one", calendar=True)
            await authorize(manager, "one", "Alice")
            calendar = manager.calendar("one")
            assert calendar._interval == 60
            assert manager.documents["one"]["config"]["messages_enabled"] is False
            assert not manager.connections
            calendar._interval = .01
            calendar.start()
            for _ in range(100):
                if (await calendar.status())["last_sync_at"]:
                    break
                await asyncio.sleep(.01)
            first = (await calendar.status())["last_sync_at"]
            await asyncio.sleep(.04)
            status = await manager.calendar_status("one")
            assert status["polling"] and status["last_sync_at"] > first and not status["error"]
        finally:
            await manager.close()
    asyncio.run(run())


def test_export_preserves_user_schedule_until_confirmed_and_replays_without_duplicates(tmp_path):
    async def run():
        service, store, provider, calendar = setup_service(str(tmp_path / "workspace.sqlite3"))
        current = await service.document("one")
        await store.update("one", current["revision"], lambda data: {**data, "schedules": [entry()]})
        provider.create_calendar_event.side_effect = RuntimeError("provider unavailable")
        body = {"schedule": entry(), "event_id": "click-once"}
        with pytest.raises(StateConflict, match="飞书未确认"):
            await service.create_schedule("one", body)
        assert (await service.document("one"))["data"]["schedules"] == [entry()]
        provider.create_calendar_event.side_effect = None
        result = await service.create_schedule("one", body)
        assert result["data"]["schedules"] == []
        assert [item["source"] for item in result["calendar"]] == ["feishu"]
        assert result["calendar"][0]["note"] == "准备资料"
        draft = provider.create_calendar_event.await_args.args[1]
        assert draft.summary == entry()["name"] and draft.description == entry()["note"]
        assert datetime.fromtimestamp(int(draft.start_timestamp), UTC).isoformat() == "2026-11-18T11:00:00+00:00"
        assert int(draft.end_timestamp) - int(draft.start_timestamp) == 1800
        key = provider.create_calendar_event.await_args.args[2]
        assert len(key) == 64
        await service.create_schedule("one", body)
        assert provider.create_calendar_event.await_count == 2  # failed request + confirmed request
        # A new browser event ID must also reuse the stable source schedule ID.
        await service.create_schedule("one", {**body, "event_id": "other-browser"})
        assert provider.create_calendar_event.await_count == 2
        await store.close()
        reopened = SqliteWorkspaceStore(str(tmp_path / "workspace.sqlite3"))
        service.store = reopened
        await service.create_schedule("one", {**body, "event_id": "after-restart"})
        assert provider.create_calendar_event.await_count == 2
        with pytest.raises(StateConflict, match="同一编号"):
            await service.create_schedule("one", {"schedule": {**entry(), "name": "different"}, "event_id": "different-content"})
        await reopened.close()
        await calendar.close()
    asyncio.run(run())


def test_disconnected_account_does_not_silently_fall_back_and_disabled_calendar_stays_local(tmp_path):
    async def run():
        service, store, provider, calendar = setup_service(":memory:")
        body = {"schedule": entry(), "event_id": "create"}
        local = await service.create_schedule("two", body)
        assert local["data"]["schedules"] == [entry()]
        assert local["calendar"][0]["source"] == "local"
        provider.create_calendar_event.assert_not_awaited()
        service.calendar_provider = lambda device: None
        with pytest.raises(StateConflict, match="连接账号"):
            await service.create_schedule("one", body)
        assert not (await service.document("one"))["data"]["schedules"]
        await store.close()
        await calendar.close()
    asyncio.run(run())


def test_export_receipts_do_not_cross_devices_or_destination_accounts(tmp_path):
    async def run():
        service, store, provider, calendar = setup_service(":memory:")
        body = {"schedule": entry(), "event_id": "first"}
        await service.create_schedule("one", body)
        key_one = provider.create_calendar_event.await_args.args[2]
        service.calendar_provider = lambda device: calendar
        service.calendar_status_provider = AsyncMock(return_value={"enabled": True, "authorized": True})
        await service.create_schedule("two", body)
        key_two = provider.create_calendar_event.await_args.args[2]
        assert key_two != key_one
        service.calendar_scope_provider = lambda device: "replacement-account"
        await service.create_schedule("one", {**body, "event_id": "second-account"})
        assert provider.create_calendar_event.await_count == 3
        assert provider.create_calendar_event.await_args.args[2] not in {key_one, key_two}
        await store.close()
        await calendar.close()
    asyncio.run(run())


@pytest.mark.integration
def test_workspace_calendar_endpoint_returns_device_snapshot_and_keeps_errors_reviewable():
    async def run():
        service, store, provider, calendar = setup_service(":memory:")
        app = web.Application()
        register_workspace_routes(app, service)
        async with TestServer(app) as server, ClientSession() as client:
            url = server.make_url("/api/v1/admin/devices/one/workspace/schedules")
            async with client.post(url, json={"schedule": entry(), "event_id": "one"}) as response:
                assert response.status == 200
                result = await response.json()
                assert result["device_id"] == "one" and result["calendar"][0]["source"] == "feishu"
            async with client.post(url, json={"schedule": {}, "event_id": "bad"}) as response:
                assert response.status == 400
            provider.create_calendar_event.side_effect = RuntimeError("permission denied")
            async with client.post(url, json={"schedule": {**entry(), "id": "another"}, "event_id": "two"}) as response:
                assert response.status == 409
                assert "permission denied" in (await response.json())["error"]
        await store.close()
        await calendar.close()
    asyncio.run(run())


def test_old_cached_events_do_not_crowd_out_the_week_or_device_snapshot(monkeypatch):
    when = datetime(2026, 11, 19, 2, tzinfo=UTC)

    class Clock(datetime):
        @classmethod
        def now(cls, tz=None):
            return when.astimezone(tz)

    monkeypatch.setattr(workspace, "datetime", Clock)

    async def run():
        store = SqliteWorkspaceStore(":memory:")
        cache = SqliteCalendarStore(":memory:")
        service = WorkspaceService(store, DeviceControlStore(), None, DeviceSessionRegistry())
        calendar = CalendarSyncService(AsyncMock(), cache, calendar_id="one", subscribe=False)
        service.calendar_provider = lambda _: calendar
        service.apply = AsyncMock()
        try:
            for i in range(120):
                await cache.upsert(CalendarEvent(
                    "one", f"old-{i}", f"旧日程 {i}",
                    start_timestamp=str(int((when - timedelta(days=i + 10)).timestamp())),
                ))
            previous = when - timedelta(days=1)
            await cache.upsert(CalendarEvent(
                "one", "all-day", "本周全天", start_date=previous.date().isoformat(),
            ))
            for i in range(17):
                await cache.upsert(CalendarEvent(
                    "one", f"new-{i:02d}", f"近期 {i}",
                    start_timestamp=str(int((when + timedelta(hours=i + 1)).timestamp())),
                ))
            await cache.upsert(CalendarEvent(
                "one", "cancelled", "已取消", start_timestamp=str(int(when.timestamp())),
                status="cancelled",
            ))

            # Even with the legacy snapshot cap, the date filter applies first.
            timezone = ZoneInfo("Asia/Shanghai")
            window = (when.astimezone(timezone), (when + timedelta(days=7)).astimezone(timezone))
            assert (await calendar.snapshot(window=window))[-1].event_id == "new-15"
            console = await service.snapshot("one", compact=True)
            assert console["calendar"][0]["id"] == "feishu:all-day"
            assert len(console["calendar"]) == 18
            assert console["calendar"][-1]["id"] == "feishu:new-16"

            await service._sync_calendar("one", "boot")
            _, command, payload, _ = service.apply.await_args.args
            assert command == "calendar.set"
            assert [entry["title"] for entry in payload["events"]] == [f"近期 {i}" for i in range(8)]
        finally:
            await calendar.close()
            await store.close()

    asyncio.run(run())
