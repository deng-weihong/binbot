from __future__ import annotations

import asyncio
import math
import time
from typing import Any

from aiohttp import ClientError, ClientSession, ClientTimeout, web


class WeatherCache:
    def __init__(self) -> None:
        self.entries: dict[tuple[str, ...], tuple[float, dict[str, Any]]] = {}
        self.lock = asyncio.Lock()

    async def fetch(self, url: str, params: dict[str, Any]) -> dict[str, Any]:
        async with ClientSession(timeout=ClientTimeout(total=10), trust_env=True) as session:
            async with session.get(url, params=params) as response:
                response.raise_for_status()
                data = await response.json()
                if not isinstance(data, dict):
                    raise ValueError("invalid weather response")
                return data

    async def get(self, query: Any) -> dict[str, Any]:
        city = str(query.get("city", "")).strip()
        coordinates = "latitude" in query or "longitude" in query
        key: tuple[str, ...]
        if coordinates:
            try:
                lat, lon = float(query["latitude"]), float(query["longitude"])
            except (KeyError, ValueError, TypeError) as exc:
                raise web.HTTPBadRequest(text="invalid coordinates") from exc
            if not (math.isfinite(lat) and math.isfinite(lon) and -90 <= lat <= 90 and -180 <= lon <= 180):
                raise web.HTTPBadRequest(text="invalid coordinates")
            key = (f"{lat:.5f}", f"{lon:.5f}")
        elif city and len(city.encode("utf-8")) <= 120:
            key = (city,)
        else:
            raise web.HTTPBadRequest(text="weather location required")
        async with self.lock:
            cached = self.entries.get(key)
            if cached and time.monotonic() - cached[0] < 1800:
                return cached[1]
            async with asyncio.timeout(12):
                if not coordinates:
                    places = await self.fetch("https://geocoding-api.open-meteo.com/v1/search", {
                        "name": city, "count": 1, "language": "zh",
                    })
                    matches = places.get("results", [])
                    if not isinstance(matches, list) or not matches:
                        raise web.HTTPNotFound(text="weather city not found")
                    lat, lon = float(matches[0]["latitude"]), float(matches[0]["longitude"])
                forecast = await self.fetch("https://api.open-meteo.com/v1/forecast", {
                    "latitude": lat, "longitude": lon,
                    "current": "temperature_2m,weather_code,is_day",
                    "daily": "temperature_2m_max,temperature_2m_min",
                    "forecast_days": 1, "timezone": "auto", "timeformat": "unixtime",
                })
            current, daily = forecast.get("current"), forecast.get("daily")
            if not isinstance(current, dict) or not isinstance(daily, dict):
                raise ValueError("weather data unavailable")
            result = {"current": current, "daily": daily}
            if len(self.entries) >= 128:
                oldest = min(self.entries, key=lambda item: self.entries[item][0])
                self.entries.pop(oldest)
            self.entries[key] = (time.monotonic(), result)
            return result


_WEATHER_KEY = web.AppKey("module_weather", WeatherCache)


async def weather(request: web.Request) -> web.Response:
    try:
        result = await request.app[_WEATHER_KEY].get(request.query)
    except (ClientError, TimeoutError, ValueError, KeyError, IndexError):
        return web.json_response({"error": "weather_unavailable"}, status=503)
    return web.json_response(result, headers={"Cache-Control": "public, max-age=60"})


def register_weather_routes(app: web.Application) -> None:
    app[_WEATHER_KEY] = WeatherCache()
    app.router.add_get("/v1/device/weather", weather)
