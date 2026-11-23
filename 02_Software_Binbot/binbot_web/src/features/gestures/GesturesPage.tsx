import { useDeviceDraft } from "../../shared/state/useDeviceDraft";
import { useState, type CSSProperties, type FormEvent } from "react";
import {
  ArrowDown,
  ArrowRight,
  ArrowUpRight,
  Camera,
  Check,
  ChevronDown,
  CircleOff,
  Lightbulb,
  Mic,
  Monitor,
  Moon,
  Play,
  ShieldCheck,
  SkipBack,
  SkipForward,
  Sparkles,
  Timer,
  VolumeX,
  Waves,
} from "lucide-react";
import { FeatureSave, FeatureSwitch, SnapshotDialog } from "../../shared/ui/FeatureControls";
import {
  gestureActions,
  gestureCatalog,
  type GestureAction,
  type GestureId,
  type GestureSettings,
} from "../../shared/model/deviceFeatures";
import type { FeaturePageProps } from "../../shared/ui/featureTypes";

const actionIcons = {
  "pc-on": Monitor,
  "pc-sleep": Moon,
  lamp: Lightbulb,
  play: Play,
  next: SkipForward,
  previous: SkipBack,
  photo: Camera,
  focus: Timer,
  wake: Mic,
  noise: Waves,
  mute: VolumeX,
  nothing: CircleOff,
};

