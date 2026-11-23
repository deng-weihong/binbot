import { after, test } from "node:test";
import assert from "node:assert/strict";
import { readFile, access } from "node:fs/promises";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-feature-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const features = await server.ssrLoadModule("/src/shared/model/deviceFeatures.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const agenda = await server.ssrLoadModule("/src/shared/model/schedules.ts");
const todos = await server.ssrLoadModule("/src/shared/model/todos.ts");
const { default: HomeTodoList } = await server.ssrLoadModule(
  "/src/features/home/HomeTodoList.tsx",
);
const { DeviceWorkspaceApp: App } = await server.ssrLoadModule("/src/app/App.tsx");

test("device catalogs match all 30 App words, 32 face IDs, and module profiles", async () => {
  const state = await readFile(
    new URL(
      "../../binbot_app/apps/binbot_app/src/core/app_state.c",
      import.meta.url,
    ),
    "utf8",
  );
  const block = state
    .split("vocabulary_cards[APP_VOCABULARY_CAPACITY] = {")[1]
    .split("\n};")[0];
  const entries = [
    ...block.matchAll(
      /\{\s*"([^"\n]*)",\s*"([^"\n]*)",\s*"([^"\n]*)",\s*"([^"\n]*)",\s*"([^"\n]*)"\s*\}/g,
    ),
  ].map((match) =>
    Object.fromEntries(
      ["word", "phonetic", "meaning", "example", "translation"].map(
        (key, index) => [key, match[index + 1]],
      ),
    ),
  );
  assert.equal(entries.length, 30);
  assert.deepEqual(features.vocabularyCards, entries);
  const index = JSON.parse(
    await readFile(
      new URL("../../binbot_app/assets/standby/index.json", import.meta.url),
      "utf8",
    ),
  );
  assert.deepEqual(
    features.watchFaces.map((face) => face.id),
    index.themes,
  );
  assert.equal(features.watchFaces.length, 32);
  await Promise.all(
    features.watchFaces.map((face) =>
      access(new URL(`../public${face.preview}`, import.meta.url)),
    ),
  );
  assert.deepEqual(
    features.moduleCatalog.map((module) => module.profile),
    [2, 1, 3, 5],
  );
  assert.equal(features.gestureCatalog.length, 5);
  assert.equal(features.gestureActions.length, 12);
  assert.equal(
    features.gestureActions.filter((action) => !action.available).length,
    3,
  );
});

test("legacy and absent feature settings receive independent per-device defaults", () => {
  for (const device of data.devices)
    assert.deepEqual(
      features.normalizeDeviceFeatures(undefined, device),
      features.createDeviceFeatures(device),
    );
  const first = features.createDeviceFeatures(data.devices[0]);
  const second = features.createDeviceFeatures(data.devices[1]);
  first.vocabulary.knownWords.length = 0;
  first.gestures.mappings.thumb.enabled = false;
  first.modules.light.brightness = 0;
  assert.equal(second.vocabulary.knownWords.length, 8);
  assert.equal(second.gestures.mappings.thumb.enabled, true);
  assert.equal(second.modules.light.brightness, 80);
  assert.equal(
    features.createDeviceFeatures(data.devices[2]).modules.connected,
    null,
  );
});

