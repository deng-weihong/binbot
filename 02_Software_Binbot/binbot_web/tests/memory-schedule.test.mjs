import { after, test } from "node:test";
import assert from "node:assert/strict";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-memory-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const memory = await server.ssrLoadModule("/src/shared/model/memorySchedule.ts");
const books = await server.ssrLoadModule("/src/shared/model/wordbooks.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const { MemoryReviewPlan, WordReviewTiming } = await server.ssrLoadModule(
  "/src/features/vocabulary/MemoryReviewPlan.tsx",
);
const { default: WordbooksPanel } = await server.ssrLoadModule(
  "/src/features/vocabulary/WordbooksPanel.tsx",
);
const now = Date.parse("2026-11-17T01:00:00.000Z");
const minute = 60_000,
  day = 24 * 60 * minute;
const iso = (at) => new Date(at).toISOString();
const card = (word = "wander") => ({
  word,
  meaning: "漫步",
  phonetic: "",
  example: "",
  translation: "",
});
const byId = (state, id) => state.books.find((book) => book.id === id);

test("first learning schedules a review at the exact 20 minute boundary", () => {
  const fresh = memory.createMemorySchedule();
  const learned = memory.scheduleAnswer(fresh, true, now);
  assert.equal(memory.reviewStatus({ review: fresh }, now), "new");
  assert.equal(learned.stage, 1);
  assert.equal(learned.nextReviewAt, iso(now + 20 * minute));
  assert.equal(learned.reviewCount, 1);
  assert.equal(
    memory.reviewStatus({ review: learned }, now + 20 * minute - 1),
    "scheduled",
  );
  assert.equal(
    memory.reviewStatus({ review: learned }, now + 20 * minute),
    "due",
  );
  assert.deepEqual(fresh, memory.createMemorySchedule());
  assert.deepEqual(memory.normalizeMemorySchedule(learned), learned);
});

test("successful due reviews expand all intervals and continue monthly consolidation", () => {
  const expected = [
    20 * minute,
    60 * minute,
    day,
    2 * day,
    4 * day,
    7 * day,
    15 * day,
    30 * day,
  ];
  let state = memory.createMemorySchedule(),
    at = now;
  for (let index = 0; index < expected.length; index++) {
    state = memory.scheduleAnswer(state, true, at);
    assert.equal(state.stage, index + 1);
    assert.equal(Date.parse(state.nextReviewAt) - at, expected[index]);
    assert.equal(state.reviewCount, index + 1);
    assert.deepEqual(memory.normalizeMemorySchedule(state), state);
    at = Date.parse(state.nextReviewAt);
  }
  state = memory.scheduleAnswer(state, true, at);
  assert.equal(state.stage, 8);
  assert.equal(Date.parse(state.nextReviewAt) - at, 30 * day);
  assert.equal(state.lapseCount, 0);
});

test("forgetting resets the sequence, waits five minutes and then relearns at twenty minutes", () => {
  let state = memory.scheduleAnswer(memory.createMemorySchedule(), true, now);
  state = memory.scheduleAnswer(state, true, Date.parse(state.nextReviewAt));
  const at = Date.parse(state.nextReviewAt);
  state = memory.scheduleAnswer(state, false, at);
  assert.equal(state.stage, 0);
  assert.equal(state.lapseCount, 1);
  assert.equal(state.nextReviewAt, iso(at + 5 * minute));
  assert.equal(
    memory.reviewStatus({ review: state }, at + 5 * minute - 1),
    "scheduled",
  );
  assert.equal(memory.reviewStatus({ review: state }, at + 5 * minute), "due");
  state = memory.scheduleAnswer(state, true, at + 5 * minute);
  assert.equal(state.stage, 1);
  assert.equal(state.nextReviewAt, iso(at + 25 * minute));
  assert.equal(state.lapseCount, 1);
  assert.deepEqual(memory.normalizeMemorySchedule(state), state);
});

test("early success cannot skip stages, extend deadlines or bypass the relearning wait", () => {
  const state = memory.scheduleAnswer(memory.createMemorySchedule(), true, now);
  const early = memory.scheduleAnswer(state, true, now + minute);
  assert.equal(early.stage, state.stage);
  assert.equal(early.nextReviewAt, state.nextReviewAt);
  assert.equal(early.reviewCount, 2);
  assert.deepEqual(memory.normalizeMemorySchedule(early), early);
  const lapse = memory.scheduleAnswer(early, false, now + 2 * minute);
  const earlyRelearn = memory.scheduleAnswer(lapse, true, now + 3 * minute);
  assert.equal(earlyRelearn.stage, 0);
  assert.equal(earlyRelearn.nextReviewAt, lapse.nextReviewAt);
  assert.deepEqual(memory.normalizeMemorySchedule(earlyRelearn), earlyRelearn);
  assert.throws(() => memory.scheduleAnswer(early, true, now - 1), /校准时间/);
  assert.throws(() => memory.scheduleAnswer(early, true, NaN), /当前时间/);
});

