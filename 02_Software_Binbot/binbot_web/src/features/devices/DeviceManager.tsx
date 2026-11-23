import { useEffect, useState } from "react";
import {
  ArrowRight,
  ArrowUpRight,
  Check,
  CircleHelp,
  MapPin,
  Pencil,
  Plus,
  RefreshCw,
  Search,
  Settings2,
  WifiOff,
} from "lucide-react";
import { useConsole } from "../../shared/state/consoleContext";
import { formatDate } from "../../shared/api/consoleApi";
import type { Device } from "../../shared/model/data";
import { devicePath, navigate } from "../../shared/navigation";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";
import { PortalHeader } from "../../shared/ui/PortalHeader";
import DeviceIllustration from "../../shared/ui/DeviceIllustration";

export default function DeviceManager() {
  const { devices, mode, refresh, refreshing, error, updateLabel } =
    useConsole();
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<"all" | "online" | "offline">("all");
  const [edit, setEdit] = useState<Device | null>(null);
  const [adding, setAdding] = useState(false);
  const [name, setName] = useState(""),
    [location, setLocation] = useState(""),
    [editError, setEditError] = useState("");
  const [message, setMessage] = useState("");
  useEffect(() => {
    document.title = "BinBot · 我的设备";
  }, []);
  useEffect(() => {
    if (!message) return;
    const timer = window.setTimeout(() => setMessage(""), 4000);
    return () => window.clearTimeout(timer);
  }, [message]);
  const online = devices.filter((device) => device.online).length;
  const shown = devices.filter(
    (device) =>
      (filter === "all" ||
        (filter === "online" ? device.online : !device.online)) &&
      `${device.name} ${device.id} ${device.location}`
        .toLowerCase()
        .includes(query.trim().toLowerCase()),
  );
  return (
    <div className="portal-page device-manager">
      <PortalHeader label="我的设备" />
      <main className="portal-main">
        <div className="portal-heading">
          <h1>设备管理</h1>
          <a
            className="button secondary public-services-link"
            href="/services"
            onClick={(event) => {
              event.preventDefault();
              navigate("/services");
            }}
          >
            <Settings2 size={16} /> 公共服务设置 <ArrowUpRight size={15} />
          </a>
        </div>
        <section className="manager-summary" aria-label="设备概览">
          <div>
            <span className="manager-count">{devices.length}</span>
            <span>
              台设备
              <small>{mode === "demo" ? "示例设备" : "已接入的设备"}</small>
            </span>
          </div>
          <div className="manager-presence">
            <span>
              <i className="status-dot" />
              <strong>{online}</strong> 在线
            </span>
            <span>
              <i className="status-dot offline" />
              <strong>{devices.length - online}</strong> 离线
            </span>
          </div>
        </section>
        <div className="manager-toolbar">
          <div
            className="feature-segments manager-filters"
            role="group"
            aria-label="筛选设备"
          >
            {(
              [
                { id: "all", label: "全部设备", count: devices.length },
                { id: "online", label: "在线", count: online },
                {
                  id: "offline",
                  label: "离线",
                  count: devices.length - online,
                },
              ] as const
            ).map(({ id, label, count }) => (
              <button
                key={id}
                aria-pressed={filter === id}
                className={filter === id ? "selected" : ""}
                onClick={() => setFilter(id)}
              >
                {label}
                <span>{count}</span>
              </button>
            ))}
          </div>
          <label className="feature-search manager-search">
            <Search size={16} />
            <input
              aria-label="搜索设备"
              placeholder="搜索名称、位置或编号"
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
          </label>
          <button
            className="icon-button manager-refresh"
            type="button"
            aria-label="刷新设备列表"
            disabled={refreshing || mode === "demo"}
            onClick={() => void refresh()}
          >
            <RefreshCw size={17} className={refreshing ? "spinning" : ""} />
          </button>
          <button className="button accent" onClick={() => setAdding(true)}>
            <Plus size={16} />
            接入设备
          </button>
        </div>
        {error && (
          <p className="portal-alert" role="alert">
            {error}{" "}
            <button type="button" onClick={() => void refresh()}>
              重新读取
            </button>
          </p>
        )}
        {mode === "demo" && (
          <p className="manager-preview-note">
            <CircleHelp size={14} />{" "}
            当前展示示例设备，名称与位置仅保存在此浏览器。
          </p>
        )}
        <section className="manager-grid" aria-label="设备列表">
          {shown.map((device) => {
            const displayName = device.name;
            return (
              <article
                className="manager-device-card"
                key={device.id}
              >
                <div className="manager-device-top">
                  <span className="manager-device-status">
                    <i
                      className={`status-dot ${device.online ? "" : "offline"}`}
                    />
                    {device.online ? "在线" : "离线"}
                  </span>
                  <button
                    className="icon-button"
                    aria-label={`编辑${displayName}的名称与位置`}
                    onClick={() => {
                      setEdit(device);
                      setName(displayName);
                      setLocation(device.location);
                      setEditError("");
                    }}
                  >
                    <Pencil size={15} />
                  </button>
                </div>
                <a
                  className="manager-device-art"
                  href={devicePath(device.id)}
                  aria-label={`进入${displayName}`}
                  onClick={(event) => {
                    if (
                      event.metaKey ||
                      event.ctrlKey ||
                      event.shiftKey ||
                      event.altKey
                    )
                      return;
                    event.preventDefault();
                    navigate(devicePath(device.id));
                  }}
                >
                  <DeviceIllustration
                    color={device.color}
                    asleep={!device.online}
                  />
                  <span className="manager-device-number">{device.id}</span>
                </a>
                <div className="manager-device-content">
                  <div className="manager-device-title">
                    <h2>{displayName}</h2>
                    <span>
                      <MapPin size={12} />
                      {device.location}
                    </span>
                  </div>
                  {!device.online && device.lastSeenAt && (
                    <p>最后在线 · {formatDate(device.lastSeenAt)}</p>
                  )}
                  <a
                    className="button secondary manager-enter"
                    href={devicePath(device.id)}
                    onClick={(event) => {
                      if (
                        event.metaKey ||
                        event.ctrlKey ||
                        event.shiftKey ||
                        event.altKey
                      )
                        return;
                      event.preventDefault();
                      navigate(devicePath(device.id));
                    }}
                  >
                    <span>进入设备</span>
                    <ArrowRight size={18} />
                  </a>
                </div>
              </article>
            );
          })}
        </section>
        {!shown.length && (
          <div className="manager-empty">
            <WifiOff size={28} strokeWidth={1.3} />
            <h2>{devices.length ? "未找到匹配的设备" : "暂无已接入设备"}</h2>
            <p>
              {devices.length
                ? "换个关键词，或切换设备状态试试。"
                : "让设备连接后台后，它就会出现在这里。"}
            </p>
            {devices.length > 0 && (
              <button
                className="button secondary"
                onClick={() => {
                  setFilter("all");
                  setQuery("");
                }}
              >
                查看全部设备
              </button>
            )}
          </div>
        )}
      </main>
      {edit && (
        <SnapshotDialog
          title="给伙伴一个名字"
          close={() => setEdit(null)}
          closeLabel="关闭设备编辑"
          note={
            mode === "live"
              ? "名称与位置保存在服务器，与你的设备绑定。"
              : "演示名称与位置保存在当前浏览器。"
          }
        >
          <form
            className="portal-edit-form"
            onSubmit={async (event) => {
              event.preventDefault();
              try {
                await updateLabel(edit.id, name, location);
                setEdit(null);
                setMessage("名称与位置已保存。");
              } catch (reason) {
                setEditError(
                  reason instanceof Error ? reason.message : "保存失败。",
                );
              }
            }}
          >
            <label className="portal-field">
              <span>设备名称</span>
              <input
                autoFocus
                maxLength={20}
                required
                value={name}
                onChange={(event) => setName(event.target.value)}
              />
            </label>
            <label className="portal-field">
              <span>位置</span>
              <input
                maxLength={30}
                placeholder="例如：书房"
                value={location}
                onChange={(event) => setLocation(event.target.value)}
              />
            </label>
            {editError && (
              <p className="portal-alert" role="alert">
                {editError}
              </p>
            )}
            <div className="dialog-actions">
              <button
                type="button"
                className="button secondary"
                onClick={() => setEdit(null)}
              >
                取消
              </button>
              <button className="button primary" disabled={!name.trim()}>
                保存 <Check size={15} />
              </button>
            </div>
          </form>
        </SnapshotDialog>
      )}
      {adding && (
        <SnapshotDialog
          title="接入设备"
          close={() => setAdding(false)}
          closeLabel="关闭设备接入说明"
          note={
            mode === "demo"
              ? "退出界面预览并登录后，可查看真实接入的设备。"
              : "设备接入和凭据需先在设备端与服务器配置完成。"
          }
        >
          <ol className="device-connect-steps">
            <li>
              <span>01</span>
              <div>
                <strong>让设备连上网络</strong>
                <p>在设备的 Wi-Fi 设置中确认已连接。</p>
              </div>
            </li>
            <li>
              <span>02</span>
              <div>
                <strong>连接到同一个后台</strong>
                <p>设备使用已配置的服务地址和接入凭据建立连接。</p>
              </div>
            </li>
            <li>
              <span>03</span>
              <div>
                <strong>回到这里，找到它</strong>
                <p>设备连接成功后会自动出现，离线记录也会保留。</p>
              </div>
            </li>
          </ol>
          <div className="dialog-actions">
            <button
              type="button"
              className="button primary"
              onClick={() => {
                setAdding(false);
                if (mode === "live") void refresh();
              }}
            >
              {mode === "demo" ? "知道了" : "刷新设备列表"}
              <ArrowRight size={15} />
            </button>
          </div>
        </SnapshotDialog>
      )}
      {message && (
        <div className="toast" role="status">
          <Check size={15} />
          {message}
        </div>
      )}
    </div>
  );
}
