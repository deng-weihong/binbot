import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createServer } from "vite";

const server = await createServer({
  cacheDir: "node_modules/.vite-console-api-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const {
  createConsoleTransport,
  parseDevices,
  matchingFirmware,
  jobPercent,
  terminalJob,
} = await server.ssrLoadModule("/src/shared/api/consoleApi.ts");
const { safeReturnPath, devicePath } =
  await server.ssrLoadModule("/src/shared/navigation.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const job = (extra = {}) => ({
  job_id: "j1",
  device_id: "desk:one",
  component_id: "component-1",
  status: "transferring",
  phase_percent: 40,
  ...extra,
});
const component = (extra = {}) => ({
  device_id: "desk:one",
  component_id: "component-1",
  profile: "round_screen",
  project: "round",
  healthy: true,
  online: true,
  capacity: 10000,
  ...extra,
});

test("login return paths preserve device fragments and reject external, malformed and traversal addresses", () => {
  for (const path of [
    "/devices",
    "/devices/desk%3Aone/modules#gallery",
    "/services#voice",
  ])
    assert.equal(safeReturnPath(path), path);
  for (const path of [
    "//example.org",
    "https://example.org",
    "/login",
    "/devices/../services",
    "/devices/%2e%2e/today",
    "/devices/a%2fb/today",
    "/devices/%ZZ/today",
    "/devices/a\\b",
    "/devices/a\n",
  ])
    assert.equal(safeReturnPath(path), "/devices", path);
  assert.equal(
    devicePath("desk:one", "modules"),
    "/devices/desk%3Aone/modules",
  );
});

test("real device lists validate identity and do not borrow demo names or status", () => {
  const [device] = parseDevices({
    devices: [
      {
        id: "desk:one",
        online: false,
        remote_ip: "192.168.1.3",
        capabilities: ["remote_control_v1", 4],
      },
    ],
  });
  assert.equal(device.name, "desk:one");
  assert.equal(device.online, false);
  assert.deepEqual(device.capabilities, ["remote_control_v1"]);
  for (const devices of [
    [{ id: "..", online: true }],
    [{ id: "a/b", online: true }],
    [{ id: "a", online: "true" }],
    [
      { id: "a", online: true },
      { id: "a", online: false },
    ],
  ])
    assert.throws(() => parseDevices({ devices }), /无效或重复/);
  assert.throws(() => parseDevices({}), /格式/);
});

test("session restoration uses the cookie check and login passes remember without storing a token", async () => {
  const calls = [];
  const api = createConsoleTransport(async (path, options) => {
    calls.push([path, options]);
    return Response.json(
      path.endsWith("session") && options.method !== "POST"
        ? { authenticated: true }
        : { status: "ok" },
    );
  });
  await api.checkSession();
  await api.login(" operator ", " secret for test ", true);
  await api.logout();
  assert.deepEqual(
    calls.map(([path]) => path),
    ["/api/v1/admin/session", "/api/v1/admin/session", "/api/v1/admin/logout"],
  );
  assert.equal(calls[0][1].method, undefined);
  assert.deepEqual(JSON.parse(calls[1][1].body), {
    username: "operator",
    password: " secret for test ",
    remember: true,
  });
  assert.ok(
    calls.every(
      ([, options]) =>
        options.credentials === "same-origin" && options.cache === "no-store",
    ),
  );
});

test("only unauthorized protected calls expire sessions; login errors and permission denials do not", async () => {
  let expired = 0;
  const api = createConsoleTransport(
    async () => new Response("denied", { status: 401 }),
    () => expired++,
  );
  await assert.rejects(api.login("a", "b", false), /账号或密码/);
  await assert.rejects(api.checkSession(undefined, false));
  assert.equal(expired, 0);
  await assert.rejects(api.devices(), /登录已失效/);
  assert.equal(expired, 1);
  await assert.rejects(
    createConsoleTransport(
      async () => Response.json({}, { status: 403 }),
      () => expired++,
    ).devices(),
    /无法执行/,
  );
  assert.equal(expired, 1);
  await assert.rejects(
    createConsoleTransport(
      async () => Response.json({ authenticated: false }),
      () => expired++,
    ).checkSession(),
  );
  assert.equal(expired, 2);
});

