import asyncio

import pytest

from binbot_server.application.camera import CameraSessions
from binbot_server.config import CameraConfig
from binbot_server.domain.errors import StateConflict


class CameraRegistry:
    def __init__(self):
        self.online = True
        self.events = []

    async def snapshot(self):
        return [{"id": "camera-1", "capabilities": ["camera_publish_v1"]}] if self.online else []

    async def send_event(self, device_id, event):
        if not self.online:
            raise StateConflict("device is not connected")
        self.events.append((device_id, event["lease_seconds"]))


def test_viewers_share_publisher_and_stop_after_last_viewer():
    async def scenario():
        now = [100.0]
        registry = CameraRegistry()
        sessions = CameraSessions(CameraConfig(), registry, lambda: now[0])
        first = await sessions.open("camera-1")
        second = await sessions.open("camera-1")
        assert registry.events == [("camera-1", 45)]
        await sessions.release("camera-1", first)
        now[0] += 20
        await sessions.refresh("camera-1", second)
        await sessions.tick()
        assert registry.events[-1] == ("camera-1", 45)
        await sessions.release("camera-1", second)
        now[0] += 29
        await sessions.tick()
        assert registry.events[-1] == ("camera-1", 45)
        now[0] += 1
        await sessions.tick()
        assert registry.events[-1] == ("camera-1", 0)
        assert not sessions.demands
    asyncio.run(scenario())


def test_expired_viewer_cannot_renew_and_eventually_stops_publisher():
    async def scenario():
        now = [100.0]
        registry = CameraRegistry()
        sessions = CameraSessions(CameraConfig(), registry, lambda: now[0])
        viewer = await sessions.open("camera-1")
        now[0] += 36
        with pytest.raises(KeyError):
            await sessions.refresh("camera-1", viewer)
        await sessions.tick()
        now[0] += 30
        await sessions.tick()
        assert registry.events[-1] == ("camera-1", 0)
        assert not sessions.viewers
    asyncio.run(scenario())


def test_new_viewer_cancels_delayed_stop_and_device_recovers():
    async def scenario():
        now = [100.0]
        registry = CameraRegistry()
        sessions = CameraSessions(CameraConfig(), registry, lambda: now[0])
        first = await sessions.open("camera-1")
        await sessions.release("camera-1", first)
        now[0] += 20
        second = await sessions.open("camera-1")
        registry.online = False
        now[0] += 11
        await sessions.tick()
        registry.online = True
        now[0] += 11
        await sessions.refresh("camera-1", second)
        await sessions.tick()
        assert registry.events[-1] == ("camera-1", 45)
        assert all(seconds != 0 for _, seconds in registry.events)
        await sessions.close()
        assert registry.events[-1] == ("camera-1", 0)
    asyncio.run(scenario())


def test_sessions_are_bound_to_device_and_require_online_camera():
    async def scenario():
        registry = CameraRegistry()
        sessions = CameraSessions(CameraConfig(), registry)
        viewer = await sessions.open("camera-1")
        with pytest.raises(KeyError):
            await sessions.refresh("other-device", viewer)
        await sessions.release("other-device", viewer)
        assert viewer in sessions.viewers
        registry.online = False
        with pytest.raises(StateConflict):
            await sessions.open("camera-1")
        registry.online = True
        await sessions.close()
    asyncio.run(scenario())


def test_disconnect_during_open_does_not_leave_an_unwatched_publisher():
    async def scenario():
        registry = CameraRegistry()

        async def disconnect(device_id, event):
            raise ConnectionResetError()

        registry.send_event = disconnect
        sessions = CameraSessions(CameraConfig(), registry)
        with pytest.raises(StateConflict):
            await sessions.open("camera-1")
        assert not sessions.viewers and not sessions.demands
        await sessions.tick()
    asyncio.run(scenario())
