"""SQLite-backed stores for conversation history and device control state."""

from __future__ import annotations

import asyncio
import json
import sqlite3
from collections.abc import Sequence
from datetime import datetime
from pathlib import Path
from typing import Any
from uuid import UUID, uuid4

from binbot_server.application.device_control import (
    CommandRecord, DeviceControlStore, _now, offline_device_snapshot,
)
from binbot_server.application.device_commands import WORKSPACE_COMMANDS
from binbot_server.application.calendar import CalendarEvent, CalendarStore, select_calendar_events
from binbot_server.application.ports import ConversationSummary, MemoryItem, MemoryStore
from binbot_server.domain.model import ChatMessage, legacy_web_conversation_prefix
from binbot_server.transport.protocol import DeviceCommandResultMessage, DeviceStateMessage


def _connect(path: str) -> sqlite3.Connection:
    if path != ":memory:":
        Path(path).expanduser().parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(path, check_same_thread=False)
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA busy_timeout=5000")
    return connection


class SqliteConversationStore:
    """Persistent bounded conversation history isolated by device identifier."""

    def __init__(self, path: str, max_messages: int):
        if max_messages < 0:
            raise ValueError("max_messages must not be negative")
        self._max_messages = max_messages
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS conversation_messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL
            )
            """
        )
        self._connection.execute(
            "CREATE INDEX IF NOT EXISTS conversation_messages_device_id_id "
            "ON conversation_messages(device_id, id)"
        )
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS conversation_index (
                conversation_key TEXT PRIMARY KEY,
                message_count INTEGER NOT NULL,
                updated_at TEXT NOT NULL
            )
            """
        )
        self._connection.execute(
            """
            INSERT OR IGNORE INTO conversation_index(conversation_key, message_count, updated_at)
            SELECT device_id, COUNT(*), ?
            FROM conversation_messages
            GROUP BY device_id
            """,
            (_now(),),
        )
        self._connection.commit()

    def _load_sync(self, device_id: str) -> tuple[ChatMessage, ...]:
        rows = self._connection.execute(
            "SELECT role, content FROM conversation_messages "
            "WHERE device_id = ? ORDER BY id",
            (device_id,),
        ).fetchall()
        return tuple(ChatMessage(role=str(row[0]), content=str(row[1])) for row in rows)

    async def load(self, device_id: str) -> Sequence[ChatMessage]:
        async with self._lock:
            return self._load_sync(device_id)

    async def load_device_history(self, device_id: str) -> Sequence[ChatMessage]:
        prefix = legacy_web_conversation_prefix(device_id)
        # The global row ID preserves write order across the retained conversation keys.
        async with self._lock:
            rows = self._connection.execute(
                """
                SELECT role, content FROM conversation_messages
                WHERE device_id = ? OR (device_id >= ? AND device_id < ?)
                ORDER BY id DESC LIMIT ?
                """,
                (device_id, prefix, prefix[:-1] + ";", self._max_messages),
            ).fetchall()
        return tuple(ChatMessage(role=row[0], content=row[1]) for row in reversed(rows))

    def _append_sync(
        self, device_id: str, user_text: str, assistant_text: str
    ) -> tuple[ChatMessage, ...]:
        self._connection.execute(
            "INSERT INTO conversation_messages(device_id, role, content) VALUES (?, ?, ?)",
            (device_id, "user", user_text),
        )
        self._connection.execute(
            "INSERT INTO conversation_messages(device_id, role, content) VALUES (?, ?, ?)",
            (device_id, "assistant", assistant_text),
        )
        if self._max_messages == 0:
            self._connection.execute(
                "DELETE FROM conversation_messages WHERE device_id = ?", (device_id,)
            )
            self._connection.execute(
                "DELETE FROM conversation_index WHERE conversation_key = ?", (device_id,)
            )
        else:
            self._connection.execute(
                """
                DELETE FROM conversation_messages
                WHERE device_id = ? AND id NOT IN (
                    SELECT id FROM conversation_messages
                    WHERE device_id = ? ORDER BY id DESC LIMIT ?
                )
                """,
                (device_id, device_id, self._max_messages),
            )
            count = self._connection.execute(
                "SELECT COUNT(*) FROM conversation_messages WHERE device_id = ?",
                (device_id,),
            ).fetchone()[0]
            self._connection.execute(
                """
                INSERT INTO conversation_index(conversation_key, message_count, updated_at)
                VALUES (?, ?, ?)
                ON CONFLICT(conversation_key) DO UPDATE SET
                    message_count = excluded.message_count,
                    updated_at = excluded.updated_at
                """,
                (device_id, int(count), _now()),
            )
        self._connection.commit()
        return self._load_sync(device_id)

    async def append_turn(
        self, device_id: str, user_text: str, assistant_text: str, *, source: str = ""
    ) -> Sequence[ChatMessage]:
        async with self._lock:
            return self._append_sync(device_id, user_text, assistant_text)

    async def list_conversations(
        self, prefix: str = "", limit: int = 50
    ) -> Sequence[ConversationSummary]:
        async with self._lock:
            rows = self._connection.execute(
                "SELECT conversation_key, message_count, updated_at "
                "FROM conversation_index ORDER BY updated_at DESC"
            ).fetchall()
            entries = [
                ConversationSummary(str(row[0]), int(row[1]), str(row[2]))
                for row in rows
                if str(row[0]).startswith(prefix)
            ]
            return tuple(entries[: max(1, min(limit, 100))])

    def _close_sync(self) -> None:
        self._connection.close()

    async def close(self) -> None:
        async with self._lock:
            self._close_sync()


