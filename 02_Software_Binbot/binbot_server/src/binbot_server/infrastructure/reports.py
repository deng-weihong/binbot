"""Daily report persistence, independent of rolling conversation storage."""

from __future__ import annotations

import asyncio
import json
from collections import Counter
from datetime import UTC, datetime, timedelta
from typing import Any

from binbot_server.application.reports import Activity, ActivityBatch

from .sqlite import _connect


class InMemoryReportStore:
    def __init__(self, retention_days: int = 90):
        self._started_at = datetime.now(UTC).isoformat()
        self._retention_days = retention_days
        self._activities: list[Activity] = []
        self._reports: dict[str, dict[str, Any]] = {}

    async def started_at(self) -> str:
        return self._started_at

    async def append(self, activity: Activity) -> None:
        cutoff = (datetime.now(UTC) - timedelta(days=self._retention_days)).isoformat()
        self._activities = [item for item in self._activities if item.created_at >= cutoff]
        if not any(item.activity_id == activity.activity_id for item in self._activities):
            self._activities.append(activity)

    async def activities(self, start: str, end: str, limit: int = 5000) -> ActivityBatch:
        items = sorted(
            (item for item in self._activities if start <= item.created_at < end),
            key=lambda item: item.created_at,
        )
        return ActivityBatch(
            tuple(items[:limit]),
            dict(Counter(item.status for item in items)),
            items[-1].created_at if items else "",
        )

    async def has_reference(self, reference_id: str) -> bool:
        return any(item.reference_id == reference_id for item in self._activities)

    async def save_report(self, report: dict[str, Any]) -> None:
        self._reports[report["date"]] = json.loads(json.dumps(report))

    async def get_report(self, day: str) -> dict[str, Any] | None:
        value = self._reports.get(day)
        return json.loads(json.dumps(value)) if value is not None else None

    async def list_reports(self, limit: int = 31) -> tuple[dict[str, Any], ...]:
        return tuple(
            json.loads(json.dumps(self._reports[key]))
            for key in sorted(self._reports, reverse=True)[:limit]
        )

    async def close(self) -> None:
        pass


class SqliteReportStore:
    def __init__(self, path: str, retention_days: int = 90):
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._retention_days = retention_days
        self._connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS agent_activity (
                activity_id TEXT PRIMARY KEY,
                created_at TEXT NOT NULL,
                kind TEXT NOT NULL,
                source TEXT NOT NULL,
                device_id TEXT NOT NULL,
                title TEXT NOT NULL,
                status TEXT NOT NULL,
                details_json TEXT NOT NULL,
                reference_id TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS agent_activity_created ON agent_activity(created_at);
            CREATE INDEX IF NOT EXISTS agent_activity_reference ON agent_activity(reference_id);
            CREATE TABLE IF NOT EXISTS agent_daily_reports (
                date TEXT PRIMARY KEY,
                report_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS agent_report_metadata (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
        """
        )
        self._connection.execute(
            "INSERT OR IGNORE INTO agent_report_metadata(key, value) VALUES ('started_at', ?)",
            (datetime.now(UTC).isoformat(),),
        )
        self._connection.commit()

    async def started_at(self) -> str:
        async with self._lock:
            row = self._connection.execute(
                "SELECT value FROM agent_report_metadata WHERE key = 'started_at'"
            ).fetchone()
            return str(row[0])

    async def append(self, activity: Activity) -> None:
        async with self._lock:
            self._connection.execute(
                "INSERT OR IGNORE INTO agent_activity VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    activity.activity_id,
                    activity.created_at,
                    activity.kind,
                    activity.source,
                    activity.device_id,
                    activity.title,
                    activity.status,
                    json.dumps(activity.details, ensure_ascii=False),
                    activity.reference_id,
                ),
            )
            cutoff = (datetime.now(UTC) - timedelta(days=self._retention_days)).isoformat()
            self._connection.execute("DELETE FROM agent_activity WHERE created_at < ?", (cutoff,))
            self._connection.commit()

    async def activities(self, start: str, end: str, limit: int = 5000) -> ActivityBatch:
        async with self._lock:
            rows = self._connection.execute(
                "SELECT * FROM agent_activity WHERE created_at >= ? AND created_at < ? "
                "ORDER BY created_at, rowid LIMIT ?",
                (start, end, limit),
            ).fetchall()
            counts = self._connection.execute(
                "SELECT status, COUNT(*) FROM agent_activity "
                "WHERE created_at >= ? AND created_at < ? GROUP BY status",
                (start, end),
            ).fetchall()
            latest = self._connection.execute(
                "SELECT MAX(created_at) FROM agent_activity "
                "WHERE created_at >= ? AND created_at < ?",
                (start, end),
            ).fetchone()
            return ActivityBatch(
                tuple(
                    Activity(
                        str(row[0]),
                        str(row[1]),
                        str(row[2]),
                        str(row[3]),
                        str(row[4]),
                        str(row[5]),
                        str(row[6]),
                        json.loads(str(row[7])),
                        str(row[8]),
                    )
                    for row in rows
                ),
                {str(row[0]): int(row[1]) for row in counts},
                str(latest[0] or ""),
            )

    async def has_reference(self, reference_id: str) -> bool:
        async with self._lock:
            return (
                self._connection.execute(
                    "SELECT 1 FROM agent_activity WHERE reference_id = ? LIMIT 1",
                    (reference_id,),
                ).fetchone()
                is not None
            )

    async def save_report(self, report: dict[str, Any]) -> None:
        async with self._lock:
            self._connection.execute(
                "INSERT INTO agent_daily_reports(date, report_json) VALUES (?, ?) "
                "ON CONFLICT(date) DO UPDATE SET report_json = excluded.report_json",
                (report["date"], json.dumps(report, ensure_ascii=False)),
            )
            self._connection.commit()

    async def get_report(self, day: str) -> dict[str, Any] | None:
        async with self._lock:
            row = self._connection.execute(
                "SELECT report_json FROM agent_daily_reports WHERE date = ?",
                (day,),
            ).fetchone()
            return json.loads(row[0]) if row else None

    async def list_reports(self, limit: int = 31) -> tuple[dict[str, Any], ...]:
        async with self._lock:
            rows = self._connection.execute(
                "SELECT report_json FROM agent_daily_reports ORDER BY date DESC LIMIT ?",
                (limit,),
            ).fetchall()
            return tuple(json.loads(row[0]) for row in rows)

    async def close(self) -> None:
        async with self._lock:
            self._connection.close()
