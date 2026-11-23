import { after, test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createServer } from "vite";
import React from "react";
import { renderToStaticMarkup } from "react-dom/server";

const server = await createServer({
  cacheDir: "node_modules/.vite-noise-tests",
  logLevel: "error",
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  appType: "custom",
});
after(() => server.close());
const noise = await server.ssrLoadModule("/src/shared/model/whiteNoise.ts");
const { NoiseEngine } = await server.ssrLoadModule("/src/features/home/NoiseEngine.ts");
const features = await server.ssrLoadModule("/src/shared/model/deviceFeatures.ts");
const data = await server.ssrLoadModule("/src/shared/model/data.ts");
const { DeviceWorkspaceApp: App } = await server.ssrLoadModule("/src/app/App.tsx");

test("all nine sound profiles, four mixes and timers match the device App", async () => {
  const screen = await readFile(
    new URL(
      "../../binbot_app/apps/binbot_app/src/presentation/lvgl/screens/noise_screen.c",
      import.meta.url,
    ),
    "utf8",
  );
  const dsp = await readFile(
    new URL(
      "../../binbot_app/services/binbot_audio/src/noise.c",
      import.meta.url,
    ),
    "utf8",
  );
  assert.deepEqual(
    noise.noiseSounds.map((sound) => sound.name),
    [...screen.matchAll(/\{"([^"]+)", "[^"]+", "[^"]+", 0x/g)].map(
      (match) => match[1],
    ),
  );
  const presets = [
    ...screen.matchAll(/\{"([^"]+)", "[^"]+", \{([\d, ]+)\}\}/g),
  ].map((match) => ({
    name: match[1],
    levels: match[2].split(",").map(Number),
  }));
  assert.equal(presets.length, 4);
  assert.deepEqual(
    noise.noisePresets.map(({ name, levels }) => ({ name, levels })),
    presets,
  );
  const profiles = [
    ...dsp.matchAll(
      /\{(WHITE|PINK|BROWN), (LOWPASS|BANDPASS|HIGHPASS), ([^}]+)\}/g,
    ),
  ].map((match) => [
    match[1].toLowerCase(),
    match[2].toLowerCase(),
    ...match[3].split(",").map(parseFloat),
  ]);
  assert.equal(profiles.length, 9);
  assert.deepEqual(
    noise.noiseSounds.map((sound) => [
      sound.kind,
      sound.filter,
      sound.frequency,
      sound.q,
      sound.lfo,
      sound.depth,
    ]),
    profiles,
  );
  assert.deepEqual(
    noise.noiseTimers.map(({ seconds }) => seconds),
    screen
      .match(/durations\[\] = \{([^}]+)\}/)[1]
      .split(",")
      .map(Number),
  );
});

test("legacy mixes migrate silently, clamp invalid levels and remain isolated per device", () => {
  const first = features.normalizeDeviceFeatures(undefined, data.devices[0]);
  const second = features.normalizeDeviceFeatures(undefined, data.devices[1]);
  first.whiteNoise.levels.rain = 100;
  assert.equal(second.whiteNoise.levels.rain, 55);
  const settings = noise.normalizeWhiteNoiseSettings({
    levels: { rain: 999, waves: -1, fire: 2, cafe: null, unknown: 100 },
    master: 0,
    timerSeconds: 17,
    playing: true,
  });
  assert.equal(settings.levels.rain, 100);
  assert.equal(settings.levels.waves, 0);
  assert.equal(settings.levels.fire, 5);
  assert.equal(settings.levels.cafe, 35);
  assert.equal(settings.master, 0);
  assert.equal(settings.timerSeconds, 0);
  assert.equal(Object.keys(settings.levels).length, 9);
  assert.ok(!("playing" in settings));
  assert.deepEqual(
    noise.normalizeWhiteNoiseSettings({
      levels: [],
      master: NaN,
      timerSeconds: Infinity,
    }),
    noise.createWhiteNoiseSettings(),
  );
});

