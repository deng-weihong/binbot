import asyncio
import base64
from dataclasses import replace
from uuid import uuid4

from aiohttp import ClientSession, WSMsgType, web
from aiohttp.test_utils import TestServer
import pytest

from binbot_server.bootstrap import create_application
from binbot_server.config import ComponentConfig, SeeduplexConfig, Settings, validate_settings
from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import ChatMessage, TurnContext
from binbot_server.infrastructure import provider_settings
from binbot_server.infrastructure.audio import OpusEncoder
from binbot_server.infrastructure.providers.seeduplex import SeeduplexProvider, SeeduplexSession
from binbot_server.transport.protocol import AudioFrame, AudioKind, encode_audio_frame
from test_http_integration import receive_until
from test_realtime_session import until


def test_legacy_configuration_retains_text_provider_and_new_voice_settings():
    defaults = Settings()
    assert defaults.providers.seeduplex.allow_interruption is False
    legacy = provider_settings.provider_values(defaults)
    legacy.pop("voice")
    legacy.pop("seeduplex")
    updated = provider_settings.validate_values(defaults, legacy)
    assert updated == defaults
    values = provider_settings.provider_values(defaults)
    values["voice"] = "seeduplex"
    assert provider_settings.validate_values(defaults, values).providers.llm == "mock"
    with pytest.raises(ValueError, match="16 kHz"):
        validate_settings(replace(
            defaults, providers=replace(defaults.providers, voice="seeduplex"),
            audio=replace(defaults.audio, uplink_sample_rate=24000),
        ))
    with pytest.raises(ValueError, match="1.2.6.1"):
        validate_settings(replace(defaults, providers=replace(
            defaults.providers, seeduplex=replace(defaults.providers.seeduplex, model="3.0"),
        )))
    with pytest.raises(ValueError, match="allow_interruption must be a boolean"):
        validate_settings(replace(defaults, providers=replace(
            defaults.providers, seeduplex=replace(defaults.providers.seeduplex, allow_interruption="false"),
        )))


def test_muting_discards_queued_microphone_audio_and_resumes_with_fresh_frames():
    async def scenario():
        sent = []
        first_sent, release_send = asyncio.Event(), asyncio.Event()

        class GatedSession(SeeduplexSession):
            async def send_event(self, event):
                sent.append(event)
                if event["type"] == "input_audio_buffer.append" and not first_sent.is_set():
                    first_sent.set()
                    await release_send.wait()

        session = GatedSession(SeeduplexConfig(), "test-key", set())
        session._sender = asyncio.create_task(session._send_audio_loop())
        try:
            await session.send_audio(bytes(640))
            await asyncio.wait_for(first_sent.wait(), 1)
            for _ in range(10):
                await session.send_audio(b"\x01\x00" * 320)
            await session.commit_audio()
            await session.set_input_muted(True)
            await session.send_audio(b"\x02\x00" * 320)
            release_send.set()
            await until(lambda: any(e["type"] == "input_audio_mute.commit" for e in sent))
            assert [e["type"] for e in sent] == [
                "input_audio_buffer.append", "input_audio_buffer.commit", "input_audio_mute.commit",
            ]
            await session.set_input_muted(False)
            fresh = b"\x03\x00" * 320
            await session.send_audio(fresh)
            await until(lambda: sum(e["type"] == "input_audio_buffer.append" for e in sent) == 2)
            assert sent[-2]["type"] == "input_audio_unmute.commit"
            assert base64.b64decode(sent[-1]["audio"]) == fresh
        finally:
            release_send.set()
            await session.close()
    asyncio.run(scenario())


