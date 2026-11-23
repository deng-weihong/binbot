"""Regression coverage for the changed web TTS session/challenge contract."""

import importlib.util
from io import BytesIO
from pathlib import Path
import sys
from urllib.parse import parse_qs

import pytest

spec = importlib.util.spec_from_file_location(
    "binbot_tts_cn_script", Path(__file__).parents[1] / "scripts/text_to_speech_cn.py"
)
assert spec and spec.loader
tts = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = tts
spec.loader.exec_module(tts)


class Response(BytesIO):
    def __init__(self, content, url="https://www.text-to-speech.cn/audio/test.wav"):
        super().__init__(content)
        self.url = url

    def geturl(self):
        return self.url


class Opener:
    def __init__(self, *responses):
        self.responses = list(responses)
        self.requests = []

    def open(self, request, timeout):
        self.requests.append(request)
        assert timeout > 0
        return self.responses.pop(0)


def test_synthesis_uses_one_time_challenge_before_generating_and_keeps_cookie_processor_in_charge():
    opener = Opener(
        Response(b"const token = 'test-nonce:www.text-to-speech.cn'; var yzm = 'test-yzm';"),
        Response(b'{"code":200,"challenge":"test-one-time-challenge"}'),
        Response(b'{"code":200,"download":"https://tts.text-to-speech.cn/test.wav"}'),
    )
    client = tts.TextToSpeechClient(opener=opener)
    assert client.synthesize(tts.SynthesisOptions(text="serene 宁静的")) == "https://tts.text-to-speech.cn/test.wav"
    assert [request.full_url for request in opener.requests] == [
        "https://www.text-to-speech.cn/", "https://www.text-to-speech.cn/api/challenge.php",
        "https://www.text-to-speech.cn/getSpeek.php",
    ]
    request = opener.requests[-1]
    form = parse_qs(request.data.decode())
    assert list(form)[:4] == ["dt", "challenge", "ts", "fp"]
    assert form["text"] == ["serene 宁静的"]
    assert form["yzm"] == ["test-yzm"]
    assert form["dt"][0] == "|".join(("d", "test-nonce", "www.text-to-speech.cn", "test-one-time-challenge", form["ts"][0], form["fp"][0]))
    assert all(not request.has_header("Cookie") for request in opener.requests)


def test_missing_challenge_fails_before_synthesis():
    opener = Opener(
        Response(b"const token = 'test-nonce'; var yzm = 'test-yzm';"),
        Response(b'{"code":403,"msg":"verification unavailable"}'),
    )
    with pytest.raises(tts.TtsError, match="challenge"):
        tts.TextToSpeechClient(opener=opener).synthesize(tts.SynthesisOptions(text="serene"))
    assert len(opener.requests) == 2


@pytest.mark.parametrize("url", [
    "http://tts.text-to-speech.cn/a.wav", "https://text-to-speech.cn.evil.example/a.wav",
    "https://localhost/a.wav", "https://user:password@tts.text-to-speech.cn/a.wav",
    "https://tts.text-to-speech.cn:8443/a.wav",
])
def test_rejects_untrusted_downloads(url):
    with pytest.raises(tts.TtsError, match="untrusted"):
        tts.validate_download_url(url)


def test_rejected_download_and_partial_download_do_not_replace_an_existing_file(tmp_path, monkeypatch):
    output = tmp_path / "word.wav"
    output.write_bytes(b"existing")
    client = tts.TextToSpeechClient(opener=Opener())
    with pytest.raises(FileExistsError):
        client.download("https://tts.text-to-speech.cn/a.wav", output)
    monkeypatch.setattr(tts, "MAX_AUDIO_BYTES", 4)
    client = tts.TextToSpeechClient(opener=Opener(Response(b"too large")))
    with pytest.raises(tts.TtsError, match="size limit"):
        client.download("https://tts.text-to-speech.cn/a.wav", output, force=True)
    assert output.read_bytes() == b"existing"
    assert list(tmp_path.iterdir()) == [output]


def test_empty_or_redirected_download_is_not_saved(tmp_path):
    output = tmp_path / "word.wav"
    client = tts.TextToSpeechClient(opener=Opener(Response(b"")))
    with pytest.raises(tts.TtsError, match="empty"):
        client.download("https://tts.text-to-speech.cn/a.wav", output)
    client = tts.TextToSpeechClient(opener=Opener(Response(b"audio", "https://evil.example/a.wav")))
    with pytest.raises(tts.TtsError, match="untrusted"):
        client.download("https://tts.text-to-speech.cn/a.wav", output)
    assert not list(tmp_path.iterdir())


def test_remote_error_is_reported_and_invalid_text_does_not_make_requests():
    with pytest.raises(tts.TtsError, match="403.*动作声明缺失"):
        tts.parse_download_url('{"code":403,"msg":"动作声明缺失"}')
    opener = Opener()
    client = tts.TextToSpeechClient(opener=opener)
    for text in (" ", "x" * 10001):
        with pytest.raises(ValueError):
            client.synthesize(tts.SynthesisOptions(text=text))
    assert not opener.requests
