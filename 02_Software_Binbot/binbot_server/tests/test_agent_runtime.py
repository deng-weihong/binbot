import asyncio

import pytest

from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.ports import AgentModelResponse
from binbot_server.application.tools import ToolContext, ToolError, ToolRegistry, ToolSpec
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.domain.model import ToolCall, TurnContext
from uuid import uuid4


def test_agent_runtime_trims_history_and_includes_tool_catalog() -> None:
    async def scenario() -> None:
        conversations = InMemoryConversationStore(max_messages=20)
        await conversations.append_turn("device-a", "旧问题", "旧回答")
        await conversations.append_turn("device-a", "最近问题", "最近回答")
        tools = ToolRegistry()

        async def execute(_: ToolContext, __: dict[str, object]) -> dict[str, object]:
            return {"ok": True}

        tools.register(
            ToolSpec("test.read", "Read data", {"type": "object"}, "device.read", execute)
        )
        runtime = AgentRuntime(conversations, tools, max_context_messages=3)
        context = await runtime.build_context("device-a", "当前问题", source="feishu")
        assert [message.content for message in context.messages] == [
            "最近问题", "最近回答", "当前问题"
        ]
        assert context.source == "feishu"
        assert context.tools[0]["name"] == "test.read"

    asyncio.run(scenario())


def test_agent_runtime_preserves_current_message_over_character_budget() -> None:
    async def scenario() -> None:
        conversations = InMemoryConversationStore(max_messages=20)
        await conversations.append_turn("device-a", "old", "old answer " * 40)
        runtime = AgentRuntime(
            conversations, ToolRegistry(), max_context_messages=8, max_context_chars=256
        )
        context = await runtime.build_context("device-a", "现在的问题")
        assert [message.content for message in context.messages] == ["现在的问题"]

    asyncio.run(scenario())


def test_agent_runtime_requires_approval_for_control_tools() -> None:
    async def scenario() -> None:
        tools = ToolRegistry()

        async def execute(_: ToolContext, __: dict[str, object]) -> dict[str, object]:
            return {"accepted": True}

        tools.register(
            ToolSpec(
                "test.control", "Control device", {"type": "object"},
                "device.control", execute,
            )
        )
        runtime = AgentRuntime(
            InMemoryConversationStore(max_messages=4), tools, max_context_messages=4
        )
        context = await runtime.build_context(None, "执行操作")
        with pytest.raises(ToolError, match="approval required"):
            await runtime.execute_tool("test.control", context)
        assert await runtime.execute_tool("test.control", context, approved=True) == {
            "accepted": True
        }

    asyncio.run(scenario())


def test_agent_runtime_executes_tool_calls_until_final_response() -> None:
    async def scenario() -> None:
        tools = ToolRegistry()
        observed: list[dict[str, object]] = []

        async def execute(_: ToolContext, args: dict[str, object]) -> dict[str, object]:
            observed.append(args)
            return {"value": 42}

        tools.register(
            ToolSpec(
                "test.lookup", "Lookup data", {"type": "object"},
                "device.read", execute,
            )
        )

        class Model:
            rounds = 0

            async def complete(
                self, _: TurnContext, messages: list[object], __: tuple[dict[str, object], ...]
            ) -> AgentModelResponse:
                self.rounds += 1
                if self.rounds == 1:
                    return AgentModelResponse(
                        tool_calls=(ToolCall("call-1", "test.lookup", {"key": "x"}),)
                    )
                assert getattr(messages[-1], "role") == "tool"
                return AgentModelResponse(text="查询完成")

        model = Model()
        runtime = AgentRuntime(
            InMemoryConversationStore(max_messages=4), tools, 4, model=model
        )
        context = await runtime.build_context(None, "查一下")
        result = await runtime.run(context, TurnContext(uuid4(), uuid4(), "device-a"))
        assert result.text == "查询完成"
        assert observed == [{"key": "x"}]
        assert model.rounds == 2

    asyncio.run(scenario())
