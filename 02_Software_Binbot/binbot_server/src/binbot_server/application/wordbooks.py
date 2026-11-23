"""Canonical wordbooks, small wire representations and daily learning queues."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime
import json
from pathlib import Path
from typing import Any, cast
import unicodedata
from zoneinfo import ZoneInfo

CATALOG_DIRECTORY = Path(__file__).parents[1] / "catalogs"
DAILY_CARDS = json.loads((CATALOG_DIRECTORY / "vocabulary.json").read_text())
CET4_CARDS = [
    {**card, "example": "", "translation": ""}
    for card in json.loads((CATALOG_DIRECTORY / "cet4.json").read_text())
]
BUILTIN_BOOKS = {
    "daily-english": ("日常英语", DAILY_CARDS),
    "cet4": ("大学英语四级", CET4_CARDS),
}
DEFAULT_DAILY_NEW_LIMIT = 20
CET4_BY_WORD = {card["word"]: card for card in CET4_CARDS}


def empty_review() -> dict[str, Any]:
    return {
        "stage": 0,
        "firstReviewedAt": None,
        "lastReviewedAt": None,
        "nextReviewAt": None,
        "reviewCount": 0,
        "lapseCount": 0,
        "history": [],
    }


def active_book(books: dict[str, Any]) -> dict[str, Any]:
    return next(book for book in books["books"] if book["id"] == books["activeBookId"])


def word_identity(book_id: str, word: str) -> str:
    # Angle/angle, China/china, Grant/grant and Polish/polish are distinct CET4 entries.
    normalized = " ".join(unicodedata.normalize("NFKC", word).split())
    return normalized if book_id == "cet4" else normalized.lower()


def first_review_at(review: dict[str, Any]) -> str | None:
    if review.get("firstReviewedAt"):
        return str(review["firstReviewedAt"])
    history = review.get("history", [])
    if history and len(history) == review.get("reviewCount"):
        return str(min(event["at"] for event in history))
    return None


def study_cards(book: dict[str, Any]) -> list[dict[str, Any]]:
    """Resolve the shared CET4 catalog against the device's sparse progress rows."""
    if book["id"] != "cet4":
        return cast(list[dict[str, Any]], book["words"])
    reviews = {card["word"]: card for card in book["words"]}
    return [
        {**card, "remembered": False, "review": empty_review(), **reviews.get(card["word"], {})}
        for card in CET4_CARDS
    ]


def full_wordbooks(books: dict[str, Any]) -> dict[str, Any]:
    return {**books, "books": [{**book, "words": study_cards(book)} for book in books["books"]]}


def learning_queue(
    book: dict[str, Any],
    daily_limit: int,
    timezone: str,
    now: datetime,
) -> list[dict[str, Any]]:
    return learning_plan(book, daily_limit, timezone, now)[0]


