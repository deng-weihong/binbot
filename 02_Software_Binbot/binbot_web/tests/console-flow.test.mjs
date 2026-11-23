import { after, afterEach, beforeEach, test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";
import "fake-indexeddb/auto";
import { createServer } from "vite";

// Component interaction tests in a simulated DOM, without a browser engine.
const dom = new JSDOM("<!doctype html><div id='root'></div>", {
  url: "http://console.test/login",
  pretendToBeVisual: true,
});
for (const key of [
  "window",
  "document",
  "localStorage",
  "sessionStorage",
  "HTMLElement",
  "HTMLInputElement",
  "HTMLDialogElement",
  "Event",
  "MouseEvent",
  "PopStateEvent",
  "StorageEvent",
  "Node",
])
  globalThis[key] = dom.window[key];
Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  value: dom.window.navigator,
});
globalThis.IS_REACT_ACT_ENVIRONMENT = true;
HTMLDialogElement.prototype.showModal = function () {
  this.open = true;
};
let requests = [],
  handle;
let workspaces = new Map(), galleryRows = new Map();
globalThis.fetch = async (path, options = {}) => {
  requests.push([path, options]);
  if (path.includes("/workspace")) path = path.split("?")[0];
  const workspaceMatch = path.match(/^\/api\/v1\/admin\/devices\/([^/]+)\/workspace$/);
  if (workspaceMatch) {
    const id = decodeURIComponent(workspaceMatch[1]);
    let value = workspaces.get(id);
    if (!value) { const device = { id, name: id, location: '', online: true, color: 'lilac' }; const data = emptyLiveData(device); value = { device_id: id, revision: 'r1', data, reported: { ...data.features, todos: [], microphone: true, notifications: true }, control: { ready: true, brightness: 50, volume: 40 }, capabilities: ['workspace_v1'], calendar: [], system: {} }; workspaces.set(id, value); }
    if (options.method === 'PUT') { const body = JSON.parse(options.body); value = { ...value, revision: value.revision + 'x', data: { ...value.data, ...body.data } }; workspaces.set(id, value); }
    return Response.json(value);
  }
  const galleryMatch = path.match(/^\/api\/v1\/admin\/devices\/([^/]+)\/gallery(?:\/([^/?]+))?(?:\?.*)?$/);
  if (galleryMatch) {
    const id = decodeURIComponent(galleryMatch[1]), key = galleryMatch[2];
    if (options.method === 'PUT') { const row = { id: key, assetId: 'asset-1', name: key, createdAt: Date.now() }; galleryRows.set(id, [row]); return Response.json(row); }
    return Response.json({ device_id: id, images: galleryRows.get(id) || [] });
  }
  return handle(path, options);
};
const React = await import("react"),
  { act } = React;
