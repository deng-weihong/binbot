"""Task stores backed by process memory or SQLite."""

from __future__ import annotations

import asyncio
import json
from collections.abc import Sequence
from datetime import UTC, datetime
from typing import Any
from uuid import UUID, uuid4

from binbot_server.application.ports import TaskItem, TaskStore

from .sqlite import _connect


def _now() -> str:
    return datetime.now(UTC).isoformat()


def _due(run_at: str, now: str) -> bool:
    return run_at <= now


class InMemoryTaskStore(TaskStore):
    def __init__(self, max_tasks: int = 500):
        if max_tasks < 1:
            raise ValueError("max_tasks must be positive")
        self._max_tasks = max_tasks
        self._items: dict[UUID, TaskItem] = {}
        self._order: list[UUID] = []
        self._lock = asyncio.Lock()

    async def create(
        self, device_id: str, name: str, payload: dict[str, object], run_at: str
    ) -> TaskItem:
        timestamp = _now()
        item = TaskItem(
            uuid4(), device_id, name, dict(payload), run_at, "pending", timestamp, timestamp
        )
        async with self._lock:
            self._items[item.task_id] = item
            self._order.append(item.task_id)
            while len(self._order) > self._max_tasks:
                self._items.pop(self._order.pop(0), None)
        return item

    async def list(
        self, device_id: str, limit: int = 50, statuses: Sequence[str] = ()
    ) -> tuple[TaskItem, ...]:
        allowed = set(statuses)
        async with self._lock:
            return tuple(
                self._items[item_id]
                for item_id in reversed(self._order)
                if item_id in self._items
                and self._items[item_id].device_id == device_id
                and (not allowed or self._items[item_id].status in allowed)
            )[: max(1, min(limit, 100))]

    async def get(self, device_id: str, task_id: UUID) -> TaskItem | None:
        async with self._lock:
            item = self._items.get(task_id)
            return item if item is not None and item.device_id == device_id else None

    async def cancel(self, device_id: str, task_id: UUID) -> bool:
        async with self._lock:
            item = self._items.get(task_id)
            if item is None or item.device_id != device_id or item.status != "pending":
                return False
            self._items[task_id] = TaskItem(
                item.task_id, item.device_id, item.name, item.payload, item.run_at,
                "cancelled", item.created_at, _now(), item.error
            )
            return True

    async def claim_due(self, now: str, limit: int = 10) -> tuple[TaskItem, ...]:
        claimed: list[TaskItem] = []
        async with self._lock:
            for item_id in self._order:
                item = self._items.get(item_id)
                if item is None or item.status != "pending" or not _due(item.run_at, now):
                    continue
                updated = TaskItem(
                    item.task_id, item.device_id, item.name, item.payload, item.run_at,
                    "running", item.created_at, _now(), item.error
                )
                self._items[item_id] = updated
                claimed.append(updated)
                if len(claimed) >= max(1, limit):
                    break
        return tuple(claimed)

    async def finish(self, task_id: UUID, success: bool, error: str = "") -> None:
        async with self._lock:
            item = self._items.get(task_id)
            if item is None or item.status != "running":
                return
            self._items[task_id] = TaskItem(
                item.task_id, item.device_id, item.name, item.payload, item.run_at,
                "completed" if success else "failed", item.created_at, _now(), error[:256]
            )

    async def close(self) -> None:
        return None


