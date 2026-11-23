import {
  createDeviceFeatures,
  normalizeDeviceFeatures,
  type DeviceFeatures,
} from "./deviceFeatures";
import {
  createDemoSchedules,
  normalizeSchedules,
  type Schedule,
} from "./schedules";
import {
  createServicePreferences,
  normalizeServicePreferences,
  type ServicePreferences,
} from "./serviceConfiguration";
import { createDemoTodos, normalizeTodos, type Todo } from "./todos";
import {
  createWordbooks,
  normalizeWordbooks,
  type Wordbooks,
} from "./wordbooks";

export type Device = {
  id: string;
  name: string;
  location: string;
  online: boolean;
  color: "lilac" | "mint" | "peach";
  remoteIp?: string;
  connectedAt?: string;
  lastSeenAt?: string;
  capabilities?: string[];
  controlOnline?: boolean;
  controlReady?: boolean;
  voiceOnline?: boolean;
};

export const devices: Device[] = [
  {
    id: "binbot-001",
    name: "小白",
    location: "书房",
    online: true,
    color: "lilac",
  },
  {
    id: "binbot-002",
    name: "小青",
    location: "客厅",
    online: true,
    color: "mint",
  },
  {
    id: "binbot-003",
    name: "小眠",
    location: "卧室",
    online: false,
    color: "peach",
  },
];
export type Page =
  | "today"
  | "assistant"
  | "control"
  | "vocabulary"
  | "watchface"
  | "gestures"
  | "modules"
  | "settings";
export type Assistant = {
  name: string;
  voice: string;
  style: string;
  instruction: string;
  memories: string[];
};
export type DeviceData = {
  timezone?: string;
  features: DeviceFeatures;
  deviceName: string;
  assistant: Assistant;
  schedules: Schedule[];
  todos: Todo[];
  wordbooks: Wordbooks;
  services: ServicePreferences;
  brightness: number;
  volume: number;
  microphone: boolean;
  notifications: boolean;
};

export const uid = () => {
  if (typeof crypto.randomUUID === "function") return crypto.randomUUID();
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  const hex = Array.from(bytes, (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
};

export function createDemoData(device: Device, wordbooks?: Wordbooks): DeviceData {
  const name = device.name;
  return {
    features: createDeviceFeatures(device),
    deviceName: name,
    assistant: {
      name,
      voice: "温柔自然",
      style: "简洁清晰",
      instruction: "先给我结论，再补充必要的细节。提醒的时候，轻松一点就好。",
      memories:
        device.id === "binbot-002"
          ? ["喜欢在下午听轻音乐。", "周末通常会在家看书。"]
          : [
              "工作日早上喜欢先整理当天的待办。",
              "专注的时候，喜欢没有歌词的音乐。",
              "提醒我每隔一小时起来走走。",
            ],
    },
    schedules: createDemoSchedules(device),
    todos: createDemoTodos(device),
    wordbooks: wordbooks || createWordbooks(),
    services: createServicePreferences(),
    brightness: 72,
    volume: 40,
    microphone: true,
    notifications: true,
  };
}

export function workspaceStorageKey(
  deviceId: string,
  mode: "demo" | "live" = "demo",
) {
  return mode === "demo"
    ? `binbot-ui-v1:${deviceId}`
    : `binbot-ui-v1:live:${deviceId}`;
}

export function loadDemoData(
  device: Device,
  mode: "demo" | "live" = "demo",
): DeviceData {
  const defaults = createDemoData(device);
  try {
    const value = JSON.parse(
      localStorage.getItem(workspaceStorageKey(device.id, mode)) || "null",
    ) as DeviceData | null;
    if (value && value.assistant && Array.isArray(value.assistant.memories)) {
      const preferences = { ...value };
      // Retire the local activity feed without losing saved device preferences.
      Reflect.deleteProperty(preferences, "activities");
      return {
        ...defaults,
        ...preferences,
        schedules: normalizeSchedules(value.schedules, defaults.schedules),
        todos: normalizeTodos(value.todos, defaults.todos),
        wordbooks: normalizeWordbooks(value.wordbooks),
        services: normalizeServicePreferences(value.services),
        // Retain personal preferences while retiring legacy capability permissions.
        assistant: {
          name:
            typeof value.assistant.name === "string"
              ? value.assistant.name
              : defaults.assistant.name,
          voice:
            typeof value.assistant.voice === "string"
              ? value.assistant.voice
              : defaults.assistant.voice,
          style:
            typeof value.assistant.style === "string"
              ? value.assistant.style
              : defaults.assistant.style,
          instruction:
            typeof value.assistant.instruction === "string"
              ? value.assistant.instruction
              : defaults.assistant.instruction,
          memories: value.assistant.memories,
        },
        features: normalizeDeviceFeatures(value.features, device),
      };
    }
  } catch {
    /* A blocked or malformed local store should not prevent the prototype from opening. */
  }
  return defaults;
}
