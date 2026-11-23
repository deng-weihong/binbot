from __future__ import annotations

import asyncio
import base64
import json
import logging
from collections.abc import Sequence
from uuid import uuid4

import aiohttp

from binbot_server.application.ports import RealtimeVoiceSession
from binbot_server.config import SeeduplexConfig
from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import ChatMessage, TurnContext

_LOG = logging.getLogger("binbot.seeduplex")
_COMMIT = object()
_MUTE = object()


class SeeduplexProvider:
    def __init__(self, config: SeeduplexConfig, api_key: str, instructions: str):
        self.config = config
        self._api_key = api_key
        self._instructions = instructions
        self._sessions: set[SeeduplexSession] = set()

    async def open_session(
        self, context: TurnContext, history: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> RealtimeVoiceSession:
        session = SeeduplexSession(self.config, self._api_key, self._sessions)
        self._sessions.add(session)
        try:
            async with asyncio.timeout(self.config.connect_timeout_seconds):
                await session.connect(context, self._instructions, history, tools)
        except BaseException:
            await session.close()
            raise
        return session

    async def close(self) -> None:
        await asyncio.gather(*(session.close() for session in tuple(self._sessions)))


class SeeduplexSession:
    def __init__(
        self, config: SeeduplexConfig, api_key: str, sessions: set[SeeduplexSession],
    ):
        self._config = config
        self._api_key = api_key
        self._sessions = sessions
        self._http: aiohttp.ClientSession | None = None
        self._ws: aiohttp.ClientWebSocketResponse | None = None
        self._write_lock = asyncio.Lock()
        self._close_lock = asyncio.Lock()
        self._uplink: asyncio.Queue[tuple[int, bytes] | object] = asyncio.Queue(maxsize=100)
        self._input_muted = False
        self._input_epoch = 0
        self._sender: asyncio.Task[None] | None = None
        self._created = False
        self._closed = False
        self.log_id = ""

    async def connect(
        self, context: TurnContext, instructions: str, history: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> None:
        self._http = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=None))
        try:
            self._ws = await self._http.ws_connect(
                self._config.endpoint, headers={"X-Api-Key": self._api_key},
                heartbeat=30, max_msg_size=self._config.max_message_bytes, compress=0,
            )
        except aiohttp.WSServerHandshakeError as exc:
            raise ProviderFailure(
                f"Seeduplex authentication/handshake failed (HTTP {exc.status})"
            ) from exc
        self.log_id = self._ws._response.headers.get("X-Tt-Logid", "")
        await self.send_event({
            "type": "session.create",
            "session": {
                "id": str(context.turn_id), "model": self._config.model,
                "instructions": instructions,
                "audio": {
                    "input": {"format": {"type": "pcm", "rate": 16000}},
                    "output": {"format": {"type": "pcm_s16le", "rate": 24000},
                               "voice": self._config.voice},
                },
                "tools": [{"type": "function", "name": item["name"],
                           "description": item["description"], "parameters": item["parameters"]}
                          for item in tools],
            },
            "extension": {"dialog": {"extra": {
                "enable_loudness_norm": True, "enable_user_query_exit": True,
                "enable_music": True,
            }}},
        })
        await self._expect("session.created")
        self._created = True
        items: list[dict[str, object]] = []
        pairs: list[tuple[ChatMessage, ChatMessage]] = []
        chars = 0
        for index in range(len(history) - 2, -1, -2):
            user, assistant = history[index:index + 2]
            if user.role != "user" or assistant.role != "assistant":
                continue
            size = len(user.content) + len(assistant.content)
            if len(pairs) >= 20 or chars + size > 12000:
                break
            pairs.append((user, assistant))
            chars += size
        for pair in reversed(pairs):
            for message in pair:
                items.append({
                    "id": str(uuid4()), "type": "message", "role": message.role,
                    "content": [{"type": "input_text", "text": message.content}],
                })
        if items:
            await self.send_event({"type": "conversation.item.create", "items": items})
            await self._expect("conversation.item.added")
        self._sender = asyncio.create_task(self._send_audio_loop(), name="seeduplex-uplink")
        _LOG.info("session created device_id=%s log_id=%s", context.device_id, self.log_id)

    async def send_event(self, event: dict[str, object]) -> None:
        if self._ws is None or self._ws.closed:
            raise ProviderFailure("Seeduplex connection is closed")
        async with self._write_lock:
            async with asyncio.timeout(self._config.connect_timeout_seconds):
                await self._ws.send_json({"event_id": str(uuid4()), **event})

    def _enqueue(self, value: tuple[int, bytes] | object) -> None:
        if self._closed or self._sender is None:
            raise ProviderFailure("Seeduplex connection is closed")
        if self._sender.done():
            self._sender.result()
            raise ProviderFailure("Seeduplex audio sender stopped")
        try:
            self._uplink.put_nowait(value)
        except asyncio.QueueFull as exc:
            raise ProviderFailure(
                "Seeduplex uplink exceeded two seconds of buffered audio"
            ) from exc

    async def send_audio(self, pcm: bytes) -> None:
        if len(pcm) != 640:
            raise ProviderFailure("Seeduplex input must be 20 ms / 640 bytes of 16 kHz PCM")
        if not self._input_muted:
            self._enqueue((self._input_epoch, bytes(pcm)))

    async def set_input_muted(self, muted: bool) -> None:
        if muted == self._input_muted:
            return
        self._input_muted = muted
        self._input_epoch += 1
        if muted:
            # Discard queued microphone frames and invalidate any paced frame
            # already held by the sender. Keep an explicit end-of-input commit.
            commit = False
            while not self._uplink.empty():
                commit |= self._uplink.get_nowait() is _COMMIT
            if commit:
                self._enqueue(_COMMIT)
            self._enqueue(_MUTE)

    async def commit_audio(self) -> None:
        self._enqueue(_COMMIT)

    async def _send_audio_loop(self) -> None:
        loop = asyncio.get_running_loop()
        next_frame = loop.time()
        muted = False
        while True:
            try:
                item = await asyncio.wait_for(self._uplink.get(), timeout=0.5)
            except TimeoutError:
                if not muted:
                    await self.send_event({"type": "input_audio_mute.commit"})
                    muted = True
                continue
            if item is _COMMIT or item is _MUTE:
                if item is _COMMIT:
                    await self.send_event({"type": "input_audio_buffer.commit"})
                if not muted:
                    await self.send_event({"type": "input_audio_mute.commit"})
                    muted = True
                continue
            assert isinstance(item, tuple)
            epoch, pcm = item
            if self._input_muted or epoch != self._input_epoch:
                continue
            if muted:
                await self.send_event({"type": "input_audio_unmute.commit"})
                next_frame = loop.time()
                muted = False
            delay = next_frame - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            if self._input_muted or epoch != self._input_epoch:
                continue
            await self.send_event({"type": "input_audio_buffer.append",
                                   "audio": base64.b64encode(pcm).decode("ascii")})
            next_frame = max(next_frame + 0.02, loop.time())

    async def _read(self) -> dict[str, object]:
        assert self._ws is not None
        message = await self._ws.receive()
        if message.type != aiohttp.WSMsgType.TEXT:
            raise ProviderFailure("Seeduplex connection closed before the session finished")
        try:
            event = json.loads(message.data)
        except (ValueError, TypeError) as exc:
            raise ProviderFailure("Seeduplex returned invalid JSON") from exc
        if not isinstance(event, dict) or not isinstance(event.get("type"), str):
            raise ProviderFailure("Seeduplex returned an invalid event")
        if event["type"] == "error":
            error = event.get("error")
            details = error if isinstance(error, dict) else event
            code = details.get("code", details.get("status_code", "unknown"))
            text = str(details.get("message", "provider error")).replace(
                self._api_key, "[redacted]"
            )
            raise ProviderFailure(f"Seeduplex error {code}: {text[:400]} (log_id={self.log_id})")
        return event

    async def _expect(self, event_type: str) -> None:
        while True:
            event = await self._read()
            if event["type"] == event_type:
                return

    async def receive_event(self) -> dict[str, object]:
        reader = asyncio.create_task(self._read())
        try:
            if self._sender is not None:
                done, _ = await asyncio.wait(
                    (reader, self._sender), return_when=asyncio.FIRST_COMPLETED
                )
                if self._sender in done:
                    self._sender.result()
                    raise ProviderFailure("Seeduplex audio sender stopped")
            return await reader
        finally:
            if not reader.done():
                reader.cancel()
            await asyncio.gather(reader, return_exceptions=True)

    async def close(self) -> None:
        async with self._close_lock:
            if self._closed:
                return
            self._closed = True
            if self._sender is not None:
                self._sender.cancel()
                await asyncio.gather(self._sender, return_exceptions=True)
            if self._ws is not None and not self._ws.closed:
                if self._created:
                    try:
                        async with asyncio.timeout(3):
                            await self.send_event({"type": "session.close"})
                            await self._expect("session.closed")
                    except (Exception, asyncio.CancelledError):
                        _LOG.debug("session close ack unavailable log_id=%s", self.log_id)
                await self._ws.close()
            if self._http is not None:
                await self._http.close()
            self._sessions.discard(self)
