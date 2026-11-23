import asyncio
from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.tools import (
    MemoryService,
    ToolContext,
    ToolRegistry,
    register_device_tools,
)
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.memory import InMemoryMemoryStore
from binbot_server.application.tools import DeviceCommandService
from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry


def test_memory_store_searches_and_isolates_devices() -> None:
    async def scenario() -> None:
        store = InMemoryMemoryStore()
        item = await store.remember("device-a", "用户喜欢绿茶", ("偏好",))
        await store.remember("device-b", "用户喜欢咖啡")
        matches = await store.search("device-a", "绿茶")
        assert [match.memory_id for match in matches] == [item.memory_id]
        assert await store.search("device-b", "绿茶") == ()
        assert await store.forget("device-a", item.memory_id) is True
        assert await store.search("device-a", "绿茶") == ()

    asyncio.run(scenario())


def test_memory_tools_and_runtime_context() -> None:
    async def scenario() -> None:
        memories = InMemoryMemoryStore()
        service = MemoryService(memories)
        tools = ToolRegistry()
        register_device_tools(
            tools,
            DeviceCommandService(DeviceSessionRegistry(), DeviceControlStore()),
            service,
        )
        result = await tools.execute(
            "memory.remember",
            ToolContext(device_id="device-a", approved=True),
            {"content": "用户的纪念日是五月一日"},
        )
        assert result["content"] == "用户的纪念日是五月一日"
        runtime = AgentRuntime(
            InMemoryConversationStore(8), tools, 8, memories=memories
        )
        context = await runtime.build_context("device-a", "纪念日是什么")
        assert len(context.memories) == 1
        assert context.memories[0].content == "用户的纪念日是五月一日"
        memory_id = result["memory_id"]
        deleted = await tools.execute(
            "memory.forget", ToolContext(device_id="device-a"), {"memory_id": memory_id}
        )
        assert deleted == {"deleted": True}
        await memories.close()

    asyncio.run(scenario())
