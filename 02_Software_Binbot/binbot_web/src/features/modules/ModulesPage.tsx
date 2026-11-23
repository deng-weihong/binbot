import { useDeviceDraft } from "../../shared/state/useDeviceDraft";
import { useWorkspace } from "../../shared/state/workspaceContext";
import { useConsole } from "../../shared/state/consoleContext";
import { equal, record } from "../../shared/model/workspaceModel";
import { useEffect, useState, type CSSProperties, type FormEvent } from "react";
import {
  ArrowUpRight,
  Check,
  CircleCheck,
  Clock3,
  Disc3,
  ImageOff,
  Images,
  Lightbulb,
  Monitor,
  Puzzle,
  RotateCw,
  Sun,
} from "lucide-react";
import {
  lightColors,
  lightEffects,
  lightPresets,
  moduleCatalog,
  moduleImages,
  rotationOptions,
  type ModuleId,
  type ModuleSettings,
} from "../../shared/model/deviceFeatures";
import { FeatureRange, FeatureSave, FeatureSwitch } from "../../shared/ui/FeatureControls";
import type { FeaturePageProps as FeatureProps } from "../../shared/ui/featureTypes";
import ModuleGallery from "./ModuleGallery";
import {
  ImageTransferProgress,
  imageTransferLabel,
  imageTransferPercent,
} from "./ImageTransferProgress";
import { terminalJob } from "../../shared/api/consoleApi";

const moduleIcons = {
  holo: Monitor,
  pen: Puzzle,
  turntable: Disc3,
  light: Lightbulb,
};
const generatedImageName =
  /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}(?:\.(?:gif|png|jpe?g|webp))?$/i;

type ModuleImage = {
  id: string;
  name: string;
  src: string;
};

function deviceModuleImages(images: unknown): ModuleImage[] {
  if (!Array.isArray(images)) return [];
  return images.flatMap((item) => {
    const { id, name } = record(item);
    if (typeof id !== "string" || !id) return [];
    return [
      {
        id,
        name: typeof name === "string" && name.trim() ? name : id,
        src: `/v1/device/module-images/${encodeURIComponent(id)}/thumbnail.png`,
      },
    ];
  });
}

export function ModulesPage(props: FeatureProps) {
  const [view, setView] = useState<"settings" | "gallery">(() =>
    typeof window !== "undefined" && window.location.hash === "#gallery"
      ? "gallery"
      : "settings",
  );
  useEffect(() => {
    const update = () =>
      setView(window.location.hash === "#gallery" ? "gallery" : "settings");
    window.addEventListener("popstate", update);
    window.addEventListener("hashchange", update);
    return () => {
      window.removeEventListener("popstate", update);
      window.removeEventListener("hashchange", update);
    };
  }, []);
  return (
    <div className="modules-workspace">
      <nav className="section-nav" aria-label="组件内容分类">
        {(
          [
            { id: "settings", label: "组件设置", icon: Puzzle },
            { id: "gallery", label: "图片库", icon: Images },
          ] as const
        ).map(({ id, label, icon: Icon }) => (
          <button
            key={id}
            type="button"
            className={view === id ? "selected" : ""}
            aria-pressed={view === id}
            onClick={() => {
              setView(id);
              window.history.replaceState(
                {},
                "",
                `${window.location.pathname}${id === "gallery" ? "#gallery" : ""}`,
              );
            }}
          >
            <Icon size={15} />
            {label}
          </button>
        ))}
      </nav>
      {view === "gallery" ? (
        <ModuleGallery
          device={props.device}
          name={props.data.deviceName}
          notify={props.notify}
        />
      ) : (
        <ModuleSettingsPage {...props} />
      )}
    </div>
  );
}