test("malformed storage cannot inject unknown faces, actions, media, or impossible counts", () => {
  const value = features.normalizeDeviceFeatures(
    {
      vocabulary: {
        knownWords: ["serene", "serene", "unknown", null],
        reviewedCount: -5,
        correctCount: 999,
        currentWord: "unknown",
        cyclesCompleted: Infinity,
      },
      watchFace: { themeId: "missing", standbyId: "face", idleSeconds: 12 },
      gestures: {
        enabled: "yes",
        mappings: { thumb: { action: "unsupported", enabled: false } },
      },
      modules: {
        connected: "missing",
        pen: {
          mode: "missing",
          imageId: "https://invalid.test/image.png",
          intervalSeconds: 17,
        },
        light: {
          brightness: 1024,
          speed: -8,
          color: "red; url(x)",
          effect: "missing",
        },
      },
    },
    data.devices[0],
  );
  assert.deepEqual(value.vocabulary.knownWords, ["serene"]);
  assert.equal(value.vocabulary.reviewedCount, 1);
  assert.equal(value.vocabulary.correctCount, 1);
  assert.equal(value.vocabulary.cyclesCompleted, 1);
  assert.equal(value.watchFace.themeId, "napping-cat");
  assert.equal(value.watchFace.idleSeconds, 60);
  assert.notEqual(value.watchFace.standbyId, "face");
  assert.equal(value.gestures.mappings.thumb.action, "play");
  assert.equal(value.gestures.mappings.thumb.enabled, false);
  assert.equal(value.modules.connected, "pen");
  assert.equal(value.modules.pen.imageId, "happy");
  assert.equal(value.modules.pen.intervalSeconds, 0);
  assert.equal(value.modules.light.brightness, 100);
  assert.equal(value.modules.light.speed, 5);
  assert.equal(value.modules.light.color, "#ffd166");
  assert.doesNotThrow(() =>
    features.normalizeDeviceFeatures(
      { vocabulary: [], gestures: null, modules: "broken" },
      data.devices[0],
    ),
  );
});

test("choosing a face cancels shuffle and remembers the previous standby face", () => {
  const original = {
    themeId: "napping-cat",
    standbyId: "napping-cat",
    idleSeconds: 60,
    shuffle: true,
  };
  const expressions = features.selectWatchFace(original, "face");
  assert.equal(expressions.shuffle, false);
  assert.equal(expressions.standbyId, "napping-cat");
  assert.equal(
    features.setWatchFaceShuffle(expressions, true).themeId,
    "napping-cat",
  );
  assert.equal(
    features.selectWatchFace(expressions, "pixel-city").standbyId,
    "pixel-city",
  );
  assert.equal(features.selectWatchFace(original, "missing"), original);
  assert.equal(original.shuffle, true);
});

test("old local data is migrated without losing device names or other preferences", () => {
  const old = data.createDemoData(data.devices[0]);
  delete old.features;
  old.scenes = [{ id: "legacy-scene", name: "旧场景", actions: [] }];
  old.schedules = [
    {
      id: "legacy-schedule",
      name: "旧日程",
      time: "11:30",
      note: "保留的备注",
      enabled: true,
      done: true,
    },
  ];
  old.deviceName = "我的小伴";
  old.volume = 23;
  old.assistant.name = "团子";
  old.assistant.style = "细致一些";
  old.assistant.permissions = {
    管理待办与提醒: false,
    控制屏幕与音乐: false,
    读取日历: false,
  };
  const store = new Map([["binbot-ui-v1:binbot-001", JSON.stringify(old)]]);
  globalThis.localStorage = { getItem: (key) => store.get(key) ?? null };
  const first = data.loadDemoData(data.devices[0]);
  assert.equal(first.deviceName, "我的小伴");
  assert.equal(first.volume, 23);
  assert.deepEqual(first.scenes, old.scenes);
  assert.deepEqual(first.schedules, [
    { ...old.schedules[0], date: agenda.demoDate },
  ]);
  assert.equal(first.assistant.name, "团子");
  assert.equal(first.assistant.style, "细致一些");
  assert.deepEqual(first.assistant.memories, old.assistant.memories);
  assert.ok(!("permissions" in first.assistant));
  first.features.watchFace = features.selectWatchFace(
    first.features.watchFace,
    "pixel-city",
  );
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(first));
  assert.equal(
    data.loadDemoData(data.devices[0]).features.watchFace.themeId,
    "pixel-city",
  );
  assert.equal(
    data.loadDemoData(data.devices[1]).features.watchFace.themeId,
    "little-grove",
  );
  globalThis.localStorage = {
    getItem: () => {
      throw new Error("blocked");
    },
  };
  assert.equal(
    data.loadDemoData(data.devices[0]).features.vocabulary.knownWords.length,
    18,
  );
});

