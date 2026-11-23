import asyncio
from collections.abc import AsyncIterator, Sequence
from uuid import uuid4

import pytest

from binbot_server.application.ports import (
    AgentModelResponse,
    ASRStream,
    OutboundAudioFrame,
    ProviderBundle,
    TranscriptEvent,
)
from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.tools import ToolContext, ToolRegistry, ToolSpec
from binbot_server.application.session_actor import SessionActor
from binbot_server.domain.model import (
    AudioFormat,
    ChatMessage,
    SessionPhase,
    TurnContext,
    VoiceSession,
    ToolCall,
)
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.domain.errors import StateConflict


class RecordingSink:
    def __init__(self) -> None:
        self.controls: list[dict[str, object]] = []
        self.audio: list[OutboundAudioFrame] = []
        self.audio_sent_at: list[float] = []
        self.finished = asyncio.Event()

    async def send_control(self, event: dict[str, object]) -> None:
        self.controls.append(event)
        if event["type"] == "assistant.final":
            self.finished.set()

    async def send_audio(self, frame: OutboundAudioFrame) -> None:
        self.audio.append(frame)
        self.audio_sent_at.append(asyncio.get_running_loop().time())


class PassCodec:
    def decode(self, packet: bytes) -> bytes:
        return packet

    def encode(self, pcm_frame: bytes) -> bytes:
        return pcm_frame[:16]

    def reset(self) -> None:
        return None

    def close(self) -> None:
        return None


class EmptyASRStream:
    async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]:
        return ()

    async def finish(self) -> TranscriptEvent:
        return TranscriptEvent("语音问题", final=True)

    async def abort(self) -> None:
        return None


class FakeASR:
    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream:
        return EmptyASRStream()

    async def close(self) -> None:
        return None


class FakeLLM:
    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        yield "自己的"
        yield "服务端。"

    async def close(self) -> None:
        return None


