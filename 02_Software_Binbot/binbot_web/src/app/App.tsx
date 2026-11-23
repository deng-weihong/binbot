import {
  WorkspaceContext,
  useDeviceWorkspace,
  useWorkspace,
} from "../shared/state/workspaceContext";
import { record } from "../shared/model/workspaceModel";
import {
  useEffect,
  useRef,
  useState,
  type ComponentType,
  type Dispatch,
  type MouseEvent as ReactMouseEvent,
  type ReactNode,
  type SetStateAction,
} from "react";
import {
  ArrowRight,
  ArrowUpRight,
  ArrowLeft,
  BookOpen,
  CalendarDays,
  Check,
  ChevronDown,
  Clock3,
  Headphones,
  House,
  Hand,
  LayoutGrid,
  LoaderCircle,
  LogOut,
  MessageCircle,
  Monitor,
  Pause,
  Play,
  Puzzle,
  RotateCcw,
  Settings2,
  ShieldCheck,
  SlidersHorizontal,
  Smile,
  Sparkles,
  Sun,
  Timer,
  WifiOff,
  X,
  Zap,
} from "lucide-react";
import {
  createDemoData,
  type Device,
  type DeviceData,
  type Page,
} from "../shared/model/data";
import ControlPage from "../features/control/ControlPage";
import AssistantPage from "../features/assistant/AssistantPage";
import HomeCameraCard from "../features/home/HomeCameraCard";
import HomeAgendaCard from "../features/home/HomeAgendaCard";
import HomeQuickControls from "../features/home/HomeQuickControls";
import HomeTodoList from "../features/home/HomeTodoList";
import GesturesPage from "../features/gestures/GesturesPage";
import { ModulesPage } from "../features/modules/ModulesPage";
import { VocabularyPage } from "../features/vocabulary/VocabularyPage";
import { WatchFacePage } from "../features/watch-faces/WatchFacePage";
import ConsolePortal from "./ConsolePortal";
import { useConsole } from "../shared/state/consoleContext";
import { devicePath, navigate } from "../shared/navigation";
import DeviceMaintenance from "../features/devices/DeviceMaintenance";
import DeviceFeishuSettings from "../features/devices/DeviceFeishuSettings";

type Icon = ComponentType<{
  size?: number;
  strokeWidth?: number;
  className?: string;
}>;

type Update = Dispatch<SetStateAction<DeviceData>>;
const navigation: { page: Page; label: string; icon: Icon }[] = [
  { page: "today", label: "今天", icon: House },
  { page: "assistant", label: "助手", icon: Sparkles },
  { page: "vocabulary", label: "单词学习", icon: BookOpen },
  { page: "control", label: "设备控制", icon: SlidersHorizontal },
  { page: "modules", label: "组件模块", icon: Puzzle },
  { page: "settings", label: "设置", icon: Settings2 },
];
const deviceNavigation: { page: Page; label: string; icon: Icon }[] = [
  { page: "control", label: "基础控制", icon: SlidersHorizontal },
  { page: "watchface", label: "表盘设置", icon: Clock3 },
  { page: "gestures", label: "手势设置", icon: Hand },
];
const pageTitles: Record<Page, string> = {
  today: "今天",
  assistant: "助手",
  control: "设备控制",
  vocabulary: "单词学习",
  watchface: "表盘设置",
  gestures: "手势设置",
  modules: "组件模块",
  settings: "设备设置",
};

function readRoute() {
  const parts = window.location.pathname.split("/").filter(Boolean);
  let id = parts[1];
  try {
    id = decodeURIComponent(id || "");
  } catch {
    /* An invalid identifier opens the missing-device view. */
  }
  return {
    deviceId: parts[0] === "devices" ? id : "",
    page: (["camera", "noise", "scenes", "activity"].includes(parts[2])
      ? "today"
      : parts[2] || "today") as Page,
  };
}
export default function App() {
  return (
    <ConsolePortal>
      <DeviceWorkspaceApp />
    </ConsolePortal>
  );
}

