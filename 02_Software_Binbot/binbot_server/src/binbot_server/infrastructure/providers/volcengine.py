from __future__ import annotations

import asyncio
import gzip
import json
import logging
from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass
from enum import IntEnum
from struct import pack, unpack
from uuid import uuid4

import aiohttp

from binbot_server.application.ports import ASRStream, TranscriptEvent
from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import AudioFormat, TurnContext


class VolcMessageType(IntEnum):
    FULL_CLIENT = 1
    AUDIO_CLIENT = 2
    FULL_SERVER = 9
    AUDIO_SERVER = 11
    ERROR = 15


class VolcMessageFlag(IntEnum):
    NO_SEQUENCE = 0
    POSITIVE_SEQUENCE = 1
    LAST_NO_SEQUENCE = 2
    NEGATIVE_SEQUENCE = 3
    WITH_EVENT = 4


class VolcEvent(IntEnum):
    START_CONNECTION = 1
    FINISH_CONNECTION = 2
    CONNECTION_STARTED = 50
    CONNECTION_FAILED = 51
    CONNECTION_FINISHED = 52
    START_SESSION = 100
    CANCEL_SESSION = 101
    FINISH_SESSION = 102
    SESSION_STARTED = 150
    SESSION_CANCELED = 151
    SESSION_FINISHED = 152
    SESSION_FAILED = 153
    TASK_REQUEST = 200
    TTS_SENTENCE_START = 350
    TTS_SENTENCE_END = 351
    TTS_RESPONSE = 352


@dataclass(frozen=True, slots=True)
class VolcMessage:
    """Codec for the provider's four-byte-header binary protocol.

    The codec deliberately has no WebSocket dependency, so malformed provider
    frames can be tested without a network and the device BBVP remains separate.
    """

    message_type: VolcMessageType
    flag: VolcMessageFlag = VolcMessageFlag.NO_SEQUENCE
    serialization: int = 0
    compression: int = 0
    event: int | None = None
    session_id: str = ""
    connect_id: str = ""
    sequence: int = 0
    error_code: int = 0
    payload: bytes = b""

    def encode(self) -> bytes:
        if not 0 <= self.serialization <= 15 or not 0 <= self.compression <= 15:
            raise ValueError("invalid serialization or compression nibble")
        body = bytearray()
        if self.flag in (VolcMessageFlag.POSITIVE_SEQUENCE, VolcMessageFlag.NEGATIVE_SEQUENCE):
            body.extend(pack(">i", self.sequence))
        if self.message_type == VolcMessageType.ERROR:
            body.extend(pack(">I", self.error_code))
        if self.flag == VolcMessageFlag.WITH_EVENT:
            if self.event is None:
                raise ValueError("event flag requires an event")
            body.extend(pack(">i", self.event))
            if self.event not in {
                VolcEvent.START_CONNECTION,
                VolcEvent.FINISH_CONNECTION,
                VolcEvent.CONNECTION_STARTED,
                VolcEvent.CONNECTION_FAILED,
                VolcEvent.CONNECTION_FINISHED,
            }:
                encoded_session = self.session_id.encode("utf-8")
                body.extend(pack(">I", len(encoded_session)))
                body.extend(encoded_session)
            if self.event in {
                VolcEvent.CONNECTION_STARTED,
                VolcEvent.CONNECTION_FAILED,
                VolcEvent.CONNECTION_FINISHED,
            }:
                encoded_connect = self.connect_id.encode("utf-8")
                body.extend(pack(">I", len(encoded_connect)))
                body.extend(encoded_connect)
        body.extend(pack(">I", len(self.payload)))
        body.extend(self.payload)
        header = bytes((0x11, (int(self.message_type) << 4) | int(self.flag),
                        (self.serialization << 4) | self.compression, 0))
        return header + body

    @classmethod
    def decode(cls, data: bytes, *, max_payload: int = 4 * 1024 * 1024) -> "VolcMessage":
        if len(data) < 8:
            raise ValueError("provider frame is too short")
        header_size = (data[0] & 0x0F) * 4
        if data[0] >> 4 != 1 or header_size < 4 or header_size > len(data):
            raise ValueError("unsupported provider protocol header")
        try:
            message_type = VolcMessageType(data[1] >> 4)
            flag = VolcMessageFlag(data[1] & 0x0F)
        except ValueError as exc:
            raise ValueError("unknown provider message type or flags") from exc
        serialization, compression = data[2] >> 4, data[2] & 0x0F
        offset = header_size
        sequence = 0
        if flag in (VolcMessageFlag.POSITIVE_SEQUENCE, VolcMessageFlag.NEGATIVE_SEQUENCE):
            if offset + 4 > len(data):
                raise ValueError("provider sequence is truncated")
            sequence = unpack(">i", data[offset : offset + 4])[0]
            offset += 4
        error_code = 0
        if message_type == VolcMessageType.ERROR:
            if offset + 4 > len(data):
                raise ValueError("provider error code is truncated")
            error_code = unpack(">I", data[offset : offset + 4])[0]
            offset += 4
        event: int | None = None
        session_id = ""
        connect_id = ""
        if flag == VolcMessageFlag.WITH_EVENT:
            if offset + 4 > len(data):
                raise ValueError("provider event is truncated")
            event = unpack(">i", data[offset : offset + 4])[0]
            offset += 4
            if event not in {
                VolcEvent.START_CONNECTION,
                VolcEvent.FINISH_CONNECTION,
                VolcEvent.CONNECTION_STARTED,
                VolcEvent.CONNECTION_FAILED,
                VolcEvent.CONNECTION_FINISHED,
            }:
                session_id, offset = _read_string(data, offset)
            if event in {
                VolcEvent.CONNECTION_STARTED,
                VolcEvent.CONNECTION_FAILED,
                VolcEvent.CONNECTION_FINISHED,
            }:
                connect_id, offset = _read_string(data, offset)
        if offset + 4 > len(data):
            raise ValueError("provider payload size is truncated")
        payload_size = unpack(">I", data[offset : offset + 4])[0]
        offset += 4
        if payload_size > max_payload or payload_size != len(data) - offset:
            raise ValueError("provider payload size is invalid")
        return cls(
            message_type=message_type,
            flag=flag,
            serialization=serialization,
            compression=compression,
            event=event,
            session_id=session_id,
            connect_id=connect_id,
            sequence=sequence,
            error_code=error_code,
            payload=data[offset:],
        )


