"""Process-local long-term memory store."""

from __future__ import annotations

import asyncio
from collections.abc import Sequence
from datetime import UTC, datetime
from uuid import UUID, uuid4

from binbot_server.application.ports import MemoryItem, MemoryStore


def _now() -> str:
    return datetime.now(UTC).isoformat()


class InMemoryMemoryStore(MemoryStore):
    def __init__(self, max_items_per_device: int = 500):
        if max_items_per_device < 1:
            raise ValueError("max_items_per_device must be positive")
        self._max_items = max_items_per_device
        self._items: dict[str, list[MemoryItem]] = {}
        self._lock = asyncio.Lock()

    async def remember(
        self, device_id: str, content: str, tags: Sequence[str] = ()
    ) -> MemoryItem:
        value = content.strip()
        if not value or len(value) > 2_000:
            raise ValueError("memory content must be 1..2000 characters")
        item = MemoryItem(uuid4(), device_id, value, tuple(tags), _now())
        async with self._lock:
            items = self._items.setdefault(device_id, [])
            items.append(item)
            del items[:-self._max_items]
        return item

    async def search(
        self, device_id: str, query: str, limit: int = 5
    ) -> tuple[MemoryItem, ...]:
        words = {word.casefold() for word in query.split() if word.strip()}
        for word in tuple(words):
            if len(word) >= 2:
                words.update(word[index : index + 2] for index in range(len(word) - 1))
        async with self._lock:
            items = list(self._items.get(device_id, ()))
        if not words:
            return tuple(items[-limit:][::-1])
        matches = [
            item for item in items
            if any(word in (item.content + " " + " ".join(item.tags)).casefold() for word in words)
        ]
        return tuple(matches[-limit:][::-1])

    async def forget(self, device_id: str, memory_id: UUID) -> bool:
        async with self._lock:
            items = self._items.get(device_id, [])
            before = len(items)
            self._items[device_id] = [item for item in items if item.memory_id != memory_id]
            return len(self._items[device_id]) != before

    async def close(self) -> None:
        return None
