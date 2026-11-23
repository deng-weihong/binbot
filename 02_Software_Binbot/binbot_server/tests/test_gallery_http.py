import asyncio
import json

import pytest
from aiohttp import ClientSession, web
from aiohttp.test_utils import TestServer

from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.transport import gallery_http


@pytest.mark.integration
def test_gallery_can_save_offline_replay_rename_and_delete_without_crossing_devices(
    tmp_path, monkeypatch
):
    async def run():
        store = SqliteWorkspaceStore(str(tmp_path / "gallery.sqlite"))
        root = tmp_path / "images"
        calls = []

        def prepare(normalizer, directory, content, name):
            calls.append(content)
            asset = "12345678-1234-1234-1234-123456789abc"
            path = directory / asset
            path.mkdir(parents=True)
            (path / "manifest.json").write_text(json.dumps({"id": asset, "name": name}))
            return asset, path

        monkeypatch.setattr(gallery_http, "prepare_module_image", prepare)
        app = web.Application()
        gallery_http.register_gallery_routes(app, store, None, root, 1024)
        async with TestServer(app) as server, ClientSession() as client:
            path = server.make_url("/api/v1/admin/devices/one/gallery/image-one")
            content = b"GIF89aexample"
            async with client.put(path, data=content) as response:
                assert response.status == 201
                saved = await response.json()
            async with client.put(path, data=content) as response:
                assert (
                    response.status == 200
                    and (await response.json())["assetId"] == saved["assetId"]
                )
            async with client.put(path, data=b"GIF89adifferent") as response:
                assert response.status == 409
            assert len(calls) == 1
            async with client.get(server.make_url("/api/v1/admin/devices/two/gallery")) as response:
                assert (await response.json())["images"] == []
            await asyncio.gather(
                client.patch(path, json={"name": "renamed"}),
                client.patch(path, json={"delivery": {"commandId": "one", "status": "completed"}}),
            )
            row = (await store.gallery("one"))[0]
            assert row["name"] == "renamed" and row["delivery"]["status"] == "completed"
            async with client.get(str(path) + "/source") as response:
                assert await response.read() == content
            async with client.delete(path) as response:
                assert response.status == 204
            async with client.patch(path, json={"name": "late"}) as response:
                assert response.status == 404
            assert await store.gallery("one") == []
        await store.close()

    asyncio.run(run())
