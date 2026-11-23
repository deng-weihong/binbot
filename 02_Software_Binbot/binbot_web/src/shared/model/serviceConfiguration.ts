export type ProviderValues = {
  llm: "mock" | "openai";
  asr: "mock" | "openai" | "volcengine";
  tts: "mock" | "openai" | "volcengine";
  voice: "pipeline" | "seeduplex";
  openai: {
    base_url: string;
    llm_model: string;
    llm_api: string;
    llm_thinking: string;
    asr_model: string;
    tts_model: string;
    tts_voice: string;
  };
  volcengine: {
    asr_endpoint: string;
    tts_endpoint: string;
    asr_resource_id: string;
    tts_resource_id: string;
    tts_model: string;
    tts_speaker: string;
  };
  seeduplex: { endpoint: string; model: string; voice: string };
};
export type SecretName = "openai" | "asr" | "tts" | "seeduplex";
export type ProviderSecrets = Partial<Record<SecretName, string>>;
export type ProviderConfiguration = {
  revision: string;
  config: ProviderValues;
  credentials_configured: Record<SecretName, boolean>;
};
export type ServicePreferences = {
  providers: ProviderValues;
  feishu: { appId: string; calendarId: string };
};

export function mergeProviderSection(
  base: ProviderValues,
  draft: ProviderValues,
  section: "llm" | "voice",
): ProviderValues {
  if (section === "llm")
    return {
      ...base,
      llm: draft.llm,
      openai: {
        ...base.openai,
        base_url: draft.openai.base_url,
        llm_model: draft.openai.llm_model,
        llm_api: draft.openai.llm_api,
        llm_thinking: draft.openai.llm_thinking,
      },
    };
  return {
    ...base,
    voice: draft.voice,
    asr: draft.asr,
    tts: draft.tts,
    openai: {
      ...base.openai,
      base_url: draft.openai.base_url,
      asr_model: draft.openai.asr_model,
      tts_model: draft.openai.tts_model,
      tts_voice: draft.openai.tts_voice,
    },
    volcengine: { ...draft.volcengine },
    seeduplex: { ...draft.seeduplex },
  };
}

export function createServicePreferences(): ServicePreferences {
  return {
    providers: {
      llm: "openai",
      asr: "volcengine",
      tts: "volcengine",
      voice: "pipeline",
      openai: {
        base_url: "https://api.openai.com/v1",
        llm_model: "gpt-4o-mini",
        llm_api: "chat_completions",
        llm_thinking: "",
        asr_model: "whisper-1",
        tts_model: "gpt-4o-mini-tts",
        tts_voice: "alloy",
      },
      volcengine: {
        asr_endpoint:
          "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
        tts_endpoint:
          "wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream",
        asr_resource_id: "volc.seedasr.sauc.duration",
        tts_resource_id: "seed-tts-2.0",
        tts_model: "seed-tts-2.0-standard",
        tts_speaker: "",
      },
      seeduplex: {
        endpoint:
          "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue",
        model: "1.2.6.1",
        voice: "zh_male_xiaotian_jupiter_bigtts",
      },
    },
    feishu: { appId: "", calendarId: "" },
  };
}

function object(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}
function strings<T extends Record<string, string>>(
  value: unknown,
  defaults: T,
): T {
  const source = object(value);
  return Object.fromEntries(
    Object.entries(defaults).map(([key, fallback]) => [
      key,
      typeof source[key] === "string" ? source[key].slice(0, 2048) : fallback,
    ]),
  ) as T;
}

/** Only public fields enter device storage; keys and login credentials stay in memory. */
export function normalizeServicePreferences(
  value: unknown,
): ServicePreferences {
  const defaults = createServicePreferences();
  const source = object(value),
    providers = object(source.providers);
  const select = <T extends string>(
    field: string,
    allowed: readonly T[],
    fallback: T,
  ): T =>
    allowed.includes(providers[field] as T)
      ? (providers[field] as T)
      : fallback;
  return {
    providers: {
      llm: select("llm", ["mock", "openai"], defaults.providers.llm),
      asr: select(
        "asr",
        ["mock", "openai", "volcengine"],
        defaults.providers.asr,
      ),
      tts: select(
        "tts",
        ["mock", "openai", "volcengine"],
        defaults.providers.tts,
      ),
      voice: select(
        "voice",
        ["pipeline", "seeduplex"],
        defaults.providers.voice,
      ),
      openai: strings(providers.openai, defaults.providers.openai),
      volcengine: strings(providers.volcengine, defaults.providers.volcengine),
      seeduplex: strings(providers.seeduplex, defaults.providers.seeduplex),
    },
    feishu: strings(source.feishu, defaults.feishu),
  };
}

export function validServiceUrl(value: string, protocols: string[]) {
  try {
    const url = new URL(value);
    return (
      protocols.includes(url.protocol) &&
      !!url.hostname &&
      !url.username &&
      !url.password &&
      !url.hash
    );
  } catch {
    return false;
  }
}

