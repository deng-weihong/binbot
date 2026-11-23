import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-wordbook-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const books = await server.ssrLoadModule("/src/shared/model/wordbooks.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const { vocabularyCards } = await server.ssrLoadModule(
  "/src/shared/model/deviceFeatures.ts",
);
const { DeviceWorkspaceApp: App } = await server.ssrLoadModule("/src/app/App.tsx");
const { SaveWordButton } = await server.ssrLoadModule(
  "/src/features/vocabulary/WordbooksPanel.tsx",
);
const { ReviewBookDialog, AddBookWordsDialog } = await server.ssrLoadModule(
  "/src/features/vocabulary/WordbookDialogs.tsx",
);
const card = () => ({
  word: "wander",
  meaning: "漫步；闲逛",
  phonetic: "/ˈwɑːndər/",
  example: "We wander through the park.",
  translation: "我们漫步穿过公园。",
});
const byId = (state, id) => state.books.find((book) => book.id === id);

test("wordbook migration preserves device progress and separates personal libraries", () => {
  const legacy = data.createDemoData(data.devices[0]);
  const deviceProgress = structuredClone(legacy.features.vocabulary);
  delete legacy.wordbooks;
  const store = new Map([["binbot-ui-v1:binbot-001", JSON.stringify(legacy)]]);
  globalThis.localStorage = { getItem: (key) => store.get(key) ?? null };
  const restored = data.loadDemoData(data.devices[0]);
  assert.equal(byId(restored.wordbooks, books.builtinBookId).words.length, 30);
  assert.deepEqual(byId(restored.wordbooks, books.savedBookId).words, []);
  restored.wordbooks = books.addBookWords(
    restored.wordbooks,
    books.savedBookId,
    [card()],
  );
  restored.wordbooks = books.answerBookWord(
    restored.wordbooks,
    books.savedBookId,
    "wander",
    true,
  );
  store.set("binbot-ui-v1:binbot-001", JSON.stringify(restored));
  assert.deepEqual(
    data.loadDemoData(data.devices[0]).features.vocabulary,
    deviceProgress,
  );
  assert.equal(
    byId(data.loadDemoData(data.devices[0]).wordbooks, books.savedBookId)
      .words[0].remembered,
    true,
  );
  assert.deepEqual(
    byId(data.loadDemoData(data.devices[1]).wordbooks, books.savedBookId).words,
    [],
  );
  const independent = books.createWordbooks();
  independent.books[0].words[0].meaning = "改动副本";
  assert.equal(
    books.createWordbooks().books[0].words[0].meaning,
    vocabularyCards[0].meaning,
  );
  delete globalThis.localStorage;
});

test("saved books reject malformed cards, deduplicate equivalent words and retain canonical built-in content", () => {
  const normalized = books.normalizeWordbooks({
    activeBookId: "missing",
    books: [
      {
        id: books.builtinBookId,
        name: "替换内置词本",
        words: [
          { ...vocabularyCards[0], meaning: "篡改释义", remembered: true },
          card(),
        ],
      },
      {
        id: books.savedBookId,
        words: [
          card(),
          { ...card(), word: " ＷＡＮＤＥＲ ", remembered: true },
          null,
          { word: "blank" },
          { ...card(), word: "" },
        ],
      },
      {
        id: "mine",
        name: " 阅读 ",
        words: [{ ...card(), password: "__ignored_secret_field__", remembered: "true" }],
      },
      { id: "mine", name: "重复标识", words: [] },
      { id: " ", name: "无效标识" },
      { id: "bad-name", name: " " },
    ],
    api_key: "__ignored_secret_field__",
  });
  assert.equal(normalized.activeBookId, books.savedBookId);
  assert.equal(normalized.books.length, 4);
  assert.equal(byId(normalized, books.builtinBookId).name, "日常英语");
  assert.equal(
    byId(normalized, books.builtinBookId).words[0].meaning,
    vocabularyCards[0].meaning,
  );
  assert.equal(byId(normalized, books.builtinBookId).words[0].remembered, true);
  assert.equal(byId(normalized, books.builtinBookId).words.length, 30);
  assert.equal(byId(normalized, books.savedBookId).words.length, 1);
  assert.equal(byId(normalized, "mine").name, "阅读");
  assert.equal(byId(normalized, "mine").words[0].remembered, false);
  assert.ok(!JSON.stringify(normalized).includes("__ignored_secret_field__"));
});

test("creating, renaming, collecting and deleting keep books independent and preserve cleared lists", () => {
  const initial = books.createWordbooks();
  let state = books.addBook(initial, "reading", "阅读中的词");
  assert.equal(state.activeBookId, "reading");
  assert.throws(() => books.addBook(state, "duplicate", "阅读中的词"), /同名/);
  assert.throws(() => books.renameBook(state, "reading", "日常英语"), /同名/);
  state = books.renameBook(state, "reading", "周末阅读");
  state = books.addBookWords(state, "reading", [
    card(),
    { ...card(), word: " WANDER " },
  ]);
  state = books.addBookWords(state, books.savedBookId, [card()]);
  assert.equal(byId(state, "reading").words.length, 1);
  state = books.removeBookWord(state, books.savedBookId, "ＷＡＮＤＥＲ");
  assert.equal(byId(state, "reading").words.length, 1);
  assert.deepEqual(
    byId(books.normalizeWordbooks(state), books.savedBookId).words,
    [],
  );
  state = books.deleteBook(state, "reading");
  assert.equal(state.activeBookId, books.savedBookId);
  assert.equal(state.books.length, 3);
  assert.equal(books.deleteBook(state, books.builtinBookId), state);
  assert.equal(books.renameBook(state, books.savedBookId, "新名字"), state);
  assert.equal(
    books.removeBookWord(state, books.builtinBookId, vocabularyCards[0].word),
    state,
  );
  assert.equal(books.addBookWords(state, books.builtinBookId, [card()]), state);
  assert.deepEqual(initial, books.createWordbooks());
});