def _read_string(data: bytes, offset: int) -> tuple[str, int]:
    if offset + 4 > len(data):
        raise ValueError("provider string length is truncated")
    size = unpack(">I", data[offset : offset + 4])[0]
    offset += 4
    if size > len(data) - offset:
        raise ValueError("provider string is truncated")
    try:
        value = data[offset : offset + size].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("provider string is not UTF-8") from exc
    return value, offset + size


def _payload(message: VolcMessage) -> bytes:
    if message.compression == 0:
        return message.payload
    if message.compression == 1:
        try:
            return gzip.decompress(message.payload)
        except OSError as exc:
            raise ProviderFailure("provider returned invalid gzip payload") from exc
    raise ProviderFailure("provider returned unsupported compression")


def _json_payload(value: object, *, compress: bool = True) -> tuple[bytes, int]:
    raw = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return (gzip.compress(raw), 1) if compress else (raw, 0)


def _response_text(message: VolcMessage) -> str:
    try:
        value = json.loads(_payload(message).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProviderFailure("ASR returned malformed JSON") from exc
    result = value.get("result", {}) if isinstance(value, dict) else {}
    if isinstance(result, list):
        result = result[-1] if result else {}
    if isinstance(result, dict):
        text = result.get("text", "")
    else:
        text = ""
    return str(text).strip() if text is not None else ""


class _VolcengineASRStream:
    def __init__(
        self,
        provider: "VolcengineASRProvider",
        websocket: aiohttp.ClientWebSocketResponse,
        sample_rate: int,
    ):
        self._provider = provider
        self._websocket = websocket
        self._sample_rate = sample_rate
        self._events: asyncio.Queue[TranscriptEvent | Exception] = asyncio.Queue(
            maxsize=provider.result_queue_size
        )
        self._reader_task = asyncio.create_task(self._read_loop())
        self._buffer = bytearray()
        self._last_chunk: bytes | None = None
        self._audio_bytes = 0
        # The full client request consumes sequence 1; audio starts at 2.
        self._sequence = 2
        self._closed = False
        self._final_seen = False
        self._final_event: TranscriptEvent | None = None
        self._error: Exception | None = None

    async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]:
        if self._closed:
            return ()
        if len(pcm) & 1:
            raise ProviderFailure("ASR PCM payload must contain complete S16 samples")
        self._audio_bytes += len(pcm)
        if self._audio_bytes > self._provider.max_audio_bytes:
            raise ProviderFailure("ASR audio exceeded the configured turn limit")
        self._buffer.extend(pcm)
        frame_bytes = self._provider.packet_ms * self._sample_rate * 2 // 1000
        while len(self._buffer) >= frame_bytes:
            chunk = bytes(self._buffer[:frame_bytes])
            del self._buffer[:frame_bytes]
            if self._last_chunk is not None:
                await self._send_audio(self._last_chunk, final=False)
            self._last_chunk = chunk
        return self._drain_events()

    async def finish(self) -> TranscriptEvent:
        if self._closed:
            raise ProviderFailure("ASR stream is already closed")
        if self._buffer:
            if self._last_chunk is not None:
                await self._send_audio(self._last_chunk, final=False)
            self._last_chunk = bytes(self._buffer)
            self._buffer.clear()
        if self._last_chunk is None:
            await self._send_audio(b"", final=True)
        else:
            await self._send_audio(self._last_chunk, final=True)
            self._last_chunk = None
        try:
            if self._final_event is not None:
                return self._final_event
            async with asyncio.timeout(self._provider.request_timeout_seconds):
                while True:
                    item = await self._events.get()
                    if isinstance(item, Exception):
                        raise item
                    if item.final:
                        self._final_seen = True
                        return item
        except TimeoutError as exc:
            raise ProviderFailure("ASR provider timed out waiting for final result") from exc
        finally:
            await self._close()

    async def abort(self) -> None:
        await self._close()

    def _drain_events(self) -> tuple[TranscriptEvent, ...]:
        output: list[TranscriptEvent] = []
        while True:
            try:
                item = self._events.get_nowait()
            except asyncio.QueueEmpty:
                return tuple(output)
            if isinstance(item, Exception):
                raise item
            if item.final:
                self._final_event = item
                continue
            if item.text:
                output.append(item)

    async def _send_audio(self, pcm: bytes, *, final: bool) -> None:
        if self._error is not None:
            raise self._error
        compressed = gzip.compress(pcm)
        sequence = -self._sequence if final else self._sequence
        message = VolcMessage(
            message_type=VolcMessageType.AUDIO_CLIENT,
            flag=(
                VolcMessageFlag.NEGATIVE_SEQUENCE
                if final
                else VolcMessageFlag.POSITIVE_SEQUENCE
            ),
            serialization=0,
            compression=1,
            sequence=sequence,
            payload=compressed,
        )
        try:
            async with asyncio.timeout(self._provider.request_timeout_seconds):
                await self._websocket.send_bytes(message.encode())
        except (aiohttp.ClientError, asyncio.TimeoutError, ConnectionError) as exc:
            if self._error is not None:
                raise self._error from exc
            raise ProviderFailure("ASR provider disconnected while sending audio") from exc
        if not final:
            self._sequence += 1

    async def _read_loop(self) -> None:
        try:
            async for incoming in self._websocket:
                if incoming.type == aiohttp.WSMsgType.BINARY:
                    try:
                        message = VolcMessage.decode(
                            incoming.data, max_payload=self._provider.max_message_bytes
                        )
                        if message.message_type == VolcMessageType.ERROR:
                            raise ProviderFailure(
                                f"ASR provider error {message.error_code}: "
                                f"{_payload(message)[:300]!r}"
                            )
                        if message.message_type != VolcMessageType.FULL_SERVER:
                            continue
                        text = _response_text(message)
                        self._enqueue(TranscriptEvent(text=text, final=message.flag in {
                            VolcMessageFlag.LAST_NO_SEQUENCE,
                            VolcMessageFlag.NEGATIVE_SEQUENCE,
                        }))
                    except (ValueError, ProviderFailure) as exc:
                        self._enqueue(exc)
                        return
                elif incoming.type in {aiohttp.WSMsgType.ERROR, aiohttp.WSMsgType.CLOSED}:
                    self._enqueue(ProviderFailure("ASR provider WebSocket closed"))
                    return
        except (aiohttp.ClientError, asyncio.CancelledError):
            if not self._closed:
                self._enqueue(ProviderFailure("ASR provider WebSocket failed"))

    def _enqueue(self, item: TranscriptEvent | Exception) -> None:
        if isinstance(item, Exception):
            self._error = item
        if self._events.full():
            try:
                self._events.get_nowait()
            except asyncio.QueueEmpty:
                pass
        self._events.put_nowait(item)

    async def _close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if not self._reader_task.done():
            self._reader_task.cancel()
            await asyncio.gather(self._reader_task, return_exceptions=True)
        if not self._websocket.closed:
            await self._websocket.close()
        self._provider.release_stream(self)


