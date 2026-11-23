import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type SetStateAction,
} from "react";
import {
  ConsoleError,
  createConsoleTransport,
  type DeviceCommand,
} from "../api/consoleApi";
import type { Schedule } from "../model/schedules";
import type { ReviewGrade } from "../model/memorySchedule";
import { bookWordKey, normalizeWordbooks, type Wordbooks } from "../model/wordbooks";
import {
  loadDemoData,
  uid,
  workspaceStorageKey,
  type Device,
  type DeviceData,
} from "../model/data";
import {
  emptyLiveData,
  equal,
  mergePatch,
  mergeWorkspaceSnapshot,
  workspaceData,
  workspacePatch,
  workspaceRequestPatch,
  type WorkspaceSnapshot,
} from "../model/workspaceModel";

type ImageCommand = {
  profile: number;
  imageId: string;
  command: DeviceCommand;
};
const imageCommandPending = (operation: ImageCommand | null) =>
  !!operation && !["completed", "failed"].includes(operation.command.status);

type WorkspaceValue = {
  live: boolean;
  loading: boolean;
  pending: boolean;
  error: string;
  snapshot: WorkspaceSnapshot | null;
  imageCommand: ImageCommand | null;
  pushModuleImage: (
    profile: number,
    imageId: string,
    settings?: Record<string, unknown>,
  ) => Promise<void>;
  action: (name: string, payload: unknown) => Promise<void>;
  saveSchedule: (schedule: Schedule) => Promise<void>;
  reviewWord: (
    bookId: string,
    word: string,
    grade: ReviewGrade,
    eventId: string,
    retry: boolean,
  ) => Promise<string | null>;
};
export const WorkspaceContext = createContext<WorkspaceValue>({
  live: false,
  loading: false,
  pending: false,
  error: "",
  snapshot: null,
  imageCommand: null,
  pushModuleImage: async () => {},
  action: async () => {},
  saveSchedule: async () => {},
  reviewWord: async () => null,
});
export const useWorkspace = () => useContext(WorkspaceContext);
const api = createConsoleTransport();

