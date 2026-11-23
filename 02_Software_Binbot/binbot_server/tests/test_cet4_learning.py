import asyncio
from datetime import UTC, datetime, timedelta
from unittest.mock import AsyncMock

import pytest

from binbot_server.application.device_commands import validate_device_payload
from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.wordbooks import learning_queue
from binbot_server.application.workspace import (
    WorkspaceService,
    default_workspace,
    empty_review,
    schedule_review,
)
from binbot_server.domain.errors import StateConflict
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore


def find_book(books, book_id="cet4"):
    return next(book for book in books["books"] if book["id"] == book_id)


def test_cet4_migrates_old_books_and_syncs_both_review_sources_without_resending_the_catalog():
    async def run():
        store = SqliteWorkspaceStore(":memory:")
        old = default_workspace("one")
        old_books = old["wordbooks"]
        old_books["books"].pop()
        old_books.pop("dailyNewLimit")
        old_books["books"][0]["words"][0]["remembered"] = True
        await store.read("one", old)
        service = WorkspaceService(store, DeviceControlStore(), None, DeviceSessionRegistry())
        service.apply = AsyncMock(return_value={"status": "completed"})
        try:
            loaded = await service.snapshot("one", compact=True)
            books = loaded["data"]["wordbooks"]
            assert books["activeBookId"] == "daily-english"
            assert books["books"][0]["progress"][0]["remembered"] is True
            assert find_book(books)["progress"] == []
            assert find_book((await store.read("one", {}))["data"]["wordbooks"])["words"] == []
            tag = loaded["wordbooks_revision"]
            poll = await service.snapshot("one", compact=True, wordbooks_revision=tag)
            assert "wordbooks" not in poll["data"]

            await service.save(
                "one",
                {
                    "revision": loaded["revision"],
                    "event_id": "select-cet4",
                    "data": {"wordbookChanges": {"activeBookId": "cet4", "dailyNewLimit": 10}},
                },
                compact=True,
            )
            _, command, deck, _ = service.apply.await_args.args
            assert command == "learning.deck.set"
            assert deck["book_id"] == "cet4"
            assert len(deck["cards"]) == 10 and deck["cards"][0]["word"] == "abandon"

            report = {
                "learning_events": [
                    {
                        "id": "offline:1",
                        "book_id": "cet4",
                        "word": "China",
                        "remembered": True,
                        "at": int(datetime.now(UTC).timestamp()),
                    },
                ]
            }
            await service.ingest("one", report)
            await service.ingest("one", report)
            assert service.apply.await_args.args[1:3] == (
                "learning.events.ack",
                {"event_ids": ["offline:1"]},
            )
            answer = {"event_id": "web:1", "book_id": "cet4", "word": "china", "remembered": False}
            saved = await service.review_word("one", answer, compact=True, wordbooks_revision=tag)
            assert (await service.review_word("one", answer, compact=True))["revision"] == saved[
                "revision"
            ]
            with pytest.raises(StateConflict):
                await service.review_word("one", {**answer, "remembered": True}, compact=True)
            words = find_book(saved["data"]["wordbooks"])["progress"]
            assert [
                (word["word"], word["review"]["reviewCount"], word["remembered"]) for word in words
            ] == [
                ("China", 1, True),
                ("china", 1, False),
            ]
            assert (
                len(find_book((await service.snapshot("one"))["data"]["wordbooks"])["words"])
                == 4413
            )
            assert (
                find_book((await service.snapshot("two", compact=True))["data"]["wordbooks"])[
                    "progress"
                ]
                == []
            )
        finally:
            await store.close()

    asyncio.run(run())


