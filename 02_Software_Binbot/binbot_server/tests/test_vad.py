from pathlib import Path

import numpy as np
import pytest
from numpy.typing import NDArray

from binbot_server.config import VADConfig
from binbot_server.infrastructure.vad import SileroEndpointDetector, SileroVAD


@pytest.fixture(scope="module")
def model() -> SileroVAD:
    return SileroVAD(VADConfig())


def speech_with_pause(gain: float) -> NDArray[np.int16]:
    speech = np.fromfile(Path(__file__).with_name("data") / "quiet-speech-16k.pcm", dtype="<i2")
    clip = np.concatenate((np.zeros(16000), speech * gain, np.zeros(19200), speech * gain))
    rng = np.random.default_rng(17)
    return (clip + rng.integers(-1000, 1001, len(clip))).astype("<i2")


@pytest.mark.parametrize("gain", [1.0, 0.7, 0.5])
def test_quiet_speech_and_sentence_pause_are_not_cut(model: SileroVAD, gain: float) -> None:
    detector = model.new_stream()
    speech = speech_with_pause(gain)
    for pos in range(0, len(speech), 320):
        assert detector.process(speech[pos:pos + 320].tobytes()) is None
    rng = np.random.default_rng(42)
    tail = rng.integers(-1000, 1001, 48000, dtype=np.int16)
    ended_at = None
    for pos in range(0, len(tail), 320):
        reason = detector.process(tail[pos:pos + 320].astype("<i2").tobytes())
        if reason:
            assert reason == "speech_ended"
            ended_at = pos // 16
            break
    assert ended_at is not None and 1500 <= ended_at <= 2800


def test_no_speech_timeout_and_per_turn_reset(model: SileroVAD) -> None:
    detector = model.new_stream()
    speech = speech_with_pause(0.7)
    assert detector.process(speech.tobytes()) is None
    detector.reset()
    for _ in range(500):
        assert detector.process(bytes(640)) is None
    assert detector.process(bytes(640)) == "no_speech"


def test_stream_states_are_independent(model: SileroVAD) -> None:
    speech = model.new_stream()
    idle = model.new_stream()
    for frame in speech_with_pause(0.7).reshape(-1, 320):
        assert speech.process(frame.tobytes()) is None
        assert idle.process(bytes(640)) is None
    assert speech.process(bytes(64000)) == "speech_ended"
    assert idle.process(bytes(64000)) == "no_speech"


def test_intermediate_probability_preserves_speech_state() -> None:
    class Scores:
        def __init__(self) -> None:
            self.scores = iter([0.9] * 3 + [0.4] * 70 + [0.1] * 70)

        def run(self, outputs: object, inputs: dict[str, object]) -> list[object]:
            return [np.array([[next(self.scores)]]), inputs["state"]]

    detector = SileroEndpointDetector(Scores(), VADConfig())  # type: ignore[arg-type]
    for _ in range(73):
        assert detector.process(bytes(1024)) is None
    assert detector.process(bytes(1024 * 70)) == "speech_ended"


def test_invalid_pcm_is_rejected(model: SileroVAD) -> None:
    with pytest.raises(ValueError, match="16-bit PCM"):
        model.new_stream().process(b"x")


def test_asr_progress_keeps_quiet_speech_past_initial_deadline(model: SileroVAD) -> None:
    detector = model.new_stream()
    for frame in range(600):
        text = f"quiet word {frame}" if frame % 50 == 0 else None
        assert detector.process(bytes(640), text) is None
    for frame in range(120):
        reason = detector.process(bytes(640))
        if reason is not None:
            break
    assert reason == "speech_ended"
    assert 1360 <= frame * 20 <= 1440


@pytest.mark.parametrize("progress,old_endpoint_ms", [
    ([1420, 1660, 2040], 2540),
    ([1700, 2040, 2240, 2620, 3100, 3320, 3580], 3720),
])
def test_recent_asr_progress_blocks_reported_premature_endpoints(
    progress: list[int], old_endpoint_ms: int,
) -> None:
    class EarlyVoice:
        def __init__(self) -> None:
            self.frames = 0

        def run(self, outputs: object, inputs: dict[str, object]) -> list[object]:
            self.frames += 1
            return [np.array([[0.9 if self.frames <= 20 else 0.01]]), inputs["state"]]

    detector = SileroEndpointDetector(EarlyVoice(), VADConfig())  # type: ignore[arg-type]
    for elapsed_ms in range(0, old_endpoint_ms + 20, 20):
        text = f"word {elapsed_ms}" if elapsed_ms in progress else None
        assert detector.process(bytes(640), text) is None
    for elapsed_ms in range(old_endpoint_ms + 20, 8000, 20):
        reason = detector.process(bytes(640))
        if reason is not None:
            break
    assert reason == "speech_ended"
    assert progress[-1] + 2350 <= elapsed_ms <= progress[-1] + 2450


def test_repeated_text_and_punctuation_do_not_extend_listening(model: SileroVAD) -> None:
    detector = model.new_stream()
    assert detector.process(bytes(640), "Hello world") is None
    for frame in range(200):
        text = ["Hello world", "HELLO, WORLD!", "Hello\nworld..."][frame % 3]
        reason = detector.process(bytes(640), text)
        if reason is not None:
            break
    assert reason == "speech_ended"
    assert 2340 <= frame * 20 <= 2440


def test_empty_asr_text_does_not_confirm_speech(model: SileroVAD) -> None:
    detector = model.new_stream()
    for _ in range(500):
        assert detector.process(bytes(640), " ...! ") is None
    assert detector.process(bytes(640), "") == "no_speech"


def test_reset_clears_transcript_confirmation(model: SileroVAD) -> None:
    detector = model.new_stream()
    assert detector.process(bytes(640), "same words") is None
    detector.reset()
    for _ in range(500):
        assert detector.process(bytes(640)) is None
    assert detector.process(bytes(640), "same words") is None
    assert detector.process(bytes(64000)) is None
    assert detector.process(bytes(64000)) == "speech_ended"