test("generated noise is finite and reproducible, with distinct white, pink and brown spectra", () => {
  const correlation = (samples) => {
    let product = 0,
      energy = 0,
      total = 0;
    for (let i = 1000; i < samples.length; i++) {
      assert.ok(Number.isFinite(samples[i]));
      total += samples[i];
      product += samples[i] * samples[i - 1];
      energy += samples[i] ** 2;
    }
    assert.ok(energy / samples.length > 0.0001);
    assert.ok(Math.abs(total / samples.length) < 0.15);
    return product / energy;
  };
  const white = noise.generateNoiseSamples("white", 24000, 123);
  assert.deepEqual(white, noise.generateNoiseSamples("white", 24000, 123));
  assert.notDeepEqual(white, noise.generateNoiseSamples("white", 24000, 124));
  const pink = correlation(noise.generateNoiseSamples("pink", 24000, 123));
  const brown = correlation(noise.generateNoiseSamples("brown", 24000, 123));
  assert.ok(Math.abs(correlation(white)) < 0.05);
  assert.ok(pink > 0.6 && brown > pink);
});

test("the sleep timer counts actual playback, preserves pauses and expires without interval drift", () => {
  const timer = new noise.NoiseCountdown(900);
  timer.start(1000);
  assert.equal(timer.remaining(1300), 900);
  timer.pause(11300);
  assert.equal(timer.remaining(100000), 890);
  timer.start(200000);
  timer.start(200100);
  assert.equal(timer.remaining(1089699), 1);
  assert.equal(timer.expired(1089700), true);
  timer.reset(1800);
  assert.equal(timer.remaining(2000000), 1800);
  timer.reset(0);
  timer.start(0);
  assert.equal(timer.expired(Number.MAX_SAFE_INTEGER), false);
});

class FakeParam {
  value = 0;
  events = [];
  setTargetAtTime(value, time, constant) {
    this.value = value;
    this.events.push(["target", value, time, constant]);
  }
  setValueAtTime(value, time) {
    this.events.push(["value", value, time]);
  }
  cancelScheduledValues(time) {
    this.events.push(["cancel", time]);
  }
}
class FakeNode {
  gain = new FakeParam();
  frequency = new FakeParam();
  Q = new FakeParam();
  threshold = new FakeParam();
  knee = new FakeParam();
  ratio = new FakeParam();
  attack = new FakeParam();
  release = new FakeParam();
  connections = [];
  stopped = false;
  disconnected = false;
  connect(target) {
    this.connections.push(target);
    return target;
  }
  disconnect() {
    this.disconnected = true;
  }
  start() {
    this.started = true;
  }
  stop() {
    this.stopped = true;
  }
}
class FakeContext {
  state = "suspended";
  currentTime = 0;
  sampleRate = 8000;
  destination = new FakeNode();
  gains = [];
  sources = [];
  oscillators = [];
  closed = 0;
  createGain() {
    const node = new FakeNode();
    this.gains.push(node);
    return node;
  }
  createDynamicsCompressor() {
    return new FakeNode();
  }
  createBiquadFilter() {
    return new FakeNode();
  }
  createBufferSource() {
    const node = new FakeNode();
    this.sources.push(node);
    return node;
  }
  createOscillator() {
    const node = new FakeNode();
    this.oscillators.push(node);
    return node;
  }
  createBuffer(_channels, length) {
    return { length, getChannelData: () => new Float32Array(length) };
  }
  async resume() {
    this.state = "running";
    this.onstatechange?.();
  }
  async suspend() {
    this.state = "suspended";
    this.onstatechange?.();
  }
  async close() {
    this.state = "closed";
    this.closed++;
    this.onstatechange?.();
  }
}

test("audition creates audio only on play and releases every source on exit", async () => {
  const context = new FakeContext();
  const states = [];
  let contexts = 0;
  const engine = new NoiseEngine(
    (playing) => states.push(playing),
    () => {
      contexts++;
      return context;
    },
  );
  const settings = noise.createWhiteNoiseSettings();
  engine.update(settings);
  assert.equal(contexts, 0);
  assert.equal(await engine.play(settings), true);
  assert.equal(context.sources.length, 9);
  assert.ok(context.sources.every((source) => source.started && source.loop));
  assert.deepEqual(states, [true]);
  engine.update({ ...settings, master: 0 });
  assert.equal(context.gains[0].gain.value, 0);
  engine.pause();
  assert.equal(engine.playing, false);
  assert.equal(context.state, "suspended");
  assert.deepEqual(states, [true, false]);
  await engine.play(settings);
  assert.equal(contexts, 1);
  engine.dispose();
  assert.equal(context.state, "closed");
  assert.equal(context.closed, 1);
  assert.ok(
    [...context.sources, ...context.oscillators].every(
      (source) => source.stopped && source.disconnected,
    ),
  );
  assert.equal(await engine.play(settings), false);
  engine.dispose();
  assert.equal(context.closed, 1);
});

