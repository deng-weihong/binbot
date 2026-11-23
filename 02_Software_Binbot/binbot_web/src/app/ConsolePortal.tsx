import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import {
  ArrowLeft,
  ArrowUpRight,
  AudioLines,
  Bot,
  Check,
  Eye,
  EyeOff,
  LoaderCircle,
  Sparkles,
} from "lucide-react";
import {
  createDemoData,
  devices as demoDevices,
  type Device,
  type DeviceData,
} from "../shared/model/data";
import {
  AUTH_EXPIRED,
  ConsoleError,
  createConsoleTransport,
} from "../shared/api/consoleApi";
import {
  ConsoleContext,
  applyDeviceLabels,
  loadDeviceLabels,
  useConsole,
  type ConsoleMode,
} from "../shared/state/consoleContext";
import {
  consoleRoute,
  currentAddress,
  loginPath,
  navigate,
  safeReturnPath,
} from "../shared/navigation";
import { normalizeServicePreferences } from "../shared/model/serviceConfiguration";
import DeviceManager from "../features/devices/DeviceManager";
import { PortalHeader } from "../shared/ui/PortalHeader";
import DeviceIllustration from "../shared/ui/DeviceIllustration";
import ServiceSettings, { type ServiceSection } from "../features/services/ServiceSettings";

const transport = createConsoleTransport();
const previewKey = "binbot-console-preview";
const authChangeKey = "binbot-console-session-change";
function previewEnabled() {
  try {
    return sessionStorage.getItem(previewKey) === "1";
  } catch {
    return false;
  }
}
function setPreview(value: boolean) {
  try {
    if (value) sessionStorage.setItem(previewKey, "1");
    else sessionStorage.removeItem(previewKey);
  } catch {
    /* Memory session still works. */
  }
}
function broadcastLogout() {
  try {
    localStorage.setItem(authChangeKey, String(Date.now()));
  } catch {
    /* No credentials are stored. */
  }
}

