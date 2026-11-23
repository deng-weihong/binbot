import asyncio
from collections.abc import AsyncIterator, Sequence
from dataclasses import replace
from pathlib import Path
from uuid import uuid4

import pytest
from aiohttp import ClientSession, CookieJar, WSMsgType
from aiohttp.test_utils import TestServer

from binbot_server.bootstrap import create_application
from binbot_server.application.ports import ProviderBundle
from binbot_server.config import AuthConfig, MockProviderConfig, Settings
from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import ChatMessage, TurnContext
from binbot_server.infrastructure.audio import OpusEncoder
from binbot_server.infrastructure.providers import (
    MockASRProvider,
    MockTTSProvider,
)
from binbot_server.transport.protocol import (
    AudioFrame, AudioKind, DeviceStateMessage, encode_audio_frame,
)


async def send_hello(
    websocket: object,
    device_id: str,
    token: str = "",
    remote_control: bool = False,
    server_vad: bool = False,
) -> list[dict[str, object]]:
    hello: dict[str, object] = {
            "type": "hello",
            "protocol_version": 1,
            "device_id": device_id,
            "client_version": "integration-test",
            "token": token,
            "uplink": {
                "codec": "opus",
                "sample_rate": 16_000,
                "channels": 1,
                "frame_ms": 20,
            },
            "downlink": {
                "codec": "opus",
                "sample_rate": 24_000,
                "channels": 1,
                "frame_ms": 20,
            },
            "capabilities": ["text_input", "turn_cancel"],
    }
    if remote_control:
        hello["capabilities"] = [
            "text_input",
            "turn_cancel",
            "remote_control_v1",
        ]
        hello["camera"] = {
            "transport": "rtsp",
            "port": 554,
            "path": "/live/binbot",
        }
    if server_vad:
        hello["capabilities"].append("server_vad_v1")  # type: ignore[union-attr]
    await websocket.send_json(hello)  # type: ignore[attr-defined]
    return [
        await websocket.receive_json(),  # type: ignore[attr-defined,no-any-return]
        await websocket.receive_json(),  # type: ignore[attr-defined,no-any-return]
    ]


async def receive_until(
    websocket: object, event_type: str, timeout: float = 3.0
) -> tuple[list[dict[str, object]], int]:
    controls: list[dict[str, object]] = []
    binary_frames = 0
    while not any(event.get("type") == event_type for event in controls):
        message = await asyncio.wait_for(  # type: ignore[attr-defined]
            websocket.receive(), timeout=timeout
        )
        if message.type == WSMsgType.TEXT:
            controls.append(message.json())
        elif message.type == WSMsgType.BINARY:
            binary_frames += 1
        else:
            raise AssertionError(f"unexpected WebSocket message: {message.type}")
    return controls, binary_frames


@pytest.mark.integration
def test_server_shutdown_closes_active_websocket() -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        client = ClientSession()
        websocket = await client.ws_connect(url)
        try:
            await send_hello(websocket, "shutdown-test")
            await asyncio.wait_for(server.close(), timeout=1)
            message = await asyncio.wait_for(websocket.receive(), timeout=1)
            assert message.type in {WSMsgType.CLOSE, WSMsgType.CLOSED, WSMsgType.CLOSING}
        finally:
            await websocket.close()
            await client.close()
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
@pytest.mark.parametrize("advertised,enabled,expected", [
    (True, True, "server_vad_v1"),
    (False, True, "client"),
    (True, False, "client"),
])
def test_server_vad_capability_negotiation(advertised: bool, enabled: bool, expected: str) -> None:
    async def scenario() -> None:
        defaults = Settings()
        application = create_application(replace(defaults, vad=replace(defaults.vad, enabled=enabled)))
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession() as client:
                async with client.ws_connect(server.make_url("/v1/device/ws")) as websocket:
                    events = await send_hello(websocket, "vad-negotiation", server_vad=advertised)
                    assert events[0]["input_endpoint"] == expected
        finally:
            await server.close()
    asyncio.run(scenario())


