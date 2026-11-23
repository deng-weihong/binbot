from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable
from difflib import SequenceMatcher

from binbot_server.domain.model import AudioFormat, TurnContext

from .ports import ASRProvider

_LOG = logging.getLogger(__name__)
_STOP_PHRASES = ("停一下", "停一停", "停下", "暂停", "停止", "等一下", "等一等", "别说了", "不要说了", "别数了")
_NUMBER_CHARS = frozenset("0123456789零〇一二三四五六七八九十百千万亿两")


def _text(text: str) -> str:
    return "".join(char for char in text if char.isalnum()).casefold().replace("1", "一")


def stop_request(text: str, assistant_text: str) -> bool:
    """Only explicit stop requests absent from the assistant's own speech qualify."""
    text, echo = _text(text), _text(assistant_text)
    return any(phrase in text and phrase not in echo for phrase in _STOP_PHRASES)


def _speech_gaps(heard: str, echo: str, *, minimum: int = 3) -> list[str]:
    # A single global alignment assumes playback is transcribed in order. AEC
    # residuals can repeat or skip earlier words, leaving a previously matched
    # phrase falsely marked as new. Match each remaining gap independently.
    pending, gaps = [heard], []
    while pending:
        fragment = pending.pop()
        if not fragment or fragment in echo:
            continue
        blocks = [block for block in SequenceMatcher(
            None, fragment, echo, autojunk=False).get_matching_blocks() if block.size >= minimum]
        if not blocks:
            gaps.append(fragment)
            continue
        remainder, end = [], 0
        for block in blocks:
            remainder.append(fragment[end:block.a])
            end = block.a + block.size
        remainder.append(fragment[end:])
        pending.extend(reversed(remainder))
    return gaps


def playback_echo(text: str, assistant_text: str) -> bool:
    """Recognize playback transcriptions, including number formatting/ASR edits."""
    heard, echo = _text(text), _text(assistant_text)
    if not heard or not echo:
        return False
    if heard in echo:
        return True
    # Spoken numbers alternate between digits and Chinese numerals in ASR.
    heard_words = "".join(char for char in heard if char not in _NUMBER_CHARS)
    echo_words = "".join(char for char in echo if char not in _NUMBER_CHARS)
    if not heard_words:
        return any(char in _NUMBER_CHARS for char in echo)
    if heard_words in echo_words:
        return True
    unmatched = sum(len(gap) for gap in _speech_gaps(heard_words, echo_words))
    # Scattered common characters alone are insufficient. Require substantial
    # spans covering most of the utterance, with no genuinely new phrase.
    return (len(heard_words) - unmatched >= len(heard_words) * 0.8
            and not novel_speech(text, assistant_text))


def novel_speech(text: str, assistant_text: str) -> str:
    """Require a greeting or a new phrase beyond recognizable playback echo."""
    heard = "".join(char for char in _text(text) if char not in _NUMBER_CHARS)
    echo = "".join(char for char in _text(assistant_text) if char not in _NUMBER_CHARS)
    if not heard or not assistant_text or heard in echo:
        return ""
    # ASR may write spoken numbers as digits, split a sentence differently, or
    # mishear individual characters. Remove substantial matching spans; short
    # gaps left by those corrections must not count as a new utterance.
    gaps = _speech_gaps(heard, echo)
    mostly_echo = len(heard) >= 12 and sum(map(len, gaps)) <= len(heard) * 0.2
    for gap in gaps:
        for greeting in ("你好", "您好", "喂喂"):
            if greeting in gap and greeting not in echo:
                return greeting
        # A partial dominated by recognized playback can stitch two short,
        # previously spoken fragments together (e.g. “底下” + “深秋”). Only
        # discard it when every remaining fragment also occurs in playback;
        # retain the whole gap if any part is new, including short questions.
        if mostly_echo and not _speech_gaps(gap, echo, minimum=2):
            continue
        if len(gap) >= 4:
            return gap
    return ""


class SpeechStopMonitor:
    """Recognize interruptions without putting ASR I/O on the duplex audio path."""

    def __init__(
        self, provider: ASRProvider, assistant_text: Callable[[], str],
        stopped: Callable[[str], Awaitable[None]],
    ):
        self._provider = provider
        self._assistant_text = assistant_text
        self._stopped = stopped
        self._queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=100)
        self._task: asyncio.Task[None] | None = None
        self._closing = False

    def start(self, context: TurnContext) -> None:
        if self._task is not None:
            raise RuntimeError("stop monitor is already started")
        self._closing = False
        self._task = asyncio.create_task(self._run(context), name="duplex-stop-asr")

    def feed(self, pcm: bytes) -> None:
        if self._closing or self._task is None or self._task.done():
            return
        try:
            self._queue.put_nowait(pcm)
        except asyncio.QueueFull:
            _LOG.warning("stop monitor disabled after two seconds of ASR backpressure")
            self._task.cancel()

    async def close(self) -> None:
        task, self._task = self._task, None
        if task is not None:
            # A detected interruption already entered its bounded cleanup.
            # Do not cancel that cleanup a second time and leak the stream.
            if not self._closing:
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def _run(self, context: TurnContext) -> None:
        stream = None
        frames = 0
        pending_text = pending_phrase = ""
        pending_since = 0.0
        loop = asyncio.get_running_loop()
        try:
            while True:
                if stream is None:
                    async with asyncio.timeout(4):
                        stream = await self._provider.open_stream(
                            context, AudioFormat("pcm_s16le", 16000, 1, 20))
                pcm = await self._queue.get()
                async with asyncio.timeout(2):
                    events = await stream.push(pcm)
                detected = ""
                for event in events:
                    if stop_request(event.text, self._assistant_text()):
                        detected = event.text
                        break
                    phrase = novel_speech(event.text, self._assistant_text())
                    if phrase:
                        if not pending_phrase or not (
                            pending_phrase in phrase or phrase in pending_phrase
                        ):
                            pending_since = loop.time()
                        pending_text, pending_phrase = event.text, phrase
                    else:
                        pending_text = pending_phrase = ""
                if (not detected and pending_phrase and loop.time() - pending_since >= 0.3
                        and novel_speech(pending_text, self._assistant_text())):
                    detected = pending_text
                if detected:
                    # Notify playback immediately; provider shutdown belongs
                    # to finally and can take seconds on a slow connection.
                    await self._stopped(detected)
                    return
                frames += 1
                # Stay within streaming ASR's per-request duration limit.
                if frames >= 1500:
                    async with asyncio.timeout(2):
                        await stream.abort()
                    stream = None
                    frames = 0
                    pending_text = pending_phrase = ""
        except asyncio.CancelledError:
            raise
        except Exception:
            _LOG.warning("stop monitor unavailable; duplex audio remains active", exc_info=True)
        finally:
            self._closing = True
            if stream is not None:
                try:
                    async with asyncio.timeout(2):
                        await stream.abort()
                except Exception:
                    _LOG.warning("stop monitor ASR close failed")
