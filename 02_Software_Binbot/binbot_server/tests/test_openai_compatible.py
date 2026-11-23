from __future__ import annotations

import asyncio
import json
from uuid import uuid4

import httpx
import pytest

from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import ChatMessage, TurnContext
from binbot_server.domain.model import ToolCall
from binbot_server.infrastructure.providers.openai_compatible import OpenAILLMProvider


def test_llm_provider_preserves_remote_error_details() -> None:
    async def scenario() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            assert request.url.path == "/api/v3/chat/completions"
            return httpx.Response(
                404,
                json={"error": {"code": "ModelNotOpen", "message": "activate model"}},
            )

        provider = OpenAILLMProvider(
            "https://ark.example/api/v3", "test-key", "model-a", 1.0, "system"
        )
        await provider._client.aclose()
        provider._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        context = TurnContext(uuid4(), uuid4(), "device-a")
        try:
            with pytest.raises(ProviderFailure, match="ModelNotOpen"):
                _ = [
                    chunk
                    async for chunk in provider.stream(
                        context, [ChatMessage(role="user", content="hello")]
                    )
                ]
        finally:
            await provider.close()

    asyncio.run(scenario())


def test_llm_provider_streams_responses_text_without_reasoning() -> None:
    async def scenario() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            assert request.url.path == "/api/v3/responses"
            body = json.loads(request.content)
            assert body == {
                "model": "model-a",
                "input": [
                    {"role": "system", "content": "system"},
                    {"role": "user", "content": "hello"},
                ],
                "stream": True,
                "thinking": {"type": "disabled"},
            }
            events = (
                'event: response.reasoning_summary_text.delta\n'
                'data: {"type":"response.reasoning_summary_text.delta","delta":"hidden"}\n\n'
                'event: response.output_text.delta\n'
                'data: {"type":"response.output_text.delta","delta":"米雅"}\n\n'
                'event: response.output_text.delta\n'
                'data: {"type":"response.output_text.delta","delta":"在线"}\n\n'
                'event: response.completed\n'
                'data: {"type":"response.completed","response":{"status":"completed"}}\n\n'
                'data: [DONE]\n\n'
            )
            return httpx.Response(200, text=events, headers={"content-type": "text/event-stream"})

        provider = OpenAILLMProvider(
            "https://ark.example/api/v3",
            "test-key",
            "model-a",
            1.0,
            "system",
            "responses",
            "disabled",
        )
        await provider._client.aclose()
        provider._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        context = TurnContext(uuid4(), uuid4(), "device-a")
        try:
            chunks = [
                chunk
                async for chunk in provider.stream(
                    context, [ChatMessage(role="user", content="hello")]
                )
            ]
            assert chunks == ["米雅", "在线"]
        finally:
            await provider.close()

    asyncio.run(scenario())


def test_llm_responses_requires_completed_event() -> None:
    async def scenario() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            del request
            return httpx.Response(
                200,
                text='data: {"type":"response.output_text.delta","delta":"partial"}\n\n',
            )

        provider = OpenAILLMProvider(
            "https://ark.example/api/v3",
            "test-key",
            "model-a",
            1.0,
            "system",
            "responses",
        )
        await provider._client.aclose()
        provider._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        context = TurnContext(uuid4(), uuid4(), "device-a")
        try:
            with pytest.raises(ProviderFailure, match="without a completed event"):
                _ = [
                    chunk
                    async for chunk in provider.stream(
                        context, [ChatMessage(role="user", content="hello")]
                    )
                ]
        finally:
            await provider.close()

    asyncio.run(scenario())


def test_llm_provider_parses_chat_tool_calls() -> None:
    async def scenario() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            assert request.url.path == "/api/v3/chat/completions"
            body = json.loads(request.content)
            assert body["tools"][0]["function"]["name"] == "device.list"
            assert body["messages"][-1] == {"role": "user", "content": "查设备"}
            return httpx.Response(
                200,
                json={
                    "choices": [{
                        "message": {
                            "role": "assistant",
                            "content": "",
                            "tool_calls": [{
                                "id": "call-1",
                                "type": "function",
                                "function": {
                                    "name": "device.list", "arguments": "{}"
                                },
                            }],
                        }
                    }]
                },
            )

        provider = OpenAILLMProvider(
            "https://ark.example/api/v3", "test-key", "model-a", 1.0, "system"
        )
        await provider._client.aclose()
        provider._client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
        try:
            response = await provider.complete(
                TurnContext(uuid4(), uuid4(), "device-a"),
                [ChatMessage(role="user", content="查设备")],
                [{"name": "device.list", "description": "List", "parameters": {"type": "object"}}],
            )
            assert response.text == ""
            assert response.tool_calls == (ToolCall("call-1", "device.list", {}),)
        finally:
            await provider.close()

    asyncio.run(scenario())
