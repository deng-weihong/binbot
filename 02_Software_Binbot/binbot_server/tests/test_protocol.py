from pathlib import Path
from uuid import UUID, uuid4

import pytest

from binbot_server.domain.errors import ProtocolViolation
from binbot_server.transport.protocol import (
    AUDIO_HEADER_SIZE,
    AudioFrame,
    AudioKind,
    DeviceCommandResultMessage,
    DeviceStateMessage,
    decode_audio_frame,
    encode_audio_frame,
    parse_control,
    parse_hello,
)


def test_binary_audio_round_trip() -> None:
    stream_id = uuid4()
    frame = AudioFrame(AudioKind.UPLINK_OPUS, stream_id, 42, 840, b"encoded-opus")
    encoded = encode_audio_frame(frame)
    assert len(encoded) == AUDIO_HEADER_SIZE + len(frame.payload)
    decoded = decode_audio_frame(encoded, max_payload_bytes=4096)
    assert decoded.kind == AudioKind.UPLINK_OPUS
    assert decoded.stream_id == stream_id
    assert decoded.sequence == 42
    assert decoded.timestamp_ms == 840
    assert decoded.payload == b"encoded-opus"


def test_binary_audio_matches_cross_language_vector() -> None:
    vector = Path(__file__).parents[2] / "contracts/voice/v1/vectors/uplink-audio-frame.hex"
    encoded_hex = "".join(
        line.strip() for line in vector.read_text().splitlines() if not line.startswith("#")
    )
    frame = AudioFrame(
        AudioKind.UPLINK_OPUS,
        UUID("00112233-4455-6677-8899-aabbccddeeff"),
        0x01020304,
        0x0102030405060708,
        bytes.fromhex("aabbcc"),
    )
    assert encode_audio_frame(frame).hex() == encoded_hex


def test_binary_audio_rejects_size_mismatch() -> None:
    frame = AudioFrame(AudioKind.UPLINK_OPUS, uuid4(), 0, 0, b"payload")
    with pytest.raises(ProtocolViolation):
        decode_audio_frame(encode_audio_frame(frame)[:-1], max_payload_bytes=4096)


def test_control_messages_reject_unknown_fields() -> None:
    with pytest.raises(ProtocolViolation):
        parse_control('{"type":"ping","nonce":"1","unexpected":true}', 1024)


def test_hello_validates_audio_contract() -> None:
    hello = parse_hello(
        """{
          "type":"hello",
          "protocol_version":1,
          "device_id":"binbot-001",
          "client_version":"0.1.0",
          "uplink":{"codec":"opus","sample_rate":16000,"channels":1,"frame_ms":20},
          "downlink":{"codec":"opus","sample_rate":24000,"channels":1,"frame_ms":20}
        }""",
        4096,
    )
    assert hello.device_id == "binbot-001"


def test_hello_accepts_restricted_camera_metadata() -> None:
    hello = parse_hello(
        """{
          "type":"hello",
          "protocol_version":1,
          "device_id":"binbot-001",
          "client_version":"0.2.0",
          "uplink":{"codec":"opus","sample_rate":16000,"channels":1,"frame_ms":20},
          "downlink":{"codec":"opus","sample_rate":24000,"channels":1,"frame_ms":20},
          "camera":{"transport":"rtsp","port":554,"path":"/live/binbot"}
        }""",
        4096,
    )
    assert hello.camera is not None
    assert hello.camera.path == "/live/binbot"
    with pytest.raises(ProtocolViolation):
        parse_hello(
            """{
              "type":"hello",
              "protocol_version":1,
              "device_id":"binbot-001",
              "client_version":"0.2.0",
              "uplink":{"codec":"opus","sample_rate":16000,"channels":1,"frame_ms":20},
              "downlink":{"codec":"opus","sample_rate":24000,"channels":1,"frame_ms":20},
              "camera":{"transport":"rtsp","port":554,"path":"http://example.test/x"}
            }""",
            4096,
        )


def test_device_state_and_command_result_parse() -> None:
    state = parse_control(
        """{
          "type":"device.state",
          "revision":7,
          "todos":[{"text":"浇花","done":false}],
          "music":{"track_index":1,"position_seconds":12,"active":true,"playing":true},
          "display":{"gif":"asset.gif"}
        }""",
        4096,
    )
    assert isinstance(state, DeviceStateMessage)
    assert state.revision == 7
    assert state.todos[0].text == "浇花"

    command_id = uuid4()
    result = parse_control(
        f'{{"type":"device.command.result","command_id":"{command_id}",'
        '"success":true,"message":"done"}',
        4096,
    )
    assert isinstance(result, DeviceCommandResultMessage)
    assert result.command_id == command_id
