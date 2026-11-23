import { useEffect, useRef, useState, type DragEvent } from "react";
import {
  Check,
  CircleAlert,
  Download,
  ImagePlus,
  Images,
  LoaderCircle,
  Monitor,
  Pencil,
  RefreshCw,
  Search,
  Send,
  Trash2,
  Upload,
} from "lucide-react";
import { useConsole } from "../../shared/state/consoleContext";
import {
  createConsoleTransport,
  ConsoleError,
  formatBytes,
  formatDate,
  jobPercent,
  terminalJob,
  type ComponentInfo,
  type DeviceJob,
} from "../../shared/api/consoleApi";
import { uid, type Device } from "../../shared/model/data";
import { moduleImages } from "../../shared/model/deviceFeatures";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";
import {
  ImageTransferProgress,
  imageTransferLabel,
} from "./ImageTransferProgress";
import {
  galleryScope,
  listGalleryAssets,
  patchGalleryAsset,
  putGalleryAsset,
  removeGalleryAsset,
  validateImageFile,
  verifyImagePreview,
  type GalleryAsset,
} from "./moduleGallery";

const api = createConsoleTransport();
const builtins = moduleImages.map((item) => ({
  ...item,
  id: `builtin-${item.id}`,
}));
type ImageItem = {
  id: string;
  name: string;
  src: string;
  builtin: boolean;
  asset?: GalleryAsset;
};