const { createRoot } = await import("react-dom/client");
const server = await createServer({
  cacheDir: "node_modules/.vite-console-flow-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
const { default: Portal } = await server.ssrLoadModule(
  "/src/app/ConsolePortal.tsx",
);
const { DeviceWorkspaceApp } = await server.ssrLoadModule("/src/app/App.tsx");
const { useConsole } = await server.ssrLoadModule("/src/shared/state/consoleContext.tsx");
const { navigate } = await server.ssrLoadModule("/src/shared/navigation.ts");
const { emptyLiveData } = await server.ssrLoadModule("/src/shared/model/workspaceModel.ts");
const { AUTH_EXPIRED } = await server.ssrLoadModule("/src/shared/api/consoleApi.ts");
const { WorkspaceContext, useDeviceWorkspace } = await server.ssrLoadModule("/src/shared/state/workspaceContext.tsx");
const { default: HomeAgendaCard } = await server.ssrLoadModule("/src/features/home/HomeAgendaCard.tsx");
const { defaultFeishuConfig } = await server.ssrLoadModule(
  "/src/shared/model/deviceFeishu.ts",
);
const deviceRows = [
  {
    id: "desk-one",
    online: true,
    capabilities: ["component_ota_v1", "remote_control_v1"],
  },
  { id: "desk-two", online: false },
];
let root;
const flush = async () => {
  for (let n = 0; n < 6; n++) await new Promise((done) => setImmediate(done));
};
const deferred = () => {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
};
function Probe() {
  const { devices } = useConsole();
  return React.createElement(
    "div",
    { "data-protected": true },
    `设备工作台 ${window.location.pathname} ${devices.map((item) => item.id).join(",")}`,
    React.createElement(
      "a",
      {
        href: "/devices",
        onClick: (event) => {
          event.preventDefault();
          navigate("/devices");
        },
      },
      "全部设备",
    ),
  );
}
async function mount(path, child = React.createElement(Probe)) {
  window.history.replaceState({}, "", path);
  root = createRoot(document.getElementById("root"));
  await act(async () => {
    root.render(React.createElement(Portal, null, child));
    await flush();
  });
}
async function click(element) {
  assert.ok(element, "expected an interactive element");
  await act(async () => {
    element.click();
    await flush();
  });
}
async function input(element, value) {
  assert.ok(element);
  await act(async () => {
    Object.getOwnPropertyDescriptor(
      HTMLInputElement.prototype,
      "value",
    ).set.call(element, value);
    element.dispatchEvent(new Event("input", { bubbles: true }));
    await flush();
  });
}
const button = (text) =>
  [...document.querySelectorAll("button")].find(
    (item) => item.textContent.trim() === text,
  );
const authorized = (path) => {
  if (path === "/api/v1/admin/session")
    return Response.json({ authenticated: true });
  if (path === "/api/v1/admin/devices")
    return Response.json({ devices: deviceRows });
  if (path === "/api/v1/admin/logout") return Response.json({ status: "ok" });
  throw new Error(`unexpected API request: ${path}`);
};
beforeEach(() => {
  requests = [];
  workspaces = new Map(); galleryRows = new Map();
  localStorage.clear();
  sessionStorage.clear();
  handle = async () => new Response("unauthorized", { status: 401 });
});
afterEach(async () => {
  if (root)
    await act(async () => {
      root.unmount();
      await flush();
    });
  root = undefined;
});
after(async () => {
  await server.close();
  dom.window.close();
});

test("an unauthenticated deep link never renders the device workspace and keeps its fragment for login", async () => {
  await mount("/devices/desk-one/modules#gallery");
  assert.equal(document.querySelector("[data-protected]"), null);
  assert.ok(document.querySelector(".login-page"));
  assert.equal(
    new URLSearchParams(window.location.search).get("returnTo"),
    "/devices/desk-one/modules#gallery",
  );
  assert.deepEqual(
    requests.map(([path]) => path),
    ["/api/v1/admin/session"],
  );
});

test("device-list loopback access cannot stand in for a real browser login session", async () => {
  handle = async (path) =>
    path === "/api/v1/admin/devices"
      ? Response.json({ devices: deviceRows })
      : new Response("no cookie", { status: 401 });
  await mount("/devices");
  assert.ok(document.querySelector(".login-page"));
  assert.equal(document.querySelector(".device-manager"), null);
  assert.equal(requests.length, 1);
});

test("login validates credentials, preserves the original page, remembers only the requested choice and stores no password", async () => {
  let authenticated = false,
    loginBody;
  handle = async (path, options) => {
    if (path === "/api/v1/admin/session" && options.method === "POST") {
      loginBody = JSON.parse(options.body);
      if (loginBody.password === "bad")
        return new Response("wrong", { status: 401 });
      authenticated = true;
      return Response.json({ status: "ok" });
    }
    if (path === "/api/v1/admin/session")
      return authenticated
        ? Response.json({ authenticated: true })
        : new Response("no cookie", { status: 401 });
    return authorized(path);
  };
  await mount("/devices/desk-one/modules#gallery");
  await input(document.querySelector('[name="username"]'), "operator");
  await input(document.querySelector('[name="password"]'), "bad");
  await click(button("登录"));
  assert.match(
    document.querySelector('[role="alert"]').textContent,
    /账号或密码/,
  );
  assert.equal(document.querySelector("[data-protected]"), null);
  await input(
    document.querySelector('[name="password"]'),
    "private-test-password",
  );
  await click(document.querySelector('[type="checkbox"]'));
  await click(button("登录"));
  assert.deepEqual(loginBody, {
    username: "operator",
    password: "private-test-password",
    remember: true,
  });
  assert.equal(
    window.location.pathname + window.location.hash,
    "/devices/desk-one/modules#gallery",
  );
  assert.ok(document.querySelector("[data-protected]"));
  assert.equal(document.querySelector('[name="password"]'), null);
  assert.doesNotMatch(
    JSON.stringify({ ...localStorage, ...sessionStorage }),
    /private-test-password|operator/,
  );
});

test("authenticated login visits sanitize external return links and render outer management without a workspace sidebar", async () => {
  handle = authorized;
  await mount("/login?returnTo=https%3A%2F%2Fexternal.test");
  assert.equal(window.location.pathname, "/devices");
  assert.ok(document.querySelector(".device-manager"));
  assert.equal(document.querySelector(".sidebar"), null);
  assert.equal(document.querySelectorAll(".manager-device-card").length, 2);
  assert.equal(document.querySelector("[data-protected]"), null);
});

test("device search, label editing, entering one workspace and returning to all devices work together", async () => {
  handle = authorized;
  await mount("/devices");
  await input(document.querySelector('[aria-label="搜索设备"]'), "desk-two");
  assert.equal(document.querySelectorAll(".manager-device-card").length, 1);
  await input(document.querySelector('[aria-label="搜索设备"]'), "");
  await click(
    document.querySelector('[aria-label="编辑desk-one的名称与位置"]'),
  );
  const dialog = document.querySelector("dialog");
  const fields = dialog.querySelectorAll("input");
  await input(fields[0], "书桌伙伴");
  await input(fields[1], "书房");
  await act(async () => {
    dialog
      .querySelector("form")
      .dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }));
    await flush();
  });
  assert.match(
    document.querySelector(".manager-device-title").textContent,
    /书桌伙伴.*书房/,
  );
  assert.match(
    JSON.stringify(workspaces.get("desk-one").data),
    /书桌伙伴/,
  );
  await click(document.querySelector('[aria-label="进入书桌伙伴"]'));
  assert.equal(window.location.pathname, "/devices/desk-one/today");
  assert.ok(document.querySelector("[data-protected]"));
  await click(document.querySelector("[data-protected] a"));
  assert.equal(window.location.pathname, "/devices");
  assert.equal(document.querySelectorAll(".manager-device-card").length, 2);
});

