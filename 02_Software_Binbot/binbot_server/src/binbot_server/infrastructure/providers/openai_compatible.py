from __future__ import annotations

import io
import json
import wave
from collections.abc import AsyncIterator, Sequence

import httpx

from binbot_server.application.ports import ASRStream, AgentModelResponse, TranscriptEvent
from binbot_server.domain.errors import ProviderFailure
from binbot_server.domain.model import AudioFormat, ChatMessage, ToolCall, TurnContext


def _wav_bytes(pcm: bytes, sample_rate: int) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()


class _OpenAIASRStream:
    def __init__(self, provider: "OpenAIASRProvider", sample_rate: int):
        self._provider = provider
        self._sample_rate = sample_rate
        self._pcm = bytearray()
        self._closed = False

    async def push(self, pcm: bytes) -> Sequence[TranscriptEvent]:
        if self._closed:
            return ()
        self._pcm.extend(pcm)
        if len(self._pcm) > self._provider.max_audio_bytes:
            raise ProviderFailure("ASR audio exceeded the configured turn limit")
        return ()

    async def finish(self) -> TranscriptEvent:
        if self._closed:
            raise ProviderFailure("ASR stream is already closed")
        self._closed = True
        text = await self._provider.transcribe(bytes(self._pcm), self._sample_rate)
        self._pcm.clear()
        return TranscriptEvent(text=text, final=True)

    async def abort(self) -> None:
        self._closed = True
        self._pcm.clear()


class _OpenAIBase:
    def __init__(self, base_url: str, api_key: str, timeout_seconds: float):
        self._base_url = base_url.rstrip("/")
        self._client = httpx.AsyncClient(
            headers={"Authorization": f"Bearer {api_key}"}, timeout=timeout_seconds
        )

    async def close(self) -> None:
        await self._client.aclose()


class OpenAIASRProvider(_OpenAIBase):
    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        timeout_seconds: float,
        max_audio_seconds: int = 60,
    ):
        super().__init__(base_url, api_key, timeout_seconds)
        self._model = model
        self.max_audio_bytes = max_audio_seconds * 16_000 * 2

    async def open_stream(self, context: TurnContext, audio_format: AudioFormat) -> ASRStream:
        del context
        return _OpenAIASRStream(self, audio_format.sample_rate)

    async def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        if not pcm:
            return ""
        response = await self._client.post(
            f"{self._base_url}/audio/transcriptions",
            data={"model": self._model},
            files={"file": ("speech.wav", _wav_bytes(pcm, sample_rate), "audio/wav")},
        )
        try:
            response.raise_for_status()
            text = response.json().get("text", "")
        except (httpx.HTTPError, ValueError, AttributeError) as exc:
            raise ProviderFailure(f"ASR request failed: {response.text[:300]}") from exc
        return str(text).strip()