function ModuleSettingsPage({ device, data, setData, notify }: FeatureProps) {
  const { mode } = useConsole();
  const workspace = useWorkspace();
  const reportedModules = record(workspace.snapshot?.reported.modules);
  const imageOptions: readonly ModuleImage[] =
    mode === "live" ? deviceModuleImages(reportedModules.images) : moduleImages;
  const saved = data.features.modules;
  const [selected, setSelected] = useState<ModuleId>(saved.connected || "pen");
  const [draft, setDraft] = useDeviceDraft(saved);
  const [imageRequest, setImageRequest] = useState<{
    profile: number;
    imageId: string;
    previousId: unknown;
  } | null>(null);
  const module = moduleCatalog.find((item) => item.id === selected)!;
  const connected = saved.connected;
  const available = device.online && connected === selected;
  const dirty = !equal(draft[selected], saved[selected]);
  const displayModule =
    selected === "pen" || selected === "holo" ? selected : undefined;
  const previewImage =
    displayModule &&
    imageOptions.find((image) => image.id === draft[displayModule].imageId);
  const transfer = record(reportedModules.imageTransfer);
  const selectedProfile = selected === "pen" ? 1 : selected === "holo" ? 2 : 0;
  const activeRequest =
    imageRequest?.profile === selectedProfile ? imageRequest : null;
  const imageCommand = workspace.imageCommand;
  const tracked =
    imageCommand &&
    activeRequest &&
    imageCommand.profile === activeRequest.profile &&
    imageCommand.imageId === activeRequest.imageId
      ? imageCommand.command
      : undefined;
  const imageBusy =
    !!imageCommand &&
    !["completed", "failed"].includes(imageCommand.command.status);
  const imageCompleted = tracked?.status === "completed";
  const imageFailed =
    tracked?.status === "failed" ||
    (!workspace.pending && !tracked && !!workspace.error);
  const transferStatus =
    typeof transfer.status === "string" ? transfer.status : "";
  const shownTransfer =
    transfer.profile === selectedProfile &&
    transferStatus &&
    (activeRequest
      ? transfer.imageId === activeRequest.imageId &&
        transfer.id !== activeRequest.previousId
      : !terminalJob(transferStatus));
  const transferName = imageOptions.find(
    (image) => image.id === (activeRequest?.imageId || transfer.imageId),
  )?.name;
  const update = <K extends ModuleId>(
    key: K,
    patch: Partial<ModuleSettings[K]>,
  ) =>
    setDraft((previous) => ({
      ...previous,
      [key]: { ...previous[key], ...patch },
    }));
  const save = (event: FormEvent) => {
    event.preventDefault();
    if (!available || !dirty || workspace.pending || imageBusy) return;
    if (
      workspace.live &&
      (selected === "pen" || selected === "holo") &&
      draft[selected].imageId &&
      draft[selected].imageId !== saved[selected].imageId
    ) {
      setImageRequest({
        profile: selectedProfile,
        imageId: draft[selected].imageId,
        previousId: transfer.id,
      });
      const { imageId, ...preferences } = draft[selected];
      const { imageId: _oldImage, ...previous } = saved[selected];
      const settings = !equal(preferences, previous)
        ? { features: { modules: { [selected]: preferences } } }
        : undefined;
      void workspace.pushModuleImage(selectedProfile, imageId, settings);
      return;
    }
    setImageRequest(null);
    setData((previous) => ({
      ...previous,
      features: {
        ...previous.features,
        modules: { ...previous.features.modules, [selected]: draft[selected] },
      },
    }));
    notify(`${module.name}设置已同步。`);
  };
  const simulateConnection = (value: string) => {
    const next = moduleCatalog.find((item) => item.id === value)?.id || null;
    setData((previous) => ({
      ...previous,
      features: {
        ...previous.features,
        modules: { ...previous.features.modules, connected: next },
      },
    }));
    if (next) setSelected(next);
    notify("演示插槽已切换，未连接真实硬件。");
  };
  return (
    <form className="modules-page feature-page" onSubmit={save}>
      <div className="module-slot">
        <span>
          <Puzzle size={18} strokeWidth={1.5} />
          <strong>扩展插槽</strong>
          <i
            className={`status-dot ${!device.online || !connected ? "offline" : ""}`}
          />
          {!device.online
            ? "设备离线"
            : connected
              ? `${moduleCatalog.find((item) => item.id === connected)?.name}已插入`
              : "暂无组件"}
        </span>
        {mode === "demo" && (
          <label>
            演示接入
            <select
              aria-label="模拟接入组件"
              value={connected || "none"}
              onChange={(event) => simulateConnection(event.target.value)}
            >
              <option value="none">未插入</option>
              {moduleCatalog.map((item) => (
                <option key={item.id} value={item.id}>
                  {item.name}
                </option>
              ))}
            </select>
          </label>
        )}
      </div>
      <div className="module-workbench">
        <aside className="card module-picker">
          <header>
            <h2>扩展组件</h2>
          </header>
          <div className="module-choices" aria-label="选择组件">
            {moduleCatalog.map((item) => {
              const Icon = moduleIcons[item.id];
              return (
                <button
                  type="button"
                  key={item.id}
                  className={`module-choice ${selected === item.id ? "selected" : ""}`}
                  onClick={() => setSelected(item.id)}
                  aria-pressed={selected === item.id}
                >
                  <span className="module-choice-icon">
                    <Icon size={26} strokeWidth={1.4} />
                  </span>
                  <span>
                    <small>{item.english}</small>
                    <strong>{item.name}</strong>
                    <em>
                      {device.online && connected === item.id
                        ? "已插入"
                        : "未连接"}
                    </em>
                  </span>
                  <ArrowUpRight size={19} strokeWidth={1.5} />
                </button>
              );
            })}
          </div>
        </aside>
        <section className="preview-panel module-preview-card">
          <header>
            <h2>{module.name}</h2>
            <span className="module-preview-badge">效果预览</span>
          </header>
          <ModulePreview
            selected={selected}
            settings={draft}
            image={previewImage}
          />
          <div className="module-preview-description">
            <span>
              {selected === "holo"
                ? "全息屏 · 320 × 240"
                : selected === "pen"
                  ? "圆形屏 · 240 × 240"
                  : selected === "turntable"
                    ? draft.turntable.direction === "stop"
                      ? "转盘已停止"
                      : `转动速度 · ${draft.turntable.speed}%`
                    : draft.light.enabled
                      ? `${draft.light.effect} · 亮度 ${draft.light.brightness}%`
                      : "灯光已关闭"}
            </span>
            <i className={`status-dot ${!available ? "offline" : ""}`} />
            <small>
              {available
                ? "组件已就位"
                : !device.online
                  ? "设备离线"
                  : "等待插入"}
            </small>
          </div>
          {selected === "pen" && draft.pen.mode === "info" && (
            <div className="module-preview-note">
              时间与天气模式下，随机换图暂停。
            </div>
          )}
        </section>
        <section className="card module-settings">
          <div className="feature-card-heading">
            <div>
              <h2>
                {selected === "light"
                  ? "灯光与色彩"
                  : selected === "turntable"
                    ? "转动方式"
                    : "显示内容"}
              </h2>
            </div>
            <span
              className={`quiet-chip ${available ? "module-connected" : ""}`}
            >
              {available ? "已插入" : "设置预览"}
            </span>
          </div>
          <fieldset disabled={!available}>
            <legend className="sr-only">{module.name}设置</legend>
            {selected === "pen" && (
              <>
                <div className="feature-segments module-mode">
                  {(
                    [
                      { id: "info", label: "时间与天气" },
                      { id: "image", label: "显示图片" },
                    ] as const
                  ).map((item) => (
                    <button
                      type="button"
                      key={item.id}
                      aria-pressed={draft.pen.mode === item.id}
                      className={draft.pen.mode === item.id ? "selected" : ""}
                      onClick={() => update("pen", { mode: item.id })}
                    >
                      {item.id === "info" ? (
                        <Clock3 size={16} />
                      ) : (
                        <Images size={16} />
                      )}
                      {item.label}
                    </button>
                  ))}
                </div>
                {draft.pen.mode === "info" && (
                  <div className="pen-face-options">
                    {(
                      [
                        { id: "both", label: "时间 + 天气" },
                        { id: "time", label: "仅时间" },
                        { id: "weather", label: "仅天气" },
                      ] as const
                    ).map((item) => (
                      <button
                        className={draft.pen.face === item.id ? "selected" : ""}
                        type="button"
                        key={item.id}
                        aria-pressed={draft.pen.face === item.id}
                        onClick={() => update("pen", { face: item.id })}
                      >
                        <PenFace face={item.id} compact />
                        <strong>{item.label}</strong>
                        {draft.pen.face === item.id && <Check size={13} />}
                      </button>
                    ))}
                  </div>
                )}
              </>
            )}
            {displayModule &&
              (displayModule === "holo" || draft.pen.mode === "image") && (
                <>
                  <div className="feature-subheading">
                    <h3 id="module-image-title">
                      {mode === "live" ? "设备图库" : "图片内容"}
                    </h3>
                    {imageOptions.length > 0 && (
                      <span>{imageOptions.length} 张</span>
                    )}
                  </div>
                  {imageOptions.length ? (
                    <div
                      className="module-image-options"
                      role="group"
                      aria-labelledby="module-image-title"
                    >
                      {imageOptions.map((image, index) => {
                        const chosen = previewImage?.id === image.id;
                        return (
                          <button
                            key={image.id}
                            type="button"
                            className={chosen ? "selected" : ""}
                            aria-pressed={chosen}
                            title={image.name}
                            onClick={() =>
                              update(displayModule, { imageId: image.id })
                            }
                          >
                            <span className="module-image-thumbnail">
                              <ModulePicture image={image} thumbnail />
                              {chosen && (
                                <Check
                                  className="module-image-check"
                                  size={13}
                                  aria-hidden="true"
                                />
                              )}
                            </span>
                            <span className="module-image-name">
                              {generatedImageName.test(image.name)
                                ? `图片 ${index + 1}`
                                : image.name}
                            </span>
                          </button>
                        );
                      })}
                    </div>
                  ) : (
                    <p className="feature-note">
                      设备图库暂无图片，可在「图片库」中添加后发送到设备。
                    </p>
                  )}
                  <label className="feature-field module-rotation">
                    <span>随机换图间隔</span>
                    <select
                      value={draft[displayModule].intervalSeconds}
                      onChange={(event) =>
                        update(displayModule, {
                          intervalSeconds: Number(event.target.value),
                        })
                      }
                    >
                      {rotationOptions.map((option) => (
                        <option key={option.value} value={option.value}>
                          {option.label}
                        </option>
                      ))}
                    </select>
                  </label>
                </>
              )}
            {selected === "turntable" && (
              <>
                <div className="feature-subheading">
                  <h3>转动方向</h3>
                  <span>选择方向与速度</span>
                </div>
                <div className="motor-directions">
                  {(
                    [
                      {
                        id: "counterclockwise",
                        label: "逆时针",
                        icon: RotateCw,
                      },
                      { id: "stop", label: "停止", icon: CircleCheck },
                      { id: "clockwise", label: "顺时针", icon: RotateCw },
                    ] as const
                  ).map((item) => (
                    <button
                      type="button"
                      key={item.id}
                      aria-pressed={draft.turntable.direction === item.id}
                      className={
                        draft.turntable.direction === item.id ? "selected" : ""
                      }
                      onClick={() =>
                        update("turntable", { direction: item.id })
                      }
                    >
                      <item.icon size={22} strokeWidth={1.5} />
                      <strong>{item.label}</strong>
                    </button>
                  ))}
                </div>
                <FeatureRange
                  label="转动速度"
                  value={draft.turntable.speed}
                  onChange={(speed) => update("turntable", { speed })}
                />
                <p className="feature-note">
                  停止时保留速度设置，下次转动时继续使用。
                </p>
              </>
            )}
            {selected === "light" && (
              <>
                <div className="feature-preference light-power">
                  <div>
                    <strong>氛围灯开关</strong>
                    <small>为桌面留一点喜欢的光</small>
                  </div>
                  <FeatureSwitch
                    label="氛围灯开关"
                    checked={draft.light.enabled}
                    onChange={() =>
                      update("light", { enabled: !draft.light.enabled })
                    }
                  />
                </div>
                <fieldset disabled={!draft.light.enabled}>
                  <legend className="sr-only">灯光效果</legend>
                  <div className="feature-subheading">
                    <h3>常用氛围</h3>
                    <span>点一下，切换心情</span>
                  </div>
                  <div className="light-presets">
                    {lightPresets.map((preset) => (
                      <button
                        type="button"
                        key={preset.name}
                        onClick={() =>
                          update("light", {
                            enabled: true,
                            color: preset.color,
                            effect: preset.effect,
                            brightness: preset.brightness,
                            speed: preset.speed,
                          })
                        }
                      >
                        <i style={{ background: preset.color }} />
                        {preset.name}
                      </button>
                    ))}
                  </div>
                  <div className="feature-subheading">
                    <h3>灯光效果</h3>
                  </div>
                  <div className="feature-segments light-effects">
                    {lightEffects.map((effect) => (
                      <button
                        type="button"
                        key={effect}
                        className={
                          draft.light.effect === effect ? "selected" : ""
                        }
                        aria-pressed={draft.light.effect === effect}
                        onClick={() => update("light", { effect })}
                      >
                        {effect}
                      </button>
                    ))}
                  </div>
                  <div className="light-colors" aria-label="灯光颜色">
                    {lightColors.map((color, index) => (
                      <button
                        key={color}
                        type="button"
                        style={{ background: color }}
                        className={
                          draft.light.color === color ? "selected" : ""
                        }
                        onClick={() => update("light", { color })}
                        aria-label={
                          [
                            "珊瑚橙",
                            "暖橙",
                            "暖黄",
                            "薄荷绿",
                            "晴空蓝",
                            "浅紫",
                            "樱花粉",
                            "白色",
                          ][index]
                        }
                        aria-pressed={draft.light.color === color}
                      >
                        {draft.light.color === color && <Check size={16} />}
                      </button>
                    ))}
                  </div>
                  <FeatureRange
                    label="灯光亮度"
                    value={draft.light.brightness}
                    onChange={(brightness) => update("light", { brightness })}
                  />
                  <FeatureRange
                    label="变化速度"
                    min={5}
                    disabled={draft.light.effect === "常亮"}
                    value={draft.light.speed}
                    onChange={(speed) => update("light", { speed })}
                  />
                </fieldset>
              </>
            )}
          </fieldset>
          {shownTransfer ? (
            <ImageTransferProgress
              label={imageTransferLabel(transferStatus)}
              name={transferName}
              status={transferStatus}
              percent={imageTransferPercent(
                transferStatus,
                transfer.bytesWritten,
                transfer.totalBytes,
              )}
              written={
                typeof transfer.bytesWritten === "number"
                  ? transfer.bytesWritten
                  : undefined
              }
              total={
                typeof transfer.totalBytes === "number"
                  ? transfer.totalBytes
                  : undefined
              }
            />
          ) : activeRequest &&
            (workspace.pending || workspace.error || tracked) ? (
            <ImageTransferProgress
              label={
                imageCompleted
                  ? "组件已更新图片"
                  : imageFailed
                    ? "图片更新未确认"
                    : "等待设备开始更新"
              }
              name={transferName}
              status={
                imageCompleted
                  ? "succeeded"
                  : imageFailed
                    ? "failed"
                    : undefined
              }
              percent={imageCompleted ? 100 : undefined}
              detail={
                tracked?.status === "failed" ? tracked.message : workspace.error
              }
            />
          ) : null}
          <FeatureSave
            online={device.online}
            dirty={dirty}
            disabled={!available || imageBusy}
            unavailable={
              connected !== selected
                ? `插入${module.name}后即可保存设置。`
                : undefined
            }
            reset={() =>
              setDraft((previous) => ({
                ...previous,
                [selected]: saved[selected],
              }))
            }
          />
        </section>
      </div>
    </form>
  );
}

