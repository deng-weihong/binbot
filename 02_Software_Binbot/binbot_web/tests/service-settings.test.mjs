import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-service-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const services = await server.ssrLoadModule("/src/shared/model/serviceConfiguration.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const { GlobalServices: App } = await server.ssrLoadModule(
  "/src/app/ConsolePortal.tsx",
);
const configured = { openai: true, asr: true, tts: true, seeduplex: true };
const fixture = () => ({
  revision: "revision-one",
  config: services.createServicePreferences().providers,
  credentials_configured: configured,
});

test("service drafts migrate per device and discard credentials and unknown fields", () => {
  const first = data.createDemoData(data.devices[0]);
  first.services.providers.openai.llm_model = "my-model";
  first.services.providers.openai.api_key = "private-test-key";
  first.services.feishu.appId = "cli_test";
  first.services.feishu.app_secret = "private-test-secret";
  first.services.secrets = { asr: "private-test-asr" };
  first.services.password = "private-test-password";
  const store = new Map([["binbot-ui-v1:binbot-001", JSON.stringify(first)]]);
  globalThis.localStorage = { getItem: (key) => store.get(key) ?? null };
  const restored = data.loadDemoData(data.devices[0]);
  assert.equal(restored.services.providers.openai.llm_model, "my-model");
  assert.equal(restored.services.feishu.appId, "cli_test");
  assert.doesNotMatch(JSON.stringify(restored.services), /private-test/);
  assert.notEqual(
    data.loadDemoData(data.devices[1]).services.providers.openai.llm_model,
    "my-model",
  );
  delete first.services;
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(first));
  assert.equal(
    data.loadDemoData(data.devices[0]).services.providers.voice,
    "pipeline",
  );
});

test("saving one provider section preserves the other section and the shared revision", async () => {
  const snapshot = fixture();
  const before = structuredClone(snapshot);
  const draft = structuredClone(snapshot.config);
  draft.openai.llm_model = "new-model";
  draft.openai.tts_voice = "unsaved-voice";
  draft.voice = "seeduplex";
  const llm = services.mergeProviderSection(snapshot.config, draft, "llm");
  assert.equal(llm.openai.llm_model, "new-model");
  assert.equal(llm.openai.tts_voice, before.config.openai.tts_voice);
  assert.equal(llm.voice, before.config.voice);
  const voice = services.mergeProviderSection(snapshot.config, draft, "voice");
  assert.equal(voice.openai.llm_model, before.config.openai.llm_model);
  assert.equal(voice.openai.tts_voice, "unsaved-voice");
  assert.deepEqual(snapshot, before);
  const calls = [];
  const transport = services.createServiceTransport(async (path, options) => {
    calls.push([path, options]);
    return Response.json(
      options.method === "PUT"
        ? { ...snapshot, revision: "revision-two", config: llm }
        : snapshot,
    );
  });
  const loaded = await transport.load();
  const saved = await transport.save(loaded, llm, {
    openai: " new-test-key ",
    asr: " ",
    unknown: "never-send",
  });
  const [path, options] = calls[1],
    body = JSON.parse(options.body);
  assert.equal(path, "/api/v1/admin/providers/configuration");
  assert.equal(options.method, "PUT");
  assert.equal(options.credentials, "same-origin");
  assert.equal(options.cache, "no-store");
  assert.equal(body.revision, "revision-one");
  assert.deepEqual(body.secrets, { openai: "new-test-key" });
  assert.deepEqual(Object.keys(body).sort(), ["config", "revision", "secrets"]);
  assert.equal(saved.revision, "revision-two");
  assert.doesNotMatch(JSON.stringify(saved), /new-test-key/);
  assert.ok(calls.every(([url]) => !url.includes("/secrets")));
});

test("configuration failures cannot masquerade as an applied or authorized service", async () => {
  for (const [response, status] of [
    [new Response("<html>", { headers: { "Content-Type": "text/html" } }), 200],
    [new Response("denied", { status: 401 }), 401],
    [new Response("denied", { status: 403 }), 403],
    [Response.json({ error: "conflict" }, { status: 409 }), 409],
    [Response.json({ config: {} }), 502],
  ]) {
    await assert.rejects(
      services.createServiceTransport(async () => response).load(),
      (error) =>
        error instanceof services.ServiceError && error.status === status,
    );
  }
  const transport = services.createServiceTransport(async () =>
    Response.json({ error: "private-test-key" }, { status: 500 }),
  );
  await assert.rejects(
    transport.save(fixture(), fixture().config, {}),
    (error) => !error.message.includes("private-test-key"),
  );
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    services
      .createServiceTransport(async (_path, options) => {
        assert.ok(options.signal.aborted);
        throw new DOMException("Aborted", "AbortError");
      })
      .load(controller.signal),
    { name: "AbortError" },
  );
});

test("provider validation checks active credentials, endpoints and voice IDs", () => {
  const config = fixture().config;
  config.volcengine.tts_speaker = "my-voice";
  assert.doesNotThrow(() =>
    services.validateProviderValues(config, configured, {}),
  );
  assert.throws(
    () =>
      services.validateProviderValues(
        config,
        { ...configured, openai: false },
        {},
      ),
    /API Key/,
  );
  assert.doesNotThrow(() =>
    services.validateProviderValues(
      config,
      { ...configured, openai: false },
      { openai: "replacement" },
    ),
  );
  config.voice = "seeduplex";
  assert.throws(
    () =>
      services.validateProviderValues(
        config,
        { ...configured, seeduplex: false },
        {},
      ),
    /实时语音/,
  );
  for (const url of [
    "file:///tmp/model",
    "https://user:password@example.com/v1",
    "invalid",
    "https://example.com/#secret",
  ])
    assert.equal(services.validServiceUrl(url, ["http:", "https:"]), false);
});

for (const device of data.devices) {
  for (const section of ["llm", "voice"]) {
    test(`public ${section} remains shared when opened after ${device.id}, without exposing stored credentials`, () => {
      globalThis.window = {
        location: {
          pathname: "/services",
          hash: `#${section}`,
        },
      };
      globalThis.localStorage = { getItem: () => null };
      const html = renderToStaticMarkup(React.createElement(App));
      assert.ok(html.includes('aria-label="公共服务分类"'));
      assert.ok(html.includes("service-settings"));
      assert.ok(html.includes("保存草稿"));
      assert.ok(!html.includes("保存名称"));
      assert.ok(html.includes(`/services#${section}`));
      assert.ok(!html.includes('aria-label="主导航"'));
      if (section === "llm") {
        assert.ok(html.includes("模型名称"));
        assert.ok(html.includes('type="password"'));
      }
      if (section === "voice") {
        assert.ok(html.includes("听你说 · 语音识别"));
        assert.ok(html.includes("说给你听 · 语音合成"));
      }
    });
  }
}
