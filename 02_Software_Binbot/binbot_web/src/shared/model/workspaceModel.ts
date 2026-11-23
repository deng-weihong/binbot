import { noiseSounds, type NoiseLevels } from "./whiteNoise";
import { createDemoData, type Device, type DeviceData } from "./data";
import { normalizeDeviceFeatures } from "./deviceFeatures";
import {
  normalizeWordbooks,
  wordbookChanges,
  type Wordbooks,
} from "./wordbooks";
import { normalizeTodos } from "./todos";
import { normalizeSchedules, type Schedule } from "./schedules";

export const record = (value: unknown): Record<string, unknown> =>
  value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
export const equal = (a: unknown, b: unknown) =>
  JSON.stringify(a) === JSON.stringify(b);
export type WorkspaceSnapshot = {
  device_id: string;
  revision: string;
  wordbooks_revision?: string;
  data: Record<string, unknown>;
  reported: Record<string, unknown>;
  system: Record<string, unknown>;
  calendar: (Schedule & { source?: string })[];
  calendar_sync?: {
    enabled?: boolean;
    authorized?: boolean;
    polling?: boolean;
    poll_interval_seconds?: number;
    last_sync_at?: string;
    error?: string;
  };
  control: {
    ready?: boolean;
    brightness?: number;
    volume?: number;
    boot_id?: string;
  };
  capabilities: string[];
  server_time: string;
};
export function parseWorkspace(
  value: unknown,
  deviceId: string,
): WorkspaceSnapshot {
  const row = record(value);
  if (
    row.device_id !== deviceId ||
    typeof row.revision !== "string" ||
    !row.revision ||
    !row.data ||
    typeof row.data !== "object"
  )
    throw new Error("工作台数据与当前设备不一致。");
  return {
    device_id: deviceId,
    revision: row.revision,
    wordbooks_revision:
      typeof row.wordbooks_revision === "string"
        ? row.wordbooks_revision
        : undefined,
    data: record(row.data),
    reported: record(row.reported),
    system: record(row.system),
    calendar_sync: record(row.calendar_sync),
    control: record(row.control),
    calendar: Array.isArray(row.calendar)
      ? (row.calendar as WorkspaceSnapshot["calendar"])
      : [],
    capabilities: Array.isArray(row.capabilities)
      ? row.capabilities.filter((s): s is string => typeof s === "string")
      : [],
    server_time: typeof row.server_time === "string" ? row.server_time : "",
  };
}
export function emptyLiveData(
  device: Device,
  wordbooks?: Wordbooks,
): DeviceData {
  const base = createDemoData(device, wordbooks);
  base.schedules = [];
  base.todos = [];
  base.assistant = {
    name: device.name,
    voice: "",
    style: "简洁清晰",
    instruction: "",
    memories: [],
  };
  base.features.vocabulary = {
    knownWords: [],
    currentWord: "",
    reviewedCount: 0,
    correctCount: 0,
    cyclesCompleted: 0,
  };
  base.features.modules.connected = null;
  return base;
}
export function workspaceData(
  snapshot: WorkspaceSnapshot,
  device: Device,
  cachedWordbooks?: Wordbooks,
): DeviceData {
  const wordbooks =
    cachedWordbooks || normalizeWordbooks(snapshot.data.wordbooks);
  const base = emptyLiveData(device, wordbooks),
    prefs = snapshot.data,
    reported = snapshot.reported;
  const features = normalizeDeviceFeatures(
    { ...base.features, ...reported },
    device,
  );
  for (const key of ["pen", "holo"] as const) {
    const id = record(record(reported.modules)[key]).imageId;
    features.modules[key].imageId =
      typeof id === "string" && (!id || /^[a-f0-9-]{36}$/.test(id)) ? id : "";
  }
  const noise = record(reported.whiteNoise),
    levels = record(noise.levels);
  if (typeof noise.master === "number")
    features.whiteNoise = {
      master: noise.master,
      levels: Object.fromEntries(
        noiseSounds.map(({ id }) => [id, Number(levels[id] || 0)]),
      ) as NoiseLevels,
      timerSeconds: Number(noise.timerSeconds || 0),
    };
  const words = wordbooks.books.flatMap((book) => book.words);
  const legacy = record(prefs.legacy_learning_statistics);
  features.vocabulary = {
    knownWords: [
      ...new Set(
        words.filter((word) => word.remembered).map((word) => word.word),
      ),
    ],
    currentWord: String(record(reported.vocabulary).currentWord || ""),
    reviewedCount:
      Number(legacy.reviewed || 0) +
      words.reduce((n, word) => n + word.review.reviewCount, 0),
    correctCount:
      Number(legacy.correct || 0) +
      words.reduce(
        (n, word) => n + word.review.reviewCount - word.review.lapseCount,
        0,
      ),
    cyclesCompleted: Math.max(
      Number(legacy.cycles || 0),
      Number(record(reported.vocabulary).cyclesCompleted || 0),
    ),
  };
  return {
    ...base,
    features,
    wordbooks,
    timezone:
      typeof prefs.timezone === "string" ? prefs.timezone : "Asia/Shanghai",
    deviceName:
      typeof prefs.deviceName === "string" ? prefs.deviceName : device.name,
    assistant: { ...base.assistant, ...record(prefs.assistant) },
    schedules: normalizeSchedules(prefs.schedules, []),
    todos: normalizeTodos(reported.todos, []),
    microphone: reported.microphone === true,
    notifications:
      typeof reported.notifications === "boolean"
        ? reported.notifications
        : prefs.notifications !== false,
    brightness: Number(snapshot.control.brightness ?? 0),
    volume: Number(snapshot.control.volume ?? 0),
  };
}

