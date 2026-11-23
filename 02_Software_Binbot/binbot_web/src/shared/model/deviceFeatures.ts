import vocabularyCatalog from "./catalogs/vocabulary.json";
import faceCatalog from "./catalogs/watchFaces.json";
import {
  createWhiteNoiseSettings,
  normalizeWhiteNoiseSettings,
  type WhiteNoiseSettings,
} from "./whiteNoise";

export const vocabularyCards = vocabularyCatalog;
export const watchFaces = faceCatalog;
export const idleOptions = [
  { value: 30, label: "30 秒" },
  { value: 60, label: "1 分钟" },
  { value: 300, label: "5 分钟" },
  { value: 600, label: "10 分钟" },
  { value: 0, label: "不自动进入" },
];
export const rotationOptions = [
  { value: 0, label: "不轮换" },
  { value: 10, label: "10 秒" },
  { value: 60, label: "1 分钟" },
  { value: 300, label: "5 分钟" },
  { value: 1800, label: "30 分钟" },
];
export const gestureActions = [
  { id: "pc-on", name: "打开电脑", available: false },
  { id: "pc-sleep", name: "电脑休眠", available: false },
  { id: "lamp", name: "开关台灯", available: false },
  { id: "play", name: "播放 / 暂停", available: true },
  { id: "next", name: "下一首", available: true },
  { id: "previous", name: "上一首", available: true },
  { id: "photo", name: "拍照", available: true },
  { id: "focus", name: "开始专注", available: true },
  { id: "wake", name: "唤醒小伴", available: true },
  { id: "noise", name: "白噪音", available: true },
  { id: "mute", name: "静音", available: true },
  { id: "nothing", name: "不执行", available: true },
] as const;
export const gestureCatalog = [
  { id: "ok", name: "OK", description: "拇指与食指围成圆圈，其余三指伸直。" },
  { id: "thumb", name: "点赞", description: "握拳并竖起大拇指。" },
  {
    id: "palm",
    name: "张开手掌",
    description: "五指张开，掌心朝向屏幕停留 1 秒。",
  },
  { id: "victory", name: "比耶", description: "伸出食指和中指，比出 V 字。" },
  { id: "fist", name: "握拳", description: "五指收拢握拳，停留 1 秒。" },
] as const;
export const moduleCatalog = [
  {
    id: "holo",
    name: "全息屏",
    english: "HOLO DISPLAY",
    description: "让喜欢的画面，浮现在桌面。",
    profile: 2,
  },
  {
    id: "pen",
    name: "笔筒",
    english: "PEN CUP",
    description: "收好灵感，也留意时间与天气。",
    profile: 1,
  },
  {
    id: "turntable",
    name: "转盘",
    english: "TURNTABLE",
    description: "让桌面的小物，按你的节奏转动。",
    profile: 3,
  },
  {
    id: "light",
    name: "氛围灯",
    english: "AMBIENT LIGHT",
    description: "为阅读、专注和休息，留一束光。",
    profile: 5,
  },
] as const;
export const moduleImages = [
  { id: "happy", name: "开心小猫", src: "/module-media/cat_happy.gif" },
  { id: "sleepy", name: "安静小猫", src: "/module-media/cat_sad.gif" },
  { id: "music", name: "音乐时光", src: "/module-media/music_cover.png" },
] as const;
export const lightEffects = ["常亮", "呼吸", "流光", "闪烁", "心跳"] as const;
export const lightColors = [
  "#e8643c",
  "#f6b26b",
  "#ffd166",
  "#7ed38f",
  "#4cc9f0",
  "#7b5cff",
  "#ff5fa2",
  "#ffffff",
];
export const lightPresets = [
  { name: "阅读", color: "#ffd166", effect: "常亮", brightness: 80, speed: 50 },
  { name: "专注", color: "#4cc9f0", effect: "呼吸", brightness: 60, speed: 50 },
  { name: "助眠", color: "#7b5cff", effect: "呼吸", brightness: 20, speed: 30 },
  { name: "派对", color: "#ff5fa2", effect: "流光", brightness: 80, speed: 75 },
] as const;

