import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-camera-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const { CameraConnection, createCameraTransport, cameraSessionPath } =
  await server.ssrLoadModule("/src/features/home/cameraSession.ts");
const { devices } = await server.ssrLoadModule("/src/shared/model/data.ts");
const { DeviceWorkspaceApp: App } = await server.ssrLoadModule("/src/app/App.tsx");

test("camera requests encode the device and lease, preserve session auth, and release with keepalive", async () => {
  const calls = [];
  const transport = createCameraTransport(async (path, options) => {
    calls.push([path, options]);
    if (path.endsWith("/webrtc"))
      return new Response("v=0\r\nanswer", {
        headers: { "Content-Type": "application/sdp" },
      });
    if (options.method === "DELETE") return new Response(null, { status: 204 });
    return Response.json(
      options.method === "POST"
        ? { session_id: "lease / 1", heartbeat_seconds: 10 }
        : { status: "ok" },
    );
  });
  const id = "desk / one";
  const controller = new AbortController();
  const lease = await transport.open(id);
  await transport.refresh(id, lease.session_id, controller.signal);
  assert.equal(
    await transport.negotiate(
      id,
      lease.session_id,
      "v=0\r\noffer",
      controller.signal,
    ),
    "v=0\r\nanswer",
  );
  await transport.close(id, lease.session_id);
  const base = "/api/v1/admin/devices/desk%20%2F%20one/camera/sessions";
  assert.equal(cameraSessionPath(id), base);
  assert.deepEqual(
    calls.map(([path]) => path),
    [
      base,
      `${base}/lease%20%2F%201`,
      `${base}/lease%20%2F%201/webrtc`,
      `${base}/lease%20%2F%201`,
    ],
  );
  assert.ok(
    calls.every(([, options]) => options.credentials === "same-origin"),
  );
  assert.equal(calls[2][1].body, "v=0\r\noffer");
  assert.equal(calls[3][1].keepalive, true);
});

test("missing services, denied access, offline devices and malformed leases never appear connected", async () => {
  const responses = [
    [
      new Response("<!doctype html>", {
        headers: { "Content-Type": "text/html" },
      }),
      /尚未连接/,
    ],
    [new Response("unauthorized", { status: 401 }), /观看权限/],
    [Response.json({ error: "设备离线" }, { status: 409 }), /设备离线/],
    [Response.json({ session_id: "lease", heartbeat_seconds: 0 }), /会话无效/],
  ];
  for (const [response, expected] of responses)
    await assert.rejects(
      createCameraTransport(async () => response).open("one"),
      expected,
    );
  const transport = createCameraTransport(
    async () =>
      new Response("<html>", { headers: { "Content-Type": "text/html" } }),
  );
  await assert.rejects(
    transport.negotiate("one", "lease", "v=0", new AbortController().signal),
    /尚未连接/,
  );
});

class FakePeer {
  iceGatheringState = "complete";
  connectionState = "new";
  remoteDescriptions = [];
  closed = false;
  addTransceiver(kind, options) {
    assert.equal(kind, "video");
    assert.equal(options.direction, "recvonly");
    return {};
  }
  async createOffer() {
    return { type: "offer", sdp: "v=0\r\noffer" };
  }
  async setLocalDescription(offer) {
    this.localDescription = offer;
  }
  async setRemoteDescription(answer) {
    this.remoteDescriptions.push(answer);
  }
  close() {
    this.closed = true;
  }
}
function harness(id = "one", overrides = {}) {
  const calls = [],
    phases = [],
    streams = [];
  const peer = new FakePeer();
  const transport = {
    async open(device) {
      calls.push(["open", device]);
      return { session_id: `${device}-lease`, heartbeat_seconds: 10 };
    },
    async refresh(device, session) {
      calls.push(["refresh", device, session]);
    },
    async negotiate(device, session, _offer, signal) {
      calls.push(["negotiate", device, session, signal]);
      return "v=0\r\nanswer";
    },
    async close(device, session) {
      calls.push(["close", device, session]);
    },
    ...overrides,
  };
  const connection = new CameraConnection(
    id,
    {
      phase: (phase, detail) => phases.push([phase, detail]),
      stream: (stream) => streams.push(stream),
    },
    { transport, createPeer: () => peer },
  );
  return { connection, peer, calls, phases, streams };
}