@pytest.mark.integration
def test_real_opus_and_silero_endpoint_keep_paused_speech_and_connection() -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        server = TestServer(application.http)
        await server.start_server()
        encoder = OpusEncoder(16000, 1, 20, 24000)
        try:
            async with ClientSession() as client:
                async with client.ws_connect(server.make_url("/v1/device/ws")) as websocket:
                    events = await send_hello(websocket, "vad-opus", server_vad=True)
                    assert events[0]["input_endpoint"] == "server_vad_v1"
                    turn_id, stream_id = uuid4(), uuid4()
                    await websocket.send_json({"type": "turn.start", "turn_id": str(turn_id),
                                               "stream_id": str(stream_id)})
                    speech = (Path(__file__).with_name("data") / "quiet-speech-16k.pcm").read_bytes()
                    pcm = bytes(32000) + speech + bytes(38400) + speech + bytes(96000)
                    for seq, pos in enumerate(range(0, len(pcm), 640)):
                        await websocket.send_bytes(encode_audio_frame(AudioFrame(
                            AudioKind.UPLINK_OPUS, stream_id, seq, seq * 20,
                            encoder.encode(pcm[pos:pos + 640]),
                        )))
                    await websocket.send_json({"type": "audio.end", "turn_id": str(turn_id),
                                               "stream_id": str(stream_id)})
                    controls, _ = await receive_until(websocket, "assistant.final")
                    endpoints = [e for e in controls if e["type"] == "audio.input_ended"]
                    assert len(endpoints) == 1 and endpoints[0]["reason"] == "speech_ended"
                    assert endpoints[0]["stream_id"] == str(stream_id)
                    assert any(e["type"] == "asr.final" for e in controls)
                    assert not any(e["type"] == "error" for e in controls)
                    await websocket.send_json({"type": "ping", "nonce": "after-endpoint"})
                    controls, _ = await receive_until(websocket, "pong")
                    assert not any(e["type"] == "error" for e in controls)
        finally:
            encoder.close()
            await server.close()
    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_devices_reports_active_sessions() -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession() as client:
                async with client.get(str(server.make_url("/api/v1/admin/devices"))) as response:
                    assert response.status == 200
                    assert await response.json() == {"devices": []}
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_offline_device_retains_details_and_history_but_rejects_commands() -> None:
    async def scenario() -> None:
        defaults = Settings()
        application = create_application(replace(
            defaults, auth=AuthConfig(allow_anonymous=False, tokens={"known-device": "test-token"})
        ))
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession() as client:
                devices_url = server.make_url("/api/v1/admin/devices")
                async with client.get(devices_url) as response:
                    known = (await response.json())["devices"]
                    assert known[0]["online"] is False
                    assert "test-token" not in str(known)
                websocket = await client.ws_connect(server.make_url("/v1/device/ws"))
                await send_hello(websocket, "known-device", "test-token", remote_control=True)
                async with client.get(devices_url) as response:
                    live = (await response.json())["devices"][0]
                    assert live["online"] is True
                await application.conversations.append_turn("known-device", "昨天的请求", "昨天的答复")
                await application.controls.update_state(
                    "known-device", DeviceStateMessage(type="device.state", revision=7)
                )
                record = await application.controls.create_command("known-device", "music.play", {})
                await application.controls.mark_failed(record.command_id, "previous failure")
                await websocket.close()
                for _ in range(50):
                    if await application.registry.count() == 0:
                        break
                    await asyncio.sleep(0.01)
                assert await application.registry.count() == 0
                async with client.get(devices_url) as response:
                    offline = (await response.json())["devices"][0]
                    assert offline["online"] is False and offline["phase"] == "offline"
                    assert "remote_control_v1" in offline["capabilities"]
                    assert offline["connected_at"] == live["connected_at"]
                async with client.get(
                    server.make_url("/api/v1/admin/devices/known-device/state")
                ) as response:
                    state = await response.json()
                    assert state["online"] is False and state["state"]["revision"] == 7
                async with client.get(
                    server.make_url("/api/v1/admin/devices/known-device/conversations")
                ) as response:
                    assert (await response.json())["messages"][-1]["content"] == "昨天的答复"
                async with client.get(
                    server.make_url("/api/v1/admin/devices/known-device/commands")
                ) as response:
                    assert str(record.command_id) in await response.text()
                async with client.post(
                    server.make_url("/api/v1/admin/devices/known-device/commands"),
                    json={"name": "music.play", "payload": {}},
                ) as response:
                    assert response.status == 409
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_providers_returns_safe_llm_configuration() -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession() as client:
                async with client.get(str(server.make_url("/api/v1/admin/providers"))) as response:
                    assert response.status == 200
                    payload = await response.json()
                    assert payload["llm"] == {
                        "provider": "mock",
                        "model": "mock",
                        "base_url": None,
                        "api": None,
                        "thinking": None,
                        "api_key_configured": True,
                    }
                    assert "api_key" not in payload["llm"]
                    assert payload["asr"] == {
                        "provider": "mock",
                        "endpoint": None,
                        "model": "mock",
                        "api_key_configured": False,
                    }
                    assert payload["tts"] == {
                        "provider": "mock",
                        "endpoint": None,
                        "model": "mock",
                        "voice": "",
                        "api_key_configured": False,
                    }
        finally:
            await server.close()

    asyncio.run(scenario())


