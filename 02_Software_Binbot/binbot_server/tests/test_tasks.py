import asyncio
from datetime import UTC, datetime, timedelta

from binbot_server.application.tasks import TaskScheduler, TaskService, TaskWorker
from binbot_server.infrastructure.tasks import InMemoryTaskStore, SqliteTaskStore


def _past() -> str:
    return (datetime.now(UTC) - timedelta(seconds=1)).isoformat()


def test_memory_tasks_are_device_scoped_and_scheduler_updates_status() -> None:
    async def scenario() -> None:
        store = InMemoryTaskStore()
        service = TaskService(store)
        created = await service.create("device-a", "remind", {"text": "hi"}, _past())
        assert (await service.list("device-b"))["tasks"] == []
        scheduler = TaskScheduler(store)
        seen: list[str] = []

        async def execute(item) -> None:
            seen.append(item.name)

        claimed = await scheduler.run_due_once(execute)
        assert len(claimed) == 1
        assert seen == ["remind"]
        listed = await service.list("device-a")
        assert listed["tasks"][0]["status"] == "completed"
        assert await service.cancel("device-a", created["task_id"]) == {"cancelled": False}

    asyncio.run(scenario())


def test_scheduler_records_failures_and_cancel_only_pending() -> None:
    async def scenario() -> None:
        store = InMemoryTaskStore()
        service = TaskService(store)
        created = await service.create("device-a", "fail", {}, _past())

        async def execute(_item) -> None:
            raise RuntimeError("bad task")
        await TaskScheduler(store).run_due_once(execute)
        task = (await service.list("device-a"))["tasks"][0]
        assert task["status"] == "failed"
        assert task["error"] == "bad task"
        assert await service.cancel("device-a", created["task_id"]) == {"cancelled": False}

    asyncio.run(scenario())


def test_sqlite_tasks_survive_restart(tmp_path) -> None:
    async def scenario() -> None:
        path = str(tmp_path / "tasks.sqlite3")
        first = SqliteTaskStore(path)
        service = TaskService(first)
        created = await service.create("device-a", "persist", {"n": 1}, _past())
        await first.close()
        second = SqliteTaskStore(path)
        restored = await TaskService(second).list("device-a")
        assert restored["tasks"][0]["task_id"] == created["task_id"]
        assert restored["tasks"][0]["payload"] == {"n": 1}
        await second.close()

    asyncio.run(scenario())


def test_task_worker_polls_and_stops() -> None:
    async def scenario() -> None:
        store = InMemoryTaskStore()
        await store.create("device-a", "work", {}, _past())
        observed: list[str] = []

        async def execute(item) -> None:
            observed.append(item.name)

        worker = TaskWorker(TaskScheduler(store), execute, poll_interval_seconds=0.01)
        worker.start()
        for _ in range(20):
            if observed:
                break
            await asyncio.sleep(0.01)
        await worker.stop()
        assert observed == ["work"]

    asyncio.run(scenario())