test("the audio-clock stop survives volume adjustments and can be cancelled", async () => {
  const context = new FakeContext();
  const engine = new NoiseEngine(
    () => {},
    () => context,
  );
  const settings = noise.createWhiteNoiseSettings();
  await engine.play(settings);
  context.currentTime = 10;
  engine.setStopAfter(900);
  assert.deepEqual(context.gains[0].gain.events.at(-1), ["value", 0, 910]);
  context.currentTime = 15;
  engine.update({ ...settings, master: 70 });
  assert.deepEqual(context.gains[0].gain.events.at(-1), ["value", 0, 910]);
  engine.setStopAfter(null);
  assert.equal(context.gains[0].gain.events.at(-1)[0], "target");
  engine.dispose();
});

test("pausing or leaving during a pending resume cannot restart playback", async () => {
  for (const leave of [false, true]) {
    const context = new FakeContext();
    let resolveResume;
    const pending = new Promise((resolve) => {
      resolveResume = resolve;
    });
    context.resume = async () => {
      await pending;
      if (context.state === "closed") throw new Error("closed during resume");
      context.state = "running";
      context.onstatechange?.();
    };
    const states = [];
    const engine = new NoiseEngine(
      (playing) => states.push(playing),
      () => context,
    );
    const playing = engine.play(noise.createWhiteNoiseSettings());
    if (leave) engine.dispose();
    else engine.pause();
    resolveResume();
    assert.equal(await playing, false);
    assert.equal(engine.playing, false);
    assert.deepEqual(states, []);
    assert.equal(context.state, leave ? "closed" : "suspended");
    engine.dispose();
  }
});

test("blocked audio can be retried without leaking another context", async () => {
  const context = new FakeContext();
  const engine = new NoiseEngine(
    () => {},
    () => context,
  );
  const resume = context.resume;
  context.resume = async () => {
    throw new Error("audio blocked");
  };
  await assert.rejects(
    engine.play(noise.createWhiteNoiseSettings()),
    /audio blocked/,
  );
  assert.equal(engine.playing, false);
  context.resume = resume;
  assert.equal(await engine.play(noise.createWhiteNoiseSettings()), true);
  assert.equal(context.sources.length, 9);
  engine.dispose();
});

for (const device of data.devices) {
  test(`${device.id}/home embeds white noise in the device card, including the retired noise URL`, () => {
    for (const page of ["today", "noise"]) {
      globalThis.window = {
        location: { pathname: `/devices/${device.id}/${page}` },
      };
      globalThis.localStorage = { getItem: () => null };
      const html = renderToStaticMarkup(React.createElement(App));
      assert.ok(html.includes('class="home-noise"'));
      assert.ok(html.includes('id="white-noise"'));
      assert.ok(html.includes("本机试听"));
      assert.ok(html.includes("白噪音预设"));
      assert.ok(html.includes('aria-label="调整白噪音混音"'));
      assert.ok(html.includes('aria-label="白噪音定时关闭"'));
      assert.match(html, /aria-label="开始白噪音试听"/);
      assert.doesNotMatch(html, /disabled=""[^>]*aria-label="开始白噪音试听"/);
      assert.ok(!html.includes(`href="/devices/${device.id}/noise"`));
      assert.ok(!html.includes("noise-page"));
      assert.ok(!html.includes("home-music"));
      assert.ok(!html.includes("home-noise-sounds"));
      assert.ok(!html.includes("basic-controls"));
      assert.ok(html.includes("首页快捷控制"));
    }
  });
  test(`${device.id}/control keeps only screen and sound settings`, () => {
    globalThis.window = {
      location: { pathname: `/devices/${device.id}/control` },
    };
    globalThis.localStorage = { getItem: () => null };
    const html = renderToStaticMarkup(React.createElement(App));
    assert.ok(html.includes("basic-screen-card"));
    assert.ok(html.includes("basic-sound-card"));
    assert.ok(!html.includes("noise-player-card"));
    assert.ok(!html.includes("control-model"));
    assert.ok(!html.includes("camera-frame"));
    assert.ok(!html.includes('aria-label="播放音乐"'));
    if (!device.online) assert.match(html, /<fieldset disabled="">/);
  });
}