class InterruptibleLLM:
    def __init__(self) -> None:
        self.block = asyncio.Event()

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context
        if messages[-1].content == "first":
            yield "旧响应。"
            await self.block.wait()
        else:
            yield "新响应。"

    async def close(self) -> None:
        return None


class NeverLLM:
    def __init__(self) -> None:
        self.block = asyncio.Event()

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context, messages
        await self.block.wait()
        yield "unreachable"

    async def close(self) -> None:
        return None


class ErrorLLM:
    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context, messages
        raise ProviderFailure("LLM unavailable")
        yield "unreachable"

    async def close(self) -> None:
        return None


@pytest.mark.integration
def test_real_websocket_voice_turn() -> None:
    async def scenario() -> None:
        defaults = Settings()
        providers = replace(
            defaults.providers,
            mock=MockProviderConfig(transcript="语音测试", response="好的。"),
        )
        settings = replace(defaults, providers=providers)
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        encoder = OpusEncoder(16_000, 1, 20, 24_000)
        event_types: list[str] = []
        binary_frames = 0
        try:
            async with ClientSession() as client:
                websocket = await client.ws_connect(url, protocols=("binbot-bbvp",))
                assert websocket.protocol == "binbot-bbvp"
                await websocket.send_json(
                    {
                        "type": "hello",
                        "protocol_version": 1,
                        "device_id": "integration-test",
                        "client_version": "0.1.0",
                        "uplink": {
                            "codec": "opus",
                            "sample_rate": 16_000,
                            "channels": 1,
                            "frame_ms": 20,
                        },
                        "downlink": {
                            "codec": "opus",
                            "sample_rate": 24_000,
                            "channels": 1,
                            "frame_ms": 20,
                        },
                    }
                )
                for _ in range(2):
                    event_types.append((await websocket.receive_json())["type"])

                turn_id = uuid4()
                stream_id = uuid4()
                await websocket.send_json(
                    {
                        "type": "turn.start",
                        "turn_id": str(turn_id),
                        "stream_id": str(stream_id),
                        "wake_word": "你好米雅",
                    }
                )
                encoded_silence = encoder.encode(bytes(320 * 2))
                for sequence in range(12):
                    await websocket.send_bytes(
                        encode_audio_frame(
                            AudioFrame(
                                AudioKind.UPLINK_OPUS,
                                stream_id,
                                sequence,
                                sequence * 20,
                                encoded_silence,
                            )
                        )
                    )
                await websocket.send_json(
                    {
                        "type": "audio.end",
                        "turn_id": str(turn_id),
                        "stream_id": str(stream_id),
                    }
                )

                while "assistant.final" not in event_types:
                    message = await asyncio.wait_for(websocket.receive(), timeout=3)
                    if message.type == WSMsgType.TEXT:
                        event_types.append(message.json()["type"])
                    elif message.type == WSMsgType.BINARY:
                        binary_frames += 1
                    else:
                        raise AssertionError(f"unexpected WebSocket message: {message.type}")
                await websocket.close()
        finally:
            encoder.close()
            await server.close()

        assert event_types[:2] == ["hello.ack", "state"]
        assert "asr.final" in event_types
        assert "audio.start" in event_types
        assert "audio.end" in event_types
        assert binary_frames > 0

    asyncio.run(scenario())


