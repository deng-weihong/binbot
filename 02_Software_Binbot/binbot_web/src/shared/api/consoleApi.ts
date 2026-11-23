import { parseWorkspace } from "../model/workspaceModel";
import { uid, type Device } from "../model/data";
import type { Schedule } from "../model/schedules";
import type { ReviewGrade } from "../model/memorySchedule";
import { parseDeviceChat } from "../model/assistantConversation";
import {
  feishuSaveBody,
  parseFeishuSnapshot,
  type DeviceFeishuSnapshot,
  type FeishuConfig,
  type FeishuSecrets,
} from "../model/deviceFeishu";

export const AUTH_EXPIRED = "binbot:auth-expired";
export class ConsoleError extends Error {
  constructor(
    public status: number,
    message: string,
    public code = "",
  ) {
    super(message);
  }
}
export const objectValue = (value: unknown): Record<string, unknown> =>
  value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
const stringValue = (value: unknown) =>
  typeof value === "string" ? value : "";

export type ComponentInfo = {
  device_id: string;
  component_id: string;
  profile: string;
  healthy: boolean;
  version: string;
  project: string;
  capacity: number;
  observed_at?: number;
  fresh: boolean;
  online: boolean;
};
export type Firmware = {
  firmware_id: string;
  profile: string;
  project: string;
  version: string;
  size_bytes: number;
  image_size_bytes?: number;
  sha256: string;
  created_at: number;
};
export type DeviceJob = {
  job_id: string;
  device_id: string;
  component_id: string;
  kind?: string;
  status: string;
  phase_percent: number;
  bytes_written: number;
  total_bytes: number;
  source_name?: string;
  result?: string | null;
  waiting_reason?: string;
  created_at: number;
  target?: Partial<Firmware>;
  before?: { profile?: string };
  asset_id?: string;
};
export type DeviceCommand = {
  command_id: string;
  device_id: string;
  name: string;
  status: string;
  message: string;
};
export const terminalJob = (status: string) =>
  ["succeeded", "failed", "cancelled"].includes(status);
export const jobPercent = (job: DeviceJob) =>
  job.status === "succeeded"
    ? 100
    : Math.min(
        99,
        Math.max(
          0,
          Math.round(
            Number.isFinite(job.phase_percent) ? job.phase_percent : 0,
          ),
        ),
      );
export const jobLabels: Record<string, string> = {
  waiting_for_component: "等待组件连接",
  preparing: "准备中",
  probe: "读取组件信息",
  downloading: "正在下载",
  transferring: "正在传输",
  validating: "校验中",
  activating: "正在应用",
  verifying: "确认设备状态",
  reconciling: "同步结果",
  succeeded: "已完成",
  failed: "执行失败",
  cancelled: "已取消",
  cancel_requested: "正在取消",
};
export const profileLabels: Record<string, string> = {
  round_screen: "笔筒圆屏",
  holographic_screen: "全息屏",
  showcase_turntable: "转盘",
  slave_mcu: "从机",
  ambient_light: "氛围灯",
};
export function formatBytes(value: number | undefined) {
  if (value === undefined || !Number.isFinite(value) || value < 0) return "—";
  return value >= 1024 * 1024
    ? `${(value / 1024 / 1024).toFixed(1)} MB`
    : `${Math.round(value / 1024)} KB`;
}
export function formatDate(value: string | number | undefined) {
  if (!value) return "暂无记录";
  const date = new Date(typeof value === "number" ? value * 1000 : value);
  return Number.isFinite(date.getTime())
    ? date.toLocaleString("zh-CN", { hour12: false })
    : "暂无记录";
}