class VolcengineASRProvider:
    def __init__(
        self,
        endpoint: str,
        api_key: str,
        resource_id: str,
        packet_ms: int = 200,
        request_timeout_seconds: float = 60.0,
        max_audio_seconds: float = 60.0,
        max_message_bytes: int = 4 * 1024 * 1024,
        result_queue_size: int = 64,
    ):
        self.endpoint = endpoint
        self.api_key = api_key
        self.resource_id = resource_id
        self.packet_ms = packet_ms
        self.request_timeout_seconds = request_timeout_seconds
        self.max_audio_bytes = int(max_audio_seconds * 16_000 * 2)
        self.max_message_bytes = max_message_bytes
        self.result_queue_size = result_queue_size
        self._session: aiohttp.ClientSession | None = None
        self._streams: set[_VolcengineASRStream] = set()

    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream:
        if audio_format.sample_rate != 16_000 or audio_format.channels != 1:
            raise ProviderFailure("Volcengine ASR requires 16 kHz mono PCM")
        session = self._get_session()
        headers = {
            "X-Api-Key": self.api_key,
            "X-Api-Resource-Id": self.resource_id,
            "X-Api-Request-Id": str(uuid4()),
        }
        try:
            websocket = await session.ws_connect(
                self.endpoint,
                headers=headers,
                max_msg_size=self.max_message_bytes,
                heartbeat=20,
            )
            payload, compression = _json_payload({
                "user": {"uid": context.device_id},
                "audio": {
                    "format": "pcm", "codec": "raw", "rate": 16_000,
                    "bits": 16, "channel": 1,
                },
                "request": {
                    "model_name": "bigmodel", "enable_itn": True,
                    "enable_punc": True, "result_type": "full",
                    "enable_nonstream": False,
                },
            })
            await websocket.send_bytes(VolcMessage(
                message_type=VolcMessageType.FULL_CLIENT,
                flag=VolcMessageFlag.POSITIVE_SEQUENCE,
                serialization=1,
                compression=compression,
                sequence=1,
                payload=payload,
            ).encode())
        except (aiohttp.ClientError, asyncio.TimeoutError) as exc:
            raise ProviderFailure("ASR provider connection failed") from exc
        stream = _VolcengineASRStream(self, websocket, audio_format.sample_rate)
        self._streams.add(stream)
        return stream

    async def close(self) -> None:
        streams = tuple(self._streams)
        for stream in streams:
            await stream.abort()
        self._streams.clear()
        if self._session is not None:
            await self._session.close()
            self._session = None

    def release_stream(self, stream: _VolcengineASRStream) -> None:
        self._streams.discard(stream)

    def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session


