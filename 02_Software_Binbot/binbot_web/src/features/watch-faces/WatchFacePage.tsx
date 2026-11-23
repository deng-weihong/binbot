import { useDeviceDraft } from "../../shared/state/useDeviceDraft";
import { useWorkspace } from "../../shared/state/workspaceContext";
import { equal } from "../../shared/model/workspaceModel";
import { useState, type FormEvent } from "react";
import { ArrowUpRight, Check, Clock3, Search, Shuffle } from "lucide-react";
import {
  idleOptions,
  selectWatchFace,
  setWatchFaceShuffle,
  watchFaces,
} from "../../shared/model/deviceFeatures";
import { FeatureSave, FeatureSwitch, SnapshotDialog } from "../../shared/ui/FeatureControls";
import type { FeaturePageProps as FeatureProps } from "../../shared/ui/featureTypes";

export function WatchFacePage({ device, data, setData, notify }: FeatureProps) {
  const workspace = useWorkspace();
  const saved = data.features.watchFace;
  const savedPreferences = {
    idleSeconds: saved.idleSeconds,
    shuffle: saved.shuffle,
  };
  const [draft, setDraft] = useDeviceDraft(savedPreferences);
  const [search, setSearch] = useState("");
  const [preview, setPreview] = useState(false);
  const [selecting, setSelecting] = useState("");
  const ready =
    device.online && (!workspace.live || workspace.snapshot?.control.ready);
  const face =
    watchFaces.find((item) => item.id === saved.themeId) || watchFaces[0];
  const faces = watchFaces.filter((item) =>
    `${item.name} ${item.english}`.toLowerCase().includes(search.toLowerCase()),
  );
  const dirty = !equal(draft, savedPreferences);
  const save = (event: FormEvent) => {
    event.preventDefault();
    if (!ready || !dirty || workspace.pending) return;
    const next = setWatchFaceShuffle(
      { ...saved, idleSeconds: draft.idleSeconds },
      draft.shuffle,
    );
    setData((previous) => ({
      ...previous,
      features: { ...previous.features, watchFace: next },
    }));
    notify("表盘设置已同步。");
  };
  async function choose(id: string) {
    if (!ready || selecting || workspace.pending) return;
    const next = selectWatchFace(saved, id);
    if (!workspace.live) {
      setData((previous) => ({
        ...previous,
        features: { ...previous.features, watchFace: next },
      }));
      return;
    }
    setSelecting(id);
    try {
      await workspace.action("watchface.set", { ...next, showNow: true });
    } finally {
      setSelecting("");
    }
  }
  return (
    <>
      <form className="watchface-layout feature-page" onSubmit={save}>
        <aside className="watchface-aside">
          <section className="card preview-panel face-preview-card">
            <div className="feature-card-heading">
              <div>
                <h2>当前表盘</h2>
              </div>
              <span className="quiet-chip">主屏</span>
            </div>
            <button
              type="button"
              className="face-large-preview"
              onClick={() => setPreview(true)}
              aria-label={`放大预览${face.name}`}
            >
              <img
                src={face.preview}
                alt={`${face.name}静态预览`}
                width="152"
                height="152"
              />
            </button>
            <div className="face-current-name">
              <div>
                <h3>{face.name}</h3>
                <p>{face.english || "Expressions"}</p>
              </div>
              <button
                className="icon-button dark"
                type="button"
                aria-label="放大表盘预览"
                onClick={() => setPreview(true)}
              >
                <ArrowUpRight size={18} />
              </button>
            </div>
            <p className="feature-note">静态缩略图 · 时间及天气为示意数据</p>
          </section>
          <section className="card face-preferences">
            <div className="feature-card-heading">
              <h2>显示习惯</h2>
              <Clock3 size={18} strokeWidth={1.5} />
            </div>
            <label className="feature-field" htmlFor="face-idle">
              <span>闲置后显示</span>
              <select
                id="face-idle"
                value={draft.idleSeconds}
                onChange={(event) =>
                  setDraft({
                    ...draft,
                    idleSeconds: Number(event.target.value),
                  })
                }
              >
                {idleOptions.map((option) => (
                  <option key={option.value} value={option.value}>
                    {option.label}
                  </option>
                ))}
              </select>
            </label>
            <div className="feature-preference">
              <div>
                <strong>随机轮换</strong>
                <small>从 31 款待机表盘中随机选择</small>
              </div>
              <FeatureSwitch
                label="随机轮换表盘"
                checked={draft.shuffle}
                onChange={() => setDraft({ ...draft, shuffle: !draft.shuffle })}
              />
            </div>
            <p className="feature-note">
              手动选择表盘会关闭随机轮换。小伴表情不参与随机。
            </p>
          </section>
          <FeatureSave
            online={device.online}
            dirty={dirty}
            reset={() => setDraft(savedPreferences)}
          />
        </aside>
        <section className="card face-library">
          <div className="feature-card-heading">
            <div>
              <h2>
                表盘图库<span className="feature-count">{watchFaces.length} 款</span>
              </h2>
              <p role="status">
                {selecting
                  ? "正在切换设备表盘…"
                  : "点选表盘后立即在设备上显示。"}
              </p>
            </div>
            <Shuffle size={21} strokeWidth={1.5} />
          </div>
          <div className="feature-toolbar">
            <span className="feature-muted">全部表盘 · {faces.length} 款</span>
            <label className="feature-search">
              <Search size={15} />
              <input
                type="search"
                aria-label="搜索表盘"
                placeholder="搜索表盘"
                value={search}
                onChange={(event) => setSearch(event.target.value)}
              />
            </label>
          </div>
          <div className="face-grid">
            {faces.map((item) => (
              <button
                key={item.id}
                className={`face-option ${saved.themeId === item.id ? "selected" : ""}`}
                type="button"
                aria-pressed={saved.themeId === item.id}
                aria-busy={selecting === item.id}
                disabled={!ready || !!selecting || workspace.pending}
                onClick={() => void choose(item.id)}
              >
                <span className="face-thumbnail">
                  <img
                    src={item.preview}
                    alt=""
                    loading="lazy"
                    width="152"
                    height="152"
                  />
                  {saved.themeId === item.id && (
                    <span className="face-check">
                      <Check size={13} />
                    </span>
                  )}
                </span>
                <strong>{item.name}</strong>
                <small>{item.english || "Expressions"}</small>
              </button>
            ))}
          </div>
          {faces.length === 0 && (
            <div className="feature-empty">
              <Search size={26} />
              <h3>还没有找到这款表盘</h3>
              <p>试试「猫咪」「像素」或英文名称。</p>
            </div>
          )}
        </section>
      </form>
      {preview && (
        <SnapshotDialog title={face.name} close={() => setPreview(false)}>
          <img
            className="dialog-face-preview"
            src={face.preview}
            width="152"
            height="152"
            alt={`${face.name}静态预览`}
          />
        </SnapshotDialog>
      )}
    </>
  );
}
