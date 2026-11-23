export type DeviceControlSnapshot = {
  device_id: string;
  online: boolean;
  control_online: boolean;
  control_ready: boolean;
  voice_online: boolean;
  control: {
    boot_id: string;
    revision: number;
    ready: boolean;
    brightness: number | null;
    volume: number | null;
    capabilities: string[];
    updated_at: string;
  } | null;
};

const record = (value: unknown): Record<string, unknown> =>
  value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};

export function parseDeviceControl(
  value: unknown,
  deviceId: string,
): DeviceControlSnapshot {
  const root = record(value),
    state = record(root.state),
    control = record(state.control);
  if (root.device_id !== deviceId || typeof root.online !== "boolean")
    throw new Error("返回的状态与当前设备不一致。");
  const number = (key: string, min: number): number | null => {
    const item = control[key];
    if (item === null || item === undefined) return null;
    if (
      !Number.isInteger(item) ||
      (item as number) < min ||
      (item as number) > 100
    )
      throw new Error("设备上报的控制数值不正确。");
    return item as number;
  };
  let parsed: DeviceControlSnapshot["control"] = null;
  if (state.control !== undefined && state.control !== null) {
    if (
      control.schema_version !== 2 ||
      typeof control.boot_id !== "string" ||
      !/^[0-9a-f]{32}$/.test(control.boot_id) ||
      !Number.isInteger(control.revision) ||
      (control.revision as number) < 0 ||
      typeof control.ready !== "boolean" ||
      !Array.isArray(control.capabilities) ||
      !control.capabilities.every((item) => typeof item === "string") ||
      typeof control.updated_at !== "string" ||
      !Number.isFinite(Date.parse(control.updated_at))
    )
      throw new Error("设备控制状态格式不正确。");
    parsed = {
      boot_id: control.boot_id,
      revision: control.revision as number,
      ready: control.ready,
      brightness: number("brightness", 1),
      volume: number("volume", 0),
      capabilities: control.capabilities as string[],
      updated_at: control.updated_at,
    };
  }
  return {
    device_id: deviceId,
    online: root.online,
    control_online: root.control_online === true,
    control_ready: root.control_ready === true,
    voice_online: root.voice_online === true,
    control: parsed,
  };
}

export async function controlDelay(milliseconds: number, signal: AbortSignal) {
  if (signal.aborted) throw signal.reason;
  await new Promise<void>((resolve, reject) => {
    const abort = () => {
      clearTimeout(timer);
      reject(signal.reason);
    };
    const timer = setTimeout(() => {
      signal.removeEventListener("abort", abort);
      resolve();
    }, milliseconds);
    signal.addEventListener("abort", abort, { once: true });
  });
}