test("late reviews advance one stage from actual answer time and do not skip missed deadlines", () => {
  const state = memory.scheduleAnswer(memory.createMemorySchedule(), true, now);
  const late = memory.scheduleAnswer(state, true, now + 9 * day);
  assert.equal(late.stage, 2);
  assert.equal(late.nextReviewAt, iso(now + 9 * day + 60 * minute));
});

test("sessions prioritize the oldest due words, then new words, and exclude waiting words", () => {
  const earlier = {
    ...card("earlier"),
    review: memory.scheduleAnswer(memory.createMemorySchedule(), true, now),
  };
  const later = {
    ...card("later"),
    review: memory.scheduleAnswer(
      memory.createMemorySchedule(),
      false,
      now + 20 * minute,
    ),
  };
  const waiting = {
    ...card("waiting"),
    review: memory.scheduleAnswer(
      memory.createMemorySchedule(),
      true,
      now + 25 * minute,
    ),
  };
  const fresh = { ...card("fresh"), review: memory.createMemorySchedule() };
  const queue = memory.scheduledReviewWords(
    [waiting, fresh, later, earlier],
    now + 30 * minute,
  );
  assert.deepEqual(
    queue.map((word) => word.word),
    ["earlier", "later", "fresh"],
  );
  queue[0].review.history[0].stage = 8;
  assert.equal(earlier.review.history[0].stage, 1);
  assert.deepEqual(
    memory.scheduledReviewWords([waiting], now + 30 * minute),
    [],
  );
  const summary = memory.reviewSummary(
    [earlier, later, waiting, fresh],
    now + 30 * minute,
  );
  assert.deepEqual(summary, {
    due: 2,
    fresh: 1,
    scheduled: 1,
    nextReviewAt: waiting.review.nextReviewAt,
  });
});

test("legacy remembered words retain their value without invented review times or history", () => {
  const old = books.addBookWords(books.createWordbooks(), books.savedBookId, [
    card(),
  ]);
  const legacyWord = byId(old, books.savedBookId).words[0];
  legacyWord.remembered = true;
  delete legacyWord.review;
  const migrated = books.normalizeWordbooks(old);
  const word = byId(migrated, books.savedBookId).words[0];
  assert.equal(word.remembered, true);
  assert.deepEqual(word.review, memory.createMemorySchedule());
  assert.equal(
    books.reviewWords(byId(migrated, books.savedBookId), now).length,
    1,
  );
});

test("malformed saved schedules cannot postpone review and invalid history is discarded", () => {
  const valid = memory.scheduleAnswer(memory.createMemorySchedule(), true, now);
  for (const patch of [
    { stage: 99 },
    { stage: 1.5 },
    { reviewCount: -1 },
    { lapseCount: 4 },
    { nextReviewAt: iso(now - 1) },
    { nextReviewAt: iso(now + 40 * day) },
    { lastReviewedAt: "2026-02-30T01:00:00.000Z" },
    { nextReviewAt: "not-a-date" },
  ])
    assert.deepEqual(
      memory.normalizeMemorySchedule({ ...valid, ...patch }),
      memory.createMemorySchedule(),
    );
  const sanitized = memory.normalizeMemorySchedule({
    ...valid,
    history: [
      null,
      { at: "bad", nextReviewAt: "bad" },
      { ...valid.history[0], stage: 99 },
      { ...valid.history[0], at: iso(now + minute) },
      { ...valid.history[0], secret: "discard" },
    ],
    secret: "discard",
  });
  assert.deepEqual(sanitized, valid);
});

test("only bounded real review history persists after repeated sessions and refresh", () => {
  let state = memory.createMemorySchedule(),
    at = now;
  for (let index = 0; index < 70; index++) {
    state = memory.scheduleAnswer(state, index % 6 !== 0, at);
    at = Date.parse(state.nextReviewAt);
  }
  assert.equal(state.reviewCount, 70);
  assert.equal(state.lapseCount, 12);
  assert.equal(state.history.length, 32);
  assert.deepEqual(
    memory.normalizeMemorySchedule(JSON.parse(JSON.stringify(state))),
    state,
  );
  assert.equal(state.history.at(-1).at, state.lastReviewedAt);
});

