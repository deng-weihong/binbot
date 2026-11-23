from __future__ import annotations

import json
import struct
from enum import IntEnum
from typing import Annotated, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from binbot_server.domain.errors import ProtocolViolation

PROTOCOL_VERSION = 1
AUDIO_MAGIC = b"BBV1"
AUDIO_HEADER = struct.Struct("!4sBBH16sIQI")
AUDIO_HEADER_SIZE = AUDIO_HEADER.size


class AudioKind(IntEnum):
    UPLINK_OPUS = 1
    DOWNLINK_OPUS = 2


class StrictMessage(BaseModel):
    model_config = ConfigDict(extra="forbid")


class AudioSpec(StrictMessage):
    codec: Literal["opus"]
    sample_rate: Literal[8000, 12000, 16000, 24000, 48000]
    channels: Literal[1]
    frame_ms: Literal[10, 20, 40, 60]


class CameraStreamSpec(StrictMessage):
    transport: Literal["rtsp"]
    port: Annotated[int, Field(ge=1, le=65_535)] = 554
    path: Annotated[
        str,
        Field(
            min_length=1,
            max_length=128,
            pattern=r"^/[A-Za-z0-9._~!$&'()*+,;=:@%/-]+$",
        ),
    ]


class HelloMessage(StrictMessage):
    type: Literal["hello"]
    protocol_version: Literal[1]
    device_id: Annotated[
        str,
        Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$"),
    ]
    client_version: Annotated[str, Field(min_length=1, max_length=32)]
    token: Annotated[str, Field(max_length=512)] = ""
    uplink: AudioSpec
    downlink: AudioSpec
    capabilities: set[str] = Field(default_factory=set)
    camera: CameraStreamSpec | None = None


class TurnStartMessage(StrictMessage):
    type: Literal["turn.start"]
    turn_id: UUID
    stream_id: UUID
    wake_word: Annotated[str, Field(max_length=64)] = ""


class AudioEndMessage(StrictMessage):
    type: Literal["audio.end"]
    turn_id: UUID
    stream_id: UUID


class TurnCancelMessage(StrictMessage):
    type: Literal["turn.cancel"]
    turn_id: UUID
    reason: Annotated[str, Field(max_length=64)] = "client_request"


class TextInputMessage(StrictMessage):
    type: Literal["text.input"]
    turn_id: UUID
    text: Annotated[str, Field(min_length=1, max_length=8_192)]


class AgentResponseMessage(StrictMessage):
    type: Literal["agent.response"]
    turn_id: UUID
    text: Annotated[str, Field(min_length=1, max_length=8_192)]


class PingMessage(StrictMessage):
    type: Literal["ping"]
    nonce: Annotated[str, Field(max_length=128)] = ""


class DeviceCommandResultMessage(StrictMessage):
    type: Literal["device.command.result"]
    command_id: UUID
    success: bool
    message: Annotated[str, Field(max_length=256)] = ""


class ComponentInfo(StrictMessage):
    profile: Literal["round_screen", "holographic_screen", "showcase_turntable", "slave_mcu", "ambient_light"]
    state: Annotated[int, Field(ge=0, le=4)]
    healthy: bool
    boot_id: Annotated[str, Field(pattern=r"^[0-9a-f]{16}$")]
    capacity: Annotated[int, Field(ge=1, le=16 * 1024 * 1024)]
    version: Annotated[str, Field(max_length=31)]
    project: Annotated[str, Field(max_length=31)]
    elf_sha256: Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")]


class ComponentEventMessage(StrictMessage):
    type: Literal["component.event"]
    request_id: UUID
    component_id: Literal["component-1", "slave-1"]
    event_seq: Annotated[int, Field(ge=1)]
    status: Literal["probe", "preparing", "downloading", "transferring", "validating",
                    "activating", "verifying", "reconciling", "succeeded", "failed", "cancelled"]
    bytes_written: Annotated[int, Field(ge=0)] = 0
    total_bytes: Annotated[int, Field(ge=0)] = 0
    info: ComponentInfo | None = None
    message: Annotated[str, Field(max_length=256)] = ""


class DeviceTodoState(StrictMessage):
    text: Annotated[str, Field(min_length=1, max_length=72)]
    done: bool = False


class DeviceMusicState(StrictMessage):
    track_index: Annotated[int, Field(ge=0, le=255)] = 0
    position_seconds: Annotated[int, Field(ge=0, le=86_400)] = 0
    active: bool = False
    playing: bool = False


class DeviceDisplayState(StrictMessage):
    gif: Annotated[str, Field(max_length=256)] = ""


class DeviceStateMessage(StrictMessage):
    type: Literal["device.state"]
    revision: Annotated[int, Field(ge=0, le=4_294_967_295)]
    todos: Annotated[list[DeviceTodoState], Field(max_length=8)] = Field(
        default_factory=list
    )
    music: DeviceMusicState = Field(default_factory=DeviceMusicState)
    display: DeviceDisplayState = Field(default_factory=DeviceDisplayState)