export default function ModuleGallery({
  device,
  name,
  notify,
}: {
  device: Device;
  name: string;
  notify: (message: string) => void;
}) {
  const { mode } = useConsole();
  const scope = galleryScope(mode, device.id);
  const [assets, setAssets] = useState<GalleryAsset[]>([]),
    [urls, setUrls] = useState<Record<string, string>>({});
  const [selected, setSelected] = useState(builtins[0].id),
    [query, setQuery] = useState("");
  const [filter, setFilter] = useState<"all" | "custom">("all");
  const [profile, setProfile] = useState<"round" | "holo">("round");
  const [info, setInfo] = useState<ComponentInfo | null>(null);
  const [busy, setBusy] = useState(""),
    [error, setError] = useState(""),
    [readError, setReadError] = useState("");
  const [upload, setUpload] = useState<{
    name: string;
    label: string;
    percent?: number;
  } | null>(null);
  const [dragging, setDragging] = useState(false),
    [loading, setLoading] = useState(true);
  const [editing, setEditing] = useState<"rename" | "remove" | null>(null),
    [newName, setNewName] = useState("");
  const [dialogError, setDialogError] = useState("");
  const lifetime = useRef<AbortController | null>(null);
  const sendRequests = useRef(new Map<string, string>());
  const inputRef = useRef<HTMLInputElement>(null);
  const images: ImageItem[] = [
    ...builtins.map((item) => ({
      ...item,
      builtin: true,
      asset: assets.find((asset) => asset.id === item.id),
    })),
    ...assets
      .filter((asset) => !asset.id.startsWith("builtin-"))
      .map((asset) => ({
        id: asset.id,
        name: asset.name,
        src: urls[asset.id] || "",
        builtin: false,
        asset,
      })),
  ];
  const current = images.find((item) => item.id === selected) || images[0];
  const shown = images.filter(
    (item) =>
      (filter === "all" || !item.builtin) &&
      item.name.toLowerCase().includes(query.trim().toLowerCase()),
  );
  const screenReady =
    mode === "live" &&
    device.online &&
    info?.online &&
    info.healthy &&
    ["round_screen", "holographic_screen"].includes(info.profile) &&
    device.capabilities?.includes("component_ota_v1");
  const galleryReady =
    mode === "live" &&
    device.online &&
    (device.capabilities?.includes("workspace_v1") ||
      device.capabilities?.includes("remote_control_v1"));
  const awaitingPush =
    current.asset?.push && !terminalJob(current.asset.push.status);
  const activePush = assets.find(
    (asset) => asset.push && !terminalJob(asset.push.status),
  );
  const pushAsset = activePush || current.asset;
  const push = pushAsset?.push;
  const awaitingDelivery =
    current.asset?.delivery &&
    !["completed", "failed"].includes(current.asset.delivery.status);

  useEffect(() => {
    const controller = new AbortController();
    lifetime.current = controller;
    void listGalleryAssets(scope)
      .then((result) => {
        if (!controller.signal.aborted) setAssets(result);
      })
      .catch((reason) => {
        if (!controller.signal.aborted)
          setError(
            reason instanceof Error ? reason.message : "图片库读取失败。",
          );
      })
      .finally(() => {
        if (!controller.signal.aborted) setLoading(false);
      });
    if (mode === "live")
      void api
        .component(device.id, "component-1", false, controller.signal)
        .then((result) => {
          if (!controller.signal.aborted) setInfo(result);
        })
        .catch(() => {
          /* Cached component info is optional; fresh reads have an explicit result. */
        });
    return () => controller.abort();
  }, [scope, device.id, mode]);

  useEffect(() => {
    const next = Object.fromEntries(
      assets.map((asset) => [
        asset.id,
        asset.sourceUrl || URL.createObjectURL(asset.blob),
      ]),
    );
    setUrls(next);
    return () => Object.values(next).forEach((url) => URL.revokeObjectURL(url));
  }, [assets]);

  async function patch(
    id: string,
    value: Partial<Pick<GalleryAsset, "name" | "delivery" | "push">>,
  ) {
    await patchGalleryAsset(scope, id, value);
    if (!lifetime.current?.signal.aborted)
      setAssets((previous) =>
        previous.map((item) => (item.id === id ? { ...item, ...value } : item)),
      );
  }
  const polling = assets.some(
    (asset) =>
      (asset.delivery &&
        !["completed", "failed"].includes(asset.delivery.status)) ||
      (asset.push && !terminalJob(asset.push.status)),
  );
  const assetRef = useRef(assets);
  assetRef.current = assets;
  useEffect(() => {
    if (!polling || mode !== "live") return;
    const controller = new AbortController();
    let timer: number | undefined;
    const run = async () => {
      const pending = assetRef.current;
      const updates = await Promise.allSettled(
        pending.map(async (asset) => {
          if (
            asset.delivery &&
            !["completed", "failed"].includes(asset.delivery.status)
          ) {
            const result = await api.command(
              device.id,
              asset.delivery.commandId,
              controller.signal,
            );
            if (!controller.signal.aborted)
              await patch(asset.id, {
                delivery: {
                  ...asset.delivery,
                  status: result.status,
                  message: result.message,
                },
              });
          }
          if (asset.push && !terminalJob(asset.push.status)) {
            const result = await api.job(
              device.id,
              asset.push.jobId,
              controller.signal,
            );
            const percent = jobPercent(result);
            const message = result.result || undefined;
            if (
              !controller.signal.aborted &&
              (asset.push.status !== result.status ||
                asset.push.percent !== percent ||
                asset.push.message !== message ||
                asset.push.written !== result.bytes_written ||
                asset.push.total !== result.total_bytes)
            )
              await patch(asset.id, {
                push: {
                  ...asset.push,
                  status: result.status,
                  percent,
                  written: result.bytes_written,
                  total: result.total_bytes,
                  message,
                },
              });
          }
        }),
      );
      if (!controller.signal.aborted) {
        const failed = updates.find((result) => result.status === "rejected");
        setReadError(failed ? "执行状态暂时无法读取，正在等待重新连接。" : "");
        timer = window.setTimeout(() => void run(), failed ? 3000 : 800);
      }
    };
    void run();
    return () => {
      controller.abort();
      window.clearTimeout(timer);
    };
    // Poll the latest asset refs without restarting the timer on every progress update.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [polling, device.id, scope, mode]);

  function uploadProgress(name: string, signal?: AbortSignal) {
    return (written: number, total: number) => {
      if (signal?.aborted) return;
      setUpload(
        written < total
          ? {
              name,
              label: "正在上传图片",
              percent: Math.floor((written * 100) / total),
            }
          : { name, label: "正在转换图片" },
      );
    };
  }

  async function addFile(file?: File) {
    if (!file || busy) return;
    setBusy("add");
    setError("");
    const signal = lifetime.current?.signal;
    try {
      const type = await validateImageFile(file),
        id = uid();
      await verifyImagePreview(new Blob([file], { type }), signal);
      if (signal?.aborted) return;
      const asset: GalleryAsset = {
        key: `${scope}:${id}`,
        scope,
        id,
        name: file.name.replace(/\.[^.]+$/, "").slice(0, 40) || "新图片",
        blob: new Blob([file], { type }),
        createdAt: Date.now(),
      };
      if (mode === "live")
        setUpload({ name: asset.name, label: "正在上传图片" });
      await putGalleryAsset(asset, signal, uploadProgress(asset.name, signal));
      if (signal?.aborted) return;
      setAssets((previous) => [asset, ...previous]);
      setSelected(id);
      setFilter("all");
      setQuery("");
      notify(
        mode === "live"
          ? "图片已保存到设备素材库。"
          : "图片已加入浏览器素材库。",
      );
    } catch (reason) {
      if (!signal?.aborted)
        setError(reason instanceof Error ? reason.message : "图片添加失败。");
    } finally {
      if (!signal?.aborted) {
        setBusy("");
        setUpload(null);
      }
    }
  }

  async function readComponent() {
    if (mode !== "live" || busy) return;
    setBusy("probe");
    setError("");
    try {
      const result = await api.component(
        device.id,
        "component-1",
        true,
        lifetime.current?.signal,
      );
      if (lifetime.current?.signal.aborted) return;
      setInfo(result);
      setProfile(result.profile === "holographic_screen" ? "holo" : "round");
      notify("组件信息已重新读取。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted) {
        setInfo(null);
        setError(reason instanceof Error ? reason.message : "组件读取失败。");
      }
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }

  async function send(target: "gallery" | "component") {
    if (
      busy ||
      mode !== "live" ||
      (target === "component"
        ? !screenReady || activePush
        : !galleryReady || awaitingDelivery)
    )
      return;
    setBusy(target);
    setError("");
    const controller = lifetime.current;
    setUpload({ name: current.name, label: "正在准备图片" });
    let fingerprint = "";
    try {
      let asset = current.asset;
      if (!asset) {
        const response = await fetch(current.src, {
          signal: controller?.signal,
          credentials: "same-origin",
        });
        if (!response.ok) throw new Error("图片素材读取失败。");
        const blob = await response.blob();
        await validateImageFile(blob);
        asset = {
          key: `${scope}:${current.id}`,
          scope,
          id: current.id,
          name: current.name,
          blob,
          createdAt: Date.now(),
        };
        await putGalleryAsset(
          asset,
          controller?.signal,
          uploadProgress(asset.name, controller?.signal),
        );
        if (controller?.signal.aborted) return;
        setAssets((previous) => [
          asset!,
          ...previous.filter((item) => item.id !== asset!.id),
        ]);
      }
      fingerprint = `${scope}:${asset.id}:${target}`;
      if (!sendRequests.current.has(fingerprint))
        sendRequests.current.set(fingerprint, uid());
      if (target === "gallery" && asset.serverId) {
        setUpload({ name: asset.name, label: "正在连接设备图库" });
        const command = await api.sendCommand(
          device.id,
          "modules.image",
          {
            asset_id: asset.serverId,
            asset_path: `/v1/device/module-images/${asset.serverId}`,
          },
          sendRequests.current.get(fingerprint)!,
          controller?.signal,
        );
        if (!controller?.signal.aborted) {
          await patch(asset.id, {
            delivery: {
              commandId: command.command_id,
              status: command.status,
              message: command.message,
              at: Date.now(),
            },
          });
        }
        sendRequests.current.delete(fingerprint);
        return;
      }
      const content = asset.blob.size
        ? asset.blob
        : await (
            await fetch(asset.sourceUrl!, {
              credentials: "same-origin",
              signal: controller?.signal,
            })
          ).blob();
      setUpload({ name: asset.name, label: "正在上传图片" });
      const result = await api.uploadImage(
        device.id,
        content,
        asset.name,
        target,
        controller?.signal,
        sendRequests.current.get(fingerprint),
        uploadProgress(asset.name, controller?.signal),
      );
      if (controller?.signal.aborted) return;
      if ("command" in result) {
        await patch(asset.id, {
          delivery: {
            commandId: result.command.command_id,
            status: result.command.status,
            message: result.command.message,
            at: Date.now(),
          },
        });
        notify("图片已提交，正在等待设备接收。");
      } else {
        const job = result as DeviceJob;
        await patch(asset.id, {
          push: {
            jobId: job.job_id,
            status: job.status,
            percent: jobPercent(job),
            written: job.bytes_written,
            total: job.total_bytes,
            at: Date.now(),
          },
        });
        notify("推送已提交，正在等待组件确认。");
      }
      sendRequests.current.delete(fingerprint);
    } catch (reason) {
      if (
        reason instanceof ConsoleError &&
        reason.status >= 400 &&
        reason.status < 500
      )
        sendRequests.current.delete(fingerprint);
      if (!controller?.signal.aborted)
        setError(
          reason instanceof Error ? reason.message : "发送失败，请重试。",
        );
    } finally {
      if (!controller?.signal.aborted) {
        setBusy("");
        setUpload(null);
      }
    }
  }

  async function download() {
    if (!current.asset) return;
    const content = current.asset.blob.size
      ? current.asset.blob
      : await (
          await fetch(current.asset.sourceUrl!, { credentials: "same-origin" })
        ).blob();
    const url = URL.createObjectURL(content),
      anchor = document.createElement("a");
    const extension =
      (
        {
          "image/png": "png",
          "image/gif": "gif",
          "image/jpeg": "jpg",
          "image/webp": "webp",
        } as Record<string, string>
      )[current.asset.blob.type] || "png";
    anchor.href = url;
    anchor.download = `${current.name}.${extension}`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function drag(event: DragEvent) {
    event.preventDefault();
    if (!busy) setDragging(true);
  }
  return (
    <div className="gallery-page">
      <section className="card gallery-library">
        <header className="gallery-heading">
          <div>
            <h2>组件图库</h2>
            <p>
              {name}的素材库 ·{" "}
              {mode === "live" ? "图片保存在服务器" : "图片保存在此浏览器"}
            </p>
          </div>
          <button
            className="button accent"
            disabled={!!busy || loading}
            onClick={() => inputRef.current?.click()}
          >
            <ImagePlus size={16} />
            {busy === "add" ? "正在添加" : "添加图片"}
          </button>
          <input
            ref={inputRef}
            type="file"
            className="sr-only"
            accept="image/png,image/gif,image/jpeg,image/webp"
            aria-label="添加图库图片"
            disabled={!!busy}
            onChange={(event) => {
              void addFile(event.target.files?.[0]);
              event.target.value = "";
            }}
          />
        </header>
        <div className="gallery-toolbar">
          <div className="feature-segments">
            <button
              type="button"
              className={filter === "all" ? "selected" : ""}
              aria-pressed={filter === "all"}
              onClick={() => setFilter("all")}
            >
              全部素材 <span>{images.length}</span>
            </button>
            <button
              type="button"
              className={filter === "custom" ? "selected" : ""}
              aria-pressed={filter === "custom"}
              onClick={() => setFilter("custom")}
            >
              我的图片
            </button>
          </div>
          <label className="feature-search gallery-search">
            <Search size={14} />
            <input
              aria-label="搜索图库"
              placeholder="找一张图片"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
          </label>
        </div>
        {error && (
          <p className="portal-alert" role="alert">
            <CircleAlert size={16} />
            {error}
          </p>
        )}
        <div
          className={`gallery-drop-region ${dragging ? "is-dragging" : ""}`}
          onDragOver={drag}
          onDragLeave={(event) => {
            if (
              !event.currentTarget.contains(event.relatedTarget as Node | null)
            )
              setDragging(false);
          }}
          onDrop={(event) => {
            event.preventDefault();
            setDragging(false);
            void addFile(event.dataTransfer.files[0]);
          }}
        >
          <div className="gallery-grid">
            {shown.map((item) => (
              <button
                key={item.id}
                className={`gallery-tile ${current.id === item.id ? "selected" : ""}`}
                type="button"
                aria-pressed={current.id === item.id}
                aria-label={`预览${item.name}`}
                onClick={() => setSelected(item.id)}
              >
                <span className="gallery-tile-image">
                  {item.src ? (
                    <img src={item.src} alt="" loading="lazy" />
                  ) : (
                    <Images size={26} />
                  )}
                  <i>
                    {item.builtin
                      ? "App 素材"
                      : item.asset?.blob.type === "image/gif"
                        ? "GIF"
                        : "图片"}
                  </i>
                  {current.id === item.id && (
                    <b>
                      <Check size={12} />
                    </b>
                  )}
                </span>
                <strong>{item.name}</strong>
                <small>
                  {item.asset?.delivery?.status === "completed"
                    ? "已加入设备图库"
                    : item.builtin
                      ? "内置精选"
                      : formatBytes(item.asset?.blob.size)}
                </small>
              </button>
            ))}
            <button
              className="gallery-upload-tile"
              disabled={!!busy || loading}
              onClick={() => inputRef.current?.click()}
            >
              <Upload size={22} strokeWidth={1.4} />
              <strong>放一张喜欢的图片</strong>
              <small>点击选择，或拖放到这里</small>
              <span>PNG · GIF · JPG · WEBP</span>
            </button>
          </div>
          {dragging && (
            <div className="gallery-drag-overlay">
              <ImagePlus size={34} />
              <strong>松开鼠标，添加图片</strong>
            </div>
          )}
          {!shown.length && (
            <p className="gallery-no-results">
              {query
                ? "没有找到这张图片，换个关键词试试。"
                : "还没有自己的图片，从添加一张开始。"}
            </p>
          )}
        </div>
        <footer className="gallery-library-footer">
          <span>
            <Images size={14} /> 单张不超过 16 MB
          </span>
          <span>切换设备后，素材与发送记录独立保存。</span>
        </footer>
      </section>
      <aside className="gallery-inspector">
        <section className="preview-panel gallery-preview-card">
          <header>
            <h2>效果预览</h2>
            <div
              className="gallery-profile-switch"
              role="group"
              aria-label="图片适配预览"
            >
              <button
                type="button"
                aria-pressed={profile === "round"}
                className={profile === "round" ? "selected" : ""}
                onClick={() => setProfile("round")}
              >
                笔筒圆屏
              </button>
              <button
                type="button"
                aria-pressed={profile === "holo"}
                className={profile === "holo" ? "selected" : ""}
                onClick={() => setProfile("holo")}
              >
                全息屏
              </button>
            </div>
          </header>
          <div className={`gallery-screen-preview ${profile}`}>
            {current.src ? (
              <img src={current.src} alt={`${current.name}适配效果`} />
            ) : (
              <Images size={32} />
            )}
          </div>
          <h3>{current.name}</h3>
          <p>
            {profile === "round"
              ? "240 × 240 · 圆形屏幕"
              : "320 × 240 · 全息屏幕"}
          </p>
        </section>
        <section className="card gallery-send-card">
          <div className="maintenance-card-heading">
            <span className="maintenance-heading-icon">
              <Monitor size={18} />
            </span>
            <div>
              <h3>推送到{name}</h3>
              <p>
                {mode === "demo"
                  ? "当前为界面预览"
                  : !device.online
                    ? "设备离线"
                    : info
                      ? `${info.profile === "holographic_screen" ? "全息屏" : info.profile === "round_screen" ? "笔筒圆屏" : "当前组件不支持图片"} · ${info.healthy ? "最近读取正常" : "组件未就绪"}`
                      : "等待读取组件信息"}
              </p>
            </div>
            <button
              type="button"
              className="icon-button"
              aria-label="读取当前组件"
              disabled={mode !== "live" || !device.online || !!busy}
              onClick={() => void readComponent()}
            >
              <RefreshCw
                size={16}
                className={busy === "probe" ? "spinning" : ""}
              />
            </button>
          </div>
          <button
            type="button"
            className="button secondary gallery-send-button"
            disabled={!galleryReady || !!busy || !!awaitingDelivery}
            onClick={() => void send("gallery")}
          >
            {busy === "gallery" ? (
              <LoaderCircle size={15} className="spinning" />
            ) : (
              <Upload size={15} />
            )}
            加入设备图库
          </button>
          <button
            type="button"
            className="button primary gallery-send-button"
            disabled={!screenReady || !!busy || !!activePush}
            onClick={() => void send("component")}
          >
            {busy === "component" ? (
              <LoaderCircle size={15} className="spinning" />
            ) : (
              <Send size={15} />
            )}
            推送到当前屏幕
          </button>
          <p className="gallery-send-note">
            {mode === "demo"
              ? "预览模式可管理本地图片，登录后再发送到真实设备。"
              : "加入设备图库后可在 App 选用；推送到屏幕会更新当前插入的屏幕组件。"}
          </p>
          {current.asset?.delivery && (
            <div
              className={`gallery-transfer-status ${current.asset.delivery.status === "completed" ? "is-success" : current.asset.delivery.status === "failed" ? "is-error" : ""}`}
              role="status"
            >
              <Upload size={14} />
              <div>
                <strong>
                  {current.asset.delivery.status === "completed"
                    ? "已加入设备图库"
                    : current.asset.delivery.status === "failed"
                      ? "设备接收失败"
                      : current.asset.delivery.status === "delivered"
                        ? "设备正在接收图片"
                        : "等待设备接收"}
                </strong>
                <small>
                  {current.asset.delivery.status === "failed"
                    ? current.asset.delivery.message || "请检查设备连接后重试。"
                    : formatDate(current.asset.delivery.at / 1000)}
                </small>
              </div>
            </div>
          )}
          {upload ? (
            <ImageTransferProgress {...upload} />
          ) : (
            push && (
              <ImageTransferProgress
                label={imageTransferLabel(push.status)}
                name={pushAsset?.name}
                status={push.status}
                percent={push.percent}
                written={push.written}
                total={push.total}
                detail={
                  push.status === "failed"
                    ? push.message || "组件未确认显示，请重试。"
                    : undefined
                }
              />
            )
          )}
          {readError && (
            <p className="maintenance-muted-error" role="status">
              {readError}
            </p>
          )}
          {info?.observed_at && (
            <small className="gallery-observed">
              组件信息 · {formatDate(info.observed_at)}
            </small>
          )}
        </section>
        {!current.builtin && (
          <div className="gallery-asset-actions">
            <button
              type="button"
              disabled={!!busy}
              onClick={() => {
                setNewName(current.name);
                setDialogError("");
                setEditing("rename");
              }}
            >
              <Pencil size={14} />
              重命名
            </button>
            <button type="button" onClick={() => void download()}>
              <Download size={14} />
              下载
            </button>
            <button
              type="button"
              disabled={!!busy || !!awaitingPush || !!awaitingDelivery}
              onClick={() => {
                setDialogError("");
                setEditing("remove");
              }}
            >
              <Trash2 size={14} />
              移除
            </button>
          </div>
        )}
      </aside>
      {editing && (
        <SnapshotDialog
          title={editing === "rename" ? "换一个图片名称" : "从素材库移除图片？"}
          close={() => setEditing(null)}
          closeLabel="关闭图片编辑"
          note={
            editing === "remove"
              ? "移除素材库记录，已发送到设备的图片会保留。"
              : "修改网页素材名称，不会修改设备中已有的图片名称。"
          }
        >
          <form
            className="portal-edit-form"
            onSubmit={async (event) => {
              event.preventDefault();
              if (busy) return;
              setBusy("edit");
              setDialogError("");
              try {
                if (editing === "rename")
                  await patch(current.id, { name: newName.trim() });
                else {
                  await removeGalleryAsset(scope, current.id);
                  if (!lifetime.current?.signal.aborted) {
                    setAssets((previous) =>
                      previous.filter((item) => item.id !== current.id),
                    );
                    setSelected(builtins[0].id);
                  }
                }
                if (!lifetime.current?.signal.aborted) {
                  setEditing(null);
                  notify(
                    editing === "rename"
                      ? "图片名称已更新。"
                      : "图片已从素材库移除。",
                  );
                }
              } catch (reason) {
                if (!lifetime.current?.signal.aborted)
                  setDialogError(
                    reason instanceof Error ? reason.message : "保存失败。",
                  );
              } finally {
                if (!lifetime.current?.signal.aborted) setBusy("");
              }
            }}
          >
            {editing === "rename" ? (
              <label className="portal-field">
                <span>图片名称</span>
                <input
                  autoFocus
                  maxLength={40}
                  required
                  value={newName}
                  onChange={(event) => setNewName(event.target.value)}
                />
              </label>
            ) : (
              <p className="dialog-intro">{current.name}</p>
            )}
            {dialogError && (
              <p className="portal-alert" role="alert">
                {dialogError}
              </p>
            )}
            <div className="dialog-actions">
              <button
                className="button secondary"
                type="button"
                onClick={() => setEditing(null)}
              >
                取消
              </button>
              <button
                className="button primary"
                disabled={!!busy || (editing === "rename" && !newName.trim())}
              >
                {editing === "rename" ? "保存名称" : "确认移除"}
              </button>
            </div>
          </form>
        </SnapshotDialog>
      )}
    </div>
  );
}