test("unavailable services and pre-aborted requests cannot appear successful", async () => {
  await assert.rejects(
    createConsoleTransport(async () => new Response("<html>")).devices(),
    /尚未连接/,
  );
  await assert.rejects(
    createConsoleTransport(async () => {
      throw new Error("network");
    }).devices(),
    /无法连接后台/,
  );
  let calls = 0;
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    createConsoleTransport(async () => {
      calls++;
      return Response.json({ devices: [] });
    }).devices(controller.signal),
    { name: "AbortError" },
  );
  assert.equal(calls, 0);
});

test("device, component, job and command mismatches are rejected before reaching the current workspace", async () => {
  const cases = [
    [{ device_id: "other" }, (api) => api.state("desk:one")],
    [
      component({ component_id: "slave-1" }),
      (api) => api.component("desk:one", "component-1"),
    ],
    [{ items: [job({ device_id: "other" })] }, (api) => api.jobs("desk:one")],
    [job({ job_id: "other" }), (api) => api.job("desk:one", "j1")],
    [
      { command_id: "other", device_id: "desk:one" },
      (api) => api.command("desk:one", "c1"),
    ],
    [
      job({ component_id: "slave-1" }),
      (api) => api.startUpgrade("desk:one", "component-1", "f1"),
    ],
    [job({ job_id: "other" }), (api) => api.cancel("desk:one", "j1")],
  ];
  for (const [payload, invoke] of cases)
    await assert.rejects(
      invoke(createConsoleTransport(async () => Response.json(payload))),
      /不一致/,
    );
});

test("gallery and screen sends use binary bodies, scoped endpoints and their actual pending receipts", async () => {
  const calls = [],
    blob = new Blob(["test image"], { type: "image/png" });
  const api = createConsoleTransport(async (path, options) => {
    calls.push([path, options]);
    return Response.json(
      path.includes("/modules/")
        ? {
            asset_id: "asset",
            name: "一张图",
            command: {
              command_id: "c1",
              device_id: "desk:one",
              status: "sent",
            },
          }
        : job(),
    );
  });
  const gallery = await api.uploadImage(
    "desk:one",
    blob,
    "一张图",
    "gallery",
    undefined,
    "retry-key",
  );
  const screen = await api.uploadImage(
    "desk:one",
    blob,
    "一张图",
    "component",
    undefined,
    "retry-key",
  );
  assert.match(
    calls[0][0],
    /^\/api\/v1\/admin\/devices\/desk%3Aone\/modules\/images\?name=/,
  );
  assert.match(calls[1][0], /\/components\/component-1\/gif\?name=/);
  assert.ok(
    calls.every(
      ([, options]) =>
        options.body === blob &&
        options.method === "PUT" &&
        options.headers["Idempotency-Key"] === "retry-key",
    ),
  );
  assert.equal(gallery.command.status, "sent");
  assert.equal(terminalJob(screen.status), false);
  assert.equal(jobPercent(screen), 40);
  assert.equal(
    jobPercent(job({ status: "verifying", phase_percent: 100 })),
    99,
  );
  assert.equal(jobPercent(job({ status: "succeeded" })), 100);
  assert.equal(jobPercent(job({ phase_percent: NaN })), 0);
});