export default function GesturesPage({
  device,
  data,
  setData,
  notify,
}: FeaturePageProps) {
  const saved = data.features.gestures;
  const [draft, setDraft] = useDeviceDraft(saved);
  const [selected, setSelected] = useState<GestureId>("thumb");
  const [picker, setPicker] = useState<GestureId | null>(null);
  const [preview, setPreview] = useState(false);
  const dirty = JSON.stringify(draft) !== JSON.stringify(saved);
  const gesture = gestureCatalog.find((item) => item.id === selected)!;
  const action = gestureActions.find(
    (item) => item.id === draft.mappings[selected].action,
  )!;
  const ActionIcon = actionIcons[action.id];
  const enabled = draft.enabled && draft.mappings[selected].enabled;
  const updateMapping = (
    id: GestureId,
    patch: Partial<GestureSettings["mappings"][GestureId]>,
  ) =>
    setDraft((previous) => ({
      ...previous,
      mappings: {
        ...previous.mappings,
        [id]: { ...previous.mappings[id], ...patch },
      },
    }));
  const save = (event: FormEvent) => {
    event.preventDefault();
    if (!device.online || !dirty) return;
    setData((previous) => ({
      ...previous,
      features: { ...previous.features, gestures: draft },
    }));
    notify("手势设置已同步。");
  };
  return (
    <>
      <form
        className="gesture-layout gesture-page feature-page"
        onSubmit={save}
      >
        <section className="card gesture-mappings">
          <div className="feature-card-heading">
            <div>
              <h2>手势与操作</h2>
            </div>
            <FeatureSwitch
              label="手势识别"
              checked={draft.enabled}
              onChange={() => setDraft({ ...draft, enabled: !draft.enabled })}
            />
          </div>
          <div className="gesture-list-status">
            <span>
              <i className={`status-dot ${draft.enabled ? "" : "offline"}`} />
              {draft.enabled ? "手势识别已开启" : "手势识别已暂停"}
            </span>
            <small>
              {
                gestureCatalog.filter((item) => draft.mappings[item.id].enabled)
                  .length
              }{" "}
              / 5 个手势启用
            </small>
          </div>
          <fieldset disabled={!draft.enabled}>
            <legend className="sr-only">手势与对应操作</legend>
            <div className="gesture-table-heading">
              <span>识别手势</span>
              <span>对应动作</span>
              <span>启用</span>
            </div>
            <div className="gesture-rows">
              {gestureCatalog.map((item) => {
                const mapping = draft.mappings[item.id];
                const currentAction = gestureActions.find(
                  (candidate) => candidate.id === mapping.action,
                )!;
                const RowActionIcon = actionIcons[currentAction.id];
                return (
                  <div
                    className={`gesture-row ${selected === item.id ? "is-selected" : ""} ${!mapping.enabled ? "is-disabled" : ""}`}
                    key={item.id}
                  >
                    <button
                      type="button"
                      className="gesture-identity"
                      onClick={() => setSelected(item.id)}
                      aria-label={`查看${item.name}手势详情`}
                      aria-pressed={selected === item.id}
                    >
                      <span className={`gesture-glyph glyph-${item.id}`}>
                        <GestureMark id={item.id} />
                      </span>
                      <span>
                        <strong>{item.name}</strong>
                        <small>
                          {item.id === "ok"
                            ? "指尖成环，轻轻示意"
                            : item.id === "thumb"
                              ? "竖起拇指，轻松回应"
                              : item.id === "palm"
                                ? "掌心朝前，保持片刻"
                                : item.id === "victory"
                                  ? "比个耶，记录此刻"
                                  : "握紧拳头，保持片刻"}
                        </small>
                      </span>
                    </button>
                    <ArrowRight
                      className="gesture-row-arrow"
                      size={17}
                      strokeWidth={1.3}
                    />
                    <button
                      type="button"
                      className={`gesture-action-button ${!currentAction.available ? "unavailable" : ""}`}
                      disabled={!mapping.enabled}
                      onClick={() => {
                        setSelected(item.id);
                        setPicker(item.id);
                      }}
                      aria-label={`修改${item.name}对应操作，当前${currentAction.name}`}
                    >
                      <RowActionIcon size={15} strokeWidth={1.5} />
                      <span>{currentAction.name}</span>
                      <ChevronDown size={13} />
                    </button>
                    <FeatureSwitch
                      label={`启用${item.name}手势`}
                      checked={mapping.enabled}
                      onChange={() =>
                        updateMapping(item.id, { enabled: !mapping.enabled })
                      }
                    />
                  </div>
                );
              })}
            </div>
          </fieldset>
          <div className="gesture-list-footnote">
            <ShieldCheck size={16} strokeWidth={1.5} />
            <p>
              电脑和台灯控制暂不可执行。其余动作会在设备识别后显示操作提示。
            </p>
          </div>
        </section>

        <aside className="gesture-detail-column">
          <section className="preview-panel gesture-detail-card">
            <header>
              <h2>{gesture.name}</h2>
              <span
                className={`gesture-detail-state ${!enabled ? "is-off" : ""}`}
              >
                <i />
                {enabled ? "已启用" : "未启用"}
              </span>
            </header>
            <div className={`gesture-detail-art ${!enabled ? "is-off" : ""}`}>
              <span />
              <span />
              <GestureMark id={selected} />
            </div>
            <p>{gesture.description}</p>
            <div className="gesture-mapped-preview">
              <span>识别后</span>
              <ActionIcon size={17} strokeWidth={1.5} />
              <strong>{enabled ? action.name : "不执行动作"}</strong>
            </div>
            <button
              type="button"
              className="gesture-preview-link"
              onClick={() => setPreview(true)}
            >
              查看识别提示
              <ArrowUpRight size={17} strokeWidth={1.5} />
            </button>
          </section>
          <section className="card gesture-preferences">
            <div className="feature-card-heading">
              <div>
                <h2>识别偏好</h2>
              </div>
              <Sparkles size={19} strokeWidth={1.5} />
            </div>
            <fieldset disabled={!draft.enabled}>
              <legend className="sr-only">识别偏好</legend>
              <div className="gesture-sensitivity-label">
                <strong id="sensitivity-label">灵敏度</strong>
                <span>
                  {draft.sensitivity === "medium"
                    ? "日常推荐"
                    : draft.sensitivity === "low"
                      ? "稳妥一点"
                      : "灵敏一点"}
                </span>
              </div>
              <div
                className="feature-segments sensitivity-options"
                role="group"
                aria-labelledby="sensitivity-label"
              >
                {(
                  [
                    { id: "low", label: "低" },
                    { id: "medium", label: "中" },
                    { id: "high", label: "高" },
                  ] as const
                ).map((item) => (
                  <button
                    type="button"
                    key={item.id}
                    className={draft.sensitivity === item.id ? "selected" : ""}
                    aria-pressed={draft.sensitivity === item.id}
                    onClick={() => setDraft({ ...draft, sensitivity: item.id })}
                  >
                    {item.label}
                  </button>
                ))}
              </div>
              <div className="feature-preference">
                <div>
                  <strong>敏感操作先确认</strong>
                  <small>在设备上确认后，再执行。</small>
                </div>
                <FeatureSwitch
                  label="敏感操作先确认"
                  checked={draft.confirmSensitive}
                  onChange={() =>
                    setDraft({
                      ...draft,
                      confirmSensitive: !draft.confirmSensitive,
                    })
                  }
                />
              </div>
            </fieldset>
          </section>
        </aside>
        <div className="gesture-save-bar">
          <FeatureSave
            online={device.online}
            dirty={dirty}
            reset={() => setDraft(saved)}
          />
        </div>
      </form>
      {picker && (
        <SnapshotDialog
          title={`「${gestureCatalog.find((item) => item.id === picker)?.name}」对应动作`}
          note="选择动作后，保存设置即可更新当前设备的手势映射。"
          close={() => setPicker(null)}
        >
          <div className="gesture-action-picker">
            {gestureActions.map((option) => {
              const Icon = actionIcons[option.id];
              const isSelected = draft.mappings[picker].action === option.id;
              return (
                <button
                  type="button"
                  className={isSelected ? "selected" : ""}
                  key={option.id}
                  aria-pressed={isSelected}
                  onClick={() => {
                    updateMapping(picker, {
                      action: option.id as GestureAction,
                    });
                    setPicker(null);
                  }}
                >
                  <Icon size={21} strokeWidth={1.4} />
                  <strong>{option.name}</strong>
                  <small>{option.available ? "设备动作" : "暂不可用"}</small>
                  {isSelected && <Check size={13} />}
                </button>
              );
            })}
          </div>
        </SnapshotDialog>
      )}
      {preview && (
        <SnapshotDialog
          title={`${gesture.name} · 手势提示`}
          close={() => setPreview(false)}
        >
          <div className="gesture-preview-flow">
            <span className="gesture-glyph">
              <GestureMark id={selected} />
            </span>
            <h3>{gesture.name}</h3>
            <p>{gesture.description}</p>
            <ArrowDown size={22} />
            <strong>{enabled ? action.name : "当前未启用"}</strong>
            {!action.available && <small>App 的此项控制服务尚未接入。</small>}
          </div>
        </SnapshotDialog>
      )}
    </>
  );
}

function GestureMark({ id }: { id: GestureId }) {
  return (
    <i
      className="gesture-mark"
      aria-hidden="true"
      style={{ maskImage: `url(/gestures/${id}.png)` } as CSSProperties}
    />
  );
}