test("a viewer starts on demand, reports live only on playback, and releases just its own lease", async () => {
  const h = harness();
  assert.deepEqual(h.calls, []);
  await h.connection.start();
  assert.deepEqual(
    h.phases.map(([phase]) => phase),
    ["connecting"],
  );
  const track = h.peer.ontrack;
  track({ streams: [{ id: "one-video" }] });
  h.connection.markPlaying();
  assert.deepEqual(h.streams, [{ id: "one-video" }]);
  assert.deepEqual(
    h.phases.map(([phase]) => phase),
    ["connecting", "playing"],
  );
  h.connection.dispose();
  h.connection.dispose();
  track({ streams: [{ id: "stale-video" }] });
  h.connection.markPlaying();
  assert.equal(h.peer.closed, true);
  assert.deepEqual(
    h.calls.filter(([method]) => method === "close"),
    [["close", "one", "one-lease"]],
  );
  assert.equal(h.streams.length, 1);
  assert.equal(h.phases.length, 2);
});

test("leaving while a camera lease opens releases the late lease and cannot attach video", async () => {
  let resolve;
  const pending = new Promise((done) => {
    resolve = done;
  });
  const h = harness("old", { open: () => pending });
  const opening = h.connection.start();
  h.connection.dispose();
  resolve({ session_id: "late-lease", heartbeat_seconds: 10 });
  await opening;
  assert.deepEqual(h.calls, [["close", "old", "late-lease"]]);
  assert.equal(h.peer.closed, true);
  assert.equal(h.peer.remoteDescriptions.length, 0);
  assert.deepEqual(
    h.phases.map(([phase]) => phase),
    ["connecting"],
  );
});

test("late negotiation from another device is ignored after switching", async () => {
  let resolve;
  const pending = new Promise((done) => {
    resolve = done;
  });
  const old = harness("old", { negotiate: () => pending });
  const opening = old.connection.start();
  await new Promise((done) => setImmediate(done));
  old.connection.dispose();
  const current = harness("new");
  await current.connection.start();
  resolve("v=0\r\nold-answer");
  await opening;
  assert.equal(old.peer.remoteDescriptions.length, 0);
  assert.equal(current.peer.remoteDescriptions.length, 1);
  assert.deepEqual(
    old.calls.filter(([method]) => method === "close"),
    [["close", "old", "old-lease"]],
  );
  current.connection.dispose();
});

test("failed connections stop the peer and expose an error instead of live status", async () => {
  const h = harness("one", {
    negotiate: async () => {
      throw new Error("设备离线");
    },
  });
  await h.connection.start();
  assert.equal(h.peer.closed, true);
  assert.deepEqual(
    h.phases.map(([phase]) => phase),
    ["connecting", "error"],
  );
  assert.equal(h.phases.at(-1)[1], "设备离线");
  assert.deepEqual(h.streams, [null]);
  assert.equal(h.calls.filter(([method]) => method === "close").length, 1);
});

for (const device of devices) {
  for (const page of ["today", "camera", "scenes"]) {
    test(`${device.id}/${page} shows the home preview without a separate camera page or navigation entry`, () => {
      globalThis.window = {
        location: { pathname: `/devices/${device.id}/${page}` },
      };
      globalThis.localStorage = { getItem: () => null };
      const html = renderToStaticMarkup(React.createElement(App));
      assert.ok(!html.includes(`href="/devices/${device.id}/camera"`));
      assert.ok(!html.includes("camera-page"));
      assert.ok(html.includes("camera-frame"));
      assert.ok(html.includes('id="camera-preview"'));
      assert.ok(!html.includes("camera-live-tag"));
      assert.ok(!html.includes("设备设置分类"));
      assert.ok(!html.includes("noise-player-card"));
      assert.ok(html.includes("首页快捷控制"));
      assert.ok(!html.includes("打开摄像头页面"));
      assert.ok(!html.includes(`href="/devices/${device.id}/noise"`));
      assert.ok(!html.includes(`href="/devices/${device.id}/scenes"`));
      assert.ok(html.includes("home-agenda"));
      assert.ok(html.includes("今天的日程"));
      assert.ok(html.includes('aria-labelledby="home-todos-title"'));
      assert.ok(html.includes('aria-label="新待办内容"'));
      assert.ok(html.includes('aria-label="添加日程"'));
      assert.ok(!html.includes("scenes-card"));
      assert.ok(!html.includes("新建安排"));
      assert.ok(html.includes('id="white-noise"'));
      assert.ok(html.includes("全屏预览"));
      assert.ok(html.includes("重新连接摄像头"));
      assert.ok(html.includes("镜像画面"));
      if (!device.online)
        assert.match(html, /disabled="" aria-label="连接摄像头预览"/);
    });
  }
}
