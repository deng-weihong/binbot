"""Device workspace data, review scheduling and confirmed device operations."""

from __future__ import annotations

import asyncio
from copy import deepcopy
from datetime import UTC, datetime, timedelta
import hashlib
import json
import re
from typing import Any, cast
from uuid import UUID
from zoneinfo import ZoneInfo

from binbot_server.domain.errors import StateConflict
from .device_commands import validate_device_payload
from .calendar import CalendarEventDraft
from .memory import Grade, adaptive_interval, grade_from, iso
from .wordbooks import (
    BUILTIN_BOOKS,
    CET4_BY_WORD,
    DAILY_CARDS,
    DEFAULT_DAILY_NEW_LIMIT,
    active_book,
    apply_wordbook_changes,
    compact_wordbooks,
    empty_review as empty_review,
    first_review_at,
    full_wordbooks,
    learning_plan,
    word_identity,
)

REVIEW_MINUTES = (20, 60, 1440, 2880, 5760, 10080, 21600, 43200)


def learning_batch(
    book: dict[str, Any], daily_limit: int, timezone: str, now: datetime,
) -> tuple[list[dict[str, Any]], dict[str, int], list[int]]:
    queue, plan = learning_plan(book, daily_limit, timezone, now)
    cards = [
        {field: card[field] for field in ("word", "phonetic", "meaning", "example", "translation")}
        for card in queue[:30]
    ]
    # The MQTT envelope escapes non-ASCII text and must fit the device IPC buffer.
    while cards and len(json.dumps(cards).encode()) > 13000:
        cards.pop()
    return cards, plan, [card["review"]["reviewCount"] for card in queue[:len(cards)]]


def schedule_review(
    previous: dict[str, Any], feedback: Grade | bool, at: datetime,
    timezone: str = "Asia/Shanghai", *, retry: bool = False,
) -> dict[str, Any]:
    previous = deepcopy(previous)
    first = first_review_at(previous)
    if first:
        previous["firstReviewedAt"] = min(first, iso(at))
    elif not previous["reviewCount"]:
        previous["firstReviewedAt"] = iso(at)
    last = (
        datetime.fromisoformat(previous["lastReviewedAt"])
        if previous.get("lastReviewedAt")
        else None
    )
    due = datetime.fromisoformat(previous["nextReviewAt"]) if previous.get("nextReviewAt") else None
    late = bool(last and at < last)
    grade: Grade = ("known" if feedback else "forgot") if isinstance(feedback, bool) else feedback
    remembered = grade == "known"
    stage = previous["stage"]
    memory = previous.get("memory")
    adaptive = not isinstance(feedback, bool) or memory is not None
    if adaptive:
        if late:
            next_at = due if due and due > at else at + timedelta(minutes=5)
        else:
            memory, next_at, short = adaptive_interval(
                previous, grade, at, ZoneInfo(timezone), retry=retry,
            )
            retry = retry or short
    else:
        early = bool(due and at < due)
        stage = (
            (stage if early else min(stage + 1, len(REVIEW_MINUTES)))
            if remembered
            else 0
        )
        next_at = (
            due
            if remembered and early
            else at + timedelta(minutes=REVIEW_MINUTES[stage - 1] if remembered else 5)
        )
    assert next_at is not None
    event = {"at": iso(at), "remembered": remembered, "stage": stage, "nextReviewAt": iso(next_at)}
    if adaptive:
        event.update(grade=grade, retry=retry)
    previous.update(
        reviewCount=previous["reviewCount"] + 1,
        lapseCount=previous["lapseCount"] + int(grade == "forgot"),
    )
    if not late:
        previous.update(stage=stage, lastReviewedAt=iso(at), nextReviewAt=iso(next_at))
        if memory is not None:
            previous["memory"] = memory
    previous["history"] = sorted([*previous["history"], event], key=lambda item: item["at"])[-32:]
    return previous


def record_review(
    data: dict[str, Any], book_id: str, word: str, feedback: Grade | bool, at: datetime,
    *, allow_missing: bool = False, retry: bool = False,
) -> dict[str, Any]:
    book = next((item for item in data["wordbooks"]["books"] if item["id"] == book_id), None)
    key = word_identity(book_id, word)
    if book is not None:
        card = next((item for item in book["words"]
                     if word_identity(book_id, item["word"]) == key), None)
        if card is None and book_id == "cet4" and key in CET4_BY_WORD:
            card = {"word": key, "remembered": False, "review": empty_review()}
            book["words"].append(card)
        if card is not None:
            old_at = card["review"]["lastReviewedAt"]
            card["review"] = schedule_review(card["review"], feedback, at, data["timezone"], retry=retry)
            if not old_at or iso(at) >= old_at:
                card["remembered"] = feedback in (True, "known")
            return data
    if allow_missing:
        return data
    raise ValueError("这个单词已不在词本中，请重新打开复习")