test("a failed logout keeps the authenticated page and a retry clears it only after server confirmation", async () => {
  let attempts = 0;
  handle = async (path) =>
    path.endsWith("logout") && ++attempts === 1
      ? Response.json({}, { status: 500 })
      : authorized(path);
  await mount("/devices");
  await click(button("退出登录"));
  assert.ok(document.querySelector(".device-manager"));
  assert.match(document.body.textContent, /操作未能完成/);
  await click(button("退出登录"));
  assert.equal(window.location.pathname, "/login");
  assert.ok(document.querySelector(".login-page"));
  assert.equal(document.querySelector(".device-manager"), null);
});

test("session expiry removes protected content and repeated unauthorized events retain the original page and fragment", async () => {
  handle = authorized;
  await mount("/devices/desk-one/settings#information");
  for (let n = 0; n < 2; n++)
    await act(async () => {
      window.dispatchEvent(new Event(AUTH_EXPIRED));
      await flush();
    });
  assert.equal(document.querySelector("[data-protected]"), null);
  assert.equal(
    new URLSearchParams(window.location.search).get("returnTo"),
    "/devices/desk-one/settings#information",
  );
  assert.match(document.body.textContent, /登录已失效/);
});

test("a late device-list refresh cannot reopen a logged-out session", async () => {
  const pending = deferred();
  let reads = 0;
  handle = async (path) =>
    path.endsWith("devices") && ++reads > 1
      ? pending.promise
      : authorized(path);
  await mount("/devices");
  await click(document.querySelector('[aria-label="刷新设备列表"]'));
  assert.equal(reads, 2);
  await click(button("退出登录"));
  await act(async () => {
    pending.resolve(
      Response.json({ devices: [{ id: "stale-device", online: true }] }),
    );
    await flush();
  });
  assert.ok(document.querySelector(".login-page"));
  assert.doesNotMatch(document.body.textContent, /stale-device/);
  assert.equal(window.location.pathname, "/login");
});

