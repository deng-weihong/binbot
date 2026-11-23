import asyncio
import json
from dataclasses import replace
from datetime import UTC, datetime, time, timedelta
from uuid import uuid4
from zoneinfo import ZoneInfo

import pytest
from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer

from binbot_server.application.reports import (
    Activity,
    ActivityJournal,
    DailyReportService,
    JournalConversationStore,
)
from binbot_server.application.tools import ToolContext, ToolError, ToolRegistry, ToolSpec
from binbot_server.bootstrap import create_application
from binbot_server.config import Settings, validate_settings
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.reports import InMemoryReportStore, SqliteReportStore

TZ = ZoneInfo("Asia/Shanghai")


def _midnight() -> datetime:
    return datetime.combine(datetime.now(TZ).date(), time.min, TZ)


def _activity(at: datetime, status: str = "completed", text: str = "查询天气") -> Activity:
    return Activity(
        uuid4().hex,
        at.astimezone(UTC).isoformat(),
        "tool",
        "web",
        "web:console",
        text,
        status,
        {"result": text},
    )


class ReportModel:
    def __init__(self, result=None, error=False):
        self.calls = []
        self.result = result or {
            "summary": "我查询了天气。",
            "completed": ["查询天气"],
            "problems": [],
            "pending": [],
        }
        self.error = error

    async def stream(self, context, messages):
        self.calls.append((context, messages))
        if self.error:
            raise RuntimeError("model unavailable")
        yield json.dumps(self.result, ensure_ascii=False)


def test_report_uses_local_day_and_records_failed_and_submitted_outcomes():
    async def scenario():
        store = InMemoryReportStore()
        model = ReportModel()
        service = DailyReportService(store, model)
        midnight = _midnight()
        await store.append(_activity(midnight - timedelta(seconds=1), text="昨天"))
        await store.append(_activity(midnight, text="今天"))
        await store.append(_activity(midnight + timedelta(hours=1), "failed", "离线命令"))
        await store.append(_activity(midnight + timedelta(hours=2), "submitted", "音乐播放"))
        await store.append(_activity(midnight + timedelta(days=1), text="明天"))
        report = await service.generate(
            midnight.date().isoformat(), now=midnight + timedelta(hours=23)
        )
        assert report["activity_count"] == 3
        assert report["counts"] == {"completed": 1, "failed": 1, "submitted": 1}
        assert report["final"] is False
        evidence = model.calls[0][1][-1].content
        assert "昨天" not in evidence and "明天" not in evidence
        assert "离线命令" in evidence and "音乐播放" in evidence
        assert report["mode"] == "model"

    asyncio.run(scenario())


def test_journal_survives_conversation_trimming_and_sqlite_restart(tmp_path):
    async def scenario():
        path = str(tmp_path / "binbot.sqlite3")
        store = SqliteReportStore(path)
        started = await store.started_at()
        conversations = JournalConversationStore(
            InMemoryConversationStore(2), ActivityJournal(store)
        )
        await conversations.append_turn("web:context", "第一件事", "查找资料")
        await conversations.append_turn("web:context", "第二件事", "整理结果")
        assert len(await conversations.load("web:context")) == 2
        service = DailyReportService(store)
        report = await service.generate(service.today())
        await store.close()
        restored = SqliteReportStore(path)
        assert await restored.started_at() == started
        assert (await restored.get_report(service.today())) == report
        start, end = service.bounds(service.today())
        batch = await restored.activities(start, end)
        assert batch.total == 2
        assert batch.items[0].details["request"] == "第一件事"
        assert batch.items[1].details["response"] == "整理结果"
        await restored.close()

    asyncio.run(scenario())


def test_tools_record_errors_and_redact_structured_secrets():
    async def scenario():
        store = InMemoryReportStore()
        tools = ToolRegistry(ActivityJournal(store))

        async def handle(_context, _arguments):
            return {"result": "查询完成", "api_key": "sensitive-key"}

        tools.register(ToolSpec("test.read", "查询资料", {}, "device.read", handle))
        await tools.execute(
            "test.read", ToolContext(source="web"), {"password": "sensitive-password"}
        )
        with pytest.raises(ToolError):
            await tools.execute("test.unknown", ToolContext(source="feishu"))
        start, end = DailyReportService(store).bounds(DailyReportService(store).today())
        batch = await store.activities(start, end)
        assert batch.counts == {"completed": 1, "failed": 1}
        encoded = json.dumps([item.snapshot() for item in batch.items])
        assert "sensitive-key" not in encoded and "sensitive-password" not in encoded
        assert "[redacted]" in encoded

    asyncio.run(scenario())