export function validateProviderValues(
  config: ProviderValues,
  credentials: Record<SecretName, boolean>,
  secrets: ProviderSecrets,
) {
  const has = (key: SecretName) => !!secrets[key]?.trim() || credentials[key];
  const required = (value: string, label: string) => {
    if (!value.trim()) throw new Error(`请填写${label}。`);
  };
  if (
    config.llm === "openai" ||
    (config.voice === "pipeline" && [config.asr, config.tts].includes("openai"))
  ) {
    if (!validServiceUrl(config.openai.base_url, ["http:", "https:"]))
      throw new Error("请填写有效的模型服务地址。");
    if (!has("openai")) throw new Error("请填写 OpenAI 兼容服务的 API Key。");
  }
  if (config.llm === "openai") required(config.openai.llm_model, "模型名称");
  if (config.voice === "seeduplex") {
    if (!validServiceUrl(config.seeduplex.endpoint, ["ws:", "wss:"]))
      throw new Error("请填写有效的实时语音地址。");
    required(config.seeduplex.model, "实时语音模型");
    required(config.seeduplex.voice, "实时语音音色");
    if (!has("seeduplex")) throw new Error("请填写实时语音的 API Key。");
  } else {
    for (const kind of ["asr", "tts"] as const) {
      if (config[kind] === "volcengine") {
        if (
          !validServiceUrl(config.volcengine[`${kind}_endpoint`], [
            "ws:",
            "wss:",
          ])
        )
          throw new Error("请填写有效的语音服务地址。");
        required(config.volcengine[`${kind}_resource_id`], "语音资源 ID");
        if (!has(kind))
          throw new Error(
            `请填写${kind === "asr" ? "语音识别" : "语音合成"}的 API Key。`,
          );
      }
      if (config[kind] === "openai")
        required(config.openai[`${kind}_model`], "语音模型名称");
    }
    if (config.tts === "volcengine") {
      required(config.volcengine.tts_model, "语音合成模型");
      required(config.volcengine.tts_speaker, "音色 ID");
    }
    if (config.tts === "openai") required(config.openai.tts_voice, "音色");
  }
}

export class ServiceError extends Error {
  constructor(
    public status: number,
    message: string,
  ) {
    super(message);
  }
}

function parseConfiguration(
  payload: Record<string, unknown>,
): ProviderConfiguration {
  const config = object(payload.config),
    defaults = createServicePreferences().providers;
  if (
    typeof payload.revision !== "string" ||
    !payload.revision ||
    !["mock", "openai"].includes(String(config.llm)) ||
    !["mock", "openai", "volcengine"].includes(String(config.asr)) ||
    !["mock", "openai", "volcengine"].includes(String(config.tts)) ||
    !["pipeline", "seeduplex"].includes(String(config.voice)) ||
    !["openai", "volcengine", "seeduplex"].every((group) =>
      Object.keys(
        defaults[group as "openai" | "volcengine" | "seeduplex"],
      ).every((key) => typeof object(config[group])[key] === "string"),
    )
  )
    throw new ServiceError(502, "配置服务返回的内容不完整，请检查后端版本。");
  const credentials = object(payload.credentials_configured);
  return {
    revision: payload.revision,
    config: normalizeServicePreferences({ providers: config }).providers,
    credentials_configured: Object.fromEntries(
      ["openai", "asr", "tts", "seeduplex"].map((key) => [
        key,
        credentials[key] === true,
      ]),
    ) as Record<SecretName, boolean>,
  };
}

export function providerSaveBody(
  snapshot: ProviderConfiguration,
  config: ProviderValues,
  secrets: ProviderSecrets,
) {
  return {
    revision: snapshot.revision,
    config: normalizeServicePreferences({ providers: config }).providers,
    secrets: Object.fromEntries(
      (["openai", "asr", "tts", "seeduplex"] as const)
        .filter((key) => !!secrets[key]?.trim())
        .map((key) => [key, secrets[key]!.trim()]),
    ),
  };
}

export function createServiceTransport(request: typeof fetch = fetch) {
  async function json(path: string, options: RequestInit = {}) {
    let response: Response;
    try {
      response = await request(path, {
        ...options,
        credentials: "same-origin",
        cache: "no-store",
        headers: { Accept: "application/json", ...options.headers },
        signal: options.signal
          ? AbortSignal.any([options.signal, AbortSignal.timeout(15000)])
          : AbortSignal.timeout(15000),
      });
    } catch (error) {
      if (options.signal?.aborted) throw error;
      throw new ServiceError(
        0,
        "暂时无法连接配置服务，可先保存不含密钥的草稿。",
      );
    }
    if (response.status === 401 || response.status === 403)
      throw new ServiceError(
        response.status,
        "登录管理服务后，才能读取或应用配置。",
      );
    if (response.status === 409)
      throw new ServiceError(409, "配置已在其他窗口修改，请重新读取后再保存。");
    if (!response.headers.get("Content-Type")?.includes("application/json"))
      throw new ServiceError(
        response.status,
        "配置服务尚未接入，可先编辑本地草稿。",
      );
    const payload = object(await response.json());
    if (!response.ok) {
      const message = typeof payload.error === "string" ? payload.error : "";
      throw new ServiceError(
        response.status,
        /same API key|sharing one credential/.test(message)
          ? "当前语音识别与合成共用一个密钥，请填写相同的 API Key。"
          : response.status === 404
            ? "该服务尚未启用，请先完成服务端配置。"
            : "配置未能应用，请检查参数、密钥及服务端状态。",
      );
    }
    return payload;
  }
  return {
    async load(signal?: AbortSignal) {
      return parseConfiguration(
        await json("/api/v1/admin/providers/configuration", { signal }),
      );
    },
    async save(
      snapshot: ProviderConfiguration,
      config: ProviderValues,
      secrets: ProviderSecrets,
      signal?: AbortSignal,
    ) {
      return parseConfiguration(
        await json("/api/v1/admin/providers/configuration", {
          method: "PUT",
          signal,
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(providerSaveBody(snapshot, config, secrets)),
        }),
      );
    },
    async login(username: string, password: string, signal?: AbortSignal) {
      await json("/api/v1/admin/session", {
        method: "POST",
        signal,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password, remember: false }),
      });
    },
  };
}
