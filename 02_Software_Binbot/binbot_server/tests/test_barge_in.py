import asyncio
from uuid import uuid4

import pytest

from binbot_server.application.barge_in import (
    SpeechStopMonitor, novel_speech, playback_echo, stop_request,
)
from binbot_server.application.ports import TranscriptEvent
from binbot_server.domain.model import TurnContext


@pytest.mark.parametrize(("text", "assistant", "expected"), [
    ("停，一下，现在几点", "一二三四五", True),
    ("等一下等一下", "继续讲故事", True),
    ("暂停", "今天的天气晴朗", True),
    ("停下", "一二三四五", True),
    ("停1下", "一二三四五", True),
    ("停1下", "你可以说停一下", False),
    ("我说“暂停”后会继续", "我说暂停后会继续", False),
    ("11十二十三", "十一，十二，十三", False),
    ("停", "继续讲", False),
])
def test_only_explicit_commands_absent_from_assistant_speech_trigger(text, assistant, expected):
    assert stop_request(text, assistant) == expected


@pytest.mark.parametrize(("text", "assistant", "expected"), [
    ("9十11，12 13。", "好的，九、十、十一、十二、十三。", ""),
    ("今天天气不搓，我们去公园走走。", "今天天气不错，我们去公园走走。", ""),
    ("小刺猬走进森林，小刺猬走进森林。", "小刺猬走进森林。", ""),
    ("萤火虫找到了妈妈。小刺猬走进森林。", "小刺猬走进森林，萤火虫找到了妈妈。", ""),
    ("小刺猬走进森林，现在几点了，小刺猬走进森林。", "小刺猬走进森林。", "现在几点了"),
    ("你好，现在几点了？", "好的，一、二、三。", "你好"),
    ("今天天气不错。现在几点了？", "今天天气不错，我们去公园走走。", "现在几点了"),
    ("你好", "你好，请问需要什么帮助？", ""),
    ("现在几点了", "", ""),
])
def test_novel_speech_filters_echo_and_number_formatting(text, assistant, expected):
    assert novel_speech(text, assistant) == expected


@pytest.mark.parametrize(("text", "assistant", "expected"), [
    ("小兔子走进森林。", "有一天，小兔子走进森林，遇见了一只狐狸。", True),
    ("今天天气不搓，我们去公园走走。", "今天天气不错，我们去公园走走。", True),
    ("9十11，12 13。", "好的，九、十、十一、十二、十三。", True),
    ("还有2只小兔子", "还有两只小兔子", True),
    ("小刺猬走进森林，小刺猬走进森林。", "小刺猬走进森林。", True),
    ("萤火虫找到了妈妈。小刺猬走进森林。", "小刺猬走进森林，萤火虫找到了妈妈。", True),
    ("小刺猬走进森林，现在几点了，小刺猬走进森林。", "小刺猬走进森林。", False),
    ("为什么", "小兔子走进森林", False),
    ("你好", "小兔子走进森林", False),
    ("换一个", "有一天，小兔子走进森林，遇见了一只狐狸。", False),
    ("今天天气不错。现在几点了？", "今天天气不错，我们去公园走走。", False),
    ("", "继续讲故事", False),
    ("现在几点了", "", False),
])
def test_playback_echo_preserves_new_short_questions(text, assistant, expected):
    assert playback_echo(text, assistant) == expected


@pytest.mark.parametrize("heard", [
    ("顺口的老槐树底下住着一只叫融团的小刺猬，他总觉得自己的刺太扎人，"
     "没人愿意和他老槐树底下深秋"),
    ("顺口的老槐树底下住着一只叫融团的小刺猬，他总觉得自己的刺太扎人，"
     "没人愿意和他老槐树底下深秋的一个晚上。"),
])
def test_recorded_echo_with_asr_edits_and_reordered_words_is_not_new_speech(heard):
    assistant = ("巷口的老槐树底下，住着一只叫绒团的小刺猬，它总觉得自己的刺太扎人，"
                 "没人愿意和它玩。深秋的一个晚上。")
    assert novel_speech(heard, assistant) == ""
    assert playback_echo(heard, assistant)
    assert novel_speech(heard + "你好现在几点了", assistant) == "你好"
    assert "现在几点了" in novel_speech(heard + "现在几点了", assistant)
    assert not playback_echo(heard + "现在几点了", assistant)


