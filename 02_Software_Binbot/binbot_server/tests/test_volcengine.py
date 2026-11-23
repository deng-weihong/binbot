from __future__ import annotations

import asyncio
import gzip
import json
from dataclasses import replace
from uuid import uuid4

from aiohttp import WSMsgType, web
from aiohttp.test_utils import TestServer
import pytest

from binbot_server.config import Settings
from binbot_server.domain.model import AudioFormat, TurnContext
from binbot_server.infrastructure.providers import (
    VolcengineASRProvider,
    VolcengineTTSProvider,
)
from binbot_server.infrastructure.providers.volcengine import (
    VolcEvent,
    VolcMessage,
    VolcMessageFlag,
    VolcMessageType,
)


def _json_frame(
    message_type: VolcMessageType,
    flag: VolcMessageFlag,
    payload: object,
    *,
    sequence: int = 0,
    event: int | None = None,
    session_id: str = "",
) -> bytes:
    raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return VolcMessage(
        message_type=message_type,
        flag=flag,
        serialization=1,
        compression=1,
        sequence=sequence,
        event=event,
        session_id=session_id,
        payload=gzip.compress(raw),
    ).encode()


def test_volc_message_round_trip_with_event_and_session() -> None:
    message = VolcMessage(
        message_type=VolcMessageType.FULL_CLIENT,
        flag=VolcMessageFlag.WITH_EVENT,
        serialization=1,
        event=VolcEvent.START_SESSION,
        session_id="session-1",
        payload=b"{}",
    )
    decoded = VolcMessage.decode(message.encode())
    assert decoded.event == VolcEvent.START_SESSION
    assert decoded.session_id == "session-1"
    assert decoded.payload == b"{}"


def test_volc_message_rejects_truncated_payload() -> None:
    message = VolcMessage(
        message_type=VolcMessageType.AUDIO_CLIENT,
        flag=VolcMessageFlag.POSITIVE_SEQUENCE,
        sequence=1,
        payload=b"abc",
    ).encode()
    try:
        VolcMessage.decode(message[:-1])
    except ValueError as exc:
        assert "payload size" in str(exc)
    else:
        raise AssertionError("truncated provider frame was accepted")


@pytest.mark.integration
def test_volcengine_asr_stream_uses_bounded_packets_and_final_sequence() -> None:
    async def scenario() -> None:
        received: list[VolcMessage] = []

        async def handler(request: web.Request) -> web.StreamResponse:
            websocket = web.WebSocketResponse()
            await websocket.prepare(request)
            async for incoming in websocket:
                if incoming.type != WSMsgType.BINARY:
                    continue
                message = VolcMessage.decode(incoming.data)
                received.append(message)
                if message.message_type == VolcMessageType.FULL_CLIENT:
                    await websocket.send_bytes(
                        _json_frame(
                            VolcMessageType.FULL_SERVER,
                            VolcMessageFlag.POSITIVE_SEQUENCE,
                            {"result": {"text": "你好"}},
                            sequence=1,
                        )
                    )
                elif message.flag == VolcMessageFlag.NEGATIVE_SEQUENCE:
                    await websocket.send_bytes(
                        _json_frame(
                            VolcMessageType.FULL_SERVER,
                            VolcMessageFlag.NEGATIVE_SEQUENCE,
                            {"result": {"text": "你好 BinBot"}},
                            sequence=-3,
                        )
                    )
            return websocket

        app = web.Application()
        app.router.add_get("/asr", handler)
        server = TestServer(app)
        await server.start_server()
        endpoint = str(server.make_url("/asr")).replace("http://", "ws://")
        provider = VolcengineASRProvider(
            endpoint, "test-key", "test-resource", request_timeout_seconds=2
        )
        try:
            context = TurnContext(uuid4(), uuid4(), "device-a")
            audio_format = AudioFormat("opus", 16_000, 1, 20)
            stream = await provider.open_stream(context, audio_format)
            await stream.push(b"\x01\x00" * 3_200)
            await asyncio.sleep(0.01)
            partial = await stream.push(b"\x02\x00" * 3_200)
            assert partial and partial[0].text == "你好"
            final = await stream.finish()
            assert final.final is True
            assert final.text == "你好 BinBot"
            assert [message.message_type for message in received] == [
                VolcMessageType.FULL_CLIENT,
                VolcMessageType.AUDIO_CLIENT,
                VolcMessageType.AUDIO_CLIENT,
            ]
            assert received[-1].sequence == -3
            assert gzip.decompress(received[-1].payload) == b"\x02\x00" * 3_200
        finally:
            await provider.close()
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_volcengine_tts_stream_yields_pcm_until_session_finished() -> None:
    async def scenario() -> None:
        received_events: list[int | None] = []

        async def handler(request: web.Request) -> web.StreamResponse:
            websocket = web.WebSocketResponse()
            await websocket.prepare(request)
            session_id = ""
            async for incoming in websocket:
                if incoming.type != WSMsgType.BINARY:
                    continue
                message = VolcMessage.decode(incoming.data)
                received_events.append(message.event)
                if message.event == VolcEvent.START_SESSION:
                    session_id = message.session_id
                    await websocket.send_bytes(
                        VolcMessage(
                            message_type=VolcMessageType.AUDIO_SERVER,
                            flag=VolcMessageFlag.WITH_EVENT,
                            event=VolcEvent.TTS_RESPONSE,
                            session_id=session_id,
                            payload=b"\x10\x00\x11\x00",
                        ).encode()
                    )
                    await websocket.send_bytes(
                        VolcMessage(
                            message_type=VolcMessageType.FULL_SERVER,
                            flag=VolcMessageFlag.WITH_EVENT,
                            serialization=1,
                            event=VolcEvent.SESSION_FINISHED,
                            session_id=session_id,
                            payload=b"{}",
                        ).encode()
                    )
            return websocket

        app = web.Application()
        app.router.add_get("/tts", handler)
        server = TestServer(app)
        await server.start_server()
        endpoint = str(server.make_url("/tts")).replace("http://", "ws://")
        provider = VolcengineTTSProvider(
            endpoint, "test-key", "seed-tts-2.0", "seed-tts-2.0-standard", "speaker-a"
        )
        try:
            context = TurnContext(uuid4(), uuid4(), "device-a")
            chunks = [
                chunk
                async for chunk in provider.stream(context, "你好", 24_000)
            ]
            assert chunks == [b"\x10\x00\x11\x00"]
            assert received_events == [VolcEvent.START_SESSION]
        finally:
            await provider.close()
            await server.close()

    asyncio.run(scenario())


def test_volcengine_provider_selection_is_independent(monkeypatch) -> None:
    from binbot_server.bootstrap import build_providers

    monkeypatch.setenv("BINBOT_VOLC_API_KEY", "test-key")
    defaults = Settings()
    settings = replace(
        defaults,
        providers=replace(
            defaults.providers,
            asr="volcengine",
            tts="volcengine",
            volcengine=replace(defaults.providers.volcengine, tts_speaker="speaker-a"),
        ),
    )
    providers = build_providers(settings)
    assert isinstance(providers.asr, VolcengineASRProvider)
    assert isinstance(providers.tts, VolcengineTTSProvider)
    asyncio.run(providers.close())
