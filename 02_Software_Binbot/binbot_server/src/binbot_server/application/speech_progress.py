from __future__ import annotations

import unicodedata


def spoken_prefix(text: str, played_ms: int, duration_ms: int | None = None) -> str:
    """Estimate a reading position; the duplex API has no word timestamps.

    Once synthesis ends, use its actual PCM duration. Before then, use a
    conservative speech rate. Punctuation contributes time for pauses.
    Callers keep the cursor monotonic when the estimate is recalibrated.
    """
    if not text or played_ms <= 0:
        return ""

    def weight(char: str) -> float:
        if char in "。！？!?；;\n":
            return 2.0
        if char in "，,、：:":
            return 1.0
        if char.isspace() or unicodedata.category(char).startswith(("S", "P")):
            return 0.1
        return 0.3 if char.isascii() and char.isalpha() else 1.0

    weights = [weight(char) for char in text]
    if duration_ms is not None and duration_ms > 0:
        budget = sum(weights) * min(played_ms / duration_ms, 1.0)
    else:
        budget = played_ms / 220
    consumed = 0.0
    for index, units in enumerate(weights):
        consumed += units
        if consumed > budget:
            return text[:index]
    return text