export function mergeWorkspaceSnapshot(
  next: WorkspaceSnapshot,
  previous: WorkspaceSnapshot | null,
): WorkspaceSnapshot {
  if ("wordbooks" in next.data) return next;
  if (
    !previous ||
    previous.device_id !== next.device_id ||
    !next.wordbooks_revision ||
    next.wordbooks_revision !== previous.wordbooks_revision ||
    !("wordbooks" in previous.data)
  )
    throw new Error("词本缓存已变化，请重新读取工作台。");
  return {
    ...next,
    data: { ...next.data, wordbooks: previous.data.wordbooks },
  };
}

export function workspaceRequestPatch(
  previous: Wordbooks,
  patch: Record<string, unknown>,
) {
  if (!patch.wordbooks) return patch;
  const { wordbooks, ...small } = patch;
  const changes = wordbookChanges(previous, wordbooks as Wordbooks);
  if (Object.keys(changes).length) small.wordbookChanges = changes;
  return small;
}
/** Only writable changes are sent. Runtime counters and connection reports are never written back. */
export function workspacePatch(
  previous: DeviceData,
  next: DeviceData,
): Record<string, unknown> {
  const patch: Record<string, unknown> = {};
  for (const key of [
    "deviceName",
    "timezone",
    "assistant",
    "schedules",
    "todos",
    "wordbooks",
    "microphone",
    "notifications",
  ] as const)
    if (previous[key] !== next[key] && !equal(previous[key], next[key]))
      patch[key] = next[key];
  const features: Record<string, unknown> = {};
  for (const key of ["watchFace", "gestures"] as const)
    if (!equal(previous.features[key], next.features[key]))
      features[key] = next.features[key];
  const modules: Record<string, unknown> = {};
  for (const key of ["holo", "pen", "turntable", "light"] as const)
    if (!equal(previous.features.modules[key], next.features.modules[key]))
      modules[key] = next.features.modules[key];
  if (Object.keys(modules).length) features.modules = modules;
  if (Object.keys(features).length) patch.features = features;
  return patch;
}
export function mergePatch(
  base: Record<string, unknown>,
  patch: Record<string, unknown>,
): Record<string, unknown> {
  const result = { ...base, ...patch };
  if (base.features && patch.features) {
    result.features = { ...record(base.features), ...record(patch.features) };
    if (record(base.features).modules && record(patch.features).modules)
      (result.features as Record<string, unknown>).modules = {
        ...record(record(base.features).modules),
        ...record(record(patch.features).modules),
      };
  }
  return result;
}