@pytest.mark.integration
def test_authenticated_text_turn_with_downlink_audio() -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            auth=AuthConfig(allow_anonymous=False, tokens={"auth-device": "secret"}),
            providers=replace(
                defaults.providers,
                mock=MockProviderConfig(transcript="unused", response="好。"),
            ),
        )
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            async with ClientSession() as client:
                unauthorized = await client.ws_connect(url)
                await unauthorized.send_json(
                    {
                        "type": "hello",
                        "protocol_version": 1,
                        "device_id": "auth-device",
                        "client_version": "integration-test",
                        "token": "wrong",
                        "uplink": {
                            "codec": "opus",
                            "sample_rate": 16_000,
                            "channels": 1,
                            "frame_ms": 20,
                        },
                        "downlink": {
                            "codec": "opus",
                            "sample_rate": 24_000,
                            "channels": 1,
                            "frame_ms": 20,
                        },
                    }
                )
                assert (await unauthorized.receive_json())["code"] == "authentication_failed"
                await unauthorized.close()

                websocket = await client.ws_connect(url)
                hello = await send_hello(websocket, "auth-device", "secret")
                assert [event["type"] for event in hello] == ["hello.ack", "state"]
                turn_id = uuid4()
                await websocket.send_json(
                    {"type": "text.input", "turn_id": str(turn_id), "text": "你好"}
                )
                controls, binary_frames = await receive_until(websocket, "assistant.final")
                assert binary_frames > 0
                assert "audio.start" in {event["type"] for event in controls}
                assert "audio.end" in {event["type"] for event in controls}
                await websocket.close()
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_cancellation_and_stale_turn_isolation_over_websocket() -> None:
    async def scenario() -> None:
        settings = Settings()
        llm = InterruptibleLLM()
        providers = ProviderBundle(
            MockASRProvider("unused"), llm, MockTTSProvider()
        )
        application = create_application(settings, providers)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            async with ClientSession() as client:
                websocket = await client.ws_connect(url)
                await send_hello(websocket, "cancel-device")
                first_turn = uuid4()
                await websocket.send_json(
                    {
                        "type": "text.input",
                        "turn_id": str(first_turn),
                        "text": "first",
                    }
                )
                await receive_until(websocket, "assistant.delta")
                await websocket.send_json(
                    {
                        "type": "turn.cancel",
                        "turn_id": str(first_turn),
                        "reason": "user_interrupt",
                    }
                )
                cancelled, _ = await receive_until(websocket, "turn.cancelled")
                assert next(
                    event for event in cancelled if event["type"] == "turn.cancelled"
                )["turn_id"] == str(first_turn)

                second_turn = uuid4()
                await websocket.send_json(
                    {
                        "type": "text.input",
                        "turn_id": str(second_turn),
                        "text": "second",
                    }
                )
                controls, _ = await receive_until(websocket, "assistant.final")
                finals = [event for event in controls if event["type"] == "assistant.final"]
                assert [event["turn_id"] for event in finals] == [str(second_turn)]
                await websocket.close()
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
@pytest.mark.parametrize(
    ("llm", "expected_code"),
    [(NeverLLM(), "provider_timeout"), (ErrorLLM(), "provider_failure")],
)
def test_provider_timeout_and_error_end_turn(llm: object, expected_code: str) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            pipeline=replace(defaults.pipeline, llm_timeout_seconds=0.02),
        )
        providers = ProviderBundle(
            MockASRProvider("unused"),  # type: ignore[arg-type]
            llm,  # type: ignore[arg-type]
            MockTTSProvider(),  # type: ignore[arg-type]
        )
        application = create_application(settings, providers)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            async with ClientSession() as client:
                websocket = await client.ws_connect(url)
                await send_hello(websocket, f"failure-{expected_code}")
                await websocket.send_json(
                    {"type": "text.input", "turn_id": str(uuid4()), "text": "test"}
                )
                controls, _ = await receive_until(websocket, "error")
                error = next(event for event in controls if event["type"] == "error")
                assert error["code"] == expected_code
                later, _ = await receive_until(websocket, "state")
                states = [
                    event["state"]
                    for event in [*controls, *later]
                    if event["type"] == "state"
                ]
                assert "error" in states
                await websocket.close()
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_invalid_binary_frame_closes_connection() -> None:
    async def scenario() -> None:
        application = create_application(Settings())
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            async with ClientSession() as client:
                websocket = await client.ws_connect(url)
                await send_hello(websocket, "invalid-frame")
                await websocket.send_bytes(b"not-a-BBV1-frame")
                error = await websocket.receive_json()
                assert error["code"] == "protocol_violation"
                close = await websocket.receive()
                assert close.type in {WSMsgType.CLOSE, WSMsgType.CLOSED}
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_conversation_store_isolates_devices_over_reconnects() -> None:
    async def run_turn(
        url: str,
        device: str,
        text: str,
        application: object,
    ) -> None:
        async with ClientSession() as client:
            websocket = await client.ws_connect(url)
            await send_hello(websocket, device)
            await websocket.send_json(
                {"type": "text.input", "turn_id": str(uuid4()), "text": text}
            )
            await receive_until(websocket, "assistant.final")
            await websocket.close()
        for _ in range(200):
            if await application.registry.count() == 0:  # type: ignore[attr-defined]
                break
            await asyncio.sleep(0.01)
        else:
            raise AssertionError("connection actor was not released after WebSocket close")

    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            providers=replace(
                defaults.providers,
                mock=MockProviderConfig(transcript="unused", response="回答。"),
            ),
        )
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            await run_turn(url, "history-a", "A1", application)
            await run_turn(url, "history-b", "B1", application)
            await run_turn(url, "history-a", "A2", application)
            history_a = await application.conversations.load("history-a")
            history_b = await application.conversations.load("history-b")
            assert [message.content for message in history_a] == [
                "A1",
                "回答。",
                "A2",
                "回答。",
            ]
            assert [message.content for message in history_b] == ["B1", "回答。"]
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_remote_control_state_conversations_and_gif(tmp_path: object) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            server=replace(
                defaults.server,
                asset_directory=str(tmp_path),
                video_start_timeout_seconds=0.1,
            ),
        )
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        url = str(server.make_url("/v1/device/ws")).replace("http://", "ws://")
        try:
            async with ClientSession() as client:
                websocket = await client.ws_connect(url)
                await send_hello(websocket, "remote-device", remote_control=True)

                command_url = server.make_url(
                    "/api/v1/admin/devices/remote-device/commands"
                )
                async with client.post(
                    command_url,
                    json={"name": "todo.add", "payload": {"text": "测试待办"}},
                ) as response:
                    assert response.status == 202
                    command = await response.json()
                delivered = await websocket.receive_json()
                assert delivered["type"] == "device.command"
                assert delivered["name"] == "todo.add"
                assert delivered["command_id"] == command["command_id"]

                async with client.post(
                    command_url,
                    json={"name": "voice.wake", "payload": {}},
                ) as response:
                    assert response.status == 202
                    wake_command = await response.json()
                wake_delivered = await websocket.receive_json()
                assert wake_delivered["type"] == "device.command"
                assert wake_delivered["name"] == "voice.wake"
                assert wake_delivered["command_id"] == wake_command["command_id"]

                idempotency_headers = {"Idempotency-Key": "pause-request-1"}
                async with client.post(
                    command_url,
                    json={"name": "music.pause", "payload": {}},
                    headers=idempotency_headers,
                ) as response:
                    assert response.status == 202
                    first_pause = await response.json()
                pause_delivered = await websocket.receive_json()
                assert pause_delivered["command_id"] == first_pause["command_id"]
                async with client.post(
                    command_url,
                    json={"name": "music.pause", "payload": {}},
                    headers=idempotency_headers,
                ) as response:
                    assert response.status == 202
                    second_pause = await response.json()
                assert second_pause["command_id"] == first_pause["command_id"]

                await websocket.send_json(
                    {
                        "type": "device.command.result",
                        "command_id": command["command_id"],
                        "success": True,
                        "message": "todo added",
                    }
                )
                await websocket.send_json(
                    {
                        "type": "device.state",
                        "revision": 1,
                        "todos": [{"text": "测试待办", "done": False}],
                        "music": {
                            "track_index": 0,
                            "position_seconds": 0,
                            "active": False,
                            "playing": False,
                        },
                        "display": {"gif": "default.gif"},
                    }
                )
                await asyncio.sleep(0)

                async with client.get(server.make_url(
                    "/api/v1/admin/devices/remote-device/state"
                )) as response:
                    payload = await response.json()
                    assert payload["online"] is True
                    assert payload["state"]["todos"][0]["text"] == "测试待办"

                await application.conversations.append_turn(
                    "remote-device", "你好", "你好，我是 BinBot。"
                )
                async with client.get(server.make_url(
                    "/api/v1/admin/devices/remote-device/conversations"
                )) as response:
                    payload = await response.json()
                    assert [item["content"] for item in payload["messages"]] == [
                        "你好",
                        "你好，我是 BinBot。",
                    ]

                gif_content = (
                    Path(__file__).parents[2]
                    / "binbot_web/public/binbot-right-component.gif"
                ).read_bytes()
                async with client.put(
                    server.make_url(
                        "/api/v1/admin/devices/remote-device/display/gif"
                    ),
                    data=gif_content,
                    headers={"Content-Type": "image/gif"},
                ) as response:
                    assert response.status == 202
                    upload = await response.json()
                gif_command = await websocket.receive_json()
                assert gif_command["name"] == "display.gif"
                asset_path = gif_command["payload"]["asset_path"]
                async with client.get(server.make_url(asset_path)) as response:
                    assert response.status == 200
                    normalized_gif = await response.read()
                    assert normalized_gif.startswith((b"GIF87a", b"GIF89a"))
                    assert normalized_gif[6:10] == (216).to_bytes(2, "little") * 2
                    assert normalized_gif != gif_content
                assert upload["asset_path"] == asset_path

                async with client.get(server.make_url(
                    f"/api/v1/admin/commands/{command['command_id']}"
                )) as response:
                    completed = await response.json()
                    assert completed["status"] == "completed"

                async with client.get(server.make_url(
                    "/api/v1/admin/devices/remote-device/video.mjpeg"
                )) as response:
                    assert response.status in {200, 502}
                await websocket.close()
                assert upload["asset_id"]
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_session_protects_management_api(monkeypatch: object) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            admin=replace(
                defaults.admin,
                allow_unauthenticated_loopback=False,
                token_env="BINBOT_TEST_ADMIN_TOKEN",
            ),
        )
        monkeypatch.setenv(  # type: ignore[attr-defined]
            "BINBOT_TEST_ADMIN_TOKEN", "test-admin-secret"
        )
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                devices_url = server.make_url("/api/v1/admin/devices")
                async with client.get(devices_url) as response:
                    assert response.status == 401
                    assert "WWW-Authenticate" not in response.headers
                async with client.post(
                    server.make_url("/api/v1/admin/session"),
                    json={"token": "test-admin-secret"},
                ) as response:
                    assert response.status == 200
                    assert await response.json() == {"status": "ok"}
                async with client.get(devices_url) as response:
                    assert response.status == 200
                    assert await response.json() == {"devices": []}
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_session_authenticates_component_workspace(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path,
) -> None:
    async def scenario() -> None:
        defaults = Settings()
        monkeypatch.setenv("BINBOT_TEST_ADMIN_PASSWORD", "workspace-password")
        monkeypatch.setenv("BINBOT_TEST_COMPONENT_TOKEN", "read-only-service")
        settings = replace(
            defaults,
            admin=replace(defaults.admin, allow_unauthenticated_loopback=False,
                          username_env="BINBOT_TEST_ADMIN_USERNAME",
                          password_env="BINBOT_TEST_ADMIN_PASSWORD"),
            component=replace(defaults.component, directory=str(tmp_path / "components"),
                              token_env="BINBOT_TEST_COMPONENT_TOKEN", scopes=("device:read",)),
        )
        monkeypatch.setenv("BINBOT_TEST_ADMIN_USERNAME", "operator")
        server = TestServer(create_application(settings).http)
        await server.start_server()
        routes = (
            ("/api/v1/admin/firmware", 200),
            ("/api/v1/admin/ota/jobs?device_id=screen-device", 200),
            ("/api/v1/admin/devices/screen-device/components/component-1?fresh=false", 404),
        )
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                for path, _ in routes:
                    async with client.get(server.make_url(path)) as response:
                        assert response.status == 401
                async with client.post(server.make_url("/api/v1/admin/session"), json={
                    "username": "operator", "password": "workspace-password",
                }) as response:
                    assert response.status == 200
                for path, expected in routes:
                    async with client.get(server.make_url(path)) as response:
                        assert response.status == expected
                        body = await response.json()
                        if expected == 200:
                            assert body["items"] == []
                        else:
                            assert body["code"] == "NO_SNAPSHOT"
                async with client.post(server.make_url("/api/v1/admin/logout")) as response:
                    assert response.status == 200
                for path, _ in routes:
                    async with client.get(server.make_url(path)) as response:
                        assert response.status == 401

            async with ClientSession(headers={"Authorization": "Bearer read-only-service"}) as machine:
                async with machine.get(server.make_url(routes[1][0])) as response:
                    assert response.status == 200
                async with machine.get(server.make_url(routes[0][0])) as response:
                    assert response.status == 403
        finally:
            await server.close()

    asyncio.run(scenario())