export type GestureId = (typeof gestureCatalog)[number]["id"];
export type GestureAction = (typeof gestureActions)[number]["id"];
export type ModuleId = (typeof moduleCatalog)[number]["id"];
export type WatchFaceSettings = {
  themeId: string;
  standbyId: string;
  idleSeconds: number;
  shuffle: boolean;
};
export type GestureSettings = {
  enabled: boolean;
  confirmSensitive: boolean;
  sensitivity: "low" | "medium" | "high";
  mappings: Record<GestureId, { enabled: boolean; action: GestureAction }>;
};
export type ImageSettings = { imageId: string; intervalSeconds: number };
export type ModuleSettings = {
  connected: ModuleId | null;
  holo: ImageSettings;
  pen: ImageSettings & {
    mode: "image" | "info";
    face: "both" | "time" | "weather";
  };
  turntable: {
    direction: "stop" | "clockwise" | "counterclockwise";
    speed: number;
  };
  light: {
    enabled: boolean;
    effect: (typeof lightEffects)[number];
    color: string;
    brightness: number;
    speed: number;
  };
};
export type VocabularyProgress = {
  knownWords: string[];
  currentWord: string;
  reviewedCount: number;
  correctCount: number;
  cyclesCompleted: number;
};
export type DeviceFeatures = {
  whiteNoise: WhiteNoiseSettings;
  vocabulary: VocabularyProgress;
  watchFace: WatchFaceSettings;
  gestures: GestureSettings;
  modules: ModuleSettings;
};

export function createDeviceFeatures(device: { id: string }): DeviceFeatures {
  const known =
    device.id === "binbot-001" ? 18 : device.id === "binbot-002" ? 8 : 5;
  return {
    whiteNoise: createWhiteNoiseSettings(),
    vocabulary: {
      knownWords: vocabularyCards.slice(0, known).map((card) => card.word),
      currentWord: vocabularyCards[known].word,
      reviewedCount: known + 9,
      correctCount: known + 2,
      cyclesCompleted: device.id === "binbot-001" ? 1 : 0,
    },
    watchFace: {
      themeId: device.id === "binbot-002" ? "little-grove" : "napping-cat",
      standbyId: device.id === "binbot-002" ? "little-grove" : "napping-cat",
      idleSeconds: 60,
      shuffle: false,
    },
    gestures: {
      enabled: true,
      confirmSensitive: true,
      sensitivity: "medium",
      mappings: {
        ok: { enabled: true, action: "pc-on" },
        thumb: { enabled: true, action: "play" },
        palm: { enabled: false, action: "play" },
        victory: { enabled: true, action: "photo" },
        fist: { enabled: true, action: "lamp" },
      },
    },
    modules: {
      connected:
        device.id === "binbot-002"
          ? "light"
          : device.id === "binbot-003"
            ? null
            : "pen",
      holo: { imageId: "happy", intervalSeconds: 0 },
      pen: { mode: "info", face: "both", imageId: "happy", intervalSeconds: 0 },
      turntable: { direction: "stop", speed: 40 },
      light: {
        enabled: true,
        effect: "常亮",
        color: "#ffd166",
        brightness: 80,
        speed: 50,
      },
    },
  };
}

export function selectWatchFace(
  settings: WatchFaceSettings,
  themeId: string,
): WatchFaceSettings {
  if (!watchFaces.some((face) => face.id === themeId)) return settings;
  return {
    ...settings,
    themeId,
    standbyId: themeId === "face" ? settings.standbyId : themeId,
    shuffle: false,
  };
}

export function setWatchFaceShuffle(
  settings: WatchFaceSettings,
  shuffle: boolean,
): WatchFaceSettings {
  return {
    ...settings,
    shuffle,
    themeId:
      shuffle && settings.themeId === "face"
        ? settings.standbyId
        : settings.themeId,
  };
}

const object = (value: unknown): Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
const bool = (value: unknown, fallback: boolean) =>
  typeof value === "boolean" ? value : fallback;
const bounded = (value: unknown, fallback: number, min: number, max: number) =>
  Math.min(
    max,
    Math.max(
      min,
      Math.trunc(
        typeof value === "number" && Number.isFinite(value) ? value : fallback,
      ),
    ),
  );
function choice<T extends string | number>(
  value: unknown,
  choices: readonly T[],
  fallback: T,
): T {
  return choices.includes(value as T) ? (value as T) : fallback;
}

