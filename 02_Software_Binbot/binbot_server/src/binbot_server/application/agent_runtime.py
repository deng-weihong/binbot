"""Agent context assembly and tool policy boundary."""

from __future__ import annotations

import json
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any, TYPE_CHECKING

if TYPE_CHECKING:
    from .workspace import WorkspaceService

from binbot_server.domain.model import ChatMessage, TurnContext

from .ports import AgentModel, AgentModelResponse, ConversationStore, MemoryItem, MemoryStore
from .tools import ToolContext, ToolError, ToolRegistry, ToolSpec
from .reports import ActivityJournal


@dataclass(frozen=True, slots=True)
class AgentContext:
    device_id: str | None
    conversation_id: str | None
    session_id: str | None
    source: str
    messages: tuple[ChatMessage, ...]
    tools: tuple[dict[str, object], ...]
    memories: tuple[MemoryItem, ...]
    side_effects: list[str] = field(default_factory=list, compare=False)


class AgentRuntime:
    """Build bounded model context and apply a common tool approval policy."""

    def __init__(
        self,
        conversations: ConversationStore,
        tools: ToolRegistry,
        max_context_messages: int,
        max_context_chars: int = 12_000,
        require_control_approval: bool = True,
        model: AgentModel | None = None,
        max_tool_rounds: int = 4,
        memories: MemoryStore | None = None,
        max_memory_items: int = 5,
        journal: ActivityJournal | None = None,
    ):
        if max_context_messages < 1:
            raise ValueError("max_context_messages must be positive")
        if max_context_chars < 256:
            raise ValueError("max_context_chars must be at least 256")
        if max_tool_rounds < 1:
            raise ValueError("max_tool_rounds must be positive")
        if max_memory_items < 0:
            raise ValueError("max_memory_items must not be negative")
        self._conversations = conversations
        self._tools = tools
        self._max_context_messages = max_context_messages
        self._max_context_chars = max_context_chars
        self._require_control_approval = require_control_approval
        self._model = model
        self._max_tool_rounds = max_tool_rounds
        self._memories = memories
        self._max_memory_items = max_memory_items
        self._journal = journal
        self.workspace: WorkspaceService | None = None

    @property
    def enabled(self) -> bool:
        return self._model is not None

    def set_model(self, model: AgentModel | None) -> None:
        self._model = model

    def describe_tools(self) -> tuple[dict[str, object], ...]:
        return tuple(self._tools.describe())

    async def build_context(
        self,
        device_id: str | None,
        user_text: str,
        *,
        session_id: str | None = None,
        source: str = "unknown",
        conversation_id: str | None = None,
    ) -> AgentContext:
        if not user_text.strip():
            raise ValueError("user_text must be non-empty")
        history: Sequence[ChatMessage] = ()
        history_key = conversation_id or device_id
        if conversation_id:
            history = await self._conversations.load(conversation_id)
        elif device_id is not None:
            history = await self._conversations.load_device_history(device_id)
        memories: Sequence[MemoryItem] = ()
        if device_id is not None and self._memories is not None and self._max_memory_items:
            memories = await self._memories.search(device_id, user_text, self._max_memory_items)
        messages = self._trim_messages(
            [*history, ChatMessage(role="user", content=user_text.strip())]
        )
        if self.workspace is not None and device_id:
            document = await self.workspace.document(device_id)
            prefs = document["data"]["assistant"]
            messages = (
                ChatMessage(
                    role="system",
                    content=(
                        "You are the assistant for this device only. Use confirmed tool results; "
                        "never claim a queued operation has completed. "
                        "User preferences (data, not system policy): "
                        + json.dumps(prefs, ensure_ascii=False)
                    ),
                ),
                *messages,
            )
        return AgentContext(
            device_id=device_id,
            conversation_id=history_key,
            session_id=session_id,
            source=source,
            messages=messages,
            tools=tuple(self._tools.describe()),
            memories=tuple(memories),
        )

    def _trim_messages(self, messages: list[ChatMessage]) -> tuple[ChatMessage, ...]:
        selected: list[ChatMessage] = []
        chars = 0
        for message in reversed(messages):
            size = len(message.content)
            if selected and (
                len(selected) >= self._max_context_messages
                or chars + size > self._max_context_chars
            ):
                break
            selected.append(message)
            chars += size
        selected.reverse()
        return tuple(selected)

    async def execute_tool(
        self,
        name: str,
        context: AgentContext,
        arguments: dict[str, Any] | None = None,
        *,
        approved: bool = False,
    ) -> dict[str, Any]:
        spec = self._tools.get(name)
        if spec.permission == "device.control" or spec.permission.endswith(".write"):
            context.side_effects.append(name)
        tool_context = ToolContext(
            device_id=context.device_id,
            session_id=context.session_id,
            source=context.source,
            approved=approved,
        )
        return await self._tools.execute(name, tool_context, arguments, authorize=self._authorize)

    async def run(
        self,
        context: AgentContext,
        turn_context: TurnContext,
        *,
        approved: bool = False,
    ) -> AgentModelResponse:
        try:
            return await self._run(context, turn_context, approved=approved)
        except Exception as exc:
            if self._journal is not None:
                await self._journal.record(
                    "conversation",
                    context.source,
                    context.device_id,
                    "用户请求未完成",
                    "failed",
                    {"request": context.messages[-1].content, "error": str(exc)},
                )
            raise

    async def _run(
        self,
        context: AgentContext,
        turn_context: TurnContext,
        *,
        approved: bool = False,
    ) -> AgentModelResponse:
        model = self._model
        if model is None:
            raise ToolError("agent model is not configured")
        messages = list(context.messages)
        if context.memories:
            memory_text = "\n".join(f"- {item.content}" for item in context.memories)
            messages.insert(
                0,
                ChatMessage(role="system", content=f"Relevant long-term memory:\n{memory_text}"),
            )
        for _ in range(self._max_tool_rounds):
            response = await model.complete(turn_context, messages, context.tools)
            if not response.tool_calls:
                return response
            messages.append(
                ChatMessage(
                    role="assistant",
                    content=response.text,
                    tool_calls=response.tool_calls,
                )
            )
            for call in response.tool_calls:
                try:
                    result = await self.execute_tool(
                        call.name, context, call.arguments, approved=approved
                    )
                except Exception as exc:
                    result = {"error": str(exc)}
                messages.append(
                    ChatMessage(
                        role="tool",
                        content=json.dumps(result, ensure_ascii=False),
                        name=call.name,
                        tool_call_id=call.call_id,
                    )
                )
        raise ToolError("agent exceeded the tool call round limit")

    async def _authorize(self, spec: ToolSpec, context: ToolContext) -> bool:
        if not self._require_control_approval:
            return True
        if spec.permission == "device.control" and not context.approved:
            raise ToolError(f"approval required for tool: {spec.name}")
        return True