test("logout in another tab expires a live session", async () => {
  handle = authorized;
  await mount("/devices/desk-one/control");
  await act(async () => {
    window.dispatchEvent(
      new StorageEvent("storage", {
        key: "binbot-console-session-change",
        newValue: "1",
      }),
    );
    await flush();
  });
  assert.ok(document.querySelector(".login-page"));
});

test("explicit preview stays separate, browses real workspace pages, and sends no real device or service calls", async () => {
  await mount("/login", React.createElement(DeviceWorkspaceApp));
  await click(button("进入界面预览"));
  const count = requests.length;
  assert.equal(document.querySelectorAll(".manager-device-card").length, 3);
  assert.match(document.body.textContent, /界面预览/);
  await act(async () => {
    navigate("/devices/binbot-001/modules#gallery");
    await flush();
  });
  assert.ok(document.querySelector(".gallery-page"));
  assert.equal(button("加入设备图库").disabled, true);
  assert.equal(button("推送到当前屏幕").disabled, true);
  await act(async () => {
    navigate("/devices/binbot-001/modules");
    await flush();
  });
  assert.equal(document.querySelector(".gallery-page"), null);
  await act(async () => {
    navigate("/devices/binbot-001/modules#gallery");
    await flush();
  });
  assert.ok(document.querySelector(".gallery-page"));
  await act(async () => {
    navigate("/devices/binbot-001/settings#information");
    await flush();
  });
  assert.ok(document.querySelector(".maintenance-page"));
  assert.equal(button("刷新版本").disabled, true);
  assert.match(document.body.textContent, /尚未上报/);
  await act(async () => {
    navigate("/devices/binbot-001/settings#general");
    await flush();
  });
  assert.equal(document.querySelector(".maintenance-page"), null);
  await act(async () => {
    navigate("/services#voice");
    await flush();
  });
  assert.equal(document.querySelector(".sidebar"), null);
  assert.equal(
    document.querySelector('[aria-current="page"]').textContent,
    "语音",
  );
  await act(async () => {
    navigate("/services#feishu");
    await flush();
  });
  assert.equal(
    document.querySelector('[aria-current="page"]').textContent,
    "语音",
  );
  assert.match(document.body.textContent, /飞书连接已移到每台设备/);
  await act(async () => {
    navigate("/devices/binbot-001/settings#feishu");
    await flush();
  });
  assert.ok(document.querySelector(".device-feishu-page"));
  assert.equal(button("连接飞书").disabled, true);
  assert.equal(requests.length, count);
  await click(document.querySelector('[aria-label="退出预览"]'));
  assert.equal(sessionStorage.getItem("binbot-console-preview"), null);
  assert.ok(document.querySelector(".login-page"));
});

test("live study reveals the answer, uses the server batch, and submits a separate retest", async () => {
  const id = "desk-one";
  const rootPath = `/api/v1/admin/devices/${id}/workspace`;
  const device = { id, name: id, online: true, color: "lilac" };
  const data = emptyLiveData(device);
  const book = data.wordbooks.books[0];
  data.wordbooks.activeBookId = book.id;
  const [first, other] = book.words;
  const snapshot = { device_id: id, revision: "r1", data, control: { ready: true } };
  workspaces.set(id, snapshot);
  const answers = [];
  handle = async (path, options) => {
    if (path === `${rootPath}/learning`)
      return Response.json({ book_id: book.id, words: [other.word, first.word] });
    if (path === `${rootPath}/reviews`) {
      const answer = JSON.parse(options.body);
      answers.push([answer.word, answer.grade, answer.retry]);
      return Response.json(snapshot);
    }
    return authorized(path);
  };
  await mount(`/devices/${id}/vocabulary#wordbooks`, React.createElement(DeviceWorkspaceApp));
  await click(button("开始复习"));
  assert.ok(requests.some(([path]) => path === `${rootPath}/learning?book_id=${book.id}`));
  const dialog = document.querySelector("dialog");
  assert.ok(dialog);
  assert.equal(dialog.querySelector("h2[lang='en']").textContent, other.word);
  assert.ok(!dialog.textContent.includes(other.meaning));
  await click(button("想一想，再看释义"));
  assert.ok(dialog.textContent.includes(other.meaning));
  await click(button("模糊"));
  assert.equal(dialog.querySelector("h2[lang='en']").textContent, first.word);
  await click(button("想一想，再看释义"));
  await click(button("认识"));
  assert.equal(dialog.querySelector("h2[lang='en']").textContent, other.word);
  assert.match(dialog.textContent, /本轮重测/);
  await click(button("想一想，再看释义"));
  await click(button("忘记"));
  assert.match(dialog.textContent, /本批完成/);
  assert.deepEqual(answers, [
    [other.word, "fuzzy", false],
    [first.word, "known", false],
    [other.word, "forgot", true],
  ]);
});