test("built-in and personal plans survive device storage without changing device learning snapshots", () => {
  const current = data.createDemoData(data.devices[0]);
  const originalProgress = structuredClone(current.features.vocabulary);
  const builtin = byId(current.wordbooks, books.builtinBookId).words[0];
  current.wordbooks = books.addBookWords(current.wordbooks, books.savedBookId, [
    builtin,
  ]);
  current.wordbooks = books.answerBookWord(
    current.wordbooks,
    books.builtinBookId,
    builtin.word,
    true,
    now,
  );
  current.wordbooks = books.answerBookWord(
    current.wordbooks,
    books.savedBookId,
    builtin.word,
    false,
    now,
  );
  const saved = JSON.stringify(current);
  globalThis.localStorage = {
    getItem: (key) => (key === "binbot-ui-v1:binbot-001" ? saved : null),
  };
  const restored = data.loadDemoData(data.devices[0]);
  assert.equal(
    byId(restored.wordbooks, books.builtinBookId).words[0].review.nextReviewAt,
    iso(now + 20 * minute),
  );
  assert.equal(
    byId(restored.wordbooks, books.savedBookId).words[0].review.nextReviewAt,
    iso(now + 5 * minute),
  );
  assert.deepEqual(restored.features.vocabulary, originalProgress);
  assert.equal(
    byId(data.loadDemoData(data.devices[1]).wordbooks, books.builtinBookId)
      .words[0].review.reviewCount,
    0,
  );
  const csv = books.wordbookCsv(byId(restored.wordbooks, books.savedBookId));
  assert.ok(csv.includes("下次复习时间"));
  assert.ok(csv.includes(iso(now + 5 * minute)));
  delete globalThis.localStorage;
});

test("relative timing and answer labels describe the actual scheduled interval", () => {
  const state = memory.scheduleAnswer(memory.createMemorySchedule(), true, now);
  assert.equal(memory.formatReviewTime(state.nextReviewAt, now), "20 分钟后");
  assert.equal(
    memory.formatReviewTime(state.nextReviewAt, now + 20 * minute),
    "已到复习时间",
  );
  assert.equal(memory.formatReviewTime(null, now), "首次学习后安排");
  assert.equal(
    memory.answerIntervalLabel(memory.createMemorySchedule(), true, now),
    "20 分钟后再见",
  );
  assert.equal(memory.answerIntervalLabel(state, true, now), "保持原定时间");
  assert.equal(
    memory.answerIntervalLabel(state, true, now + 20 * minute),
    "1 小时后再见",
  );
  assert.equal(memory.answerIntervalLabel(state, false, now), "5 分钟后再学");
});

test("the curve is an explicit illustration while counts and per-word timing use saved schedules", () => {
  let state = books.addBookWords(books.createWordbooks(), books.savedBookId, [
    card("waiting"),
    card("newword"),
  ]);
  state = books.answerBookWord(state, books.savedBookId, "waiting", true, now);
  const book = byId(state, books.savedBookId);
  const html = renderToStaticMarkup(
    React.createElement(MemoryReviewPlan, { book, now }),
  );
  assert.ok(html.includes("艾宾浩斯复习计划"));
  assert.ok(html.includes("到期待复习"));
  assert.ok(html.includes("新词待学"));
  assert.ok(html.includes("不代表个人记忆测量"));
  assert.ok(!html.includes("准确率"));
  assert.ok(!html.includes("保持率 90"));
  const detail = renderToStaticMarkup(
    React.createElement(WordReviewTiming, { word: book.words[0], now }),
  );
  assert.ok(detail.includes("20 分钟后"));
  assert.ok(detail.includes("已复习 1 次"));
});

test("all waiting words disable an immediate repeat session and retain the next review time", () => {
  const at = Date.now();
  const current = data.createDemoData(data.devices[0]);
  current.wordbooks = books.addBookWords(current.wordbooks, books.savedBookId, [
    card(),
  ]);
  current.wordbooks = books.answerBookWord(
    current.wordbooks,
    books.savedBookId,
    "wander",
    true,
    at,
  );
  assert.deepEqual(
    books.reviewWords(byId(current.wordbooks, books.savedBookId), at),
    [],
  );
  const html = renderToStaticMarkup(
    React.createElement(WordbooksPanel, {
      device: data.devices[0],
      data: current,
      setData: () => {},
      notify: () => {},
    }),
  );
  assert.match(html, /disabled="">等待下次复习/);
  assert.ok(html.includes("20 分钟后"));
});
