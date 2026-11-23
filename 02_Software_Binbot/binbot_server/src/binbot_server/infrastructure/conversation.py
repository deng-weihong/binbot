from __future__ import annotations

import asyncio
from collections.abc import Sequence
from datetime import UTC, datetime

from binbot_server.application.ports import ConversationSummary
from binbot_server.domain.model import ChatMessage, legacy_web_conversation_prefix


class InMemoryConversationStore:
    """Process-local bounded history, isolated by device identifier."""

    def __init__(self, max_messages: int):
        if max_messages < 0:
            raise ValueError("max_messages must not be negative")
        self._max_messages = max_messages
        self._messages: dict[str, list[tuple[int, ChatMessage]]] = {}
        self._sequence = 0
        self._updated_at: dict[str, str] = {}
        self._lock = asyncio.Lock()

    async def load(self, device_id: str) -> Sequence[ChatMessage]:
        async with self._lock:
            return tuple(message for _, message in self._messages.get(device_id, ()))

    async def load_device_history(self, device_id: str) -> Sequence[ChatMessage]:
        prefix = legacy_web_conversation_prefix(device_id)
        async with self._lock:
            entries = sorted(
                (
                    entry
                    for key, messages in self._messages.items()
                    if key == device_id or key.startswith(prefix)
                    for entry in messages
                ),
                key=lambda item: item[0],
                reverse=True,
            )[: self._max_messages]
            return tuple(message for _, message in reversed(entries))

    async def append_turn(
        self, device_id: str, user_text: str, assistant_text: str, *, source: str = ""
    ) -> Sequence[ChatMessage]:
        async with self._lock:
            history = self._messages.setdefault(device_id, [])
            history.extend(
                (
                    (self._sequence, ChatMessage(role="user", content=user_text)),
                    (self._sequence + 1, ChatMessage(role="assistant", content=assistant_text)),
                )
            )
            self._sequence += 2
            if self._max_messages == 0:
                history.clear()
            elif len(history) > self._max_messages:
                del history[: len(history) - self._max_messages]
            self._updated_at[device_id] = datetime.now(UTC).isoformat()
            return tuple(message for _, message in history)

    async def list_conversations(
        self, prefix: str = "", limit: int = 50
    ) -> Sequence[ConversationSummary]:
        async with self._lock:
            entries = [
                ConversationSummary(key, len(messages), self._updated_at.get(key, ""))
                for key, messages in self._messages.items()
                if key.startswith(prefix) and messages
            ]
            entries.sort(key=lambda item: item.updated_at, reverse=True)
            return tuple(entries[: max(1, min(limit, 100))])

    async def close(self) -> None:
        """Match persistent stores so application cleanup is uniform."""