function PenFace({
  face,
  compact = false,
}: {
  face: ModuleSettings["pen"]["face"];
  compact?: boolean;
}) {
  return (
    <div className={`pen-face pen-face-${face} ${compact ? "compact" : ""}`}>
      {face !== "weather" && (
        <>
          <small>MON · 11 / 16</small>
          <strong>
            20<span>:</span>00
          </strong>
        </>
      )}
      {face !== "time" && (
        <span className="pen-weather">
          <Sun size={compact ? 16 : 28} strokeWidth={1.3} />
          <span>晴 · 23°</span>
        </span>
      )}
      {face === "time" && (
        <span className="pen-time-line">留一点时间，给自己。</span>
      )}
    </div>
  );
}

function ModulePicture({
  image,
  thumbnail = false,
}: {
  image: ModuleImage;
  thumbnail?: boolean;
}) {
  const [failedSrc, setFailedSrc] = useState("");
  if (failedSrc === image.src)
    return thumbnail ? (
      <ImageOff
        size={19}
        className="module-image-unavailable"
        aria-hidden="true"
      />
    ) : (
      <span>无法预览这张图片</span>
    );
  return (
    <img
      src={image.src}
      alt={thumbnail ? "" : image.name}
      loading={thumbnail ? "lazy" : undefined}
      decoding="async"
      onError={() => setFailedSrc(image.src)}
    />
  );
}

