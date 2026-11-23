"""Browser sessions must be checked independently of the loopback exemption."""

import asyncio
from dataclasses import replace
from pathlib import Path

import pytest
from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer

from binbot_server.bootstrap import create_application
from binbot_server.config import Settings
from binbot_server.transport.http import _session_cookie


@pytest.mark.integration
@pytest.mark.parametrize("remember", [False, True])
def test_console_session_restore_and_logout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, remember: bool,
) -> None:
    defaults = Settings()
    settings = replace(
        defaults,
        server=replace(defaults.server, asset_directory=str(tmp_path / "assets")),
        admin=replace(defaults.admin, allow_unauthenticated_loopback=True,
                      username_env="BINBOT_CONSOLE_TEST_USERNAME",
                      password_env="BINBOT_CONSOLE_TEST_PASSWORD",
                      token_env="BINBOT_CONSOLE_TEST_UNUSED_TOKEN"),
        auth=replace(defaults.auth, tokens_env="BINBOT_CONSOLE_TEST_UNUSED_DEVICE_TOKENS"),
        component=replace(defaults.component, directory=str(tmp_path / "component"),
                          token_env="BINBOT_CONSOLE_TEST_COMPONENT_TOKEN"),
        vad=replace(defaults.vad, enabled=False),
        daily_report=replace(defaults.daily_report, enabled=False),
    )
    monkeypatch.setenv("BINBOT_CONSOLE_TEST_USERNAME", "operator")
    monkeypatch.setenv("BINBOT_CONSOLE_TEST_PASSWORD", "test-session-password")

    async def scenario() -> None:
        application = create_application(settings)
        server = TestServer(application.http)
        await server.start_server()
        try:
            async with ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
                session_url = server.make_url("/api/v1/admin/session")
                # Existing local tooling remains compatible, but the portal's
                # session check still requires an actual signed browser cookie.
                async with client.get(server.make_url("/api/v1/admin/devices")) as response:
                    assert response.status == 200
                async with client.get(session_url) as response:
                    assert response.status == 401
                    assert response.headers["Cache-Control"] == "no-store"
                async with client.post(session_url, json={"username": "operator", "password": "wrong"}) as response:
                    assert response.status == 401
                    assert "binbot_admin" not in response.cookies
                async with client.post(session_url, json={
                    "username": "operator", "password": "test-session-password", "remember": remember,
                }) as response:
                    assert response.status == 200
                    cookie = response.cookies["binbot_admin"]
                    assert cookie["httponly"]
                    assert cookie["samesite"] == "Strict"
                    assert cookie["path"] == "/api/v1/"
                    assert cookie["max-age"] == (str(settings.admin.remember_days * 86400) if remember else "")
                    assert response.headers["Cache-Control"] == "no-store"
                async with client.get(session_url) as response:
                    assert response.status == 200
                    assert await response.json() == {"authenticated": True, "username": "operator"}
                    assert response.headers["Cache-Control"] == "no-store"
                async with client.post(server.make_url("/api/v1/admin/logout"), json={}) as response:
                    assert response.status == 200
                    assert response.cookies["binbot_admin"]["max-age"] == "0"
                async with client.get(session_url) as response:
                    assert response.status == 401

                invalid_cookies = ["forged.signature", _session_cookie(settings, "different-user", True)]
                # A correctly signed cookie with a past expiry is still invalid.
                with monkeypatch.context() as clock:
                    clock.setattr("binbot_server.transport.http.time.time", lambda: 1)
                    invalid_cookies.append(_session_cookie(settings, "operator", True))
                for invalid in invalid_cookies:
                    client.cookie_jar.clear()
                    async with client.get(session_url, cookies={"binbot_admin": invalid}) as response:
                        assert response.status == 401
        finally:
            await server.close()

    asyncio.run(scenario())