@pytest.mark.integration
def test_admin_session_accepts_username_password_and_remember_choice(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    async def scenario() -> None:
        defaults = Settings()
        settings = replace(
            defaults,
            admin=replace(
                defaults.admin,
                allow_unauthenticated_loopback=False,
                username_env="BINBOT_TEST_ADMIN_USERNAME",
                password_env="BINBOT_TEST_ADMIN_PASSWORD",
                token_env="BINBOT_UNUSED_ADMIN_TOKEN",
            ),
        )
        monkeypatch.setenv("BINBOT_TEST_ADMIN_USERNAME", "operator")
        monkeypatch.setenv("BINBOT_TEST_ADMIN_PASSWORD", "correct horse battery")
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                login_url = server.make_url("/api/v1/admin/session")
                async with client.post(
                    login_url,
                    json={"username": "operator", "password": "wrong"},
                ) as response:
                    assert response.status == 401
                    assert "WWW-Authenticate" not in response.headers
                async with client.post(
                    login_url,
                    json={"username": "operator", "password": "correct horse battery", "remember": True},
                ) as response:
                    assert response.status == 200
                    cookie = response.cookies["binbot_admin"]
                    assert cookie["max-age"] == str(defaults.admin.remember_days * 86400)
                async with client.get(server.make_url("/api/v1/admin/devices")) as response:
                    assert response.status == 200
        finally:
            await server.close()

    asyncio.run(scenario())