export function DeviceWorkspaceApp() {
  const { devices } = useConsole();
  const [route, setRoute] = useState(readRoute);
  useEffect(() => {
    const update = () => setRoute(readRoute());
    window.addEventListener("popstate", update);
    const retiredPage = window.location.pathname.match(
      /^\/devices\/([^/]+)\/(camera|noise|scenes|activity)\/?$/,
    );
    if (retiredPage && devices.some((item) => item.id === retiredPage[1])) {
      const anchor =
        retiredPage[2] === "camera"
          ? "#camera-preview"
          : retiredPage[2] === "noise"
            ? "#white-noise"
            : "";
      navigate(`/devices/${retiredPage[1]}/today${anchor}`, true);
    }
    if (
      route.page === "settings" &&
      ["llm", "voice"].includes(window.location.hash?.slice(1))
    )
      navigate(`/services${window.location.hash}`, true);
    return () => window.removeEventListener("popstate", update);
  }, []);
  useEffect(() => {
    if (route.page !== "assistant" || !["#voice", "#web"].includes(window.location.hash))
      return;
    const { pathname, search } = window.location;
    navigate(pathname + search, true);
  }, [route.page, route.deviceId]);
  const device = devices.find((item) => item.id === route.deviceId);
  if (!device || !Object.hasOwn(pageTitles, route.page))
    return (
      <div className="not-found">
        <div className="brand-symbol">
          <Smile />
        </div>
        <h1>这个空间还没准备好</h1>
        <p>请从已有的设备页面开始。</p>
        <button className="button primary" onClick={() => navigate("/devices")}>
          返回全部设备 <ArrowRight size={16} />
        </button>
      </div>
    );
  return <Workspace key={device.id} device={device} page={route.page} />;
}

