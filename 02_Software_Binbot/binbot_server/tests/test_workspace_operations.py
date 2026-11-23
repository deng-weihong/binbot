import asyncio
from copy import deepcopy
from datetime import UTC, datetime
from uuid import UUID

import pytest

from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.tools import DeviceCommandService
from binbot_server.application.workspace import WorkspaceService
from binbot_server.domain.errors import StateConflict
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.transport.protocol import DeviceCommandResultMessage


def test_settings_require_real_receipts_are_idempotent_and_keep_devices_isolated():
    async def run():
        store = SqliteWorkspaceStore(":memory:")
        controls, registry = DeviceControlStore(), DeviceSessionRegistry()
        commands = DeviceCommandService(registry, controls)
        service = WorkspaceService(store, controls, commands, registry)
        executions = []

        async def send(event):
            executions.append(event)
            payload = event["payload"]
            state = await controls.state("one")
            report = state["control"]
            report["revision"] += 1
            report["features"]["watchFace"] = payload
            await controls.update_control_state("one", report)
            await controls.complete(
                "one",
                DeviceCommandResultMessage(
                    type="device.command.result",
                    command_id=UUID(event["command_id"]),
                    success=True,
                    message="applied",
                ),
            )

        await registry.register_control("one", "client", send)
        await registry.control_seen(
            "one", "client", ready=True, boot_id="a" * 32, capabilities=["workspace_v1"]
        )
        await controls.update_control_state(
            "one",
            {
                "boot_id": "a" * 32,
                "revision": 1,
                "ready": True,
                "capabilities": ["workspace_v1"],
                "features": {},
            },
        )
        before = await service.snapshot("one")
        other = await service.snapshot("two")
        face = {
            "themeId": "napping-cat",
            "standbyId": "napping-cat",
            "idleSeconds": 300,
            "shuffle": False,
        }
        body = {
            "revision": before["revision"],
            "event_id": "save-one",
            "data": {"deviceName": "书房", "features": {"watchFace": face}},
        }
        saved = await service.save("one", body)
        assert saved["data"]["deviceName"] == "书房"
        assert saved["reported"]["watchFace"] == face
        assert len(executions) == 1
        assert (await service.save("one", body))["revision"] == saved["revision"]
        assert len(executions) == 1
        assert (await service.snapshot("two"))["data"] == other["data"]
        with pytest.raises(StateConflict):
            await service.save("one", {**body, "event_id": "stale"})
        offline = await service.snapshot("offline")
        with pytest.raises(StateConflict):
            await service.save(
                "offline",
                {
                    "revision": offline["revision"],
                    "event_id": "offline",
                    "data": {"microphone": False},
                },
            )
        assert (await service.snapshot("offline"))["revision"] == offline["revision"]
        await store.close()

    asyncio.run(run())


def test_offline_reviews_commit_once_before_ack_and_keep_legacy_history_honest():
    async def run():
        store = SqliteWorkspaceStore(":memory:")
        controls = DeviceControlStore()
        service = WorkspaceService(store, controls, None, None)
        acks = []

        async def apply(device, name, payload, key):
            if name == "learning.events.ack":
                current = await service.document(device)
                assert (
                    current["data"]["wordbooks"]["books"][0]["words"][0]["review"]["reviewCount"]
                    == 1
                )
                acks.append(payload["event_ids"])
            return {"status": "completed"}

        service.apply = apply
        initial = await service.document("one")
        word = initial["data"]["wordbooks"]["books"][0]["words"][0]["word"]
        event = {
            "id": "offline:1",
            "book_id": "daily-english",
            "word": word,
            "at": int(datetime.now(UTC).timestamp()),
            "remembered": True,
        }
        report = {
            "boot_id": "a" * 32,
            "features": {"vocabulary": {"legacyKnownWords": [word], "legacyReviewedCount": 17}},
            "learning_events": [event],
        }
        await service.ingest("one", deepcopy(report))
        await service.ingest("one", deepcopy(report))
        data = (await service.document("one"))["data"]
        assert acks == [["offline:1"], ["offline:1"]]
        assert data["wordbooks"]["books"][0]["words"][0]["review"]["reviewCount"] == 1
        assert len(data["wordbooks"]["books"][0]["words"][0]["review"]["history"]) == 1
        assert data["legacy_learning_statistics"]["reviewed"] == 17
        assert (await service.document("two"))["data"]["wordbooks"]["books"][0]["words"][0][
            "review"
        ]["reviewCount"] == 0
        await store.close()

    asyncio.run(run())


def test_server_gallery_index_survives_restart_without_cross_device_leaks(tmp_path):
    async def run():
        path = str(tmp_path / "workspace.sqlite")
        store = SqliteWorkspaceStore(path)
        await store.gallery_put("one", "image", {"id": "image", "name": "one"})
        await store.gallery_put("two", "image", {"id": "image", "name": "two"})
        await store.close()
        store = SqliteWorkspaceStore(path)
        assert (await store.gallery("one"))[0]["name"] == "one"
        await store.gallery_delete("one", "image")
        assert await store.gallery("one") == []
        assert (await store.gallery("two"))[0]["name"] == "two"
        await store.close()

    asyncio.run(run())


def test_reminder_save_compares_reported_setting_when_server_default_already_matches():
    async def run():
        store = SqliteWorkspaceStore(":memory:")
        controls, registry = DeviceControlStore(), DeviceSessionRegistry()
        service = WorkspaceService(store, controls, None, registry)
        await controls.update_control_state(
            "one",
            {
                "boot_id": "a" * 32,
                "revision": 1,
                "features": {"notifications": False, "timezone": "Asia/Shanghai"},
            },
        )
        seen = []

        async def apply(device, name, payload, key):
            seen.append((name, payload))
            return {"status": "completed"}

        service.apply = apply
        current = await service.document("one")
        assert current["data"]["notifications"] is True
        await service.save(
            "one",
            {
                "revision": current["revision"],
                "event_id": "restore-reminders",
                "data": {"notifications": True},
            },
        )
        assert seen == [
            (
                "preferences.set",
                {"notifications": True, "timezone": "Asia/Shanghai", "utcOffset": 28800},
            )
        ]
        await store.close()

    asyncio.run(run())


def test_modern_apps_keep_the_merged_calendar_when_legacy_voice_calendar_updates():
    async def run():
        registry = DeviceSessionRegistry()

        async def send(event):
            raise AssertionError("calendar should be owned by the workspace")

        await registry.register_control("one", "client", send)
        await registry.control_seen(
            "one", "client", ready=True, boot_id="a" * 32, capabilities=["workspace_v1"]
        )
        assert await registry.workspace_enabled("one")
        await registry.send_event("one", {"type": "device.calendar", "events": []})
        await registry.unregister_control("one", "client")
        assert await registry.workspace_enabled("one")
        await registry.send_event("one", {"type": "device.calendar", "events": []})
        assert not await registry.workspace_enabled("other")

    asyncio.run(run())
