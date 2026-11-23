from __future__ import annotations

import asyncio
import math
from array import array
from collections.abc import AsyncIterator, Sequence

from binbot_server.application.ports import ASRStream, AgentModelResponse, TranscriptEvent
from binbot_server.domain.model import AudioFormat, ChatMessage, TurnContext


class _MockASRStream:
    def __init__(self, transcript: str):
        self._transcript = transcript
        self._frames = 0
        self._closed = False

    async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]:
        if self._closed:
            return ()
        if pcm:
            self._frames += 1
        if self._frames == 10:
            return (TranscriptEvent(self._transcript[: max(1, len(self._transcript) // 2)]),)
        return ()

    async def finish(self) -> TranscriptEvent:
        self._closed = True
        await asyncio.sleep(0)
        return TranscriptEvent(self._transcript, final=True)

    async def abort(self) -> None:
        self._closed = True


class MockASRProvider:
    def __init__(self, transcript: str):
        self._transcript = transcript

    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream:
        del context, audio_format
        return _MockASRStream(self._transcript)

    async def close(self) -> None:
        return None


class MockLLMProvider:
    def __init__(self, response: str, chunk_chars: int = 6):
        self._response = response
        self._chunk_chars = chunk_chars

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context, messages
        for offset in range(0, len(self._response), self._chunk_chars):
            await asyncio.sleep(0)
            yield self._response[offset : offset + self._chunk_chars]

    async def complete(
        self,
        context: TurnContext,
        messages: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> AgentModelResponse:
        del context, messages, tools
        return AgentModelResponse(text=self._response)

    async def close(self) -> None:
        return None


class MockTTSProvider:
    """Produces a deterministic tone so the whole audio path can be tested without a cloud."""

    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        del context
        duration_ms = min(2_500, max(240, len(text) * 55))
        total_samples = sample_rate * duration_ms // 1_000
        chunk_samples = sample_rate // 20  # 50 ms provider chunks
        amplitude = 3_000
        frequency = 660.0
        for start in range(0, total_samples, chunk_samples):
            count = min(chunk_samples, total_samples - start)
            samples = array(
                "h",
                (
                    int(amplitude * math.sin(2.0 * math.pi * frequency * (start + i) / sample_rate))
                    for i in range(count)
                ),
            )
            await asyncio.sleep(0)
            yield samples.tobytes()

    async def close(self) -> None:
        return None
