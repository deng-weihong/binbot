import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";

const server = await createServer({
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const { ConversationController, parseDeviceChat } = await server.ssrLoadModule(
  "/src/shared/model/assistantConversation.ts",
);
const { createConsoleTransport } =
  await server.ssrLoadModule("/src/shared/api/consoleApi.ts");

const messages = [
  { role: "user", content: "你好" },
  { role: "assistant", content: "你好，有什么想聊的？" },
];
const reply = (id = "a", list = messages) => ({
  device_id: id,
  messages: list,
});
const stub = (extra = {}) => ({
  chatHistory: async () => [],
  sendChat: async () => messages,
  ...extra,
});

test("chat responses must belong to the current device and contain dialogue messages", () => {
  assert.throws(() => parseDeviceChat(reply("b"), "a"), /不一致/);
  assert.throws(
    () => parseDeviceChat(reply("a", [{ role: "system", content: "x" }]), "a"),
    /格式不正确/,
  );
  assert.deepEqual(parseDeviceChat(reply(), "a"), messages);
});

test("refresh restores the device chat, retains it on an error and picks up new turns", async () => {
  let state,
    failure = false,
    history = messages;
  const controller = new ConversationController(
    "a",
    stub({
      chatHistory: async () => {
        if (failure) throw new Error("unavailable");
        return history;
      },
    }),
    (value) => {
      state = value;
    },
  );
  await controller.load();
  assert.deepEqual(state.messages, messages);
  failure = true;
  await controller.load();
  assert.equal(state.syncError, true);
  assert.deepEqual(state.messages, messages);
  failure = false;
  history = [...messages, { role: "user", content: "今天聊过什么？" }];
  await controller.load();
  assert.equal(state.syncError, false);
  assert.deepEqual(state.messages, history);
  controller.dispose();
});

test("sending aborts a stale refresh and accepts only one pending send", async () => {
  let finish,
    oldHistory,
    oldSignal,
    state,
    reads = 0,
    sends = 0;
  const controller = new ConversationController(
    "a",
    stub({
      chatHistory: async (_id, signal) => {
        if (!reads++) return [];
        oldSignal = signal;
        return new Promise((resolve) => {
          oldHistory = resolve;
        });
      },
      sendChat: () => {
        sends++;
        return new Promise((resolve) => {
          finish = resolve;
        });
      },
    }),
    (value) => {
      state = value;
    },
  );
  await controller.load();
  const refresh = controller.load();
  const sending = controller.send("你好");
  assert.equal(oldSignal.aborted, true);
  assert.equal(state.sending, true);
  assert.equal(await controller.send("你好"), false);
  assert.equal(sends, 1);
  finish(messages);
  assert.equal(await sending, true);
  oldHistory([]);
  await refresh;
  assert.deepEqual(state.messages, messages);
  controller.dispose();
});

test("changing devices aborts the request and drops its late success", async () => {
  let finish,
    signal,
    changes = 0;
  const controller = new ConversationController(
    "a",
    stub({
      sendChat: (_id, _text, _request, requestSignal) => {
        signal = requestSignal;
        return new Promise((resolve) => {
          finish = resolve;
        });
      },
    }),
    () => changes++,
  );
  await controller.load();
  const sending = controller.send("你好");
  controller.dispose();
  const before = changes;
  assert.equal(signal.aborted, true);
  finish(messages);
  assert.equal(await sending, false);
  assert.equal(changes, before);
});

test("failed sends retain confirmed messages and reuse the request ID on retry", async () => {
  let state;
  const ids = [];
  const controller = new ConversationController(
    "a",
    stub({
      chatHistory: async () => messages,
      sendChat: async (_id, _text, id) => {
        ids.push(id);
        if (ids.length === 1) throw new Error("服务暂不可用");
        return messages;
      },
    }),
    (value) => {
      state = value;
    },
  );
  await controller.load();
  assert.equal(await controller.send("继续"), false);
  assert.deepEqual(state.messages, messages);
  assert.equal(state.error, "服务暂不可用");
  assert.equal(await controller.send("继续"), true);
  assert.equal(ids[0], ids[1]);
  controller.dispose();
});

test("history and sending use the same authenticated device endpoint", async () => {
  let responseDevice = "desk:01";
  const requests = [];
  const transport = createConsoleTransport(async (url, init) => {
    requests.push(init);
    assert.equal(url, "/api/v1/admin/devices/desk%3A01/chat");
    assert.equal(init.credentials, "same-origin");
    assert.equal(init.cache, "no-store");
    return Response.json(reply(responseDevice));
  });

  assert.deepEqual(await transport.chatHistory("desk:01"), messages);
  assert.deepEqual(
    await transport.sendChat("desk:01", "继续", "request-one"),
    messages,
  );
  assert.equal(requests[1].method, "POST");
  assert.deepEqual(JSON.parse(requests[1].body), {
    text: "继续",
    request_id: "request-one",
  });
  responseDevice = "desk:02";
  await assert.rejects(transport.chatHistory("desk:01"), /当前设备不一致/);
});