export default function ConsolePortal({ children }: { children: ReactNode }) {
  const [address, setAddress] = useState(currentAddress);
  const [phase, setPhase] = useState<"checking" | "guest" | ConsoleMode>(
    "checking",
  );
  const [items, setItems] = useState<Device[]>([]);
  const [error, setError] = useState("");
  const [refreshing, setRefreshing] = useState(false);
  const requests = useRef<AbortController | null>(null);
  const generation = useRef(0);
  const route = consoleRoute(address.split(/[?#]/)[0]);

  const expire = useCallback(() => {
    const returnTo =
      consoleRoute(window.location.pathname) === "login"
        ? safeReturnPath(
            new URLSearchParams(window.location.search).get("returnTo"),
          )
        : currentAddress();
    generation.current++;
    requests.current?.abort();
    setPreview(false);
    setPhase("guest");
    setItems([]);
    setRefreshing(false);
    setError("登录已失效，请重新登录后继续。");
    navigate(loginPath(returnTo), true);
  }, []);

  useEffect(() => {
    const update = () => setAddress(currentAddress());
    window.addEventListener("popstate", update);
    window.addEventListener("hashchange", update);
    window.addEventListener(AUTH_EXPIRED, expire);
    const storage = (event: StorageEvent) => {
      if (event.key === authChangeKey && !previewEnabled()) expire();
    };
    window.addEventListener("storage", storage);
    return () => {
      window.removeEventListener("popstate", update);
      window.removeEventListener("hashchange", update);
      window.removeEventListener(AUTH_EXPIRED, expire);
      window.removeEventListener("storage", storage);
    };
  }, [expire]);

  useEffect(() => {
    if (previewEnabled()) {
      setItems(applyDeviceLabels(demoDevices, "demo"));
      setPhase("demo");
      return;
    }
    const controller = new AbortController();
    requests.current = controller;
    void transport
      .checkSession(controller.signal, false)
      .then(() => transport.devices(controller.signal, false))
      .then((result) => {
        if (controller.signal.aborted) return;
        setItems(applyDeviceLabels(result, "live"));
        setPhase("live");
      })
      .catch((reason) => {
        if (controller.signal.aborted) return;
        setPhase("guest");
        if (!(reason instanceof ConsoleError && reason.status === 401))
          setError(
            reason instanceof Error ? reason.message : "暂时无法连接后台。",
          );
      });
    return () => controller.abort();
  }, []);

  const refresh = useCallback(async () => {
    if (phase !== "live") return;
    requests.current?.abort();
    const controller = new AbortController(),
      epoch = generation.current;
    requests.current = controller;
    setRefreshing(true);
    try {
      await transport.checkSession(controller.signal);
      const result = await transport.devices(controller.signal);
      if (!controller.signal.aborted && epoch === generation.current) {
        setItems(applyDeviceLabels(result, "live"));
        setError("");
      }
    } catch (reason) {
      if (!controller.signal.aborted && epoch === generation.current)
        setError(
          reason instanceof Error ? reason.message : "设备列表读取失败。",
        );
    } finally {
      if (!controller.signal.aborted && epoch === generation.current)
        setRefreshing(false);
    }
  }, [phase]);

  useEffect(() => {
    if (phase !== "live") return;
    const refreshVisible = () => {
      if (document.visibilityState === "visible") void refresh();
    };
    const timer = window.setInterval(refreshVisible, 30000);
    window.addEventListener("focus", refreshVisible);
    return () => {
      window.clearInterval(timer);
      window.removeEventListener("focus", refreshVisible);
    };
  }, [phase, refresh]);

  useEffect(() => {
    if (phase === "checking") return;
    if (phase === "guest" && route !== "login")
      navigate(loginPath(address), true);
    if ((phase === "live" || phase === "demo") && route === "login") {
      const query = new URLSearchParams(window.location.search);
      navigate(safeReturnPath(query.get("returnTo")), true);
    }
    if (
      (phase === "live" || phase === "demo") &&
      window.location.pathname === "/"
    )
      navigate("/devices", true);
  }, [phase, route, address]);

  const logout = async () => {
    if (phase === "live") {
      try {
        await transport.logout();
      } catch (reason) {
        if (!(reason instanceof ConsoleError && reason.status === 401)) {
          setError(
            reason instanceof Error ? reason.message : "退出失败，请重试。",
          );
          throw reason;
        }
      }
    }
    generation.current++;
    requests.current?.abort();
    setPreview(false);
    if (phase === "live") broadcastLogout();
    setItems([]);
    setPhase("guest");
    setError("");
    navigate("/login", true);
  };

  const updateLabel = async (id: string, name: string, location: string) => {
    if (phase !== "live" && phase !== "demo") return;
    const labels = loadDeviceLabels(phase);
    const next = {
      name: name.trim().slice(0, 20),
      location: location.trim().slice(0, 30) || "未设置位置",
    };
    if (!next.name) throw new Error("请填写设备名称。");
    if (phase === "live") {
      const snapshot = await transport.workspace(id);
      await transport.saveWorkspace(
        id,
        snapshot.revision,
        { deviceName: next.name, location: next.location },
        crypto.randomUUID(),
      );
    } else
      try {
        localStorage.setItem(
          `binbot-device-labels-v1:${phase}`,
          JSON.stringify({ ...labels, [id]: next }),
        );
      } catch {
        throw new Error("浏览器无法保存名称与位置，请检查存储设置。");
      }
    setItems((previous) =>
      previous.map((item) => (item.id === id ? { ...item, ...next } : item)),
    );
  };

  if (phase === "checking")
    return (
      <div className="portal-loading" role="status">
        <span className="portal-loading-mark">
          <Sparkles size={24} />
        </span>
        <LoaderCircle size={20} className="spinning" />
        <p>正在打开你的桌面空间</p>
      </div>
    );
  if (phase === "guest" || route === "login")
    return (
      <LoginPage
        error={error}
        onLogin={(result) => {
          setPreview(false);
          setError("");
          setItems(applyDeviceLabels(result, "live"));
          setPhase("live");
        }}
        onPreview={() => {
          setPreview(true);
          setError("");
          setItems(applyDeviceLabels(demoDevices, "demo"));
          setPhase("demo");
          navigate("/devices", true);
        }}
      />
    );
  return (
    <ConsoleContext.Provider
      value={{
        mode: phase,
        devices: items,
        refreshing,
        error,
        refresh,
        logout,
        updateLabel,
        expire,
      }}
    >
      {route === "devices" ? (
        <DeviceManager />
      ) : route === "services" ? (
        <GlobalServices />
      ) : (
        children
      )}
    </ConsoleContext.Provider>
  );
}

export function LoginPage({
  error: initialError = "",
  onLogin,
  onPreview,
}: {
  error?: string;
  onLogin: (devices: Device[]) => void;
  onPreview: () => void;
}) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [visible, setVisible] = useState(false);
  const [remember, setRemember] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const requestRef = useRef<AbortController | null>(null);
  useEffect(() => {
    document.title = "BinBot · 登录";
    return () => requestRef.current?.abort();
  }, []);
  return (
    <main className="login-page">
      <section className="login-story">
        <a className="wordmark" href="/login">
          binbot<span>®</span>
        </a>
        <div className="login-device-scene" aria-hidden="true">
          <DeviceIllustration color="mint" />
        </div>
      </section>
      <section className="login-form-area">
        <div className="login-form-card">
          <h1>登录设备工作台</h1>
          <form
            onSubmit={async (event) => {
              event.preventDefault();
              if (busy || !username.trim() || !password) return;
              const controller = new AbortController();
              requestRef.current?.abort();
              requestRef.current = controller;
              setBusy(true);
              setError("");
              try {
                await transport.login(
                  username,
                  password,
                  remember,
                  controller.signal,
                );
                const result = await transport.devices(
                  controller.signal,
                  false,
                );
                if (!controller.signal.aborted) {
                  setPassword("");
                  onLogin(result);
                }
              } catch (reason) {
                if (!controller.signal.aborted)
                  setError(
                    reason instanceof Error
                      ? reason.message
                      : "登录失败，请重试。",
                  );
              } finally {
                if (!controller.signal.aborted) setBusy(false);
              }
            }}
          >
            <label className="portal-field">
              <span>账号</span>
              <input
                name="username"
                autoComplete="username"
                value={username}
                required
                maxLength={128}
                disabled={busy}
                placeholder="输入管理员账号"
                onChange={(event) => setUsername(event.target.value)}
              />
            </label>
            <label className="portal-field">
              <span>密码</span>
              <span className="portal-password">
                <input
                  name="password"
                  type={visible ? "text" : "password"}
                  autoComplete="current-password"
                  value={password}
                  required
                  disabled={busy}
                  placeholder="输入密码"
                  onChange={(event) => setPassword(event.target.value)}
                />
                <button
                  type="button"
                  aria-label={visible ? "隐藏密码" : "显示密码"}
                  aria-pressed={visible}
                  onClick={() => setVisible(!visible)}
                >
                  {visible ? <EyeOff size={17} /> : <Eye size={17} />}
                </button>
              </span>
            </label>
            <label className="login-remember">
              <input
                type="checkbox"
                checked={remember}
                disabled={busy}
                onChange={(event) => setRemember(event.target.checked)}
              />
              <span>记住登录状态</span>
            </label>
            {(error || initialError) && (
              <p className="portal-alert" role="alert">
                {error || initialError}
              </p>
            )}
            <button
              className="button accent login-submit"
              disabled={busy || !username.trim() || !password}
            >
              {busy ? (
                <>
                  正在登录 <LoaderCircle size={17} className="spinning" />
                </>
              ) : (
                <>
                  登录 <ArrowUpRight size={18} />
                </>
              )}
            </button>
          </form>
          <div className="login-preview">
            <button className="button secondary" type="button" disabled={busy} onClick={onPreview}>
              进入界面预览 <ArrowUpRight size={14} />
            </button>
            <small>使用示例设备，不连接或控制真实设备。</small>
          </div>
        </div>
      </section>
    </main>
  );
}

export function GlobalServices() {
  const [section, setSection] = useState<ServiceSection>(
    () =>
      (["llm", "voice"].includes(window.location.hash.slice(1))
        ? window.location.hash.slice(1)
        : "llm") as ServiceSection,
  );
  const [message, setMessage] = useState("");
  const { mode } = useConsole();
  const storageKey = `binbot-public-services-v1:${mode}`;
  const [data, setData] = useState<DeviceData>(() => {
    const initial = createDemoData({
      id: "public-services",
      name: "公共服务",
      location: "",
      online: false,
      color: "lilac",
    });
    try {
      initial.services = normalizeServicePreferences(
        JSON.parse(localStorage.getItem(storageKey) || "null"),
      );
    } catch {
      /* Use public defaults. */
    }
    return initial;
  });
  useEffect(() => {
    document.title = "BinBot · 公共服务设置";
  }, []);
  useEffect(() => {
    try {
      localStorage.setItem(
        storageKey,
        JSON.stringify({ providers: data.services.providers }),
      );
    } catch {
      /* A draft may remain in memory. */
    }
  }, [data.services, storageKey]);
  useEffect(() => {
    const change = () => {
      const value = window.location.hash.slice(1);
      if (["llm", "voice"].includes(value)) setSection(value as ServiceSection);
    };
    window.addEventListener("popstate", change);
    window.addEventListener("hashchange", change);
    return () => {
      window.removeEventListener("popstate", change);
      window.removeEventListener("hashchange", change);
    };
  }, []);
  useEffect(() => {
    if (!message) return;
    const timer = window.setTimeout(() => setMessage(""), 4000);
    return () => window.clearTimeout(timer);
  }, [message]);
  return (
    <div className="portal-page">
      <PortalHeader label="公共服务" />
      <main className="portal-main services-main">
        <a
          className="portal-back"
          href="/devices"
          onClick={(event) => {
            event.preventDefault();
            navigate("/devices");
          }}
        >
          <ArrowLeft size={16} /> 全部设备
        </a>
        <div className="portal-heading">
          <div>
            <h1>公共服务设置</h1>
            <p>
              大模型与语音服务由所有设备共用。飞书在各设备的设置中单独连接。
            </p>
          </div>
        </div>
        <nav className="section-nav" aria-label="公共服务分类">
          {(
            [
              { id: "llm", label: "大模型", icon: Bot },
              { id: "voice", label: "语音", icon: AudioLines },
            ] as const
          ).map(({ id, label, icon: Icon }) => (
            <a
              key={id}
              href={`/services#${id}`}
              aria-current={section === id ? "page" : undefined}
              className={section === id ? "selected" : ""}
              onClick={(event) => {
                event.preventDefault();
                setSection(id);
                navigate(`/services#${id}`, true);
              }}
            >
              <Icon size={16} />
              {label}
            </a>
          ))}
        </nav>
        {window.location.hash === "#feishu" && (
          <p className="portal-alert" role="status">
            飞书连接已移到每台设备的设置中。
            <a
              href="/devices"
              onClick={(event) => {
                event.preventDefault();
                navigate("/devices");
              }}
            >
              选择设备
            </a>
          </p>
        )}
        <ServiceSettings
          section={section}
          data={data}
          setData={setData}
          notify={setMessage}
        />
      </main>
      {message && (
        <div className="toast" role="status">
          <Check size={16} />
          {message}
        </div>
      )}
    </div>
  );
}