test("reported module images use the same thumbnail in the picker and preview", async () => {
  handle = async (path) => authorized(path);
  const id = "desk-one";
  const first = "12345678-1234-4abc-8def-123456789012";
  const second = "12345678-1234-4abc-8def-123456789013";
  const thumbnail = (image) => `/v1/device/module-images/${image}/thumbnail.png`;
  workspaces.set(id, {
    device_id: id,
    revision: "r1",
    data: { wordbooks: {} },
    reported: {
      modules: {
        connected: "pen",
        pen: { mode: "image", imageId: first },
        images: [
          { id: first, name: `${first}.gif` },
          { id: second, name: "自定义图片.gif" },
        ],
      },
    },
  });
  await mount(`/devices/${id}/modules`, React.createElement(DeviceWorkspaceApp));
  const [current, other] = document.querySelectorAll(
    ".module-image-options button",
  );
  const preview = () => document.querySelector(".module-preview-card img");
  assert.equal(current.textContent, "图片 1");
  assert.equal(current.title, `${first}.gif`);
  assert.equal(
    current.querySelector("img").getAttribute("src"),
    thumbnail(first),
  );
  assert.equal(preview().getAttribute("src"), thumbnail(first));
  await click(other);
  assert.equal(other.getAttribute("aria-pressed"), "true");
  assert.equal(
    other.querySelector("img").getAttribute("src"),
    thumbnail(second),
  );
  assert.equal(preview().getAttribute("src"), thumbnail(second));
  await act(async () => {
    other.querySelector("img").dispatchEvent(new Event("error"));
  });
  assert.ok(other.querySelector(".module-image-unavailable"));
});

test("a server gallery delivery waits for the current device receipt before showing success", async () => {
  const receipt = deferred();
  handle = async (path, options) => {
    if (path.endsWith('/components/component-1?fresh=false')) return Response.json({ device_id: 'desk-one', component_id: 'component-1', profile: 'round_screen', online: true, healthy: true });
    if (path.startsWith('/module-media/')) return new Response(new Uint8Array([137,80,78,71,13,10,26,10]), { headers: { 'Content-Type': 'image/png' } });
    if (path.endsWith('/devices/desk-one/commands')) { const body=JSON.parse(options.body); assert.equal(body.name, 'modules.image'); assert.equal(body.payload.asset_id, 'asset-1'); return Response.json({ device_id: 'desk-one', command_id: 'send-one', name: 'modules.image', status: 'delivered' }); }
    if (path.endsWith('/commands/send-one')) return receipt.promise;
    return authorized(path);
  };
  await mount('/devices/desk-one/modules#gallery', React.createElement(DeviceWorkspaceApp));
  await click(button('加入设备图库'));
  assert.match(document.body.textContent, /设备正在接收图片/);
  assert.doesNotMatch(document.body.textContent, /已加入设备图库/);
  assert.equal(button('加入设备图库').disabled, true);
  await act(async () => { receipt.resolve(Response.json({ device_id: 'desk-one', command_id: 'send-one', status: 'completed' })); await flush(); });
  assert.match(document.body.textContent, /已加入设备图库/);
  assert.equal(button('加入设备图库').disabled, false);
});
function deviceFeishuSnapshot(id, name = "") {
  return {
    device_id: id,
    revision: "revision-one",
    config: {
      ...defaultFeishuConfig(),
      enabled: true,
      app_id: `cli_${id}`,
      calendar_enabled: true,
    },
    credentials_configured: { app_secret: true, verification_token: false },
    account: { authorized: !!name, display_name: name },
    connected: false,
    error: "",
    calendar: null,
  };
}