def test_blocked_asr_does_not_block_duplex_and_closes_on_shutdown():
    class SlowASR:
        def __init__(self):
            self.push_started = asyncio.Event()
            self.closed = False

        async def open_stream(self, *_args):
            return self

        async def push(self, _pcm):
            self.push_started.set()
            await asyncio.Event().wait()

        async def abort(self):
            self.closed = True

    async def scenario():
        asr = SlowASR()
        fired = []

        async def stopped(text):
            fired.append(text)

        monitor = SpeechStopMonitor(asr, lambda: "", stopped)
        monitor.start(TurnContext(uuid4(), uuid4(), "device"))
        try:
            monitor.feed(bytes(640))
            await asr.push_started.wait()
            for _ in range(101):
                monitor.feed(bytes(640))
            await asyncio.sleep(0)
            assert not fired
        finally:
            await monitor.close()
        assert asr.closed
    asyncio.run(scenario())


def test_monitor_ignores_assistant_quoted_command_then_detects_distinct_stop():
    class ASR:
        async def open_stream(self, *_args):
            return self

        async def push(self, pcm):
            return (TranscriptEvent("停一下" if pcm[0] == 0 else "等一下"),)

        async def abort(self):
            pass

    async def scenario():
        fired = asyncio.Event()

        async def stopped(text):
            assert text == "等一下"
            fired.set()

        monitor = SpeechStopMonitor(ASR(), lambda: "你可以说停一下", stopped)
        monitor.start(TurnContext(uuid4(), uuid4(), "device"))
        try:
            monitor.feed(bytes(640))
            await asyncio.sleep(0.01)
            assert not fired.is_set()
            monitor.feed(b"\x01" * 640)
            async with asyncio.timeout(1):
                await fired.wait()
        finally:
            await monitor.close()
    asyncio.run(scenario())


def test_recognized_stop_survives_asr_cleanup_failure():
    class ASR:
        async def open_stream(self, *_args):
            return self

        async def push(self, _pcm):
            return (TranscriptEvent("停一下"),)

        async def abort(self):
            raise OSError("connection already closed")

    async def scenario():
        fired = asyncio.Event()

        async def stopped(text):
            assert text == "停一下"
            fired.set()

        monitor = SpeechStopMonitor(ASR(), lambda: "", stopped)
        monitor.start(TurnContext(uuid4(), uuid4(), "device"))
        try:
            monitor.feed(bytes(640))
            async with asyncio.timeout(1):
                await fired.wait()
        finally:
            await monitor.close()
    asyncio.run(scenario())


def test_recognized_stop_is_reported_before_slow_asr_cleanup():
    class ASR:
        def __init__(self):
            self.abort_started = asyncio.Event()
            self.release_abort = asyncio.Event()
            self.closed = False

        async def open_stream(self, *_args):
            return self

        async def push(self, _pcm):
            return (TranscriptEvent("停一下"),)

        async def abort(self):
            self.abort_started.set()
            await self.release_abort.wait()
            self.closed = True

    async def scenario():
        asr = ASR()
        fired = asyncio.Event()

        async def stopped(text):
            assert text == "停一下"
            fired.set()

        monitor = SpeechStopMonitor(asr, lambda: "继续讲故事", stopped)
        monitor.start(TurnContext(uuid4(), uuid4(), "device"))
        try:
            monitor.feed(bytes(640))
            async with asyncio.timeout(1):
                await asr.abort_started.wait()
            assert fired.is_set(), "interruption must precede ASR cleanup"
        finally:
            asr.release_abort.set()
            await monitor.close()
        assert asr.closed
    asyncio.run(scenario())


def test_a_stable_new_question_interrupts_without_an_explicit_stop_word():
    class ASR:
        first = True

        async def open_stream(self, *_args):
            return self

        async def push(self, _pcm):
            if self.first:
                self.first = False
                return (TranscriptEvent("现在几点了"),)
            return ()

        async def abort(self):
            pass

    async def scenario():
        fired = asyncio.Event()

        async def stopped(text):
            assert text == "现在几点了"
            fired.set()

        monitor = SpeechStopMonitor(ASR(), lambda: "好的，一二三四五", stopped)
        monitor.start(TurnContext(uuid4(), uuid4(), "device"))
        try:
            monitor.feed(bytes(640))
            await asyncio.sleep(0.02)
            assert not fired.is_set()
            async with asyncio.timeout(1):
                while not fired.is_set():
                    monitor.feed(bytes(640))
                    await asyncio.sleep(0.02)
        finally:
            await monitor.close()
    asyncio.run(scenario())
