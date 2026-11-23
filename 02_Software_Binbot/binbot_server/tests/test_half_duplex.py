import asyncio
import base64
from uuid import uuid4

import pytest

from binbot_server.domain.model import SessionPhase
from test_realtime_session import UnusedASR, event, make_actor, question, until


@pytest.mark.parametrize("overlap", ["停一下", "我想问另一个问题"])
def test_half_duplex_finishes_reply_then_resumes_the_same_conversation(overlap):
    async def scenario():
        actor, voice, _, sink, store = make_actor(
            allow_interruption=False, interruption_asr=UnusedASR(),
        )
        await actor.start()
        turn, stream = uuid4(), uuid4()
        pcm = b"\x01\x00" * 320
        try:
            await actor.begin_turn(turn, stream)
            await actor.push_audio(stream, 0, 1000, pcm)
            await question(actor, "讲个故事")
            assert actor.session.phase == SessionPhase.THINKING
            await actor.push_audio(stream, 1, 1020, pcm)
            assert voice.session.audio == [pcm]

            await event(actor, "response.output_text.delta", response_id="story", delta="完整的故事。")
            await event(actor, "response.output_audio.delta", response_id="story",
                        delta=base64.b64encode(bytes(960 * 12)).decode())
            await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            assert actor._stop_monitor is None
            await actor._ask("speech_stop", (turn, actor.session.generation, actor._output_epoch))
            await question(actor, overlap)
            await actor.push_audio(stream, 2, 1040, pcm)
            await event(actor, "response.output_text.delta", response_id="overlap", delta="不应播放")
            await event(actor, "response.output_audio.delta",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="story")
            await until(lambda: any(e["type"] == "audio.end" for e in sink.controls))
            assert actor.session.phase == SessionPhase.SPEAKING
            assert voice.session.input_mutes == [True]
            await actor.push_audio(stream, 3, 1060, pcm)
            assert voice.session.audio == [pcm]
            await until(lambda: actor.session.phase == SessionPhase.LISTENING)

            # Recognition or output from the overlap can arrive after playback.
            await event(actor, "conversation.item.input_audio_transcription.completed", transcript=overlap)
            await event(actor, "response.output_text.delta", response_id="late-overlap", delta="不应播放")
            await event(actor, "response.output_text.done", response_id="story", text="迟到的文字")
            await event(actor, "response.output_audio.delta", response_id="story",
                        delta=base64.b64encode(bytes(960)).decode())
            assert [e["text"] for e in sink.controls if e["type"] == "asr.final"] == ["讲个故事"]
            assert [e["text"] for e in sink.controls if e["type"] == "assistant.final"] == ["完整的故事。"]
            assert not [e for e in sink.controls if e["type"] == "audio.cancel"]
            assert not voice.session.sent
            assert voice.session.input_mutes == [True, False]
            assert len(sink.audio) == 12

            await actor.push_audio(stream, 4, 1080, pcm)
            assert voice.session.audio == [pcm, pcm]
            await question(actor, "后来呢")
            await event(actor, "response.output_text.delta", response_id="next", delta="后来找到了朋友。")
            await event(actor, "response.output_audio.delta", response_id="next",
                        delta=base64.b64encode(bytes(960)).decode())
            await event(actor, "response.output_audio.done", response_id="next")
            await until(lambda: actor.session.phase == SessionPhase.LISTENING)
            assert actor.session.turn_id == turn and actor.session.uplink_stream_id == stream
            assert len(voice.opened) == 1
            assert voice.session.input_mutes == [True, False, True, False]
            assert sum(e["type"] == "input.started" for e in sink.controls) == 2
            assert [m.content for m in await store.load("duplex-device")] == [
                "讲个故事", "完整的故事。", "后来呢", "后来找到了朋友。",
            ]
        finally:
            await actor.close()
    asyncio.run(scenario())


@pytest.mark.parametrize("audio_frames", [0, 12])
def test_half_duplex_drains_buffered_audio_on_upstream_cancel_without_getting_stuck(audio_frames):
    async def scenario():
        actor, voice, _, sink, _ = make_actor(allow_interruption=False)
        await actor.start()
        try:
            await actor.begin_turn(uuid4(), uuid4())
            await question(actor, "你好")
            if audio_frames:
                await event(actor, "response.output_text.delta", response_id="reply", delta="你好呀。")
                await event(actor, "response.output_audio.delta", response_id="reply",
                            delta=base64.b64encode(bytes(960 * audio_frames)).decode())
                await until(lambda: actor.session.phase == SessionPhase.SPEAKING)
            await event(actor, "response.canceled", response_id="reply")
            await until(lambda: actor.session.phase == SessionPhase.LISTENING)
            assert len(sink.audio) == audio_frames
            assert not [e for e in sink.controls if e["type"] == "audio.cancel"]
            assert voice.session.input_mutes == [True, False]
        finally:
            await actor.close()
    asyncio.run(scenario())