class FakeTTS:
    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        yield bytes(sample_rate * 20 // 1000 * 2)

    async def close(self) -> None:
        return None


class InterruptibleLLM:
    def __init__(self) -> None:
        self.first_delta = asyncio.Event()
        self.never = asyncio.Event()

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context
        if messages[-1].content == "第一轮":
            yield "不应保留的旧结果"
            self.first_delta.set()
            await self.never.wait()
            yield "也不应到达这里。"
        else:
            yield "第二轮结果。"

    async def close(self) -> None:
        return None


class RecordingLLM:
    def __init__(self) -> None:
        self.calls: list[tuple[str, ...]] = []

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context
        self.calls.append(tuple(message.content for message in messages))
        yield "回答。"

    async def close(self) -> None:
        return None


class NeverLLM:
    def __init__(self) -> None:
        self.wait_forever = asyncio.Event()

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context, messages
        await self.wait_forever.wait()
        yield "不会输出"

    async def close(self) -> None:
        return None


class EmptyTTS:
    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        del context, text, sample_rate
        if False:
            yield b""

    async def close(self) -> None:
        return None


class DelayedBurstTTS:
    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        del context, text
        await asyncio.sleep(0.1)
        frame_bytes = sample_rate * 20 // 1_000 * 2
        yield bytes(frame_bytes * 6)

    async def close(self) -> None:
        return None


class ConcurrentOrderedTTS:
    def __init__(self) -> None:
        self.second_started = asyncio.Event()

    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        del context
        if text == "第一句。":
            await asyncio.wait_for(self.second_started.wait(), timeout=1)
            yield b"A" * (sample_rate * 20 // 1_000 * 2)
        else:
            self.second_started.set()
            yield b"B" * (sample_rate * 20 // 1_000 * 2)

    async def close(self) -> None:
        return None


class TwoSentenceLLM:
    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context, messages
        yield "第一句。第二句。"

    async def close(self) -> None:
        return None


async def wait_for_event(sink: RecordingSink, event_type: str) -> None:
    for _ in range(200):
        if any(event["type"] == event_type for event in sink.controls):
            return
        await asyncio.sleep(0.005)
    raise AssertionError(f"timed out waiting for {event_type}")


def make_actor(
    device_id: str,
    sink: RecordingSink,
    llm: object,
    tts: object,
    **kwargs: object,
) -> SessionActor:
    session = VoiceSession(
        device_id=device_id,
        uplink=AudioFormat("opus", 16_000, 1, 20),
        downlink=AudioFormat("opus", 24_000, 1, 20),
        max_history_messages=8,
    )
    return SessionActor(
        session,
        ProviderBundle(FakeASR(), llm, tts),  # type: ignore[arg-type]
        PassCodec(),
        PassCodec(),
        sink,
        **kwargs,  # type: ignore[arg-type]
    )


def test_text_turn_runs_through_actor_pipeline() -> None:
    async def scenario() -> None:
        session = VoiceSession(
            device_id="test-device",
            uplink=AudioFormat("opus", 16_000, 1, 20),
            downlink=AudioFormat("opus", 24_000, 1, 20),
            max_history_messages=8,
        )
        sink = RecordingSink()
        actor = SessionActor(
            session,
            ProviderBundle(FakeASR(), FakeLLM(), FakeTTS()),
            PassCodec(),
            PassCodec(),
            sink,
        )
        await actor.start()
        await actor.submit_text(uuid4(), "请回答")
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        for _ in range(20):
            if session.phase == SessionPhase.IDLE:
                break
            await asyncio.sleep(0)
        await actor.close()
        event_types = [event["type"] for event in sink.controls]
        assert "asr.final" in event_types
        assert "assistant.delta" in event_types
        assert "audio.start" in event_types
        assert "audio.end" in event_types
        assert sink.audio
        assert sink.audio[0].timestamp_ms > 0
        assert [
            frame.timestamp_ms for frame in sink.audio
        ] == [
            sink.audio[0].timestamp_ms + index * 20
            for index in range(len(sink.audio))
        ]
        assert session.phase == SessionPhase.CLOSED

    asyncio.run(scenario())


def test_text_turn_can_run_through_agent_runtime_tool_loop() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        tools = ToolRegistry()
        executed: list[dict[str, object]] = []

        async def execute(_: ToolContext, args: dict[str, object]) -> dict[str, object]:
            executed.append(args)
            return {"playing": True}

        tools.register(
            ToolSpec(
                "test.play", "Play", {"type": "object"}, "device.control", execute
            )
        )

        class Model:
            rounds = 0

            async def complete(
                self,
                _: TurnContext,
                messages: Sequence[ChatMessage],
                __: Sequence[dict[str, object]],
            ) -> AgentModelResponse:
                self.rounds += 1
                if self.rounds == 1:
                    return AgentModelResponse(
                        tool_calls=(ToolCall("call-1", "test.play", {}),)
                    )
                assert messages[-1].role == "tool"
                return AgentModelResponse(text="已经播放")

        model = Model()
        runtime = AgentRuntime(
            InMemoryConversationStore(max_messages=8), tools, 8, model=model
        )
        actor = make_actor(
            "agent-runtime-device", sink, FakeLLM(), FakeTTS(), agent_runtime=runtime
        )
        await actor.start()
        await actor.submit_text(uuid4(), "播放音乐")
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        await actor.close()
        assert executed == [{}]
        assert model.rounds == 2
        assert any(
            event["type"] == "assistant.final" and event["text"] == "已经播放"
            for event in sink.controls
        )

    asyncio.run(scenario())


def test_device_agent_response_bypasses_server_llm_and_uses_existing_tts() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        llm = RecordingLLM()
        actor = make_actor(
            "device-agent", sink, llm, FakeTTS(), device_agent=True
        )
        turn_id = uuid4()
        await actor.start()
        await actor.submit_text(turn_id, "摄像头前是什么？")
        await wait_for_event(sink, "agent.request")
        request = next(
            event for event in sink.controls if event["type"] == "agent.request"
        )
        assert request == {
            "type": "agent.request",
            "turn_id": str(turn_id),
            "text": "摄像头前是什么？",
        }
        await actor.submit_agent_response(turn_id, "摄像头前有一个杯子。")
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        for _ in range(20):
            if actor.session.phase == SessionPhase.IDLE:
                break
            await asyncio.sleep(0)
        assert llm.calls == []
        assert any(frame.payload for frame in sink.audio)
        assert [message.content for message in actor.session.history] == [
            "摄像头前是什么？",
            "摄像头前有一个杯子。",
        ]
        await actor.close()

    asyncio.run(scenario())


def test_new_turn_cancels_old_pipeline_without_stale_final_event() -> None:
    async def scenario() -> None:
        session = VoiceSession(
            device_id="interrupt-test",
            uplink=AudioFormat("opus", 16_000, 1, 20),
            downlink=AudioFormat("opus", 24_000, 1, 20),
            max_history_messages=8,
        )
        sink = RecordingSink()
        llm = InterruptibleLLM()
        actor = SessionActor(
            session,
            ProviderBundle(FakeASR(), llm, FakeTTS()),
            PassCodec(),
            PassCodec(),
            sink,
        )
        first_turn = uuid4()
        second_turn = uuid4()
        await actor.start()
        await actor.submit_text(first_turn, "第一轮")
        await asyncio.wait_for(llm.first_delta.wait(), timeout=1)
        await actor.submit_text(second_turn, "第二轮")
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        await actor.close()

        cancelled = [event for event in sink.controls if event["type"] == "turn.cancelled"]
        final = [event for event in sink.controls if event["type"] == "assistant.final"]
        assert cancelled == [
            {
                "type": "turn.cancelled",
                "session_id": str(session.session_id),
                "turn_id": str(first_turn),
                "reason": "superseded",
            }
        ]
        assert len(final) == 1
        assert final[0]["turn_id"] == str(second_turn)
        assert final[0]["text"] == "第二轮结果。"
        assert [message.content for message in session.history] == ["第二轮", "第二轮结果。"]

    asyncio.run(scenario())


def test_concurrent_close_is_idempotent() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        actor = make_actor("concurrent-close", sink, FakeLLM(), FakeTTS())
        await actor.start()
        await asyncio.wait_for(
            asyncio.gather(actor.close(), actor.close()),
            timeout=1,
        )
        assert actor.session.phase == SessionPhase.CLOSED

    asyncio.run(scenario())


def test_conversation_history_survives_reconnect_and_isolates_devices() -> None:
    async def scenario() -> None:
        store = InMemoryConversationStore(max_messages=8)
        llm = RecordingLLM()

        first_sink = RecordingSink()
        first = make_actor(
            "device-a", first_sink, llm, FakeTTS(), conversation_store=store
        )
        await first.start()
        await first.submit_text(uuid4(), "第一问")
        await asyncio.wait_for(first_sink.finished.wait(), timeout=1)
        await first.close()

        second_sink = RecordingSink()
        second = make_actor(
            "device-a", second_sink, llm, FakeTTS(), conversation_store=store
        )
        await second.start()
        await store.append_turn("device-a", "文字输入", "文字答复", source="web")
        await second.submit_text(uuid4(), "第二问")
        await asyncio.wait_for(second_sink.finished.wait(), timeout=1)
        await second.close()

        other_sink = RecordingSink()
        other = make_actor(
            "device-b", other_sink, llm, FakeTTS(), conversation_store=store
        )
        await other.start()
        await other.submit_text(uuid4(), "独立问题")
        await asyncio.wait_for(other_sink.finished.wait(), timeout=1)
        await other.close()

        assert llm.calls[0] == ("第一问",)
        assert llm.calls[1] == ("第一问", "回答。", "文字输入", "文字答复", "第二问")
        assert llm.calls[2] == ("独立问题",)

    asyncio.run(scenario())


def test_llm_timeout_reports_error_and_returns_idle() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        actor = make_actor(
            "timeout-device",
            sink,
            NeverLLM(),
            FakeTTS(),
            llm_timeout_seconds=0.01,
        )
        await actor.start()
        await actor.submit_text(uuid4(), "超时问题")
        await wait_for_event(sink, "error")
        assert [
            event.get("state") for event in sink.controls if event["type"] == "state"
        ][-2:] == ["error", "idle"]
        error = next(event for event in sink.controls if event["type"] == "error")
        assert error["code"] == "provider_timeout"
        await actor.close()

    asyncio.run(scenario())


def test_tts_prepares_concurrently_but_emits_in_text_order() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        tts = ConcurrentOrderedTTS()
        actor = make_actor(
            "ordered-tts", sink, TwoSentenceLLM(), tts, tts_concurrency=2
        )
        await actor.start()
        await actor.submit_text(uuid4(), "开始")
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        assert tts.second_started.is_set()
        assert [frame.payload[:1] for frame in sink.audio] == [b"A", b"B"]
        await actor.close()

    asyncio.run(scenario())


def test_tts_pacing_starts_with_first_audio_after_provider_delay() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        actor = make_actor("delayed-tts", sink, FakeLLM(), DelayedBurstTTS())
        await actor.start()
        await actor.submit_text(uuid4(), "开始")
        await asyncio.wait_for(sink.finished.wait(), timeout=2)

        assert len(sink.audio_sent_at) == 6
        # Three frames are prebuffered; subsequent frames follow the 20 ms clock.
        assert sink.audio_sent_at[-1] - sink.audio_sent_at[0] >= 0.045
        await actor.close()

    asyncio.run(scenario())


def test_empty_tts_is_a_failed_turn() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        actor = make_actor("empty-tts", sink, FakeLLM(), EmptyTTS())
        await actor.start()
        await actor.submit_text(uuid4(), "开始")
        await wait_for_event(sink, "error")
        assert not any(event["type"] == "assistant.final" for event in sink.controls)
        assert actor.session.phase == SessionPhase.IDLE
        await actor.close()

    asyncio.run(scenario())


class EndpointAfterFrames:
    def __init__(self, limit: int = 3):
        self.limit = limit
        self.reset()

    def reset(self) -> None:
        self.frames = 0
        self.transcripts: list[str | None] = []

    def process(self, pcm: bytes, transcript: str | None = None) -> str | None:
        self.frames += 1
        self.transcripts.append(transcript)
        return "speech_ended" if self.frames >= self.limit else None


class PartialASR(FakeASR):
    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream:
        class Stream(EmptyASRStream):
            async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]:
                return (TranscriptEvent("正在识别"),)
        return Stream()


def test_server_endpoint_preserves_partials_and_tolerates_in_flight_audio() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        detector = EndpointAfterFrames()
        actor = SessionActor(
            VoiceSession("endpoint-device", AudioFormat("opus", 16000, 1, 20),
                         AudioFormat("opus", 24000, 1, 20), 8),
            ProviderBundle(PartialASR(), FakeLLM(), FakeTTS()),
            PassCodec(), PassCodec(), sink, endpoint_detector=detector,
        )
        await actor.start()
        turn_id, stream_id = uuid4(), uuid4()
        await actor.begin_turn(turn_id, stream_id)
        for seq in range(3):
            await actor.push_audio(stream_id, seq, 1000 + seq * 20, bytes(640))
        endpoint = [e for e in sink.controls if e["type"] == "audio.input_ended"]
        assert len(endpoint) == 1 and endpoint[0]["reason"] == "speech_ended"
        assert endpoint[0]["turn_id"] == str(turn_id) and endpoint[0]["stream_id"] == str(stream_id)
        assert len([e for e in sink.controls if e["type"] == "asr.partial"]) == 3
        assert detector.transcripts == ["正在识别"] * 3
        await actor.push_audio(stream_id, 3, 1060, bytes(640))
        await actor.end_audio(turn_id, stream_id)
        await actor.end_audio(turn_id, stream_id)
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        assert len([e for e in sink.controls if e["type"] == "asr.final"]) == 1
        with pytest.raises(StateConflict):
            await actor.push_audio(uuid4(), 0, 0, bytes(640))
        await actor.close()
    asyncio.run(scenario())


def test_manual_stop_and_new_turn_reset_with_server_endpoint() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        detector = EndpointAfterFrames()
        actor = make_actor("manual-endpoint", sink, FakeLLM(), FakeTTS(),
                           endpoint_detector=detector)
        await actor.start()
        turn_id, stream_id = uuid4(), uuid4()
        await actor.begin_turn(turn_id, stream_id)
        await actor.push_audio(stream_id, 0, 0, bytes(640))
        await actor.end_audio(turn_id, stream_id)
        await actor.push_audio(stream_id, 1, 20, bytes(640))
        await asyncio.wait_for(sink.finished.wait(), timeout=1)
        assert not any(e["type"] == "audio.input_ended" for e in sink.controls)
        next_turn, next_stream = uuid4(), uuid4()
        await actor.begin_turn(next_turn, next_stream)
        assert detector.frames == 0
        await actor.push_audio(stream_id, 2, 40, bytes(640))
        assert detector.frames == 0
        for seq in range(3):
            await actor.push_audio(next_stream, seq, seq * 20, bytes(640))
        endpoints = [e for e in sink.controls if e["type"] == "audio.input_ended"]
        assert len(endpoints) == 1 and endpoints[0]["turn_id"] == str(next_turn)
        await actor.close()
    asyncio.run(scenario())


def test_server_endpoint_maximum_duration_is_a_normal_input_end() -> None:
    async def scenario() -> None:
        sink = RecordingSink()
        actor = make_actor("max-endpoint", sink, FakeLLM(), FakeTTS(),
                           endpoint_detector=EndpointAfterFrames(100), max_turn_audio_seconds=0.06)
        await actor.start()
        turn_id, stream_id = uuid4(), uuid4()
        await actor.begin_turn(turn_id, stream_id)
        for seq in range(3):
            await actor.push_audio(stream_id, seq, seq * 20, bytes(640))
        endpoints = [e for e in sink.controls if e["type"] == "audio.input_ended"]
        assert len(endpoints) == 1 and endpoints[0]["reason"] == "max_duration"
        assert not any(e["type"] == "error" for e in sink.controls)
        await actor.close()
    asyncio.run(scenario())