test("review prioritizes pending words, persists self-evaluation and never mutates another book", () => {
  const now = Date.parse("2026-11-17T01:00:00.000Z");
  let state = books.addBookWords(
    books.createWordbooks(),
    books.savedBookId,
    vocabularyCards.slice(0, 3),
  );
  const before = structuredClone(state);
  state = books.answerBookWord(
    state,
    books.savedBookId,
    vocabularyCards[0].word,
    true,
    now,
  );
  assert.equal(byId(before, books.savedBookId).words[0].remembered, false);
  assert.equal(byId(state, books.builtinBookId).words[0].remembered, false);
  const pending = books.reviewWords(byId(state, books.savedBookId), now);
  assert.deepEqual(
    pending.map((word) => word.word),
    vocabularyCards.slice(1, 3).map((word) => word.word),
  );
  pending[0].meaning = "独立的复习快照";
  assert.notEqual(
    byId(state, books.savedBookId).words[1].meaning,
    pending[0].meaning,
  );
  for (const item of vocabularyCards.slice(1, 3))
    state = books.answerBookWord(
      state,
      books.savedBookId,
      item.word,
      true,
      now,
    );
  assert.equal(
    books.reviewWords(byId(state, books.savedBookId), now).length,
    0,
  );
  state = books.answerBookWord(
    state,
    books.savedBookId,
    vocabularyCards[0].word,
    false,
    now,
  );
  assert.equal(
    books.reviewWords(
      byId(books.normalizeWordbooks(state), books.savedBookId),
      now + 5 * 60_000,
    ).length,
    1,
  );
});

test("manual words validate required content and CSV export preserves multiline text without executable cells", () => {
  assert.throws(() => books.validateWord({ ...card(), word: "123" }), /英文/);
  assert.throws(() => books.validateWord({ ...card(), meaning: " " }), /释义/);
  assert.throws(
    () => books.validateWord({ ...card(), example: "x".repeat(501) }),
    /例句/,
  );
  const library = books.addBookWords(
    books.createWordbooks(),
    books.savedBookId,
    [
      {
        ...card(),
        meaning: '=HYPERLINK("x")',
        example: 'First line, "quoted"\nNext line.',
      },
    ],
  );
  const csv = books.wordbookCsv(byId(library, books.savedBookId));
  assert.ok(csv.startsWith("\uFEFF"));
  assert.ok(csv.includes('"\'=HYPERLINK(""x"")"'));
  assert.ok(csv.includes('"First line, ""quoted""\nNext line."'));
  assert.ok(csv.includes("网页复习状态"));
  assert.ok(!csv.includes("准确率"));
});

test("review starts with the answer hidden and saved words remain distinguishable in both entry points", () => {
  const current = data.createDemoData(data.devices[0]);
  current.wordbooks = books.addBookWords(current.wordbooks, books.savedBookId, [
    card(),
  ]);
  const savedBook = byId(current.wordbooks, books.savedBookId);
  const review = renderToStaticMarkup(
    React.createElement(ReviewBookDialog, {
      book: savedBook,
      words: books.reviewWords(savedBook),
      answer: () => {},
      close: () => {},
    }),
  );
  assert.ok(review.includes("wander"));
  assert.ok(review.includes("想一想，再看释义"));
  assert.ok(!review.includes(card().meaning));
  assert.ok(!review.includes(card().example));
  const button = renderToStaticMarkup(
    React.createElement(SaveWordButton, {
      data: current,
      setData: () => {},
      notify: () => {},
      card: card(),
    }),
  );
  assert.ok(button.includes('aria-pressed="true"'));
  assert.ok(button.includes("从生词本移除 wander"));
  const pickerBook = byId(
    books.addBookWords(current.wordbooks, books.savedBookId, [
      vocabularyCards[0],
    ]),
    books.savedBookId,
  );
  const picker = renderToStaticMarkup(
    React.createElement(AddBookWordsDialog, {
      book: pickerBook,
      save: () => true,
      close: () => {},
    }),
  );
  assert.ok(picker.includes("已加入"));
  assert.ok(picker.includes('disabled="" checked=""'));
});

for (const device of data.devices) {
  test(`${device.id}/vocabulary#wordbooks opens books in the current device, including offline use`, () => {
    globalThis.window = {
      location: {
        pathname: `/devices/${device.id}/vocabulary`,
        hash: "#wordbooks",
      },
    };
    const stored = data.createDemoData(device);
    stored.wordbooks = books.addBook(stored.wordbooks, "mine", "阅读生词");
    stored.wordbooks = books.addBookWords(stored.wordbooks, "mine", [card()]);
    globalThis.localStorage = {
      getItem: (key) =>
        key === `binbot-ui-v1:${device.id}` ? JSON.stringify(stored) : null,
    };
    const html = renderToStaticMarkup(React.createElement(App));
    assert.ok(html.includes('aria-label="单词学习分类"'));
    assert.ok(html.includes('aria-label="选择单词本"'));
    assert.ok(html.includes("我的生词本"));
    assert.ok(html.includes("阅读生词"));
    assert.ok(html.includes("wander"));
    assert.ok(html.includes(`${device.name}的单词本`));
    assert.ok(html.includes("开始复习"));
    assert.ok(!html.includes("本轮作答"));
    assert.ok(!html.includes("准确率"));
    delete globalThis.localStorage;
  });
}