class SqliteMemoryStore(MemoryStore):
    """Persistent keyword-searchable long-term memory."""

    def __init__(self, path: str, max_items_per_device: int = 500):
        if max_items_per_device < 1:
            raise ValueError("max_items_per_device must be positive")
        self._max_items = max_items_per_device
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS memory_items (
                memory_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                content TEXT NOT NULL,
                tags_json TEXT NOT NULL,
                created_at TEXT NOT NULL
            )
            """
        )
        self._connection.execute(
            "CREATE INDEX IF NOT EXISTS memory_items_device_created "
            "ON memory_items(device_id, created_at DESC)"
        )
        self._connection.commit()

    @staticmethod
    def _item(row: tuple[Any, ...]) -> MemoryItem:
        tags = json.loads(str(row[3]))
        return MemoryItem(
            memory_id=UUID(str(row[0])), device_id=str(row[1]),
            content=str(row[2]), tags=tuple(str(tag) for tag in tags),
            created_at=str(row[4]),
        )

    def _remember_sync(
        self, device_id: str, content: str, tags: Sequence[str]
    ) -> MemoryItem:
        value = content.strip()
        if not value or len(value) > 2_000:
            raise ValueError("memory content must be 1..2000 characters")
        item = MemoryItem(uuid4(), device_id, value, tuple(tags), _now())
        self._connection.execute(
            "INSERT INTO memory_items(memory_id, device_id, content, tags_json, created_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (str(item.memory_id), device_id, value, json.dumps(item.tags), item.created_at),
        )
        self._connection.execute(
            """
            DELETE FROM memory_items WHERE device_id = ? AND memory_id NOT IN (
                SELECT memory_id FROM memory_items
                WHERE device_id = ? ORDER BY created_at DESC LIMIT ?
            )
            """,
            (device_id, device_id, self._max_items),
        )
        self._connection.commit()
        return item

    async def remember(
        self, device_id: str, content: str, tags: Sequence[str] = ()
    ) -> MemoryItem:
        async with self._lock:
            return self._remember_sync(device_id, content, tags)

    def _search_sync(
        self, device_id: str, query: str, limit: int
    ) -> tuple[MemoryItem, ...]:
        rows = self._connection.execute(
            """
            SELECT memory_id, device_id, content, tags_json, created_at
            FROM memory_items WHERE device_id = ?
            ORDER BY created_at DESC LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
        items = [self._item(row) for row in rows]
        words = [word.casefold() for word in query.split() if word.strip()]
        for word in tuple(words):
            if len(word) >= 2:
                words.extend(word[index : index + 2] for index in range(len(word) - 1))
        if not words:
            return tuple(items)
        return tuple(
            item for item in items
            if any(word in (item.content + " " + " ".join(item.tags)).casefold() for word in words)
        )

    async def search(
        self, device_id: str, query: str, limit: int = 5
    ) -> Sequence[MemoryItem]:
        async with self._lock:
            return self._search_sync(device_id, query, max(1, min(limit, 100)))

    def _forget_sync(self, device_id: str, memory_id: UUID) -> bool:
        cursor = self._connection.execute(
            "DELETE FROM memory_items WHERE device_id = ? AND memory_id = ?",
            (device_id, str(memory_id)),
        )
        self._connection.commit()
        return cursor.rowcount > 0

    async def forget(self, device_id: str, memory_id: UUID) -> bool:
        async with self._lock:
            return self._forget_sync(device_id, memory_id)

    def _close_sync(self) -> None:
        self._connection.close()

    async def close(self) -> None:
        async with self._lock:
            self._close_sync()


