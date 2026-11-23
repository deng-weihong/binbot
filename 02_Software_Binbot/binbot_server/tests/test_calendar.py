import asyncio
from dataclasses import replace
from pathlib import Path
from unittest.mock import AsyncMock

import pytest

from binbot_server.application.calendar import (
    CalendarChanges,
    CalendarEvent,
    CalendarEventDraft,
    CalendarSyncService,
    InMemoryCalendarStore,
    calendar_date_range,
)
from binbot_server.infrastructure.sqlite import SqliteCalendarStore


def test_zero_interval_syncs_at_startup_and_on_push_without_periodic_polling() -> None:
    async def scenario() -> None:
        provider, published = AsyncMock(), AsyncMock()
        provider.calendar_primary.return_value = "calendar"
        provider.calendar_events.return_value = ([], "initial-cursor")
        service = CalendarSyncService(
            provider, InMemoryCalendarStore(), interval_seconds=0,
            user_access_token="user-token", notify=published,
        )
        try:
            service.start()
            assert service._task is not None
            await asyncio.wait_for(service._task, 1)
            provider.calendar_events.assert_awaited_once()
            provider.subscribe_calendar.assert_awaited_once_with("calendar", "user-token")
            published.reset_mock()

            event = CalendarEvent("calendar", "new-event", "New meeting")
            provider.calendar_events.return_value = ([event], "next-cursor")
            await service.handle_event({"event": {"calendar_id": "calendar"}})
            assert provider.calendar_events.await_count == 2
            published.assert_awaited_once_with("calendar", (event,), CalendarChanges(added=1))
        finally:
            await service.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("calendar_id", ["", "configured-calendar"])
def test_new_user_grant_resolves_primary_but_preserves_explicit_calendar(calendar_id: str) -> None:
    async def scenario() -> None:
        class Provider:
            primary_id = "application-calendar"

            async def calendar_primary(self) -> str:
                return self.primary_id

            async def calendar_events(
                self, calendar_id: str, sync_token: str = "",
                start_time: str = "", end_time: str = "",
            ) -> tuple[list[CalendarEvent], str]:
                return [CalendarEvent(calendar_id, "event-1", calendar_id)], "sync-token"

            async def subscribe_calendar(self, calendar_id: str, user_access_token: str) -> None:
                return None

        provider = Provider()
        service = CalendarSyncService(
            provider, InMemoryCalendarStore(), calendar_id=calendar_id, subscribe=False,
        )
        await service.sync_once()
        events = await service.snapshot()
        assert [event.summary for event in events] == [calendar_id or "application-calendar"]

        provider.primary_id = "user-calendar"
        await service.set_user_access_token("user-token")
        await service.sync_once()
        events = await service.snapshot()
        assert [event.summary for event in events] == [calendar_id or "user-calendar"]

        await service.set_user_access_token("")
        events = await service.snapshot()
        assert [event.summary for event in events] == ([calendar_id] if calendar_id else [])
        await service.close()

    asyncio.run(scenario())


def test_draft_preserves_instants_and_exclusive_all_day_end() -> None:
    draft = CalendarEventDraft.from_payload({
        "summary": " 测试会议 ", "start_time": "2026-10-19T10:00:00+08:00",
        "end_time": "2026-10-19T03:00:00Z", "location": "会议室", "description": "备注",
    })
    assert int(draft.end_timestamp) - int(draft.start_timestamp) == 3600
    assert draft.summary == "测试会议" and draft.description == "备注"
    all_day = CalendarEventDraft.from_payload({
        "summary": "假期", "all_day": True, "start_time": "2026-10-19", "end_time": "2026-10-21",
    })
    assert all_day.start_date == "2026-10-19" and all_day.end_date == "2026-10-21"
    assert all_day.start_timestamp == all_day.end_timestamp == ""


@pytest.mark.parametrize("changes", [
    {"summary": " "}, {"summary": "a" * 201}, {"summary": 42},
    {"all_day": "true"}, {"timezone": "invalid/timezone"}, {"start_time": "2026-10-19T10:00"},
    {"end_time": "2026-10-19T09:00:00+08:00"}, {"location": []},
    {"start_time": "bad"}, {"extra": True},
    {"all_day": True, "start_time": "2026-10-19", "end_time": "2026-10-19"},
    {"all_day": True, "start_time": "2026-02-30", "end_time": "2026-03-01"},
])
def test_draft_rejects_invalid_input(changes: dict[str, object]) -> None:
    with pytest.raises(ValueError):
        CalendarEventDraft.from_payload({
            "summary": "会议", "start_time": "2026-10-19T10:00:00+08:00",
            "end_time": "2026-10-19T11:00:00+08:00", **changes,
        })


def test_calendar_range_uses_shanghai_midnight_and_limits_queries() -> None:
    start, end = calendar_date_range("2026-10-01", "2026-11-01")
    assert int(end) - int(start) == 31 * 86400
    from datetime import UTC, datetime
    assert datetime.fromtimestamp(int(start), UTC).isoformat() == "2026-09-30T16:00:00+00:00"
    for first, last in [("", ""), ("2026-10-01", "2026-12-01"),
                        ("2026-10-02", "2026-10-01"), ("2026-10-01", "2026-10-01")]:
        with pytest.raises(ValueError):
            calendar_date_range(first, last)