test("home agenda restores absent data, preserves an empty list and keeps each device's schedule", () => {
  const saved = data.createDemoData(data.devices[0]);
  delete saved.schedules;
  const store = new Map([["binbot-ui-v1:binbot-001", JSON.stringify(saved)]]);
  globalThis.localStorage = { getItem: (key) => store.get(key) ?? null };
  const restored = data.loadDemoData(data.devices[0]);
  assert.equal(restored.schedules.length, 3);
  assert.equal(restored.schedules[0].name, "整理新产品的想法");
  saved.schedules = [];
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(saved));
  assert.deepEqual(data.loadDemoData(data.devices[0]).schedules, []);
  assert.equal(
    data.loadDemoData(data.devices[1]).schedules[0].name,
    "留一点时间读书",
  );
  assert.equal(restored.schedules.length, 3);
});

test("agenda dates keep different days separate and reject invalid saved entries", () => {
  const base = data.createDemoData(data.devices[0]).schedules[0];
  const normalized = agenda.normalizeSchedules(
    [
      { ...base, id: "late", time: "18:00", done: true },
      { ...base, id: "tomorrow", date: "2026-11-17" },
      { ...base, id: "hidden", enabled: false },
      { ...base, id: "early", time: "09:00" },
      { ...base, id: "bad-time", time: "25:00" },
      { ...base, id: "bad-date", date: "2026-02-30" },
      { ...base, id: "bad-name", name: " " },
      null,
    ],
    [],
  );
  assert.deepEqual(
    normalized.map(({ id }) => id),
    ["late", "tomorrow", "hidden", "early"],
  );
  assert.deepEqual(
    agenda.schedulesForDate(normalized, agenda.demoDate).map(({ id }) => id),
    ["early", "late"],
  );
  assert.deepEqual(
    agenda.schedulesForDate(normalized, "2026-11-17").map(({ id }) => id),
    ["tomorrow"],
  );
  assert.equal(normalized[0].id, "late");
});

test("todo migration restores missing fields, preserves cleared lists and isolates device records", () => {
  const saved = data.createDemoData(data.devices[0]);
  delete saved.todos;
  const store = new Map([["binbot-ui-v1:binbot-001", JSON.stringify(saved)]]);
  globalThis.localStorage = { getItem: (key) => store.get(key) ?? null };
  const restored = data.loadDemoData(data.devices[0]);
  assert.equal(restored.todos.length, 3);
  assert.deepEqual(restored.schedules, saved.schedules);
  restored.todos = [{ id: "mine", text: "自己的待办", done: true }];
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(restored));
  assert.deepEqual(data.loadDemoData(data.devices[0]).todos, restored.todos);
  assert.ok(
    data
      .loadDemoData(data.devices[1])
      .todos.every((todo) => todo.text !== "自己的待办"),
  );
  restored.todos = [];
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(restored));
  assert.deepEqual(data.loadDemoData(data.devices[0]).todos, []);
});

test("saved todos reject invalid entries and honor the device text and slot limits", () => {
  const normalized = todos.normalizeTodos(
    [
      { id: "ok", text: " 保留的待办 ", done: true },
      { id: "ok", text: "重复记录", done: false },
      { id: "empty", text: " " },
      { id: "long", text: "字".repeat(24) },
      { id: " ", text: "无效标识" },
      null,
      ...Array.from({ length: 10 }, (_, index) => ({
        id: `valid-${index}`,
        text: "字".repeat(23),
        done: false,
      })),
    ],
    [],
  );
  assert.equal(normalized.length, 8);
  assert.deepEqual(normalized[0], { id: "ok", text: "保留的待办", done: true });
  assert.equal(normalized[1].text, "字".repeat(23));
  assert.ok(
    !normalized.some((todo) => ["empty", "long", " "].includes(todo.id)),
  );
  const fallback = todos.createDemoTodos(data.devices[0]);
  todos.normalizeTodos(null, fallback)[0].text = "独立副本";
  assert.notEqual(fallback[0].text, "独立副本");
});

