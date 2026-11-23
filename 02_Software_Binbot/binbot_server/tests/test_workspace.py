import asyncio
from datetime import UTC, datetime, timedelta

import pytest

from binbot_server.application.workspace import (
    default_workspace,
    schedule_review,
    empty_review,
    WorkspaceService,
)
from binbot_server.application.web import WebAgentGateway
from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.tools import ToolRegistry
from binbot_server.domain.errors import StateConflict
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.infrastructure.conversation import InMemoryConversationStore


def test_workspace_versions_idempotency_and_device_isolation_survive_restart(tmp_path):
    async def run():
        path = str(tmp_path / "workspace.sqlite3")
        store = SqliteWorkspaceStore(path)
        a = await store.read("a", default_workspace("a"))
        b = await store.read("b", default_workspace("b"))
        change = lambda data: {**data, "deviceName": "书房"}
        updated = await store.update(
            "a", a["revision"], change, event_id="one", event_payload={"name": "书房"}
        )
        duplicate = await store.update(
            "a", a["revision"], change, event_id="one", event_payload={"name": "书房"}
        )
        assert duplicate["revision"] == updated["revision"]
        assert (await store.read("b", {}))["data"] == b["data"]
        with pytest.raises(StateConflict):
            await store.update("a", a["revision"], change)
        with pytest.raises(StateConflict):
            await store.update("a", "", change, event_id="one", event_payload={"name": "another"})
        await store.close()
        restored = SqliteWorkspaceStore(path)
        assert (await restored.read("a", {}))["data"]["deviceName"] == "书房"
        await restored.close()

    asyncio.run(run())


def test_review_due_boundaries_early_practice_lapses_and_late_uploads():
    now = datetime(2026, 11, 18, tzinfo=UTC)
    first = schedule_review(empty_review(), True, now)
    assert first["stage"] == 1
    assert datetime.fromisoformat(first["nextReviewAt"]) == now + timedelta(minutes=20)
    early = schedule_review(first, True, now + timedelta(minutes=10))
    assert early["stage"] == 1 and early["nextReviewAt"] == first["nextReviewAt"]
    due = schedule_review(early, True, now + timedelta(minutes=20))
    assert due["stage"] == 2
    missed = schedule_review(due, False, now + timedelta(hours=2))
    assert missed["stage"] == 0 and missed["lapseCount"] == 1
    late = schedule_review(missed, True, now + timedelta(minutes=1))
    assert late["nextReviewAt"] == missed["nextReviewAt"]
    assert late["stage"] == missed["stage"]
    assert late["reviewCount"] == missed["reviewCount"] + 1


def test_wordbook_content_updates_cannot_forge_review_statistics():
    old = default_workspace("a")["wordbooks"]
    import copy

    changed = copy.deepcopy(old)
    changed["books"][0]["words"][0]["review"]["reviewCount"] = 100
    with pytest.raises(ValueError):
        WorkspaceService.validate_wordbooks(old, changed)
    changed["books"][0]["words"][0]["review"]["reviewCount"] = 1
    changed["books"][0]["words"][0]["remembered"] = True
    result = WorkspaceService.validate_wordbooks(old, changed)
    review = result["books"][0]["words"][0]["review"]
    assert review["reviewCount"] == 1 and review["stage"] == 1
    assert old["books"][0]["words"][0]["review"]["reviewCount"] == 0


def test_chat_request_replays_confirmed_answer_without_calling_model_twice(tmp_path):
    async def run():
        class Model:
            calls = 0

            async def complete(self, _context, _messages, _tools):
                self.calls += 1
                return type("Reply", (), {"text": "真实模型回复", "tool_calls": ()})()

        store = SqliteWorkspaceStore(str(tmp_path / "requests.sqlite3"))
        conversations = InMemoryConversationStore(20)
        model = Model()
        gateway = WebAgentGateway(
            AgentRuntime(conversations, ToolRegistry(), 20, model=model), conversations, store
        )
        first = await gateway.handle("a", "你好", conversation_id="one", request_id="request-a")
        second = await gateway.handle("a", "你好", conversation_id="one", request_id="request-a")
        assert first.text == second.text
        assert model.calls == 1 and len(second.messages) == 2
        with pytest.raises(StateConflict):
            await gateway.handle("a", "不同问题", conversation_id="one", request_id="request-a")
        await store.close()

    asyncio.run(run())


def test_an_interrupted_chat_cannot_replay_tools_after_restart(tmp_path):
    async def run():
        path = str(tmp_path / "requests.sqlite3")
        store = SqliteWorkspaceStore(path)
        assert await store.chat_claim("web:a", "request", "打开设备") is None
        await store.close()
        restored = SqliteWorkspaceStore(path)
        with pytest.raises(StateConflict):
            await restored.chat_claim("web:a", "request", "打开设备")
        await restored.close()

    asyncio.run(run())


def test_cancelling_a_write_keeps_the_connection_locked_until_commit(tmp_path):
    async def run():
        import threading

        started, release = threading.Event(), threading.Event()
        store = SqliteWorkspaceStore(str(tmp_path / "cancel.sqlite"))

        def write(db):
            started.set()
            assert release.wait(2)
            db.execute(
                "INSERT INTO workspace_gallery VALUES (?, ?, ?)", ("one", "kept", '{"id":"kept"}')
            )

        writing = asyncio.create_task(store._transaction(write))
        assert await asyncio.to_thread(started.wait, 1)
        writing.cancel()
        reading = asyncio.create_task(store.gallery("one"))
        await asyncio.sleep(0.01)
        assert not reading.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await writing
        assert (await reading)[0]["id"] == "kept"
        await store.close()

    asyncio.run(run())