def test_muting_invalidates_a_paced_frame_even_if_input_resumes_before_it_wakes(monkeypatch):
    async def scenario():
        sent = []
        first_sent, paced, release = asyncio.Event(), asyncio.Event(), asyncio.Event()

        class GatedSession(SeeduplexSession):
            async def send_event(self, event):
                sent.append(event)
                if event["type"] == "input_audio_buffer.append":
                    first_sent.set()

        session = GatedSession(SeeduplexConfig(), "test-key", set())
        real_sleep = asyncio.sleep

        async def hold_paced_frame(delay):
            if asyncio.current_task() is session._sender:
                paced.set()
                await release.wait()
            else:
                await real_sleep(delay)

        monkeypatch.setattr(asyncio, "sleep", hold_paced_frame)
        session._sender = asyncio.create_task(session._send_audio_loop())
        try:
            await session.send_audio(bytes(640))
            await asyncio.wait_for(first_sent.wait(), 1)
            await session.send_audio(b"\x01\x00" * 320)
            await asyncio.wait_for(paced.wait(), 1)
            await session.set_input_muted(True)
            await session.set_input_muted(False)
            fresh = b"\x02\x00" * 320
            await session.send_audio(fresh)
            release.set()
            await until(lambda: sum(e["type"] == "input_audio_buffer.append" for e in sent) == 2)
            assert [e["type"] for e in sent] == [
                "input_audio_buffer.append", "input_audio_mute.commit",
                "input_audio_unmute.commit", "input_audio_buffer.append",
            ]
            assert base64.b64decode(sent[-1]["audio"]) == fresh
        finally:
            release.set()
            await session.close()
    asyncio.run(scenario())


def test_provider_configuration_can_use_a_persistent_directory(tmp_path):
    defaults = Settings()
    settings = replace(defaults, providers=replace(
        defaults.providers, settings_directory=str(tmp_path / "providers"),
    ))
    provider_settings.persist(settings, provider_settings.provider_values(settings), {})
    assert provider_settings.settings_path(settings) == tmp_path / "providers/provider-settings.json"
    assert provider_settings.settings_path(settings).stat().st_mode & 0o777 == 0o600
    assert provider_settings.load_provider_overrides(settings) == settings


@pytest.mark.integration
def test_json_protocol_audio_pacing_mute_history_tools_and_graceful_close():
    async def scenario():
        received, audio_at = [], []
        closed = asyncio.Event()

        async def handler(request):
            assert request.headers["X-Api-Key"] == "speech-test-key"
            ws = web.WebSocketResponse()
            ws.headers["X-Tt-Logid"] = "test-log"
            await ws.prepare(request)
            async for message in ws:
                assert message.type == WSMsgType.TEXT
                event = message.json()
                received.append(event)
                assert event.get("event_id")
                kind = event["type"]
                if kind == "session.create":
                    await ws.send_json({"type": "session.created", "session": {"id": "dialog"}})
                elif kind == "conversation.item.create":
                    await ws.send_json({"type": "conversation.item.added", "items": event["items"]})
                elif kind == "input_audio_buffer.append":
                    audio_at.append(asyncio.get_running_loop().time())
                    assert base64.b64decode(event["audio"]) == bytes(640)
                elif kind == "session.close":
                    await ws.send_json({"type": "session.closed"})
                    closed.set()
            return ws

        app = web.Application()
        app.router.add_get("/voice", handler)
        async with TestServer(app) as server:
            config = replace(SeeduplexConfig(), endpoint=str(server.make_url("/voice")))
            provider = SeeduplexProvider(config, "speech-test-key", "BinBot prompt")
            try:
                session = await provider.open_session(
                    TurnContext(uuid4(), uuid4(), "test"),
                    [ChatMessage("user", "previous question"), ChatMessage("assistant", "previous answer")],
                    [{"name": "device.test", "description": "test", "parameters": {"type": "object"},
                      "permission": "device.control"}],
                )
                assert session.log_id == "test-log"
                created = received[0]["session"]
                assert created["model"] == "1.2.6.1" and created["instructions"] == "BinBot prompt"
                assert created["audio"]["input"]["format"] == {"type": "pcm", "rate": 16000}
                assert created["audio"]["output"]["format"] == {"type": "pcm_s16le", "rate": 24000}
                assert received[0]["extension"]["dialog"]["extra"]["enable_music"] is True
                assert received[0]["extension"]["dialog"]["extra"]["enable_user_query_exit"] is True
                assert "permission" not in created["tools"][0]
                assert [i["role"] for i in received[1]["items"]] == ["user", "assistant"]
                for _ in range(5):
                    await session.send_audio(bytes(640))
                await session.commit_audio()
                await until(lambda: any(e["type"] == "input_audio_mute.commit" for e in received))
                assert len(audio_at) == 5
                assert min(b - a for a, b in zip(audio_at, audio_at[1:])) >= 0.012
                await session.send_audio(bytes(640))
                await until(lambda: len(audio_at) == 6)
                kinds = [e["type"] for e in received]
                assert kinds[-2:] == ["input_audio_unmute.commit", "input_audio_buffer.append"]
                await until(lambda: sum(e["type"] == "input_audio_mute.commit" for e in received) == 2)
                await session.close()
                assert closed.is_set() and received[-1]["type"] == "session.close"
            finally:
                await provider.close()
    asyncio.run(scenario())


