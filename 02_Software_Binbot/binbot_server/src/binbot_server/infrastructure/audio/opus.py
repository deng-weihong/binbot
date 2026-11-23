from __future__ import annotations

import ctypes
import ctypes.util


class OpusError(RuntimeError):
    pass


class _OpusLibrary:
    _instance: "_OpusLibrary | None" = None

    def __new__(cls) -> "_OpusLibrary":
        if cls._instance is None:
            instance = super().__new__(cls)
            instance._load()
            cls._instance = instance
        return cls._instance

    def _load(self) -> None:
        name = ctypes.util.find_library("opus")
        if not name:
            raise OpusError("libopus was not found; install the system libopus package")
        self.lib = ctypes.CDLL(name)

        self.lib.opus_strerror.argtypes = [ctypes.c_int]
        self.lib.opus_strerror.restype = ctypes.c_char_p

        self.lib.opus_decoder_create.argtypes = [
            ctypes.c_int32,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
        ]
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_decoder_destroy.restype = None
        self.lib.opus_decode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int32,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int,
            ctypes.c_int,
        ]
        self.lib.opus_decode.restype = ctypes.c_int

        self.lib.opus_encoder_create.argtypes = [
            ctypes.c_int32,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
        ]
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_encoder_destroy.restype = None
        self.lib.opus_encode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int32,
        ]
        self.lib.opus_encode.restype = ctypes.c_int32
        self.lib.opus_encoder_ctl.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.opus_encoder_ctl.restype = ctypes.c_int

    def check(self, code: int, operation: str) -> int:
        if code < 0:
            description = self.lib.opus_strerror(code).decode("utf-8", "replace")
            raise OpusError(f"{operation} failed: {description} ({code})")
        return code


class OpusDecoder:
    def __init__(self, sample_rate: int, channels: int, frame_ms: int):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_samples = sample_rate * frame_ms // 1_000
        self._opus = _OpusLibrary()
        self._state: int | None = None
        self._create()

    def _create(self) -> None:
        error = ctypes.c_int()
        state = self._opus.lib.opus_decoder_create(
            self.sample_rate, self.channels, ctypes.byref(error)
        )
        self._opus.check(error.value, "opus_decoder_create")
        if not state:
            raise OpusError("opus_decoder_create returned a null state")
        self._state = state

    def decode(self, packet: bytes) -> bytes:
        if self._state is None:
            raise OpusError("decoder is closed")
        if not packet:
            raise OpusError("empty Opus packets are not accepted")
        encoded = (ctypes.c_ubyte * len(packet)).from_buffer_copy(packet)
        output = (ctypes.c_int16 * (self.frame_samples * self.channels))()
        decoded_samples = self._opus.check(
            self._opus.lib.opus_decode(
                self._state,
                encoded,
                len(packet),
                output,
                self.frame_samples,
                0,
            ),
            "opus_decode",
        )
        return ctypes.string_at(output, decoded_samples * self.channels * 2)

    def reset(self) -> None:
        self.close()
        self._create()

    def close(self) -> None:
        if self._state is not None:
            self._opus.lib.opus_decoder_destroy(self._state)
            self._state = None

    def __del__(self) -> None:
        self.close()


class OpusEncoder:
    _APPLICATION_VOIP = 2048
    _SET_BITRATE_REQUEST = 4002
    _MAX_PACKET_BYTES = 4_000

    def __init__(
        self,
        sample_rate: int,
        channels: int,
        frame_ms: int,
        bitrate: int = 32_000,
    ):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_samples = sample_rate * frame_ms // 1_000
        self.bitrate = bitrate
        self._opus = _OpusLibrary()
        self._state: int | None = None
        self._create()

    def _create(self) -> None:
        error = ctypes.c_int()
        state = self._opus.lib.opus_encoder_create(
            self.sample_rate, self.channels, self._APPLICATION_VOIP, ctypes.byref(error)
        )
        self._opus.check(error.value, "opus_encoder_create")
        if not state:
            raise OpusError("opus_encoder_create returned a null state")
        self._state = state
        result = self._opus.lib.opus_encoder_ctl(
            self._state, self._SET_BITRATE_REQUEST, ctypes.c_int(self.bitrate)
        )
        self._opus.check(result, "opus_encoder_ctl(OPUS_SET_BITRATE)")

    def encode(self, pcm_frame: bytes) -> bytes:
        if self._state is None:
            raise OpusError("encoder is closed")
        expected = self.frame_samples * self.channels * 2
        if len(pcm_frame) != expected:
            raise OpusError(f"PCM frame is {len(pcm_frame)} bytes; expected {expected}")
        pcm = (ctypes.c_int16 * (self.frame_samples * self.channels)).from_buffer_copy(pcm_frame)
        output = (ctypes.c_ubyte * self._MAX_PACKET_BYTES)()
        encoded_bytes = self._opus.check(
            self._opus.lib.opus_encode(
                self._state,
                pcm,
                self.frame_samples,
                output,
                self._MAX_PACKET_BYTES,
            ),
            "opus_encode",
        )
        return bytes(output[:encoded_bytes])

    def reset(self) -> None:
        self.close()
        self._create()

    def close(self) -> None:
        if self._state is not None:
            self._opus.lib.opus_encoder_destroy(self._state)
            self._state = None

    def __del__(self) -> None:
        self.close()
