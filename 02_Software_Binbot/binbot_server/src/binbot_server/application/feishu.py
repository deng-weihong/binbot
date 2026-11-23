"""Feishu channel mapping to the shared Agent runtime."""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Protocol
from uuid import uuid4

from binbot_server.domain.model import TurnContext

from .agent_runtime import AgentRuntime
from .ports import ConversationStore


_LOG = logging.getLogger("binbot.feishu")


class FeishuError(ValueError):
    """A malformed or unsupported Feishu callback."""


class FeishuMessenger(Protocol):
    async def send_text(self, receive_id: str, text: str, receive_id_type: str) -> None: ...

    async def close(self) -> None: ...


@dataclass(frozen=True, slots=True)
class FeishuMessage:
    event_id: str
    message_id: str
    chat_id: str
    sender_id: str
    chat_type: str
    text: str


def _string(value: object, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise FeishuError(f"{name} is required")
    return value.strip()


def parse_message(payload: Mapping[str, object]) -> FeishuMessage | None:
    header = payload.get("header")
    if not isinstance(header, Mapping):
        raise FeishuError("header is required")
    event_type = header.get("event_type")
    if event_type != "im.message.receive_v1":
        return None
    event_id = _string(header.get("event_id"), "header.event_id")
    event = payload.get("event")
    if not isinstance(event, Mapping):
        raise FeishuError("event is required")
    message = event.get("message")
    sender = event.get("sender")
    if not isinstance(message, Mapping) or not isinstance(sender, Mapping):
        raise FeishuError("message and sender are required")
    content = _string(message.get("content"), "event.message.content")
    if message.get("message_type") != "text":
        return None
    try:
        decoded = json.loads(content)
    except json.JSONDecodeError as exc:
        raise FeishuError("event.message.content is not valid JSON") from exc
    if not isinstance(decoded, Mapping):
        raise FeishuError("event.message.content must be an object")
    text = _string(decoded.get("text"), "event.message.content.text")
    sender_id = sender.get("sender_id")
    if not isinstance(sender_id, Mapping):
        raise FeishuError("event.sender.sender_id is required")
    sender_value = next(
        (sender_id.get(key) for key in ("open_id", "user_id", "union_id") if sender_id.get(key)),
        None,
    )
    return FeishuMessage(
        event_id=event_id,
        message_id=_string(message.get("message_id"), "event.message.message_id"),
        chat_id=_string(message.get("chat_id"), "event.message.chat_id"),
        sender_id=_string(sender_value, "event.sender.sender_id"),
        chat_type=str(message.get("chat_type") or "p2p"),
        text=text,
    )


class FeishuGateway:
    """Turns Feishu text callbacks into the same agent conversation flow."""

    def __init__(
        self,
        agent: AgentRuntime,
        conversations: ConversationStore,
        messenger: FeishuMessenger,
        receive_id_type: str = "chat_id",
        target_device_id: str = "",
        allow_control: bool = False,
        max_seen_events: int = 2_000,
        conversation_namespace: str = "",
    ):
        if receive_id_type not in {"chat_id", "open_id", "user_id", "union_id"}:
            raise ValueError("unsupported Feishu receive_id_type")
        if max_seen_events < 1:
            raise ValueError("max_seen_events must be positive")
        self._agent = agent
        self._conversations = conversations
        self._messenger = messenger
        self._receive_id_type = receive_id_type
        self._target_device_id = target_device_id.strip()
        self._allow_control = allow_control
        self._max_seen_events = max_seen_events
        self._conversation_namespace = conversation_namespace
        self._seen: dict[str, None] = {}
        self._lock = asyncio.Lock()

    async def _mark_seen(self, event_id: str) -> bool:
        async with self._lock:
            if event_id in self._seen:
                return False
            self._seen[event_id] = None
            while len(self._seen) > self._max_seen_events:
                self._seen.pop(next(iter(self._seen)))
            return True

    def _device_id(self, message: FeishuMessage) -> str:
        if self._target_device_id:
            return self._target_device_id
        if message.chat_type == "group":
            return f"feishu:chat:{message.chat_id}"
        return f"feishu:user:{message.sender_id}"

    def _conversation_id(self, message: FeishuMessage) -> str:
        prefix = f"feishu:device:{self._conversation_namespace}:" if self._conversation_namespace else "feishu:"
        if message.chat_type == "group":
            return f"{prefix}chat:{message.chat_id}"
        return f"{prefix}user:{message.sender_id}"

    def _receive_id(self, message: FeishuMessage) -> str:
        return message.chat_id if self._receive_id_type == "chat_id" else message.sender_id

    async def handle(self, payload: Mapping[str, object]) -> dict[str, object]:
        message = parse_message(payload)
        if message is None:
            return {"status": "ignored"}
        if not await self._mark_seen(message.event_id):
            return {"status": "duplicate", "event_id": message.event_id}
        device_id = self._device_id(message)
        _LOG.info(
            "Feishu message received event_id=%s chat_type=%s device_id=%s",
            message.event_id,
            message.chat_type,
            device_id,
        )
        session_id = f"feishu:{self._conversation_namespace}:{message.sender_id}"
        turn_context = TurnContext(uuid4(), uuid4(), device_id)
        context = await self._agent.build_context(
            device_id,
            message.text,
            session_id=session_id,
            source="feishu",
            conversation_id=self._conversation_id(message),
        )
        response = await self._agent.run(
            context, turn_context, approved=self._allow_control
        )
        text = response.text.strip()
        if not text:
            raise FeishuError("agent returned an empty response")
        await self._conversations.append_turn(
            context.conversation_id or device_id, message.text, text
        )
        await self._messenger.send_text(self._receive_id(message), text, self._receive_id_type)
        _LOG.info("Feishu response sent event_id=%s", message.event_id)
        return {"status": "ok", "event_id": message.event_id, "text": text}

    async def send_text(self, receive_id: str, text: str) -> None:
        await self._messenger.send_text(receive_id, text, self._receive_id_type)

    async def close(self) -> None:
        await self._messenger.close()