@pytest.mark.parametrize("early", [True, False])
def test_device_receipt_resolves_submission_and_duplicates_are_ignored(early):
    async def scenario():
        store = InMemoryReportStore()
        journal = ActivityJournal(store)
        if early:
            await journal.command_result("device-a", "command-a", True, "播放成功")
        await journal.record(
            "tool",
            "web",
            "device-a",
            "播放音乐",
            "submitted",
            {"result": {"status": "delivered"}},
            "command-a",
        )
        if not early:
            await journal.command_result("device-a", "command-a", True, "播放成功")
        await journal.command_result("device-a", "command-a", True, "播放成功")
        service = DailyReportService(store)
        report = await service.generate(service.today())
        assert report["activity_count"] == 2
        assert report["content"]["pending"] == []
        assert "播放成功" in report["content"]["completed"][0]

    asyncio.run(scenario())


def test_worker_finalizes_late_activity_and_is_idempotent_after_restart(tmp_path):
    async def scenario():
        store = SqliteReportStore(str(tmp_path / "reports.sqlite3"))
        model = ReportModel()
        service = DailyReportService(store, model)
        midnight = _midnight()
        await store.append(_activity(midnight + timedelta(hours=12)))
        await service.run_due_once(midnight + timedelta(hours=22))
        assert await store.get_report(midnight.date().isoformat()) is None
        await service.run_due_once(midnight + timedelta(hours=23, minutes=1))
        assert len(model.calls) == 1
        await service.run_due_once(midnight + timedelta(hours=23, minutes=2))
        assert len(model.calls) == 1
        await store.append(_activity(midnight + timedelta(hours=23, minutes=30), text="晚间工作"))
        await service.run_due_once(midnight + timedelta(days=1, minutes=1))
        report = await store.get_report(midnight.date().isoformat())
        assert report["final"] is True and report["activity_count"] == 2
        assert "晚间工作" in model.calls[-1][1][-1].content
        await store.close()
        restored = SqliteReportStore(str(tmp_path / "reports.sqlite3"))
        await DailyReportService(restored, model).run_due_once(
            midnight + timedelta(days=1, minutes=2)
        )
        assert len(model.calls) == 2
        assert len(await restored.list_reports()) == 1
        await restored.close()

    asyncio.run(scenario())


def test_missing_model_no_activity_and_model_failure_do_not_invent_work():
    async def scenario():
        store = InMemoryReportStore()
        model = ReportModel(error=True)
        service = DailyReportService(store, model)
        empty = await service.generate(service.today())
        assert empty["mode"] == "empty"
        assert empty["content"]["completed"] == []
        assert model.calls == []
        await ActivityJournal(store).record(
            "tool", "web", None, "播放音乐", "failed", {"error": "设备离线"}
        )
        fallback = await service.generate(service.today())
        assert fallback["mode"] == "fallback"
        assert "设备离线" in fallback["content"]["problems"][0]
        assert fallback["content"]["completed"] == []

    asyncio.run(scenario())


def test_worker_catchup_respects_short_retention():
    class EarlierStore(InMemoryReportStore):
        async def started_at(self):
            return (_midnight() - timedelta(days=10)).astimezone(UTC).isoformat()

    async def scenario():
        store = EarlierStore(retention_days=7)
        service = DailyReportService(store, retention_days=7)
        await service.run_due_once(_midnight() + timedelta(hours=12))
        reports = await store.list_reports()
        assert len(reports) == 6
        assert all(item["final"] for item in reports)

    asyncio.run(scenario())


def test_context_sample_is_bounded_but_statistics_cover_all_events():
    async def scenario():
        store = InMemoryReportStore()
        journal = ActivityJournal(store)
        for _ in range(20):
            await journal.record(
                "conversation", "web", None, "答复", "completed", {"response": "x" * 2000}
            )
        model = ReportModel()
        service = DailyReportService(store, model, max_context_chars=1000)
        report = await service.generate(service.today())
        assert report["activity_count"] == 20
        assert 0 < report["evidence_count"] < 20
        assert len(model.calls[0][1][-1].content) < 1300

    asyncio.run(scenario())


