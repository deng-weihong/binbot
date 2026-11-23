import {
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type ReactNode,
  type SetStateAction,
} from "react";
import {
  AudioLines,
  Bot,
  Check,
  ChevronDown,
  Eye,
  EyeOff,
  LoaderCircle,
  LogIn,
  RefreshCw,
} from "lucide-react";
import type { DeviceData } from "../../shared/model/data";
import { useConsole } from "../../shared/state/consoleContext";
import {
  createServiceTransport,
  mergeProviderSection,
  ServiceError,
  validateProviderValues,
  type ProviderConfiguration,
  type ProviderSecrets,
  type ProviderValues,
  type SecretName,
} from "../../shared/model/serviceConfiguration";

export type ServiceSection = "llm" | "voice";
const copy = {
  llm: {
    title: "大模型配置",
    description: "设置服务地址、模型和 API 密钥。",
    icon: Bot,
  },
  voice: {
    title: "语音服务配置",
    description: "设置语音识别和语音合成。",
    icon: AudioLines,
  },
};

function Field({
  label,
  value,
  onChange,
  placeholder,
  type = "text",
  required = false,
  hint,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  placeholder?: string;
  type?: string;
  required?: boolean;
  hint?: string;
}) {
  return (
    <label className="service-field">
      <span>{label}</span>
      <input
        type={type}
        value={value}
        required={required}
        maxLength={2048}
        placeholder={placeholder}
        onChange={(event) => onChange(event.target.value)}
      />
      {hint && <small>{hint}</small>}
    </label>
  );
}

function Choice({
  label,
  value,
  onChange,
  children,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  children: ReactNode;
}) {
  return (
    <label className="service-field">
      <span>{label}</span>
      <div className="service-select">
        <select
          value={value}
          onChange={(event) => onChange(event.target.value)}
        >
          {children}
        </select>
        <ChevronDown size={14} />
      </div>
    </label>
  );
}

function Credential({
  label = "API Key",
  value,
  onChange,
  configured,
  online,
}: {
  label?: string;
  value: string;
  onChange: (value: string) => void;
  configured: boolean;
  online: boolean;
}) {
  const [visible, setVisible] = useState(false);
  return (
    <label className="service-field">
      <span>
        {label}
        {configured && (
          <em>
            <Check size={11} /> 已配置
          </em>
        )}
      </span>
      <div className="service-secret">
        <input
          type={visible ? "text" : "password"}
          autoComplete="new-password"
          spellCheck={false}
          maxLength={4096}
          value={value}
          placeholder={configured ? "留空保留已有密钥" : "填写服务商提供的密钥"}
          onChange={(event) => onChange(event.target.value)}
        />
        <button
          type="button"
          aria-label={`${visible ? "隐藏" : "显示"}${label}`}
          onClick={() => setVisible(!visible)}
        >
          {visible ? <EyeOff size={16} /> : <Eye size={16} />}
        </button>
      </div>
      <small>
        {online
          ? "密钥交由服务器保存，页面不会读取已有密钥。"
          : "本地草稿不保存密钥，离开页面后需重新填写。"}
      </small>
    </label>
  );
}