test("editing live Feishu settings saves only to the current device and never persists its secret in browser storage", async () => {
  let saved;
  handle = async (path, options) => {
    if (path === "/api/v1/admin/devices/desk-one/feishu") {
      if (options.method === "PUT") {
        saved = JSON.parse(options.body);
        return Response.json({
          ...deviceFeishuSnapshot("desk-one"),
          config: saved.config,
          revision: "revision-two",
        });
      }
      return Response.json(deviceFeishuSnapshot("desk-one"));
    }
    return authorized(path);
  };
  await mount(
    "/devices/desk-one/settings#feishu",
    React.createElement(DeviceWorkspaceApp),
  );
  assert.equal(
    document.querySelector('[aria-label="当前设备 App ID"]').value,
    "cli_desk-one",
  );
  await input(
    document.querySelector('[aria-label="当前设备 App Secret"]'),
    "per-device-private-secret",
  );
  await click(button("保存设备配置"));
  assert.deepEqual(saved.secrets, { app_secret: "per-device-private-secret" });
  assert.equal(
    document.querySelector('[aria-label="当前设备 App Secret"]').value,
    "",
  );
  assert.doesNotMatch(
    JSON.stringify({ ...localStorage, ...sessionStorage }),
    /per-device-private-secret/,
  );
  assert.equal(
    requests.filter(([, options]) => options.method === "PUT").length,
    1,
  );
  assert.match(document.body.textContent, /已保存到当前设备/);
});

test("switching devices during a pending Feishu read cannot attach another device's account or config", async () => {
  const old = deferred();
  handle = async (path) => {
    if (path === "/api/v1/admin/devices/desk-one/feishu") return old.promise;
    if (path === "/api/v1/admin/devices/desk-two/feishu")
      return Response.json(deviceFeishuSnapshot("desk-two", "Bob"));
    return authorized(path);
  };
  await mount(
    "/devices/desk-one/settings#feishu",
    React.createElement(DeviceWorkspaceApp),
  );
  await act(async () => {
    navigate("/devices/desk-two/settings#feishu");
    await flush();
  });
  assert.match(document.body.textContent, /Bob/);
  await act(async () => {
    old.resolve(Response.json(deviceFeishuSnapshot("desk-one", "Alice")));
    await flush();
  });
  assert.equal(
    document.querySelector('[aria-label="当前设备 App ID"]').value,
    "cli_desk-two",
  );
  assert.doesNotMatch(
    document.querySelector(".device-feishu-page").textContent,
    /Alice/,
  );
  assert.ok(
    requests.find(([path]) => path.endsWith("/desk-one/feishu"))[1].signal
      .aborted,
  );
});

test("disconnect confirms the current device and does not call another device or a global Feishu endpoint", async () => {
  let disconnected = false;
  handle = async (path) => {
    if (path === "/api/v1/admin/devices/desk-one/feishu/oauth/disconnect") {
      disconnected = true;
      return Response.json(deviceFeishuSnapshot("desk-one"));
    }
    if (path === "/api/v1/admin/devices/desk-one/feishu")
      return Response.json(deviceFeishuSnapshot("desk-one", "Alice"));
    if (path === "/api/v1/admin/devices/desk-two/feishu")
      return Response.json(deviceFeishuSnapshot("desk-two", "Bob"));
    return authorized(path);
  };
  await mount(
    "/devices/desk-one/settings#feishu",
    React.createElement(DeviceWorkspaceApp),
  );
  await click(button("断开当前设备的账号"));
  assert.equal(disconnected, false);
  await click(button("确认断开"));
  assert.equal(disconnected, true);
  assert.equal(button("断开当前设备的账号"), undefined);
  await act(async () => {
    navigate("/devices/desk-two/settings#feishu");
    await flush();
  });
  assert.match(document.body.textContent, /Bob/);
  assert.deepEqual(
    requests
      .filter(([path]) => path.includes("/disconnect"))
      .map(([path]) => path),
    ["/api/v1/admin/devices/desk-one/feishu/oauth/disconnect"],
  );
});