/** Migrate earlier local prototypes and discard invalid device settings. */
export function normalizeDeviceFeatures(
  value: unknown,
  device: { id: string },
): DeviceFeatures {
  const defaults = createDeviceFeatures(device);
  const source = object(value);
  const vocabulary = object(source.vocabulary);
  const storedKnownWords = vocabulary.knownWords;
  const knownWords = Array.isArray(storedKnownWords)
    ? vocabularyCards
        .filter((card) => storedKnownWords.includes(card.word))
        .map((card) => card.word)
    : defaults.vocabulary.knownWords;
  const reviewedCount = bounded(
    vocabulary.reviewedCount,
    defaults.vocabulary.reviewedCount,
    knownWords.length,
    65535,
  );
  const watchFace = object(source.watchFace);
  const faceIds = watchFaces.map((face) => face.id);
  const themeId = choice(
    watchFace.themeId,
    faceIds,
    defaults.watchFace.themeId,
  );
  const gestures = object(source.gestures);
  const mappings = object(gestures.mappings);
  const modules = object(source.modules);
  const pen = object(modules.pen);
  const turntable = object(modules.turntable);
  const light = object(modules.light);
  const image = (raw: unknown, fallback: ImageSettings): ImageSettings => {
    const setting = object(raw);
    return {
      imageId: choice(
        setting.imageId,
        moduleImages.map((item) => item.id),
        fallback.imageId,
      ),
      intervalSeconds: choice(
        setting.intervalSeconds,
        rotationOptions.map((item) => item.value),
        fallback.intervalSeconds,
      ),
    };
  };
  return {
    whiteNoise: normalizeWhiteNoiseSettings(source.whiteNoise),
    vocabulary: {
      knownWords,
      currentWord: choice(
        vocabulary.currentWord,
        vocabularyCards.map((card) => card.word),
        defaults.vocabulary.currentWord,
      ),
      reviewedCount,
      correctCount: bounded(
        vocabulary.correctCount,
        Math.min(defaults.vocabulary.correctCount, reviewedCount),
        knownWords.length,
        reviewedCount,
      ),
      cyclesCompleted: bounded(
        vocabulary.cyclesCompleted,
        defaults.vocabulary.cyclesCompleted,
        0,
        65535,
      ),
    },
    watchFace: setWatchFaceShuffle(
      {
        themeId,
        standbyId: choice(
          watchFace.standbyId,
          faceIds.filter((id) => id !== "face"),
          themeId === "face" ? defaults.watchFace.standbyId : themeId,
        ),
        idleSeconds: choice(
          watchFace.idleSeconds,
          idleOptions.map((option) => option.value),
          60,
        ),
        shuffle: false,
      },
      bool(watchFace.shuffle, false),
    ),
    gestures: {
      enabled: bool(gestures.enabled, defaults.gestures.enabled),
      confirmSensitive: bool(
        gestures.confirmSensitive,
        defaults.gestures.confirmSensitive,
      ),
      sensitivity: choice(
        gestures.sensitivity,
        ["low", "medium", "high"] as const,
        "medium",
      ),
      mappings: Object.fromEntries(
        gestureCatalog.map(({ id }) => {
          const mapping = object(mappings[id]);
          return [
            id,
            {
              enabled: bool(
                mapping.enabled,
                defaults.gestures.mappings[id].enabled,
              ),
              action: choice(
                mapping.action,
                gestureActions.map((action) => action.id),
                defaults.gestures.mappings[id].action,
              ),
            },
          ];
        }),
      ) as GestureSettings["mappings"],
    },
    modules: {
      connected:
        modules.connected === null
          ? null
          : moduleCatalog.some((module) => module.id === modules.connected)
            ? (modules.connected as ModuleId)
            : defaults.modules.connected,
      holo: image(modules.holo, defaults.modules.holo),
      pen: {
        ...image(pen, defaults.modules.pen),
        mode: choice(pen.mode, ["image", "info"] as const, "info"),
        face: choice(pen.face, ["both", "time", "weather"] as const, "both"),
      },
      turntable: {
        direction: choice(
          turntable.direction,
          ["stop", "clockwise", "counterclockwise"] as const,
          "stop",
        ),
        speed: bounded(turntable.speed, 40, 0, 100),
      },
      light: {
        enabled: bool(light.enabled, true),
        effect: choice(light.effect, lightEffects, "常亮"),
        color: choice(light.color, lightColors, "#ffd166"),
        brightness: bounded(light.brightness, 80, 0, 100),
        speed: bounded(light.speed, 50, 5, 100),
      },
    },
  };
}
