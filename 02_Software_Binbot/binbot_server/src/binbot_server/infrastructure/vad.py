from __future__ import annotations

from collections import deque
import logging
from pathlib import Path

import numpy as np
import onnxruntime as ort
from numpy.typing import NDArray

from binbot_server.config import VADConfig


class SileroVAD:
    """Share model weights, but allocate recurrent state per audio stream."""

    def __init__(self, config: VADConfig):
        self.config = config
        options = ort.SessionOptions()
        options.inter_op_num_threads = 1
        options.intra_op_num_threads = 1
        self.session = ort.InferenceSession(
            str(Path(__file__).with_name("models") / "silero_vad.onnx"),
            sess_options=options,
            providers=["CPUExecutionProvider"],
        )

    def new_stream(self) -> SileroEndpointDetector:
        return SileroEndpointDetector(self.session, self.config)


class SileroEndpointDetector:
    def __init__(self, session: ort.InferenceSession, config: VADConfig):
        self._session = session
        self._config = config
        self.reset()

    def reset(self) -> None:
        self._state: NDArray[np.float32] = np.zeros((2, 1, 128), dtype=np.float32)
        self._context: NDArray[np.float32] = np.zeros((1, 64), dtype=np.float32)
        self._pending = bytearray()
        self._window: deque[bool] = deque(maxlen=5)
        self._last_is_voice = False
        self._speech_started = False
        self._samples = 0
        self._last_voice_sample = 0
        self._last_transcript = ""
        self._last_transcript_sample: int | None = None
        self._max_probability = 0.0
        self._ended: str | None = None

    def process(self, pcm: bytes, transcript: str | None = None) -> str | None:
        if len(pcm) % 2:
            raise ValueError("VAD requires signed 16-bit PCM")
        if self._ended:
            return self._ended
        if transcript:
            text = "".join(char for char in transcript if char.isalnum()).casefold()
            if text and text != self._last_transcript:
                self._last_transcript = text
                self._last_transcript_sample = self._samples
                # ASR can hear quiet speech that the acoustic detector misses.
                self._speech_started = True
        self._pending.extend(pcm)
        while len(self._pending) >= 1024:
            frame = np.frombuffer(bytes(self._pending[:1024]), dtype="<i2")
            del self._pending[:1024]
            audio = np.concatenate(
                (self._context, frame.astype(np.float32).reshape(1, -1) / 32768), axis=1
            ).astype(np.float32, copy=False)
            probability, self._state = self._session.run(
                None,
                {"input": audio, "state": self._state, "sr": np.array(16000, dtype=np.int64)},
            )
            self._context = audio[:, -64:]
            self._samples += 512
            score = float(probability.item())
            self._max_probability = max(self._max_probability, score)
            if score >= self._config.threshold:
                self._last_is_voice = True
            elif score <= self._config.threshold_low:
                self._last_is_voice = False
            self._window.append(self._last_is_voice)
            if self._window.count(True) >= 3:
                self._speech_started = True
                self._last_voice_sample = self._samples
            elif self._speech_started:
                voice_idle = self._samples - self._last_voice_sample
                transcript_idle = (
                    self._samples - self._last_transcript_sample
                    if self._last_transcript_sample is not None else None
                )
                if voice_idle >= self._config.end_silence_ms * 16 and (
                    transcript_idle is None
                    or transcript_idle >= self._config.transcript_grace_ms * 16
                ):
                    self._ended = "speech_ended"
            elif self._samples >= self._config.start_timeout_ms * 16:
                self._ended = "no_speech"
            if self._ended:
                logging.getLogger(__name__).info(
                    "endpoint reason=%s input_ms=%d voice_idle_ms=%d "
                    "transcript_idle_ms=%s max_probability=%.3f",
                    self._ended, self._samples // 16,
                    (self._samples - self._last_voice_sample) // 16,
                    (self._samples - self._last_transcript_sample) // 16
                    if self._last_transcript_sample is not None else None,
                    self._max_probability,
                )
                return self._ended
        return None
