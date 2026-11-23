from binbot_server.application.speech_progress import spoken_prefix


def test_cursor_uses_audio_duration_instead_of_text_arrival():
    text = '今天先完成工作，然后喝水休息。'
    assert spoken_prefix(text, 0, 8000) == ''
    early = spoken_prefix(text, 2000, 8000)
    late = spoken_prefix(text, 6000, 8000)
    assert 0 < len(early) < len(late) < len(text)
    assert spoken_prefix(text, 8000, 8000) == text
    assert spoken_prefix(text, 12000, 8000) == text


def test_unknown_duration_advances_conservatively_without_splitting_unicode():
    text = '你好，BinBot。喝杯水吧！'
    prefixes = [spoken_prefix(text, ms) for ms in range(0, 5000, 100)]
    assert prefixes == sorted(prefixes, key=len)
    assert all(text.startswith(prefix) for prefix in prefixes)
    assert spoken_prefix('', 1000) == ''
    assert spoken_prefix(text, 0) == ''
    assert len(spoken_prefix('水。水', 440)) == 1
