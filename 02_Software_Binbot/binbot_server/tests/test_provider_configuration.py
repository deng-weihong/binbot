import asyncio
from dataclasses import replace
from pathlib import Path

from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer
import pytest

from binbot_server.bootstrap import create_application
from binbot_server.config import AdminConfig, ComponentConfig, Settings
from binbot_server.infrastructure import provider_settings
from binbot_server.infrastructure.providers import OpenAILLMProvider


@pytest.mark.integration
def test_provider_save_auth_runtime_persistence_and_conflicts(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("BINBOT_ADMIN_USERNAME", "test-admin")
    monkeypatch.setenv("BINBOT_ADMIN_PASSWORD", "test-password")
    monkeypatch.setenv("BINBOT_MODEL_API_KEY", "existing-test-key")
    settings = replace(Settings(), admin=AdminConfig(allow_unauthenticated_loopback=False),
                       component=ComponentConfig(directory=str(tmp_path / "components")))

    async def scenario() -> None:
        application = create_application(settings)
        shared_bundle = application.providers
        async with TestServer(application.http) as server, ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
            url = server.make_url("/api/v1/admin/providers/configuration")
            async with client.get(url) as response:
                assert response.status == 401
            async with client.put(url, json={}) as response:
                assert response.status == 401
            async with client.post(server.make_url("/api/v1/admin/session"), json={
                "username": "test-admin", "password": "test-password", "remember": False,
            }) as response:
                assert response.status == 200
            async with client.get(url) as response:
                before = await response.json()
                assert response.status == 200
                assert before["credentials_configured"]["openai"]
                assert "existing-test-key" not in str(before)
            changed = before["config"]
            changed["llm"] = "openai"
            changed["openai"]["llm_model"] = "saved-model"
            async with client.put(url, json={"revision": before["revision"], "config": changed}) as response:
                saved = await response.json()
                assert response.status == 200, saved
                assert saved["config"]["openai"]["llm_model"] == "saved-model"
                assert saved["revision"] != before["revision"]
            assert application.providers is shared_bundle
            assert isinstance(shared_bundle.llm, OpenAILLMProvider)
            assert application.agent.enabled
            path = provider_settings.settings_path(settings)
            assert path.stat().st_mode & 0o777 == 0o600
            restored = provider_settings.load_provider_overrides(settings)
            assert restored.providers.llm == "openai"
            assert restored.providers.openai.llm_model == "saved-model"
            async with client.put(url, json={"revision": before["revision"], "config": changed}) as response:
                assert response.status == 409
            invalid = {**changed, "llm": "unsupported"}
            async with client.put(url, json={"revision": saved["revision"], "config": invalid}) as response:
                assert response.status == 400
            original_persist = provider_settings.persist

            def fail_persist(*_args: object) -> None:
                raise OSError("unwritable")

            monkeypatch.setattr(provider_settings, "persist", fail_persist)
            async with client.put(url, json={"revision": saved["revision"], "config": changed,
                                             "secrets": {"openai": "replacement-test-key"}}) as response:
                assert response.status == 500
            assert restored.providers.openai.api_key() == "existing-test-key"
            async with client.get(url) as response:
                assert (await response.json())["revision"] == saved["revision"]
            monkeypatch.setattr(provider_settings, "persist", original_persist)
            async with client.put(url, json={"revision": saved["revision"], "config": changed,
                                             "secrets": {"openai": "replacement-test-key"}}) as response:
                secret_save = await response.json()
                assert response.status == 200
                assert "replacement-test-key" not in str(secret_save)
            assert provider_settings.load_provider_overrides(settings).providers.openai.api_key() == "replacement-test-key"
            async with client.put(url, json={"revision": secret_save["revision"], "config": changed}) as response:
                assert response.status == 200
            assert restored.providers.openai.api_key() == "replacement-test-key"

    asyncio.run(scenario())


def test_configuration_rejects_extra_fields_urls_and_shared_secret_conflicts() -> None:
    settings = Settings()
    values = provider_settings.provider_values(settings)
    assert provider_settings.validate_values(settings, values) == settings
    with pytest.raises(ValueError, match="invalid provider"):
        provider_settings.validate_values(settings, {**values, "secret": "unknown"})
    with pytest.raises(ValueError, match="URL"):
        provider_settings.validate_values(settings, {**values, "openai": {**values["openai"], "base_url": "file:///tmp/key"}})
    with pytest.raises(ValueError, match="same API key"):
        provider_settings.apply_secrets(settings, {"asr": "different-one", "tts": "different-two"})
