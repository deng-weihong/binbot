"""SQLite documents and idempotent events for device workspaces."""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime
import hashlib
import json
from pathlib import Path
import sqlite3
from typing import Any, Callable, TypeVar, cast
from uuid import uuid4

from binbot_server.domain.errors import StateConflict

T = TypeVar("T")


class SqliteWorkspaceStore:
    def __init__(self, path: str):
        if path != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(path, timeout=10, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA busy_timeout=10000")
        self._db.executescript(
            """
            CREATE TABLE IF NOT EXISTS workspace_documents (
                device_id TEXT PRIMARY KEY, revision TEXT NOT NULL,
                document TEXT NOT NULL, updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS workspace_events (
                device_id TEXT NOT NULL, event_id TEXT NOT NULL,
                fingerprint TEXT NOT NULL, created_at TEXT NOT NULL,
                PRIMARY KEY(device_id, event_id)
            );
            CREATE TABLE IF NOT EXISTS web_chat_requests (
                conversation_key TEXT NOT NULL, request_id TEXT NOT NULL,
                fingerprint TEXT NOT NULL, status TEXT NOT NULL,
                response TEXT NOT NULL DEFAULT '{}', created_at TEXT NOT NULL,
                PRIMARY KEY(conversation_key, request_id)
            );
            CREATE TABLE IF NOT EXISTS workspace_gallery (
                device_id TEXT NOT NULL, image_id TEXT NOT NULL,
                metadata TEXT NOT NULL, PRIMARY KEY(device_id, image_id)
            );
            CREATE TABLE IF NOT EXISTS workspace_calendar_exports (
                device_id TEXT NOT NULL, schedule_id TEXT NOT NULL,
                destination TEXT NOT NULL, fingerprint TEXT NOT NULL,
                response TEXT NOT NULL,
                PRIMARY KEY(device_id, schedule_id, destination)
            );
        """
        )
        self._db.commit()
        self._lock = asyncio.Lock()

    async def gallery(self, device_id: str) -> list[dict[str, Any]]:
        return await self._transaction(
            lambda db: [
                json.loads(row[0])
                for row in db.execute(
                    "SELECT metadata FROM workspace_gallery WHERE device_id=? ORDER BY rowid DESC",
                    (device_id,),
                )
            ]
        )

    async def calendar_export(
        self, device_id: str, schedule_id: str, destination: str, payload: Any,
    ) -> dict[str, Any] | None:
        fingerprint = hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=False).encode()).hexdigest()

        def operation(db: sqlite3.Connection) -> dict[str, Any] | None:
            row = db.execute(
                "SELECT fingerprint,response FROM workspace_calendar_exports "
                "WHERE device_id=? AND schedule_id=? AND destination=?",
                (device_id, schedule_id, destination),
            ).fetchone()
            if row is None:
                return None
            if row[0] != fingerprint:
                raise StateConflict("这条日程已经同步飞书，不能用同一编号创建不同内容")
            return cast(dict[str, Any], json.loads(row[1]))

        return await self._transaction(operation)

    async def remember_calendar_export(
        self, device_id: str, schedule_id: str, destination: str, payload: Any, response: Any,
    ) -> None:
        fingerprint = hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        await self._transaction(lambda db: db.execute(
            "INSERT INTO workspace_calendar_exports VALUES (?,?,?,?,?) "
            "ON CONFLICT(device_id,schedule_id,destination) DO NOTHING",
            (device_id, schedule_id, destination, fingerprint, json.dumps(response, ensure_ascii=False)),
        ).rowcount)

    async def gallery_put(self, device_id: str, image_id: str, metadata: dict[str, Any]) -> None:
        await self._transaction(
            lambda db: db.execute(
                "INSERT INTO workspace_gallery VALUES (?, ?, ?) "
                "ON CONFLICT(device_id, image_id) DO UPDATE SET metadata=excluded.metadata",
                (device_id, image_id, json.dumps(metadata, ensure_ascii=False)),
            ).rowcount
        )

    async def gallery_delete(self, device_id: str, image_id: str) -> None:
        await self._transaction(
            lambda db: db.execute(
                "DELETE FROM workspace_gallery WHERE device_id=? AND image_id=?",
                (device_id, image_id),
            ).rowcount
        )

    async def _transaction(self, operation: Callable[[sqlite3.Connection], T]) -> T:
        async with self._lock:

            def execute() -> T:
                try:
                    self._db.execute("BEGIN IMMEDIATE")
                    value = operation(self._db)
                    self._db.commit()
                    return value
                except BaseException:
                    self._db.rollback()
                    raise

            # Cancellation does not stop a worker thread. Keep the lock until its
            # transaction ends so shutdown cannot overlap another BEGIN or close.
            task = asyncio.create_task(asyncio.to_thread(execute))
            try:
                return await asyncio.shield(task)
            except asyncio.CancelledError:
                await asyncio.shield(task)
                raise

    @staticmethod
    def _snapshot(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "device_id": row["device_id"],
            "revision": row["revision"],
            "data": json.loads(row["document"]),
            "updated_at": row["updated_at"],
        }

    async def read(self, device_id: str, defaults: dict[str, Any]) -> dict[str, Any]:
        def operation(db: sqlite3.Connection) -> dict[str, Any]:
            row = db.execute(
                "SELECT * FROM workspace_documents WHERE device_id=?", (device_id,),
            ).fetchone()
            if row is not None:
                return self._snapshot(row)
            db.execute(
                "INSERT INTO workspace_documents VALUES (?, ?, ?, ?)",
                (
                    device_id,
                    str(uuid4()),
                    json.dumps(defaults, ensure_ascii=False),
                    datetime.now(UTC).isoformat(),
                ),
            )
            row = db.execute(
                "SELECT * FROM workspace_documents WHERE device_id=?", (device_id,)
            ).fetchone()
            assert row is not None
            return self._snapshot(row)

        return await self._transaction(operation)

    async def update(
        self,
        device_id: str,
        revision: str,
        change: Callable[[dict[str, Any]], dict[str, Any]],
        *,
        event_id: str = "",
        event_payload: Any = None,
    ) -> dict[str, Any]:
        fingerprint = hashlib.sha256(
            json.dumps(event_payload, sort_keys=True, ensure_ascii=False).encode()
        ).hexdigest()

        def operation(db: sqlite3.Connection) -> dict[str, Any]:
            row = db.execute(
                "SELECT * FROM workspace_documents WHERE device_id=?", (device_id,)
            ).fetchone()
            if row is None:
                raise StateConflict("设备工作台尚未初始化")
            if event_id:
                event = db.execute(
                    "SELECT fingerprint FROM workspace_events " "WHERE device_id=? AND event_id=?",
                    (device_id, event_id),
                ).fetchone()
                if event is not None:
                    if event["fingerprint"] != fingerprint:
                        raise StateConflict("同一操作编号已用于不同内容")
                    return self._snapshot(row)
            if revision and revision != row["revision"]:
                raise StateConflict("设置已在其他页面或设备上更新，请刷新后重试")
            value = change(json.loads(row["document"]))
            now = datetime.now(UTC).isoformat()
            db.execute(
                "UPDATE workspace_documents SET revision=?, document=?, updated_at=? "
                "WHERE device_id=?",
                (
                    str(uuid4()),
                    json.dumps(value, ensure_ascii=False, allow_nan=False),
                    now,
                    device_id,
                ),
            )
            if event_id:
                db.execute(
                    "INSERT INTO workspace_events VALUES (?, ?, ?, ?)",
                    (device_id, event_id, fingerprint, now),
                )
            updated = db.execute(
                "SELECT * FROM workspace_documents WHERE device_id=?", (device_id,)
            ).fetchone()
            assert updated is not None
            return self._snapshot(updated)

        return await self._transaction(operation)

    async def event_seen(self, device_id: str, event_id: str, payload: Any) -> bool:
        fingerprint = hashlib.sha256(
            json.dumps(payload, sort_keys=True, ensure_ascii=False).encode()
        ).hexdigest()

        def operation(db: sqlite3.Connection) -> bool:
            row = db.execute(
                "SELECT fingerprint FROM workspace_events " "WHERE device_id=? AND event_id=?",
                (device_id, event_id),
            ).fetchone()
            if row is not None and row["fingerprint"] != fingerprint:
                raise StateConflict("同一操作编号已用于不同内容")
            return row is not None

        return await self._transaction(operation)

    async def chat_claim(self, key: str, request_id: str, text: str) -> dict[str, Any] | None:
        fingerprint = hashlib.sha256(text.encode()).hexdigest()

        def operation(db: sqlite3.Connection) -> dict[str, Any] | None:
            row = db.execute(
                "SELECT * FROM web_chat_requests WHERE conversation_key=? AND request_id=?",
                (key, request_id),
            ).fetchone()
            if row is not None:
                if row["fingerprint"] != fingerprint:
                    raise StateConflict("请求编号已经用于另一条消息")
                if row["status"] == "completed":
                    return cast(dict[str, Any], json.loads(row["response"]))
                if row["status"] != "retryable":
                    raise StateConflict("上次请求尚未确认，请读取对话和设备状态后再发送新消息")
                db.execute(
                    "UPDATE web_chat_requests SET status='running' "
                    "WHERE conversation_key=? AND request_id=?",
                    (key, request_id),
                )
                return None
            db.execute(
                "INSERT INTO web_chat_requests VALUES (?, ?, ?, 'running', '{}', ?)",
                (key, request_id, fingerprint, datetime.now(UTC).isoformat()),
            )
            return None

        return await self._transaction(operation)

    async def chat_finish(
        self, key: str, request_id: str, response: dict[str, Any], status: str = "completed"
    ) -> None:
        await self._transaction(
            lambda db: db.execute(
                "UPDATE web_chat_requests SET status=?, response=? "
                "WHERE conversation_key=? AND request_id=?",
                (status, json.dumps(response, ensure_ascii=False), key, request_id),
            )
        )

    async def close(self) -> None:
        async with self._lock:
            await asyncio.to_thread(self._db.close)
