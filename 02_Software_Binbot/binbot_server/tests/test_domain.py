from uuid import uuid4

import pytest

from binbot_server.domain.errors import StateConflict
from binbot_server.domain.model import AudioFormat, SessionPhase, VoiceSession


def make_session() -> VoiceSession:
    return VoiceSession(
        device_id="binbot-test",
        uplink=AudioFormat("opus", 16_000, 1, 20),
        downlink=AudioFormat("opus", 24_000, 1, 20),
        max_history_messages=4,
    )


def test_voice_session_happy_path() -> None:
    session = make_session()
    turn_id = uuid4()
    stream_id = uuid4()
    session.start_turn(turn_id, stream_id)
    assert session.phase == SessionPhase.LISTENING
    session.finish_input(turn_id, stream_id)
    assert session.phase == SessionPhase.THINKING
    session.start_speaking(turn_id)
    session.complete_turn(turn_id, "问题", "回答")
    assert session.phase == SessionPhase.IDLE
    assert [item.content for item in session.history] == ["问题", "回答"]


def test_voice_session_rejects_stale_turn() -> None:
    session = make_session()
    session.start_turn(uuid4(), uuid4())
    with pytest.raises(StateConflict):
        session.finish_input(uuid4(), uuid4())
