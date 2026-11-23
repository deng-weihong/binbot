import type { Device } from "./data";

// The current device protocol has eight slots and a 72-byte text buffer.
export const maxTodos = 8;
export const validTodoText = (text: string) =>
  !!text.trim() && new TextEncoder().encode(text.trim()).length < 72;

export type Todo = { id: string; text: string; done: boolean };

export function createDemoTodos(device: Device): Todo[] {
  const texts =
    device.id === "binbot-002"
      ? ["挑一本周末想读的书", "给窗边的植物浇水"]
      : device.id === "binbot-003"
        ? ["换上干净的床单", "准备明天要穿的衣服"]
        : ["整理产品参考图", "回复设计反馈", "给桌上的植物浇水"];
  return texts.map((text, index) => ({
    id: `todo-${index + 1}`,
    text,
    done: false,
  }));
}

export function normalizeTodos(value: unknown, defaults: Todo[]): Todo[] {
  if (!Array.isArray(value)) return defaults.map((item) => ({ ...item }));
  const ids = new Set<string>();
  return value
    .flatMap((entry) => {
      if (!entry || typeof entry !== "object") return [];
      const { id, text, done } = entry;
      if (
        typeof id !== "string" ||
        !id.trim() ||
        ids.has(id) ||
        typeof text !== "string" ||
        !validTodoText(text)
      )
        return [];
      ids.add(id);
      return [{ id, text: text.trim(), done: done === true }];
    })
    .slice(0, maxTodos);
}