class SqliteTaskStore(TaskStore):
    def __init__(self, path: str, max_tasks: int = 500):
        if max_tasks < 1:
            raise ValueError("max_tasks must be positive")
        self._max_tasks = max_tasks
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS scheduled_tasks (
                task_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                name TEXT NOT NULL,
                payload_json TEXT NOT NULL,
                run_at TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                error TEXT NOT NULL DEFAULT ''
            )
            """
        )
        self._connection.execute(
            "CREATE INDEX IF NOT EXISTS scheduled_tasks_due "
            "ON scheduled_tasks(status, run_at)"
        )
        self._connection.execute(
            "CREATE INDEX IF NOT EXISTS scheduled_tasks_device "
            "ON scheduled_tasks(device_id, created_at DESC)"
        )
        self._connection.commit()

    @staticmethod
    def _item(row: tuple[Any, ...]) -> TaskItem:
        payload = json.loads(str(row[3]))
        return TaskItem(
            UUID(str(row[0])), str(row[1]), str(row[2]),
            dict(payload) if isinstance(payload, dict) else {}, str(row[4]),
            str(row[5]), str(row[6]), str(row[7]), str(row[8]),
        )

    def _row(self, task_id: UUID) -> TaskItem | None:
        row = self._connection.execute(
            "SELECT task_id, device_id, name, payload_json, run_at, status, "
            "created_at, updated_at, error FROM scheduled_tasks WHERE task_id = ?",
            (str(task_id),),
        ).fetchone()
        return self._item(row) if row is not None else None

    async def create(
        self, device_id: str, name: str, payload: dict[str, object], run_at: str
    ) -> TaskItem:
        async with self._lock:
            timestamp = _now()
            item = TaskItem(
                uuid4(), device_id, name, dict(payload), run_at, "pending", timestamp, timestamp
            )
            self._connection.execute(
                "INSERT INTO scheduled_tasks(task_id, device_id, name, payload_json, "
                "run_at, status, created_at, updated_at, error) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (str(item.task_id), device_id, name, json.dumps(item.payload), run_at,
                 item.status, timestamp, timestamp, ""),
            )
            self._connection.execute(
                "DELETE FROM scheduled_tasks WHERE task_id NOT IN "
                "(SELECT task_id FROM scheduled_tasks ORDER BY created_at DESC LIMIT ?)",
                (self._max_tasks,),
            )
            self._connection.commit()
            return item

    async def list(
        self, device_id: str, limit: int = 50, statuses: Sequence[str] = ()
    ) -> tuple[TaskItem, ...]:
        async with self._lock:
            safe_limit = max(1, min(limit, 100))
            query = (
                "SELECT task_id, device_id, name, payload_json, run_at, status, "
                "created_at, updated_at, error FROM scheduled_tasks WHERE device_id = ?"
            )
            params: list[Any] = [device_id]
            if statuses:
                query += " AND status IN (" + ",".join("?" for _ in statuses) + ")"
                params.extend(statuses)
            query += " ORDER BY created_at DESC LIMIT ?"
            params.append(safe_limit)
            rows = self._connection.execute(query, params).fetchall()
            return tuple(self._item(row) for row in rows)

    async def get(self, device_id: str, task_id: UUID) -> TaskItem | None:
        async with self._lock:
            item = self._row(task_id)
            return item if item is not None and item.device_id == device_id else None

    async def cancel(self, device_id: str, task_id: UUID) -> bool:
        async with self._lock:
            cursor = self._connection.execute(
                "UPDATE scheduled_tasks SET status = 'cancelled', updated_at = ? "
                "WHERE task_id = ? AND device_id = ? AND status = 'pending'",
                (_now(), str(task_id), device_id),
            )
            self._connection.commit()
            return cursor.rowcount > 0

    async def claim_due(self, now: str, limit: int = 10) -> tuple[TaskItem, ...]:
        async with self._lock:
            rows = self._connection.execute(
                "SELECT task_id FROM scheduled_tasks WHERE status = 'pending' AND run_at <= ? "
                "ORDER BY run_at, created_at LIMIT ?", (now, max(1, limit))
            ).fetchall()
            items: list[TaskItem] = []
            for row in rows:
                task_id = str(row[0])
                updated = _now()
                cursor = self._connection.execute(
                    "UPDATE scheduled_tasks SET status = 'running', updated_at = ? "
                    "WHERE task_id = ? AND status = 'pending'", (updated, task_id)
                )
                if cursor.rowcount:
                    item = self._row(UUID(task_id))
                    if item is not None:
                        items.append(item)
            self._connection.commit()
            return tuple(items)

    async def finish(self, task_id: UUID, success: bool, error: str = "") -> None:
        async with self._lock:
            self._connection.execute(
                "UPDATE scheduled_tasks SET status = ?, error = ?, updated_at = ? "
                "WHERE task_id = ? AND status = 'running'",
                ("completed" if success else "failed", error[:256], _now(), str(task_id)),
            )
            self._connection.commit()

    async def close(self) -> None:
        async with self._lock:
            self._connection.close()
