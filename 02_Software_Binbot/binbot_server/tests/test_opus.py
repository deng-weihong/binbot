import math
from array import array

import pytest

from binbot_server.infrastructure.audio import OpusDecoder, OpusEncoder


@pytest.mark.parametrize("sample_rate,bitrate", [(16_000, 24_000), (24_000, 32_000)])
def test_opus_encode_decode_round_trip(sample_rate: int, bitrate: int) -> None:
    frame_samples = sample_rate * 20 // 1000
    pcm = array(
        "h",
        (
            int(4_000 * math.sin(2 * math.pi * 440 * index / sample_rate))
            for index in range(frame_samples)
        ),
    ).tobytes()
    encoder = OpusEncoder(sample_rate, 1, 20, bitrate)
    decoder = OpusDecoder(sample_rate, 1, 20)
    try:
        packet = encoder.encode(pcm)
        decoded = decoder.decode(packet)
    finally:
        encoder.close()
        decoder.close()
    assert 0 < len(packet) < len(pcm)
    assert len(decoded) == len(pcm)