def test_successful_task_cancellation_resolves_pending_creation():
    async def scenario():
        store = InMemoryReportStore()
        tools = ToolRegistry(ActivityJournal(store))

        async def create(_context, _arguments):
            return {"task_id": "task-a", "status": "pending"}

        async def cancel(_context, arguments):
            return {"cancelled": arguments["task_id"] == "task-a"}

        tools.register(ToolSpec("task.create", "创建任务", {}, "task.write", create))
        tools.register(ToolSpec("task.cancel", "取消任务", {}, "task.write", cancel))
        await tools.execute("task.create", ToolContext(source="web"))
        await tools.execute("task.cancel", ToolContext(source="web"), {"task_id": "task-a"})
        await tools.execute("task.cancel", ToolContext(source="web"), {"task_id": "missing"})
        service = DailyReportService(store)
        report = await service.generate(service.today())
        assert report["content"]["pending"] == []
        assert report["counts"] == {"submitted": 1, "completed": 1, "failed": 1}
        assert report["content"]["completed"][0].startswith("取消任务")

    asyncio.run(scenario())


def test_future_dates_and_dates_before_recording_are_rejected():
    async def scenario():
        service = DailyReportService(InMemoryReportStore())
        for day in (_midnight().date() - timedelta(days=1), _midnight().date() + timedelta(days=1)):
            with pytest.raises(ValueError):
                await service.generate(day.isoformat())
        with pytest.raises(ValueError):
            await service.generate("2026-10-7")

    asyncio.run(scenario())


@pytest.mark.parametrize(
    "updates", [{"timezone": "No/SuchZone"}, {"generate_at": "25:00"}, {"generate_at": "23:00:00"}]
)
def test_invalid_report_schedule_is_rejected(updates):
    settings = Settings()
    with pytest.raises(ValueError, match="daily_report"):
        validate_settings(replace(settings, daily_report=replace(settings.daily_report, **updates)))


@pytest.mark.integration
def test_reports_require_admin_and_work_without_devices(monkeypatch):
    async def scenario():
        defaults = Settings()
        settings = replace(
            defaults,
            admin=replace(
                defaults.admin,
                allow_unauthenticated_loopback=False,
                password_env="BINBOT_TEST_REPORT_PASSWORD",
                token_env="BINBOT_TEST_REPORT_TOKEN",
            ),
        )
        monkeypatch.setenv("BINBOT_TEST_REPORT_PASSWORD", "report-test-password")
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                for path in ("/api/v1/agent/reports", "/api/v1/agent/reports/2026-10-07"):
                    async with client.get(server.make_url(path)) as response:
                        assert response.status == 401
                async with client.post(
                    server.make_url("/api/v1/agent/reports/generate"), json={"date": "2026-10-07"}
                ) as response:
                    assert response.status == 401
                async with client.post(
                    server.make_url("/api/v1/admin/session"),
                    json={"username": "admin", "password": "report-test-password"},
                ) as response:
                    assert response.status == 200
                async with client.get(server.make_url("/api/v1/agent/reports")) as response:
                    assert response.status == 200
                    index = await response.json()
                assert index["automatic"] is True
                assert index["timezone"] == "Asia/Shanghai"
                async with client.post(
                    server.make_url("/api/v1/agent/chat"),
                    json={
                        "device_id": "web:console",
                        "conversation_id": "web:report-test",
                        "text": "查询天气",
                    },
                ) as response:
                    assert response.status == 200
                async with client.post(
                    server.make_url("/api/v1/agent/reports/generate"), json={"date": index["today"]}
                ) as response:
                    assert response.status == 200
                    report = (await response.json())["report"]
                    assert report["activity_count"] == 1
                    assert report["mode"] == "template"
                async with client.get(
                    server.make_url("/api/v1/agent/reports/" + index["today"])
                ) as response:
                    detail = await response.json()
                    assert detail["activities"][0]["details"]["request"] == "查询天气"
                async with client.post(
                    server.make_url("/api/v1/agent/reports/generate"), json={"date": "not-a-date"}
                ) as response:
                    assert response.status == 400
                async with client.get(server.make_url("/healthz")) as response:
                    health = await response.json()
                    assert health["connected_devices"] == 0
                    assert health["daily_report_worker"] is True
        finally:
            await server.close()
        assert application.report_worker.running is False

    asyncio.run(scenario())