class VolcengineTTSProvider:
    def __init__(
        self,
        endpoint: str,
        api_key: str,
        resource_id: str,
        model: str,
        speaker: str,
        request_timeout_seconds: float = 60.0,
        max_message_bytes: int = 4 * 1024 * 1024,
    ):
        self.endpoint = endpoint
        self.api_key = api_key
        self.resource_id = resource_id
        self.model = model
        self.speaker = speaker
        self.request_timeout_seconds = request_timeout_seconds
        self.max_message_bytes = max_message_bytes
        self._session: aiohttp.ClientSession | None = None
        self._log = logging.getLogger(__name__)

    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        if sample_rate != 24_000:
            raise ProviderFailure("Volcengine TTS requires 24 kHz PCM output")
        if not text.strip():
            return
        if not self.speaker:
            raise ProviderFailure("Volcengine TTS speaker is not configured")
        websocket: aiohttp.ClientWebSocketResponse | None = None
        session_id = str(uuid4())
        emitted = False
        finished = False
        try:
            session = self._get_session()
            websocket = await session.ws_connect(
                self.endpoint,
                headers={
                    "X-Api-Key": self.api_key,
                    "X-Api-Resource-Id": self.resource_id,
                    "X-Api-Request-Id": str(uuid4()),
                },
                max_msg_size=self.max_message_bytes,
                heartbeat=20,
            )
            request = {
                "user": {"uid": context.device_id},
                "req_params": {
                    "text": text,
                    "speaker": self.speaker,
                    "model": self.model,
                    "audio_params": {"format": "pcm", "sample_rate": 24_000},
                },
            }
            payload, _compression = _json_payload(request, compress=False)
            await websocket.send_bytes(VolcMessage(
                message_type=VolcMessageType.FULL_CLIENT,
                flag=VolcMessageFlag.WITH_EVENT,
                serialization=1,
                event=VolcEvent.START_SESSION,
                session_id=session_id,
                payload=payload,
            ).encode())
            async with asyncio.timeout(self.request_timeout_seconds):
                async for incoming in websocket:
                    if incoming.type != aiohttp.WSMsgType.BINARY:
                        if incoming.type in {aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR}:
                            raise ProviderFailure("TTS provider WebSocket closed")
                        continue
                    message = VolcMessage.decode(
                        incoming.data, max_payload=self.max_message_bytes
                    )
                    if message.message_type == VolcMessageType.ERROR:
                        raise ProviderFailure(
                            f"TTS provider error {message.error_code}: "
                            f"{_payload(message)[:300]!r}"
                        )
                    event = message.event
                    if message.message_type == VolcMessageType.AUDIO_SERVER:
                        audio = _payload(message)
                        if audio:
                            if len(audio) & 1:
                                raise ProviderFailure("TTS provider returned odd-sized PCM")
                            emitted = True
                            yield audio
                    elif event == VolcEvent.SESSION_FAILED:
                        raise ProviderFailure("TTS provider session failed")
                    elif event == VolcEvent.SESSION_FINISHED:
                        finished = True
                        break
            if not finished:
                raise ProviderFailure("TTS provider ended without SessionFinished")
            if not emitted:
                raise ProviderFailure("TTS provider returned empty audio")
        except asyncio.TimeoutError as exc:
            raise ProviderFailure("TTS provider timed out") from exc
        except aiohttp.ClientError as exc:
            raise ProviderFailure("TTS provider connection failed") from exc
        finally:
            if websocket is not None and not websocket.closed:
                try:
                    if not finished:
                        await websocket.send_bytes(
                            VolcMessage(
                                message_type=VolcMessageType.FULL_CLIENT,
                                flag=VolcMessageFlag.WITH_EVENT,
                                event=VolcEvent.CANCEL_SESSION,
                                session_id=session_id,
                                payload=b"{}",
                            ).encode()
                        )
                    await websocket.close()
                except aiohttp.ClientError:
                    self._log.debug("TTS WebSocket close failed", exc_info=True)

    async def close(self) -> None:
        if self._session is not None:
            await self._session.close()
            self._session = None

    def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session


__all__ = [
    "VolcEvent",
    "VolcMessage",
    "VolcMessageFlag",
    "VolcMessageType",
    "VolcengineASRProvider",
    "VolcengineTTSProvider",
]
