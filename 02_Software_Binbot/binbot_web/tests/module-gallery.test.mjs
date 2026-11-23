import { after, test } from "node:test";
import assert from "node:assert/strict";
import "fake-indexeddb/auto";
import { createServer } from "vite";

const server = await createServer({
  cacheDir: "node_modules/.vite-gallery-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const remote = new Map();
globalThis.fetch = async (path, options={}) => {
  const match=path.match(/^\/api\/v1\/admin\/devices\/([^/]+)\/gallery(?:\/([^/?]+))?(?:\?.*)?$/);
  assert.ok(match); assert.equal(options.credentials, 'same-origin');
  const device=decodeURIComponent(match[1]), id=match[2], key=`${device}:${id}`;
  if(options.method==='PUT') { const row={ id, assetId: `server-${id}`, name:id, createdAt: remote.size + 1 }; remote.set(key,row); return Response.json(row); }
  if(options.method==='PATCH') { const old=remote.get(key); if(old) remote.set(key,{...old,...JSON.parse(options.body)}); return Response.json({}); }
  if(options.method==='DELETE') { remote.delete(key); return new Response(null,{status:204}); }
  return Response.json({ device_id:device, images:[...remote].filter(([key])=>key.startsWith(device+':')).map(([,value])=>value).reverse() });
};
const gallery = await server.ssrLoadModule("/src/features/modules/moduleGallery.ts");
const asset = (scope, id, createdAt = 1) => ({
  key: `${scope}:${id}`,
  scope,
  id,
  name: id,
  blob: new Blob([`image:${id}`], { type: "image/png" }),
  createdAt,
});

test("gallery blobs persist with independent device and preview scopes and newest first", async () => {
  const live = gallery.galleryScope("live", "device-a"),
    demo = gallery.galleryScope("demo", "device-a"),
    other = gallery.galleryScope("live", "device-b");
  await gallery.putGalleryAsset(asset(live, "one", 1));
  await gallery.putGalleryAsset(asset(live, "two", 2));
  await gallery.putGalleryAsset(asset(demo, "one"));
  await gallery.putGalleryAsset(asset(other, "one"));
  const rows = await gallery.listGalleryAssets(live);
  assert.deepEqual(
    rows.map((row) => row.id),
    ["two", "one"],
  );
  assert.equal(rows[0].serverId, "server-two");
  assert.match(rows[0].sourceUrl, /device-a\/gallery\/two\/source$/);
  assert.equal(rows[0].blob.size, 0);
  assert.equal((await gallery.listGalleryAssets(demo)).length, 1);
  await gallery.removeGalleryAsset(live, "one");
  assert.equal((await gallery.listGalleryAssets(demo)).length, 1);
  assert.equal((await gallery.listGalleryAssets(other)).length, 1);
});

test("renaming and receipt updates in parallel retain each other and deleted images stay deleted", async () => {
  const scope = gallery.galleryScope("live", "transactions");
  await gallery.putGalleryAsset(asset(scope, "a"));
  await Promise.all([
    gallery.patchGalleryAsset(scope, "a", { name: "new name" }),
    gallery.patchGalleryAsset(scope, "a", {
      push: { jobId: "job", status: "transferring", percent: 25, at: 10 },
    }),
    gallery.patchGalleryAsset(scope, "a", {
      delivery: { commandId: "command", status: "completed", at: 12 },
    }),
  ]);
  const [saved] = await gallery.listGalleryAssets(scope);
  assert.equal(saved.name, "new name");
  assert.equal(saved.push.percent, 25);
  assert.equal(saved.delivery.status, "completed");
  await gallery.removeGalleryAsset(scope, "a");
  await gallery.patchGalleryAsset(scope, "a", {
    push: { jobId: "job", status: "succeeded", percent: 100, at: 13 },
  });
  assert.deepEqual(await gallery.listGalleryAssets(scope), []);
});

test("image type comes from file signatures and empty, unsupported and oversized files are refused", async () => {
  for (const [bytes, type] of [
    [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), "image/png"],
    ["GIF89a", "image/gif"],
    [new Uint8Array([255, 216, 255, 0]), "image/jpeg"],
    ["RIFF0000WEBP", "image/webp"],
  ])
    assert.equal(
      await gallery.validateImageFile(
        new Blob([bytes], { type: "text/plain" }),
      ),
      type,
    );
  await assert.rejects(gallery.validateImageFile(new Blob()), /16 MB/);
  await assert.rejects(
    gallery.validateImageFile({ size: gallery.MAX_IMAGE_BYTES + 1 }),
    /16 MB/,
  );
  await assert.rejects(
    gallery.validateImageFile(
      new Blob(["<svg>unsafe</svg>"], { type: "image/png" }),
    ),
    /有效的/,
  );
});

test("corrupt images fail actual decoding; successful, failed and aborted previews release temporary URLs", async () => {
  const OriginalImage = globalThis.Image,
    create = URL.createObjectURL,
    revoke = URL.revokeObjectURL;
  let behavior = "good",
    releases = [],
    allocations = 0;
  globalThis.Image = class {
    naturalWidth = 240;
    naturalHeight = 240;
    set src(value) {
      if (!value || behavior === "pending") return;
      const next = behavior;
      queueMicrotask(() =>
        next === "good" ? this.onload?.() : this.onerror?.(),
      );
    }
  };
  URL.createObjectURL = () => `blob:test-${++allocations}`;
  URL.revokeObjectURL = (url) => releases.push(url);
  try {
    await gallery.verifyImagePreview(new Blob(["good"]));
    behavior = "bad";
    await assert.rejects(
      gallery.verifyImagePreview(new Blob(["bad"])),
      /已损坏/,
    );
    behavior = "pending";
    const controller = new AbortController(),
      pending = gallery.verifyImagePreview(
        new Blob(["wait"]),
        controller.signal,
      );
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    await assert.rejects(
      gallery.verifyImagePreview(new Blob(), controller.signal),
      { name: "AbortError" },
    );
    assert.deepEqual(releases, ["blob:test-1", "blob:test-2", "blob:test-3"]);
    assert.equal(allocations, 3);
  } finally {
    globalThis.Image = OriginalImage;
    URL.createObjectURL = create;
    URL.revokeObjectURL = revoke;
  }
});