def test_queue_rolls_at_device_midnight_and_always_includes_due_reviews():
    # 15:59 UTC is 23:59 for the device. Review again before and after midnight.
    at = datetime(2026, 11, 19, 15, 59, tzinfo=UTC)
    book = {
        "id": "cet4",
        "words": [
            {
                "word": "abandon",
                "review": schedule_review(
                    empty_review(),
                    False,
                    at - timedelta(minutes=6),
                ),
            }
        ],
    }
    assert [card["word"] for card in learning_queue(book, 1, "Asia/Shanghai", at)] == ["abandon"]
    after_midnight = learning_queue(book, 1, "Asia/Shanghai", at + timedelta(minutes=2))
    assert [card["word"] for card in after_midnight] == ["abandon", "ability"]


def test_adaptive_reviews_keep_day_retests_out_of_the_long_term_model():
    at = datetime(2026, 11, 19, 15, 50, tzinfo=UTC)
    legacy = schedule_review(empty_review(), True, at - timedelta(days=1))
    fuzzy = schedule_review(legacy, "fuzzy", at)
    assert fuzzy["reviewCount"] == 2 and fuzzy["lapseCount"] == 0
    assert fuzzy["history"][0] == legacy["history"][0]
    assert datetime.fromisoformat(fuzzy["nextReviewAt"]) == at + timedelta(minutes=5)

    retry = schedule_review(fuzzy, "known", at + timedelta(minutes=11), retry=True)
    assert retry["history"][-1]["retry"] is True
    assert retry["memory"] == fuzzy["memory"]
    assert retry["nextReviewAt"] == retry["memory"]["longTermAt"]
    at_due = datetime.fromisoformat(retry["nextReviewAt"]) + timedelta(hours=8)
    known = schedule_review(retry, "known", at_due)
    assert known["memory"]["halfLifeDays"] > retry["memory"]["halfLifeDays"]
    assert known["history"][-1]["retry"] is False
    late = schedule_review(known, "forgot", at + timedelta(minutes=1))
    assert late["memory"] == known["memory"]
    assert late["nextReviewAt"] == known["nextReviewAt"]
    assert late["lapseCount"] == known["lapseCount"] + 1


def test_old_review_load_reduces_new_words_even_after_today_reviews_are_answered():
    at = datetime(2026, 11, 19, 3, 0, tzinfo=UTC)
    old = schedule_review(empty_review(), "known", at - timedelta(days=2))
    book = {"id": "cet4", "words": [
        {"word": word, "review": old}
        for word in ("abandon", "ability", "able", "abnormal")
    ]}
    assert len(learning_queue(book, 2, "Asia/Shanghai", at)) == 4
    for word in book["words"]:
        word["review"] = schedule_review(old, "known", at)
    assert learning_queue(book, 2, "Asia/Shanghai", at) == []


def test_web_and_device_share_the_batch_when_long_cards_hit_the_mqtt_size_limit():
    async def run():
        store = SqliteWorkspaceStore(":memory:")
        controls = DeviceControlStore()
        service = WorkspaceService(store, controls, None, DeviceSessionRegistry())
        service.apply = AsyncMock(return_value={"status": "completed"})
        await controls.update_control_state("one", {
            "boot_id": "boot", "revision": 1,
            "features": {"vocabulary": {"studyPlanVersion": 1}},
        })
        data = default_workspace("one")
        books = data["wordbooks"]
        books["activeBookId"] = "custom"
        books["books"].append({"id": "custom", "name": "Custom", "words": [
            {"word": f"word {i}", "meaning": "长" * 280, "phonetic": "", "example": "",
             "translation": "", "review": empty_review(), "remembered": False}
            for i in range(20)
        ]})
        try:
            await store.read("one", data)
            web = await service.study_batch("one", "custom")
            await service.activate_book("one", books, "test", data["timezone"])
            _, command, deck, _ = service.apply.await_args.args
            assert command == "learning.deck.set"
            assert validate_device_payload(command, deck) == deck
            assert 0 < len(deck["cards"]) < 20
            assert web["words"] == [card["word"] for card in deck["cards"]]
            assert deck["plan"] == {
                "due": 0, "new": 20, "learned_today": 0, "next_review_at": 0,
                "new_mask": (1 << len(deck["cards"])) - 1,
            }
        finally:
            await store.close()

    asyncio.run(run())
