from __future__ import annotations

import asyncio
import base64
import binascii
import json
from collections import deque
from dataclasses import dataclass, field
from typing import Any
from uuid import UUID, uuid4

from binbot_server.domain.errors import ProtocolViolation, ProviderFailure, StateConflict
from binbot_server.domain.model import SessionPhase

from .barge_in import SpeechStopMonitor, novel_speech, playback_echo, stop_request
from .ports import ASRProvider, OutboundAudioFrame, RealtimeVoiceSession
from .session_actor import SessionActor
from .speech_progress import spoken_prefix
from .text_segmenter import PCMFramer


@dataclass
class _PendingInput:
    assistant_text: str
    text: str = ""
    events: list[dict[str, object]] = field(default_factory=list)
    event_bytes: int = 0


class RealtimeSessionActor(SessionActor):
    """Run continuous voice conversations, taking turns with playback by default."""

    def __init__(
        self, *args: Any, idle_timeout_seconds: float = 90,
        max_tool_rounds: int = 4, max_output_buffer_seconds: float = 300,
        interruption_asr: ASRProvider | None = None,
        allow_interruption: bool = False,
        **kwargs: Any,
    ):
        super().__init__(*args, **kwargs)
        self._idle_timeout_seconds = idle_timeout_seconds
        self._max_tool_rounds = max_tool_rounds
        self._realtime: RealtimeVoiceSession | None = None
        self._reader: asyncio.Task[None] | None = None
        self._playback: asyncio.Task[None] | None = None
        self._output: asyncio.Queue[bytes | None] | None = None
        self._buffered_bytes = 0
        self._output_audio_bytes = 0
        self._max_output_buffer_bytes = int(
            self.session.downlink.sample_rate * self.session.downlink.channels
            * 2 * max_output_buffer_seconds
        )
        self._output_epoch = 0
        self._retired_input_streams: deque[UUID] = deque(maxlen=32)
        self._tool_tasks: set[asyncio.Task[None]] = set()
        self._tool_lock = asyncio.Lock()
        self._tool_results: dict[str, tuple[str, str]] = {}
        self._tool_rounds = 0
        self._cycle = 0
        self._input_text = ""
        self._user_text = ""
        self._assistant_text = ""
        self._response_id = ""
        self._blocked_responses: deque[str] = deque(maxlen=128)
        self._provider_response_id = ""
        self._pending_input: _PendingInput | None = None
        self._echo_input = False
        self._audio_done = False
        self._exit = False
        self._interruption_asr = interruption_asr
        self._allow_interruption = allow_interruption
        self._input_muted = False
        self._discard_input = False
        self._stop_monitor: SpeechStopMonitor | None = None
        self._stop_monitor_closers: set[asyncio.Task[None]] = set()

    async def _dispatch(self, name: str, payload: Any) -> None:
        if name == "realtime_event":
            turn_id, generation, event = payload
            if self._realtime is not None and self._is_current(turn_id, generation):
                try:
                    await self._handle_realtime_event(event)
                except Exception as exc:
                    await self._handle_pipeline_failed(
                        turn_id, generation, getattr(exc, "code", "provider_failure"), str(exc)
                    )
        elif name == "realtime_played":
            turn_id, generation, epoch, user_text, assistant_text, exit_intent = payload
            if self._is_current(turn_id, generation) and epoch == self._output_epoch:
                await self._close_stop_monitor()
                self._playback = None
                self._output = None
                if self._conversation_store is not None and user_text and assistant_text:
                    self.session.history = list(await self._conversation_store.append_turn(
                        self.session.device_id, user_text, assistant_text
                    ))
                await self._send_turn_event("assistant.final", text=assistant_text)
                if exit_intent:
                    await self._cancel_active("user_exit", notify=True)
                else:
                    await self._set_input_muted(False)
                    if not self._allow_interruption:
                        self._discard_input = True
                    self.session.phase = SessionPhase.LISTENING
                    await self._send_state()
        elif name == "realtime_idle":
            playing = self._playback is not None and (
                self._audio_done
                or self._buffered_bytes >= self.session.downlink.frame_samples * 2
            )
            if self._is_current(*payload) and not playing:
                await self._cancel_active("voice_inactivity", notify=True)
        elif name == "realtime_speaking":
            turn_id, generation, epoch = payload
            if self._is_current(turn_id, generation) and epoch == self._output_epoch:
                self.session.phase = SessionPhase.SPEAKING
                await self._send_state()
                self._start_stop_monitor(turn_id, generation, epoch)
        elif name == "speech_stop":
            turn_id, generation, epoch = payload
            if (self._allow_interruption and self._realtime is not None
                    and self._is_current(turn_id, generation)
                    and epoch == self._output_epoch
                    and self.session.phase == SessionPhase.SPEAKING):
                self._log.info("speech interruption device_id=%s", self.session.device_id)
                await self._begin_input()
                try:
                    await self._realtime.send_event({"type": "response.cancel"})
                except Exception as exc:
                    await self._handle_pipeline_failed(
                        turn_id, generation, getattr(exc, "code", "provider_failure"), str(exc))
        else:
            await super()._dispatch(name, payload)

    async def _handle_begin_turn(self, turn_id: UUID, stream_id: UUID, wake_word: str) -> None:
        await self._cancel_active("superseded", notify=True)
        context = self.session.start_turn(turn_id, stream_id)
        self._decoder.reset()
        self._last_input_sequence = -1
        self._last_input_timestamp_ms = -1
        self._ended_input = None
        self._input_text = self._user_text = self._assistant_text = self._response_id = ""
        self._blocked_responses.clear()
        self._provider_response_id = ""
        self._pending_input = None
        self._echo_input = False
        self._tool_results.clear()
        self._cycle = self._tool_rounds = 0
        await self._send_state(wake_word=wake_word)
        provider = self._providers.realtime
        assert provider is not None
        try:
            self._realtime = await provider.open_session(
                context, await self._load_history(),
                self._agent_runtime.describe_tools() if self._agent_runtime is not None else (),
            )
            self._reader = asyncio.create_task(
                self._receive_events(turn_id, self.session.generation),
                name=f"realtime:{self.session.device_id}:{turn_id}",
            )
        except Exception as exc:
            await self._handle_pipeline_failed(
                turn_id, self.session.generation,
                getattr(exc, "code", "provider_failure"), str(exc),
            )

    async def _receive_events(self, turn_id: UUID, generation: int) -> None:
        assert self._realtime is not None
        try:
            while True:
                try:
                    async with asyncio.timeout(self._idle_timeout_seconds):
                        event = await self._realtime.receive_event()
                except TimeoutError:
                    await self._post("realtime_idle", (turn_id, generation))
                else:
                    await self._post("realtime_event", (turn_id, generation, event))
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._post("pipeline_failed", (
                turn_id, generation, getattr(exc, "code", "provider_failure"), str(exc),
            ))

    async def _handle_push_audio(
        self, stream_id: UUID, sequence: int, timestamp_ms: int, payload: bytes,
    ) -> None:
        if stream_id in self._retired_input_streams:
            return
        if self._ended_input is not None and self._ended_input[1] == stream_id:
            return
        if self._realtime is None or stream_id != self.session.uplink_stream_id:
            raise StateConflict("audio stream does not belong to an active realtime session")
        if sequence != self._last_input_sequence + 1:
            raise StateConflict("realtime audio sequence must be contiguous")
        if (self._last_input_timestamp_ms >= 0
                and timestamp_ms != self._last_input_timestamp_ms + 20):
            raise StateConflict("realtime audio timestamps must advance by 20 ms")
        try:
            pcm = self._decoder.decode(payload)
        except Exception as exc:
            raise ProtocolViolation("invalid Opus uplink payload") from exc
        self._last_input_sequence = sequence
        self._last_input_timestamp_ms = timestamp_ms
        if self._input_muted:
            # Decode and account for every frame so the continuous device stream
            # can resume without resetting Opus, sequence numbers or timestamps.
            return
        try:
            await self._realtime.send_audio(pcm)
        except Exception as exc:
            assert self.session.turn_id is not None
            await self._handle_pipeline_failed(
                self.session.turn_id, self.session.generation,
                getattr(exc, "code", "provider_failure"), str(exc),
            )
        if self._stop_monitor is not None and self.session.phase == SessionPhase.SPEAKING:
            self._stop_monitor.feed(pcm)

    async def _handle_end_audio(self, turn_id: UUID, stream_id: UUID) -> None:
        if stream_id in self._retired_input_streams:
            return
        if self._ended_input == (turn_id, stream_id):
            return
        self.session.assert_turn(turn_id)
        if self._realtime is None or stream_id != self.session.uplink_stream_id:
            raise StateConflict("audio stream does not belong to the realtime session")
        await self._realtime.commit_audio()
        self._ended_input = (turn_id, stream_id)
        self.session.phase = SessionPhase.THINKING
        await self._send_state()

    def _start_stop_monitor(self, turn_id: UUID, generation: int, epoch: int) -> None:
        if (not self._allow_interruption or self._interruption_asr is None
                or self._stop_monitor is not None):
            return

        async def stopped(_text: str) -> None:
            await self._post("speech_stop", (turn_id, generation, epoch))

        self._stop_monitor = SpeechStopMonitor(
            self._interruption_asr, lambda: self._assistant_text, stopped)
        self._stop_monitor.start(self.session.context())

    async def _set_input_muted(self, muted: bool) -> None:
        if self._allow_interruption or muted == self._input_muted:
            return
        if self._realtime is not None:
            await self._realtime.set_input_muted(muted)
        self._input_muted = muted

    async def _close_stop_monitor(self) -> None:
        monitor, self._stop_monitor = self._stop_monitor, None
        if monitor is not None:
            # Stop feeding this monitor immediately, but keep its network
            # cleanup off the device-control and continuous microphone path.
            task = asyncio.create_task(monitor.close(), name="duplex-stop-asr-close")
            self._stop_monitor_closers.add(task)
            task.add_done_callback(self._stop_monitor_closers.discard)

    async def _stop_output(self) -> None:
        self._output_epoch += 1
        if self._playback is not None:
            self._playback.cancel()
            await asyncio.gather(self._playback, return_exceptions=True)
        self._playback = None
        self._output = None
        self._buffered_bytes = 0
        self._output_audio_bytes = 0
        await self._close_stop_monitor()

    async def _stop_realtime(self) -> None:
        self._input_muted = False
        self._discard_input = False
        self._pending_input = None
        self._echo_input = False
        self._provider_response_id = ""
        stream_id = self.session.uplink_stream_id
        if stream_id is not None and stream_id not in self._retired_input_streams:
            # Frames already in flight can arrive after failure, stop or a new wake.
            self._retired_input_streams.append(stream_id)
        if self._reader is not None:
            self._reader.cancel()
            await asyncio.gather(self._reader, return_exceptions=True)
            self._reader = None
        for task in self._tool_tasks:
            task.cancel()
        await asyncio.gather(*self._tool_tasks, return_exceptions=True)
        self._tool_tasks.clear()
        await self._stop_output()
        if self._stop_monitor_closers:
            await asyncio.gather(*tuple(self._stop_monitor_closers), return_exceptions=True)
        if self._realtime is not None:
            realtime, self._realtime = self._realtime, None
            try:
                await realtime.send_event({"type": "response.cancel"})
            except Exception:
                pass
            await realtime.close()

    async def _cancel_active(self, reason: str, notify: bool) -> None:
        await self._stop_realtime()
        await super()._cancel_active(reason, notify)

    async def _handle_pipeline_failed(
        self, turn_id: UUID, generation: int, code: str, message: str,
    ) -> None:
        if self._is_current(turn_id, generation):
            self._log.warning("realtime failure turn_id=%s code=%s message=%s",
                              turn_id, code, message)
            await self._stop_realtime()
        await super()._handle_pipeline_failed(turn_id, generation, code, message)

    async def _handle_realtime_event(self, event: dict[str, object]) -> None:
        kind = str(event["type"])
        if (not self._allow_interruption
                and kind.startswith("conversation.item.input_audio_transcription.")):
            if kind.endswith(".started"):
                self._discard_input = self._input_muted
            if self._discard_input:
                return
        response_id = str(event.get("response_id") or "")
        if response_id and response_id in self._blocked_responses:
            return
        function_event = kind == "response.function_call_arguments.done"
        output_event = kind.startswith("response.output_") or function_event
        if kind.startswith("response.output_"):
            # Audio deltas can omit response_id after an identified start.
            # Remember the provider stream even when its response is rejected,
            # so those unkeyed packets cannot leak into the preserved playback.
            if response_id:
                self._provider_response_id = response_id
            elif self._provider_response_id:
                response_id = self._provider_response_id
                event = {**event, "response_id": response_id}
        if response_id and response_id in self._blocked_responses:
            return
        if (not self._allow_interruption and output_event
                and self._audio_done and self._playback is None):
            # Late text/audio from a completed exchange must not mute input
            # again. Only a fresh recognized utterance opens the next exchange.
            return
        if (not self._allow_interruption and output_event
                and (self._playback is not None or self._audio_done)
                and self._response_id and response_id and response_id != self._response_id):
            # A late upstream recognition must not replace the reply being
            # played, even if its audio/text arrives after listening resumes.
            self._blocked_responses.append(response_id)
            return
        if (kind.startswith("response.output_") and not response_id
                and not self._response_id and self._blocked_responses):
            # Some late audio deltas omit response_id. Require an identified
            # new response before accepting unkeyed output after an interrupt.
            return
        if output_event and (response_id != self._response_id or function_event):
            if self._pending_input is not None:
                pending = self._pending_input
                event_bytes = len(json.dumps(event, ensure_ascii=False).encode())
                if (len(pending.events) >= 2048 or pending.event_bytes + event_bytes
                        > self._max_output_buffer_bytes * 4 // 3 + 65536):
                    raise ProviderFailure(
                        "Seeduplex unconfirmed response exceeded the buffer limit")
                pending.events.append(event)
                pending.event_bytes += event_bytes
                return
            if self._echo_input:
                if response_id:
                    self._blocked_responses.append(response_id)
                return
        if kind == "conversation.item.input_audio_transcription.started":
            self._reject_pending_input()
            self._echo_input = False
            if self._playback is not None and self._assistant_text:
                # Native VAD also hears residual playback, especially while AEC
                # converges. Keep playing until its ASR establishes new speech.
                self._pending_input = _PendingInput(self._assistant_text)
            else:
                await self._begin_input()
        elif kind == "conversation.item.input_audio_transcription.delta":
            # Seeduplex ASR deltas are complete, revisable hypotheses; output
            # text deltas below are incremental tokens.
            text = str(event.get("delta") or "")
            deferred = await self._validate_input(text, final=False)
            if deferred is None:
                return
            self._input_text = text
            await self._send_turn_event("asr.partial", text=self._input_text)
            await self._release_response_events(deferred)
        elif kind == "conversation.item.input_audio_transcription.completed":
            fallback = (self._pending_input.text if self._pending_input is not None
                        else self._input_text)
            text = str(event.get("transcript") or event.get("text") or fallback)
            deferred = await self._validate_input(text, final=True)
            if deferred is None:
                return
            self._user_text = text
            await self._set_input_muted(True)
            if not self._allow_interruption:
                self._discard_input = True
            await self._send_turn_event("asr.final", text=self._user_text)
            if self.session.phase != SessionPhase.SPEAKING:
                self.session.phase = SessionPhase.THINKING
                await self._send_state()
            await self._release_response_events(deferred)
        elif kind == "conversation.item.input_audio_transcription.failed":
            if self._pending_input is not None or self._echo_input:
                self._reject_pending_input()
                return
            raise ProviderFailure("Seeduplex speech recognition failed")
        elif kind in {"response.output_text.delta", "response.output_text.done"}:
            await self._set_input_muted(True)
            await self._select_response(response_id)
            if kind.endswith(".delta"):
                text = str(event.get("delta") or "")
                self._assistant_text += text
                await self._send_turn_event("assistant.delta", text=text)
            else:
                self._assistant_text = str(event.get("text") or self._assistant_text)
        elif kind in {"response.output_audio.started", "response.output_audio.delta"}:
            await self._set_input_muted(True)
            await self._select_response(response_id)
            if self._audio_done:
                return
            if kind.endswith(".delta"):
                try:
                    pcm = base64.b64decode(str(event.get("delta") or ""), validate=True)
                except (ValueError, binascii.Error) as exc:
                    raise ProviderFailure("Seeduplex returned invalid audio Base64") from exc
                if not pcm:
                    return
                self._start_output()
                assert self._output is not None
                if self._buffered_bytes + len(pcm) > self._max_output_buffer_bytes:
                    raise ProviderFailure("Seeduplex output exceeded the buffered audio limit")
                self._buffered_bytes += len(pcm)
                self._output_audio_bytes += len(pcm)
                self._output.put_nowait(pcm)
        elif kind == "response.output_audio.done":
            await self._select_response(response_id)
            if self._audio_done:
                return
            self._audio_done = True
            # The live API signals user exit with this status code rather than
            # a boolean; retain the explicit flags for compatible deployments.
            self._exit = str(event.get("status_code")) == "20000002"
            self._exit |= event.get("user_query_exit") is True
            extension = event.get("extension")
            if isinstance(extension, dict):
                self._exit |= extension.get("user_query_exit") is True
            self._start_output()
            assert self._output is not None
            self._output.put_nowait(None)
        elif kind == "response.function_call_arguments.done":
            await self._set_input_muted(True)
            self._tool_rounds += 1
            if self._tool_rounds > self._max_tool_rounds:
                raise ProviderFailure("Seeduplex exceeded the tool call round limit")
            items = event.get("items")
            if not isinstance(items, list) or not 1 <= len(items) <= 16:
                raise ProviderFailure("Seeduplex returned an invalid function call batch")
            task = asyncio.create_task(self._execute_tools(items, self._cycle))
            self._tool_tasks.add(task)
        elif kind == "response.canceled":
            if (self._pending_input is not None or self._echo_input
                    or (not self._allow_interruption and self._input_muted)):
                # The provider may cancel synthesis on its own VAD. Preserve all
                # queued PCM; if synthesis ended early, drain rather than wait
                # indefinitely for an audio.done that will never arrive.
                if ((not response_id or not self._response_id or response_id == self._response_id)
                        and not self._audio_done
                        and (self._output is not None or not self._allow_interruption)):
                    self._audio_done = True
                    self._start_output()
                    assert self._output is not None
                    self._output.put_nowait(None)
                return
            await self._stop_output()
            await self._send_turn_event("audio.cancel")
            self.session.phase = SessionPhase.LISTENING
            await self._send_state()
        elif kind == "session.closed":
            await self._cancel_active("provider_closed", notify=True)
        # response.done is usage reporting, not an audio/turn boundary.

    def _reject_pending_input(self) -> None:
        pending, self._pending_input = self._pending_input, None
        if pending is None:
            return
        for event in pending.events:
            response_id = str(event.get("response_id") or "")
            if (response_id and response_id != self._response_id
                    and response_id not in self._blocked_responses):
                self._blocked_responses.append(response_id)
        self._echo_input = True

    async def _validate_input(
        self, text: str, *, final: bool,
    ) -> list[dict[str, object]] | None:
        pending = self._pending_input
        if pending is None:
            return None if self._echo_input else []
        pending.text = text
        echo = self._assistant_text or pending.assistant_text
        new_speech = stop_request(text, echo) or bool(novel_speech(text, echo))
        # Short questions such as “为什么” must also work once ASR is final.
        new_speech |= (final and any(char.isalnum() for char in text)
                       and not playback_echo(text, echo))
        if not new_speech:
            if final:
                self._log.info("playback echo ignored device_id=%s chars=%s",
                               self.session.device_id, len(text))
                self._reject_pending_input()
            return None
        deferred = pending.events
        self._pending_input = None
        await self._begin_input()
        return deferred

    async def _release_response_events(self, events: list[dict[str, object]]) -> None:
        for event in events:
            await self._handle_realtime_event(event)

    async def _begin_input(self) -> None:
        # Auxiliary ASR can confirm an interruption before the native ASR does.
        # Discard any still-unconfirmed response when that path takes over.
        self._reject_pending_input()
        self._echo_input = False
        if self._response_id:
            self._blocked_responses.append(self._response_id)
        await self._stop_output()
        for task in self._tool_tasks:
            task.cancel()
        await asyncio.gather(*self._tool_tasks, return_exceptions=True)
        self._tool_tasks.clear()
        self._cycle += 1
        self._tool_rounds = 0
        self._input_text = self._user_text = self._assistant_text = self._response_id = ""
        self._audio_done = self._exit = False
        await self._send_turn_event("input.started", message_id=str(uuid4()))
        self.session.phase = SessionPhase.LISTENING
        await self._send_state()

    async def _select_response(self, response_id: str) -> None:
        if response_id and self._response_id and response_id != self._response_id:
            self._blocked_responses.append(self._response_id)
            await self._stop_output()
            self._assistant_text = ""
            self._audio_done = self._exit = False
        if response_id:
            self._response_id = response_id

    def _start_output(self) -> None:
        if self._output is None:
            # Song synthesis can run far ahead of playback. Bound PCM bytes,
            # independent of provider chunk sizes, without blocking the microphone.
            self._output = asyncio.Queue()
            self._output_audio_bytes = 0
            self._playback = asyncio.create_task(
                self._play_output(self._output, self._output_epoch), name="realtime-playback"
            )

    async def _play_output(self, queue: asyncio.Queue[bytes | None], epoch: int) -> None:
        context = self.session.context()
        generation = self.session.generation
        stream_id = str(uuid4())
        framer = PCMFramer(self.session.downlink.frame_samples)
        self._encoder.reset()
        sequence = 0
        loop = asyncio.get_running_loop()
        start = loop.time()
        started = False
        spoken_chars = 0

        async def emit(pcm: bytes) -> None:
            nonlocal sequence, started, start, spoken_chars
            if not self._is_current(context.turn_id, generation) or epoch != self._output_epoch:
                raise asyncio.CancelledError
            if not started:
                start = loop.time()
                await self._ask("realtime_speaking", (context.turn_id, generation, epoch))
                await self._send_turn_event(
                    "audio.start", stream_id=stream_id, codec="opus", sample_rate=24000,
                    channels=1, frame_ms=self.session.downlink.frame_ms,
                )
                started = True
            delay = (start + max(0, sequence - 2) * self.session.downlink.frame_ms / 1000
                     - loop.time())
            if delay > 0:
                await asyncio.sleep(delay)
            await self._sink.send_audio(OutboundAudioFrame(
                stream_id=stream_id, sequence=sequence,
                timestamp_ms=int(start * 1000) + sequence * self.session.downlink.frame_ms,
                payload=self._encoder.encode(pcm),
            ))
            self._buffered_bytes = max(0, self._buffered_bytes - len(pcm))
            sequence += 1
            if sequence % 10 == 0:
                # The audio sender runs two frames ahead; leave another four
                # frames for device playback buffering. This is a reading
                # guide rather than a provider-supplied word alignment.
                played_ms = max(0, sequence - 6) * self.session.downlink.frame_ms
                duration_ms = self._output_audio_bytes // 48 if self._audio_done else None
                prefix = spoken_prefix(self._assistant_text, played_ms, duration_ms)
                if len(prefix) > spoken_chars:
                    spoken_chars = len(prefix)
                    await self._send_turn_event(
                        "assistant.playback", stream_id=stream_id,
                        text=prefix, estimated=True,
                    )

        try:
            while True:
                chunk = await queue.get()
                if chunk is None:
                    break
                for frame in framer.push(chunk):
                    await emit(frame)
            tail = framer.flush()
            if tail is not None:
                await emit(tail)
            if started:
                await self._send_turn_event("audio.end", stream_id=stream_id, frames=sequence)
                if not self._allow_interruption:
                    # The paced sender leads playback by two frames and the
                    # device buffers audio. Keep input muted through this tail.
                    await asyncio.sleep(0.2)
            await self._post("realtime_played", (
                context.turn_id, generation, epoch,
                self._user_text, self._assistant_text, self._exit,
            ))
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._post("pipeline_failed", (
                context.turn_id, generation, getattr(exc, "code", "provider_failure"), str(exc),
            ))

    async def _handle_pipeline_speaking(self, turn_id: UUID, generation: int) -> None:
        if self._realtime is None:
            await super()._handle_pipeline_speaking(turn_id, generation)
        elif self._is_current(turn_id, generation):
            self.session.phase = SessionPhase.SPEAKING
            await self._send_state()

    async def _execute_tools(self, items: list[Any], cycle: int) -> None:
        async with self._tool_lock:
            if self._cycle == cycle:
                await self._execute_tool_batch(items, cycle)

    async def _execute_tool_batch(self, items: list[Any], cycle: int) -> None:
        realtime = self._realtime
        turn_id, generation = self.session.turn_id, self.session.generation
        assert realtime is not None and turn_id is not None
        try:
            if self._agent_runtime is None:
                raise ProviderFailure("voice tools are disabled")
            context = await self._agent_runtime.build_context(
                self.session.device_id, self._user_text or self._input_text or "Voice tool request",
                session_id=str(self.session.session_id), source="voice",
            )
            results: list[dict[str, object]] = []
            for item in items:
                if (not isinstance(item, dict) or not isinstance(item.get("call_id"), str)
                        or not item["call_id"]):
                    raise ProviderFailure("Seeduplex function call has no call_id")
                call_id = item["call_id"]
                signature = json.dumps([item.get("name"), item.get("arguments")], sort_keys=True)
                cached = self._tool_results.get(call_id)
                if cached is not None:
                    if cached[0] != signature:
                        raise ProviderFailure("Seeduplex reused a call_id with different arguments")
                    output = cached[1]
                else:
                    if len(self._tool_results) >= 256:
                        raise ProviderFailure("Seeduplex session exceeded the tool call limit")
                    try:
                        arguments = json.loads(item.get("arguments", "{}"))
                        if not isinstance(arguments, dict) or not isinstance(item.get("name"), str):
                            raise ValueError("tool arguments must be a JSON object")
                        result = await self._agent_runtime.execute_tool(
                            item["name"], context, arguments, approved=True,
                        )
                    except Exception as exc:
                        result = {"error": str(exc)}
                    output = json.dumps(result, ensure_ascii=False)
                    self._tool_results[call_id] = (signature, output)
                results.append({"type": "message", "role": "tool", "call_id": call_id,
                                "content": [{"type": "input_text", "text": output}]})
            if self._is_current(turn_id, generation) and self._cycle == cycle:
                await realtime.send_event({"type": "conversation.item.create", "items": results})
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._post("pipeline_failed", (
                turn_id, generation, getattr(exc, "code", "provider_failure"), str(exc),
            ))