test("firmware upload hashes bytes on plain HTTP, uses only a local API path, then registers", async () => {
  const file = new File([new Uint8Array(200).fill(17)], "firmware.bin"),
    calls = [];
  const api = createConsoleTransport(async (path, options) => {
    calls.push([path, options]);
    if (calls.length === 1)
      return Response.json({
        upload_id: "upload-1",
        upload_url:
          "https://configured-host.invalid/api/v1/admin/firmware/uploads/upload-1?token=test",
      });
    return Response.json(
      calls.length === 2 ? { status: "uploaded" } : { firmware_id: "f1" },
    );
  });
  assert.equal((await api.uploadFirmware(file)).firmware_id, "f1");
  assert.deepEqual(JSON.parse(calls[0][1].body), {
    size_bytes: 200,
    sha256: createHash("sha256").update(Buffer.alloc(200, 17)).digest("hex"),
  });
  assert.equal(
    calls[1][0],
    "/api/v1/admin/firmware/uploads/upload-1?token=test",
  );
  assert.equal(calls[1][1].body, file);
  assert.deepEqual(JSON.parse(calls[2][1].body), { upload_id: "upload-1" });
  assert.ok(calls.every(([path]) => path.startsWith("/api/")));
  await assert.rejects(
    api.uploadFirmware(new File(["short"], "short.bin")),
    /168 B/,
  );
  await assert.rejects(
    createConsoleTransport(async () =>
      Response.json({
        upload_id: "u",
        upload_url: "https://external.invalid/collect",
      }),
    ).uploadFirmware(file),
    /地址无效/,
  );
});

test("firmware pagination stops at the final page and rejects repeated cursors", async () => {
  let page = 0;
  const items = await createConsoleTransport(async () =>
    Response.json(
      ++page === 1
        ? {
            items: [{ firmware_id: "old", created_at: 1 }],
            next_cursor: "next",
          }
        : { items: [{ firmware_id: "new", created_at: 2 }], next_cursor: null },
    ),
  ).firmware();
  assert.deepEqual(
    items.map((item) => item.firmware_id),
    ["new", "old"],
  );
  assert.equal(page, 2);
  await assert.rejects(
    createConsoleTransport(async () =>
      Response.json({ items: [], next_cursor: "loop" }),
    ).firmware(),
    /分页异常/,
  );
});

test("upgrade matching requires online healthy hardware, matching project/profile and enough capacity", () => {
  const fw = { profile: "round_screen", project: "round", size_bytes: 9000 };
  assert.equal(matchingFirmware(component(), fw), true);
  for (const patch of [
    { online: false },
    { healthy: false },
    { project: "other" },
    { profile: "slave_mcu" },
    { capacity: 8999 },
  ])
    assert.equal(matchingFirmware(component(patch), fw), false);
  assert.equal(
    matchingFirmware(component({ capacity: 8500 }), {
      ...fw,
      image_size_bytes: 8000,
    }),
    true,
  );
});

test("upgrade retries retain the given request key and cancellation targets the same job", async () => {
  const calls = [],
    api = createConsoleTransport(async (path, options) => {
      calls.push([path, options]);
      return Response.json(job());
    });
  await api.startUpgrade(
    "desk:one",
    "component-1",
    "f1",
    undefined,
    "same-key",
  );
  await api.startUpgrade(
    "desk:one",
    "component-1",
    "f1",
    undefined,
    "same-key",
  );
  await api.cancel("desk:one", "j1");
  const body = JSON.parse(calls[0][1].body);
  assert.deepEqual(body, {
    firmware_id: "f1",
    idempotency_key: "same-key",
    wait_for_component: false,
    allow_downgrade: false,
  });
  assert.equal(calls[0][1].body, calls[1][1].body);
  assert.equal(calls[2][0], "/api/v1/admin/ota/jobs/j1/cancel");
});

test("workspace preferences remain isolated between live devices and the preview with the same ID", () => {
  const storage = new Map();
  globalThis.localStorage = { getItem: (key) => storage.get(key) || null };
  const device = data.devices[0],
    stored = data.createDemoData(device);
  stored.deviceName = "演示中的名字";
  storage.set(
    data.workspaceStorageKey(device.id, "demo"),
    JSON.stringify(stored),
  );
  assert.equal(data.loadDemoData(device).deviceName, "演示中的名字");
  assert.equal(data.loadDemoData(device, "live").deviceName, device.name);
  assert.notEqual(
    data.workspaceStorageKey(device.id, "live"),
    data.workspaceStorageKey(data.devices[1].id, "live"),
  );
});
