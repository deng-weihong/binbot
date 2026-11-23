import asyncio
import base64
import json
from uuid import uuid4

import pytest

from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.ports import ProviderBundle, TranscriptEvent
from binbot_server.application.realtime_session import RealtimeSessionActor
from binbot_server.application.tools import ToolContext, ToolRegistry, ToolSpec
from binbot_server.domain.model import AudioFormat, SessionPhase, VoiceSession
from binbot_server.domain.errors import ProviderFailure, StateConflict
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from test_session_actor import FakeASR, FakeTTS, PassCodec, RecordingLLM, RecordingSink


class VoiceSessionStub:
    def __init__(self):
        self.audio = []
        self.sent = []
        self.events = asyncio.Queue()
        self.closed = False
        self.input_mutes = []

    async def send_audio(self, pcm):
        self.audio.append(pcm)

    async def set_input_muted(self, muted):
        self.input_mutes.append(muted)

    async def commit_audio(self):
        self.sent.append({"type": "input_audio_buffer.commit"})

    async def send_event(self, event):
        self.sent.append(event)

    async def receive_event(self):
        event = await self.events.get()
        if isinstance(event, Exception):
            raise event
        return event

    async def close(self):
        self.closed = True


class VoiceProviderStub:
    def __init__(self):
        self.session = VoiceSessionStub()
        self.opened = []

    async def open_session(self, context, history, tools):
        self.opened.append((context, history, tools))
        return self.session

    async def close(self):
        await self.session.close()


class UnusedASR(FakeASR):
    async def open_stream(self, *_args):
        raise AssertionError("realtime voice must not use the ASR pipeline")


def make_actor(**kwargs):
    # Existing interruption coverage opts into the legacy duplex policy.
    kwargs.setdefault("allow_interruption", True)
    voice, llm, sink = VoiceProviderStub(), RecordingLLM(), RecordingSink()
    store = InMemoryConversationStore(20)
    actor = RealtimeSessionActor(
        VoiceSession("duplex-device", AudioFormat("opus", 16000, 1, 20),
                     AudioFormat("opus", 24000, 1, 20), 20),
        ProviderBundle(UnusedASR(), llm, FakeTTS(), voice), PassCodec(), PassCodec(), sink,
        conversation_store=store, **kwargs,
    )
    return actor, voice, llm, sink, store


async def until(predicate):
    async with asyncio.timeout(2):
        while not predicate():
            await asyncio.sleep(0.005)


async def event(actor, kind, **fields):
    await actor._ask("realtime_event", (
        actor.session.turn_id, actor.session.generation, {"type": kind, **fields},
    ))


async def question(actor, text):
    await event(actor, "conversation.item.input_audio_transcription.started")
    await event(actor, "conversation.item.input_audio_transcription.delta", delta=text[:1])
    await event(actor, "conversation.item.input_audio_transcription.delta", delta=text)
    await event(actor, "conversation.item.input_audio_transcription.completed", transcript=text)