@pytest.mark.integration
def test_upstream_errors_are_redacted_and_connections_are_released():
    async def scenario():
        async def handler(request):
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            await ws.receive_json()
            await ws.send_json({"type": "error", "error": {
                "code": 403, "message": "key speech-test-key has no Seeduplex permission",
            }})
            await ws.close()
            return ws

        app = web.Application()
        app.router.add_get("/voice", handler)
        async with TestServer(app) as server:
            provider = SeeduplexProvider(
                replace(SeeduplexConfig(), endpoint=str(server.make_url("/voice"))), "speech-test-key", "prompt"
            )
            with pytest.raises(ProviderFailure, match="403") as failure:
                await provider.open_session(TurnContext(uuid4(), uuid4(), "test"), (), ())
            assert "speech-test-key" not in str(failure.value)
            assert not provider._sessions
            await provider.close()
    asyncio.run(scenario())


@pytest.mark.integration
def test_configuration_validates_new_credentials_and_routes_device_voice_separately(tmp_path, monkeypatch):
    monkeypatch.setenv("BINBOT_ADMIN_PASSWORD", "test-admin-password")
    monkeypatch.delenv("BINBOT_SEEDUPLEX_API_KEY", raising=False)

    async def scenario():
        connections, incoming_audio = [], []

        async def handler(request):
            ws = web.WebSocketResponse()
            await ws.prepare(request)
            connections.append(ws)
            async for message in ws:
                if message.type != WSMsgType.TEXT:
                    break
                event = message.json()
                if event["type"] == "session.create":
                    if request.headers.get("X-Api-Key") != "working-key":
                        await ws.send_json({"type": "error", "error": {"code": 403, "message": "not enabled"}})
                    else:
                        await ws.send_json({"type": "session.created", "session": {"id": "dialog"}})
                elif event["type"] == "input_audio_buffer.append":
                    incoming_audio.append(base64.b64decode(event["audio"]))
                    response_id = f"r{len(incoming_audio)}"
                    for reply in [
                        {"type": "conversation.item.input_audio_transcription.started"},
                        {"type": "conversation.item.input_audio_transcription.delta", "delta": "voice"},
                        {"type": "conversation.item.input_audio_transcription.delta", "delta": "voice question"},
                        {"type": "conversation.item.input_audio_transcription.completed", "transcript": "voice question"},
                        {"type": "response.output_text.delta", "response_id": response_id, "delta": "voice answer"},
                        {"type": "response.output_audio.delta", "response_id": response_id,
                         "delta": base64.b64encode(bytes(960 * 12)).decode()},
                        {"type": "response.output_audio.done", "response_id": response_id},
                    ]:
                        await ws.send_json(reply)
                elif event["type"] == "session.close":
                    await ws.send_json({"type": "session.closed"})
            return ws

        upstream = web.Application()
        upstream.router.add_get("/voice", handler)
        async with TestServer(upstream) as upstream_server:
            defaults = Settings()
            settings = replace(defaults, component=ComponentConfig(directory=str(tmp_path / "components")),
                               vad=replace(defaults.vad, enabled=False))
            application = create_application(settings)
            async with TestServer(application.http) as server, ClientSession(
                headers={"Authorization": "Bearer test-admin-password"}
            ) as client:
                url = server.make_url("/api/v1/admin/providers/configuration")
                async with client.get(url) as response:
                    before = await response.json()
                config = {**before["config"], "voice": "seeduplex", "seeduplex": {
                    **before["config"]["seeduplex"],
                    "endpoint": str(upstream_server.make_url("/voice").with_scheme("ws")),
                }}
                async with client.put(url, json={"revision": before["revision"], "config": config,
                                                 "secrets": {"seeduplex": "invalid-key"}}) as response:
                    assert response.status == 400
                assert application.providers.realtime is None
                assert not provider_settings.snapshot(settings)["credentials_configured"]["seeduplex"]
                async with client.put(url, json={"revision": before["revision"], "config": config,
                                                 "secrets": {"seeduplex": "working-key"}}) as response:
                    saved = await response.json()
                    assert response.status == 200, saved
                assert saved["config"]["llm"] == before["config"]["llm"]
                assert saved["credentials_configured"]["seeduplex"]
                assert "working-key" not in str(saved)
                assert connections[-1].closed
                # Old firmware must report a clear capability error before starting voice.
                async with client.ws_connect(server.make_url("/v1/device/ws")) as ws:
                    hello = {
                        "type": "hello", "protocol_version": 1, "device_id": "voice-device",
                        "client_version": "test", "token": "", "capabilities": [],
                        "uplink": {"codec": "opus", "sample_rate": 16000, "channels": 1, "frame_ms": 20},
                        "downlink": {"codec": "opus", "sample_rate": 24000, "channels": 1, "frame_ms": 20},
                    }
                    await ws.send_json(hello)
                    assert "realtime_voice_v1" in (await ws.receive_json())["message"]
                async with client.ws_connect(server.make_url("/v1/device/ws")) as ws:
                    hello["capabilities"] = ["realtime_voice_v1"]
                    await ws.send_json(hello)
                    assert (await ws.receive_json())["input_endpoint"] == "realtime_voice_v1"
                    await ws.receive_json()
                    turn, stream = uuid4(), uuid4()
                    await ws.send_json({"type": "turn.start", "turn_id": str(turn), "stream_id": str(stream)})
                    encoder = OpusEncoder(16000, 1, 20)
                    try:
                        await ws.send_bytes(encode_audio_frame(AudioFrame(
                            AudioKind.UPLINK_OPUS, stream, 0, 1000, encoder.encode(bytes(640))
                        )))
                        controls, audio_count = await receive_until(ws, "audio.start")
                        # Firmware continues to send contiguous frames while
                        # speaking. They must not trigger another upstream reply.
                        for sequence in (1, 2):
                            await ws.send_bytes(encode_audio_frame(AudioFrame(
                                AudioKind.UPLINK_OPUS, stream, sequence, 1000 + sequence * 20,
                                encoder.encode(bytes(640)),
                            )))
                        tail, tail_audio = await receive_until(ws, "assistant.final")
                        controls.extend(tail)
                        audio_count += tail_audio
                        states, _ = await receive_until(ws, "state")
                        assert states[-1]["state"] == "listening"
                        assert audio_count == 12 and len(incoming_audio) == 1
                        await ws.send_bytes(encode_audio_frame(AudioFrame(
                            AudioKind.UPLINK_OPUS, stream, 3, 1060, encoder.encode(bytes(640)),
                        )))
                        next_controls, next_audio = await receive_until(ws, "assistant.final")
                        assert next_audio == 12 and len(incoming_audio) == 2
                        assert [e["text"] for e in next_controls if e["type"] == "assistant.final"] == ["voice answer"]
                    finally:
                        encoder.close()
                    assert len(incoming_audio[0]) == 640
                    assert [e["text"] for e in controls if e["type"] == "asr.partial"] == ["voice", "voice question"]
                    assert [e["text"] for e in controls if e["type"] == "assistant.final"] == ["voice answer"]
                    await ws.send_json({"type": "text.input", "turn_id": str(uuid4()), "text": "text question"})
                    controls, _ = await receive_until(ws, "assistant.final")
                    assert [e["text"] for e in controls if e["type"] == "assistant.final"] == [defaults.providers.mock.response]
                restored = provider_settings.load_provider_overrides(settings)
                assert restored.providers.voice == "seeduplex" and restored.providers.llm == "mock"
    asyncio.run(scenario())