def text(value: Any, maximum: int, empty: bool = False) -> str:
    if (
        not isinstance(value, str)
        or len(value) > maximum
        or "\x00" in value
        or (not empty and not value.strip())
    ):
        raise ValueError("文字为空、过长或格式无效")
    return value.strip()


def default_workspace(device_id: str, timezone: str = "Asia/Shanghai") -> dict[str, Any]:
    return {
        "deviceName": device_id,
        "location": "未设置位置",
        "timezone": timezone,
        "assistant": {
            "name": device_id,
            "voice": "",
            "style": "简洁清晰",
            "instruction": "",
            "memories": [],
        },
        "notifications": True,
        "schedules": [],
        "wordbooks": {
            "activeBookId": "daily-english",
            "dailyNewLimit": DEFAULT_DAILY_NEW_LIMIT,
            "books": [
                {
                    "id": "daily-english",
                    "name": "日常英语",
                    "words": [
                        {**card, "remembered": False, "review": empty_review()}
                        for card in DAILY_CARDS
                    ],
                },
                {"id": "saved-words", "name": "我的生词本", "words": []},
                {"id": "cet4", "name": BUILTIN_BOOKS["cet4"][0], "words": []},
            ],
        },
        "legacy_learning_imported": False,
    }


class WorkspaceService:
    def __init__(
        self,
        store: Any,
        controls: Any,
        commands: Any,
        registry: Any,
        timezone: str = "Asia/Shanghai",
    ):
        self.store, self.controls, self.commands, self.registry = (
            store,
            controls,
            commands,
            registry,
        )
        self.timezone = timezone
        self.locks: dict[str, asyncio.Lock] = {}
        self.operation_locks: dict[str, asyncio.Lock] = {}
        self.background: set[asyncio.Task[Any]] = set()
        self._learning_reports: dict[str, str] = {}
        self._learning_refresh: set[str] = set()
        self._calendar_signatures: dict[str, str] = {}
        self.calendar_provider: Any = None
        self.calendar_status_provider: Any = None
        self.calendar_scope_provider: Any = None
        self._worker: asyncio.Task[None] | None = None
        self._migration_locks: dict[str, asyncio.Lock] = {}
        self._wordbooks_cache: dict[str, tuple[str, str, dict[str, Any]]] = {}

    @staticmethod
    def validate_id(device_id: str) -> str:
        if not re.fullmatch(r"[A-Za-z0-9._:-]{1,64}", device_id) or device_id in {".", ".."}:
            raise ValueError("设备编号无效")
        return device_id

    async def document(self, device_id: str) -> dict[str, Any]:
        self.validate_id(device_id)
        defaults = default_workspace(device_id, self.timezone)
        document = cast(dict[str, Any], await self.store.read(device_id, defaults))
        if not any(book["id"] == "cet4" for book in document["data"]["wordbooks"]["books"]):
            async with self._migration_locks.setdefault(device_id, asyncio.Lock()):
                document = await self.store.read(device_id, defaults)
                if not any(book["id"] == "cet4" for book in document["data"]["wordbooks"]["books"]):
                    def upgrade(data: dict[str, Any]) -> dict[str, Any]:
                        data["wordbooks"]["books"].append(defaults["wordbooks"]["books"][2])
                        data["wordbooks"].setdefault("dailyNewLimit", DEFAULT_DAILY_NEW_LIMIT)
                        return data
                    document = await self.store.update(device_id, "", upgrade)
        return document

    async def snapshot(
        self, device_id: str, *, compact: bool = False, wordbooks_revision: str = "",
    ) -> dict[str, Any]:
        document = await self.document(device_id)
        state = await self.controls.state(device_id) or {}
        control = state.get("control", {})
        devices = await self.registry.snapshot()
        live: dict[str, Any] = next((row for row in devices if row["id"] == device_id), {})
        control = {**control, "ready": bool(control.get("ready") and live.get("control_ready"))}
        public = {
            key: value
            for key, value in document["data"].items()
            if key != "legacy_learning_imported"
        }
        books = document["data"]["wordbooks"]
        cached = self._wordbooks_cache.get(device_id)
        if cached is None or cached[0] != document["revision"]:
            wire = compact_wordbooks(books)
            fingerprint = hashlib.sha256(
                json.dumps(wire, ensure_ascii=False, sort_keys=True).encode()
            ).hexdigest()
            cached = (document["revision"], fingerprint, wire)
            self._wordbooks_cache[device_id] = cached
        if compact:
            if cached[1] == wordbooks_revision:
                public.pop("wordbooks", None)
            else:
                public["wordbooks"] = cached[2]
        else:
            public["wordbooks"] = full_wordbooks(books)
        now = datetime.now(UTC)
        day = now.astimezone(ZoneInfo(public["timezone"])).replace(
            hour=0, minute=0, second=0, microsecond=0,
        )
        monday = day - timedelta(days=day.weekday())
        window = (monday, monday + timedelta(days=7)) if compact else None
        return {
            **document,
            "data": public,
            "wordbooks_revision": cached[1],
            "server_time": iso(now),
            "reported": control.get("features"),
            "system": control.get("system"),
            "calendar": await self.calendar_entries(device_id, document["data"], window=window),
            "calendar_sync": await self.calendar_status(device_id),
            "control": {
                key: control.get(key)
                for key in ("boot_id", "revision", "ready", "brightness", "volume")
            },
            "capabilities": control.get("capabilities", []),
        }

    async def calendar_status(self, device_id: str) -> dict[str, Any]:
        if self.calendar_status_provider:
            return cast(dict[str, Any], await self.calendar_status_provider(device_id))
        return {"enabled": False, "authorized": False, "error": ""}

    async def create_schedule(
        self, device_id: str, body: Any, *, compact: bool = False, wordbooks_revision: str = "",
    ) -> dict[str, Any]:
        """Create in this device's Feishu calendar, or locally when it is disabled.

        A local row is removed only after Feishu confirms creation. Stable source
        IDs and destination-scoped receipts prevent duplicates on browser retry.
        """
        self.validate_id(device_id)
        if not isinstance(body, dict) or set(body) != {"schedule", "event_id"}:
            raise ValueError("日程请求不完整")
        event_id = "schedule:" + text(body["event_id"], 80)
        async with self.locks.setdefault(device_id, asyncio.Lock()):
            current = await self.document(device_id)
            item = self.validate_preferences(current["data"], {"schedules": [body["schedule"]]})["schedules"][0]
            if item["id"].startswith("feishu:"):
                raise ValueError("飞书日程已同步，请在飞书中编辑")
            if await self.store.event_seen(device_id, event_id, item):
                return await self.snapshot(
                    device_id, compact=compact, wordbooks_revision=wordbooks_revision
                )
            old = next((row for row in current["data"]["schedules"] if row["id"] == item["id"]), None)
            if old is not None and old != item:
                raise StateConflict("日程已修改，请刷新后再同步")
            sync = await self.calendar_status(device_id)
            service = self.calendar_provider(device_id) if self.calendar_provider else None
            remote = sync["enabled"]
            if remote and service is None:
                raise StateConflict("请先在当前设备的飞书设置中重新连接账号，日程尚未保存")
            if remote:
                assert service is not None
                if item["done"] or not item["enabled"]:
                    raise ValueError("已完成或已关闭的日程不能创建到飞书")
                scope = self.calendar_scope_provider(device_id) if self.calendar_scope_provider else "default"
                destination = str(scope)
                receipt = await self.store.calendar_export(device_id, item["id"], destination, item)
                if receipt is None:
                    zone = ZoneInfo(current["data"]["timezone"])
                    start = datetime.fromisoformat(item["date"] + "T" + item["time"]).replace(tzinfo=zone)
                    draft = CalendarEventDraft(
                        summary=item["name"], start_timestamp=str(int(start.timestamp())),
                        end_timestamp=str(int((start + timedelta(minutes=30)).timestamp())),
                        timezone=current["data"]["timezone"], description=item["note"],
                    )
                    key = hashlib.sha256(json.dumps([device_id, destination, item["id"]]).encode()).hexdigest()
                    try:
                        event = await service.create_event(draft, key)
                    except Exception as exc:
                        raise StateConflict("飞书未确认创建日程，请重试：" + str(exc)[:250]) from exc
                    await self.store.remember_calendar_export(device_id, item["id"], destination, item, event.snapshot())

            def change(data: dict[str, Any]) -> dict[str, Any]:
                rows = [row for row in data["schedules"] if row["id"] != item["id"]]
                data["schedules"] = rows if remote else [*rows, item]
                return data

            await self.store.update(device_id, current["revision"], change, event_id=event_id, event_payload=item)
            self.invalidate_calendar(device_id)
            return await self.snapshot(
                device_id, compact=compact, wordbooks_revision=wordbooks_revision
            )

    async def review_word(
        self, device_id: str, body: Any, *, compact: bool = False, wordbooks_revision: str = "",
    ) -> dict[str, Any]:
        self.validate_id(device_id)
        required = {"book_id", "word", "event_id"}
        if (
            not isinstance(body, dict) or not required <= body.keys()
            or body.keys() - required not in ({"remembered"}, {"grade"}, {"grade", "retry"})
        ):
            raise ValueError("作答请求不完整")
        book_id = text(body["book_id"], 64)
        word = text(body["word"], 80)
        feedback = grade_from(body)
        event_id = "web-review:" + text(body["event_id"], 80)
        async with self.locks.setdefault(device_id, asyncio.Lock()):
            await self.document(device_id)
            now = datetime.now(UTC)
            await self.store.update(
                device_id, "", lambda data: record_review(
                    data, book_id, word, feedback, now, retry=body.get("retry", False),
                ),
                event_id=event_id, event_payload=body,
            )
            self._learning_refresh.add(device_id)
            return await self.snapshot(
                device_id, compact=compact, wordbooks_revision=wordbooks_revision
            )

    async def study_batch(self, device_id: str, book_id: str) -> dict[str, Any]:
        data = (await self.document(device_id))["data"]
        books = data["wordbooks"]
        if books["activeBookId"] != book_id:
            raise StateConflict("学习词本已切换，请刷新后开始复习")
        cards, _, _ = learning_batch(
            active_book(books), books.get("dailyNewLimit", DEFAULT_DAILY_NEW_LIMIT),
            data["timezone"], datetime.now(UTC),
        )
        return {"book_id": book_id, "words": [card["word"] for card in cards]}

    async def apply(
        self, device_id: str, name: str, payload: dict[str, Any], key: str
    ) -> dict[str, Any]:
        async with self.operation_locks.setdefault(device_id, asyncio.Lock()):
            return await self._apply(device_id, name, payload, key)

    async def _apply(
        self, device_id: str, name: str, payload: dict[str, Any], key: str
    ) -> dict[str, Any]:
        payload = validate_device_payload(name, payload)
        command = await self.commands.send(device_id, name, payload, key)
        deadline = asyncio.get_running_loop().time() + (
            58 if name in {"display.gif", "modules.image", "module.display.set"} else 18
        )
        while (
            command["status"] in {"queued", "delivered"}
            and asyncio.get_running_loop().time() < deadline
        ):
            await asyncio.sleep(0.04)
            command = await self.commands.command(UUID(command["command_id"])) or command
        if command["status"] != "completed":
            raise StateConflict(str(command.get("message") or "设备执行结果尚未确认，请刷新状态"))
        return cast(dict[str, Any], command)

    def validate_preferences(
        self, previous: dict[str, Any], patch: dict[str, Any]
    ) -> dict[str, Any]:
        allowed = {
            "deviceName",
            "location",
            "timezone",
            "assistant",
            "notifications",
            "schedules",
            "wordbooks",
            "wordbookChanges",
            "features",
            "todos",
            "microphone",
        }
        if not isinstance(patch, dict) or set(patch) - allowed:
            raise ValueError("工作台设置包含未知字段")
        value = deepcopy(previous)
        for key, maximum in (("deviceName", 20), ("location", 30)):
            if key in patch:
                value[key] = text(patch[key], maximum, empty=key == "location")
        if "timezone" in patch:
            zone = text(patch["timezone"], 80)
            ZoneInfo(zone)
            value["timezone"] = zone
        if "assistant" in patch:
            row = patch["assistant"]
            if not isinstance(row, dict) or set(row) != {
                "name",
                "voice",
                "style",
                "instruction",
                "memories",
            }:
                raise ValueError("助手设置不完整")
            if not isinstance(row["memories"], list) or len(row["memories"]) > 50:
                raise ValueError("最多保存 50 条偏好")
            value["assistant"] = {
                "name": text(row["name"], 24),
                "voice": text(row["voice"], 100, True),
                "style": text(row["style"], 100, True),
                "instruction": text(row["instruction"], 2000, True),
                "memories": [text(item, 500) for item in row["memories"]],
            }
        if "notifications" in patch:
            if type(patch["notifications"]) is not bool:
                raise ValueError("通知开关无效")
            value["notifications"] = patch["notifications"]
        if "schedules" in patch:
            rows = patch["schedules"]
            if not isinstance(rows, list) or len(rows) > 500:
                raise ValueError("日程数量过多")
            ids = set()
            normalized = []
            for row in rows:
                if not isinstance(row, dict) or set(row) != {
                    "id",
                    "name",
                    "date",
                    "time",
                    "note",
                    "enabled",
                    "done",
                }:
                    raise ValueError("日程格式无效")
                entry = {
                    key: text(row[key], maximum, empty=key == "note")
                    for key, maximum in (
                        ("id", 64),
                        ("name", 95),
                        ("date", 10),
                        ("time", 5),
                        ("note", 500),
                    )
                }
                if entry["id"] in ids:
                    raise ValueError("日程编号重复")
                datetime.strptime(entry["date"] + " " + entry["time"], "%Y-%m-%d %H:%M")
                if type(row["enabled"]) is not bool or type(row["done"]) is not bool:
                    raise ValueError("日程开关无效")
                ids.add(entry["id"])
                normalized.append({**entry, "enabled": row["enabled"], "done": row["done"]})
            value["schedules"] = normalized
        if "wordbooks" in patch:
            if "wordbookChanges" in patch:
                raise ValueError("请只提交一种词本变更")
            value["wordbooks"] = self.validate_wordbooks(previous["wordbooks"], patch["wordbooks"])
        if "wordbookChanges" in patch:
            value["wordbooks"] = self.validate_wordbooks(
                previous["wordbooks"],
                apply_wordbook_changes(previous["wordbooks"], patch["wordbookChanges"]),
            )
        return value

    @staticmethod
    def validate_wordbooks(previous: dict[str, Any], raw: Any) -> dict[str, Any]:
        if (
            not isinstance(raw, dict)
            or set(raw) - {"activeBookId", "dailyNewLimit", "books"}
            or not {"activeBookId", "books"} <= set(raw)
            or not isinstance(raw["books"], list)
            or not 2 <= len(raw["books"]) <= 25
        ):
            raise ValueError("词本配置无效，最多保存 25 个词本")
        old_books = {book["id"]: book for book in previous["books"]}
        ids = set()
        books = []
        total = 0
        for book in raw["books"]:
            if not isinstance(book, dict) or set(book) != {"id", "name", "words"}:
                raise ValueError("词本格式无效")
            book_id = text(book["id"], 64)
            name = text(book["name"], 24)
            if book_id in ids or not isinstance(book["words"], list) or len(book["words"]) > 10000:
                raise ValueError("词本编号重复或词数过多")
            ids.add(book_id)
            seen = set()
            words = []
            old_words = {
                word_identity(book_id, card["word"]): card
                for card in old_books.get(book_id, {}).get("words", [])
            }
            for card in book["words"]:
                if not isinstance(card, dict):
                    raise ValueError("单词格式无效")
                spelling = text(card.get("word"), 80)
                key = word_identity(book_id, spelling)
                if book_id == "cet4":
                    if key not in CET4_BY_WORD:
                        raise ValueError("四级词本中的单词无效")
                    word = {"word": key}
                else:
                    if not re.search(r"[a-zA-Z]", spelling):
                        raise ValueError("请填写英文单词或短语")
                    word = {
                        "word": spelling,
                        "phonetic": text(card.get("phonetic"), 120, empty=True),
                        "meaning": text(card.get("meaning"), 300),
                        "example": text(card.get("example"), 500, empty=True),
                        "translation": text(card.get("translation"), 500, empty=True),
                    }
                if key in seen:
                    raise ValueError("词本中存在重复单词")
                seen.add(key)
                old = old_words.get(key, {})
                review = deepcopy(old.get("review") or empty_review())
                remembered = old.get("remembered", False)
                incoming = card.get("review") or {}
                count = incoming.get("reviewCount", 0) if isinstance(incoming, dict) else 0
                if type(count) is not int or count < 0:
                    raise ValueError("学习次数无效")
                if count > review["reviewCount"]:
                    if (
                        count != review["reviewCount"] + 1
                        or type(card.get("remembered")) is not bool
                    ):
                        raise ValueError("学习记录已变化，请刷新后按顺序复习")
                    remembered = card["remembered"]
                    review = schedule_review(review, remembered, datetime.now(UTC))
                words.append({**word, "remembered": remembered, "review": review})
            if book_id == "cet4":
                progress = {**old_words, **{card["word"]: card for card in words}}
                words = [
                    {
                        "word": key,
                        "remembered": card["remembered"],
                        "review": card["review"],
                    }
                    for key, card in progress.items()
                    if key in CET4_BY_WORD and (card["review"]["reviewCount"] or card["remembered"])
                ]
                name = BUILTIN_BOOKS[book_id][0]
            else:
                total += len(words)
            books.append({"id": book_id, "name": name, "words": words})
        if "cet4" not in ids and "cet4" in old_books:
            books.append(deepcopy(old_books["cet4"]))
            ids.add("cet4")
        if len(books) > 25:
            raise ValueError("最多保存 25 个词本")
        if not {"daily-english", "saved-words"} <= ids or raw["activeBookId"] not in ids:
            raise ValueError("请保留内置词本并选择有效的学习词本")
        if total > 20000:
            raise ValueError("所有词本合计最多保存 20000 词")
        daily_limit = raw.get(
            "dailyNewLimit", previous.get("dailyNewLimit", DEFAULT_DAILY_NEW_LIMIT)
        )
        if type(daily_limit) is not int or not 1 <= daily_limit <= 100:
            raise ValueError("每日新词量须为 1 至 100")
        return {"activeBookId": raw["activeBookId"], "dailyNewLimit": daily_limit, "books": books}

    async def save(
        self, device_id: str, body: Any, *, compact: bool = False, wordbooks_revision: str = "",
    ) -> dict[str, Any]:
        self.validate_id(device_id)
        if not isinstance(body, dict) or set(body) != {"revision", "data", "event_id"}:
            raise ValueError("设置请求不完整")
        event_id = text(body["event_id"], 96)
        async with self.locks.setdefault(device_id, asyncio.Lock()):
            current = await self.document(device_id)
            if await self.store.event_seen(device_id, event_id, body["data"]):
                return await self.snapshot(
                    device_id, compact=compact, wordbooks_revision=wordbooks_revision
                )
            if body["revision"] != current["revision"]:
                raise StateConflict("设置已更新，请刷新后重试")
            updated = self.validate_preferences(current["data"], body["data"])
            patch = body["data"]
            control = (await self.controls.state(device_id) or {}).get("control", {})
            reported = control.get("features") or {}
            actions: list[tuple[str, dict[str, Any]]] = []
            features = patch.get("features", {})
            if not isinstance(features, dict):
                raise ValueError("设备设置格式无效")
            for field, name in (("watchFace", "watchface.set"), ("gestures", "gestures.set")):
                if field in features and features[field] != reported.get(field):
                    actions.append((name, features[field]))
            if "modules" in features:
                module = features["modules"]
                if not isinstance(module, dict):
                    raise ValueError("组件设置无效")
                old_module = reported.get("modules") or {}
                for field, name in (
                    ("turntable", "module.motor.set"),
                    ("light", "module.light.set"),
                ):
                    if field in module and module[field] != old_module.get(field):
                        actions.append((name, module[field]))
                if "pen" in module:
                    pen = module["pen"]
                    old_pen = old_module.get("pen") or {}
                    if any(pen.get(k) != old_pen.get(k) for k in ("mode", "face")):
                        actions.append(("module.pen.set", {k: pen[k] for k in ("mode", "face")}))
                for field, profile in (("pen", 1), ("holo", 2)):
                    row = module.get(field) or {}
                    old = old_module.get(field) or {}
                    if (
                        row.get("intervalSeconds") != old.get("intervalSeconds")
                        and "intervalSeconds" in row
                    ):
                        actions.append(
                            (
                                "module.rotation.set",
                                {"profile": profile, "seconds": row["intervalSeconds"]},
                            )
                        )
                    if row.get("imageId") != old.get("imageId") and row.get("imageId"):
                        actions.append(
                            ("module.display.set", {"profile": profile, "image_id": row["imageId"]})
                        )
            if "microphone" in patch:
                actions.append(("microphone.set", {"enabled": patch["microphone"]}))
            if any(
                k in patch and updated[k] != reported.get(k) for k in ("notifications", "timezone")
            ):
                actions.append(
                    ("preferences.set", {k: updated[k] for k in ("notifications", "timezone")})
                )
            if "todos" in patch:
                actions.append(("todo.replace", {"items": patch["todos"]}))
            actions = [(name, validate_device_payload(name, payload)) for name, payload in actions]
            for index, (name, payload) in enumerate(actions):
                await self.apply(device_id, name, payload, f"workspace:{event_id}:{index}")
            await self.store.update(
                device_id,
                current["revision"],
                lambda _: updated,
                event_id=event_id,
                event_payload=patch,
            )

            def active_content(books: dict[str, Any]) -> list[Any]:
                active = active_book(books)
                if active["id"] == "cet4":
                    return [active["id"]]
                return [
                    active["id"],
                    [
                        {k: v for k, v in word.items() if k not in {"review", "remembered"}}
                        for word in active["words"]
                    ],
                ]

            if active_content(updated["wordbooks"]) != active_content(
                current["data"]["wordbooks"]
            ) or updated["wordbooks"].get("dailyNewLimit") != current["data"]["wordbooks"].get(
                "dailyNewLimit"
            ):
                self._learning_refresh.add(device_id)
                vocabulary = (control.get("features") or {}).get("vocabulary") or {}
                if not control.get("learning_events") and (
                    not vocabulary.get("studying")
                    or vocabulary.get("bookId") != updated["wordbooks"]["activeBookId"]
                ):
                    try:
                        await self.activate_book(
                            device_id, updated["wordbooks"], event_id, updated["timezone"]
                        )
                    except StateConflict:
                        pass  # Retry when the device is ready.
                    else:
                        self._learning_refresh.discard(device_id)
            if actions:
                await asyncio.sleep(0.08)
            return await self.snapshot(
                device_id, compact=compact, wordbooks_revision=wordbooks_revision
            )

    async def activate_book(
        self, device_id: str, books: dict[str, Any], key: str, timezone: str | None = None,
    ) -> None:
        book = active_book(books)
        cards, plan, review_counts = learning_batch(
            book, books.get("dailyNewLimit", DEFAULT_DAILY_NEW_LIMIT),
            timezone or self.timezone, datetime.now(UTC),
        )
        state = await self.controls.state(device_id) or {}
        vocabulary = (state.get("control", {}).get("features") or {}).get("vocabulary") or {}
        details: dict[str, Any] = {}
        if vocabulary.get("studyPlanVersion") == 1:
            # Due cards precede new ones in each batch.
            first_new = min(plan["due"], len(cards))
            plan["new_mask"] = ((1 << len(cards)) - 1) ^ ((1 << first_new) - 1)
            details["plan"] = plan
        deck_id = hashlib.sha256(
            json.dumps(
                [book["id"], cards, review_counts, details],
                ensure_ascii=False,
            ).encode()
        ).hexdigest()[:32]
        if vocabulary.get("deckId") == deck_id and vocabulary.get("bookId") == book["id"]:
            return
        await self.apply(
            device_id,
            "learning.deck.set",
            {"book_id": book["id"], "deck_id": deck_id, "cards": cards, **details},
            "deck:" + key + ":" + deck_id,
        )

    async def ingest(self, device_id: str, report: dict[str, Any]) -> None:
        features = report.get("features") or {}
        vocabulary = features.get("vocabulary") or {}
        events = report.get("learning_events") or []
        if not vocabulary and not events:
            return
        async with self.locks.setdefault(device_id, asyncio.Lock()):
            document = await self.document(device_id)
            if not document["data"].get("legacy_learning_imported"):
                known = set(vocabulary.get("legacyKnownWords", vocabulary.get("knownWords", [])))

                def import_legacy(data: dict[str, Any]) -> dict[str, Any]:
                    daily = next(book for book in data["wordbooks"]["books"]
                                 if book["id"] == "daily-english")
                    for card in daily["words"]:
                        if not card["review"]["reviewCount"]:
                            card["remembered"] = card["remembered"] or card["word"] in known
                    data["legacy_learning_imported"] = True
                    data["legacy_learning_statistics"] = {
                        "reviewed": vocabulary.get(
                            "legacyReviewedCount", vocabulary.get("reviewedCount", 0)
                        ),
                        "correct": vocabulary.get(
                            "legacyCorrectCount", vocabulary.get("correctCount", 0)
                        ),
                        "cycles": vocabulary.get(
                            "legacyCyclesCompleted", vocabulary.get("cyclesCompleted", 0)
                        ),
                    }
                    return data

                await self.store.update(device_id, "", import_legacy)
            acknowledged = []
            for event in events[:32]:
                if (
                    not isinstance(event, dict)
                    or not isinstance(event.get("id"), str)
                    or not isinstance(event.get("book_id"), str)
                    or not isinstance(event.get("word"), str)
                ):
                    continue
                try:
                    feedback = grade_from(event)
                    at = datetime.fromtimestamp(int(event["at"]), UTC)
                    if not 2000 <= at.year < 2100 or at > datetime.now(UTC) + timedelta(minutes=5):
                        continue

                    await self.store.update(
                        device_id,
                        "",
                        lambda data: record_review(
                            data, event["book_id"], event["word"], feedback, at,
                            allow_missing=True, retry=event.get("retry", False),
                        ),
                        event_id="learning:" + event["id"],
                        event_payload=event,
                    )
                    acknowledged.append(event["id"])
                except (ValueError, KeyError, TypeError, OverflowError, OSError, StateConflict):
                    continue
        if acknowledged:
            try:
                await self.apply(
                    device_id,
                    "learning.events.ack",
                    {"event_ids": acknowledged},
                    "learning-ack:"
                    + hashlib.sha256(
                        json.dumps(
                            [
                                report.get("boot_id"),
                                int(datetime.now(UTC).timestamp()) // 20,
                                acknowledged,
                            ]
                        ).encode()
                    ).hexdigest(),
                )
            except StateConflict:
                self._learning_reports.pop(device_id, None)
        if report.get("ready") and "workspace_v1" in report.get("capabilities", []):
            await self._sync_learning(device_id)

    async def _sync_learning(self, device_id: str) -> None:
        async with self.locks.setdefault(device_id, asyncio.Lock()):
            state = await self.controls.state(device_id) or {}
            control = state.get("control", {})
            vocabulary = (control.get("features") or {}).get("vocabulary") or {}
            if not control.get("ready") or "workspace_v1" not in control.get("capabilities", []):
                return
            # Finish committing queued device answers before choosing the next batch.
            if control.get("learning_events"):
                return
            data = (await self.document(device_id))["data"]
            desired = data["wordbooks"]
            book_changed = vocabulary.get("bookId") != desired["activeBookId"]
            if vocabulary.get("studying") and not book_changed:
                return
            if (
                device_id in self._learning_refresh
                or book_changed
                or vocabulary.get("complete")
                or vocabulary.get("studyPlanVersion") == 1
            ):
                try:
                    await self.activate_book(
                        device_id, desired,
                        f"{control.get('boot_id', '')}:{int(datetime.now(UTC).timestamp()) // 20}",
                        data["timezone"],
                    )
                except StateConflict:
                    pass
                else:
                    self._learning_refresh.discard(device_id)

    def accept_report(self, device_id: str, report: dict[str, Any]) -> None:
        # Never block the MQTT receive lock while waiting for device receipts.
        fingerprint = json.dumps(
            [
                report.get("boot_id"),
                report.get("ready"),
                (report.get("features") or {}).get("vocabulary"),
                report.get("learning_events"),
            ],
            sort_keys=True,
        )
        if self._learning_reports.get(device_id) == fingerprint:
            return
        self._learning_reports[device_id] = fingerprint
        task = asyncio.create_task(self.ingest(device_id, report), name="workspace-device-report")
        self.background.add(task)

        def done(value: asyncio.Task[Any]) -> None:
            self.background.discard(value)
            if not value.cancelled():
                value.exception()

        task.add_done_callback(done)

    async def close(self) -> None:
        if self._worker:
            self._worker.cancel()
            await asyncio.gather(self._worker, return_exceptions=True)
        tasks = list(self.background)
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    async def calendar_entries(
        self, device_id: str, data: dict[str, Any] | None = None,
        *, window: tuple[datetime, datetime] | None = None,
    ) -> list[dict[str, Any]]:
        if data is None:
            data = (await self.document(device_id))["data"]
        entries = [{**item, "source": "local"} for item in data["schedules"]]
        if window:
            start_day, end_day = (value.date().isoformat() for value in window)
            entries = [item for item in entries if start_day <= item["date"] <= end_day]
        service = self.calendar_provider(device_id) if self.calendar_provider else None
        if service is not None:
            zone = ZoneInfo(data["timezone"])
            for event in await service.snapshot(unbounded=True, window=window):
                if event.is_deleted or event.status == "cancelled":
                    continue
                try:
                    start = (
                        datetime.fromtimestamp(int(event.start_timestamp), UTC).astimezone(zone)
                        if event.start_timestamp
                        else datetime.fromisoformat(event.start_date).replace(tzinfo=zone)
                    )
                    entries.append(
                        {
                            "id": "feishu:" + event.event_id,
                            "name": event.summary,
                            "date": start.date().isoformat(),
                            "time": start.strftime("%H:%M"),
                            "note": event.description or event.location,
                            "enabled": True,
                            "done": False,
                            "source": "feishu",
                        }
                    )
                except (ValueError, TypeError, OverflowError):
                    continue
        return sorted(entries, key=lambda item: (item["date"], item["time"], item["id"]))

    async def _sync_calendar(self, device_id: str, boot_id: str) -> None:
        data = (await self.document(device_id))["data"]
        zone = ZoneInfo(data["timezone"])
        now = datetime.now(UTC).replace(second=0, microsecond=0)
        start = now - timedelta(hours=1)
        end = now + timedelta(days=7)
        entries = await self.calendar_entries(
            device_id,
            data,
            window=(start.astimezone(zone), (end + timedelta(minutes=1)).astimezone(zone)),
        )
        offset_now = now.astimezone(zone).utcoffset()
        assert offset_now is not None
        reported = (await self.controls.state(device_id) or {}).get("control", {}).get(
            "features"
        ) or {}
        if reported.get("timezone") != data["timezone"] or reported.get("utcOffset") != int(
            offset_now.total_seconds()
        ):
            await self.apply(
                device_id,
                "preferences.set",
                {
                    "notifications": reported.get("notifications", data["notifications"]),
                    "timezone": data["timezone"],
                },
                "timezone:" + boot_id + ":" + str(int(now.timestamp()) // 60),
            )
        events = []
        for item in entries:
            if not item["enabled"] or item["done"]:
                continue
            start = datetime.fromisoformat(item["date"] + "T" + item["time"]).replace(tzinfo=zone)
            if not now - timedelta(hours=1) <= start <= end:
                continue
            # Bound text by the existing UTF-8 device calendar field, not by codepoints.
            title = item["name"].encode()[:95].decode(errors="ignore")
            events.append(
                {
                    "id": hashlib.sha256(item["id"].encode()).hexdigest()[:32],
                    "title": title,
                    "start": int(start.timestamp()),
                    "end": int(start.timestamp()) + 1800,
                }
            )
        events = events[:8]
        utc_offset = now.astimezone(zone).utcoffset()
        assert utc_offset is not None
        offset = int(utc_offset.total_seconds() // 60)
        signature = hashlib.sha256(
            json.dumps([boot_id, offset, events], sort_keys=True).encode()
        ).hexdigest()
        if self._calendar_signatures.get(device_id) == signature:
            return
        await self.apply(
            device_id,
            "calendar.set",
            {"revision": int(signature[:8], 16), "timezone_offset": offset, "events": events},
            "calendar:" + signature,
        )
        self._calendar_signatures[device_id] = signature

    def invalidate_calendar(self, device_id: str) -> None:
        self._calendar_signatures.pop(device_id, None)

    def start(self) -> None:
        async def worker() -> None:
            while True:
                for device in await self.registry.snapshot():
                    if (
                        not device.get("control_ready")
                        or "workspace_v1" not in device["capabilities"]
                    ):
                        continue
                    state = await self.controls.state(device["id"]) or {}
                    try:
                        await self._sync_calendar(
                            device["id"], state.get("control", {}).get("boot_id", "")
                        )
                    except (StateConflict, ValueError):
                        pass
                    try:
                        await self._sync_learning(device["id"])
                    except (StateConflict, ValueError):
                        pass
                await asyncio.sleep(5)

        self._worker = asyncio.create_task(worker(), name="workspace-calendar-sync")