def test_asr_snapshots_replace_partial_text_and_keep_spoken_repetitions():
    async def scenario():
        actor, _, _, sink, store = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await event(actor, "conversation.item.input_audio_transcription.started")
            snapshots = ["我", "我不知道", "我不知道呀", "我不知道呀", "我不知道聊什么"]
            for text in snapshots:
                await event(actor, "conversation.item.input_audio_transcription.delta", delta=text)
            assert [e["text"] for e in sink.controls if e["type"] == "asr.partial"] == snapshots
            await event(actor, "conversation.item.input_audio_transcription.completed",
                        text="我不知道该聊什么。")
            assert [e["text"] for e in sink.controls if e["type"] == "asr.final"] == [
                "我不知道该聊什么。"
            ]

            await event(actor, "conversation.item.input_audio_transcription.started")
            for text in ("你好", "你好你好"):
                await event(actor, "conversation.item.input_audio_transcription.delta", delta=text)
            await event(actor, "conversation.item.input_audio_transcription.completed")
            assert [e["text"] for e in sink.controls if e["type"] == "asr.final"][-1] == "你好你好"
            for text in ("哈", "哈"):
                await event(actor, "response.output_text.delta", response_id="r1", delta=text)
            await event(actor, "response.output_text.done", response_id="r1")
            await event(actor, "response.output_audio.done", response_id="r1")
            await until(lambda: sink.finished.is_set())
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == ["哈哈"]
            assert [m.content for m in await store.load("duplex-device")] == ["你好你好", "哈哈"]
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_text_uses_original_model_and_never_opens_realtime():
    async def scenario():
        actor, voice, llm, sink, _ = make_actor()
        await actor.start()
        try:
            await actor.submit_text(uuid4(), "文字问题")
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert llm.calls == [("文字问题",)]
            assert not voice.opened
            assert any(e.get("text") == "回答。" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_playback_cursor_follows_pcm_and_stops_on_interruption():
    async def scenario():
        actor, _, _, sink, _ = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, '说一段长回复')
            text = '先整理今天的工作，再喝一杯水，然后休息。' * 8
            await event(actor, 'response.output_text.delta', response_id='follow', delta=text)
            assert not [e for e in sink.controls if e['type'] == 'assistant.playback']
            await event(actor, 'response.output_audio.delta', response_id='follow',
                        delta=base64.b64encode(bytes(960 * 100)).decode())
            await event(actor, 'response.output_audio.done', response_id='follow')
            await until(lambda: sum(e['type'] == 'assistant.playback' for e in sink.controls) >= 2)
            cues = [e for e in sink.controls if e['type'] == 'assistant.playback']
            assert all(e['estimated'] is True and text.startswith(e['text']) for e in cues)
            assert 0 < len(cues[0]['text']) < len(cues[-1]['text']) < len(text)
            assert len({e['stream_id'] for e in cues}) == 1
            await question(actor, '停一下')
            count = len([e for e in sink.controls if e['type'] == 'assistant.playback'])
            await asyncio.sleep(0.05)
            assert len([e for e in sink.controls if e['type'] == 'assistant.playback']) == count
            assert actor._output_audio_bytes == 0
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_duplex_keeps_microphone_active_during_output_and_across_exchanges():
    async def scenario():
        actor, voice, llm, sink, store = make_actor()
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await actor.push_audio(stream, 0, 1000, bytes(640))
            await question(actor, "今天几号")
            await event(actor, "response.output_text.delta", response_id="r1", delta="今天周一。")
            await event(actor, "response.output_text.done", response_id="r1", text="今天周一。")
            await event(actor, "response.output_audio.delta", response_id="r1",
                        delta=base64.b64encode(bytes(960 * 12)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await actor.push_audio(stream, 1, 1020, bytes(640))
            await event(actor, "response.done", response_id="r1", usage={"total_tokens": 10})
            assert not sink.finished.is_set()
            await event(actor, "response.output_audio.done", response_id="r1")
            await until(lambda: sink.finished.is_set())
            assert len(voice.session.audio) == 2 and not llm.calls
            assert actor.session.turn_id == turn and actor.session.phase == SessionPhase.LISTENING
            assert [m.content for m in await store.load("duplex-device")] == ["今天几号", "今天周一。"]
            sink.finished.clear()
            await question(actor, "明天呢")
            await actor.push_audio(stream, 2, 1040, bytes(640))
            await event(actor, "response.output_text.delta", response_id="r2", delta="明天周二。")
            await event(actor, "response.output_audio.delta", response_id="r2",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="r2")
            await until(lambda: sink.finished.is_set())
            ids = [e["message_id"] for e in sink.controls if e["type"] == "input.started"]
            assert len(ids) == len(set(ids)) == 2
            assert len(await store.load("duplex-device")) == 4
            assert len(voice.opened) == 1
            assert len(sink.audio) == 13
        finally:
            await actor.close()
        assert voice.session.closed
        assert {"type": "response.cancel"} in voice.session.sent
    asyncio.run(scenario())


def test_barge_in_drops_buffered_and_late_audio_and_text_from_previous_response():
    async def scenario():
        actor, voice, _, sink, _ = make_actor()
        await actor.start()
        try:
            turn, stream = uuid4(), uuid4()
            await actor.begin_turn(turn, stream)
            await question(actor, "第一问")
            await event(actor, "response.output_text.delta", response_id="old", delta="旧回复")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(b"\x01\x00" * (480 * 60)).decode())
            await event(actor, "response.output_audio.done", response_id="old",
                        status_code="20000002")
            await until(lambda: len(sink.audio) >= 1)
            await question(actor, "第二问")
            count = len(sink.audio)
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_text.delta", response_id="old", delta="过期文本")
            await event(actor, "response.output_audio.done", response_id="old",
                        status_code="20000002")
            await event(actor, "response.output_text.delta", response_id="new", delta="新回复")
            await event(actor, "response.output_audio.delta", response_id="new",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="new")
            await until(lambda: sink.finished.is_set())
            assert len(sink.audio) == count + 1
            assert not any(e.get("text") == "过期文本" for e in sink.controls)
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == ["新回复"]
            assert actor.session.phase == SessionPhase.LISTENING
            assert not voice.session.closed
            await actor.push_audio(stream, 0, 0, bytes(640))
            await actor.cancel_turn(turn, "user_cancel")
            assert actor.session.phase == SessionPhase.IDLE and voice.session.closed
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("echo_text", [
    "小兔子走进森林，遇见了一只狐狸。",
    "小兔子走进森林，遇见了一只狐狸。小兔子走进森林。",
])
def test_native_echo_keeps_buffered_playback_and_rejects_echo_generated_responses(echo_text):
    async def scenario():
        actor, voice, _, sink, store = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "讲个故事")
            reply = "有一天，小兔子走进森林，遇见了一只狐狸。"
            await event(actor, "response.output_text.delta", response_id="story", delta=reply)
            await event(actor, "response.output_audio.delta", response_id="story",
                        delta=base64.b64encode(b"\x01\x00" * (480 * 25)).decode())
            await event(actor, "response.output_audio.done", response_id="story")
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            epoch = actor._output_epoch
            await event(actor, "conversation.item.input_audio_transcription.started")
            await event(actor, "conversation.item.input_audio_transcription.delta", delta="小兔子")
            assert actor.session.phase == SessionPhase.SPEAKING
            # The provider can start an echo-induced answer before ASR final.
            await event(actor, "response.output_text.delta", response_id="echo", delta="不该播出")
            await event(actor, "response.output_audio.started", response_id="echo")
            await event(actor, "response.output_audio.delta",
                        delta=base64.b64encode(b"\x02\x00" * 480).decode())
            await event(actor, "response.function_call_arguments.done",
                        items=[{"call_id": "echo-tool", "name": "device.test", "arguments": "{}"}])
            await event(actor, "response.canceled", response_id="story")
            await event(actor, "conversation.item.input_audio_transcription.completed",
                        transcript=echo_text)
            await event(actor, "response.output_audio.delta",
                        delta=base64.b64encode(b"\x02\x00" * 480).decode())
            await event(actor, "response.output_audio.done", response_id="echo")
            await event(actor, "response.output_text.delta", response_id="another-echo",
                        delta="也不该播出")
            await until(lambda: sink.finished.is_set())
            assert actor._output_epoch == epoch and len(sink.audio) == 25
            assert all(frame.payload == b"\x01\x00" * 8 for frame in sink.audio)
            assert [e["text"] for e in sink.controls if e["type"] == "asr.final"] == ["讲个故事"]
            assert sum(e["type"] == "input.started" for e in sink.controls) == 1
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == [reply]
            assert any(e["type"] == "assistant.playback" for e in sink.controls)
            assert not any(e["type"] in {"error", "audio.cancel"} for e in sink.controls)
            assert not any("不该播出" in e.get("text", "") for e in sink.controls)
            assert not voice.session.sent
            assert [m.content for m in await store.load("duplex-device")] == ["讲个故事", reply]
            sink.finished.clear()
            await question(actor, "现在几点")
            await event(actor, "response.output_text.delta", response_id="real", delta="现在八点。")
            await event(actor, "response.output_audio.delta", response_id="real",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="real")
            await until(lambda: sink.finished.is_set())
            assert [m.content for m in await store.load("duplex-device")] == [
                "讲个故事", reply, "现在几点", "现在八点。"]
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_short_genuine_final_releases_response_received_before_native_asr_final():
    async def scenario():
        actor, _, _, sink, store = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "讲个故事")
            await event(actor, "response.output_text.delta", response_id="old", delta="小兔子走进森林。")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960 * 100)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await event(actor, "conversation.item.input_audio_transcription.started")
            await event(actor, "conversation.item.input_audio_transcription.delta", delta="为")
            await event(actor, "response.output_text.delta", response_id="new", delta="因为它要找朋友。")
            await event(actor, "response.output_audio.started", response_id="new")
            await event(actor, "response.output_audio.delta",
                        delta=base64.b64encode(b"\x02\x00" * 960).decode())
            await event(actor, "response.output_audio.done", response_id="new")
            assert actor.session.phase == SessionPhase.SPEAKING
            count = len(sink.audio)
            await event(actor, "conversation.item.input_audio_transcription.completed",
                        transcript="为什么")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960)).decode())
            await until(lambda: sink.finished.is_set())
            assert len(sink.audio) == count + 2
            assert all(frame.payload == b"\x02\x00" * 8 for frame in sink.audio[count:])
            assert [e["text"] for e in sink.controls if e["type"] == "asr.final"] == [
                "讲个故事", "为什么"]
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == [
                "因为它要找朋友。"]
            assert [m.content for m in await store.load("duplex-device")] == ["为什么", "因为它要找朋友。"]
            assert not any(e["type"] == "error" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("text", ["停一下", "你好", "换个故事吧"])
def test_genuine_native_partial_interrupts_before_transcription_finishes(text):
    async def scenario():
        actor, _, _, sink, _ = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "讲个故事")
            await event(actor, "response.output_text.delta", response_id="old", delta="小兔子走进森林。")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960 * 100)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await event(actor, "conversation.item.input_audio_transcription.started")
            assert actor.session.phase == SessionPhase.SPEAKING
            await event(actor, "conversation.item.input_audio_transcription.delta", delta=text)
            assert actor.session.phase == SessionPhase.LISTENING
            count = len(sink.audio)
            await asyncio.sleep(0.03)
            assert len(sink.audio) == count
            assert sum(e["type"] == "input.started" for e in sink.controls) == 2
            assert [e["text"] for e in sink.controls if e["type"] == "asr.partial"][-1] == text
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_failed_unconfirmed_input_and_provider_cancel_drain_existing_audio():
    async def scenario():
        actor, _, _, sink, _ = make_actor()
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "讲个故事")
            await event(actor, "response.output_text.delta", response_id="story", delta="小兔子走进森林。")
            await event(actor, "response.output_audio.delta", response_id="story",
                        delta=base64.b64encode(bytes(960 * 10)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await event(actor, "conversation.item.input_audio_transcription.started")
            await event(actor, "conversation.item.input_audio_transcription.failed")
            await event(actor, "response.canceled", response_id="story")
            await until(lambda: sink.finished.is_set())
            assert len(sink.audio) == 10
            assert actor.session.phase == SessionPhase.LISTENING
            assert sum(e["type"] == "input.started" for e in sink.controls) == 1
            assert not any(e["type"] in {"error", "audio.cancel"} for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("native", [False, True])
def test_slow_monitor_cleanup_does_not_delay_interruption_or_microphone(native):
    class MonitorASR:
        def __init__(self):
            self.opened = asyncio.Event()
            self.abort_started = asyncio.Event()
            self.release_abort = asyncio.Event()
            self.closed = False

        async def open_stream(self, *_args):
            self.opened.set()
            return self

        async def push(self, _pcm):
            return () if native else (TranscriptEvent("停一下"),)

        async def abort(self):
            self.abort_started.set()
            await self.release_abort.wait()
            self.closed = True

    async def scenario():
        monitor = MonitorASR()
        actor, voice, _, sink, _ = make_actor(interruption_asr=monitor)
        await actor.start()
        stream = uuid4()
        trigger = None
        try:
            await actor.begin_turn(uuid4(), stream)
            await question(actor, "讲故事")
            await event(actor, "response.output_text.delta", response_id="old",
                        delta="小兔子走进森林。")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960 * 300)).decode())
            await monitor.opened.wait()
            if native:
                await event(actor, "conversation.item.input_audio_transcription.started")
                trigger = asyncio.create_task(event(
                    actor, "conversation.item.input_audio_transcription.delta", delta="现在几点了"))
            else:
                await actor.push_audio(stream, 0, 0, bytes(640))
            async with asyncio.timeout(1):
                await monitor.abort_started.wait()
                while sum(e["type"] == "input.started" for e in sink.controls) < 2:
                    await asyncio.sleep(0)
                if trigger is not None:
                    await trigger
                # Keep cleanup blocked while requiring the next microphone
                # frame to reach the native duplex provider.
                await actor.push_audio(stream, 0 if native else 1,
                                       0 if native else 20, bytes(640))
            assert actor.session.phase == SessionPhase.LISTENING
            assert len(voice.session.audio) == (1 if native else 2)
            assert not monitor.closed
            frames = len(sink.audio)
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960)).decode())
            assert len(sink.audio) == frames
        finally:
            monitor.release_abort.set()
            if trigger is not None:
                await asyncio.gather(trigger, return_exceptions=True)
            await actor.close()
        assert monitor.closed
    asyncio.run(scenario())


