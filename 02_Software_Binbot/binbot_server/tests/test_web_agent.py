import asyncio
from dataclasses import replace
from pathlib import Path

import pytest
from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer

from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.tools import ToolRegistry
from binbot_server.application.web import WebAgentGateway
from binbot_server.bootstrap import create_application
from binbot_server.config import Settings
from binbot_server.domain.model import legacy_web_conversation_prefix
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.sqlite import SqliteConversationStore
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore


def test_web_agent_gateway_preserves_conversation_history() -> None:
    async def scenario() -> None:
        conversations = InMemoryConversationStore(10)
        runtime = AgentRuntime(
            conversations,
            ToolRegistry(),
            10,
            model=_Model(),
            require_control_approval=False,
        )
        gateway = WebAgentGateway(runtime, conversations)
        first = await gateway.handle(
            "device-a", "第一条", conversation_id="web:one"
        )
        second = await gateway.handle(
            "device-a", "第二条", conversation_id="web:one"
        )
        assert first.text == "回复：第一条"
        assert second.text == "回复：第二条"
        assert [item.content for item in second.messages] == [
            "第一条",
            "回复：第一条",
            "第二条",
            "回复：第二条",
        ]
        assert await gateway.load("device-a", "web:one") == second.messages
        assert await gateway.load("device-b", "web:one") == ()
        assert await conversations.load("web:one") == ()

    asyncio.run(scenario())


class _Model:
    async def complete(self, context, messages, tools):
        del context, tools
        return type("Response", (), {"text": f"回复：{messages[-1].content}", "tool_calls": ()})()


@pytest.mark.parametrize("persistent", [False, True])
def test_device_history_combines_existing_turns_and_shares_new_text(
    tmp_path: Path, persistent: bool
) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "chat.sqlite3")
        if persistent:
            conversations = SqliteConversationStore(path, 8)
        else:
            conversations = InMemoryConversationStore(8)
        requests = SqliteWorkspaceStore(path)
        device = "desk_1"
        legacy = legacy_web_conversation_prefix(device) + "web:browser:one"
        await conversations.append_turn(device, "先用语音提问", "第一次答复")
        await conversations.append_turn(legacy, "旧的文字提问", "文字答复")
        await conversations.append_turn(
            legacy_web_conversation_prefix("deskX1") + "web:browser:one",
            "另一台设备的文字",
            "另一台设备的答复",
        )
        await conversations.append_turn(device, "接着用语音提问", "第二次答复")
        await conversations.append_turn("feishu:desk_1", "飞书消息", "飞书答复")

        if persistent:
            await conversations.close()
            conversations = SqliteConversationStore(path, 8)
        runtime = AgentRuntime(
            conversations, ToolRegistry(), 8, model=_Model(), require_control_approval=False
        )
        gateway = WebAgentGateway(runtime, conversations, requests)
        previous = await conversations.load_device_history(device)
        assert [item.content for item in previous] == [
            "先用语音提问", "第一次答复",
            "旧的文字提问", "文字答复",
            "接着用语音提问", "第二次答复",
        ]
        assert len(await gateway.load(device, "web:browser:one")) == 2

        first = await gateway.handle(device, "继续刚才的话题", request_id="send-1")
        retry = await gateway.handle(device, "继续刚才的话题", request_id="send-1")
        assert retry.messages == first.messages == await conversations.load_device_history(device)
        assert first.messages[:6] == previous
        assert first.messages[-1].content == "回复：继续刚才的话题"
        assert (await conversations.load(device))[-2:] == first.messages[-2:]
        voice = await runtime.build_context(device, "继续说", source="voice")
        assert voice.messages[:-1] == first.messages[1:]
        assert voice.messages[-1].content == "继续说"

        await conversations.append_turn(device, "之后再聊", "第三次答复")
        assert await conversations.load_device_history(device) == (
            *first.messages[2:], *(await conversations.load(device))[-2:]
        )
        await conversations.close()
        await requests.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_web_agent_http_routes_share_admin_session(monkeypatch: pytest.MonkeyPatch) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            admin=replace(
                defaults.admin,
                allow_unauthenticated_loopback=False,
                token_env="BINBOT_TEST_WEB_ADMIN_TOKEN",
            ),
        )
        monkeypatch.setenv("BINBOT_TEST_WEB_ADMIN_TOKEN", "web-test-secret")
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                async with client.get(server.make_url("/api/v1/agent/tools")) as response:
                    assert response.status == 401
                device_chat = server.make_url("/api/v1/admin/devices/web-device/chat")
                async with client.get(device_chat) as response:
                    assert response.status == 401
                async with client.post(device_chat, json={"text": "question"}) as response:
                    assert response.status == 401
                async with client.post(
                    server.make_url("/api/v1/admin/session"),
                    json={"token": "web-test-secret"},
                ) as response:
                    assert response.status == 200
                async with client.get(server.make_url("/api/v1/agent/tools")) as response:
                    assert response.status == 200
                    tools = await response.json()
                    assert "device.list" in [item["name"] for item in tools["tools"]]
                async with client.post(
                    server.make_url("/api/v1/agent/chat"),
                    json={
                        "device_id": "web-device",
                        "conversation_id": "web:test",
                        "text": "你好",
                    },
                ) as response:
                    assert response.status == 200
                    payload = await response.json()
                    assert payload["conversation_id"] == "web:test"
                    assert payload["messages"][-1]["role"] == "assistant"
                async with client.get(
                    server.make_url(
                        "/api/v1/agent/conversations/web:test?device_id=web-device"
                    )
                ) as response:
                    assert response.status == 200
                    history = await response.json()
                    assert history["messages"] == payload["messages"]
                async with client.get(
                    server.make_url(
                        "/api/v1/agent/conversations?device_id=web-device"
                    )
                ) as response:
                    assert response.status == 200
                    conversations = await response.json()
                    assert conversations["conversations"][0]["conversation_id"] == "web:test"
                await application.conversations.append_turn("web-device", "语音问题", "语音答复")
                async with client.get(device_chat) as response:
                    assert response.status == 200
                    assert response.headers["Cache-Control"] == "no-store"
                    chat = await response.json()
                    assert chat["device_id"] == "web-device"
                    assert [item["content"] for item in chat["messages"]][::2] == ["你好", "语音问题"]
                async with client.post(
                    device_chat, json={"text": "接着聊", "request_id": "shared-turn"}
                ) as response:
                    assert response.status == 200
                    chat = await response.json()
                    assert [item["content"] for item in chat["messages"]][::2] == [
                        "你好", "语音问题", "接着聊"
                    ]
                async with client.get(
                    server.make_url("/api/v1/admin/devices/another-device/chat")
                ) as response:
                    assert (await response.json())["messages"] == []
                async with client.post(
                    server.make_url("/api/v1/agent/conversations"),
                    json={"device_id": "web-device"},
                ) as response:
                    assert response.status == 201
                    created = await response.json()
                    assert created["conversation_id"].startswith("web:browser:")
                async with client.get(
                    server.make_url("/api/v1/agent/memories?device_id=web-device")
                ) as response:
                    assert response.status == 200
                    assert (await response.json())["memories"] == []
                async with client.get(
                    server.make_url("/api/v1/agent/tasks?device_id=web-device")
                ) as response:
                    assert response.status == 200
                    assert (await response.json())["tasks"] == []
        finally:
            await server.close()

    asyncio.run(scenario())