class OpenAILLMProvider(_OpenAIBase):
    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        timeout_seconds: float,
        system_prompt: str,
        api_style: str = "chat_completions",
        thinking: str = "",
    ):
        super().__init__(base_url, api_key, timeout_seconds)
        self._model = model
        self._system_prompt = system_prompt
        self._api_style = api_style
        self._thinking = thinking

    async def stream(
        self, context: TurnContext, messages: Sequence[ChatMessage]
    ) -> AsyncIterator[str]:
        del context
        request_messages = [{"role": "system", "content": self._system_prompt}]
        request_messages.extend({"role": item.role, "content": item.content} for item in messages)
        try:
            if self._api_style == "responses":
                async for delta in self._stream_responses(request_messages):
                    yield delta
            else:
                async for delta in self._stream_chat_completions(request_messages):
                    yield delta
        except httpx.HTTPError as exc:
            raise ProviderFailure(f"LLM request failed: {exc}") from exc

    async def complete(
        self,
        context: TurnContext,
        messages: Sequence[ChatMessage],
        tools: Sequence[dict[str, object]],
    ) -> AgentModelResponse:
        del context
        if self._api_style != "chat_completions":
            raise ProviderFailure("tool calling currently requires chat_completions API")
        request_messages: list[dict[str, object]] = [
            {"role": "system", "content": self._system_prompt}
        ]
        request_messages.extend(_chat_message_payload(item) for item in messages)
        request: dict[str, object] = {
            "model": self._model,
            "messages": request_messages,
        }
        if tools:
            request["tools"] = [
                {
                    "type": "function",
                    "function": {
                        "name": item["name"],
                        "description": item["description"],
                        "parameters": item["parameters"],
                    },
                }
                for item in tools
            ]
        if self._thinking:
            request["thinking"] = {"type": self._thinking}
        try:
            response = await self._client.post(
                f"{self._base_url}/chat/completions", json=request
            )
            await self._raise_for_error(response)
            message = response.json()["choices"][0]["message"]
            text = message.get("content") or ""
            calls: list[ToolCall] = []
            for item in message.get("tool_calls") or []:
                function = item["function"]
                arguments = json.loads(function.get("arguments") or "{}")
                if not isinstance(arguments, dict):
                    raise ProviderFailure("LLM tool arguments must be an object")
                calls.append(
                    ToolCall(
                        call_id=str(item["id"]),
                        name=str(function["name"]),
                        arguments=dict(arguments),
                    )
                )
            return AgentModelResponse(text=str(text), tool_calls=tuple(calls))
        except (httpx.HTTPError, KeyError, IndexError, TypeError, ValueError) as exc:
            if isinstance(exc, ProviderFailure):
                raise
            raise ProviderFailure(f"LLM tool request returned malformed data: {exc}") from exc

    async def _stream_chat_completions(
        self, request_messages: list[dict[str, str]]
    ) -> AsyncIterator[str]:
        request: dict[str, object] = {
            "model": self._model,
            "messages": request_messages,
            "stream": True,
        }
        if self._thinking:
            request["thinking"] = {"type": self._thinking}
        async with self._client.stream(
            "POST",
            f"{self._base_url}/chat/completions",
            json=request,
        ) as response:
            await self._raise_for_error(response)
            async for line in response.aiter_lines():
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    payload = json.loads(data)
                    delta = payload["choices"][0]["delta"].get("content")
                except (json.JSONDecodeError, KeyError, IndexError, TypeError) as exc:
                    raise ProviderFailure("LLM returned a malformed streaming event") from exc
                if delta:
                    yield str(delta)

    async def _stream_responses(
        self, request_messages: list[dict[str, str]]
    ) -> AsyncIterator[str]:
        request: dict[str, object] = {
            "model": self._model,
            "input": request_messages,
            "stream": True,
        }
        if self._thinking:
            request["thinking"] = {"type": self._thinking}
        completed = False
        async with self._client.stream(
            "POST", f"{self._base_url}/responses", json=request
        ) as response:
            await self._raise_for_error(response)
            async for line in response.aiter_lines():
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    payload = json.loads(data)
                    event_type = payload["type"]
                except (json.JSONDecodeError, KeyError, TypeError) as exc:
                    raise ProviderFailure("LLM returned a malformed streaming event") from exc
                if event_type == "response.output_text.delta":
                    delta = payload.get("delta")
                    if not isinstance(delta, str):
                        raise ProviderFailure("LLM returned a malformed text delta")
                    if delta:
                        yield delta
                elif event_type == "response.completed":
                    completed = True
                elif event_type in {"response.failed", "response.incomplete"}:
                    details = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
                    raise ProviderFailure(f"LLM response did not complete: {details[:300]}")
        if not completed:
            raise ProviderFailure("LLM response stream ended without a completed event")

    @staticmethod
    async def _raise_for_error(response: httpx.Response) -> None:
        if not response.is_error:
            return
        body = (await response.aread()).decode("utf-8", errors="replace")
        raise ProviderFailure(
            f"LLM request failed ({response.status_code}): {body[:300]}"
        )


def _chat_message_payload(message: ChatMessage) -> dict[str, object]:
    payload: dict[str, object] = {"role": message.role, "content": message.content}
    if message.name is not None:
        payload["name"] = message.name
    if message.tool_call_id is not None:
        payload["tool_call_id"] = message.tool_call_id
    if message.tool_calls:
        payload["tool_calls"] = [
            {
                "id": call.call_id,
                "type": "function",
                "function": {
                    "name": call.name,
                    "arguments": json.dumps(call.arguments, ensure_ascii=False),
                },
            }
            for call in message.tool_calls
        ]
    return payload


class OpenAITTSProvider(_OpenAIBase):
    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        voice: str,
        timeout_seconds: float,
    ):
        super().__init__(base_url, api_key, timeout_seconds)
        self._model = model
        self._voice = voice

    async def stream(
        self, context: TurnContext, text: str, sample_rate: int
    ) -> AsyncIterator[bytes]:
        del context
        if sample_rate != 24_000:
            raise ProviderFailure("OpenAI-compatible PCM TTS currently requires 24 kHz output")
        pending = bytearray()
        try:
            async with self._client.stream(
                "POST",
                f"{self._base_url}/audio/speech",
                json={
                    "model": self._model,
                    "voice": self._voice,
                    "input": text,
                    "response_format": "pcm",
                },
            ) as response:
                response.raise_for_status()
                async for chunk in response.aiter_bytes():
                    pending.extend(chunk)
                    even_size = len(pending) & ~1
                    if even_size:
                        yield bytes(pending[:even_size])
                        del pending[:even_size]
        except httpx.HTTPError as exc:
            raise ProviderFailure(f"TTS request failed: {exc}") from exc
        if pending:
            pending.append(0)
            yield bytes(pending)
