import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-device-feishu-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const { createConsoleTransport } =
  await server.ssrLoadModule("/src/shared/api/consoleApi.ts");
const feishu = await server.ssrLoadModule("/src/shared/model/deviceFeishu.ts");
const { DeviceWorkspaceApp } = await server.ssrLoadModule("/src/app/App.tsx");
const fixture = (id = "desk-a", patch = {}) => ({
  device_id: id,
  revision: "r1",
  config: { ...feishu.defaultFeishuConfig(), enabled: true, app_id: "cli_a" },
  credentials_configured: { app_secret: true, verification_token: false },
  account: { authorized: false, display_name: "" },
  connected: false,
  error: "",
  calendar: null,
  ...patch,
});

test("Feishu requests all carry the device identity and never read a global account or existing secret", async () => {
  const calls = [],
    id = "desk:a";
  const api = createConsoleTransport(async (path, options) => {
    calls.push([path, options]);
    if (path.includes("/oauth/url"))
      return Response.json({
        device_id: id,
        authorize_url:
          "https://open.feishu.cn/open-apis/authen/v1/authorize?state=only-a",
      });
    if (path.endsWith("/calendar/sync"))
      return Response.json({ device_id: id, received: 2 });
    return Response.json(fixture(id));
  });
  const snapshot = await api.feishu(id);
  await api.saveFeishu(id, snapshot, snapshot.config, {
    app_secret: " new-test-secret ",
  });
  await api.authorizeFeishu(
    id,
    "https://console.test/api/v1/feishu/oauth/callback",
  );
  await api.syncFeishu(id);
  await api.disconnectFeishu(id);
  assert.ok(
    calls.every(
      ([path, options]) =>
        path.startsWith("/api/v1/admin/devices/desk%3Aa/") &&
        options.credentials === "same-origin" &&
        options.cache === "no-store",
    ),
  );
  assert.ok(
    calls.every(
      ([path]) => !path.includes("/secrets") && !path.includes("/healthz"),
    ),
  );
  assert.deepEqual(JSON.parse(calls[1][1].body).secrets, {
    app_secret: "new-test-secret",
  });
  assert.equal(calls[1][1].method, "PUT");
  assert.equal(calls[4][1].method, "POST");
});

test("another device's config, OAuth URL, disconnect and sync results are refused", async () => {
  const api = createConsoleTransport(async () =>
    Response.json({
      ...fixture("other"),
      authorize_url: "https://open.feishu.cn/open-apis/authen/v1/authorize",
    }),
  );
  await assert.rejects(api.feishu("desk-a"), /当前设备/);
  await assert.rejects(
    api.authorizeFeishu("desk-a", "https://console.test/callback"),
    /不一致/,
  );
  await assert.rejects(api.disconnectFeishu("desk-a"), /当前设备/);
  await assert.rejects(api.syncFeishu("desk-a"), /不一致/);
  let calls = 0;
  const save = createConsoleTransport(async () => {
    calls++;
    return Response.json(fixture());
  });
  await assert.rejects(
    save.saveFeishu("desk-a", fixture("other"), fixture().config, {}),
    /不一致/,
  );
  assert.equal(calls, 0);
});

test("authorization accepts only the actual Feishu authorize endpoint", async () => {
  for (const url of [
    "https://external.test/collect",
    "http://open.feishu.cn/open-apis/authen/v1/authorize",
    "https://open.feishu.cn/other",
    "https://secret@open.feishu.cn/open-apis/authen/v1/authorize",
  ])
    await assert.rejects(
      createConsoleTransport(async () =>
        Response.json({ device_id: "desk-a", authorize_url: url }),
      ).authorizeFeishu("desk-a", "https://console.test/callback"),
      /地址无效/,
    );
});

test("public drafts never store credentials and are separate across devices, modes and the former global draft", () => {
  const storage = new Map();
  globalThis.localStorage = { getItem: (key) => storage.get(key) || null };
  storage.set(
    "binbot-public-services-v1:live",
    JSON.stringify({ feishu: { appId: "global-app" } }),
  );
  storage.set(
    feishu.feishuDraftKey("demo", "a"),
    JSON.stringify({
      app_id: "demo-a",
      app_secret: "private-test",
      access_token: "private-access",
      enabled: true,
    }),
  );
  assert.equal(feishu.readFeishuDraft("demo", "a").app_id, "demo-a");
  assert.equal(feishu.readFeishuDraft("live", "a").app_id, "");
  assert.equal(feishu.readFeishuDraft("demo", "b").app_id, "");
  assert.doesNotMatch(
    JSON.stringify(feishu.readFeishuDraft("demo", "a")),
    /private|secret|token/,
  );
  assert.deepEqual(
    feishu.feishuSaveBody(fixture(), fixture().config, {
      app_secret: "",
      verification_token: "",
    }).secrets,
    {},
  );
  assert.throws(
    () =>
      feishu.parseFeishuSnapshot({ device_id: "desk-a", config: {} }, "desk-a"),
    /返回异常/,
  );
});

test("known device-specific public drafts migrate only to that same device", () => {
  const source = JSON.stringify({
    services: {
      feishu: {
        appId: "cli_legacy",
        calendarId: "legacy-calendar",
        app_secret: "must-drop",
      },
    },
  });
  globalThis.localStorage = {
    getItem: (key) => (key === "binbot-ui-v1:demo-a" ? source : null),
  };
  assert.equal(feishu.readFeishuDraft("demo", "demo-a").app_id, "cli_legacy");
  assert.equal(feishu.readFeishuDraft("demo", "demo-b").app_id, "");
  assert.doesNotMatch(
    JSON.stringify(feishu.readFeishuDraft("demo", "demo-a")),
    /must-drop/,
  );
});

test("Feishu deep links stay in the current device's settings and expose no global Feishu editor", () => {
  globalThis.window = {
    location: {
      pathname: "/devices/binbot-001/settings",
      hash: "#feishu",
      origin: "http://console.test",
    },
  };
  globalThis.localStorage = { getItem: () => null };
  const html = renderToStaticMarkup(React.createElement(DeviceWorkspaceApp));
  assert.match(html, /device-feishu-page/);
  assert.match(html, /\/devices\/binbot-001\/settings#feishu/);
  assert.match(html, /应用、账号和日历只用于当前设备/);
  assert.doesNotMatch(html, /href="\/services#feishu"/);
});
