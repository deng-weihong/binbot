"""Calendar synchronization state shared by Feishu and device transports."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable, Mapping, Sequence
from dataclasses import dataclass
from datetime import UTC, date, datetime, time
from typing import Protocol
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


@dataclass(frozen=True, slots=True)
class CalendarEvent:
    calendar_id: str
    event_id: str
    summary: str
    start_timestamp: str = ""
    end_timestamp: str = ""
    start_date: str = ""
    end_date: str = ""
    timezone: str = "Asia/Shanghai"
    location: str = ""
    meeting_url: str = ""
    status: str = "confirmed"
    app_link: str = ""
    recurrence: str = ""
    is_deleted: bool = False
    description: str = ""

    def snapshot(self, *, include_description: bool = True) -> dict[str, object]:
        payload: dict[str, object] = {
            "calendar_id": self.calendar_id,
            "event_id": self.event_id,
            "summary": self.summary,
            "start_timestamp": self.start_timestamp,
            "end_timestamp": self.end_timestamp,
            "start_date": self.start_date,
            "end_date": self.end_date,
            "timezone": self.timezone,
            "location": self.location,
            "meeting_url": self.meeting_url,
            "status": self.status,
            "app_link": self.app_link,
            "recurrence": self.recurrence,
            "is_deleted": self.is_deleted,
        }
        if include_description:
            payload["description"] = self.description
        return payload


@dataclass(frozen=True, slots=True)
class CalendarChanges:
    added: int = 0
    updated: int = 0
    removed: int = 0

    def snapshot(self) -> dict[str, int]:
        return {"added": self.added, "updated": self.updated, "removed": self.removed}


@dataclass(frozen=True, slots=True)
class CalendarEventDraft:
    summary: str
    start_timestamp: str = ""
    end_timestamp: str = ""
    start_date: str = ""
    end_date: str = ""
    timezone: str = "Asia/Shanghai"
    location: str = ""
    description: str = ""

    @classmethod
    def from_payload(cls, payload: object) -> CalendarEventDraft:
        if not isinstance(payload, dict):
            raise ValueError("日程内容必须是对象")
        allowed = {"summary", "start_time", "end_time", "all_day", "timezone", "location", "description"}
        if set(payload) - allowed:
            raise ValueError("日程包含不支持的字段")

        def text(key: str, limit: int, default: str = "") -> str:
            value = payload.get(key, default)
            if not isinstance(value, str) or len(value) > limit:
                raise ValueError(f"{key} 必须是长度不超过 {limit} 的文本")
            return value.strip()

        summary = text("summary", 200)
        if not summary:
            raise ValueError("请填写日程标题")
        timezone = text("timezone", 100, "Asia/Shanghai")
        try:
            ZoneInfo(timezone)
        except (ValueError, ZoneInfoNotFoundError) as exc:
            raise ValueError("无效的时区") from exc
        all_day = payload.get("all_day", False)
        if not isinstance(all_day, bool):
            raise ValueError("all_day 必须是布尔值")
        start, end = text("start_time", 64), text("end_time", 64)
        values: dict[str, str] = {}
        try:
            if all_day:
                first, last = date.fromisoformat(start), date.fromisoformat(end)
                if first.isoformat() != start or last.isoformat() != end:
                    raise ValueError("日期格式应为 YYYY-MM-DD")
                if last <= first:
                    raise ValueError("全天日程的结束日期须晚于开始日期（结束日期不包含在内）")
                values = {"start_date": start, "end_date": end}
            else:
                begin, finish = datetime.fromisoformat(start), datetime.fromisoformat(end)
                if begin.tzinfo is None or finish.tzinfo is None:
                    raise ValueError("起止时间必须包含时区")
                if finish <= begin:
                    raise ValueError("结束时间须晚于开始时间")
                values = {
                    "start_timestamp": str(int(begin.timestamp())),
                    "end_timestamp": str(int(finish.timestamp())),
                }
        except (ValueError, OverflowError) as exc:
            raise ValueError(f"日程时间无效：{exc}") from exc
        return cls(summary=summary, timezone=timezone, location=text("location", 500),
                   description=text("description", 10000), **values)


def calendar_date_range(start: str, end: str) -> tuple[str, str]:
    try:
        first, last = date.fromisoformat(start), date.fromisoformat(end)
        if first.isoformat() != start or last.isoformat() != end:
            raise ValueError
        if not 0 < (last - first).days <= 31:
            raise ValueError
    except ValueError as exc:
        raise ValueError("请提供有效的起止日期，查询范围须为 1 至 31 天") from exc
    timezone = ZoneInfo("Asia/Shanghai")
    return (
        str(int(datetime.combine(first, time.min, timezone).timestamp())),
        str(int(datetime.combine(last, time.min, timezone).timestamp())),
    )


class CalendarStore(Protocol):
    async def get(self, calendar_id: str, event_id: str) -> CalendarEvent | None: ...

    async def upsert(self, event: CalendarEvent) -> bool: ...

    async def delete(self, calendar_id: str, event_id: str) -> bool: ...

    async def list(
        self, calendar_id: str, limit: int | None = 16,
        *, window: tuple[datetime, datetime] | None = None,
    ) -> Sequence[CalendarEvent]: ...

    async def sync_token(self, calendar_id: str) -> str: ...

    async def set_sync_token(self, calendar_id: str, token: str) -> None: ...

    async def status(self, calendar_id: str) -> Mapping[str, object]: ...

    async def close(self) -> None: ...


class CalendarProvider(Protocol):
    async def calendar_primary(self) -> str: ...

    async def calendar_events(
        self,
        calendar_id: str,
        sync_token: str = "",
        start_time: str = "",
        end_time: str = "",
    ) -> tuple[list[CalendarEvent], str]: ...

    async def subscribe_calendar(self, calendar_id: str, user_access_token: str) -> None: ...

    async def calendar_instances(
        self, calendar_id: str, start_time: str, end_time: str,
    ) -> list[CalendarEvent]: ...

    async def create_calendar_event(
        self, calendar_id: str, draft: CalendarEventDraft, idempotency_key: str,
    ) -> CalendarEvent: ...


def _now() -> str:
    return datetime.now(UTC).isoformat()


def select_calendar_events(
    events: Sequence[CalendarEvent], limit: int | None,
    window: tuple[datetime, datetime] | None = None,
) -> Sequence[CalendarEvent]:
    """Filter the complete cache before limiting; compare timed and all-day instants alike."""
    ordered = []
    start_at, end_at = (window[0].timestamp(), window[1].timestamp()) if window else (0, 0)
    for event in events:
        if event.is_deleted or event.status == "cancelled":
            continue
        try:
            if event.start_timestamp:
                start = int(event.start_timestamp)
            elif event.start_date:
                zone = window[0].tzinfo if window else ZoneInfo(event.timezone)
                start = int(
                    datetime.combine(
                        date.fromisoformat(event.start_date), time.min, zone
                    ).timestamp()
                )
            elif window:
                continue
            else:
                start = 0
        except (ValueError, OverflowError, ZoneInfoNotFoundError):
            continue
        if window and not start_at <= start < end_at:
            continue
        ordered.append((start, event.event_id, event))
    ordered.sort(key=lambda row: (row[0], row[1]))
    selected = [row[2] for row in ordered]
    return tuple(selected if limit is None else selected[:max(1, min(limit, 100))])


class InMemoryCalendarStore(CalendarStore):
    def __init__(self) -> None:
        self._events: dict[tuple[str, str], CalendarEvent] = {}
        self._tokens: dict[str, str] = {}
        self._last_sync: dict[str, str] = {}
        self._revision: dict[str, int] = {}
        self._lock = asyncio.Lock()

    async def get(self, calendar_id: str, event_id: str) -> CalendarEvent | None:
        async with self._lock:
            return self._events.get((calendar_id, event_id))

    async def upsert(self, event: CalendarEvent) -> bool:
        key = (event.calendar_id, event.event_id)
        async with self._lock:
            changed = self._events.get(key) != event
            self._events[key] = event
            if changed:
                self._revision[event.calendar_id] = self._revision.get(event.calendar_id, 0) + 1
            self._last_sync[event.calendar_id] = _now()
            return changed

    async def delete(self, calendar_id: str, event_id: str) -> bool:
        key = (calendar_id, event_id)
        async with self._lock:
            previous = self._events.pop(key, None)
            if previous is None:
                return False
            self._revision[calendar_id] = self._revision.get(calendar_id, 0) + 1
            self._last_sync[calendar_id] = _now()
            return True

    async def list(
        self, calendar_id: str, limit: int | None = 16,
        *, window: tuple[datetime, datetime] | None = None,
    ) -> Sequence[CalendarEvent]:
        async with self._lock:
            events = [
                event for (stored_calendar, _), event in self._events.items()
                if stored_calendar == calendar_id and not event.is_deleted
            ]
            return select_calendar_events(events, limit, window)

    async def sync_token(self, calendar_id: str) -> str:
        async with self._lock:
            return self._tokens.get(calendar_id, "")

    async def set_sync_token(self, calendar_id: str, token: str) -> None:
        async with self._lock:
            self._tokens[calendar_id] = token
            self._last_sync[calendar_id] = _now()

    async def status(self, calendar_id: str) -> Mapping[str, object]:
        async with self._lock:
            return {
                "calendar_id": calendar_id,
                "revision": self._revision.get(calendar_id, 0),
                "sync_token_configured": bool(self._tokens.get(calendar_id)),
                "last_sync_at": self._last_sync.get(calendar_id, ""),
                "event_count": sum(
                    1 for (stored_calendar, _), event in self._events.items()
                    if stored_calendar == calendar_id and not event.is_deleted
                ),
            }

    async def close(self) -> None:
        return None


_LOG = logging.getLogger("binbot.calendar")


class CalendarSyncService:
    """Pull Feishu calendar deltas and publish a bounded device snapshot."""

    def __init__(
        self,
        provider: CalendarProvider,
        store: CalendarStore,
        *,
        calendar_id: str = "",
        interval_seconds: float = 60.0,
        max_events: int = 16,
        user_access_token: str = "",
        subscribe: bool = True,
        notify: Callable[[str, Sequence[CalendarEvent], CalendarChanges], Awaitable[None]] | None = None,
        prepare: Callable[[], Awaitable[None]] | None = None,
    ) -> None:
        self._provider = provider
        self._store = store
        self._configured_calendar_id = calendar_id.strip()
        self._calendar_id = self._configured_calendar_id
        self._interval = interval_seconds
        self._max_events = max_events
        self._user_access_token = user_access_token
        self._subscribe = subscribe
        self._notify = notify
        self._prepare = prepare
        self._task: asyncio.Task[None] | None = None
        self._lock = asyncio.Lock()
        self._subscribed = False
        self._last_error = ""

    @property
    def calendar_id(self) -> str:
        return self._calendar_id

    @property
    def enabled(self) -> bool:
        return bool(self._calendar_id) or self._task is not None

    async def set_user_access_token(self, token: str) -> None:
        async with self._lock:
            token = token.strip()
            if token != self._user_access_token:
                self._calendar_id = self._configured_calendar_id
            self._user_access_token = token
            self._subscribed = False

    async def _resolve_calendar(self) -> str:
        if not self._calendar_id:
            self._calendar_id = await self._provider.calendar_primary()
            _LOG.info("using Feishu primary calendar calendar_id=%s", self._calendar_id)
        return self._calendar_id

    async def sync_once(self) -> dict[str, object]:
        if self._prepare is not None:
            await self._prepare()
        async with self._lock:
            calendar_id = await self._resolve_calendar()
            if self._subscribe and self._user_access_token and not self._subscribed:
                await self._provider.subscribe_calendar(calendar_id, self._user_access_token)
                self._subscribed = True
                _LOG.info("subscribed to Feishu calendar changes calendar_id=%s", calendar_id)
            token = await self._store.sync_token(calendar_id)
            if token:
                events, next_token = await self._provider.calendar_events(
                    calendar_id, sync_token=token
                )
            else:
                now = int(datetime.now(UTC).timestamp())
                events, next_token = await self._provider.calendar_events(
                    calendar_id,
                    start_time=str(now - 86_400),
                    end_time=str(now + 30 * 86_400),
                )
            changed = False
            added = updated = removed = 0
            for event in events:
                if event.is_deleted:
                    event_changed = await self._store.delete(calendar_id, event.event_id)
                    removed += int(event_changed)
                else:
                    previous = await self._store.get(calendar_id, event.event_id)
                    event_changed = await self._store.upsert(event)
                    if event.status in {"cancelled", "canceled"}:
                        removed += int(event_changed)
                    elif previous is None or previous.status in {"cancelled", "canceled"}:
                        added += int(event_changed)
                    else:
                        updated += int(event_changed)
                changed = event_changed or changed
            if next_token:
                await self._store.set_sync_token(calendar_id, next_token)
            snapshot = await self.snapshot()
            if changed or not token:
                await self._publish(
                    calendar_id, snapshot, CalendarChanges(added=added, updated=updated, removed=removed),
                )
            return {
                "calendar_id": calendar_id,
                "changed": changed,
                "received": len(events),
                "sync_token_configured": bool(next_token),
            }

    async def events_between(self, start_date: str, end_date: str) -> list[CalendarEvent]:
        start, end = calendar_date_range(start_date, end_date)
        if self._prepare is not None:
            await self._prepare()
        async with self._lock:
            calendar_id = await self._resolve_calendar()
            events = await self._provider.calendar_instances(calendar_id, start, end)
            events = [event for event in events if not event.is_deleted]
            events.sort(key=lambda event: (
                int(event.start_timestamp) if event.start_timestamp else
                int(datetime.combine(date.fromisoformat(event.start_date), time.min,
                                     ZoneInfo(event.timezone)).timestamp()),
                event.event_id,
            ))
            return events

    async def create_event(self, draft: CalendarEventDraft, idempotency_key: str) -> CalendarEvent:
        if self._prepare is not None:
            await self._prepare()
        async with self._lock:
            calendar_id = await self._resolve_calendar()
            event = await self._provider.create_calendar_event(calendar_id, draft, idempotency_key)
            changed = await self._store.upsert(event)
            try:
                await self._publish(calendar_id, await self.snapshot(), CalendarChanges(added=int(changed)))
            except Exception:
                _LOG.exception("created calendar event but device notification failed")
            return event

    async def _publish(
        self, calendar_id: str, events: Sequence[CalendarEvent], changes: CalendarChanges,
    ) -> None:
        if self._notify is not None:
            await self._notify(calendar_id, events, changes)

    async def handle_event(self, payload: Mapping[str, object]) -> None:
        event = payload.get("event")
        if not isinstance(event, Mapping):
            return
        calendar_id = event.get("calendar_id")
        if self._calendar_id and calendar_id != self._calendar_id:
            return
        header = payload.get("header")
        event_id = header.get("event_id", "") if isinstance(header, Mapping) else ""
        _LOG.info(
            "received Feishu calendar change calendar_id=%s event_id=%s", calendar_id, event_id,
        )
        try:
            await self.sync_once()
        except Exception:
            _LOG.exception("Feishu calendar event sync failed")

    async def snapshot(
        self, *, unbounded: bool = False, window: tuple[datetime, datetime] | None = None,
    ) -> Sequence[CalendarEvent]:
        calendar_id = self._calendar_id
        if not calendar_id:
            return ()
        return await self._store.list(
            calendar_id, None if unbounded else self._max_events,
            window=window,
        )

    async def status(self) -> Mapping[str, object]:
        calendar_id = self._calendar_id
        result = dict(await self._store.status(calendar_id)) if calendar_id else {
            "calendar_id": "",
            "revision": 0,
            "sync_token_configured": False,
            "last_sync_at": "",
            "event_count": 0,
        }
        return {**result, "poll_interval_seconds": self._interval,
                "polling": bool(self._task and not self._task.done()),
                "error": self._last_error}

    async def _run(self) -> None:
        while True:
            try:
                await self.sync_once()
                self._last_error = ""
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                self._last_error = str(exc)[:500]
                _LOG.exception("Feishu calendar polling failed")
            if self._interval == 0:
                _LOG.info("Feishu calendar periodic polling disabled")
                return
            await asyncio.sleep(self._interval)

    def start(self) -> None:
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self._run(), name="binbot-feishu-calendar-sync")

    async def stop(self) -> None:
        task = self._task
        self._task = None
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def close(self) -> None:
        await self.stop()
        await self._store.close()
