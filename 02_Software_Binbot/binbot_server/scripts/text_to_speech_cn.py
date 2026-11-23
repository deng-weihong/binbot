#!/usr/bin/env python3
"""Generate and download TTS using the site's current session/challenge workflow."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
import http.cookiejar
import json
import os
from pathlib import Path
import re
import secrets
import sys
from tempfile import NamedTemporaryFile
import time
from typing import Any, BinaryIO
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urljoin, urlsplit
from urllib.request import HTTPCookieProcessor, OpenerDirector, Request, build_opener

DEFAULT_BASE_URL = "https://www.text-to-speech.cn/"
DEFAULT_VOICE = "zh-CN-XiaoyiNeural"
DEFAULT_FORMAT = "riff-16khz-16bit-mono-pcm"
TOKEN_ENV = "TEXT_TO_SPEECH_CN_TOKEN"
YZM_ENV = "TEXT_TO_SPEECH_CN_YZM"
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36"
TOKEN_PATTERN = re.compile(r"\bconst\s+token\s*=\s*(['\"])(?P<value>.+?)\1\s*;")
YZM_PATTERN = re.compile(r"\bvar\s+yzm\s*=\s*(['\"])(?P<value>.+?)\1\s*;")
MAX_RESPONSE_BYTES = 2 * 1024 * 1024
MAX_AUDIO_BYTES = 32 * 1024 * 1024


class TtsError(RuntimeError):
    """A remote TTS request could not be completed."""


class TtsRequestError(TtsError):
    """A service error with its application status, without transient credentials."""

    def __init__(self, code: str, message: str) -> None:
        self.code = code
        self.message = message
        super().__init__(f"TTS generation failed (code {code}): {message}")


@dataclass(frozen=True)
class PageCredentials:
    token: str
    yzm: str


@dataclass(frozen=True)
class SynthesisOptions:
    text: str
    language: str = "中文（普通话，简体）"
    voice: str = DEFAULT_VOICE
    role: str = "0"
    style: str = "cheerful"
    rate: int = 0
    pitch: int = 0
    audio_format: str = DEFAULT_FORMAT
    silence: str = ""
    style_degree: float = 1.0
    volume: int = 75
    predict: str = "0"
    user_id: str = ""
    ssml: bool = False

    def as_form(self, credentials: PageCredentials) -> dict[str, str]:
        return {
            **({"type": "SSML"} if self.ssml else {}),
            "language": self.language, "voice": self.voice, "text": self.text,
            "role": self.role, "style": self.style, "rate": str(self.rate),
            "pitch": str(self.pitch), "kbitrate": self.audio_format,
            "silence": self.silence, "styledegree": str(self.style_degree),
            "volume": str(self.volume), "predict": self.predict,
            "user_id": self.user_id, "yzm": credentials.yzm, "replice": "1",
        }


def parse_page_credentials(html: str, token_override: str = "", yzm_override: str = "") -> PageCredentials:
    token_match, yzm_match = TOKEN_PATTERN.search(html), YZM_PATTERN.search(html)
    token = token_override or (token_match.group("value") if token_match else "")
    yzm = yzm_override or (yzm_match.group("value") if yzm_match else "")
    if not token:
        raise TtsError(f"No current token found on the TTS page; set {TOKEN_ENV}")
    if not yzm:
        raise TtsError(f"No current yzm value found on the TTS page; set {YZM_ENV}")
    return PageCredentials(token, yzm)


def parse_download_url(response_text: str) -> str:
    try:
        payload = json.loads(response_text)
    except json.JSONDecodeError as exc:
        raise TtsError("The TTS endpoint returned invalid JSON") from exc
    if not isinstance(payload, dict):
        raise TtsError("The TTS endpoint returned an unexpected response")
    if str(payload.get("code")) != "200":
        code = str(payload.get("code"))
        message = payload.get("msg") or (
            "The site requires additional quota or authorization; check its daily character limit"
            if code == "401" else "unknown error"
        )
        raise TtsRequestError(code, str(message))
    url = payload.get("download")
    if not isinstance(url, str) or not url:
        raise TtsError("The TTS response did not include a download URL")
    validate_download_url(url)
    return url


def validate_download_url(url: str) -> None:
    parsed = urlsplit(url)
    hostname = (parsed.hostname or "").lower()
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.port not in (None, 443)
            or not (hostname == "text-to-speech.cn" or hostname.endswith(".text-to-speech.cn"))):
        raise TtsError("The TTS response returned an untrusted download URL")


def audio_file_extension(audio_format: str) -> str:
    if audio_format.startswith("riff-"):
        return ".wav"
    return ".mp3" if audio_format.endswith("-mp3") else ".audio"


class TextToSpeechClient:
    def __init__(self, base_url: str = DEFAULT_BASE_URL, timeout: float = 45,
                 opener: OpenerDirector | None = None) -> None:
        validate_download_url(base_url)
        self.base_url = base_url.rstrip("/") + "/"
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.timeout = timeout
        self.opener = opener or build_opener(HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self._loaded_at = time.monotonic()

    def _open(self, request: Request) -> Any:
        return self.opener.open(request, timeout=self.timeout)

    @staticmethod
    def _decode_response(response: BinaryIO) -> str:
        headers = getattr(response, "headers", None)
        charset = headers.get_content_charset() if headers is not None else None
        content = response.read(MAX_RESPONSE_BYTES + 1)
        if len(content) > MAX_RESPONSE_BYTES:
            raise TtsError("The TTS endpoint returned an oversized response")
        return content.decode(charset or "utf-8")

    def load_credentials(self) -> PageCredentials:
        with self._open(Request(self.base_url, headers={"User-Agent": USER_AGENT, "Accept": "text/html"})) as response:
            html = self._decode_response(response)
        self._loaded_at = time.monotonic()
        return parse_page_credentials(html, os.environ.get(TOKEN_ENV, ""), os.environ.get(YZM_ENV, ""))

    def prepare_auth(self, credentials: PageCredentials) -> dict[str, str]:
        # The current page gets a one-use challenge with the same session cookie,
        # then sends d|nonce|host|challenge|timestamp|fingerprint. A legacy token
        # field alone is rejected with "动作声明缺失".
        request = Request(urljoin(self.base_url, "api/challenge.php"), headers={
            "User-Agent": USER_AGENT, "Accept": "application/json", "Referer": self.base_url,
        })
        with self._open(request) as response:
            try:
                payload = json.loads(self._decode_response(response))
            except json.JSONDecodeError as exc:
                raise TtsError("The challenge endpoint returned invalid JSON") from exc
        if not isinstance(payload, dict) or str(payload.get("code")) != "200" or not isinstance(payload.get("challenge"), str) or not payload["challenge"]:
            raise TtsError("The challenge endpoint did not supply a current challenge")
        challenge = payload["challenge"]
        nonce, separator, host = credentials.token.partition(":")
        host = host if separator else (urlsplit(self.base_url).hostname or "")
        ts = str(int(time.time()))
        prefix = re.sub(r"[^A-Za-z0-9_-]", "_", USER_AGENT[:40])
        hostname = re.sub(r"[^A-Za-z0-9_-]", "_", urlsplit(self.base_url).hostname or "")
        elapsed = max(0, int(time.monotonic() - self._loaded_at))
        fp = f"{prefix}_{hostname}_{elapsed}_{secrets.token_hex(4)}"[:128]
        return {"dt": "|".join(("d", nonce, host, challenge, ts, fp)),
                "challenge": challenge, "ts": ts, "fp": fp}

    def synthesize(self, options: SynthesisOptions) -> str:
        if not options.text.strip():
            raise ValueError("Text must not be empty")
        if len(options.text.replace("\n", "").replace("\r", "")) > 10_000:
            raise ValueError("Text must not exceed the site's 10,000-character limit")
        if not 0 <= options.volume <= 100:
            raise ValueError("volume must be between 0 and 100")
        credentials = self.load_credentials()
        body = urlencode({**self.prepare_auth(credentials), **options.as_form(credentials)}).encode()
        request = Request(urljoin(self.base_url, "getSpeek.php"), data=body, headers={
            "User-Agent": USER_AGENT, "Accept": "application/json, text/javascript, */*; q=0.01",
            "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8",
            "Origin": self.base_url.rstrip("/"), "Referer": self.base_url,
            "X-Requested-With": "XMLHttpRequest",
            # Do not set Cookie manually: that would discard the session cookie
            # retained by HTTPCookieProcessor and break challenge validation.
        }, method="POST")
        with self._open(request) as response:
            return parse_download_url(self._decode_response(response))

    def download(self, url: str, output_path: Path, force: bool = False) -> Path:
        validate_download_url(url)
        output_path = output_path.expanduser().resolve()
        if output_path.exists() and not force:
            raise FileExistsError(f"Output already exists: {output_path}; pass --force to replace it")
        output_path.parent.mkdir(parents=True, exist_ok=True)
        temporary_path: Path | None = None
        try:
            with self._open(Request(url, headers={"User-Agent": USER_AGENT, "Referer": self.base_url})) as response:
                validate_download_url(response.geturl())
                with NamedTemporaryFile(mode="wb", prefix=f".{output_path.name}.", suffix=".part", dir=output_path.parent, delete=False) as target:
                    temporary_path = Path(target.name)
                    total = 0
                    while chunk := response.read(64 * 1024):
                        total += len(chunk)
                        if total > MAX_AUDIO_BYTES:
                            raise TtsError("The audio download exceeded the size limit")
                        target.write(chunk)
            if not temporary_path or temporary_path.stat().st_size == 0:
                raise TtsError("The downloaded audio file was empty")
            if not force and output_path.exists():
                raise FileExistsError("Output was created during download; refusing to replace it")
            temporary_path.replace(output_path)
        except BaseException:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)
            raise
        return output_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", help="Text to synthesize")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--voice", default=DEFAULT_VOICE)
    parser.add_argument("--language", default="中文（普通话，简体）")
    parser.add_argument("--style", default="cheerful")
    parser.add_argument("--role", default="0")
    parser.add_argument("--rate", type=int, default=0)
    parser.add_argument("--pitch", type=int, default=0)
    parser.add_argument("--volume", type=int, default=75)
    parser.add_argument("--style-degree", type=float, default=1.0)
    parser.add_argument("--format", dest="audio_format", default=DEFAULT_FORMAT)
    parser.add_argument("--silence", default="")
    parser.add_argument("--user-id", default="")
    parser.add_argument("--ssml", action="store_true", help="Submit text as SSML, including its voice element")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--force", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    path = args.output or Path(f"tts_{datetime.now():%Y%m%d_%H%M%S}{audio_file_extension(args.audio_format)}")
    options = SynthesisOptions(
        text=args.text, language=args.language, voice=args.voice, style=args.style, role=args.role,
        rate=args.rate, pitch=args.pitch, volume=args.volume, style_degree=args.style_degree,
        audio_format=args.audio_format, silence=args.silence, user_id=args.user_id,
        ssml=args.ssml,
    )
    try:
        client = TextToSpeechClient(timeout=args.timeout)
        output = client.download(client.synthesize(options), path, args.force)
    except (TtsError, ValueError, OSError, HTTPError, URLError) as exc:
        # Transient page credentials and download URLs are never printed.
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
