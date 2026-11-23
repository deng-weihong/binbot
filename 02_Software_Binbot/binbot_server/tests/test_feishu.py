import asyncio
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

import pytest

from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.feishu import FeishuGateway, parse_message
from binbot_server.application.ports import AgentModelResponse
from binbot_server.domain.model import TurnContext
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.feishu import (
    FeishuClientError, FeishuOAuthManager, FeishuTokenStore, HttpFeishuClient,
)
from binbot_server.application.calendar import CalendarEventDraft
from binbot_server.application.tools import ToolRegistry


def _event(event_id: str = "evt-1") -> dict[str, object]:
    return {
        "header": {"event_id": event_id, "event_type": "im.message.receive_v1"},
        "event": {
            "sender": {"sender_id": {"open_id": "ou_user"}},
            "message": {
                "message_id": "om_msg",
                "chat_id": "oc_chat",
                "chat_type": "p2p",
                "message_type": "text",
                "content": '{"text":"播放音乐"}',
            },
        },
    }


def test_parse_feishu_text_message() -> None:
    message = parse_message(_event())
    assert message is not None
    assert message.sender_id == "ou_user"
    assert message.text == "播放音乐"


def test_group_messages_use_chat_scoped_conversation() -> None:
    payload = _event("evt-group")
    event = payload["event"]
    assert isinstance(event, dict)
    message = event["message"]
    assert isinstance(message, dict)
    message["chat_type"] = "group"
    parsed = parse_message(payload)
    assert parsed is not None
    assert parsed.chat_type == "group"


def test_feishu_gateway_calls_agent_and_deduplicates() -> None:
    async def scenario() -> None:
        class Model:
            async def complete(
                self, _: TurnContext, __: object, ___: object
            ) -> AgentModelResponse:
                return AgentModelResponse(text="已收到")

        class Messenger:
            sent: list[tuple[str, str, str]] = []

            async def send_text(self, receive_id: str, text: str, receive_id_type: str) -> None:
                self.sent.append((receive_id, text, receive_id_type))

            async def close(self) -> None:
                return None

        messenger = Messenger()
        conversations = InMemoryConversationStore(10)
        runtime = AgentRuntime(
            conversations, ToolRegistry(), 10, model=Model(), require_control_approval=False
        )
        gateway = FeishuGateway(runtime, conversations, messenger)
        first = await gateway.handle(_event())
        second = await gateway.handle(_event())
        assert first["status"] == "ok"
        assert second["status"] == "duplicate"
        assert messenger.sent == [("oc_chat", "已收到", "chat_id")]
        history = await conversations.load("feishu:user:ou_user")
        assert [message.content for message in history] == ["播放音乐", "已收到"]
        await gateway.close()

    asyncio.run(scenario())


def test_calendar_requests_use_user_grant_and_messages_keep_tenant_auth() -> None:
    pytest.importorskip("lark_oapi")
    from lark_oapi import RequestOption
    from lark_oapi.api.calendar.v4 import (
        ListCalendarEventRequest,
        CreateCalendarEventRequest,
        InstanceViewCalendarEventRequest,
        PrimaryCalendarRequest,
        SubscriptionCalendarEventRequest,
    )
    from lark_oapi.api.im.v1 import CreateMessageRequest
    from lark_oapi.core.model import AccessTokenType
    from lark_oapi.core.token.auth import verify
    from lark_oapi.core.token.manager import TokenManager

    client = HttpFeishuClient(
        "https://open.feishu.cn", "test-app", "test-secret",
        user_access_token="test-user-token",
    )
    config = client._sdk()._config
    requests = (
        PrimaryCalendarRequest.builder().build(),
        ListCalendarEventRequest.builder().calendar_id("test-calendar").build(),
        SubscriptionCalendarEventRequest.builder().calendar_id("test-calendar").build(),
        CreateCalendarEventRequest.builder().calendar_id("test-calendar").build(),
        InstanceViewCalendarEventRequest.builder().calendar_id("test-calendar").build(),
    )
    with patch.object(
        TokenManager, "get_self_tenant_token", return_value="test-tenant-token"
    ) as tenant:
        for request in requests:
            option = client._user_option()
            verify(config, request, option)
            assert request.token_types == {AccessTokenType.USER}
            assert option.user_access_token == "test-user-token"
        tenant.assert_not_called()

        message = CreateMessageRequest.builder().build()
        option = RequestOption.builder().build()
        verify(config, message, option)
        assert message.token_types == {AccessTokenType.TENANT}
        assert option.tenant_access_token == "test-tenant-token"