test("home todo panel distinguishes completed items and keeps an empty list usable", () => {
  const current = data.createDemoData(data.devices[0]);
  current.todos = [
    { id: "open", text: "还要做的事", done: false },
    { id: "done", text: "已做完的事", done: true },
  ];
  const render = () =>
    renderToStaticMarkup(
      React.createElement(HomeTodoList, {
        data: current,
        setData: () => {},
        notify: () => {},
      }),
    );
  const html = render();
  assert.ok(html.includes('aria-label="新待办内容"'));
  assert.ok(html.includes('aria-label="完成待办：还要做的事"'));
  assert.ok(html.includes('aria-label="恢复待办：已做完的事"'));
  assert.ok(html.includes('aria-label="修改待办：还要做的事"'));
  assert.ok(html.includes('aria-label="删除待办：已做完的事"'));
  current.todos = [];
  assert.ok(render().includes("暂无待办"));
  current.todos = [{ id: "done", text: "已做完的事", done: true }];
  assert.ok(render().includes("待办已全部完成"));
});

for (const device of data.devices) {
  for (const page of ["vocabulary", "watchface", "gestures", "modules"]) {
    test(`${device.id}/${page} renders in its own context, including offline restrictions`, () => {
      globalThis.window = {
        location: { pathname: `/devices/${device.id}/${page}` },
      };
      globalThis.localStorage = { getItem: () => null };
      const html = renderToStaticMarkup(React.createElement(App));
      assert.ok(html.includes(`${device.name}的专属空间`));
      assert.ok(html.includes(`/devices/${device.id}/vocabulary`));
      assert.ok(!html.includes("这个空间还没准备好"));
      if (page === "vocabulary") {
        assert.equal((html.match(/查看 [a-z]+ 的词卡/g) || []).length, 30);
        assert.ok(html.includes("本轮作答"));
      } else {
        if (page === "modules") {
          assert.ok(!html.includes('aria-label="设备设置分类"'));
          assert.ok(html.includes('aria-label="组件内容分类"'));
          assert.match(
            html,
            /class="rail-link active" aria-current="page" aria-label="组件模块"/,
          );
        } else {
          assert.ok(html.includes('aria-label="设备设置分类"'));
        }
        assert.ok(html.includes("设置仅保存在当前浏览器"));
        if (!device.online) assert.match(html, /type="submit" disabled=""/);
      }
      if (page === "watchface")
        assert.equal((html.match(/class="face-option/g) || []).length, 32);
      if (page === "gestures")
        assert.equal((html.match(/class="gesture-row /g) || []).length, 5);
      if (page === "modules")
        assert.equal((html.match(/class="module-choice /g) || []).length, 4);
    });
  }
}

for (const connected of ["holo", "pen", "turntable", "light", null]) {
  test(`module preview renders the ${connected || "empty"} slot without enabling absent hardware`, () => {
    const fixture = data.createDemoData(data.devices[0]);
    fixture.features.modules.connected = connected;
    globalThis.window = {
      location: { pathname: "/devices/binbot-001/modules" },
    };
    globalThis.localStorage = { getItem: () => JSON.stringify(fixture) };
    const html = renderToStaticMarkup(React.createElement(App));
    if (connected === "holo") assert.ok(html.includes("随机换图间隔"));
    if (connected === "pen") assert.ok(html.includes("时间与天气"));
    if (connected === "turntable") assert.ok(html.includes("转动速度"));
    if (connected === "light") {
      assert.ok(html.includes("灯光亮度"));
      assert.match(html, /min="5"/);
    }
    if (connected === null) {
      assert.ok(html.includes("插入笔筒后即可保存设置"));
      assert.ok(html.includes('<fieldset disabled="">'));
    }
  });
}