function Workspace({ device, page }: { device: Device; page: Page }) {
  const { devices, mode, logout, error: directoryError } = useConsole();

  const [loggingOut, setLoggingOut] = useState(false);
  const [switcher, setSwitcher] = useState(false);
  const [modal, setModal] = useState<"help" | null>(null);
  const [toast, setToast] = useState("");
  const {
    data,
    setData,
    notify,
    value: workspace,
  } = useDeviceWorkspace(device, mode, setToast);
  const switcherRef = useRef<HTMLDivElement>(null);
  const inControlSection = deviceNavigation.some((item) => item.page === page);
  const currentNavigationPage = inControlSection ? "control" : page;

  const openPage = (path: string) => {
    setSwitcher(false);
    setModal(null);
    navigate(path);
    if (window.scrollY) window.scrollTo(0, 0);
  };
  const go = (target: Page) => openPage(devicePath(device.id, target));
  const followLink = (event: ReactMouseEvent<HTMLAnchorElement>) => {
    if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey)
      return;
    event.preventDefault();
    openPage(event.currentTarget.pathname);
  };
  useEffect(() => {
    if (!toast) return;
    const timeout = window.setTimeout(() => setToast(""), 4200);
    return () => window.clearTimeout(timeout);
  }, [toast]);
  useEffect(() => {
    const outside = (e: MouseEvent) => {
      if (!switcherRef.current?.contains(e.target as Node)) setSwitcher(false);
    };
    const escape = (e: KeyboardEvent) => {
      if (e.key === "Escape") setSwitcher(false);
    };
    document.addEventListener("mousedown", outside);
    document.addEventListener("keydown", escape);
    return () => {
      document.removeEventListener("mousedown", outside);
      document.removeEventListener("keydown", escape);
    };
  }, []);
  useEffect(() => {
    document.title = `BinBot · ${data.deviceName} · ${pageTitles[page]}`;
  }, [data.deviceName, page]);

  return (
    <WorkspaceContext.Provider value={workspace}>
      <div className="app-shell">
        <nav className="rail" aria-label="工作台导航">
          <a
            className="brand-symbol"
            href={devicePath(device.id)}
            onClick={followLink}
            aria-label="BinBot 首页"
          >
            <svg viewBox="0 0 30 30" fill="none">
              <path
                d="M5 20V10a5 5 0 0 1 5-5h10a5 5 0 0 1 5 5v10a5 5 0 0 1-5 5H10a5 5 0 0 1-5-5Z"
                stroke="currentColor"
                strokeWidth="2"
              />
              <path
                d="M11 12v4m8-4v4m-7 4h6"
                stroke="currentColor"
                strokeWidth="2"
                strokeLinecap="round"
              />
              <path
                d="M15 1v4"
                stroke="currentColor"
                strokeWidth="2"
                strokeLinecap="round"
              />
            </svg>
          </a>
          <div className="rail-links">
            {navigation.map(({ page: target, label, icon: NavIcon }) => (
              <a
                key={target}
                href={devicePath(device.id, target)}
                onClick={followLink}
                className={
                  target === currentNavigationPage
                    ? "rail-link active"
                    : "rail-link"
                }
                aria-current={
                  target === currentNavigationPage ? "page" : undefined
                }
                aria-label={label}
              >
                <NavIcon size={20} strokeWidth={1.7} />
                <span className="rail-label">{label}</span>
              </a>
            ))}
          </div>
          <div className="rail-bottom">
            <button
              type="button"
              className="rail-link"
              aria-label="使用帮助"
              onClick={() => setModal("help")}
            >
              <Headphones size={18} strokeWidth={1.7} />
              <span className="rail-label">帮助</span>
            </button>
            <a
              className="rail-link rail-directory"
              href="/devices"
              onClick={followLink}
            >
              <LayoutGrid size={19} strokeWidth={1.8} />
              <span className="rail-label">全部设备</span>
            </a>
          </div>
        </nav>

        <div
          className={`workspace${page === "today" || page === "modules" ? " compact-workspace" : ""}`}
        >
          <header className="topbar">
            <div className="topbar-left">
              <a
                className="wordmark"
                href={devicePath(device.id)}
                onClick={followLink}
              >
                binbot<span>®</span>
              </a>
              <span className="topbar-divider" />
              <a
                className="workspace-all-devices"
                href="/devices"
                aria-label="全部设备"
                onClick={followLink}
              >
                <ArrowLeft size={16} />
              </a>
              <div className="device-switcher" ref={switcherRef}>
                <button
                  className={`device-switch-button ${switcher ? "open" : ""}`}
                  onClick={() => setSwitcher(!switcher)}
                  aria-expanded={switcher}
                  aria-haspopup="menu"
                >
                  <span className={`tiny-device ${device.color}`}>
                    <Smile size={16} />
                  </span>
                  <span>
                    {data.deviceName}
                    <small>{device.location}</small>
                  </span>
                  <ChevronDown size={14} />
                </button>
                {switcher && (
                  <div className="device-menu" role="menu">
                    <div className="menu-caption">
                      选择你的桌面伙伴{" "}
                      <span>{mode === "demo" ? "示例设备" : "我的设备"}</span>
                    </div>
                    {devices.map((item) => (
                      <button
                        key={item.id}
                        role="menuitem"
                        onClick={() => {
                          setSwitcher(false);
                          if (item.id !== device.id)
                            navigate(devicePath(item.id, page));
                        }}
                      >
                        <span className={`tiny-device ${item.color}`}>
                          <Smile size={18} />
                        </span>
                        <span className="device-menu-name">
                          {item.id === device.id ? data.deviceName : item.name}
                          <small>
                            {item.location} · {item.online ? "在线" : "离线"}
                          </small>
                        </span>
                        {item.id === device.id && <Check size={16} />}
                      </button>
                    ))}
                    <p>每个空间只属于一台设备。</p>
                  </div>
                )}
              </div>
            </div>
            <div className="topbar-right">
              <button className="demo-badge" onClick={() => setModal("help")}>
                <span />
                {mode === "demo" ? "界面预览" : "设备工作台"}
              </button>
              <button
                className="icon-button workspace-logout"
                aria-label={mode === "demo" ? "退出预览" : "退出登录"}
                disabled={loggingOut}
                onClick={async () => {
                  setLoggingOut(true);
                  try {
                    await logout();
                  } catch (reason) {
                    setToast(
                      reason instanceof Error
                        ? reason.message
                        : "退出失败，请重试。",
                    );
                  } finally {
                    setLoggingOut(false);
                  }
                }}
              >
                {loggingOut ? (
                  <LoaderCircle size={17} className="spinning" />
                ) : (
                  <LogOut size={17} />
                )}
              </button>
              <button
                className="avatar"
                aria-label="个人偏好"
                onClick={() => go("settings")}
              >
                D<span />
              </button>
            </div>
          </header>

          <main id="main-content">
            <div className="page-heading">
              <h1>{pageTitles[page]}</h1>
              {page === "today" && (
                <div className="page-actions">
                  <span className="date-chip">
                    <CalendarDays size={16} />
                    {mode === "demo"
                      ? "11 月 16 日 星期一"
                      : new Intl.DateTimeFormat("zh-CN", {
                          timeZone: String(
                            workspace.snapshot?.data.timezone ||
                              "Asia/Shanghai",
                          ),
                          month: "long",
                          day: "numeric",
                          weekday: "long",
                        }).format(new Date())}
                  </span>
                </div>
              )}
            </div>
            {inControlSection && (
              <nav className="section-nav" aria-label="设备设置分类">
                {deviceNavigation.map(
                  ({ page: target, label, icon: TabIcon }) => (
                    <a
                      href={devicePath(device.id, target)}
                      key={target}
                      className={page === target ? "selected" : ""}
                      aria-current={page === target ? "page" : undefined}
                      onClick={followLink}
                    >
                      <TabIcon size={16} strokeWidth={1.5} />
                      {label}
                    </a>
                  ),
                )}
              </nav>
            )}
            {!device.online && (
              <div className="offline-banner">
                <WifiOff size={18} />
                <div>
                  <strong>{data.deviceName}暂时离线</strong>
                  <span>
                    仍可查看记录和调整偏好，实时控制将在连接恢复后可用。
                  </span>
                </div>
              </div>
            )}
            {directoryError && (
              <p className="portal-alert workspace-session-error" role="alert">
                {directoryError}
              </p>
            )}
            {workspace.error && (
              <p className="portal-alert" role="alert">
                {workspace.error}
              </p>
            )}
            {workspace.pending && (
              <p className="feature-note" role="status">
                正在同步设置…
              </p>
            )}
            {workspace.loading && <p role="status">正在读取设备工作台…</p>}
            {!workspace.loading && (mode === "demo" || workspace.snapshot) && (
              <>
                {page === "today" && (
                  <Today
                    device={device}
                    data={data}
                    setData={setData}
                    go={go}
                    notify={notify}
                  />
                )}
                {page === "assistant" && (
                  <AssistantPage
                    key={`${device.id}-assistant`}
                    device={device}
                    data={data}
                  />
                )}
                {page === "control" && (
                  <ControlPage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
                {page === "vocabulary" && (
                  <VocabularyPage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
                {page === "watchface" && (
                  <WatchFacePage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
                {page === "gestures" && (
                  <GesturesPage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
                {page === "modules" && (
                  <ModulesPage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
                {page === "settings" && (
                  <SettingsPage
                    device={device}
                    data={data}
                    setData={setData}
                    notify={notify}
                  />
                )}
              </>
            )}
          </main>
          <footer className="page-footer">
            <span>
              <span
                className={`status-dot ${device.online ? "" : "offline"}`}
              />
              {data.deviceName}的专属空间
            </span>
            <span>
              {mode === "demo"
                ? "界面预览 · 演示数据"
                : "设备工作台 · 设置同步至服务器"}
            </span>
          </footer>
        </div>

        {toast && (
          <div className="toast" role="status">
            <span>
              <Check size={16} />
            </span>
            {toast}
            <button aria-label="关闭提示" onClick={() => setToast("")}>
              <X size={15} />
            </button>
          </div>
        )}
        {modal === "help" && (
          <Modal title="欢迎来到新的 BinBot" close={() => setModal(null)}>
            <div className="help-illustration">
              <Smile size={52} strokeWidth={1.2} />
              <Sparkles size={24} />
            </div>
            <p className="dialog-intro">
              这是一个可以点击体验的 UI
              原型。日程、待办、单词本和设备偏好会按设备保存在当前浏览器，白噪音可以直接在首页试听。
            </p>
            <div className="help-list">
              <div>
                <Monitor size={19} />
                <span>
                  <strong>一个页面，一位伙伴</strong>
                  <small>顶部可以切换设备，页面始终属于当前设备。</small>
                </span>
              </div>
              <div>
                <Zap size={19} />
                <span>
                  <strong>试着调整桌面伙伴</strong>
                  <small>
                    在首页使用快捷入口，或到设备控制和组件模块中调整设置。
                  </small>
                </span>
              </div>
              <div>
                <ShieldCheck size={19} />
                <span>
                  <strong>{mode === "demo" ? "界面预览" : "设备工作台"}</strong>
                  <small>
                    {mode === "demo"
                      ? "预览各页面，了解设备的主要功能。"
                      : "设置与当前设备同步，摄像头、AI 对话和学习记录按实际连接显示。"}
                  </small>
                </span>
              </div>
            </div>
            <button
              className="button primary full-width"
              onClick={() => setModal(null)}
            >
              开始探索 <ArrowRight size={16} />
            </button>
          </Modal>
        )}
      </div>
    </WorkspaceContext.Provider>
  );
}

type CommonProps = { device: Device; data: DeviceData; setData: Update };

function Today({
  device,
  data,
  setData,
  go,
  notify,
}: CommonProps & {
  go: (page: Page) => void;
  notify: (message: string) => void;
}) {
  return (
    <div className="today-layout home-layout">
      <section className={`device-card ${device.color}`} aria-label="当前设备">
        <div className="device-card-top">
          <div className="device-card-name">
            <h2 title={data.deviceName}>{data.deviceName}</h2>
            <p title={device.location}>
              BinBot One <span>·</span> {device.location}
            </p>
          </div>
          <span
            className={`connection-pill ${device.online ? "" : "is-offline"}`}
          >
            <span className="status-dot" />
            {device.online ? "在线" : "离线"}
          </span>
        </div>
        <div className="device-stage">
          <img
            className="device-stage-illustration"
            src="/device-illustration.svg"
            alt="BinBot 设备示意图"
            width={420}
            height={300}
          />
        </div>
        <div className="device-card-bottom">
          <HomeQuickControls
            device={device}
            data={data}
            setData={setData}
            notify={notify}
          />
          <button className="device-control-link" onClick={() => go("control")}>
            打开设备控制{" "}
            <span>
              <ArrowUpRight size={18} />
            </span>
          </button>
        </div>
      </section>

      <HomeAgendaCard data={data} setData={setData} notify={notify} />

      <HomeTodoList data={data} setData={setData} notify={notify} />

      <HomeCameraCard device={device} name={data.deviceName} notify={notify} />

      <FocusCard
        key={device.id}
        online={device.online}
        onComplete={() => notify("这段专注完成了，休息一下吧。")}
      />
    </div>
  );
}

function FocusCard({
  online,
  onComplete,
}: {
  online: boolean;
  onComplete: () => void;
}) {
  const workspace = useWorkspace();
  const focus = record(workspace.snapshot?.reported.focus);
  const [localRemaining, setRemaining] = useState(25 * 60);
  const [localRunning, setRunning] = useState(false);
  const remaining = workspace.live
    ? Number(focus.secondsLeft ?? 1500)
    : localRemaining;
  const running = workspace.live ? focus.running === true : localRunning;
  const duration = workspace.live
    ? Number(focus.mode === "break" ? 300 : focus.focusSeconds || 1500)
    : 1500;
  const completeRef = useRef(onComplete);
  completeRef.current = onComplete;
  useEffect(() => {
    if (workspace.live || !running) return;
    const deadline = Date.now() + remaining * 1000;
    const interval = window.setInterval(() => {
      const next = Math.max(0, Math.ceil((deadline - Date.now()) / 1000));
      setRemaining(next);
      if (next === 0) {
        setRunning(false);
        completeRef.current();
      }
    }, 250);
    return () => window.clearInterval(interval);
    // Capture remaining only when the user starts or resumes the timer.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [running]);
  return (
    <section className="preview-panel focus-card" aria-labelledby="focus-title">
      <header className="focus-top">
        <h2 id="focus-title">专注计时</h2>
        <Timer size={19} strokeWidth={1.5} aria-hidden="true" />
      </header>
      <div className="focus-dial">
        <svg viewBox="0 0 176 176" aria-hidden="true">
          <circle className="dial-track" cx="88" cy="88" r="76" />
          <circle
            className="dial-progress"
            cx="88"
            cy="88"
            r="76"
            strokeDasharray={`${(remaining / duration) * 477.5} 477.5`}
          />
        </svg>
        <div>
          <strong>
            {String(Math.floor(remaining / 60)).padStart(2, "0")}
            <span>:</span>
            {String(remaining % 60).padStart(2, "0")}
          </strong>
          {remaining === 0 && <small>本轮完成</small>}
        </div>
      </div>
      <div className="focus-actions">
        <button
          className="button accent"
          disabled={
            !online ||
            workspace.pending ||
            (workspace.live && !workspace.snapshot?.control.ready)
          }
          onClick={() => {
            if (workspace.live) {
              void workspace.action("focus.set", {
                action: running ? "pause" : "start",
              });
              return;
            }
            if (remaining === 0) setRemaining(1500);
            setRunning(!running);
          }}
        >
          {running ? (
            <Pause size={14} />
          ) : (
            <Play size={14} fill="currentColor" />
          )}
          {running
            ? "暂停一下"
            : remaining < duration && remaining > 0
              ? "继续专注"
              : "开始专注"}
        </button>
        {remaining < duration && (
          <button
            className="icon-button focus-reset"
            aria-label="重置专注计时"
            disabled={workspace.pending || !online}
            onClick={() => {
              if (workspace.live) {
                void workspace.action("focus.set", { action: "reset" });
                return;
              }
              setRunning(false);
              setRemaining(1500);
            }}
          >
            <RotateCcw size={15} />
          </button>
        )}
      </div>
    </section>
  );
}

function SettingsPage({
  device,
  data,
  setData,
  notify,
}: CommonProps & { notify: (message: string) => void }) {
  const sections = [
    { id: "general", label: "设备偏好", icon: Settings2 },
    { id: "information", label: "信息与维护", icon: ShieldCheck },
    { id: "feishu", label: "飞书连接", icon: MessageCircle },
  ] as const;
  const [section, setSection] = useState<"general" | "information" | "feishu">(
    () => {
      const initial = window.location.hash?.slice(1);
      return sections.find((item) => item.id === initial)?.id || "general";
    },
  );
  useEffect(() => {
    const update = () =>
      setSection(
        window.location.hash === "#feishu"
          ? "feishu"
          : window.location.hash === "#information"
            ? "information"
            : "general",
      );
    window.addEventListener("popstate", update);
    window.addEventListener("hashchange", update);
    return () => {
      window.removeEventListener("popstate", update);
      window.removeEventListener("hashchange", update);
    };
  }, []);
  return (
    <div className="settings-page">
      <nav className="section-nav" aria-label="设置分类">
        {sections.map(({ id, label, icon: SectionIcon }) => (
          <a
            key={id}
            href={`/devices/${device.id}/settings#${id}`}
            className={section === id ? "selected" : ""}
            aria-current={section === id ? "page" : undefined}
            onClick={(event) => {
              if (
                event.metaKey ||
                event.ctrlKey ||
                event.shiftKey ||
                event.altKey
              )
                return;
              event.preventDefault();
              setSection(id);
              navigate(`/devices/${device.id}/settings#${id}`, true);
            }}
          >
            <SectionIcon size={16} strokeWidth={1.5} /> {label}
          </a>
        ))}
      </nav>
      {section === "general" ? (
        <GeneralSettingsPage
          device={device}
          data={data}
          setData={setData}
          notify={notify}
        />
      ) : section === "feishu" ? (
        <DeviceFeishuSettings key={device.id} device={device} notify={notify} />
      ) : (
        <DeviceMaintenance
          device={device}
          name={data.deviceName}
          notify={notify}
        />
      )}
      <a
        className="device-public-services"
        href="/services"
        onClick={(event) => {
          event.preventDefault();
          navigate("/services");
        }}
      >
        <Settings2 size={15} />
        <span>大模型与语音在公共服务设置中管理。</span>
        <ArrowUpRight size={15} />
      </a>
    </div>
  );
}

function GeneralSettingsPage({
  device,
  data,
  setData,
  notify,
}: CommonProps & { notify: (message: string) => void }) {
  const { updateLabel, mode } = useConsole();
  const [name, setName] = useState(data.deviceName);
  const [assistantDraft, setAssistantDraft] = useState(data.assistant);
  const [reset, setReset] = useState(false);
  return (
    <div className="settings-layout">
      <section className="card settings-card">
        <div className="card-heading">
          <div>
            <h2>设备信息</h2>
          </div>
          <span className={`round-icon ${device.color}`}>
            <Smile size={24} />
          </span>
        </div>
        <form
          onSubmit={async (e) => {
            e.preventDefault();
            try {
              await updateLabel(device.id, name, device.location);
              if (mode === "demo")
                setData((previous) => ({
                  ...previous,
                  deviceName: name.trim(),
                }));
              notify("设备名称已保存。");
            } catch (reason) {
              notify(reason instanceof Error ? reason.message : "保存失败。");
            }
          }}
        >
          <label className="field-label">
            设备名称
            <input
              value={name}
              maxLength={20}
              required
              onChange={(e) => setName(e.target.value)}
            />
          </label>
          <div className="device-info-line">
            <span>设备标识</span>
            <strong>{device.id}</strong>
          </div>
          <div className="device-info-line">
            <span>位置</span>
            <strong>{device.location}</strong>
          </div>
          <div className="device-info-line">
            <span>设备型号</span>
            <strong>BinBot One</strong>
          </div>
          <div className="form-footer">
            <span>名称与当前设备绑定</span>
            <button className="button primary" disabled={!name.trim()}>
              保存名称 <Check size={15} />
            </button>
          </div>
        </form>
      </section>
      <section className="card settings-card">
        <div className="card-heading">
          <div>
            <h2>日常偏好</h2>
          </div>
          <Settings2 size={21} strokeWidth={1.5} />
        </div>
        <div className="preference-row">
          <span>
            <strong>设备提醒</strong>
            <small>日程提醒与勿扰模式，保存后应用到设备。</small>
          </span>
          <Toggle
            checked={data.notifications}
            label="设备提醒"
            onChange={() => {
              setData((previous) => ({
                ...previous,
                notifications: !previous.notifications,
              }));
              notify("偏好已更新");
            }}
          />
        </div>
        <div className="preference-row">
          <label htmlFor="device-timezone">设备时区</label>
          <select
            id="device-timezone"
            value={data.timezone || "Asia/Shanghai"}
            onChange={(event) =>
              setData((previous) => ({
                ...previous,
                timezone: event.target.value,
              }))
            }
          >
            {[
              ...new Set([
                "Asia/Shanghai",
                "Asia/Hong_Kong",
                "Asia/Tokyo",
                "America/Denver",
                "America/Los_Angeles",
                "Europe/London",
                "UTC",
                data.timezone || "Asia/Shanghai",
              ]),
            ].map((zone) => (
              <option key={zone} value={zone}>
                {zone}
              </option>
            ))}
          </select>
        </div>
        <div className="preference-row">
          <span>
            <strong>外观</strong>
            <small>柔和的浅色，留一点呼吸的空间。</small>
          </span>
          <span className="quiet-chip">
            <Sun size={13} />
            浅色
          </span>
        </div>
      </section>
      <section className="card settings-card">
        <div className="card-heading">
          <div>
            <h2>助手偏好</h2>
          </div>
          <Sparkles size={21} />
        </div>
        <form
          className="form-stack"
          onSubmit={(event) => {
            event.preventDefault();
            setData((previous) => ({ ...previous, assistant: assistantDraft }));
            notify("助手偏好已保存");
          }}
        >
          <label className="field-label">
            助手称呼
            <input
              required
              maxLength={24}
              value={assistantDraft.name}
              onChange={(event) =>
                setAssistantDraft((previous) => ({
                  ...previous,
                  name: event.target.value,
                }))
              }
            />
          </label>
          <label className="field-label">
            回复风格
            <input
              maxLength={100}
              value={assistantDraft.style}
              onChange={(event) =>
                setAssistantDraft((previous) => ({
                  ...previous,
                  style: event.target.value,
                }))
              }
              placeholder="比如，简洁、轻松一点"
            />
          </label>
          <label className="field-label">
            想对助手说的话
            <textarea
              rows={3}
              maxLength={2000}
              value={assistantDraft.instruction}
              onChange={(event) =>
                setAssistantDraft((previous) => ({
                  ...previous,
                  instruction: event.target.value,
                }))
              }
              placeholder="比如，先给结论，再解释原因。"
            />
          </label>
          <label className="field-label">
            让助手记住的偏好
            <textarea
              rows={3}
              value={assistantDraft.memories.join("\n")}
              onChange={(event) =>
                setAssistantDraft((previous) => ({
                  ...previous,
                  memories: event.target.value.split("\n").slice(0, 50),
                }))
              }
              placeholder="每行一条，比如：专注时喜欢安静。"
            />
          </label>
          <div className="form-footer">
            <span>在这台设备的 AI 对话中生效</span>
            <button className="button primary" type="submit">
              保存偏好 <Check size={15} />
            </button>
          </div>
        </form>
      </section>
      {mode === "demo" && (
        <section className="card reset-card">
          <div>
            <h3>重新开始这段演示</h3>
            <p>恢复这台设备的初始演示数据，已应用的公共服务配置保留。</p>
          </div>
          <button className="button secondary" onClick={() => setReset(true)}>
            <RotateCcw size={15} />
            重置当前设备
          </button>
        </section>
      )}
      {reset && (
        <Modal title="恢复最初的样子？" close={() => setReset(false)}>
          <p className="dialog-intro">
            {data.deviceName}
            的日程、待办、单词本、网页复习进度和设备偏好会恢复为初始演示数据，公共服务设置保留。
          </p>
          <div className="dialog-actions">
            <button
              className="button secondary"
              onClick={() => setReset(false)}
            >
              保留现在
            </button>
            <button
              className="button primary"
              onClick={() => {
                setData(createDemoData(device));
                setName(device.name);
                setReset(false);
                notify("这台设备的演示数据已重置");
              }}
            >
              确认重置 <RotateCcw size={15} />
            </button>
          </div>
        </Modal>
      )}
    </div>
  );
}

function Modal({
  title,
  close,
  children,
}: {
  title: string;
  close: () => void;
  children: ReactNode;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const closeRef = useRef(close);
  closeRef.current = close;
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null;
    const overflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    const focusable = () =>
      Array.from(
        ref.current?.querySelectorAll<HTMLElement>(
          'button:not(:disabled), input:not(:disabled), textarea:not(:disabled), select:not(:disabled), [tabindex="0"]',
        ) || [],
      );
    if (!ref.current?.contains(document.activeElement))
      (
        ref.current?.querySelector<HTMLElement>("[autofocus]") || focusable()[0]
      )?.focus();
    const keydown = (e: KeyboardEvent) => {
      if (e.key === "Escape") closeRef.current();
      if (e.key === "Tab") {
        const nodes = focusable();
        const first = nodes[0];
        const last = nodes[nodes.length - 1];
        if (e.shiftKey && document.activeElement === first) {
          e.preventDefault();
          last?.focus();
        } else if (!e.shiftKey && document.activeElement === last) {
          e.preventDefault();
          first?.focus();
        }
      }
    };
    document.addEventListener("keydown", keydown);
    return () => {
      document.body.style.overflow = overflow;
      document.removeEventListener("keydown", keydown);
      previous?.focus();
    };
  }, []);
  return (
    <div
      className="modal-backdrop"
      onMouseDown={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      <div
        className="modal"
        role="dialog"
        aria-modal="true"
        aria-labelledby="dialog-title"
        ref={ref}
      >
        <div className="modal-heading">
          <div>
            <h2 id="dialog-title">{title}</h2>
          </div>
          <button
            className="icon-button subtle"
            aria-label="关闭弹窗"
            onClick={close}
          >
            <X size={19} />
          </button>
        </div>
        {children}
      </div>
    </div>
  );
}
function Toggle({
  checked,
  onChange,
  label,
  disabled,
}: {
  checked: boolean;
  onChange: () => void;
  label: string;
  disabled?: boolean;
}) {
  return (
    <button
      type="button"
      className={`toggle ${checked ? "checked" : ""}`}
      role="switch"
      aria-checked={checked}
      aria-label={label}
      disabled={disabled}
      onClick={onChange}
    >
      <span>{checked && <Check size={10} />}</span>
    </button>
  );
}
