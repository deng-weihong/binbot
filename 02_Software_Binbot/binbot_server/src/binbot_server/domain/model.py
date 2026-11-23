from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from uuid import UUID, uuid4

from .errors import StateConflict


class SessionPhase(str, Enum):
    IDLE = "idle"
    LISTENING = "listening"
    THINKING = "thinking"
    SPEAKING = "speaking"
    ERROR = "error"
    CLOSED = "closed"


@dataclass(frozen=True, slots=True)
class AudioFormat:
    codec: str
    sample_rate: int
    channels: int
    frame_ms: int

    @property
    def frame_samples(self) -> int:
        return self.sample_rate * self.frame_ms // 1_000


@dataclass(frozen=True, slots=True)
class TurnContext:
    session_id: UUID
    turn_id: UUID
    device_id: str


@dataclass(frozen=True, slots=True)
class ToolCall:
    call_id: str
    name: str
    arguments: dict[str, object]


@dataclass(frozen=True, slots=True)
class ChatMessage:
    role: str
    content: str
    name: str | None = None
    tool_call_id: str | None = None
    tool_calls: tuple[ToolCall, ...] = ()


def legacy_web_conversation_prefix(device_id: str) -> str:
    return f"web:{len(device_id)}:{device_id}:"


@dataclass(slots=True)
class VoiceSession:
    device_id: str
    uplink: AudioFormat
    downlink: AudioFormat
    max_history_messages: int
    session_id: UUID = field(default_factory=uuid4)
    phase: SessionPhase = SessionPhase.IDLE
    turn_id: UUID | None = None
    uplink_stream_id: UUID | None = None
    generation: int = 0
    history: list[ChatMessage] = field(default_factory=list)

    def start_turn(self, turn_id: UUID, stream_id: UUID | None) -> TurnContext:
        if self.phase == SessionPhase.CLOSED:
            raise StateConflict("cannot start a turn on a closed session")
        self.generation += 1
        self.turn_id = turn_id
        self.uplink_stream_id = stream_id
        self.phase = SessionPhase.LISTENING if stream_id else SessionPhase.THINKING
        return self.context()

    def finish_input(self, turn_id: UUID, stream_id: UUID) -> None:
        self.assert_turn(turn_id)
        if self.phase != SessionPhase.LISTENING:
            raise StateConflict(f"audio cannot end while session is {self.phase.value}")
        if stream_id != self.uplink_stream_id:
            raise StateConflict("audio stream does not belong to the active turn")
        self.phase = SessionPhase.THINKING

    def start_speaking(self, turn_id: UUID) -> None:
        self.assert_turn(turn_id)
        if self.phase not in {SessionPhase.THINKING, SessionPhase.SPEAKING}:
            raise StateConflict(f"cannot speak while session is {self.phase.value}")
        self.phase = SessionPhase.SPEAKING

    def complete_turn(
        self, turn_id: UUID, user_text: str, assistant_text: str
    ) -> None:
        self.assert_turn(turn_id)
        self.history.extend(
            [
                ChatMessage(role="user", content=user_text),
                ChatMessage(role="assistant", content=assistant_text),
            ]
        )
        overflow = len(self.history) - self.max_history_messages
        if overflow > 0:
            del self.history[:overflow]
        self.phase = SessionPhase.IDLE
        self.turn_id = None
        self.uplink_stream_id = None

    def cancel_turn(self, turn_id: UUID | None = None) -> UUID | None:
        if turn_id is not None:
            self.assert_turn(turn_id)
        cancelled = self.turn_id
        self.generation += 1
        self.phase = SessionPhase.IDLE
        self.turn_id = None
        self.uplink_stream_id = None
        return cancelled

    def fail_turn(self, turn_id: UUID) -> None:
        self.assert_turn(turn_id)
        self.phase = SessionPhase.ERROR

    def close(self) -> None:
        self.generation += 1
        self.phase = SessionPhase.CLOSED
        self.turn_id = None
        self.uplink_stream_id = None

    def assert_turn(self, turn_id: UUID) -> None:
        if self.turn_id != turn_id:
            raise StateConflict("message belongs to a stale or unknown turn")

    def context(self) -> TurnContext:
        if self.turn_id is None:
            raise StateConflict("session has no active turn")
        return TurnContext(self.session_id, self.turn_id, self.device_id)