export function parseDevices(value: unknown): Device[] {
  const rows = objectValue(value).devices;
  if (!Array.isArray(rows))
    throw new ConsoleError(502, "设备列表格式不正确，请检查后台服务。");
  const ids = new Set<string>();
  return rows.map((raw, index) => {
    const row = objectValue(raw),
      id = stringValue(row.id);
    if (
      !/^[A-Za-z0-9._:-]{1,64}$/.test(id) ||
      [".", ".."].includes(id) ||
      ids.has(id) ||
      typeof row.online !== "boolean"
    )
      throw new ConsoleError(502, "设备列表包含无效或重复的设备。");
    ids.add(id);
    return {
      id,
      name: stringValue(row.name) || id,
      location: stringValue(row.location) || "未设置位置",
      online: row.online,
      controlOnline: row.control_online === true,
      controlReady: row.control_ready === true,
      voiceOnline: row.voice_online === true,
      color: (["lilac", "mint", "peach"] as const)[index % 3],
      remoteIp: stringValue(row.remote_ip),
      connectedAt: stringValue(row.connected_at),
      lastSeenAt: stringValue(row.last_seen_at),
      capabilities: Array.isArray(row.capabilities)
        ? row.capabilities.filter(
            (item): item is string => typeof item === "string",
          )
        : [],
    };
  });
}

export function matchingFirmware(
  info: ComponentInfo | null,
  firmware: Firmware,
) {
  return (
    !!info?.online &&
    info.healthy &&
    firmware.profile === info.profile &&
    firmware.project === info.project &&
    (firmware.image_size_bytes || firmware.size_bytes) <= info.capacity
  );
}

function scoped<T extends { device_id: string }>(
  value: T,
  deviceId: string,
): T {
  if (objectValue(value).device_id !== deviceId)
    throw new ConsoleError(502, "返回的数据与当前设备不一致，请重新读取。");
  return value;
}

export type UploadProgress = (written: number, total: number) => void;

function uploadResponse(
  path: string,
  options: RequestInit,
  progress: UploadProgress,
): Promise<Response> {
  return new Promise((resolve, reject) => {
    const body = options.body as Blob;
    const xhr = new XMLHttpRequest();
    const abort = () => xhr.abort();
    const cleanup = () => options.signal?.removeEventListener("abort", abort);
    const fail = () => {
      cleanup();
      reject(options.signal?.reason || new Error("图片上传中断"));
    };
    if (options.signal?.aborted) {
      fail();
      return;
    }
    xhr.upload.onprogress = (event) =>
      progress(Math.min(event.loaded, body.size), body.size);
    xhr.upload.onload = () => progress(body.size, body.size);
    xhr.open(options.method || "PUT", path);
    for (const [name, value] of new Headers(options.headers))
      xhr.setRequestHeader(name, value);
    xhr.onload = () => {
      cleanup();
      if (!xhr.status) {
        fail();
        return;
      }
      resolve(
        new Response(xhr.status === 204 ? null : xhr.responseText, {
          status: xhr.status,
          headers: {
            "Content-Type": xhr.getResponseHeader("Content-Type") || "",
          },
        }),
      );
    };
    xhr.onerror = xhr.onabort = xhr.ontimeout = fail;
    options.signal?.addEventListener("abort", abort, { once: true });
    xhr.send(body);
  });
}

