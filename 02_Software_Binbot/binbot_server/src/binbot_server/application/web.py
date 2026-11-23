"""Web channel adapter for the shared Agent runtime."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Any, Protocol
from uuid import uuid4

from binbot_server.domain.model import (
    ChatMessage,
    TurnContext,
    legacy_web_conversation_prefix,
)

from .agent_runtime import AgentRuntime
from .ports import ConversationStore


class ChatRequestStore(Protocol):
    async def chat_claim(self, key: str, request_id: str, text: str) -> dict[str, Any] | None: ...

    async def chat_finish(
        self, key: str, request_id: str, response: dict[str, Any], status: str = "completed"
    ) -> None: ...


class WebAgentError(ValueError):
    """A malformed Web Agent request."""


@dataclass(frozen=True, slots=True)
class WebAgentReply:
    device_id: str
    conversation_id: str | None
    session_id: str
    text: str
    messages: tuple[ChatMessage, ...]


@dataclass(frozen=True, slots=True)
class WebConversationSummary:
    conversation_id: str
    message_count: int
    updated_at: str


class WebAgentGateway:
    """Map browser turns to AgentRuntime while serializing each conversation."""

    def __init__(
        self,
        agent: AgentRuntime,
        conversations: ConversationStore,
        requests: ChatRequestStore | None = None,
    ):
        self._agent = agent
        self._conversations = conversations
        self._requests = requests
        self._locks: dict[str, asyncio.Lock] = {}
        self._lock = asyncio.Lock()

    async def _conversation_lock(self, conversation_id: str) -> asyncio.Lock:
        async with self._lock:
            return self._locks.setdefault(conversation_id, asyncio.Lock())

    @staticmethod
    def _key(device_id: str, conversation_id: str) -> str:
        if not device_id.strip():
            raise WebAgentError("device_id is required")
        if not conversation_id.strip() or len(conversation_id) > 256:
            raise WebAgentError("conversation_id must be 1..256 characters")
        return legacy_web_conversation_prefix(device_id) + conversation_id

    @staticmethod
    def _prefix(device_id: str) -> str:
        device_id = device_id.strip()
        if not device_id:
            raise WebAgentError("device_id is required")
        return legacy_web_conversation_prefix(device_id)

    async def create(self, device_id: str) -> str:
        self._prefix(device_id)
        return f"web:browser:{uuid4().hex}"

    async def list(self, device_id: str, limit: int = 50) -> tuple[WebConversationSummary, ...]:
        prefix = self._prefix(device_id)
        entries = await self._conversations.list_conversations(prefix, limit)
        return tuple(
            WebConversationSummary(
                item.conversation_key[len(prefix) :], item.message_count, item.updated_at
            )
            for item in entries
        )

    async def handle(
        self,
        device_id: str,
        text: str,
        *,
        conversation_id: str | None = None,
        session_id: str = "",
        approved: bool = False,
        request_id: str = "",
    ) -> WebAgentReply:
        device_id = device_id.strip()
        text = text.strip()
        if not device_id:
            raise WebAgentError("device_id is required")
        if not text:
            raise WebAgentError("text is required")
        if len(text) > 4_000:
            raise WebAgentError("text must be at most 4000 characters")
        if conversation_id is None:
            key = device_id
            load_history = self._conversations.load_device_history
        else:
            conversation_id = conversation_id.strip()
            key = self._key(device_id, conversation_id)
            load_history = self._conversations.load
        session_id = session_id.strip() or f"web:{conversation_id or device_id}"
        conversation_lock = await self._conversation_lock(key)
        async with conversation_lock:
            if request_id and self._requests is not None:
                cached = await self._requests.chat_claim(key, request_id, text)
                if cached is not None:
                    return WebAgentReply(
                        device_id,
                        conversation_id,
                        str(cached["session_id"]),
                        str(cached["text"]),
                        tuple(await load_history(key)),
                    )
            context = None
            try:
                context = await self._agent.build_context(
                    device_id,
                    text,
                    session_id=session_id,
                    source="web",
                    conversation_id=key if conversation_id is not None else None,
                )
                response = await self._agent.run(
                    context, TurnContext(uuid4(), uuid4(), device_id), approved=approved
                )
            except BaseException:
                if request_id and self._requests is not None:
                    status = (
                        "uncertain" if context is not None and context.side_effects else "retryable"
                    )
                    await self._requests.chat_finish(key, request_id, {}, status)
                raise
            answer = response.text.strip()
            if not answer:
                if request_id and self._requests is not None:
                    await self._requests.chat_finish(
                        key, request_id, {}, "uncertain" if context.side_effects else "retryable"
                    )
                raise WebAgentError("agent returned an empty response")
            messages = await self._conversations.append_turn(key, text, answer, source="web")
            if request_id and self._requests is not None:
                await self._requests.chat_finish(
                    key, request_id, {"text": answer, "session_id": session_id}
                )
            if conversation_id is None:
                messages = await load_history(key)
            return WebAgentReply(
                device_id=device_id,
                conversation_id=conversation_id,
                session_id=session_id,
                text=answer,
                messages=tuple(messages),
            )

    async def load(self, device_id: str, conversation_id: str) -> tuple[ChatMessage, ...]:
        return tuple(await self._conversations.load(self._key(device_id, conversation_id)))