class SqliteDeviceControlStore(DeviceControlStore):
    """Persistent device state and bounded command history."""

    def __init__(self, path: str, max_commands: int = 500):
        if max_commands < 1:
            raise ValueError("max_commands must be positive")
        super().__init__(max_commands=max_commands)
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS device_states (
                device_id TEXT PRIMARY KEY,
                revision INTEGER NOT NULL,
                state_json TEXT NOT NULL,
                updated_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_commands (
                command_id TEXT PRIMARY KEY,
                device_id TEXT NOT NULL,
                name TEXT NOT NULL,
                payload_json TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at TEXT NOT NULL,
                updated_at TEXT NOT NULL,
                message TEXT NOT NULL DEFAULT '',
                idempotency_key TEXT
            );
            CREATE INDEX IF NOT EXISTS device_commands_device_created
                ON device_commands(device_id, created_at DESC);
            CREATE TABLE IF NOT EXISTS device_inventory (
                device_id TEXT PRIMARY KEY,
                snapshot_json TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS device_control_v2_states (
                device_id TEXT PRIMARY KEY,
                state_json TEXT NOT NULL
            );
            """
        )
        columns = {
            str(row[1])
            for row in self._connection.execute("PRAGMA table_info(device_commands)")
        }
        if "idempotency_key" not in columns:
            self._connection.execute(
                "ALTER TABLE device_commands ADD COLUMN idempotency_key TEXT"
            )
        self._connection.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS device_commands_idempotency "
            "ON device_commands(device_id, idempotency_key) "
            "WHERE idempotency_key IS NOT NULL"
        )
        self._connection.commit()

    async def remember_device(self, snapshot: dict[str, object]) -> None:
        async with self._lock:
            self._connection.execute(
                "INSERT INTO device_inventory(device_id, snapshot_json) VALUES (?, ?) "
                "ON CONFLICT(device_id) DO UPDATE SET snapshot_json = excluded.snapshot_json",
                (str(snapshot["id"]), json.dumps(snapshot)),
            )
            self._connection.commit()

    async def known_devices(self) -> list[dict[str, object]]:
        async with self._lock:
            rows = self._connection.execute(
                "SELECT ids.device_id, inventory.snapshot_json, states.updated_at "
                "FROM (SELECT device_id FROM device_inventory UNION "
                "SELECT device_id FROM device_states UNION "
                "SELECT device_id FROM device_commands UNION "
                "SELECT device_id FROM device_control_v2_states) AS ids "
                "LEFT JOIN device_inventory AS inventory ON inventory.device_id = ids.device_id "
                "LEFT JOIN device_states AS states ON states.device_id = ids.device_id "
                "ORDER BY ids.device_id"
            ).fetchall()
            result = []
            for row in rows:
                snapshot = offline_device_snapshot(
                    str(row[0]), json.loads(str(row[1])) if row[1] is not None else None
                )
                if row[2] is not None:
                    snapshot["last_seen_at"] = str(row[2])
                result.append(snapshot)
            return result

    def _update_state_sync(self, device_id: str, state: DeviceStateMessage) -> None:
        previous = self._connection.execute(
            "SELECT revision FROM device_states WHERE device_id = ?", (device_id,)
        ).fetchone()
        if previous is not None and int(previous[0]) > state.revision:
            return
        snapshot = state.model_dump(exclude={"type"})
        updated_at = _now()
        self._connection.execute(
            """
            INSERT INTO device_states(device_id, revision, state_json, updated_at)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                revision=excluded.revision,
                state_json=excluded.state_json,
                updated_at=excluded.updated_at
            """,
            (device_id, state.revision, json.dumps(snapshot), updated_at),
        )
        self._connection.commit()

    async def update_state(self, device_id: str, state: DeviceStateMessage) -> None:
        async with self._lock:
            self._update_state_sync(device_id, state)

    def _device_connected_sync(self, device_id: str) -> None:
        self._connection.execute("DELETE FROM device_states WHERE device_id = ?", (device_id,))
        self._connection.commit()

    async def device_connected(self, device_id: str) -> None:
        async with self._lock:
            self._device_connected_sync(device_id)

    def _state_sync(self, device_id: str) -> dict[str, object] | None:
        row = self._connection.execute(
            "SELECT revision, state_json, updated_at FROM device_states WHERE device_id = ?",
            (device_id,),
        ).fetchone()
        if row is None:
            return None
        state = json.loads(str(row[1]))
        if not isinstance(state, dict):
            return None
        state["revision"] = int(row[0])
        state["updated_at"] = str(row[2])
        return state

    async def state(self, device_id: str) -> dict[str, object] | None:
        async with self._lock:
            state = self._state_sync(device_id)
            row = self._connection.execute(
                "SELECT state_json FROM device_control_v2_states WHERE device_id = ?", (device_id,)
            ).fetchone()
            if row is not None:
                control = json.loads(row[0])
                return {**(state or {}), "control": control, "updated_at": control["updated_at"]}
            return state

    async def update_control_state(self, device_id: str, state: dict[str, Any]) -> None:
        async with self._lock:
            row = self._connection.execute(
                "SELECT state_json FROM device_control_v2_states WHERE device_id = ?", (device_id,)
            ).fetchone()
            previous = json.loads(row[0]) if row else None
            if previous and previous["boot_id"] == state["boot_id"] and previous["revision"] > state["revision"]:
                return
            snapshot = {**state, "updated_at": _now()}
            self._connection.execute(
                "INSERT INTO device_control_v2_states(device_id, state_json) VALUES (?, ?) "
                "ON CONFLICT(device_id) DO UPDATE SET state_json=excluded.state_json",
                (device_id, json.dumps(snapshot)),
            )
            self._connection.commit()

    def _record_from_row(self, row: tuple[Any, ...]) -> CommandRecord:
        return CommandRecord(
            command_id=UUID(str(row[0])),
            device_id=str(row[1]),
            name=str(row[2]),
            payload=dict(json.loads(str(row[3]))),
            status=str(row[4]),
            created_at=str(row[5]),
            updated_at=str(row[6]),
            message=str(row[7]),
            idempotency_key=str(row[8]) if row[8] is not None else None,
        )

    def _create_command_sync(
        self, device_id: str, name: str, payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> CommandRecord:
        record = CommandRecord(
            command_id=uuid4(),
            device_id=device_id,
            name=name,
            payload=dict(payload),
            status="queued",
            created_at=_now(),
            updated_at=_now(),
            idempotency_key=idempotency_key,
        )
        self._connection.execute(
            """
            INSERT INTO device_commands(
                command_id, device_id, name, payload_json, status,
                created_at, updated_at, message, idempotency_key
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                str(record.command_id), record.device_id, record.name,
                json.dumps(record.payload), record.status, record.created_at,
                record.updated_at, record.message, record.idempotency_key,
            ),
        )
        self._connection.execute(
            """
            DELETE FROM device_commands WHERE command_id NOT IN (
                SELECT command_id FROM device_commands ORDER BY created_at DESC LIMIT ?
            )
            """,
            (self._max_commands,),
        )
        self._connection.commit()
        return record

    async def create_command(
        self, device_id: str, name: str, payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> CommandRecord:
        record, _ = await self.get_or_create_command(
            device_id, name, payload, idempotency_key
        )
        return record

    def _find_idempotent_sync(
        self, device_id: str, idempotency_key: str
    ) -> CommandRecord | None:
        row = self._connection.execute(
            """
            SELECT command_id, device_id, name, payload_json, status,
                   created_at, updated_at, message, idempotency_key
            FROM device_commands
            WHERE device_id = ? AND idempotency_key = ?
            """,
            (device_id, idempotency_key),
        ).fetchone()
        return self._record_from_row(row) if row is not None else None

    async def get_or_create_command(
        self, device_id: str, name: str, payload: dict[str, object],
        idempotency_key: str | None = None,
    ) -> tuple[CommandRecord, bool]:
        async with self._lock:
            if idempotency_key is not None:
                existing = self._find_idempotent_sync(device_id, idempotency_key)
                if existing is not None:
                    return existing, False
            return self._create_command_sync(
                device_id, name, payload, idempotency_key
            ), True

    def _update_command_sync(
        self, command_id: UUID, status: str, message: str | None = None
    ) -> None:
        if message is None:
            self._connection.execute(
                "UPDATE device_commands SET status = ?, updated_at = ? "
                "WHERE command_id = ? AND status = 'queued'",
                (status, _now(), str(command_id)),
            )
        else:
            self._connection.execute(
                "UPDATE device_commands SET status = ?, message = ?, updated_at = ? "
                "WHERE command_id = ?",
                (status, message[:256], _now(), str(command_id)),
            )
        self._connection.commit()

    async def mark_delivered(self, command_id: UUID) -> None:
        async with self._lock:
            self._update_command_sync(command_id, "delivered")

    async def mark_failed(self, command_id: UUID, message: str) -> None:
        async with self._lock:
            self._update_command_sync(command_id, "failed", message)

    async def fail_unconfirmed_control_commands(self) -> None:
        async with self._lock:
            names = sorted({"screen.brightness.set", "audio.volume.set"} | WORKSPACE_COMMANDS)
            self._connection.execute(
                "UPDATE device_commands SET status = 'failed', message = ?, updated_at = ? "
                "WHERE status IN ('queued', 'delivered') AND name IN (" + ','.join('?' for _ in names) + ')',
                ("控制服务已重启，执行结果未确认", _now(), *names),
            )
            self._connection.commit()

    def _complete_sync(self, device_id: str, result: DeviceCommandResultMessage) -> None:
        self._connection.execute(
            """
            UPDATE device_commands SET status = ?, message = ?, updated_at = ?
            WHERE command_id = ? AND device_id = ?
            """,
            (
                "completed" if result.success else "failed", result.message[:256], _now(),
                str(result.command_id), device_id,
            ),
        )
        self._connection.commit()

    async def complete(
        self, device_id: str, result: DeviceCommandResultMessage
    ) -> None:
        async with self._lock:
            self._complete_sync(device_id, result)

    def _command_sync(self, command_id: UUID) -> dict[str, object] | None:
        row = self._connection.execute(
            """
            SELECT command_id, device_id, name, payload_json, status,
                   created_at, updated_at, message, idempotency_key
            FROM device_commands WHERE command_id = ?
            """,
            (str(command_id),),
        ).fetchone()
        return self._record_from_row(row).snapshot() if row is not None else None

    async def command(self, command_id: UUID) -> dict[str, object] | None:
        async with self._lock:
            return self._command_sync(command_id)

    def _recent_commands_sync(self, device_id: str, limit: int) -> list[dict[str, object]]:
        rows = self._connection.execute(
            """
            SELECT command_id, device_id, name, payload_json, status,
                   created_at, updated_at, message, idempotency_key
            FROM device_commands WHERE device_id = ?
            ORDER BY created_at DESC LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
        return [self._record_from_row(row).snapshot() for row in rows]

    async def recent_commands(
        self, device_id: str, limit: int = 20
    ) -> list[dict[str, object]]:
        async with self._lock:
            return self._recent_commands_sync(device_id, limit)

    def _close_sync(self) -> None:
        self._connection.close()

    async def close(self) -> None:
        async with self._lock:
            self._close_sync()


class SqliteCalendarStore(CalendarStore):
    """Persist calendar snapshots and sync cursors across server restarts."""

    def __init__(self, path: str):
        self._connection = _connect(path)
        self._lock = asyncio.Lock()
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS calendar_events (
                calendar_id TEXT NOT NULL,
                event_id TEXT NOT NULL,
                payload_json TEXT NOT NULL,
                PRIMARY KEY(calendar_id, event_id)
            )
            """
        )
        self._connection.execute(
            """
            CREATE TABLE IF NOT EXISTS calendar_sync_state (
                calendar_id TEXT PRIMARY KEY,
                sync_token TEXT NOT NULL,
                revision INTEGER NOT NULL DEFAULT 0,
                last_sync_at TEXT NOT NULL
            )
            """
        )
        self._connection.commit()

    @staticmethod
    def _event(payload: str) -> CalendarEvent:
        value = json.loads(payload)
        return CalendarEvent(**value)

    async def get(self, calendar_id: str, event_id: str) -> CalendarEvent | None:
        async with self._lock:
            row = self._connection.execute(
                "SELECT payload_json FROM calendar_events WHERE calendar_id = ? AND event_id = ?",
                (calendar_id, event_id),
            ).fetchone()
            return self._event(str(row[0])) if row is not None else None

    def _touch(self, calendar_id: str, changed: bool) -> None:
        self._connection.execute(
            """
            INSERT INTO calendar_sync_state(calendar_id, sync_token, revision, last_sync_at)
            VALUES (?, '', ?, ?)
            ON CONFLICT(calendar_id) DO UPDATE SET
                revision = calendar_sync_state.revision + ?, last_sync_at = excluded.last_sync_at
            """,
            (calendar_id, 1 if changed else 0, _now(), 1 if changed else 0),
        )

    async def upsert(self, event: CalendarEvent) -> bool:
        payload = json.dumps(event.snapshot(), ensure_ascii=False, sort_keys=True)
        async with self._lock:
            row = self._connection.execute(
                "SELECT payload_json FROM calendar_events WHERE calendar_id = ? AND event_id = ?",
                (event.calendar_id, event.event_id),
            ).fetchone()
            changed = row is None or str(row[0]) != payload
            self._connection.execute(
                "INSERT INTO calendar_events(calendar_id, event_id, payload_json) VALUES (?, ?, ?) "
                "ON CONFLICT(calendar_id, event_id) DO UPDATE SET payload_json = excluded.payload_json",
                (event.calendar_id, event.event_id, payload),
            )
            self._touch(event.calendar_id, changed)
            self._connection.commit()
            return changed

    async def delete(self, calendar_id: str, event_id: str) -> bool:
        async with self._lock:
            cursor = self._connection.execute(
                "DELETE FROM calendar_events WHERE calendar_id = ? AND event_id = ?",
                (calendar_id, event_id),
            )
            changed = cursor.rowcount > 0
            self._touch(calendar_id, changed)
            self._connection.commit()
            return changed

    async def list(
        self, calendar_id: str, limit: int | None = 16,
        *, window: tuple[datetime, datetime] | None = None,
    ) -> Sequence[CalendarEvent]:
        async with self._lock:
            query = "SELECT payload_json FROM calendar_events WHERE calendar_id = ?"
            params: tuple[Any, ...] = (calendar_id,)
            if window is not None:
                start, end = window
                query += """
                    AND (
                        (CAST(json_extract(payload_json, '$.start_timestamp') AS INTEGER) >= ?
                         AND CAST(json_extract(payload_json, '$.start_timestamp') AS INTEGER) < ?)
                        OR (json_extract(payload_json, '$.start_date') BETWEEN ? AND ?)
                    )
                """
                params += (
                    start.timestamp(),
                    end.timestamp(),
                    start.date().isoformat(),
                    end.date().isoformat(),
                )
            rows = self._connection.execute(query, params).fetchall()
            events = [self._event(str(row[0])) for row in rows]
            return select_calendar_events(events, limit, window)

    async def sync_token(self, calendar_id: str) -> str:
        async with self._lock:
            row = self._connection.execute(
                "SELECT sync_token FROM calendar_sync_state WHERE calendar_id = ?",
                (calendar_id,),
            ).fetchone()
            return str(row[0]) if row is not None else ""

    async def set_sync_token(self, calendar_id: str, token: str) -> None:
        async with self._lock:
            self._connection.execute(
                """
                INSERT INTO calendar_sync_state(calendar_id, sync_token, revision, last_sync_at)
                VALUES (?, ?, 0, ?)
                ON CONFLICT(calendar_id) DO UPDATE SET
                    sync_token = excluded.sync_token, last_sync_at = excluded.last_sync_at
                """,
                (calendar_id, token, _now()),
            )
            self._connection.commit()

    async def status(self, calendar_id: str) -> dict[str, object]:
        async with self._lock:
            row = self._connection.execute(
                "SELECT sync_token, revision, last_sync_at FROM calendar_sync_state WHERE calendar_id = ?",
                (calendar_id,),
            ).fetchone()
            count = self._connection.execute(
                "SELECT COUNT(*) FROM calendar_events WHERE calendar_id = ?",
                (calendar_id,),
            ).fetchone()
            return {
                "calendar_id": calendar_id,
                "revision": int(row[1]) if row is not None else 0,
                "sync_token_configured": bool(row and row[0]),
                "last_sync_at": str(row[2]) if row is not None else "",
                "event_count": int(count[0]) if count is not None else 0,
            }

    async def close(self) -> None:
        async with self._lock:
            self._connection.close()
