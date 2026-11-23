import { createContext, useContext } from "react";
import { devices, loadDemoData, type Device } from "../model/data";

export type ConsoleMode = "live" | "demo";
export type ConsoleContextValue = {
  mode: ConsoleMode;
  devices: Device[];
  refreshing: boolean;
  error: string;
  refresh: () => Promise<void>;
  logout: () => Promise<void>;
  updateLabel: (id: string, name: string, location: string) => Promise<void>;
  expire: () => void;
};

export const ConsoleContext = createContext<ConsoleContextValue>({
  mode: "demo",
  devices,
  refreshing: false,
  error: "",
  refresh: async () => {},
  logout: async () => {},
  updateLabel: async () => {},
  expire: () => {},
});
export const useConsole = () => useContext(ConsoleContext);

export type DeviceLabel = { name: string; location: string };
export function loadDeviceLabels(
  mode: ConsoleMode,
): Record<string, DeviceLabel> {
  try {
    const source: unknown = JSON.parse(
      localStorage.getItem(`binbot-device-labels-v1:${mode}`) || "{}",
    );
    if (!source || typeof source !== "object" || Array.isArray(source))
      return {};
    return Object.fromEntries(
      Object.entries(source).flatMap(([id, raw]) => {
        const row = raw as Partial<DeviceLabel> | null;
        return row &&
          typeof row.name === "string" &&
          row.name.trim() &&
          typeof row.location === "string"
          ? [
              [
                id,
                {
                  name: row.name.slice(0, 20),
                  location: row.location.slice(0, 30),
                },
              ],
            ]
          : [];
      }),
    );
  } catch {
    return {};
  }
}

export function applyDeviceLabels(items: Device[], mode: ConsoleMode) {
  const labels = mode === "demo" ? loadDeviceLabels(mode) : {};
  return items.map((device) => ({
    ...device,
    ...(mode === "demo" ? { name: loadDemoData(device).deviceName } : {}),
    ...labels[device.id],
  }));
}
