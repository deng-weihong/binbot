export const noiseSounds = [
  {
    id: "rain",
    name: "雨声",
    english: "RAIN",
    kind: "white",
    filter: "bandpass",
    frequency: 2600,
    q: 0.45,
    lfo: 0,
    depth: 0,
  },
  {
    id: "waves",
    name: "海浪",
    english: "WAVES",
    kind: "pink",
    filter: "lowpass",
    frequency: 700,
    q: 0.707,
    lfo: 0.09,
    depth: 0.85,
  },
  {
    id: "fire",
    name: "篝火",
    english: "FIRE",
    kind: "brown",
    filter: "lowpass",
    frequency: 900,
    q: 0.707,
    lfo: 2.7,
    depth: 0.25,
  },
  {
    id: "forest",
    name: "森林",
    english: "FOREST",
    kind: "pink",
    filter: "bandpass",
    frequency: 1800,
    q: 0.9,
    lfo: 0.22,
    depth: 0.4,
  },
  {
    id: "cafe",
    name: "咖啡馆",
    english: "CAFÉ",
    kind: "pink",
    filter: "bandpass",
    frequency: 650,
    q: 0.7,
    lfo: 0.6,
    depth: 0.2,
  },
  {
    id: "wind",
    name: "风声",
    english: "WIND",
    kind: "pink",
    filter: "bandpass",
    frequency: 420,
    q: 1.6,
    lfo: 0.15,
    depth: 0.7,
  },
  {
    id: "storm",
    name: "雷雨",
    english: "STORM",
    kind: "brown",
    filter: "lowpass",
    frequency: 260,
    q: 0.707,
    lfo: 0.05,
    depth: 0.9,
  },
  {
    id: "stream",
    name: "溪流",
    english: "STREAM",
    kind: "white",
    filter: "bandpass",
    frequency: 1400,
    q: 0.8,
    lfo: 1.3,
    depth: 0.2,
  },
  {
    id: "typing",
    name: "键盘声",
    english: "TYPING",
    kind: "white",
    filter: "highpass",
    frequency: 3200,
    q: 0.707,
    lfo: 7,
    depth: 0.7,
  },
] as const;

export type NoiseSoundId = (typeof noiseSounds)[number]["id"];
export type NoiseKind = (typeof noiseSounds)[number]["kind"];
export type NoiseLevels = Record<NoiseSoundId, number>;
export const noisePresets = [
  {
    id: "focus",
    name: "专注",
    description: "雨落窗边，思绪慢慢清晰。",
    levels: [55, 0, 0, 0, 35, 0, 0, 0, 0],
  },
  {
    id: "sleep",
    name: "助眠",
    description: "伴着海风，让今天轻轻落下。",
    levels: [0, 60, 0, 0, 0, 25, 0, 0, 0],
  },
  {
    id: "relax",
    name: "放松",
    description: "走进林间，听见溪流经过。",
    levels: [0, 0, 0, 55, 0, 0, 0, 40, 0],
  },
  {
    id: "night",
    name: "雨夜",
    description: "窗外有雨，身边有一点暖意。",
    levels: [60, 0, 30, 0, 0, 0, 45, 0, 0],
  },
] as const;
export const noiseTimers = [
  { seconds: 900, label: "15 分钟" },
  { seconds: 1800, label: "30 分钟" },
  { seconds: 3600, label: "60 分钟" },
  { seconds: 0, label: "不定时" },
] as const;
export type NoiseTimer = number;
export type WhiteNoiseSettings = {
  levels: NoiseLevels;
  master: number;
  timerSeconds: NoiseTimer;
};

export function levelsFromPreset(levels: readonly number[]): NoiseLevels {
  return Object.fromEntries(
    noiseSounds.map(({ id }, index) => [id, levels[index] ?? 0]),
  ) as NoiseLevels;
}

export function createWhiteNoiseSettings(): WhiteNoiseSettings {
  return {
    levels: levelsFromPreset(noisePresets[0].levels),
    master: 40,
    timerSeconds: 0,
  };
}

const object = (value: unknown): Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
const percent = (value: unknown, fallback: number) =>
  Math.max(
    0,
    Math.min(
      100,
      typeof value === "number" && Number.isFinite(value)
        ? Math.trunc(value)
        : fallback,
    ),
  );

/** Saved mixes never include a playing flag; loading the page stays silent. */
export function normalizeWhiteNoiseSettings(
  value: unknown,
): WhiteNoiseSettings {
  const defaults = createWhiteNoiseSettings();
  const source = object(value);
  const levels = object(source.levels);
  return {
    levels: Object.fromEntries(
      noiseSounds.map(({ id }) => {
        const level = percent(levels[id], defaults.levels[id]);
        return [id, level > 0 ? Math.max(5, level) : 0];
      }),
    ) as NoiseLevels,
    master: percent(source.master, defaults.master),
    timerSeconds: noiseTimers.some(
      ({ seconds }) => seconds === source.timerSeconds,
    )
      ? (source.timerSeconds as NoiseTimer)
      : defaults.timerSeconds,
  };
}

/** Same seeded noise generators as the device; filtering happens in Web Audio. */
export function generateNoiseSamples(
  kind: NoiseKind,
  length: number,
  seed: number,
): Float32Array {
  const samples = new Float32Array(length);
  let random = seed | 0 || 1;
  let pink0 = 0,
    pink1 = 0,
    pink2 = 0,
    brown = 0;
  for (let i = 0; i < samples.length; i++) {
    random ^= random << 13;
    random ^= random >>> 17;
    random ^= random << 5;
    const white = (random >>> 8) / 8388608 - 1;
    if (kind === "pink") {
      pink0 = 0.99765 * pink0 + white * 0.099046;
      pink1 = 0.963 * pink1 + white * 0.2965164;
      pink2 = 0.57 * pink2 + white * 1.0526913;
      samples[i] = (pink0 + pink1 + pink2 + white * 0.1848) * 0.16;
    } else if (kind === "brown") {
      brown = (brown + 0.02 * white) / 1.02;
      samples[i] = brown * 3.2;
    } else samples[i] = white * 0.5;
  }
  return samples;
}

/** A monotonic clock avoids timer drift and preserves time while paused. */
export class NoiseCountdown {
  private duration = 0;
  private remainingMs = 0;
  private startedAt: number | null = null;

  constructor(seconds: number) {
    this.reset(seconds);
  }
  get durationSeconds() {
    return this.duration / 1000;
  }
  reset(seconds: number) {
    this.duration = seconds * 1000;
    this.remainingMs = this.duration;
    this.startedAt = null;
  }
  start(now: number) {
    if (this.startedAt === null && this.duration > 0) this.startedAt = now;
  }
  pause(now: number) {
    this.remainingMs = this.left(now);
    this.startedAt = null;
  }
  private left(now: number) {
    return Math.max(
      0,
      this.remainingMs -
        (this.startedAt === null ? 0 : Math.max(0, now - this.startedAt)),
    );
  }
  remaining(now: number) {
    return Math.ceil(this.left(now) / 1000);
  }
  expired(now: number) {
    return this.duration > 0 && this.left(now) === 0;
  }
}