export function createConsoleTransport(
  request: typeof fetch = fetch,
  unauthorized = () => {
    if (typeof window !== "undefined")
      window.dispatchEvent(new Event(AUTH_EXPIRED));
  },
) {
  async function json<T>(
    path: string,
    options: RequestInit = {},
    announce = true,
    timeout = 20000,
    onUpload?: UploadProgress,
  ): Promise<T> {
    if (options.signal?.aborted) throw options.signal.reason;
    let response: Response;
    try {
      const init: RequestInit = {
        ...options,
        credentials: "same-origin",
        cache: "no-store",
        headers: { Accept: "application/json", ...options.headers },
        signal: options.signal
          ? AbortSignal.any([options.signal, AbortSignal.timeout(timeout)])
          : AbortSignal.timeout(timeout),
      };
      response =
        onUpload && typeof XMLHttpRequest !== "undefined" && request === fetch
          ? await uploadResponse(path, init, onUpload)
          : await request(path, init);
    } catch (error) {
      if (options.signal?.aborted) throw error;
      throw new ConsoleError(0, "暂时无法连接后台，请稍后重试。");
    }
    if (options.signal?.aborted) throw options.signal.reason;
    if (response.status === 401) {
      if (announce) unauthorized();
      throw new ConsoleError(
        401,
        announce ? "登录已失效，请重新登录。" : "账号或密码不正确。",
      );
    }
    if (response.ok && response.status === 204) return undefined as T;
    if (!response.headers.get("Content-Type")?.includes("application/json"))
      throw new ConsoleError(
        response.status,
        "后台服务尚未连接，请稍后重试，或进入界面预览。",
      );
    let payload: unknown;
    try {
      payload = await response.json();
    } catch {
      throw new ConsoleError(502, "后台返回了无法读取的数据。");
    }
    if (!response.ok) {
      const row = objectValue(payload),
        code = stringValue(row.code);
      const messages: Record<string, string> = {
        CHAT_REQUEST_UNCERTAIN:
          "上次请求的结果尚未确认，请先查看对话和设备状态，再发送新消息。",
        DEVICE_OFFLINE: "设备已离线，连接恢复后再试。",
        PROBE_TIMEOUT: "读取版本超时，请检查设备连接。",
        NO_SNAPSHOT: "设备尚未上报此组件的信息。",
        CAPABILITY_REQUIRED: "当前设备版本尚不支持此操作。",
        WRONG_PROFILE: "固件与当前组件不匹配。",
        NOT_READY: "组件暂时无法升级，请刷新设备信息。",
        PUBLIC_ORIGIN_REQUIRED: "固件上传服务尚未配置完成。",
        CANCEL_TOO_LATE: "已开始应用更新，请等待设备确认结果。",
        HASH_MISMATCH: "文件校验失败，请重新上传。",
        UNSUPPORTED_ASSET: "当前组件不支持图片显示。",
      };
      throw new ConsoleError(
        response.status,
        ([
          "DEVICE_FEISHU_ERROR",
          "WORKSPACE_ERROR",
          "CALENDAR_SYNC_ERROR",
        ].includes(code) && typeof row.error === "string"
          ? row.error.slice(0, 256)
          : "") ||
          messages[code] ||
          (response.status === 403
            ? "当前账号无法执行此操作。"
            : response.status === 404
              ? "该功能暂无数据或尚未启用。"
              : response.status === 409
                ? "设备正忙或状态已变化，请刷新后重试。"
                : "操作未能完成，请检查设备及后台状态。"),
        code,
      );
    }
    return payload as T;
  }
  const root = (id: string) =>
    `/api/v1/admin/devices/${encodeURIComponent(id)}`;
  const workspacePath = (id: string, suffix = "", revision = "") => {
    const query = new URLSearchParams({ compact: "1" });
    if (revision) query.set("wordbooks_revision", revision);
    return `${root(id)}/workspace${suffix}?${query}`;
  };
  const post = (body: unknown, signal?: AbortSignal): RequestInit => ({
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
  async function sendCommand(
    id: string,
    name: string,
    payload: unknown,
    requestKey: string,
    signal?: AbortSignal,
  ) {
    const result = scoped(
      await json<DeviceCommand>(`${root(id)}/commands`, {
        ...post({ name, payload }, signal),
        headers: {
          "Content-Type": "application/json",
          "Idempotency-Key": requestKey,
        },
      }),
      id,
    );
    if (result.name !== name || typeof result.command_id !== "string")
      throw new ConsoleError(502, "控制回执与当前操作不一致。");
    return result;
  }
  return {
    async checkSession(signal?: AbortSignal, announce = true) {
      const session = await json<{ authenticated: boolean }>(
        "/api/v1/admin/session",
        { signal },
        announce,
        10000,
      );
      if (session.authenticated !== true) {
        if (announce) unauthorized();
        throw new ConsoleError(401, "请登录后继续。");
      }
    },
    async devices(signal?: AbortSignal, announce = true) {
      return parseDevices(
        await json("/api/v1/admin/devices", { signal }, announce),
      );
    },
    async gallery(id: string) {
      return json<{ device_id: string; images: Record<string, unknown>[] }>(
        `${root(id)}/gallery`,
      );
    },
    async storeGallery(
      id: string,
      imageId: string,
      blob: Blob,
      name: string,
      signal?: AbortSignal,
      onUpload?: UploadProgress,
    ) {
      return json<Record<string, unknown>>(
        `${root(id)}/gallery/${encodeURIComponent(imageId)}?name=${encodeURIComponent(name)}`,
        {
          method: "PUT",
          headers: { "Content-Type": blob.type || "application/octet-stream" },
          body: blob,
          signal,
        },
        true,
        120000,
        onUpload,
      );
    },
    async patchGallery(id: string, imageId: string, patch: unknown) {
      return json(`${root(id)}/gallery/${encodeURIComponent(imageId)}`, {
        ...post(patch),
        method: "PATCH",
      });
    },
    async removeGallery(id: string, imageId: string) {
      await json(`${root(id)}/gallery/${encodeURIComponent(imageId)}`, {
        method: "DELETE",
      });
    },
    async workspace(id: string, signal?: AbortSignal, wordbooksRevision = "") {
      return parseWorkspace(
        await json(workspacePath(id, "", wordbooksRevision), { signal }),
        id,
      );
    },
    async saveWorkspace(
      id: string,
      revision: string,
      data: unknown,
      eventId: string,
      signal?: AbortSignal,
      wordbooksRevision = "",
    ) {
      return parseWorkspace(
        await json(
          workspacePath(id, "", wordbooksRevision),
          {
            ...post({ revision, data, event_id: eventId }, signal),
            method: "PUT",
          },
          true,
          180000,
        ),
        id,
      );
    },
    async workspaceAction(
      id: string,
      name: string,
      payload: unknown,
      eventId: string,
      signal?: AbortSignal,
      wordbooksRevision = "",
    ) {
      const result = await json<{ workspace: unknown }>(
        workspacePath(id, "/actions", wordbooksRevision),
        post({ name, payload, event_id: eventId }, signal),
        true,
        65000,
      );
      return parseWorkspace(result.workspace, id);
    },
    async createSchedule(
      id: string,
      schedule: Schedule,
      eventId: string,
      signal?: AbortSignal,
      wordbooksRevision = "",
    ) {
      const {
        id: scheduleId,
        name,
        date,
        time,
        note,
        enabled,
        done,
      } = schedule;
      return parseWorkspace(
        await json(
          workspacePath(id, "/schedules", wordbooksRevision),
          post(
            {
              schedule: {
                id: scheduleId,
                name,
                date,
                time,
                note,
                enabled,
                done,
              },
              event_id: eventId,
            },
            signal,
          ),
          true,
          65000,
        ),
        id,
      );
    },
    async reviewWord(
      id: string,
      bookId: string,
      word: string,
      grade: ReviewGrade,
      eventId: string,
      retry: boolean,
    ) {
      return parseWorkspace(
        await json(
          workspacePath(id, "/reviews"),
          post({
            book_id: bookId,
            word,
            grade,
            retry,
            event_id: eventId,
          }),
        ),
        id,
      );
    },
    async studyBatch(id: string, bookId: string, signal?: AbortSignal) {
      const query = new URLSearchParams({ book_id: bookId });
      const result = await json<{ book_id: string; words: string[] }>(
        `${root(id)}/workspace/learning?${query}`,
        { signal },
      );
      if (
        result.book_id !== bookId ||
        !Array.isArray(result.words) ||
        result.words.some((word) => typeof word !== "string")
      )
        throw new ConsoleError(502, "复习词卡与当前词本不一致。");
      return result.words;
    },
    async chatHistory(id: string, signal?: AbortSignal) {
      return parseDeviceChat(
        await json(`${root(id)}/chat`, { signal }),
        id,
      );
    },
    async sendChat(
      id: string,
      text: string,
      requestId: string,
      signal?: AbortSignal,
    ) {
      return parseDeviceChat(
        await json(
          `${root(id)}/chat`,
          post({ text, request_id: requestId }, signal),
          true,
          120000,
        ),
        id,
      );
    },
    async feishu(id: string, signal?: AbortSignal) {
      return parseFeishuSnapshot(
        await json(`${root(id)}/feishu`, { signal }),
        id,
      );
    },
    async saveFeishu(
      id: string,
      snapshot: DeviceFeishuSnapshot,
      config: FeishuConfig,
      secrets: FeishuSecrets,
      signal?: AbortSignal,
    ) {
      if (snapshot.device_id !== id)
        throw new ConsoleError(400, "飞书配置与当前设备不一致。");
      return parseFeishuSnapshot(
        await json(`${root(id)}/feishu`, {
          ...post(feishuSaveBody(snapshot, config, secrets), signal),
          method: "PUT",
        }),
        id,
      );
    },
    async authorizeFeishu(
      id: string,
      redirectUri: string,
      signal?: AbortSignal,
    ) {
      const result = scoped(
        await json<{ device_id: string; authorize_url: string }>(
          `${root(id)}/feishu/oauth/url?redirect_uri=${encodeURIComponent(redirectUri)}`,
          { signal },
        ),
        id,
      );
      let url: URL;
      try {
        url = new URL(result.authorize_url);
      } catch {
        throw new ConsoleError(502, "飞书授权地址无效。");
      }
      if (
        url.protocol !== "https:" ||
        url.hostname !== "open.feishu.cn" ||
        url.username ||
        url.password ||
        url.pathname !== "/open-apis/authen/v1/authorize"
      )
        throw new ConsoleError(502, "飞书授权地址无效。");
      return result.authorize_url;
    },
    async disconnectFeishu(id: string, signal?: AbortSignal) {
      return parseFeishuSnapshot(
        await json(`${root(id)}/feishu/oauth/disconnect`, post({}, signal)),
        id,
      );
    },
    async syncFeishu(id: string, signal?: AbortSignal) {
      return scoped(
        await json<{ device_id: string; received: number }>(
          `${root(id)}/calendar/sync`,
          post({}, signal),
        ),
        id,
      );
    },
    login(
      username: string,
      password: string,
      remember: boolean,
      signal?: AbortSignal,
    ) {
      return json(
        "/api/v1/admin/session",
        post({ username: username.trim(), password, remember }, signal),
        false,
      );
    },
    logout(signal?: AbortSignal) {
      return json("/api/v1/admin/logout", post({}, signal));
    },
    async state(id: string, signal?: AbortSignal) {
      return scoped(
        await json<{
          device_id: string;
          online: boolean;
          state: { updated_at?: string; revision?: number } | null;
        }>(`${root(id)}/state`, { signal }),
        id,
      );
    },
    async component(
      id: string,
      componentId: string,
      fresh = false,
      signal?: AbortSignal,
    ) {
      const row = scoped(
        await json<ComponentInfo>(
          `${root(id)}/components/${encodeURIComponent(componentId)}?fresh=${fresh}`,
          { signal },
        ),
        id,
      );
      if (row.component_id !== componentId)
        throw new ConsoleError(502, "组件信息与当前插槽不一致。");
      return row;
    },
    async jobs(id: string, signal?: AbortSignal) {
      const result = await json<{ items: DeviceJob[] }>(
        `/api/v1/admin/ota/jobs?device_id=${encodeURIComponent(id)}&limit=30`,
        { signal },
      );
      if (!Array.isArray(result.items))
        throw new ConsoleError(502, "任务列表格式不正确。");
      return result.items.map((item) => scoped(item, id));
    },
    async job(id: string, jobId: string, signal?: AbortSignal) {
      const result = scoped(
        await json<DeviceJob>(
          `/api/v1/admin/ota/jobs/${encodeURIComponent(jobId)}`,
          { signal },
        ),
        id,
      );
      if (result.job_id !== jobId)
        throw new ConsoleError(502, "返回的任务与当前操作不一致。");
      return result;
    },
    async command(id: string, commandId: string, signal?: AbortSignal) {
      const result = scoped(
        await json<DeviceCommand>(
          `/api/v1/admin/commands/${encodeURIComponent(commandId)}`,
          { signal },
        ),
        id,
      );
      if (result.command_id !== commandId)
        throw new ConsoleError(502, "返回的回执与当前操作不一致。");
      return result;
    },
    sendCommand,
    async sendControl(
      id: string,
      name: "screen.brightness.set" | "audio.volume.set",
      value: number,
      idempotencyKey: string,
      signal?: AbortSignal,
    ) {
      const key = name === "screen.brightness.set" ? "brightness" : "volume";
      return sendCommand(id, name, { [key]: value }, idempotencyKey, signal);
    },
    async uploadImage(
      id: string,
      file: Blob,
      name: string,
      target: "gallery" | "component",
      signal?: AbortSignal,
      requestKey = uid(),
      onUpload?: UploadProgress,
    ) {
      const options = {
        method: "PUT",
        headers: {
          "Content-Type": file.type || "application/octet-stream",
          "Idempotency-Key": requestKey,
        },
        body: file,
        signal,
      };
      if (target === "component") {
        const result = scoped(
          await json<DeviceJob>(
            `${root(id)}/components/component-1/gif?name=${encodeURIComponent(name)}`,
            options,
            true,
            120000,
            onUpload,
          ),
          id,
        );
        if (result.component_id !== "component-1")
          throw new ConsoleError(502, "返回的任务与当前组件不一致。");
        return result;
      }
      const result = await json<{
        asset_id: string;
        name: string;
        command: DeviceCommand;
      }>(
        `${root(id)}/modules/images?name=${encodeURIComponent(name)}`,
        options,
        true,
        120000,
        onUpload,
      );
      scoped(result.command, id);
      return result;
    },
    async firmware(signal?: AbortSignal) {
      const items: Firmware[] = [],
        seen = new Set<string>();
      let cursor = "";
      do {
        const page = await json<{
          items: Firmware[];
          next_cursor: string | null;
        }>(
          `/api/v1/admin/firmware?limit=100&cursor=${encodeURIComponent(cursor)}`,
          { signal },
        );
        if (!Array.isArray(page.items))
          throw new ConsoleError(502, "固件列表格式不正确。");
        items.push(...page.items);
        cursor = page.next_cursor || "";
        if (cursor && seen.has(cursor))
          throw new ConsoleError(502, "固件列表分页异常，请重新读取。");
        seen.add(cursor);
      } while (cursor);
      return items.sort((a, b) => b.created_at - a.created_at);
    },
    async uploadFirmware(file: File, signal?: AbortSignal) {
      if (file.size < 168 || file.size > 16 * 1024 * 1024)
        throw new ConsoleError(400, "请选择 168 B 至 16 MB 的固件文件。");
      const { sha256 } = await import("@noble/hashes/sha2.js");
      const bytes = new Uint8Array(await file.arrayBuffer());
      const digest = Array.from(sha256(bytes), (byte) =>
        byte.toString(16).padStart(2, "0"),
      ).join("");
      if (signal?.aborted) throw signal.reason;
      const prepared = await json<{ upload_id: string; upload_url: string }>(
        "/api/v1/admin/firmware/uploads",
        post({ size_bytes: file.size, sha256: digest }, signal),
      );
      const uploadUrl = new URL(prepared.upload_url, "http://console.invalid");
      if (
        uploadUrl.pathname !==
        `/api/v1/admin/firmware/uploads/${prepared.upload_id}`
      )
        throw new ConsoleError(502, "固件上传地址无效。");
      await json(
        `${uploadUrl.pathname}${uploadUrl.search}`,
        {
          method: "PUT",
          headers: { "Content-Type": "application/octet-stream" },
          body: file,
          signal,
        },
        true,
        120000,
      );
      return json<Firmware>(
        "/api/v1/admin/firmware",
        post({ upload_id: prepared.upload_id }, signal),
        true,
        120000,
      );
    },
    async startUpgrade(
      id: string,
      componentId: string,
      firmwareId: string,
      signal?: AbortSignal,
      requestKey = uid(),
    ) {
      const result = scoped(
        await json<DeviceJob>(
          `${root(id)}/components/${encodeURIComponent(componentId)}/ota`,
          post(
            {
              firmware_id: firmwareId,
              idempotency_key: requestKey,
              wait_for_component: false,
              allow_downgrade: false,
            },
            signal,
          ),
        ),
        id,
      );
      if (result.component_id !== componentId)
        throw new ConsoleError(502, "返回的任务与升级对象不一致。");
      return result;
    },
    async cancel(id: string, jobId: string, signal?: AbortSignal) {
      const result = scoped(
        await json<DeviceJob>(
          `/api/v1/admin/ota/jobs/${encodeURIComponent(jobId)}/cancel`,
          post({}, signal),
        ),
        id,
      );
      if (result.job_id !== jobId)
        throw new ConsoleError(502, "返回的任务与取消对象不一致。");
      return result;
    },
  };
}
