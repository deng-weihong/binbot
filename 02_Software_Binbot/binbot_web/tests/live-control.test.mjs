import { after, afterEach, test } from "node:test";
import assert from "node:assert/strict";
import { JSDOM } from "jsdom";
import { createServer } from "vite";

const dom = new JSDOM("<!doctype html><div id='root'></div>", {
  url: "http://console.test/",
  pretendToBeVisual: true,
});
for (const key of [
  "window",
  "document",
  "HTMLElement",
  "HTMLInputElement",
  "Event",
  "Node",
])
  globalThis[key] = dom.window[key];
Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  value: dom.window.navigator,
});
globalThis.IS_REACT_ACT_ENVIRONMENT = true;
const React = await import("react"),
  { act } = React;
const { createRoot } = await import("react-dom/client");
const server = await createServer({
  cacheDir: "node_modules/.vite-live-control-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
const { parseDeviceControl } = await server.ssrLoadModule(
  "/src/features/control/deviceControl.ts",
);
const { createConsoleTransport } =
  await server.ssrLoadModule("/src/shared/api/consoleApi.ts");
const { default: LiveControl } = await server.ssrLoadModule(
  "/src/features/control/LiveControlPage.tsx",
);
const snapshot = (id = "a", brightness = 70, extra = {}) => ({
  device_id: id,
  online: true,
  control_online: true,
  control_ready: true,
  voice_online: false,
  state: {
    control: {
      schema_version: 2,
      boot_id: "1".repeat(32),
      revision: 1,
      ready: true,
      brightness,
      volume: 40,
      capabilities: ["control_v2", "brightness_v1", "volume_v1"],
      updated_at: new Date().toISOString(),
    },
  },
  ...extra,
});
const command = (status, message = "", id = "a") => ({
  device_id: id,
  command_id: "11111111-1111-4111-8111-111111111111",
  name: "screen.brightness.set",
  status,
  message,
});
let root;
const flush = async () => {
  for (let i = 0; i < 5; i++) await new Promise(setImmediate);
};
async function mount(id = "a", notify = () => {}) {
  if (!root) root = createRoot(document.getElementById("root"));
  await act(async () => {
    root.render(
      React.createElement(LiveControl, { key: id, device: { id }, notify }),
    );
    await flush();
  });
}
async function applyBrightness(value) {
  await act(async () => {
    const input = document.querySelector("input[type=range]");
    Object.getOwnPropertyDescriptor(
      HTMLInputElement.prototype,
      "value",
    ).set.call(input, String(value));
    input.dispatchEvent(new Event("input", { bubbles: true }));
    await flush();
  });
  await act(async () => {
    document
      .querySelector("form")
      .dispatchEvent(new Event("submit", { bubbles: true, cancelable: true }));
    await flush();
  });
}
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

test("control state enforces device ownership and does not invent offline values", () => {
  assert.equal(
    parseDeviceControl({ device_id: "a", online: false, state: null }, "a")
      .control,
    null,
  );
  assert.throws(() => parseDeviceControl(snapshot("b"), "a"), /当前设备/);
  for (const brightness of [-1, 0, 101, true, "40"])
    assert.throws(
      () => parseDeviceControl(snapshot("a", brightness), "a"),
      /数值/,
    );
  assert.equal(
    parseDeviceControl(snapshot("a", null), "a").control.brightness,
    null,
  );
});

test("control requests use device-scoped paths and stable idempotency keys", async () => {
  let options;
  const api = createConsoleTransport(async (path, value) => {
    assert.equal(path, "/api/v1/admin/devices/desk%3Aone/commands");
    options = value;
    return Response.json(command("delivered", "", "desk:one"));
  });
  await api.sendControl(
    "desk:one",
    "screen.brightness.set",
    40,
    "request-once",
  );
  assert.equal(options.headers["Idempotency-Key"], "request-once");
  assert.deepEqual(JSON.parse(options.body), {
    name: "screen.brightness.set",
    payload: { brightness: 40 },
  });
  const wrong = createConsoleTransport(async () =>
    Response.json(command("completed", "", "b")),
  );
  await assert.rejects(
    wrong.sendControl("a", "screen.brightness.set", 40, "key"),
    /当前设备/,
  );
});

test("offline and stale controls cannot submit settings", async () => {
  let writes = 0;
  globalThis.fetch = async (_path, options) => {
    if (options.method === "POST") writes++;
    return Response.json(
      snapshot("a", 70, { control_online: false, control_ready: false }),
    );
  };
  await mount();
  assert.ok(document.querySelector("fieldset").disabled);
  assert.equal(writes, 0);
});

test("a delivered command stays pending until the real receipt, including visible failures", async () => {
  const notices = [],
    calls = [];
  let resolveReceipt;
  const receipt = new Promise((resolve) => {
    resolveReceipt = resolve;
  });
  globalThis.fetch = async (path, options) => {
    calls.push([path, options]);
    if (path.endsWith("/state")) return Response.json(snapshot());
    if (path.endsWith("/commands")) return Response.json(command("delivered"));
    return receipt;
  };
  await mount("a", (text) => notices.push(text));
  await applyBrightness(40);
  assert.match(document.body.textContent, /等待设备确认/);
  assert.equal(notices.length, 0);
  await act(async () => {
    await new Promise((resolve) => setTimeout(resolve, 450));
  });
  assert.ok(calls.some(([path]) => path.includes("/admin/commands/")));
  await act(async () => {
    resolveReceipt(Response.json(command("failed", "背光写入失败")));
    await flush();
  });
  assert.match(
    document.querySelector("[role=alert]").textContent,
    /背光写入失败/,
  );
  assert.equal(notices.length, 0);
  assert.match(document.body.textContent, /已上报 70%/);
});

test("switching devices aborts a pending operation and ignores its late success", async () => {
  const notices = [];
  let resolveCommand, signal;
  globalThis.fetch = async (path, options) => {
    if (path.endsWith("/state"))
      return Response.json(
        snapshot(
          path.includes("/b/") ? "b" : "a",
          path.includes("/b/") ? 80 : 70,
        ),
      );
    signal = options.signal;
    return new Promise((resolve) => {
      resolveCommand = resolve;
    });
  };
  await mount("a", (text) => notices.push(text));
  await applyBrightness(40);
  assert.ok(resolveCommand);
  await mount("b", (text) => notices.push(text));
  assert.ok(signal.aborted);
  await act(async () => {
    resolveCommand(Response.json(command("completed")));
    await flush();
  });
  assert.equal(notices.length, 0);
  assert.match(document.body.textContent, /已上报 80%/);
  assert.doesNotMatch(document.body.textContent, /已上报 70%/);
});

test("success refreshes reported state before showing a confirmation", async () => {
  const notices = [];
  let applied = false;
  globalThis.fetch = async (path) => {
    if (path.endsWith("/state"))
      return Response.json(snapshot("a", applied ? 40 : 70));
    applied = true;
    return Response.json(command("completed"));
  };
  await mount("a", (text) => notices.push(text));
  await applyBrightness(40);
  assert.equal(notices.length, 1);
  assert.match(document.body.textContent, /已上报 40%/);
});
