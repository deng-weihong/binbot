from __future__ import annotations


class TextSegmenter:
    """Turns arbitrary LLM deltas into speakable sentences without losing text."""

    _STRONG_BOUNDARIES = frozenset("。！？!?；;\n")
    _SOFT_BOUNDARIES = frozenset("，,：:")

    def __init__(self, first_sentence_chars: int = 12, max_segment_chars: int = 80):
        self._buffer = ""
        self._first_sentence_chars = first_sentence_chars
        self._max_segment_chars = max_segment_chars
        self._emitted_any = False

    def push(self, delta: str) -> list[str]:
        self._buffer += delta
        segments: list[str] = []
        while True:
            boundary = self._find_boundary()
            if boundary is None:
                break
            segment = self._buffer[:boundary].strip()
            self._buffer = self._buffer[boundary:]
            if segment:
                self._emitted_any = True
                segments.append(segment)
        return segments

    def flush(self) -> str | None:
        value = self._buffer.strip()
        self._buffer = ""
        if value:
            self._emitted_any = True
            return value
        return None

    def _find_boundary(self) -> int | None:
        if not self._buffer:
            return None
        for index, char in enumerate(self._buffer):
            length = index + 1
            if char in self._STRONG_BOUNDARIES:
                return length
            if (
                not self._emitted_any
                and length >= self._first_sentence_chars
                and char in self._SOFT_BOUNDARIES
            ):
                return length
            if length >= self._max_segment_chars:
                return length
        return None


class PCMFramer:
    """Produces exact S16_LE mono frames from arbitrary provider chunks."""

    def __init__(self, frame_samples: int):
        self._frame_bytes = frame_samples * 2
        self._buffer = bytearray()

    def push(self, pcm: bytes) -> list[bytes]:
        if len(pcm) % 2:
            raise ValueError("PCM provider emitted an odd number of bytes")
        self._buffer.extend(pcm)
        frames: list[bytes] = []
        while len(self._buffer) >= self._frame_bytes:
            frames.append(bytes(self._buffer[: self._frame_bytes]))
            del self._buffer[: self._frame_bytes]
        return frames

    def flush(self) -> bytes | None:
        if not self._buffer:
            return None
        self._buffer.extend(b"\x00" * (self._frame_bytes - len(self._buffer)))
        frame = bytes(self._buffer)
        self._buffer.clear()
        return frame