@pytest.mark.parametrize("sqlite", [False, True])
def test_web_calendar_is_not_device_limited_and_creation_is_persisted_and_published(
    tmp_path: Path, sqlite: bool,
) -> None:
    async def scenario() -> None:
        provider = AsyncMock()
        provider.calendar_primary.return_value = "user-calendar"
        events = [CalendarEvent("user-calendar", f"event-{i}", f"会议{i}",
                                start_timestamp=str(1792368000 + i * 3600),
                                end_timestamp=str(1792371600 + i * 3600)) for i in range(25)]
        provider.calendar_instances.return_value = [*reversed(events), replace(events[0], is_deleted=True)]
        store = SqliteCalendarStore(str(tmp_path / "calendar.sqlite3")) if sqlite else InMemoryCalendarStore()
        published = AsyncMock()
        service = CalendarSyncService(provider, store, max_events=2, notify=published, subscribe=False)

        async def prepare() -> None:
            await service.set_user_access_token("refreshed-token")

        service._prepare = prepare
        result = await asyncio.wait_for(service.events_between("2026-10-01", "2026-11-01"), 1)
        assert [event.event_id for event in result] == [event.event_id for event in events]
        provider.calendar_instances.assert_awaited_once_with(
            "user-calendar", *calendar_date_range("2026-10-01", "2026-11-01")
        )
        draft = CalendarEventDraft.from_payload({
            "summary": "测试", "all_day": True, "start_time": "2026-10-19", "end_time": "2026-10-20",
        })
        created = replace(events[0], description="保留备注")
        provider.create_calendar_event.return_value = created
        assert await asyncio.wait_for(service.create_event(draft, "request-key"), 1) == created
        provider.create_calendar_event.assert_awaited_once_with("user-calendar", draft, "request-key")
        assert await service.snapshot() == (created,)
        published.assert_awaited_once_with("user-calendar", (created,), CalendarChanges(added=1))
        published.side_effect = RuntimeError("device offline")
        assert await service.create_event(draft, "request-key") == created
        provider.create_calendar_event.side_effect = RuntimeError("permission denied")
        with pytest.raises(RuntimeError, match="permission denied"):
            await service.create_event(draft, "other-key")
        assert await service.snapshot() == (created,)
        await service.close()
        if sqlite:
            reopened = SqliteCalendarStore(str(tmp_path / "calendar.sqlite3"))
            assert await reopened.list("user-calendar") == (created,)
            await reopened.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("sqlite", [False, True])
def test_change_notifications_include_events_outside_the_device_snapshot(
    tmp_path: Path, sqlite: bool,
) -> None:
    async def scenario() -> None:
        provider, published = AsyncMock(), AsyncMock()
        provider.calendar_primary.return_value = "calendar"
        events = [CalendarEvent("calendar", f"event-{i}", f"Meeting {i}",
                                start_timestamp=str(1792368000 + i * 3600),
                                end_timestamp=str(1792371600 + i * 3600)) for i in range(3)]
        provider.calendar_events.return_value = (events, "cursor")
        store = SqliteCalendarStore(str(tmp_path / "notices.sqlite3")) if sqlite else InMemoryCalendarStore()
        service = CalendarSyncService(provider, store, max_events=1, notify=published, subscribe=False)
        await service.sync_once()
        published.assert_awaited_once_with("calendar", (events[0],), CalendarChanges(added=3))
        published.reset_mock()
        await service.sync_once()
        published.assert_not_awaited()

        draft = CalendarEventDraft(summary="Later event", start_date="2026-11-01", end_date="2026-11-02")
        created = CalendarEvent("calendar", "created", draft.summary,
                                start_timestamp="1793509200", end_timestamp="1793512800")
        provider.create_calendar_event.return_value = created
        await service.create_event(draft, "same-request")
        published.assert_awaited_once_with("calendar", (events[0],), CalendarChanges(added=1))
        published.reset_mock()
        await service.create_event(draft, "same-request")
        published.assert_awaited_once_with("calendar", (events[0],), CalendarChanges())

        published.reset_mock()
        provider.calendar_events.return_value = ([replace(created, description="Updated notes")], "cursor-2")
        await service.sync_once()
        published.assert_awaited_once_with("calendar", (events[0],), CalendarChanges(updated=1))

        published.reset_mock()
        provider.calendar_events.return_value = ([replace(created, is_deleted=True)], "cursor-3")
        await service.sync_once()
        published.assert_awaited_once_with("calendar", (events[0],), CalendarChanges(removed=1))
        published.reset_mock()
        await service.sync_once()
        published.assert_not_awaited()
        await service.close()

    asyncio.run(scenario())


@pytest.mark.parametrize("sqlite", [False, True])
def test_feishu_sync_distinguishes_additions_updates_removals_and_replays(
    tmp_path: Path, sqlite: bool,
) -> None:
    async def scenario() -> None:
        provider, published = AsyncMock(), AsyncMock()
        provider.calendar_primary.return_value = "calendar"
        events = [CalendarEvent("calendar", f"event-{i}", f"Meeting {i}") for i in range(3)]
        provider.calendar_events.return_value = (events, "baseline")
        store = SqliteCalendarStore(str(tmp_path / "deltas.sqlite3")) if sqlite else InMemoryCalendarStore()
        service = CalendarSyncService(provider, store, max_events=1, notify=published, subscribe=False)
        await service.sync_once()
        published.reset_mock()

        created = CalendarEvent("calendar", "new", "New meeting")
        modified = replace(events[0], summary="Rescheduled meeting")
        cancelled = replace(events[1], status="cancelled")
        provider.calendar_events.return_value = ([created, modified, cancelled], "changes")
        await service.sync_once()
        assert published.await_args.args[2] == CalendarChanges(added=1, updated=1, removed=1)
        assert await store.get("calendar", "new") == created
        assert await store.get("other-calendar", "new") is None
        published.reset_mock()
        await service.sync_once()
        published.assert_not_awaited()

        provider.calendar_events.return_value = ([events[1]], "restored")
        await service.sync_once()
        assert published.await_args.args[2] == CalendarChanges(added=1)
        await service.close()

    asyncio.run(scenario())