function ModulePreview({
  selected,
  settings,
  image,
}: {
  selected: ModuleId;
  settings: ModuleSettings;
  image?: ModuleImage;
}) {
  if (selected === "pen" && settings.pen.mode === "info")
    return (
      <div className="module-visual pen-visual">
        <PenFace face={settings.pen.face} />
      </div>
    );
  if (selected === "pen" || selected === "holo") {
    return (
      <div
        className={`module-visual image-visual ${selected === "pen" ? "round" : ""}`}
      >
        {image ? (
          <ModulePicture key={image.src} image={image} />
        ) : (
          <span>请选择一张设备图片</span>
        )}
      </div>
    );
  }
  if (selected === "turntable")
    return (
      <div className="module-visual turntable-visual">
        <div
          className={`turntable-disc ${settings.turntable.direction !== "stop" && settings.turntable.speed > 0 ? "rotating" : ""}`}
          style={
            {
              "--rotation-duration": `${22 - settings.turntable.speed * 0.18}s`,
              "--rotation-direction":
                settings.turntable.direction === "counterclockwise"
                  ? "reverse"
                  : "normal",
            } as CSSProperties
          }
        >
          <i />
          <span />
          <b>binbot</b>
        </div>
        <span className="turntable-base" />
      </div>
    );
  return (
    <div
      className={`module-visual lamp-visual ${settings.light.enabled ? "is-on" : ""}`}
      style={
        {
          "--lamp-color": settings.light.color,
          "--lamp-strength": settings.light.brightness / 100,
        } as CSSProperties
      }
    >
      <div className="lamp-glow" />
      <div className="lamp-diffuser" />
      <div className="lamp-base" />
    </div>
  );
}