def learning_plan(
    book: dict[str, Any], daily_limit: int, timezone: str, now: datetime,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    zone = ZoneInfo(timezone)
    today = now.astimezone(zone).date()
    learned_today = 0
    review_load = 0
    next_review: datetime | None = None
    due: list[dict[str, Any]] = []
    fresh: list[dict[str, Any]] = []
    started: set[str] = set()
    for card in book["words"]:
        review = card["review"]
        if not review.get("reviewCount"):
            fresh.append(card)
            continue
        started.add(card["word"])
        first = first_review_at(review)
        learned = bool(first and datetime.fromisoformat(first).astimezone(zone).date() == today)
        if learned:
            learned_today += 1
        scheduled = review.get("nextReviewAt")
        due_at = datetime.fromisoformat(scheduled) if scheduled else None
        overdue = bool(due_at and due_at <= now)
        last = review.get("lastReviewedAt")
        reviewed_today = bool(last and datetime.fromisoformat(last).astimezone(zone).date() == today)
        if not learned and (overdue or reviewed_today):
            review_load += 1
        if overdue:
            definition = CET4_BY_WORD[card["word"]] if book["id"] == "cet4" else {}
            due.append({**definition, **card})
        elif due_at and (next_review is None or due_at < next_review):
            next_review = due_at
    due.sort(key=lambda card: card["review"]["nextReviewAt"])
    remaining = max(0, daily_limit - learned_today - max(0, review_load - daily_limit))
    if book["id"] == "cet4":
        fresh = []
        for card in CET4_CARDS:
            if len(fresh) == remaining:
                break
            if card["word"] not in started:
                fresh.append({**card, "review": empty_review()})
    new = fresh[:remaining]
    return [*due, *new], {
        "due": len(due), "new": len(new), "learned_today": learned_today,
        "next_review_at": int(next_review.timestamp()) if next_review else 0,
    }


def compact_wordbooks(value: dict[str, Any]) -> dict[str, Any]:
    """Built-in text ships with the console; transmit only per-device progress."""
    books = []
    for book in value["books"]:
        if book["id"] not in BUILTIN_BOOKS:
            books.append(book)
            continue
        progress = []
        for card in book["words"]:
            if not card["review"].get("reviewCount") and not card.get("remembered"):
                continue
            review = {key: value for key, value in card["review"].items() if key != "history"}
            review["firstReviewedAt"] = first_review_at(card["review"])
            progress.append(
                {"word": card["word"], "remembered": card["remembered"], "review": review}
            )
        books.append(
            {
                "id": book["id"],
                "name": book["name"],
                "catalog": book["id"] + ":1",
                "progress": progress,
            }
        )
    return {**value, "books": books}


def apply_wordbook_changes(previous: dict[str, Any], changes: Any) -> dict[str, Any]:
    """Merge small edits before the normal server-side wordbook validation."""
    if not isinstance(changes, dict) or set(changes) - {"activeBookId", "dailyNewLimit", "books"}:
        raise ValueError("词本变更格式无效")
    value = deepcopy(previous)
    for key in ("activeBookId", "dailyNewLimit"):
        if key in changes:
            value[key] = changes[key]
    rows = changes.get("books", [])
    if not isinstance(rows, list) or len(rows) > 25:
        raise ValueError("词本变更数量无效")
    ids = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) - {"id", "name", "deleted", "upsert", "remove"}:
            raise ValueError("词本变更包含未知字段")
        book_id = row.get("id")
        if not isinstance(book_id, str) or not book_id or len(book_id) > 64 or book_id in ids:
            raise ValueError("词本编号无效或重复")
        ids.add(book_id)
        book = next((entry for entry in value["books"] if entry["id"] == book_id), None)
        if "deleted" in row:
            if row != {"id": book_id, "deleted": True} or book_id in {
                *BUILTIN_BOOKS,
                "saved-words",
            }:
                raise ValueError("不能删除内置词本")
            value["books"] = [entry for entry in value["books"] if entry["id"] != book_id]
            continue
        if book is None:
            book = {"id": book_id, "name": row.get("name", ""), "words": []}
            value["books"].append(book)
        if "name" in row:
            book["name"] = row["name"]
        for field in ("remove", "upsert"):
            if not isinstance(row.get(field, []), list) or len(row.get(field, [])) > 10000:
                raise ValueError("单词变更格式无效")
        if book_id in BUILTIN_BOOKS and (row.get("remove") or row.get("upsert")):
            raise ValueError("内置词库内容不可修改")
        if any(not isinstance(word, str) for word in row.get("remove", [])):
            raise ValueError("移除单词格式无效")
        removed = {word_identity(book_id, word) for word in row.get("remove", [])}
        cards = {
            word_identity(book_id, card["word"]): card
            for card in book["words"]
            if word_identity(book_id, card["word"]) not in removed
        }
        for card in row.get("upsert", []):
            if not isinstance(card, dict) or not isinstance(card.get("word"), str):
                raise ValueError("单词内容格式无效")
            key = word_identity(book_id, card["word"])
            cards[key] = {**cards.get(key, {}), **card}
        book["words"] = list(cards.values())
    return value