def test_speech_stop_works_when_duplex_asr_misses_and_unkeyed_old_audio_stays_cancelled():
    class MonitorASR:
        def __init__(self):
            self.received = []
            self.aborted = False

        async def open_stream(self, *_args):
            return self

        async def push(self, pcm):
            self.received.append(pcm)
            return (TranscriptEvent("停一下，现在几点"),)

        async def abort(self):
            self.aborted = True

    async def scenario():
        monitor = MonitorASR()
        actor, voice, _, sink, _ = make_actor(interruption_asr=monitor)
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await question(actor, "慢慢数到一百")
            await event(actor, "response.output_text.delta", response_id="old", delta="一二三四五")
            await event(actor, "response.output_audio.delta", response_id="old",
                        delta=base64.b64encode(bytes(960 * 300)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await event(actor, "conversation.item.input_audio_transcription.started")
            await event(actor, "conversation.item.input_audio_transcription.delta", delta="一二三")
            await event(actor, "response.output_text.delta", response_id="echo", delta="不能播出的回复")
            pcm = b"\x02\x00" * 320
            await actor.push_audio(stream, 0, 0, pcm)
            await until(lambda: {"type": "response.cancel"} in voice.session.sent)
            assert actor.session.phase == SessionPhase.LISTENING
            assert monitor.aborted and monitor.received == [pcm]
            assert voice.session.audio == [pcm]
            count = len(sink.audio)
            # These are real provider packet shapes: late deltas can lack an id.
            for _ in range(3):
                await event(actor, "response.output_audio.delta",
                            delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.canceled")
            await actor._ask("speech_stop", (
                turn, actor.session.generation, actor._output_epoch - 1))
            assert actor.session.phase == SessionPhase.LISTENING
            assert len(sink.audio) == count
            assert voice.session.sent.count({"type": "response.cancel"}) == 1
            await actor.push_audio(stream, 1, 20, pcm)
            assert len(voice.session.audio) == 2
            await question(actor, "现在几点")
            await event(actor, "response.output_audio.started", response_id="new")
            await event(actor, "response.output_text.delta", response_id="old", delta="迟到的旧回复")
            await event(actor, "response.output_audio.delta",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="new")
            await until(lambda: sink.finished.is_set())
            assert len(sink.audio) == count + 1
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("exit_fields", [
    {"status_code": "20000002"},
    {"status_code": 20000002},
    {"user_query_exit": True},
    {"extension": {"user_query_exit": True}},
])
def test_exit_intent_closes_conversation_only_after_goodbye_audio_finishes(exit_fields):
    async def scenario():
        actor, voice, _, sink, store = make_actor()
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await question(actor, "不聊了，再见。")
            await event(actor, "response.output_text.delta", response_id="goodbye", delta="下次再聊。")
            await event(actor, "response.output_audio.delta", response_id="goodbye",
                        delta=base64.b64encode(bytes(960 * 15)).decode())
            await event(actor, "response.output_audio.done", response_id="goodbye", **exit_fields)
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            assert not voice.session.closed
            assert not any(e["type"] == "turn.cancelled" for e in sink.controls)
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert len(sink.audio) == 15
            assert voice.session.closed
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == [
                "下次再聊。"
            ]
            kinds = [e["type"] for e in sink.controls]
            assert kinds.index("audio.end") < kinds.index("assistant.final")
            assert kinds.index("assistant.final") < kinds.index("turn.cancelled")
            cancelled = [e for e in sink.controls if e["type"] == "turn.cancelled"]
            assert cancelled[0]["reason"] == "user_exit"
            assert [m.content for m in await store.load("duplex-device")] == [
                "不聊了，再见。", "下次再聊。"
            ]
            assert not any(e["type"] == "error" for e in sink.controls)
            await actor.push_audio(stream, 0, 0, bytes(640))
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("done_fields", [
    {}, {"status_code": "20000000"}, {"status_code": 20000000},
    {"user_query_exit": False}, {"extension": {"user_query_exit": False}},
])
def test_normal_reply_does_not_end_conversation(done_fields):
    async def scenario():
        actor, voice, _, sink, _ = make_actor()
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await question(actor, "今天心情怎么样？")
            await event(actor, "response.output_text.delta", response_id="normal", delta="挺好的。")
            await event(actor, "response.output_audio.delta", response_id="normal",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="normal", **done_fields)
            await until(lambda: sink.finished.is_set())
            assert actor.session.phase == SessionPhase.LISTENING
            assert not voice.session.closed
            assert not any(e["type"] in {"turn.cancelled", "error"} for e in sink.controls)
            await actor.push_audio(stream, 0, 0, bytes(640))
            assert len(voice.session.audio) == 1
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_song_burst_over_ten_seconds_and_256_chunks_plays_completely_at_realtime():
    async def scenario():
        actor, voice, _, sink, _ = make_actor()
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await question(actor, "唱一首歌")
            await event(actor, "response.output_text.delta", response_id="song", delta="歌词")
            # 300 small provider chunks represent 12 seconds of audio.
            chunk = base64.b64encode(bytes(1920)).decode()
            for _ in range(300):
                await event(actor, "response.output_audio.delta", response_id="song", delta=chunk)
            await event(actor, "response.output_audio.done", response_id="song")
            await actor.push_audio(stream, 0, 0, bytes(640))
            async with asyncio.timeout(15):
                await sink.finished.wait()
            assert len(sink.audio) == 600
            assert [frame.sequence for frame in sink.audio] == list(range(600))
            assert sink.audio_sent_at[-1] - sink.audio_sent_at[0] >= 11
            assert actor._buffered_bytes == 0
            assert actor.session.phase == SessionPhase.LISTENING
            assert len(voice.session.audio) == 1 and not voice.session.closed
            assert not any(e["type"] == "error" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("failure", [False, True])
def test_retired_uplink_is_ignored_after_stop_or_failure_and_during_next_turn(failure):
    async def scenario():
        actor, voice, _, sink, _ = make_actor()
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            if failure:
                await voice.session.events.put(ProviderFailure("upstream failed"))
                await until(lambda: actor.session.phase == SessionPhase.IDLE)
            else:
                await actor.cancel_turn(turn, "user_cancel")
            assert voice.session.closed
            errors = sum(e["type"] == "error" for e in sink.controls)
            for sequence in range(10):
                await actor.push_audio(stream, sequence, sequence * 20, bytes(640))
            await actor.end_audio(turn, stream)
            with pytest.raises(StateConflict):
                await actor.push_audio(uuid4(), 0, 0, bytes(640))

            voice.session = VoiceSessionStub()
            new_turn, new_stream = uuid4(), uuid4()
            await actor.begin_turn(new_turn, new_stream)
            await actor.push_audio(stream, 10, 200, bytes(640))
            await actor.end_audio(turn, stream)
            await actor.push_audio(new_stream, 0, 0, bytes(640))
            assert len(voice.session.audio) == 1
            assert actor.session.turn_id == new_turn
            assert sum(e["type"] == "error" for e in sink.controls) == errors
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_buffer_byte_limit_still_fails_cleanly_and_discards_late_microphone_frames():
    async def scenario():
        actor, voice, _, sink, _ = make_actor(max_output_buffer_seconds=0.1)
        await actor.start()
        turn, stream = uuid4(), uuid4()
        try:
            await actor.begin_turn(turn, stream)
            await question(actor, "唱歌")
            await event(actor, "response.output_audio.delta", response_id="song",
                        delta=base64.b64encode(bytes(4802)).decode())
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            errors = [e for e in sink.controls if e["type"] == "error"]
            assert len(errors) == 1 and "buffered audio limit" in errors[0]["message"]
            assert voice.session.closed and actor._buffered_bytes == 0
            assert not sink.audio
            await actor.push_audio(stream, 0, 0, bytes(640))
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_inactivity_does_not_cancel_buffered_song_playback():
    async def scenario():
        actor, voice, _, sink, _ = make_actor(idle_timeout_seconds=0.03)
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "唱歌")
            await event(actor, "response.output_text.delta", response_id="song", delta="歌词")
            await event(actor, "response.output_audio.delta", response_id="song",
                        delta=base64.b64encode(bytes(960 * 10)).decode())
            await event(actor, "response.output_audio.done", response_id="song")
            await until(lambda: sink.finished.is_set())
            assert len(sink.audio) == 10
            assert not voice.session.closed
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert voice.session.closed
            assert not any(e["type"] == "error" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_stalled_output_still_times_out_after_playable_audio_is_drained():
    async def scenario():
        actor, voice, _, sink, _ = make_actor(idle_timeout_seconds=0.03)
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "唱歌")
            await event(actor, "response.output_audio.delta", response_id="song",
                        delta=base64.b64encode(bytes(962)).decode())
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert len(sink.audio) == 1
            assert voice.session.closed
            assert not sink.finished.is_set()
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_function_calls_keep_ids_tool_policy_and_do_not_repeat_side_effects():
    async def scenario():
        actor, voice, _, sink, store = make_actor()
        executions = []
        tools = ToolRegistry()

        async def execute(context: ToolContext, arguments):
            await asyncio.sleep(0.02)
            executions.append((context, arguments))
            return {"success": True}

        tools.register(ToolSpec(
            "device.test", "test", {"type": "object"}, "device.control", execute,
        ))
        actor._agent_runtime = AgentRuntime(store, tools, 20, require_control_approval=True)
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "打开灯")
            items = [{"call_id": "call-1", "name": "device.test", "arguments": '{"index":1}'},
                     {"call_id": "call-2", "name": "device.test", "arguments": "invalid"}]
            await event(actor, "response.function_call_arguments.done", items=items)
            await event(actor, "response.function_call_arguments.done", items=items)
            await until(lambda: len(voice.session.sent) == 2)
            assert len(executions) == 1
            context, arguments = executions[0]
            assert context.approved and context.source == "voice"
            assert context.device_id == "duplex-device"
            assert arguments == {"index": 1}
            for sent in voice.session.sent:
                assert sent["type"] == "conversation.item.create"
                assert [i["call_id"] for i in sent["items"]] == ["call-1", "call-2"]
                assert json.loads(sent["items"][0]["content"][0]["text"]) == {"success": True}
                assert "error" in json.loads(sent["items"][1]["content"][0]["text"])
            await event(actor, "response.function_call_arguments.done", items=[{
                "call_id": "call-1", "name": "device.test", "arguments": '{"index":2}',
            }])
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert len(executions) == 1 and voice.session.closed
            assert any(e["type"] == "error" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())


def test_inactive_voice_session_returns_to_idle():
    async def scenario():
        actor, voice, _, sink, _ = make_actor(idle_timeout_seconds=0.03)
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await until(lambda: actor.session.phase == SessionPhase.IDLE)
            assert voice.session.closed
            assert any(e.get("reason") == "voice_inactivity" for e in sink.controls)
            assert not any(e["type"] == "error" for e in sink.controls)
        finally:
            await actor.close()
    asyncio.run(scenario())
