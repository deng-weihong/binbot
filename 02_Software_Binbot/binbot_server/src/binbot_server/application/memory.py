"""Adaptive cross-day memory state and short relearning intervals."""

from datetime import UTC, datetime, time, timedelta
from math import exp, log2, sqrt
from typing import Any, Literal, cast
from zoneinfo import ZoneInfo

Grade = Literal["known", "fuzzy", "forgot"]
TARGET_RETENTION = 0.85
RELEARN_DELAY = timedelta(minutes=5)
MAX_INTERVAL_DAYS = 365
_RETENTION_INTERVAL = -log2(TARGET_RETENTION)


def grade_from(payload: dict[str, Any]) -> Grade | bool:
    if "grade" in payload:
        grade = payload["grade"]
        if grade not in ("known", "fuzzy", "forgot") or type(payload.get("retry", False)) is not bool:
            raise ValueError("作答反馈无效")
        return cast(Grade, grade)
    if type(payload.get("remembered")) is not bool:
        raise ValueError("作答反馈无效")
    return cast(bool, payload["remembered"])


def iso(value: datetime) -> str:
    return value.astimezone(UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def adaptive_interval(
    previous: dict[str, Any], grade: Grade, at: datetime, zone: ZoneInfo, *, retry: bool,
) -> tuple[dict[str, Any], datetime, bool]:
    """Update the half-life once per study day and schedule short retries separately."""
    memory = previous.get("memory")
    original_due = previous.get("nextReviewAt")
    due = datetime.fromisoformat(original_due) if original_due else None
    if memory:
        last = datetime.fromisoformat(memory["lastRecallAt"])
        long_term = datetime.fromisoformat(memory["longTermAt"])
        if retry or at.astimezone(zone).date() == last.astimezone(zone).date():
            next_at = long_term if grade == "known" else min(long_term, at + RELEARN_DELAY)
            return memory, next_at, True
        if grade == "known" and due and at < due:
            return memory, due, True
        difficulty, half_life = memory["difficulty"], memory["halfLifeDays"]
    else:
        last = (
            datetime.fromisoformat(previous["lastReviewedAt"])
            if previous.get("lastReviewedAt") else None
        )
        difficulty = {"known": 3, "fuzzy": 6, "forgot": 8}[grade] if not last else 5
        half_life = -1 / log2(0.925 - 0.05 * difficulty)
        # Seed previously learned words from their saved interval on the first new answer.
        if last and due and previous.get("stage", 0) > 0:
            half_life = max(half_life, (due - last).total_seconds() / 86400 / _RETENTION_INTERVAL)

    if last:
        elapsed = max(0, (at - last).total_seconds() / 86400)
        forgetting = max(0.001, 1 - 2 ** (-elapsed / half_life))
        # DHP state transitions from MaiMemo's public SSP-MMC model.
        if grade == "known":
            half_life *= 1 + exp(3.81) * difficulty ** -0.534 * half_life ** -0.127 * forgetting ** 0.97
        else:
            forgotten = min(half_life, exp(-0.041) * difficulty ** -0.041 * half_life ** 0.377 * forgetting ** -0.227)
            half_life = forgotten if grade == "forgot" else sqrt(half_life * forgotten)
            difficulty = min(18, difficulty + (2 if grade == "forgot" else 1))

    half_life = round(max(0.1, min(half_life, MAX_INTERVAL_DAYS / _RETENTION_INTERVAL)), 6)
    days = max(1, round(half_life * _RETENTION_INTERVAL))
    long_term = datetime.combine(at.astimezone(zone).date() + timedelta(days=days), time.min, zone)
    next_at = long_term if grade == "known" else min(long_term, at + RELEARN_DELAY)
    return {
        "version": 1,
        "difficulty": difficulty,
        "halfLifeDays": half_life,
        "lastRecallAt": iso(at),
        "longTermAt": iso(long_term),
    }, next_at, False
