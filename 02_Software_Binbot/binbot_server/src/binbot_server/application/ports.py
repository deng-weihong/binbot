from __future__ import annotations

from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass
from typing import Protocol

from binbot_server.domain.model import AudioFormat, ChatMessage, ToolCall, TurnContext
from uuid import UUID


@dataclass(frozen=True, slots=True)
class TranscriptEvent:
    text: str
    final: bool = False


class ASRStream(Protocol):
    async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]: ...

    async def finish(self) -> TranscriptEvent: ...

    async def abort(self) -> None: ...


class ASRProvider(Protocol):
    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream: ...

    async def close(self) -> None: ...


class SpeechEndpointDetector(Protocol):
    """Per-session endpoint detection over PCM and optional streaming ASR text."""

    def reset(self) -> None: ...

    def process(self, pcm: bytes, transcript: str | None = None) -> str | None: ...


class LLMProvider(Protocol):
    def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]: ...

    async def close(self) -> None: ...


@dataclass(frozen=True, slots=True)
class AgentModelResponse:
    text: str = ""
    tool_calls: tuple[ToolCall, ...] = ()


class AgentModel(Protocol):
    async def complete(
        self,
        context: TurnContext,
        messages: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> AgentModelResponse: ...


class TTSProvider(Protocol):
    def stream(self, context: TurnContext, text: str, sample_rate: int) -> AsyncIterator[bytes]: ...

    async def close(self) -> None: ...


class ConversationStore(Protocol):
    """Stores bounded conversation history independently of a connection."""

    async def load(self, device_id: str) -> Sequence[ChatMessage]: ...

    async def load_device_history(self, device_id: str) -> Sequence[ChatMessage]: ...

    async def append_turn(
        self, device_id: str, user_text: str, assistant_text: str, *, source: str = ""
    ) -> Sequence[ChatMessage]: ...

    async def list_conversations(
        self, prefix: str = "", limit: int = 50
    ) -> Sequence["ConversationSummary"]: ...

    async def close(self) -> None: ...


@dataclass(frozen=True, slots=True)
class ConversationSummary:
    """Small index entry used by channel workspaces."""

    conversation_key: str
    message_count: int
    updated_at: str

    def snapshot(self) -> dict[str, object]:
        return {
            "conversation_key": self.conversation_key,
            "message_count": self.message_count,
            "updated_at": self.updated_at,
        }


@dataclass(frozen=True, slots=True)
class MemoryItem:
    memory_id: UUID
    device_id: str
    content: str
    tags: tuple[str, ...]
    created_at: str


class MemoryStore(Protocol):
    async def remember(
        self, device_id: str, content: str, tags: Sequence[str] = ()
    ) -> MemoryItem: ...

    async def search(
        self, device_id: str, query: str, limit: int = 5
    ) -> Sequence[MemoryItem]: ...

    async def forget(self, device_id: str, memory_id: UUID) -> bool: ...

    async def close(self) -> None: ...


@dataclass(frozen=True, slots=True)
class TaskItem:
    task_id: UUID
    device_id: str
    name: str
    payload: dict[str, object]
    run_at: str
    status: str
    created_at: str
    updated_at: str
    error: str = ""

    def snapshot(self) -> dict[str, object]:
        return {
            "task_id": str(self.task_id),
            "device_id": self.device_id,
            "name": self.name,
            "payload": dict(self.payload),
            "run_at": self.run_at,
            "status": self.status,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "error": self.error,
        }


class TaskStore(Protocol):
    async def create(
        self,
        device_id: str,
        name: str,
        payload: dict[str, object],
        run_at: str,
    ) -> TaskItem: ...

    async def list(
        self,
        device_id: str,
        limit: int = 50,
        statuses: Sequence[str] = (),
    ) -> Sequence[TaskItem]: ...

    async def get(self, device_id: str, task_id: UUID) -> TaskItem | None: ...

    async def cancel(self, device_id: str, task_id: UUID) -> bool: ...

    async def claim_due(self, now: str, limit: int = 10) -> Sequence[TaskItem]: ...

    async def finish(self, task_id: UUID, success: bool, error: str = "") -> None: ...

    async def close(self) -> None: ...


class AudioDecoder(Protocol):
    def decode(self, packet: bytes) -> bytes: ...

    def reset(self) -> None: ...

    def close(self) -> None: ...


class AudioEncoder(Protocol):
    def encode(self, pcm_frame: bytes) -> bytes: ...

    def reset(self) -> None: ...

    def close(self) -> None: ...


@dataclass(frozen=True, slots=True)
class OutboundAudioFrame:
    stream_id: str
    sequence: int
    timestamp_ms: int
    payload: bytes


class EventSink(Protocol):
    async def send_control(self, event: dict[str, object]) -> None: ...

    async def send_audio(self, frame: OutboundAudioFrame) -> None: ...


class RealtimeVoiceSession(Protocol):
    async def send_audio(self, pcm: bytes) -> None: ...

    async def set_input_muted(self, muted: bool) -> None: ...

    async def commit_audio(self) -> None: ...

    async def send_event(self, event: dict[str, object]) -> None: ...

    async def receive_event(self) -> dict[str, object]: ...

    async def close(self) -> None: ...


class RealtimeVoiceProvider(Protocol):
    async def open_session(
        self, context: TurnContext, history: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> RealtimeVoiceSession: ...

    async def close(self) -> None: ...


@dataclass(slots=True)
class ProviderBundle:
    asr: ASRProvider
    llm: LLMProvider
    tts: TTSProvider
    realtime: RealtimeVoiceProvider | None = None

    async def close(self) -> None:
        await self.asr.close()
        await self.llm.close()
        await self.tts.close()
        if self.realtime is not None:
            await self.realtime.close()