export default function ServiceSettings({
  section,
  data,
  setData,
  notify,
}: {
  section: ServiceSection;
  data: DeviceData;
  setData: Dispatch<SetStateAction<DeviceData>>;
  notify: (message: string) => void;
}) {
  const { mode, expire } = useConsole();
  const [draft, setDraft] = useState<ProviderValues>(() =>
    structuredClone(data.services.providers),
  );
  const [snapshot, setSnapshot] = useState<ProviderConfiguration | null>(null);
  const [secrets, setSecrets] = useState<ProviderSecrets>({});
  const [error, setError] = useState("");
  const [authRequired, setAuthRequired] = useState(false);
  const [busy, setBusy] = useState("");
  const [status, setStatus] = useState("");
  const requestRef = useRef<AbortController | null>(null);
  const lifetime = useRef<AbortController | null>(null);
  const transport = useRef(createServiceTransport()).current;
  const currentCopy = copy[section],
    Icon = currentCopy.icon;

  function showError(reason: unknown) {
    if (
      reason instanceof ServiceError &&
      reason.status === 401 &&
      mode === "live"
    )
      expire();
    setError(reason instanceof Error ? reason.message : "暂时无法读取配置。");
    setAuthRequired(
      reason instanceof ServiceError && [401, 403].includes(reason.status),
    );
  }

  async function load() {
    if (mode === "demo") {
      setStatus("界面预览 · 仅保存公共服务草稿，不连接真实服务。");
      return;
    }
    requestRef.current?.abort();
    const controller = new AbortController();
    requestRef.current = controller;
    setBusy("load");
    setError("");
    setStatus("");
    try {
      const result = await transport.load(controller.signal);
      if (controller.signal.aborted) return;
      setSnapshot(result);
      setDraft(structuredClone(result.config));
      setSecrets({});
      setAuthRequired(false);
    } catch (reason) {
      if (!controller.signal.aborted) showError(reason);
    } finally {
      if (!controller.signal.aborted) setBusy("");
    }
  }

  useEffect(() => {
    lifetime.current = new AbortController();
    void load();
    return () => {
      requestRef.current?.abort();
      lifetime.current?.abort();
    };
    // Service state is refreshed explicitly after the first read.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  function update(
    group: "openai" | "volcengine" | "seeduplex",
    key: string,
    value: string,
  ) {
    setDraft((previous) => ({
      ...previous,
      [group]: { ...previous[group], [key]: value },
    }));
    setStatus("");
  }
  function select(key: "llm" | "asr" | "tts" | "voice", value: string) {
    setDraft((previous) => ({ ...previous, [key]: value }));
    setStatus("");
  }
  const credential = (key: SecretName, label = "API Key") => (
    <Credential
      key={key}
      label={label}
      value={secrets[key] || ""}
      configured={snapshot?.credentials_configured[key] ?? false}
      online={!!snapshot}
      onChange={(value) => {
        setSecrets((previous) => ({ ...previous, [key]: value }));
        setStatus("");
      }}
    />
  );
  const field = (
    group: "openai" | "volcengine" | "seeduplex",
    key: string,
    label: string,
    placeholder?: string,
  ) => (
    <Field
      label={label}
      value={(draft[group] as Record<string, string>)[key]}
      onChange={(value) => update(group, key, value)}
      placeholder={placeholder}
      required
    />
  );

  async function save() {
    if (!snapshot) {
      setData((previous) => ({
        ...previous,
        services: {
          ...previous.services,
          providers: mergeProviderSection(
            previous.services.providers,
            draft,
            section,
          ),
        },
      }));
      setStatus("公共服务草稿已保存到当前浏览器，API Key 未保存。");
      return;
    }
    const config = mergeProviderSection(snapshot.config, draft, section);
    const names: SecretName[] =
      section === "llm"
        ? draft.llm === "openai"
          ? ["openai"]
          : []
        : draft.voice === "seeduplex"
          ? ["seeduplex"]
          : [
              ...(draft.asr === "openai" || draft.tts === "openai"
                ? ["openai" as const]
                : []),
              ...(draft.asr === "volcengine" ? ["asr" as const] : []),
              ...(draft.tts === "volcengine" ? ["tts" as const] : []),
            ];
    const sent = Object.fromEntries(
      names
        .filter((key) => !!secrets[key]?.trim())
        .map((key) => [key, secrets[key]!.trim()]),
    );
    try {
      validateProviderValues(config, snapshot.credentials_configured, sent);
    } catch (reason) {
      showError(reason);
      return;
    }
    requestRef.current?.abort();
    const controller = new AbortController();
    requestRef.current = controller;
    setBusy("save");
    setError("");
    setStatus("");
    try {
      const result = await transport.save(
        snapshot,
        config,
        sent,
        controller.signal,
      );
      if (controller.signal.aborted) return;
      setSnapshot(result);
      setDraft((previous) =>
        mergeProviderSection(previous, result.config, section),
      );
      setSecrets((previous) =>
        Object.fromEntries(
          Object.entries(previous).filter(
            ([key]) => !names.includes(key as SecretName),
          ),
        ),
      );
      setData((previous) => ({
        ...previous,
        services: {
          ...previous.services,
          providers: mergeProviderSection(
            previous.services.providers,
            result.config,
            section,
          ),
        },
      }));
      setStatus("配置已保存并应用到服务。");
      notify("服务配置已应用");
    } catch (reason) {
      if (!controller.signal.aborted) showError(reason);
    } finally {
      if (!controller.signal.aborted) setBusy("");
    }
  }

  return (
    <section
      className="card service-settings"
      aria-label={section === "llm" ? "大模型配置" : "语音配置"}
    >
      <header className="service-heading">
        <span className="service-heading-icon">
          <Icon size={23} strokeWidth={1.5} />
        </span>
        <div>
          <h2>{currentCopy.title}</h2>
          <p>{currentCopy.description}</p>
        </div>
        <span className={`service-state ${snapshot ? "is-ready" : ""}`}>
          {busy === "load" ? "读取中" : snapshot ? "服务已读取" : "本地草稿"}
        </span>
      </header>
      <div className="service-context">
        <span>
          {snapshot
            ? "大模型与语音是服务器公共配置，应用后供所有设备使用。"
            : "尚未连接配置服务，可先编辑草稿；密钥不会保存在浏览器。"}
        </span>
        <button type="button" disabled={!!busy} onClick={() => void load()}>
          <RefreshCw size={13} />
          重新读取
        </button>
      </div>
      {error && (
        <div className="service-alert" role="alert">
          <span>{error}</span>
          {authRequired && (
            <button type="button" onClick={expire}>
              <LogIn size={14} />
              重新登录
            </button>
          )}
        </div>
      )}
      <form
        noValidate
        onSubmit={(event) => {
          event.preventDefault();
          void save();
        }}
      >
        <fieldset disabled={!!busy} className="service-form">
          {section === "llm" && (
            <>
              <div className="service-fields">
                <Choice
                  label="服务类型"
                  value={draft.llm}
                  onChange={(value) => select("llm", value)}
                >
                  <option value="openai">OpenAI 兼容接口</option>
                  <option value="mock">演示回复</option>
                </Choice>
                {draft.llm === "openai" &&
                  field(
                    "openai",
                    "llm_model",
                    "模型名称",
                    "填写服务商的模型名称或接入点 ID",
                  )}
              </div>
              {draft.llm === "openai" ? (
                <>
                  <Field
                    label="服务地址"
                    value={draft.openai.base_url}
                    onChange={(value) => update("openai", "base_url", value)}
                    placeholder="https://api.example.com/v1"
                    required
                    hint="支持 OpenAI 兼容服务，如豆包、DeepSeek 或自建接口。"
                  />
                  {credential("openai")}
                  <details className="service-advanced">
                    <summary>
                      高级设置 <ChevronDown size={14} />
                    </summary>
                    <div className="service-fields">
                      <Choice
                        label="接口格式"
                        value={draft.openai.llm_api}
                        onChange={(value) => update("openai", "llm_api", value)}
                      >
                        <option value="chat_completions">
                          Chat Completions
                        </option>
                        <option value="responses">Responses</option>
                      </Choice>
                      <Choice
                        label="思考模式"
                        value={draft.openai.llm_thinking}
                        onChange={(value) =>
                          update("openai", "llm_thinking", value)
                        }
                      >
                        <option value="">服务默认</option>
                        <option value="auto">自动</option>
                        <option value="disabled">关闭</option>
                        <option value="enabled">开启</option>
                      </Choice>
                    </div>
                  </details>
                </>
              ) : (
                <p className="service-empty">
                  使用服务端演示回复，暂不调用真实大模型。
                </p>
              )}
            </>
          )}
          {section === "voice" && (
            <>
              <Choice
                label="对话方式"
                value={draft.voice}
                onChange={(value) => select("voice", value)}
              >
                <option value="pipeline">语音识别与朗读</option>
                <option value="seeduplex">豆包实时语音</option>
              </Choice>
              {draft.voice === "seeduplex" ? (
                <>
                  <div className="service-fields">
                    {field("seeduplex", "model", "模型版本")}
                    {field("seeduplex", "voice", "音色 ID")}
                  </div>
                  {credential("seeduplex", "实时语音 API Key")}
                  <details className="service-advanced">
                    <summary>
                      高级设置 <ChevronDown size={14} />
                    </summary>
                    {field("seeduplex", "endpoint", "实时语音地址")}
                  </details>
                </>
              ) : (
                <>
                  <div className="service-voice-columns">
                    {(["asr", "tts"] as const).map((kind) => (
                      <section className="service-voice-group" key={kind}>
                        <h3>
                          {kind === "asr"
                            ? "听你说 · 语音识别"
                            : "说给你听 · 语音合成"}
                        </h3>
                        <Choice
                          label="服务提供方"
                          value={draft[kind]}
                          onChange={(value) => select(kind, value)}
                        >
                          <option value="volcengine">火山引擎</option>
                          <option value="openai">OpenAI 兼容接口</option>
                          <option value="mock">演示服务</option>
                        </Choice>
                        {draft[kind] === "volcengine" && (
                          <>
                            {kind === "tts" &&
                              field(
                                "volcengine",
                                "tts_speaker",
                                "音色 ID",
                                "填写已开通的音色 ID",
                              )}
                            {credential(
                              kind,
                              kind === "asr" ? "识别 API Key" : "合成 API Key",
                            )}
                            <details className="service-advanced">
                              <summary>
                                高级设置 <ChevronDown size={14} />
                              </summary>
                              {field(
                                "volcengine",
                                `${kind}_resource_id`,
                                "资源 ID",
                              )}
                              {kind === "tts" &&
                                field("volcengine", "tts_model", "语音模型")}
                              {field(
                                "volcengine",
                                `${kind}_endpoint`,
                                "服务地址",
                              )}
                            </details>
                          </>
                        )}
                        {draft[kind] === "openai" && (
                          <>
                            {field("openai", `${kind}_model`, "语音模型")}
                            {kind === "tts" &&
                              field("openai", "tts_voice", "音色")}
                          </>
                        )}
                        {draft[kind] === "mock" && (
                          <p className="service-empty">使用演示服务。</p>
                        )}
                      </section>
                    ))}
                  </div>
                  {(draft.asr === "openai" || draft.tts === "openai") && (
                    <div className="service-shared">
                      <p>OpenAI 兼容语音与大模型共用服务地址和密钥。</p>
                      {field("openai", "base_url", "共用服务地址")}
                      {credential("openai", "共用 API Key")}
                    </div>
                  )}
                </>
              )}
              <p className="service-hint">
                这里配置现有主服务的语音提供方；独立 Voice Server
                的配置暂未接入此页面。
              </p>
            </>
          )}
        </fieldset>
        <footer className="service-save">
          <p role="status">
            {status ||
              (snapshot
                ? "应用后更新公共服务配置；不代表模型或语音已通过连通测试。"
                : "公开草稿仅保存在当前浏览器中。")}
          </p>
          <button type="submit" className="button primary" disabled={!!busy}>
            {busy === "save" ? (
              <LoaderCircle size={15} className="spinning" />
            ) : (
              <Check size={15} />
            )}
            {snapshot ? "保存并应用" : "保存草稿"}
          </button>
        </footer>
      </form>
    </section>
  );
}
