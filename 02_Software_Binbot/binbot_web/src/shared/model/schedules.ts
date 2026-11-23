import type { Device } from "./data";

export const demoDate = "2026-11-16";
export const agendaWeek = ["一", "二", "三", "四", "五", "六", "日"].map(
  (weekday, index) => ({
    date: `2026-11-${16 + index}`,
    day: 16 + index,
    weekday,
  }),
);

export type Schedule = {
  id: string;
  name: string;
  date: string;
  time: string;
  note: string;
  enabled: boolean;
  done: boolean;
};

export function createDemoSchedules(device: Device): Schedule[] {
  const entries =
    device.id === "binbot-002"
      ? [
          ["read", "留一点时间读书", "16:00", "读几页喜欢的书"],
          ["rest", "准备休息", "22:00", "让今天慢慢安静下来"],
        ]
      : [
          ["focus", "整理新产品的想法", "10:30", "留出 25 分钟，专心做一件事"],
          ["meeting", "产品设计讨论", "14:00", "提前整理讨论要点"],
          ["finish", "收起今天，准备下班", "18:00", "整理桌面，结束一天的工作"],
        ];
  return entries.map(([id, name, time, note]) => ({
    id,
    name,
    time,
    note,
    date: demoDate,
    enabled: true,
    done: false,
  }));
}

export function normalizeSchedules(
  value: unknown,
  defaults: Schedule[],
): Schedule[] {
  if (!Array.isArray(value)) return defaults.map((item) => ({ ...item }));
  const ids = new Set<string>();
  return value.flatMap((entry) => {
    if (!entry || typeof entry !== "object") return [];
    const { id, name, time, note, enabled, done } = entry;
    if (
      typeof id !== "string" ||
      !id.trim() ||
      ids.has(id) ||
      typeof name !== "string" ||
      !name.trim() ||
      typeof time !== "string" ||
      !/^([01]\d|2[0-3]):[0-5]\d$/.test(time)
    )
      return [];
    // Older home agendas stored only a time, on the prototype's fixed day.
    const date = entry.date === undefined ? demoDate : entry.date;
    if (
      typeof date !== "string" ||
      !/^\d{4}-\d{2}-\d{2}$/.test(date) ||
      !Number.isFinite(Date.parse(`${date}T12:00:00Z`)) ||
      new Date(`${date}T12:00:00Z`).toISOString().slice(0, 10) !== date
    )
      return [];
    ids.add(id);
    return [
      {
        id,
        name,
        date,
        time,
        note: typeof note === "string" ? note : "",
        enabled: enabled !== false,
        done: done === true,
      },
    ];
  });
}

export function schedulesForDate(schedules: Schedule[], date: string) {
  return schedules
    .filter((item) => item.date === date && item.enabled)
    .sort((a, b) => a.time.localeCompare(b.time));
}
