import asyncio
from dataclasses import replace
from urllib.parse import parse_qs, urlsplit

import pytest
from aiohttp import ClientSession, CookieJar
from aiohttp.test_utils import TestServer

from binbot_server.bootstrap import create_application
from binbot_server.config import Settings
from binbot_server.transport.http import _CONTEXT_KEY
from test_device_feishu import FakeClient, FakeConnection


@pytest.mark.integration
def test_device_feishu_http_requires_login_and_keeps_oauth_and_calendars_scoped(tmp_path, monkeypatch):
    monkeypatch.setenv("BINBOT_DEVICE_FEISHU_TEST_USER", "operator")
    monkeypatch.setenv("BINBOT_DEVICE_FEISHU_TEST_PASSWORD", "test-password")
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.HttpFeishuClient", FakeClient)
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.FeishuLongConnection", FakeConnection)
    monkeypatch.setattr("binbot_server.infrastructure.device_feishu.importlib.util.find_spec", lambda name: True)
    defaults = Settings()
    settings = replace(defaults,
        component=replace(defaults.component, directory=str(tmp_path / "component")),
        server=replace(defaults.server, asset_directory=str(tmp_path / "assets")),
        admin=replace(defaults.admin, allow_unauthenticated_loopback=False,
                      username_env="BINBOT_DEVICE_FEISHU_TEST_USER", password_env="BINBOT_DEVICE_FEISHU_TEST_PASSWORD"),
        vad=replace(defaults.vad, enabled=False),
        feishu=replace(defaults.feishu, calendar=replace(defaults.feishu.calendar, sync_interval_seconds=0)),
    )

    async def scenario():
        application = create_application(settings)
        async with TestServer(application.http) as server, ClientSession(cookie_jar=CookieJar(unsafe=True)) as client:
            async def request(method, path, body=None):
                async with client.request(method, server.make_url(path), json=body) as response:
                    return response.status, await response.json()

            for method, path in [("GET", "/feishu"), ("PUT", "/feishu"), ("GET", "/feishu/oauth/url"),
                                 ("POST", "/feishu/oauth/disconnect"), ("POST", "/calendar/sync"), ("GET", "/calendar/events")]:
                assert (await request(method, "/api/v1/admin/devices/device-a" + path))[0] == 401
            assert (await request("POST", "/api/v1/admin/session", {"username": "operator", "password": "test-password"}))[0] == 200
            for device_id in ("device-a", "device-b"):
                path = f"/api/v1/admin/devices/{device_id}/feishu"
                status, before = await request("GET", path)
                assert status == 200 and before["device_id"] == device_id
                config = {**before["config"], "enabled": True, "app_id": f"cli_{device_id}", "calendar_enabled": True}
                status, after = await request("PUT", path, {"revision": before["revision"], "config": config, "secrets": {"app_secret": f"secret-{device_id}"}})
                assert status == 200 and after["credentials_configured"]["app_secret"]
                assert f"secret-{device_id}" not in str(after)
                assert (await request("PUT", path, {"revision": "stale", "config": config}))[0] == 409
                redirect = str(server.make_url("/api/v1/feishu/oauth/callback"))
                auth_path = path + "/oauth/url?redirect_uri=" + redirect
                status, auth = await request("GET", auth_path)
                assert status == 200 and auth["device_id"] == device_id
                state = parse_qs(urlsplit(auth["authorize_url"]).query)["state"][0]
                # A query parameter cannot retarget another device's OAuth state.
                async with client.get(server.make_url(f"/api/v1/feishu/oauth/callback?code={device_id}&state={state}&device_id=other")) as response:
                    assert response.status == 200 and device_id in await response.text()
                manager = application.http[_CONTEXT_KEY].device_feishu
                await manager.runtime(device_id).calendar.stop()
                assert (await request("POST", f"/api/v1/admin/devices/{device_id}/calendar/sync"))[0] == 200
            for device_id in ("device-a", "device-b"):
                status, calendar = await request("GET", f"/api/v1/admin/devices/{device_id}/calendar")
                assert status == 200 and calendar["device_id"] == device_id
                assert calendar["events"][0]["summary"] == f"access-{device_id}"
            assert (await request("GET", "/api/v1/admin/devices/unconfigured/calendar"))[0] == 404
            assert (await request("GET", "/api/v1/admin/calendar/events?start_date=2026-11-01&end_date=2026-11-30"))[0] == 404
            assert (await request("GET", "/api/v1/admin/devices/device-a/feishu/oauth/url?redirect_uri=https://external.test/api/v1/feishu/oauth/callback"))[0] == 400
            status, disconnected = await request("POST", "/api/v1/admin/devices/device-a/feishu/oauth/disconnect")
            assert status == 200 and disconnected["account"]["authorized"] is False
            assert (await request("GET", "/api/v1/admin/devices/device-b/feishu"))[1]["account"]["display_name"] == "device-b"
            assert (await request("GET", "/api/v1/admin/devices/device-a/calendar"))[0] == 404

    asyncio.run(scenario())
