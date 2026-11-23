"""Device-scoped one-shot task scheduling."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable, Sequence
from datetime import UTC, datetime
from uuid import UUID

from .ports import TaskItem, TaskStore


def normalize_run_at(value: str) -> str:
    """Validate an ISO-8601 timestamp and store it in UTC."""
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValueError("run_at must be an ISO-8601 timestamp") from exc
    if parsed.tzinfo is None:
        raise ValueError("run_at must include a timezone")
    return parsed.astimezone(UTC).isoformat()


def now_utc() -> str:
    return datetime.now(UTC).isoformat()


class TaskService:
    """Application boundary for creating and managing device tasks."""

    def __init__(self, store: TaskStore):
        self._store = store

    async def create(
        self,
        device_id: str,
        name: str,
        payload: dict[str, object],
        run_at: str,
    ) -> dict[str, object]:
        item = await self._store.create(
            device_id, name.strip(), dict(payload), normalize_run_at(run_at)
        )
        return item.snapshot()

    async def list(
        self,
        device_id: str,
        limit: int = 50,
        statuses: Sequence[str] = (),
    ) -> dict[str, object]:
        items = await self._store.list(device_id, limit, statuses)
        return {"tasks": [item.snapshot() for item in items]}

    async def cancel(self, device_id: str, task_id: str) -> dict[str, object]:
        try:
            parsed = UUID(task_id)
        except ValueError as exc:
            raise ValueError("task_id must be a UUID") from exc
        return {"cancelled": await self._store.cancel(device_id, parsed)}


TaskExecutor = Callable[[TaskItem], Awaitable[None]]


class TaskScheduler:
    """Claims due tasks and hands them to an application-provided executor."""

    def __init__(self, store: TaskStore, batch_size: int = 10):
        if batch_size < 1:
            raise ValueError("batch_size must be positive")
        self._store = store
        self._batch_size = batch_size

    async def claim_due(self, limit: int | None = None) -> Sequence[TaskItem]:
        return await self._store.claim_due(now_utc(), limit or self._batch_size)

    async def run_due_once(self, executor: TaskExecutor) -> tuple[TaskItem, ...]:
        claimed = await self.claim_due()
        for item in claimed:
            try:
                await executor(item)
            except Exception as exc:
                await self._store.finish(item.task_id, False, str(exc))
            else:
                await self._store.finish(item.task_id, True)
        return tuple(claimed)


class TaskWorker:
    """Runs the scheduler periodically while the gateway application is alive."""

    def __init__(
        self,
        scheduler: TaskScheduler,
        executor: TaskExecutor,
        poll_interval_seconds: float = 1.0,
    ):
        if poll_interval_seconds <= 0:
            raise ValueError("poll_interval_seconds must be positive")
        self._scheduler = scheduler
        self._executor = executor
        self._poll_interval = poll_interval_seconds
        self._task: asyncio.Task[None] | None = None
        self._stopping = asyncio.Event()

    def start(self) -> None:
        if self._task is None:
            self._stopping.clear()
            self._task = asyncio.create_task(self._run(), name="binbot-task-worker")

    @property
    def running(self) -> bool:
        return self._task is not None and not self._task.done()

    async def _run(self) -> None:
        try:
            while not self._stopping.is_set():
                await self._scheduler.run_due_once(self._executor)
                try:
                    await asyncio.wait_for(self._stopping.wait(), self._poll_interval)
                except asyncio.TimeoutError:
                    pass
        except asyncio.CancelledError:
            raise

    async def stop(self) -> None:
        self._stopping.set()
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
