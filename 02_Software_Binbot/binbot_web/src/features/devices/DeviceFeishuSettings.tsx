import { useEffect, useRef, useState } from "react";
import {
  Check,
  ChevronDown,
  Eye,
  EyeOff,
  Link2,
  LoaderCircle,
  MessageCircle,
  RefreshCw,
  Unplug,
} from "lucide-react";
import { useConsole } from "../../shared/state/consoleContext";
import { createConsoleTransport, formatDate } from "../../shared/api/consoleApi";
import {
  feishuDraftKey,
  normalizeFeishuDraft,
  readFeishuDraft,
  type DeviceFeishuSnapshot,
  type FeishuConfig,
  type FeishuSecrets,
} from "../../shared/model/deviceFeishu";
import type { Device } from "../../shared/model/data";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";

const api = createConsoleTransport();
export default function DeviceFeishuSettings({
  device,
  notify,
}: {
  device: Device;
  notify: (message: string) => void;
}) {
  const { mode } = useConsole();
  const [config, setConfig] = useState<FeishuConfig>(() =>
    readFeishuDraft(mode, device.id),
  );
  const [snapshot, setSnapshot] = useState<DeviceFeishuSnapshot | null>(null);
  const [secrets, setSecrets] = useState<FeishuSecrets>({});
  const [visible, setVisible] = useState(false),
    [busy, setBusy] = useState("");
  const [error, setError] = useState(""),
    [message, setMessage] = useState("");
  const [confirm, setConfirm] = useState(false);
  const lifetime = useRef<AbortController | null>(null),
    request = useRef<AbortController | null>(null);
  const dirty =
    !snapshot ||
    JSON.stringify(config) !== JSON.stringify(snapshot.config) ||
    Object.values(secrets).some(Boolean);
  const hasSecret =
    !!snapshot?.credentials_configured.app_secret &&
    config.app_id === snapshot.config.app_id;
  const authorized = snapshot?.account.authorized === true;
  function accept(value: DeviceFeishuSnapshot) {
    setSnapshot(value);
    setConfig(value.config);
    setSecrets({});
  }
  async function load(preserveDraft = false) {
    if (mode === "demo") return;
    request.current?.abort();
    const controller = new AbortController();
    request.current = controller;
    setBusy("load");
    setError("");
    try {
      const value = await api.feishu(device.id, controller.signal);
      if (controller.signal.aborted || lifetime.current?.signal.aborted) return;
      setSnapshot(value);
      if (!preserveDraft) {
        setConfig(value.config);
        setSecrets({});
      }
    } catch (reason) {
      if (!controller.signal.aborted)
        setError(
          reason instanceof Error ? reason.message : "飞书配置读取失败。",
        );
    } finally {
      if (!controller.signal.aborted) setBusy("");
    }
  }
  useEffect(() => {
    lifetime.current = new AbortController();
    void load();
    return () => {
      lifetime.current?.abort();
      request.current?.abort();
    };
    // The settings page is recreated when switching devices.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [device.id, mode]);
  function update<K extends keyof FeishuConfig>(
    key: K,
    value: FeishuConfig[K],
  ) {
    setConfig((previous) => ({ ...previous, [key]: value }));
    setMessage("");
  }
  async function save() {
    setError("");
    setMessage("");
    if (
      config.enabled &&
      (!config.app_id.trim() ||
        (mode === "live" && !hasSecret && !secrets.app_secret?.trim()))
    ) {
      setError("请填写这台设备的 App ID 和 App Secret。");
      return;
    }
    if (config.messages_enabled && !config.chat_id.trim()) {
      setError("请填写绑定到这台设备的飞书会话 ID。");
      return;
    }
    if (mode === "demo") {
      try {
        localStorage.setItem(
          feishuDraftKey(mode, device.id),
          JSON.stringify(normalizeFeishuDraft(config)),
        );
        setSecrets({});
        setMessage("当前设备的公开草稿已保存，密钥不会保存在浏览器。");
      } catch {
        setError("浏览器无法保存草稿，请检查存储设置。");
      }
      return;
    }
    if (!snapshot) return;
    setBusy("save");
    try {
      const value = await api.saveFeishu(
        device.id,
        snapshot,
        config,
        secrets,
        lifetime.current?.signal,
      );
      if (lifetime.current?.signal.aborted) return;
      accept(value);
      setMessage("已保存到当前设备，其他设备的飞书配置保留。");
      notify("当前设备的飞书配置已保存。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted)
        setError(reason instanceof Error ? reason.message : "保存失败。");
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }
  async function authorize() {
    if (mode !== "live" || !snapshot?.config.enabled || busy || dirty) return;
    const popup = window.open("about:blank", "_blank");
    if (!popup) {
      setError("请允许浏览器打开飞书授权窗口后重试。");
      return;
    }
    popup.opener = null;
    setBusy("authorize");
    setError("");
    try {
      const url = await api.authorizeFeishu(
        device.id,
        `${window.location.origin}/api/v1/feishu/oauth/callback`,
        lifetime.current?.signal,
      );
      if (lifetime.current?.signal.aborted) {
        popup.close();
        return;
      }
      popup.location.href = url;
      setMessage("完成授权后，点击「刷新状态」查看当前设备的连接。");
    } catch (reason) {
      popup.close();
      if (!lifetime.current?.signal.aborted)
        setError(
          reason instanceof Error ? reason.message : "飞书授权暂不可用。",
        );
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }
  async function disconnect() {
    if (mode !== "live" || busy) return;
    setBusy("disconnect");
    setError("");
    try {
      const value = await api.disconnectFeishu(
        device.id,
        lifetime.current?.signal,
      );
      if (lifetime.current?.signal.aborted) return;
      accept(value);
      setConfirm(false);
      setMessage("已断开这台设备的飞书账号。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted)
        setError(reason instanceof Error ? reason.message : "断开失败。");
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }
  async function sync() {
    if (mode !== "live" || busy || dirty) return;
    setBusy("sync");
    setError("");
    try {
      await api.syncFeishu(device.id, lifetime.current?.signal);
      const value = await api.feishu(device.id, lifetime.current?.signal);
      if (lifetime.current?.signal.aborted) return;
      setSnapshot(value);
      setMessage("当前设备的飞书日历已同步。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted)
        setError(reason instanceof Error ? reason.message : "同步失败。");
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }
  return (
    <div className="device-feishu-page">
      <div className="device-feishu-intro">
        <div>
          <h2>飞书连接</h2>
          <p>应用、账号和日历只用于当前设备（{device.name}）。</p>
        </div>
      </div>
      {mode === "demo" && (
        <p className="manager-preview-note">
          界面预览 · 可保存当前示例设备的公开草稿，登录后再连接真实飞书。
        </p>
      )}
      {error && (
        <p className="portal-alert" role="alert">
          {error}
        </p>
      )}
      <div className="device-feishu-grid">
        <section
          className="card service-settings"
          aria-label="当前设备飞书配置"
        >
          <header className="service-heading">
            <div>
              <h3>应用配置</h3>
              <p>可以填写独立应用，也可以与其他设备复用同一应用。</p>
            </div>
          </header>
          <form
            onSubmit={(event) => {
              event.preventDefault();
              if (!busy) void save();
            }}
          >
            <fieldset disabled={!!busy}>
              <label className="device-feishu-switch">
                <span>启用飞书连接</span>
                <input
                  type="checkbox"
                  checked={config.enabled}
                  onChange={(event) => update("enabled", event.target.checked)}
                />
              </label>
              <div className="service-fields">
                <label className="service-field">
                  <span>App ID</span>
                  <input
                    aria-label="当前设备 App ID"
                    value={config.app_id}
                    maxLength={256}
                    placeholder="cli_…"
                    onChange={(event) => update("app_id", event.target.value)}
                  />
                </label>
                <label className="service-field">
                  <span>
                    App Secret{" "}
                    {hasSecret && (
                      <em>
                        <Check size={11} />
                        已配置
                      </em>
                    )}
                  </span>
                  <div className="service-secret">
                    <input
                      aria-label="当前设备 App Secret"
                      type={visible ? "text" : "password"}
                      autoComplete="new-password"
                      value={secrets.app_secret || ""}
                      maxLength={4096}
                      placeholder={
                        hasSecret ? "留空保留当前应用密钥" : "填写应用密钥"
                      }
                      onChange={(event) =>
                        setSecrets((previous) => ({
                          ...previous,
                          app_secret: event.target.value,
                        }))
                      }
                    />
                    <button
                      type="button"
                      aria-label={visible ? "隐藏应用密钥" : "显示应用密钥"}
                      onClick={() => setVisible(!visible)}
                    >
                      {visible ? <EyeOff size={16} /> : <Eye size={16} />}
                    </button>
                  </div>
                  <small>密钥由服务器保存，页面不会读取已有密钥。</small>
                </label>
              </div>
              <div className="device-feishu-option">
                <label className="device-feishu-switch">
                  <span>
                    同步飞书日历<small>同步到当前设备的日程</small>
                  </span>
                  <input
                    type="checkbox"
                    checked={config.calendar_enabled}
                    onChange={(event) =>
                      update("calendar_enabled", event.target.checked)
                    }
                  />
                </label>
                {config.calendar_enabled && (
                  <label className="service-field">
                    <span>日历 ID</span>
                    <input
                      aria-label="当前设备日历 ID"
                      value={config.calendar_id}
                      maxLength={256}
                      placeholder="留空使用授权账号的主日历"
                      onChange={(event) =>
                        update("calendar_id", event.target.value)
                      }
                    />
                  </label>
                )}
              </div>
              <details className="service-advanced">
                <summary>
                  飞书消息与设备控制
                  <ChevronDown size={14} />
                </summary>
                <label className="device-feishu-switch">
                  <span>接收飞书消息</span>
                  <input
                    type="checkbox"
                    checked={config.messages_enabled}
                    onChange={(event) =>
                      update("messages_enabled", event.target.checked)
                    }
                  />
                </label>
                {config.messages_enabled && (
                  <>
                    <label className="service-field">
                      <span>绑定会话 ID</span>
                      <input
                        aria-label="当前设备飞书会话 ID"
                        value={config.chat_id}
                        maxLength={256}
                        placeholder="oc_…"
                        onChange={(event) =>
                          update("chat_id", event.target.value)
                        }
                      />
                      <small>
                        此会话中的消息交给当前设备。同一应用中的一个会话只能绑定一台设备。
                      </small>
                    </label>
                    <label className="device-feishu-switch">
                      <span>允许通过飞书控制此设备</span>
                      <input
                        type="checkbox"
                        checked={config.allow_control}
                        onChange={(event) =>
                          update("allow_control", event.target.checked)
                        }
                      />
                    </label>
                  </>
                )}
              </details>
              <details className="service-advanced">
                <summary>
                  应用接入信息
                  <ChevronDown size={14} />
                </summary>
                <p className="service-hint">
                  在飞书应用后台配置下方 OAuth
                  回调地址。消息默认使用长连接；会话 ID 可从飞书消息事件中获取。
                </p>
                <label className="service-field">
                  <span>OAuth 回调地址</span>
                  <input
                    aria-label="飞书回调地址"
                    readOnly
                    value={`${window.location.origin || ""}/api/v1/feishu/oauth/callback`}
                  />
                </label>
                <label className="service-field">
                  <span>消息接入方式</span>
                  <select
                    value={config.mode}
                    onChange={(event) =>
                      update("mode", event.target.value as FeishuConfig["mode"])
                    }
                  >
                    <option value="long_connection">长连接</option>
                    <option value="webhook">事件回调</option>
                  </select>
                </label>
                {config.mode === "webhook" && (
                  <>
                    <label className="service-field">
                      <span>Verification Token</span>
                      <input
                        type="password"
                        autoComplete="new-password"
                        aria-label="飞书事件校验凭证"
                        value={secrets.verification_token || ""}
                        maxLength={4096}
                        placeholder={
                          snapshot?.credentials_configured.verification_token
                            ? "留空保留已有凭证"
                            : "填写飞书事件校验凭证"
                        }
                        onChange={(event) =>
                          setSecrets((previous) => ({
                            ...previous,
                            verification_token: event.target.value,
                          }))
                        }
                      />
                    </label>
                    <p className="service-hint">
                      事件接收地址：{window.location.origin}
                      /api/v1/feishu/events。当前支持未加密事件回调。
                    </p>
                  </>
                )}
              </details>
            </fieldset>
            <footer className="service-save">
              <p role="status">
                {message ||
                  (dirty
                    ? "调整后先保存，再连接账号。"
                    : "当前设备的配置已保存。")}
              </p>
              <button
                className="button primary"
                disabled={!!busy || (mode === "live" && !snapshot)}
              >
                {busy === "save" ? (
                  <LoaderCircle size={15} className="spinning" />
                ) : (
                  <Check size={15} />
                )}
                {mode === "demo" ? "保存设备草稿" : "保存设备配置"}
              </button>
            </footer>
          </form>
        </section>
        <aside className="device-feishu-aside">
          <section className="card device-feishu-account">
            <span className="device-feishu-symbol">
              <MessageCircle size={26} strokeWidth={1.4} />
            </span>
            <h3>
              {authorized
                ? snapshot.account.display_name || "已连接飞书账号"
                : "连接你的飞书账号"}
            </h3>
            <p>
              {authorized
                ? "这个账号的授权保存在当前设备下。"
                : "保存应用配置后，为这台设备单独授权。"}
            </p>
            <button
              className="button primary"
              disabled={
                mode !== "live" || !snapshot?.config.enabled || !!busy || dirty
              }
              onClick={() => void authorize()}
            >
              <Link2 size={15} />
              {authorized ? "重新连接账号" : "连接飞书"}
            </button>
            <button
              className="device-feishu-refresh"
              disabled={mode !== "live" || !!busy}
              onClick={() => void load(dirty)}
            >
              <RefreshCw
                size={14}
                className={busy === "load" ? "spinning" : ""}
              />
              刷新状态
            </button>
            {authorized && (
              <button
                className="device-feishu-disconnect"
                disabled={!!busy}
                onClick={() => setConfirm(true)}
              >
                <Unplug size={13} />
                断开当前设备的账号
              </button>
            )}
            {snapshot?.error && (
              <p className="service-hint">{snapshot.error}</p>
            )}
          </section>
          <section className="card device-feishu-sync">
            <h3>日历同步</h3>
            <dl>
              <div>
                <dt>同步状态</dt>
                <dd>
                  {!snapshot?.config.calendar_enabled
                    ? "未开启"
                    : !authorized
                      ? "等待账号授权"
                      : snapshot?.calendar?.error
                        ? "同步失败，可重试"
                        : snapshot?.calendar?.polling
                          ? "自动同步中"
                          : "已开启"}
                </dd>
              </div>
              <div>
                <dt>最近同步</dt>
                <dd>
                  {snapshot?.calendar?.last_sync_at
                    ? formatDate(snapshot.calendar.last_sync_at)
                    : "尚未同步"}
                </dd>
              </div>
              <div>
                <dt>已缓存日程</dt>
                <dd>{snapshot?.calendar?.event_count ?? 0} 项</dd>
              </div>
            </dl>
            <button
              className="button secondary"
              disabled={
                mode !== "live" ||
                !authorized ||
                !snapshot?.config.calendar_enabled ||
                !!busy ||
                dirty
              }
              onClick={() => void sync()}
            >
              <RefreshCw
                size={14}
                className={busy === "sync" ? "spinning" : ""}
              />
              {busy === "sync" ? "正在同步" : "立即同步"}
            </button>
            <small>
              飞书日历每分钟自动刷新并下发设备；首页新建日程也会保存到当前账号的飞书日历。
            </small>
            {snapshot?.calendar?.error && (
              <p className="service-hint" role="alert">
                {snapshot.calendar.error}
              </p>
            )}
          </section>
        </aside>
      </div>
      {confirm && (
        <SnapshotDialog
          title="断开这台设备的飞书？"
          close={() => {
            if (!busy) setConfirm(false);
          }}
          closeLabel="关闭飞书断开确认"
          note="将清除当前设备保存的授权并停止日历同步，其他设备保留各自连接。"
        >
          <p className="dialog-intro">
            {device.name}之后需要重新连接飞书账号。
          </p>
          <div className="dialog-actions">
            <button
              className="button secondary"
              disabled={!!busy}
              onClick={() => setConfirm(false)}
            >
              保留连接
            </button>
            <button
              className="button primary"
              disabled={!!busy}
              onClick={() => void disconnect()}
            >
              {busy === "disconnect" ? "正在断开" : "确认断开"}
            </button>
          </div>
        </SnapshotDialog>
      )}
    </div>
  );
}
