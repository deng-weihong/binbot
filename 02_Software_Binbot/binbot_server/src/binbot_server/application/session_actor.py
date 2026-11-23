from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any
from uuid import UUID, uuid4

from binbot_server.domain.errors import (
    ProtocolViolation,
    ProviderFailure,
    ProviderTimeout,
    StateConflict,
)
from binbot_server.domain.model import ChatMessage, SessionPhase, TurnContext, VoiceSession

from .ports import (
    ASRStream,
    AudioDecoder,
    AudioEncoder,
    ConversationStore,
    EventSink,
    OutboundAudioFrame,
    ProviderBundle,
    SpeechEndpointDetector,
)
from .text_segmenter import PCMFramer, TextSegmenter

if TYPE_CHECKING:
    from .agent_runtime import AgentRuntime

_STOP = object()


@dataclass(slots=True)
class _Envelope:
    name: str
    payload: Any = None
    future: asyncio.Future[Any] | None = None


class SessionActor:
    """Serializes device state while provider I/O runs in cancellable child tasks."""

    def __init__(
        self,
        session: VoiceSession,
        providers: ProviderBundle,
        decoder: AudioDecoder,
        encoder: AudioEncoder,
        sink: EventSink,
        first_sentence_chars: int = 12,
        conversation_store: ConversationStore | None = None,
        max_turn_audio_seconds: float = 30.0,
        asr_timeout_seconds: float = 30.0,
        llm_timeout_seconds: float = 60.0,
        tts_timeout_seconds: float = 60.0,
        tts_concurrency: int = 2,
        max_tts_segments: int = 64,
        max_tts_segment_bytes: int = 1_440_000,
        device_agent: bool = False,
        agent_runtime: AgentRuntime | None = None,
        endpoint_detector: SpeechEndpointDetector | None = None,
    ):
        self.session = session
        self._providers = providers
        self._decoder = decoder
        self._encoder = encoder
        self._sink = sink
        self._first_sentence_chars = first_sentence_chars
        self._conversation_store = conversation_store
        self._max_turn_pcm_bytes = int(
            session.uplink.sample_rate
            * session.uplink.channels
            * 2
            * max_turn_audio_seconds
        )
        self._asr_timeout_seconds = asr_timeout_seconds
        self._llm_timeout_seconds = llm_timeout_seconds
        self._tts_timeout_seconds = tts_timeout_seconds
        self._tts_concurrency = tts_concurrency
        self._max_tts_segments = max_tts_segments
        self._max_tts_segment_bytes = max_tts_segment_bytes
        self._device_agent = device_agent
        self._agent_runtime = agent_runtime
        self._endpoint_detector = endpoint_detector
        self._ended_input: tuple[UUID, UUID] | None = None
        self._agent_response: asyncio.Future[str] | None = None
        self._mailbox: asyncio.Queue[_Envelope | object] = asyncio.Queue(maxsize=256)
        self._close_lock = asyncio.Lock()
        self._actor_task: asyncio.Task[None] | None = None
        self._pipeline_task: asyncio.Task[None] | None = None
        self._asr_stream: ASRStream | None = None
        self._last_input_sequence = -1
        self._last_input_timestamp_ms = -1
        self._input_pcm_bytes = 0
        self._resources_closed = False
        self._log = logging.getLogger(f"binbot.session.{session.device_id}")

    async def start(self) -> None:
        if self._actor_task is not None:
            return
        self.session.history = await self._load_history()
        self._actor_task = asyncio.create_task(
            self._run(), name=f"session:{self.session.device_id}"
        )
        await self._send_state()

    async def _load_history(self) -> list[ChatMessage]:
        if self._conversation_store is None:
            return self.session.history
        return list(await self._conversation_store.load_device_history(self.session.device_id))

    async def begin_turn(self, turn_id: UUID, stream_id: UUID, wake_word: str = "") -> None:
        await self._ask("begin_turn", (turn_id, stream_id, wake_word))

    async def push_audio(
        self, stream_id: UUID, sequence: int, timestamp_ms: int, payload: bytes
    ) -> None:
        await self._ask("push_audio", (stream_id, sequence, timestamp_ms, payload))

    async def end_audio(self, turn_id: UUID, stream_id: UUID) -> None:
        await self._ask("end_audio", (turn_id, stream_id))

    async def submit_text(self, turn_id: UUID, text: str) -> None:
        await self._ask("submit_text", (turn_id, text))

    async def submit_agent_response(self, turn_id: UUID, text: str) -> None:
        await self._ask("agent_response", (turn_id, text))

    async def cancel_turn(self, turn_id: UUID, reason: str) -> None:
        await self._ask("cancel_turn", (turn_id, reason))

    async def send_device_command(self, event: dict[str, object]) -> None:
        await self._ask("device_command", event)

    async def send_device_event(self, event: dict[str, object]) -> None:
        """Send a server initiated control event without coupling it to commands."""
        await self._ask("device_command", event)

    async def close(self) -> None:
        async with self._close_lock:
            if self._actor_task is None:
                self._close_resources()
                return
            task = self._actor_task
            try:
                await self._ask("close")
            finally:
                self._actor_task = None
                await task

    async def _ask(self, name: str, payload: Any = None) -> Any:
        if self._actor_task is None:
            raise StateConflict("session actor is not running")
        future = asyncio.get_running_loop().create_future()
        await self._mailbox.put(_Envelope(name, payload, future))
        return await future

    async def _post(self, name: str, payload: Any = None) -> None:
        await self._mailbox.put(_Envelope(name, payload, None))

    async def _run(self) -> None:
        while True:
            envelope = await self._mailbox.get()
            if envelope is _STOP:
                return
            assert isinstance(envelope, _Envelope)
            try:
                await self._dispatch(envelope.name, envelope.payload)
            except Exception as exc:
                if envelope.future is not None and not envelope.future.done():
                    envelope.future.set_exception(exc)
                else:
                    self._log.exception("unhandled internal actor event: %s", envelope.name)
            else:
                if envelope.future is not None and not envelope.future.done():
                    envelope.future.set_result(None)

    async def _dispatch(self, name: str, payload: Any) -> None:
        if name == "begin_turn":
            await self._handle_begin_turn(*payload)
        elif name == "push_audio":
            await self._handle_push_audio(*payload)
        elif name == "end_audio":
            await self._handle_end_audio(*payload)
        elif name == "submit_text":
            await self._handle_submit_text(*payload)
        elif name == "agent_response":
            await self._handle_agent_response(*payload)
        elif name == "cancel_turn":
            await self._handle_cancel(*payload)
        elif name == "device_command":
            await self._sink.send_control(payload)
        elif name == "pipeline_speaking":
            await self._handle_pipeline_speaking(*payload)
        elif name == "pipeline_complete":
            await self._handle_pipeline_complete(*payload)
        elif name == "pipeline_failed":
            await self._handle_pipeline_failed(*payload)
        elif name == "close":
            await self._handle_close()
        else:
            raise RuntimeError(f"unknown actor command: {name}")

    async def _handle_begin_turn(self, turn_id: UUID, stream_id: UUID, wake_word: str) -> None:
        if self.session.turn_id is not None:
            await self._cancel_active("superseded", notify=True)
        context = self.session.start_turn(turn_id, stream_id)
        self._decoder.reset()
        self._last_input_sequence = -1
        self._last_input_timestamp_ms = -1
        self._input_pcm_bytes = 0
        await self._send_state(wake_word=wake_word)
        if self._endpoint_detector is not None:
            self._endpoint_detector.reset()
        try:
            async with asyncio.timeout(self._asr_timeout_seconds):
                self._asr_stream = await self._providers.asr.open_stream(
                    context, self.session.uplink
                )
        except TimeoutError:
            await self._handle_pipeline_failed(
                turn_id,
                self.session.generation,
                ProviderTimeout.code,
                "ASR provider timed out while opening a stream",
            )
        except Exception as exc:
            await self._handle_pipeline_failed(
                turn_id,
                self.session.generation,
                getattr(exc, "code", "provider_failure"),
                str(exc),
            )

    async def _handle_push_audio(
        self, stream_id: UUID, sequence: int, timestamp_ms: int, payload: bytes
    ) -> None:
        # Audio already on the wire can arrive after a server endpoint event.
        # Ignore only that known completed stream, never an unrelated stream.
        if self._ended_input is not None and stream_id == self._ended_input[1]:
            return
        if self.session.phase != SessionPhase.LISTENING or self._asr_stream is None:
            raise StateConflict("audio arrived while the session is not listening")
        turn_id = self.session.turn_id
        assert turn_id is not None
        if stream_id != self.session.uplink_stream_id:
            raise StateConflict("audio stream does not belong to the active turn")
        expected_sequence = self._last_input_sequence + 1
        if sequence != expected_sequence:
            raise StateConflict(
                f"audio sequence must be contiguous; expected {expected_sequence}, got {sequence}"
            )
        expected_timestamp = (
            timestamp_ms
            if self._last_input_timestamp_ms < 0
            else self._last_input_timestamp_ms + self.session.uplink.frame_ms
        )
        if timestamp_ms != expected_timestamp:
            raise StateConflict(
                "audio timestamp must be contiguous and monotonic; "
                f"expected {expected_timestamp}, got {timestamp_ms}"
            )
        self._last_input_sequence = sequence
        self._last_input_timestamp_ms = timestamp_ms
        try:
            pcm = self._decoder.decode(payload)
        except Exception as exc:
            raise ProtocolViolation("invalid Opus uplink payload") from exc
        self._input_pcm_bytes += len(pcm)
        if self._input_pcm_bytes > self._max_turn_pcm_bytes:
            await self._handle_pipeline_failed(
                turn_id,
                self.session.generation,
                "audio_limit_exceeded",
                "turn audio exceeds the configured duration limit",
            )
            return
        try:
            async with asyncio.timeout(self._asr_timeout_seconds):
                events = await self._asr_stream.push(pcm)
        except TimeoutError:
            await self._handle_pipeline_failed(
                turn_id,
                self.session.generation,
                ProviderTimeout.code,
                "ASR provider timed out while accepting audio",
            )
            return
        except Exception as exc:
            await self._handle_pipeline_failed(
                turn_id,
                self.session.generation,
                getattr(exc, "code", "provider_failure"),
                str(exc),
            )
            return
        transcript = None
        for event in events:
            if event.text:
                transcript = event.text
                await self._send_turn_event("asr.partial", text=event.text)
        if self._endpoint_detector is not None:
            try:
                reason = await asyncio.to_thread(
                    self._endpoint_detector.process, pcm, transcript
                )
            except Exception:
                self._log.exception("server VAD failed device_id=%s", self.session.device_id)
                await self._handle_pipeline_failed(
                    turn_id, self.session.generation, "vad_failure", "speech detector failed"
                )
                return
            if self._input_pcm_bytes >= self._max_turn_pcm_bytes:
                reason = "max_duration"
            if reason is not None:
                self._log.info(
                    "server VAD ended device_id=%s turn_id=%s reason=%s input_ms=%d",
                    self.session.device_id, turn_id, reason,
                    self._input_pcm_bytes * 1000 // (self.session.uplink.sample_rate * 2),
                )
                await self._send_turn_event(
                    "audio.input_ended", stream_id=str(stream_id), reason=reason
                )
                await self._handle_end_audio(turn_id, stream_id)

    async def _handle_end_audio(self, turn_id: UUID, stream_id: UUID) -> None:
        if self._ended_input == (turn_id, stream_id):
            return
        self.session.finish_input(turn_id, stream_id)
        if self._endpoint_detector is not None:
            self._ended_input = (turn_id, stream_id)
        await self._send_state()
        context = self.session.context()
        generation = self.session.generation
        asr_stream = self._asr_stream
        self._asr_stream = None
        if asr_stream is None:
            raise StateConflict("active turn has no ASR stream")
        if self._input_pcm_bytes == 0:
            await asr_stream.abort()
            await self._handle_pipeline_failed(
                turn_id, generation, "no_speech", "turn contains no audio frames"
            )
            return
        self._pipeline_task = asyncio.create_task(
            self._finish_asr_and_respond(context, generation, asr_stream),
            name=f"pipeline:{self.session.device_id}:{turn_id}",
        )

    async def _handle_submit_text(self, turn_id: UUID, text: str) -> None:
        if self.session.turn_id is not None:
            await self._cancel_active("superseded", notify=True)
        context = self.session.start_turn(turn_id, None)
        await self._send_state()
        await self._send_turn_event("asr.final", text=text)
        generation = self.session.generation
        self._pipeline_task = asyncio.create_task(
            self._respond_or_request_agent(context, generation, text),
            name=f"pipeline:{self.session.device_id}:{turn_id}",
        )

    async def _handle_agent_response(self, turn_id: UUID, text: str) -> None:
        if not self._device_agent:
            raise ProtocolViolation("device did not negotiate device_agent_v1")
        self.session.assert_turn(turn_id)
        response = self._agent_response
        if response is None or response.done():
            raise StateConflict("turn is not waiting for a device agent response")
        response.set_result(text.strip())

    async def _handle_cancel(self, turn_id: UUID, reason: str) -> None:
        self.session.assert_turn(turn_id)
        await self._cancel_active(reason, notify=True)

    async def _handle_pipeline_speaking(self, turn_id: UUID, generation: int) -> None:
        if not self._is_current(turn_id, generation):
            return
        self.session.start_speaking(turn_id)
        await self._send_state()

    async def _handle_pipeline_complete(
        self, turn_id: UUID, generation: int, user_text: str, assistant_text: str
    ) -> None:
        if not self._is_current(turn_id, generation):
            return
        stored_history = None
        if self._conversation_store is not None:
            try:
                stored_history = await self._conversation_store.append_turn(
                    self.session.device_id, user_text, assistant_text
                )
            except Exception as exc:
                await self._handle_pipeline_failed(
                    turn_id, generation, "conversation_store_failed", str(exc)
                )
                return
        self.session.complete_turn(turn_id, user_text, assistant_text)
        if stored_history is not None:
            self.session.history = list(stored_history)
        self._pipeline_task = None
        await self._send_state()

    async def _handle_pipeline_failed(
        self, turn_id: UUID, generation: int, code: str, message: str
    ) -> None:
        if not self._is_current(turn_id, generation):
            return
        await self._send_turn_event("error", code=code, message=message, retryable=True)
        self.session.fail_turn(turn_id)
        await self._send_state()
        if self._asr_stream is not None:
            stream = self._asr_stream
            self._asr_stream = None
            try:
                async with asyncio.timeout(self._asr_timeout_seconds):
                    await stream.abort()
            except Exception:
                self._log.warning(
                    "ASR abort after failure failed device_id=%s session_id=%s turn_id=%s",
                    self.session.device_id,
                    self.session.session_id,
                    turn_id,
                    exc_info=True,
                )
        self.session.cancel_turn(turn_id)
        self._decoder.reset()
        self._encoder.reset()
        self._pipeline_task = None
        await self._send_state()

    async def _handle_close(self) -> None:
        await self._cancel_active("connection_closed", notify=False)
        self._close_resources()
        await self._mailbox.put(_STOP)

    async def _cancel_active(self, reason: str, notify: bool) -> None:
        cancelled = self.session.turn_id
        if self._pipeline_task is not None:
            task = self._pipeline_task
            self._pipeline_task = None
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        if self._asr_stream is not None:
            stream = self._asr_stream
            self._asr_stream = None
            try:
                async with asyncio.timeout(self._asr_timeout_seconds):
                    await stream.abort()
            except Exception:
                self._log.warning(
                    "ASR abort failed device_id=%s session_id=%s turn_id=%s",
                    self.session.device_id,
                    self.session.session_id,
                    cancelled,
                    exc_info=True,
                )
        if cancelled is not None:
            self.session.cancel_turn(cancelled)
            self._decoder.reset()
            self._encoder.reset()
            if notify:
                await self._sink.send_control(
                    {
                        "type": "turn.cancelled",
                        "session_id": str(self.session.session_id),
                        "turn_id": str(cancelled),
                        "reason": reason,
                    }
                )
                await self._send_state()

    async def _finish_asr_and_respond(
        self, context: TurnContext, generation: int, stream: ASRStream
    ) -> None:
        try:
            async with asyncio.timeout(self._asr_timeout_seconds):
                transcript = await stream.finish()
            text = transcript.text.strip()
            if not text:
                raise ProviderFailure("ASR returned no speech")
            if not self._is_current(context.turn_id, generation):
                return
            await self._send_turn_event("asr.final", text=text)
            await self._respond_or_request_agent(context, generation, text)
        except asyncio.CancelledError:
            await stream.abort()
            raise
        except TimeoutError:
            await stream.abort()
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    ProviderTimeout.code,
                    "ASR provider timed out",
                ),
            )
        except Exception as exc:
            await self._post(
                "pipeline_failed",
                (context.turn_id, generation, getattr(exc, "code", "pipeline_failed"), str(exc)),
            )

    async def _respond(self, context: TurnContext, generation: int, user_text: str) -> None:
        if self._agent_runtime is not None and self._agent_runtime.enabled:
            await self._respond_with_agent_runtime(context, generation, user_text)
            return
        segment_queue: asyncio.Queue[str | None] = asyncio.Queue(maxsize=16)
        segmenter = TextSegmenter(first_sentence_chars=self._first_sentence_chars)
        assistant_parts: list[str] = []
        tts_task = asyncio.create_task(
            self._run_tts(context, generation, segment_queue),
            name=f"tts:{self.session.device_id}:{context.turn_id}",
        )
        try:
            async with asyncio.timeout(self._llm_timeout_seconds):
                previous = await self._load_history()
                history = [*previous, ChatMessage(role="user", content=user_text)]
                async for delta in self._providers.llm.stream(context, history):
                    if not self._is_current(context.turn_id, generation):
                        raise asyncio.CancelledError
                    if not delta:
                        continue
                    assistant_parts.append(delta)
                    await self._send_turn_event("assistant.delta", text=delta)
                    for segment in segmenter.push(delta):
                        await segment_queue.put(segment)
            remaining = segmenter.flush()
            if remaining:
                await segment_queue.put(remaining)
            await segment_queue.put(None)
            await tts_task
            assistant_text = "".join(assistant_parts).strip()
            if not assistant_text:
                raise ProviderFailure("LLM returned an empty response")
            if self._is_current(context.turn_id, generation):
                await self._send_turn_event("assistant.final", text=assistant_text)
                await self._post(
                    "pipeline_complete",
                    (context.turn_id, generation, user_text, assistant_text),
                )
        except asyncio.CancelledError:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            raise
        except TimeoutError:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    ProviderTimeout.code,
                    "LLM provider timed out",
                ),
            )
        except Exception as exc:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            await self._post(
                "pipeline_failed",
                (context.turn_id, generation, getattr(exc, "code", "pipeline_failed"), str(exc)),
            )

    async def _respond_with_agent_runtime(
        self, context: TurnContext, generation: int, user_text: str
    ) -> None:
        assert self._agent_runtime is not None
        segment_queue: asyncio.Queue[str | None] = asyncio.Queue(maxsize=16)
        segmenter = TextSegmenter(first_sentence_chars=self._first_sentence_chars)
        tts_task = asyncio.create_task(
            self._run_tts(context, generation, segment_queue),
            name=f"tts:{self.session.device_id}:{context.turn_id}",
        )
        try:
            async with asyncio.timeout(self._llm_timeout_seconds):
                agent_context = await self._agent_runtime.build_context(
                    self.session.device_id,
                    user_text,
                    session_id=str(self.session.session_id),
                    source="voice",
                )
                response = await self._agent_runtime.run(
                    agent_context, context, approved=True
                )
            assistant_text = response.text.strip()
            if not assistant_text:
                raise ProviderFailure("LLM returned an empty response")
            if not self._is_current(context.turn_id, generation):
                raise asyncio.CancelledError
            await self._send_turn_event("assistant.delta", text=assistant_text)
            for segment in segmenter.push(assistant_text):
                await segment_queue.put(segment)
            remaining = segmenter.flush()
            if remaining:
                await segment_queue.put(remaining)
            await segment_queue.put(None)
            await tts_task
            if self._is_current(context.turn_id, generation):
                await self._send_turn_event("assistant.final", text=assistant_text)
                await self._post(
                    "pipeline_complete",
                    (context.turn_id, generation, user_text, assistant_text),
                )
        except asyncio.CancelledError:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            raise
        except TimeoutError:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    ProviderTimeout.code,
                    "LLM provider timed out",
                ),
            )
        except Exception as exc:
            tts_task.cancel()
            await asyncio.gather(tts_task, return_exceptions=True)
            await self._post(
                "pipeline_failed",
                (context.turn_id, generation, getattr(exc, "code", "pipeline_failed"), str(exc)),
            )

    async def _respond_or_request_agent(
        self, context: TurnContext, generation: int, user_text: str
    ) -> None:
        if not self._device_agent:
            await self._respond(context, generation, user_text)
            return
        response = asyncio.get_running_loop().create_future()
        self._agent_response = response
        try:
            await self._sink.send_control(
                {
                    "type": "agent.request",
                    "turn_id": str(context.turn_id),
                    "text": user_text,
                }
            )
            async with asyncio.timeout(self._llm_timeout_seconds):
                assistant_text = (await response).strip()
            if not assistant_text:
                raise ProviderFailure("device agent returned an empty response")
            if not self._is_current(context.turn_id, generation):
                return
            await self._speak_agent_response(
                context, generation, user_text, assistant_text
            )
        except asyncio.CancelledError:
            raise
        except TimeoutError:
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    ProviderTimeout.code,
                    "device agent timed out",
                ),
            )
        except Exception as exc:
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    getattr(exc, "code", "device_agent_failed"),
                    str(exc),
                ),
            )
        finally:
            if self._agent_response is response:
                self._agent_response = None

    async def _speak_agent_response(
        self,
        context: TurnContext,
        generation: int,
        user_text: str,
        assistant_text: str,
    ) -> None:
        segments: asyncio.Queue[str | None] = asyncio.Queue()
        segmenter = TextSegmenter(first_sentence_chars=self._first_sentence_chars)
        for segment in segmenter.push(assistant_text):
            await segments.put(segment)
        remaining = segmenter.flush()
        if remaining:
            await segments.put(remaining)
        await segments.put(None)
        try:
            await self._sink.send_control(
                {
                    "type": "assistant.delta",
                    "turn_id": str(context.turn_id),
                    "text": assistant_text,
                }
            )
            await self._run_tts(context, generation, segments)
            if self._is_current(context.turn_id, generation):
                await self._send_turn_event("assistant.final", text=assistant_text)
                await self._post(
                    "pipeline_complete",
                    (context.turn_id, generation, user_text, assistant_text),
                )
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._post(
                "pipeline_failed",
                (
                    context.turn_id,
                    generation,
                    getattr(exc, "code", "pipeline_failed"),
                    str(exc),
                ),
            )

    async def _run_tts(
        self,
        context: TurnContext,
        generation: int,
        segments: asyncio.Queue[str | None],
    ) -> None:
        stream_id = uuid4()
        sequence = 0
        started = False
        framer = PCMFramer(self.session.downlink.frame_samples)
        self._encoder.reset()
        loop = asyncio.get_running_loop()
        playback_epoch = 0.0
        playback_timestamp_ms = 0
        ended = False
        producer_tasks: list[asyncio.Task[None]] = []
        segment_slots = asyncio.Semaphore(self._tts_concurrency)
        prepared: asyncio.Queue[
            tuple[asyncio.Queue[bytes | BaseException | None], asyncio.Task[None]]
            | BaseException
            | None
        ] = asyncio.Queue(maxsize=self._tts_concurrency)

        async def emit_frame(pcm: bytes) -> None:
            nonlocal sequence, started, playback_epoch, playback_timestamp_ms
            if not self._is_current(context.turn_id, generation):
                raise asyncio.CancelledError
            if not started:
                playback_epoch = loop.time()
                playback_timestamp_ms = int(playback_epoch * 1_000)
                started = True
                await self._ask("pipeline_speaking", (context.turn_id, generation))
                await self._send_turn_event(
                    "audio.start",
                    stream_id=str(stream_id),
                    codec="opus",
                    sample_rate=self.session.downlink.sample_rate,
                    channels=1,
                    frame_ms=self.session.downlink.frame_ms,
                )
            if sequence >= 3:
                target = playback_epoch + (sequence - 2) * self.session.downlink.frame_ms / 1_000
                delay = target - loop.time()
                if delay > 0:
                    await asyncio.sleep(delay)
            encoded = self._encoder.encode(pcm)
            await self._sink.send_audio(
                OutboundAudioFrame(
                    stream_id=str(stream_id),
                    sequence=sequence,
                    timestamp_ms=(
                        playback_timestamp_ms
                        + sequence * self.session.downlink.frame_ms
                    ),
                    payload=encoded,
                )
            )
            sequence += 1

        async def prepare_segment(
            segment: str, output: asyncio.Queue[bytes | BaseException | None]
        ) -> None:
            total_bytes = 0
            cancelled = False
            try:
                async with segment_slots:
                    async with asyncio.timeout(self._tts_timeout_seconds):
                        async for chunk in self._providers.tts.stream(
                            context, segment, self.session.downlink.sample_rate
                        ):
                            if not chunk:
                                continue
                            total_bytes += len(chunk)
                            if total_bytes > self._max_tts_segment_bytes:
                                raise ProviderFailure(
                                    "TTS segment exceeds the configured audio limit"
                                )
                            await output.put(bytes(chunk))
            except asyncio.CancelledError:
                cancelled = True
                raise
            except TimeoutError:
                await output.put(ProviderTimeout("TTS provider timed out"))
            except BaseException as exc:
                await output.put(exc)
            finally:
                if cancelled:
                    try:
                        output.put_nowait(None)
                    except asyncio.QueueFull:
                        pass
                else:
                    await output.put(None)

        async def schedule_segments() -> None:
            segment_count = 0
            try:
                while True:
                    segment = await segments.get()
                    if segment is None:
                        break
                    segment_count += 1
                    if segment_count > self._max_tts_segments:
                        raise ProviderFailure("LLM response contains too many TTS segments")
                    output: asyncio.Queue[bytes | BaseException | None] = asyncio.Queue(
                        maxsize=8
                    )
                    task = asyncio.create_task(
                        prepare_segment(segment, output),
                        name=(
                            f"tts-segment:{self.session.device_id}:"
                            f"{context.turn_id}:{segment_count}"
                        ),
                    )
                    producer_tasks.append(task)
                    await prepared.put((output, task))
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                await prepared.put(exc)
            finally:
                current = asyncio.current_task()
                if current is not None and current.cancelling():
                    try:
                        prepared.put_nowait(None)
                    except asyncio.QueueFull:
                        pass
                else:
                    await prepared.put(None)

        scheduler = asyncio.create_task(
            schedule_segments(),
            name=f"tts-scheduler:{self.session.device_id}:{context.turn_id}",
        )
        try:
            while True:
                item = await prepared.get()
                if item is None:
                    break
                if isinstance(item, BaseException):
                    raise item
                output, producer = item
                while True:
                    chunk = await output.get()
                    if chunk is None:
                        break
                    if isinstance(chunk, BaseException):
                        raise chunk
                    for pcm_frame in framer.push(chunk):
                        await emit_frame(pcm_frame)
                await producer
            await scheduler
            final_frame = framer.flush()
            if final_frame is not None:
                await emit_frame(final_frame)
            if not started:
                raise ProviderFailure("TTS returned no audio")
            await self._send_turn_event(
                "audio.end", stream_id=str(stream_id), frames=sequence
            )
            ended = True
        finally:
            scheduler.cancel()
            for task in producer_tasks:
                task.cancel()
            await asyncio.gather(scheduler, *producer_tasks, return_exceptions=True)
            if started and not ended and self._is_current(context.turn_id, generation):
                await self._send_turn_event(
                    "audio.end", stream_id=str(stream_id), frames=sequence
                )

    def _is_current(self, turn_id: UUID, generation: int) -> bool:
        return self.session.turn_id == turn_id and self.session.generation == generation

    def _close_resources(self) -> None:
        if self._resources_closed:
            return
        self._resources_closed = True
        self.session.close()
        self._decoder.close()
        self._encoder.close()

    async def _send_state(self, **extra: object) -> None:
        event: dict[str, object] = {
            "type": "state",
            "session_id": str(self.session.session_id),
            "state": self.session.phase.value,
        }
        if self.session.turn_id is not None:
            event["turn_id"] = str(self.session.turn_id)
        event.update(extra)
        await self._sink.send_control(event)

    async def _send_turn_event(self, event_type: str, **payload: object) -> None:
        if self.session.turn_id is None:
            return
        event: dict[str, object] = {
            "type": event_type,
            "session_id": str(self.session.session_id),
            "turn_id": str(self.session.turn_id),
        }
        event.update(payload)
        await self._sink.send_control(event)
