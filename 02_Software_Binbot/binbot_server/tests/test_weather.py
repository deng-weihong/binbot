import asyncio
from unittest.mock import AsyncMock

import pytest
from aiohttp import ClientError, ClientSession, web
from aiohttp.test_utils import TestServer

from binbot_server.transport.weather_http import WeatherCache, _WEATHER_KEY, register_weather_routes


def test_weather_location_cache_and_projection() -> None:
    async def scenario() -> None:
        cache = WeatherCache()
        cache.fetch = AsyncMock(side_effect=[
            {"results": [{"latitude": 30.2, "longitude": 120.1}]},
            {"current": {"temperature_2m": 21}, "daily": {"temperature_2m_max": [24]}, "unused": "data"},
        ])
        result = await cache.get({"city": "test-city"})
        assert result == {"current": {"temperature_2m": 21}, "daily": {"temperature_2m_max": [24]}}
        assert await cache.get({"city": "test-city"}) == result
        assert cache.fetch.await_count == 2
        assert cache.fetch.call_args.args[1]["latitude"] == 30.2
        assert "is_day" in cache.fetch.call_args.args[1]["current"]
        for query in ({}, {"latitude": "nan", "longitude": "0"}, {"latitude": "0"}, {"city": "x" * 121}):
            with pytest.raises(web.HTTPBadRequest):
                await cache.get(query)
        cache.entries[next(iter(cache.entries))] = (0, result)
        cache.fetch = AsyncMock(side_effect=ClientError("offline"))
        with pytest.raises(ClientError):
            await cache.get({"city": "test-city"})
    asyncio.run(scenario())


def test_weather_http_errors_and_coordinates() -> None:
    async def scenario() -> None:
        app = web.Application()
        register_weather_routes(app)
        app[_WEATHER_KEY].fetch = AsyncMock(return_value={"current": {}, "daily": {}})
        async with TestServer(app) as server, ClientSession() as client:
            async with client.get(server.make_url("/v1/device/weather?latitude=30&longitude=120")) as response:
                assert response.status == 200
                assert await response.json() == {"current": {}, "daily": {}}
            async with client.get(server.make_url("/v1/device/weather?latitude=200&longitude=0")) as response:
                assert response.status == 400
            app[_WEATHER_KEY].fetch = AsyncMock(side_effect=ClientError("offline"))
            async with client.get(server.make_url("/v1/device/weather?city=missing")) as response:
                assert response.status == 503
                assert await response.json() == {"error": "weather_unavailable"}
    asyncio.run(scenario())
