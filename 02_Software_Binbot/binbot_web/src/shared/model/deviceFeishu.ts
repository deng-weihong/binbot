export type FeishuConfig = {
  enabled: boolean;
  app_id: string;
  calendar_id: string;
  calendar_enabled: boolean;
  messages_enabled: boolean;
  chat_id: string;
  allow_control: boolean;
  mode: "long_connection" | "webhook";
};
export type FeishuSecrets = {
  app_secret?: string;
  verification_token?: string;
};
export type DeviceFeishuSnapshot = {
  device_id: string;
  revision: string;
  config: FeishuConfig;
  credentials_configured: { app_secret: boolean; verification_token: boolean };
  account: { authorized: boolean; display_name: string; expires_at?: string };
  connected: boolean;
  error: string;
  calendar: {
    calendar_id: string;
    last_sync_at: string;
    event_count: number;
    poll_interval_seconds?: number;
    polling?: boolean;
    error?: string;
  } | null;
  callback_path: string;
};
export const defaultFeishuConfig = (): FeishuConfig => ({
  enabled: false,
  app_id: "",
  calendar_id: "",
  calendar_enabled: false,
  messages_enabled: false,
  chat_id: "",
  allow_control: false,
  mode: "long_connection",
});
export function normalizeFeishuDraft(value: unknown): FeishuConfig {
  const row =
    value && typeof value === "object"
      ? (value as Record<string, unknown>)
      : {};
  const defaults = defaultFeishuConfig();
  return Object.fromEntries(
    Object.entries(defaults).map(([key, fallback]) => [
      key,
      typeof fallback === "boolean"
        ? row[key] === true
        : key === "mode"
          ? row[key] === "webhook"
            ? "webhook"
            : "long_connection"
          : typeof row[key] === "string"
            ? row[key].trim().slice(0, 256)
            : fallback,
    ]),
  ) as FeishuConfig;
}
export function feishuDraftKey(mode: "live" | "demo", id: string) {
  return `binbot-device-feishu-v1:${mode}:${id}`;
}
export function readFeishuDraft(
  mode: "live" | "demo",
  id: string,
): FeishuConfig {
  try {
    const saved = localStorage.getItem(feishuDraftKey(mode, id));
    if (saved) return normalizeFeishuDraft(JSON.parse(saved));
    // Only migrate the current device's public draft; never copy a global grant.
    const legacy = JSON.parse(
      localStorage.getItem(
        mode === "live" ? `binbot-ui-v1:live:${id}` : `binbot-ui-v1:${id}`,
      ) || "null",
    )?.services?.feishu;
    return normalizeFeishuDraft({
      app_id: legacy?.appId,
      calendar_id: legacy?.calendarId,
    });
  } catch {
    return defaultFeishuConfig();
  }
}
export function parseFeishuSnapshot(
  value: unknown,
  id: string,
): DeviceFeishuSnapshot {
  const row = value as DeviceFeishuSnapshot | null;
  const defaults = defaultFeishuConfig();
  if (
    !row ||
    row.device_id !== id ||
    typeof row.revision !== "string" ||
    !row.revision ||
    !row.config ||
    !Object.entries(defaults).every(
      ([key, fallback]) =>
        typeof row.config[key as keyof FeishuConfig] === typeof fallback,
    ) ||
    !["long_connection", "webhook"].includes(row.config.mode) ||
    !row.credentials_configured ||
    typeof row.credentials_configured.app_secret !== "boolean" ||
    !row.account ||
    typeof row.account.authorized !== "boolean" ||
    typeof row.account.display_name !== "string"
  )
    throw new Error("当前设备的飞书配置返回异常，请重新读取。");
  return {
    device_id: row.device_id,
    revision: row.revision,
    config: normalizeFeishuDraft(row.config),
    credentials_configured: {
      app_secret: row.credentials_configured.app_secret,
      verification_token:
        row.credentials_configured.verification_token === true,
    },
    account: {
      authorized: row.account.authorized,
      display_name: row.account.display_name,
      expires_at: row.account.expires_at,
    },
    connected: row.connected === true,
    error: typeof row.error === "string" ? row.error : "",
    calendar:
      row.calendar &&
      typeof row.calendar.calendar_id === "string" &&
      typeof row.calendar.event_count === "number" &&
      typeof row.calendar.last_sync_at === "string"
        ? row.calendar
        : null,
    callback_path: "/api/v1/feishu/oauth/callback",
  };
}

export function feishuSaveBody(
  snapshot: DeviceFeishuSnapshot,
  config: FeishuConfig,
  secrets: FeishuSecrets,
) {
  return {
    revision: snapshot.revision,
    config: normalizeFeishuDraft(config),
    secrets: Object.fromEntries(
      (["app_secret", "verification_token"] as const)
        .filter((key) => !!secrets[key]?.trim())
        .map((key) => [key, secrets[key]!.trim()]),
    ),
  };
}