function AgendaHarness({ messages }) {
  const device = { id: "desk-one", name: "书桌", location: "", online: true, color: "lilac" };
  const workspace = useDeviceWorkspace(device, "live", (message) => messages.push(message));
  return React.createElement(WorkspaceContext.Provider, { value: workspace.value },
    React.createElement(HomeAgendaCard, { data: workspace.data, setData: workspace.setData, notify: workspace.notify }));
}

const agendaSnapshot = () => ({
  device_id: "desk-one", revision: "agenda-v1", data: emptyLiveData({ id: "desk-one", name: "书桌", location: "", online: true, color: "lilac" }),
  reported: {}, control: { ready: true }, calendar: [], system: {}, capabilities: ["workspace_v1"],
  calendar_sync: { enabled: true, authorized: true, polling: true, poll_interval_seconds: 60 },
});

test("home agenda waits for Feishu confirmation, retains a failed form and retries with the same schedule identity", async () => {
  const snapshot = agendaSnapshot(), messages = [], sent = [];
  workspaces.set("desk-one", snapshot);
  handle = async (path, options) => {
    if (path.endsWith("/workspace/schedules")) {
      const body = JSON.parse(options.body); sent.push(body);
      if (sent.length === 1) return Response.json({ code: "CALENDAR_SYNC_ERROR", error: "飞书未确认创建日程" }, { status: 409 });
      const saved = { ...snapshot, revision: "agenda-v2", calendar: [{ ...body.schedule, id: "feishu:event-1", source: "feishu" }] };
      workspaces.set("desk-one", saved); return Response.json(saved);
    }
    return authorized(path);
  };
  await mount("/devices/desk-one/today", React.createElement(AgendaHarness, { messages }));
  await click(document.querySelector('[aria-label="添加日程"]'));
  await input(document.querySelector('input[placeholder^="比如，读几页"]'), "下午讨论");
  await click(button("保存日程"));
  assert.match(document.querySelector('dialog [role="alert"]').textContent, /飞书未确认/);
  assert.equal(messages.length, 0);
  assert.equal(document.querySelector('input[placeholder^="比如，读几页"]').value, "下午讨论");
  await click(button("保存日程"));
  assert.deepEqual(sent[1], sent[0]);
  assert.equal(document.querySelector("dialog"), null);
  assert.match(document.querySelector(".agenda-list").textContent, /下午讨论.*飞书日历/s);
  assert.match(messages[0], /已同步飞书/);
  assert.equal(requests.some(([path, options]) => path.split("?")[0].endsWith("/workspace") && options.method === "PUT"), false);
});

test("existing device-only schedules offer an explicit Feishu sync and turn into one Feishu entry", async () => {
  const snapshot = agendaSnapshot(), messages = [];
  const date = new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Shanghai", year: "numeric", month: "2-digit", day: "2-digit" }).format(new Date());
  const schedule = { id: "old-local-id", name: "原有安排", date, time: "19:00", note: "保留备注", enabled: true, done: false };
  snapshot.data.schedules = [schedule]; snapshot.calendar = [{ ...schedule, source: "local" }];
  workspaces.set("desk-one", snapshot);
  handle = async (path, options) => {
    if (path.endsWith("/workspace/schedules")) {
      assert.deepEqual(JSON.parse(options.body).schedule, schedule);
      const saved = { ...snapshot, revision: "synced", data: { ...snapshot.data, schedules: [] }, calendar: [{ ...schedule, id: "feishu:original", source: "feishu" }] };
      workspaces.set("desk-one", saved); return Response.json(saved);
    }
    return authorized(path);
  };
  await mount("/devices/desk-one/today", React.createElement(AgendaHarness, { messages }));
  await click(document.querySelector('[aria-label="同步到飞书：原有安排"]'));
  assert.equal(document.querySelectorAll(".agenda-item").length, 1);
  assert.match(document.querySelector(".agenda-item").textContent, /飞书日历/);
  assert.equal(document.querySelector('[aria-label="同步到飞书：原有安排"]'), null);
});