@pytest.mark.parametrize("all_day", [True, False])
def test_feishu_calendar_create_and_instance_requests(all_day: bool) -> None:
    pytest.importorskip("lark_oapi")

    async def scenario() -> None:
        client = HttpFeishuClient("https://open.feishu.cn", "app", "secret", user_access_token="user-token")
        draft = CalendarEventDraft.from_payload({
            "summary": "会议", "all_day": all_day, "location": "A101", "description": "备注",
            "start_time": "2026-10-19" if all_day else "2026-10-19T10:00:00+08:00",
            "end_time": "2026-10-20" if all_day else "2026-10-19T11:00:00+08:00",
        })
        item = SimpleNamespace(event_id="created", summary=draft.summary, description="备注",
                               start_time=SimpleNamespace(date=draft.start_date, timestamp=draft.start_timestamp),
                               end_time=SimpleNamespace(date=draft.end_date, timestamp=draft.end_timestamp))
        resource = SimpleNamespace(
            acreate=AsyncMock(return_value=SimpleNamespace(success=lambda: True, data=SimpleNamespace(event=item))),
            ainstance_view=AsyncMock(return_value=SimpleNamespace(success=lambda: True, data=SimpleNamespace(items=[item]))),
        )
        with patch.object(client, "_sdk", return_value=SimpleNamespace(calendar=SimpleNamespace(v4=SimpleNamespace(calendar_event=resource)))):
            event = await client.create_calendar_event("calendar", draft, "stable-key")
            assert event.event_id == "created" and event.description == "备注"
            request, option = resource.acreate.await_args.args
            assert option.user_access_token == "user-token"
            assert request.idempotency_key == "stable-key" and request.calendar_id == "calendar"
            body = request.request_body
            assert body.location.name == "A101" and body.description == "备注"
            if all_day:
                assert body.start_time.date == "2026-10-19" and body.end_time.date == "2026-10-20"
                assert body.start_time.timestamp is None
            else:
                assert body.start_time.timestamp == draft.start_timestamp
                assert body.end_time.timestamp == draft.end_timestamp
                assert body.start_time.timezone == "Asia/Shanghai" and body.start_time.date is None
            assert await client.calendar_instances("calendar", "100", "200") == [event]
            request, option = resource.ainstance_view.await_args.args
            assert request.start_time == "100" and request.end_time == "200"
            assert option.user_access_token == "user-token"
            resource.acreate.return_value = SimpleNamespace(success=lambda: False, data=None, code=99991672, msg="permission denied")
            with pytest.raises(FeishuClientError, match="permission denied"):
                await client.create_calendar_event("calendar", draft, "stable-key")
        await client.close()

    asyncio.run(scenario())


def test_calendar_grant_refreshes_during_runtime_and_serializes_requests(tmp_path: Path) -> None:
    async def scenario() -> None:
        store = FeishuTokenStore(str(tmp_path / "grant.json"))
        store.save({"access_token": "expired", "refresh_token": "refresh", "expires_at": "1"})
        client = AsyncMock()
        client.refresh_user_token.return_value = {
            "access_token": "fresh", "refresh_token": "rotated", "expires_in": 7200,
        }
        apply = AsyncMock()
        manager = FeishuOAuthManager(client, store, "app", ("calendar:calendar:readonly",), apply)
        assert all(await asyncio.gather(*(manager.ensure_fresh_token() for _ in range(8))))
        client.refresh_user_token.assert_awaited_once_with("refresh")
        apply.assert_awaited_once_with("fresh")
        assert store.load()["refresh_token"] == "rotated"
        assert int(store.load()["expires_at"]) > time.time()
        await manager.disconnect()
        assert store.load() == {} and await manager.ensure_fresh_token() is False
        store.save({"access_token": "expired", "expires_at": "1"})
        with pytest.raises(FeishuClientError, match="重新连接"):
            await manager.ensure_fresh_token()
        assert await manager.restore() is False

    asyncio.run(scenario())