class DeviceCalendarEvent(StrictMessage):
    event_id: Annotated[str, Field(min_length=1, max_length=256)]
    summary: Annotated[str, Field(max_length=1_000)] = ""
    start_timestamp: Annotated[str, Field(max_length=32)] = ""
    end_timestamp: Annotated[str, Field(max_length=32)] = ""
    start_date: Annotated[str, Field(max_length=16)] = ""
    end_date: Annotated[str, Field(max_length=16)] = ""
    timezone: Annotated[str, Field(max_length=64)] = "Asia/Shanghai"
    location: Annotated[str, Field(max_length=512)] = ""
    meeting_url: Annotated[str, Field(max_length=2_000)] = ""
    status: Annotated[str, Field(max_length=32)] = "confirmed"


class DeviceCalendarMessage(StrictMessage):
    type: Literal["device.calendar"]
    calendar_id: Annotated[str, Field(max_length=256)] = ""
    revision: Annotated[int, Field(ge=0, le=4_294_967_295)] = 0
    events: Annotated[list[DeviceCalendarEvent], Field(max_length=16)] = Field(
        default_factory=list
    )


InboundControl = (
    TurnStartMessage
    | AudioEndMessage
    | TurnCancelMessage
    | TextInputMessage
    | AgentResponseMessage
    | PingMessage
    | DeviceCommandResultMessage
    | DeviceStateMessage
    | ComponentEventMessage
)

_CONTROL_TYPES: dict[str, type[StrictMessage]] = {
    "turn.start": TurnStartMessage,
    "audio.end": AudioEndMessage,
    "turn.cancel": TurnCancelMessage,
    "text.input": TextInputMessage,
    "agent.response": AgentResponseMessage,
    "ping": PingMessage,
    "device.command.result": DeviceCommandResultMessage,
    "device.state": DeviceStateMessage,
    "component.event": ComponentEventMessage,
}


def _json_object(raw: str | bytes, max_bytes: int) -> dict[str, object]:
    size = len(raw.encode("utf-8")) if isinstance(raw, str) else len(raw)
    if size > max_bytes:
        raise ProtocolViolation("control message exceeds configured size limit")
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ProtocolViolation("control message is not valid UTF-8 JSON") from exc
    if not isinstance(value, dict):
        raise ProtocolViolation("control message must be a JSON object")
    return value


def parse_hello(raw: str | bytes, max_bytes: int) -> HelloMessage:
    try:
        return HelloMessage.model_validate(_json_object(raw, max_bytes))
    except ValidationError as exc:
        details = exc.errors(include_url=False)
        raise ProtocolViolation(f"invalid hello message: {details}") from exc


def parse_control(raw: str | bytes, max_bytes: int) -> InboundControl:
    value = _json_object(raw, max_bytes)
    message_type = value.get("type")
    model = _CONTROL_TYPES.get(message_type) if isinstance(message_type, str) else None
    if model is None:
        raise ProtocolViolation(f"unsupported control message type: {message_type!r}")
    try:
        return model.model_validate(value)  # type: ignore[return-value]
    except ValidationError as exc:
        details = exc.errors(include_url=False)
        raise ProtocolViolation(f"invalid {message_type} message: {details}") from exc


class AudioFrame:
    __slots__ = ("kind", "stream_id", "sequence", "timestamp_ms", "payload")

    def __init__(
        self,
        kind: AudioKind,
        stream_id: UUID,
        sequence: int,
        timestamp_ms: int,
        payload: bytes,
    ):
        self.kind = kind
        self.stream_id = stream_id
        self.sequence = sequence
        self.timestamp_ms = timestamp_ms
        self.payload = payload


def decode_audio_frame(raw: bytes, max_payload_bytes: int) -> AudioFrame:
    if len(raw) < AUDIO_HEADER_SIZE:
        raise ProtocolViolation("binary audio frame is shorter than its header")
    magic, version, kind_value, flags, stream_bytes, sequence, timestamp_ms, size = (
        AUDIO_HEADER.unpack_from(raw)
    )
    if magic != AUDIO_MAGIC or version != PROTOCOL_VERSION:
        raise ProtocolViolation("binary audio frame has an unsupported magic or version")
    if flags != 0:
        raise ProtocolViolation("binary audio frame uses unsupported flags")
    try:
        kind = AudioKind(kind_value)
    except ValueError as exc:
        raise ProtocolViolation("binary audio frame has an unsupported kind") from exc
    if size > max_payload_bytes:
        raise ProtocolViolation("binary audio payload exceeds configured size limit")
    if len(raw) != AUDIO_HEADER_SIZE + size:
        raise ProtocolViolation("binary audio payload size does not match its header")
    return AudioFrame(
        kind,
        UUID(bytes=stream_bytes),
        sequence,
        timestamp_ms,
        raw[AUDIO_HEADER_SIZE:],
    )


def encode_audio_frame(frame: AudioFrame) -> bytes:
    payload = bytes(frame.payload)
    return AUDIO_HEADER.pack(
        AUDIO_MAGIC,
        PROTOCOL_VERSION,
        int(frame.kind),
        0,
        frame.stream_id.bytes,
        frame.sequence,
        frame.timestamp_ms,
        len(payload),
    ) + payload