export function useDeviceWorkspace(
  device: Device,
  mode: "live" | "demo",
  toast: (message: string) => void,
) {
  const [data, render] = useState<DeviceData>(() =>
    mode === "demo" ? loadDemoData(device) : emptyLiveData(device),
  );
  const [snapshot, setSnapshot] = useState<WorkspaceSnapshot | null>(null);
  const [imageCommand, setImageCommand] = useState<ImageCommand | null>(null);
  const imageOperation = useRef<ImageCommand | null>(null);
  const imageRequestKeys = useRef(new Map<string, string>());
  const [loading, setLoading] = useState(mode === "live"),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const current = useRef(data),
    server = useRef<WorkspaceSnapshot | null>(null),
    serverBooks = useRef<Wordbooks>(data.wordbooks),
    queue = useRef<Record<string, unknown>>({});
  const working = useRef(false),
    scheduled = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const generation = useRef(0),
    message = useRef(""),
    toastRef = useRef(toast);
  toastRef.current = toast;
  function publish(incoming: WorkspaceSnapshot, preserveDraft = false) {
    const previous = server.current;
    const next = mergeWorkspaceSnapshot(incoming, previous);
    const unchangedBooks =
      previous?.wordbooks_revision &&
      previous.wordbooks_revision === next.wordbooks_revision;
    if (!unchangedBooks)
      serverBooks.current = normalizeWordbooks(next.data.wordbooks);
    server.current = next;
    setSnapshot(next);
    if (!preserveDraft) {
      const value = workspaceData(next, device, serverBooks.current);
      if (
        value.wordbooks !== current.current.wordbooks ||
        !equal(
          { ...current.current, wordbooks: null },
          { ...value, wordbooks: null },
        )
      ) {
        current.current = value;
        render(value);
      }
    }
  }
  const saving = () =>
    working.current ||
    !!scheduled.current ||
    Object.keys(queue.current).length > 0;
  async function flush() {
    scheduled.current = undefined;
    if (working.current || !server.current) return;
    working.current = true;
    setPending(true);
    const version = generation.current;
    try {
      while (Object.keys(queue.current).length) {
        const patch = workspaceRequestPatch(serverBooks.current, queue.current);
        queue.current = {};
        if (!Object.keys(patch).length) {
          publish(server.current);
          continue;
        }
        const next = await api.saveWorkspace(
          device.id,
          server.current.revision,
          patch,
          uid(),
          undefined,
          server.current.wordbooks_revision,
        );
        if (version !== generation.current) return;
        publish(next, Object.keys(queue.current).length > 0);
      }
      if (message.current) toastRef.current("设置已同步");
      message.current = "";
      setError("");
    } catch (reason) {
      if (version !== generation.current) return;
      queue.current = {};
      message.current = "";
      const description =
        reason instanceof Error ? reason.message : "设置未能保存，请重试。";
      setError(description);
      toastRef.current(description);
      try {
        const next = await api.workspace(
          device.id,
          undefined,
          server.current?.wordbooks_revision,
        );
        if (version === generation.current) publish(next);
      } catch {
        /* Keep the failure visible. */
      }
    } finally {
      if (version === generation.current) {
        working.current = false;
        setPending(false);
      }
    }
  }
  const setData: Dispatch<SetStateAction<DeviceData>> = (value) => {
    const previous = current.current,
      next = typeof value === "function" ? value(previous) : value;
    if (mode === "demo") {
      current.current = next;
      render(next);
      return;
    }
    if (!server.current) {
      toastRef.current("请等待工作台读取完成。");
      return;
    }
    const patch = workspacePatch(previous, next);
    if (!Object.keys(patch).length) return;
    queue.current = mergePatch(queue.current, patch);
    current.current = next;
    render(next);
    setPending(true);
    setError("");
    if (!working.current) {
      if (scheduled.current) clearTimeout(scheduled.current);
      scheduled.current = setTimeout(() => void flush(), 250);
    }
  };
  const notify = (text: string) => {
    if (mode === "live" && saving()) {
      message.current = text;
      return;
    }
    toastRef.current(text);
  };
  async function action(name: string, payload: unknown) {
    if (saving()) {
      toastRef.current("正在同步设置，请稍候。");
      return;
    }
    working.current = true;
    setPending(true);
    setError("");
    const version = generation.current;
    try {
      const result = await api.workspaceAction(
        device.id,
        name,
        payload,
        uid(),
        undefined,
        server.current?.wordbooks_revision,
      );
      if (version === generation.current) publish(result);
    } catch (reason) {
      if (version === generation.current) {
        const text = reason instanceof Error ? reason.message : "设备操作失败";
        setError(text);
        toastRef.current(text);
      }
    } finally {
      if (version === generation.current) {
        working.current = false;
        setPending(false);
        if (Object.keys(queue.current).length) void flush();
      }
    }
  }
  async function pushModuleImage(
    profile: number,
    imageId: string,
    settings?: Record<string, unknown>,
  ) {
    if (
      saving() ||
      imageCommandPending(imageOperation.current) ||
      !server.current
    )
      return;
    working.current = true;
    setPending(true);
    setError("");
    imageOperation.current = null;
    setImageCommand(null);
    const version = generation.current;
    const key = `${profile}:${imageId}`;
    const requestId = imageRequestKeys.current.get(key) || uid();
    imageRequestKeys.current.set(key, requestId);
    try {
      if (settings) {
        const next = await api.saveWorkspace(
          device.id,
          server.current.revision,
          settings,
          uid(),
          undefined,
          server.current.wordbooks_revision,
        );
        if (version !== generation.current) return;
        publish(next, Object.keys(queue.current).length > 0);
      }
      const command = await api.sendCommand(
        device.id,
        "module.display.set",
        { profile, image_id: imageId },
        requestId,
      );
      if (version !== generation.current) return;
      imageRequestKeys.current.delete(key);
      imageOperation.current = { profile, imageId, command };
      setImageCommand(imageOperation.current);
    } catch (reason) {
      if (
        reason instanceof ConsoleError &&
        reason.status >= 400 &&
        reason.status < 500
      )
        imageRequestKeys.current.delete(key);
      if (version === generation.current) {
        const description =
          reason instanceof Error ? reason.message : "图片更新请求失败";
        setError(description);
        toastRef.current(description);
      }
    } finally {
      if (version === generation.current) {
        working.current = false;
        setPending(false);
        if (Object.keys(queue.current).length) void flush();
      }
    }
  }
  async function saveSchedule(schedule: Schedule) {
    if (saving()) throw new Error("正在同步设置，请稍候再保存日程。");
    working.current = true;
    setPending(true);
    setError("");
    const version = generation.current;
    try {
      const result = await api.createSchedule(
        device.id,
        schedule,
        "agenda:" + schedule.id,
        undefined,
        server.current?.wordbooks_revision,
      );
      if (version === generation.current) {
        publish(result);
        toastRef.current(
          result.calendar_sync?.enabled
            ? "日程已同步飞书，将自动下发设备"
            : "日程已保存，将自动同步设备",
        );
      }
    } catch (reason) {
      if (version === generation.current) {
        const description =
          reason instanceof Error ? reason.message : "日程暂未同步，请重试。";
        setError(description);
      }
      throw reason;
    } finally {
      if (version === generation.current) {
        working.current = false;
        setPending(false);
        if (Object.keys(queue.current).length) void flush();
      }
    }
  }
  async function reviewWord(
    bookId: string,
    word: string,
    grade: ReviewGrade,
    eventId: string,
    retry: boolean,
  ) {
    if (saving()) throw new Error("正在保存，请稍候再作答。");
    if (!server.current) throw new Error("请等待工作台读取完成。");
    working.current = true;
    setPending(true);
    setError("");
    const version = generation.current;
    try {
      const result = await api.reviewWord(
        device.id,
        bookId,
        word,
        grade,
        eventId,
        retry,
      );
      if (version !== generation.current) return null;
      publish(result, Object.keys(queue.current).length > 0);
      const card = serverBooks.current.books
        .find((book) => book.id === bookId)
        ?.words.find(
          (item) =>
            bookWordKey(bookId, item.word) === bookWordKey(bookId, word),
        );
      return card?.review.nextReviewAt ?? null;
    } finally {
      if (version === generation.current) {
        working.current = false;
        setPending(false);
        if (Object.keys(queue.current).length) void flush();
      }
    }
  }
  useEffect(() => {
    if (mode === "demo") return;
    const version = ++generation.current,
      controller = new AbortController();
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      if (document.visibilityState === "hidden") {
        timer = setTimeout(() => void poll(), 1000);
        return;
      }
      const before = server.current;
      const transfer = imageOperation.current;
      const refreshImage =
        transfer && imageCommandPending(transfer)
          ? api
              .command(
                device.id,
                transfer.command.command_id,
                controller.signal,
              )
              .then((command) => {
                if (
                  controller.signal.aborted ||
                  imageOperation.current !== transfer
                )
                  return;
                imageOperation.current = { ...transfer, command };
                setImageCommand(imageOperation.current);
              })
              .catch(() => {
                /* Retry while the command is still in progress. */
              })
          : undefined;
      try {
        const next = await api.workspace(
          device.id,
          controller.signal,
          server.current?.wordbooks_revision,
        );
        if (
          !controller.signal.aborted &&
          version === generation.current &&
          before === server.current
        ) {
          if (saving()) {
            // Keep the draft and write revision while reading live device progress.
            setSnapshot(
              (value) =>
                value && {
                  ...value,
                  reported: next.reported,
                  control: next.control,
                },
            );
          } else {
            publish(next);
            setError("");
            setLoading(false);
          }
        }
      } catch (reason) {
        if (!controller.signal.aborted && !saving()) {
          setError(reason instanceof Error ? reason.message : "工作台读取失败");
          setLoading(false);
        }
      }
      await refreshImage;
      if (!controller.signal.aborted)
        timer = setTimeout(
          () => void poll(),
          saving() || imageCommandPending(imageOperation.current) ? 800 : 2000,
        );
    };
    void poll();
    return () => {
      generation.current++;
      controller.abort();
      clearTimeout(timer);
      if (scheduled.current) clearTimeout(scheduled.current);
    };
  }, [device.id, mode]);
  useEffect(() => {
    if (mode === "demo") {
      try {
        localStorage.setItem(
          workspaceStorageKey(device.id),
          JSON.stringify(data),
        );
      } catch {
        /* Optional demo persistence. */
      }
    }
  }, [data, device.id, mode]);
  return {
    data,
    setData,
    notify,
    value: {
      live: mode === "live",
      loading,
      pending,
      error,
      snapshot,
      imageCommand,
      pushModuleImage,
      action,
      saveSchedule,
      reviewWord,
    },
  };
}
