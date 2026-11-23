from __future__ import annotations

import json
import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping
from datetime import time
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


@dataclass(frozen=True, slots=True)
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    log_level: str = "INFO"
    handshake_timeout_seconds: float = 5.0
    max_control_bytes: int = 16_384
    max_audio_payload_bytes: int = 4_096
    asset_directory: str = "/tmp/binbot_server_assets"
    max_gif_bytes: int = 16 * 1024 * 1024
    ffmpeg_path: str = "/usr/bin/ffmpeg"
    video_start_timeout_seconds: float = 10.0


@dataclass(frozen=True, slots=True)
class AuthConfig:
    allow_anonymous: bool = True
    tokens: Mapping[str, str] = field(default_factory=dict)
    tokens_env: str = "BINBOT_DEVICE_TOKENS_JSON"


@dataclass(frozen=True, slots=True)
class AdminConfig:
    username_env: str = "BINBOT_ADMIN_USERNAME"
    password_env: str = "BINBOT_ADMIN_PASSWORD"
    remember_days: int = 30
    # Legacy name kept as a password fallback for existing deployments.
    token_env: str = "BINBOT_ADMIN_TOKEN"
    allow_unauthenticated_loopback: bool = True


@dataclass(frozen=True, slots=True)
class MQTTConfig:
    enabled: bool = False
    host: str = "127.0.0.1"
    port: int = 1883
    websocket_port: int = 1884
    max_connections: int = 128
    tls_cert_file: str = ""
    tls_key_file: str = ""
    heartbeat_timeout_seconds: int = 20
    command_timeout_seconds: int = 15


@dataclass(frozen=True, slots=True)
class CameraConfig:
    enabled: bool = False
    api_url: str = "http://media:80"
    viewer_token_env: str = "BINBOT_MEDIA_VIEWER_TOKEN"
    viewer_ttl_seconds: float = 35.0
    stop_delay_seconds: float = 30.0


@dataclass(frozen=True, slots=True)
class AudioConfig:
    uplink_sample_rate: int = 16_000
    uplink_frame_ms: int = 20
    downlink_sample_rate: int = 24_000
    downlink_frame_ms: int = 20
    downlink_bitrate: int = 32_000


@dataclass(frozen=True, slots=True)
class PipelineConfig:
    max_turn_audio_seconds: float = 30.0
    asr_timeout_seconds: float = 30.0
    llm_timeout_seconds: float = 60.0
    tts_timeout_seconds: float = 60.0
    tts_concurrency: int = 2
    max_tts_segments: int = 64
    max_tts_segment_bytes: int = 1_440_000


@dataclass(frozen=True, slots=True)
class VADConfig:
    enabled: bool = True
    threshold: float = 0.5
    threshold_low: float = 0.3
    end_silence_ms: int = 1800
    transcript_grace_ms: int = 2400
    start_timeout_ms: int = 10000


@dataclass(frozen=True, slots=True)
class ConversationConfig:
    max_history_messages: int = 20
    first_sentence_chars: int = 12
    system_prompt: str = (
        "你是 BinBot，一个运行在桌面智能终端上的语音助手。"
        "回答应自然、准确、简洁，适合直接通过语音播报。"
    )


@dataclass(frozen=True, slots=True)
class StorageConfig:
    kind: str = "memory"
    sqlite_path: str = "/tmp/binbot_server.sqlite3"
    max_commands: int = 500
    max_tasks: int = 500


@dataclass(frozen=True, slots=True)
class SchedulerConfig:
    poll_interval_seconds: float = 1.0
    batch_size: int = 10


@dataclass(frozen=True, slots=True)
class DailyReportConfig:
    enabled: bool = True
    timezone: str = "Asia/Shanghai"
    generate_at: str = "23:00"
    retention_days: int = 90
    max_context_chars: int = 24000


@dataclass(frozen=True, slots=True)
class FeishuCalendarConfig:
    enabled: bool = False
    calendar_id: str = ""
    sync_interval_seconds: float = 60.0
    max_events: int = 16
    user_access_token_env: str = "BINBOT_FEISHU_USER_ACCESS_TOKEN"
    token_file: str = "/tmp/binbot_feishu_user_token.json"
    oauth_scopes: tuple[str, ...] = (
        "calendar:calendar:readonly", "calendar:calendar.event:create",
    )
    subscribe: bool = True

    def user_access_token(self) -> str:
        return os.environ.get(self.user_access_token_env, "").strip()


@dataclass(frozen=True, slots=True)
class FeishuConfig:
    enabled: bool = False
    mode: str = "long_connection"
    app_id: str = ""
    credentials_file: str = ""
    app_secret_env: str = "BINBOT_FEISHU_APP_SECRET"
    verification_token_env: str = "BINBOT_FEISHU_VERIFICATION_TOKEN"
    base_url: str = "https://open.feishu.cn"
    receive_id_type: str = "chat_id"
    device_id: str = ""
    allow_control: bool = False
    request_timeout_seconds: float = 10.0
    calendar: FeishuCalendarConfig = field(default_factory=FeishuCalendarConfig)

    def app_secret(self) -> str:
        value = os.environ.get(self.app_secret_env, "")
        if not value:
            raise ValueError(
                f"required secret environment variable is not set: {self.app_secret_env}"
            )
        return value

    def credentials(self) -> tuple[str, str]:
        if self.credentials_file.strip():
            path = Path(self.credentials_file).expanduser()
            try:
                values = [line.strip() for line in path.read_text().splitlines() if line.strip()]
            except OSError as exc:
                raise ValueError(f"could not read Feishu credentials file: {path}") from exc
            if len(values) != 2:
                raise ValueError("Feishu credentials file must contain App ID and App Secret")
            return values[0], values[1]
        if not self.app_id.strip():
            raise ValueError("feishu.app_id must not be empty")
        return self.app_id, self.app_secret()

    def verification_token(self) -> str:
        return os.environ.get(self.verification_token_env, "")


@dataclass(frozen=True, slots=True)
class AgentConfig:
    enabled: bool = True
    max_context_chars: int = 12_000
    require_control_approval: bool = True
    max_tool_rounds: int = 4
    max_memory_items: int = 5


@dataclass(frozen=True, slots=True)
class MockProviderConfig:
    transcript: str = "你好，我是 BinBot 的测试语音。"
    response: str = "你好，我是 BinBot。服务端语音链路工作正常。"


@dataclass(frozen=True, slots=True)
class OpenAIProviderConfig:
    base_url: str = "https://api.openai.com/v1"
    api_key_env: str = "BINBOT_MODEL_API_KEY"
    asr_model: str = "whisper-1"
    llm_model: str = "gpt-4o-mini"
    llm_api: str = "chat_completions"
    llm_thinking: str = ""
    tts_model: str = "gpt-4o-mini-tts"
    tts_voice: str = "alloy"
    request_timeout_seconds: float = 60.0

    def api_key(self) -> str:
        value = os.environ.get(self.api_key_env, "")
        if not value:
            raise ValueError(f"required secret environment variable is not set: {self.api_key_env}")
        return value


@dataclass(frozen=True, slots=True)
class VolcengineProviderConfig:
    """Volcengine Speech settings; the API key is always read from the environment."""

    asr_endpoint: str = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
    tts_endpoint: str = "wss://openspeech.bytedance.com/api/v3/tts/unidirectional/stream"
    api_key_env: str = "BINBOT_VOLC_API_KEY"
    asr_api_key_env: str = ""
    tts_api_key_env: str = ""
    asr_resource_id: str = "volc.seedasr.sauc.duration"
    tts_resource_id: str = "seed-tts-2.0"
    tts_model: str = "seed-tts-2.0-standard"
    tts_speaker: str = ""
    asr_packet_ms: int = 200
    request_timeout_seconds: float = 60.0
    max_audio_seconds: float = 60.0
    max_message_bytes: int = 4 * 1024 * 1024

    def api_key(self) -> str:
        value = os.environ.get(self.api_key_env, "")
        if not value:
            raise ValueError(
                f"required secret environment variable is not set: {self.api_key_env}"
            )
        return value

    def asr_api_key(self) -> str:
        env_name = self.asr_api_key_env or self.api_key_env
        value = os.environ.get(env_name, "")
        if not value:
            raise ValueError(f"required secret environment variable is not set: {env_name}")
        return value

    def tts_api_key(self) -> str:
        env_name = self.tts_api_key_env or self.api_key_env
        value = os.environ.get(env_name, "")
        if not value:
            raise ValueError(f"required secret environment variable is not set: {env_name}")
        return value


@dataclass(frozen=True, slots=True)
class SeeduplexConfig:
    endpoint: str = "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
    model: str = "1.2.6.1"
    voice: str = "zh_male_xiaotian_jupiter_bigtts"
    api_key_env: str = "BINBOT_SEEDUPLEX_API_KEY"
    connect_timeout_seconds: float = 15.0
    idle_timeout_seconds: float = 90.0
    max_message_bytes: int = 4 * 1024 * 1024
    allow_interruption: bool = False

    def api_key(self) -> str:
        value = os.environ.get(self.api_key_env, "").strip()
        if not value:
            raise ValueError(f"required secret environment variable is not set: {self.api_key_env}")
        return value


@dataclass(frozen=True, slots=True)
class ProviderConfig:
    asr: str = "mock"
    llm: str = "mock"
    tts: str = "mock"
    voice: str = "pipeline"
    settings_directory: str = ""
    mock: MockProviderConfig = field(default_factory=MockProviderConfig)
    openai: OpenAIProviderConfig = field(default_factory=OpenAIProviderConfig)
    volcengine: VolcengineProviderConfig = field(default_factory=VolcengineProviderConfig)
    seeduplex: SeeduplexConfig = field(default_factory=SeeduplexConfig)


@dataclass(frozen=True, slots=True)
class ComponentConfig:
    directory: str = "/tmp/binbot_component_ota"
    public_origin: str = ""
    token_env: str = "BINBOT_MCP_SERVICE_TOKEN"
    scopes: tuple[str, ...] = ("device:read", "firmware:read", "firmware:write", "component:ota:write")
    device_ids: tuple[str, ...] = ()
    max_firmware_bytes: int = 2 * 1024 * 1024
    allow_unauthenticated: bool = False


@dataclass(frozen=True, slots=True)
class Settings:
    server: ServerConfig = field(default_factory=ServerConfig)
    auth: AuthConfig = field(default_factory=AuthConfig)
    admin: AdminConfig = field(default_factory=AdminConfig)
    audio: AudioConfig = field(default_factory=AudioConfig)
    pipeline: PipelineConfig = field(default_factory=PipelineConfig)
    vad: VADConfig = field(default_factory=VADConfig)
    conversation: ConversationConfig = field(default_factory=ConversationConfig)
    storage: StorageConfig = field(default_factory=StorageConfig)
    scheduler: SchedulerConfig = field(default_factory=SchedulerConfig)
    daily_report: DailyReportConfig = field(default_factory=DailyReportConfig)
    feishu: FeishuConfig = field(default_factory=FeishuConfig)
    agent: AgentConfig = field(default_factory=AgentConfig)
    providers: ProviderConfig = field(default_factory=ProviderConfig)
    component: ComponentConfig = field(default_factory=ComponentConfig)
    camera: CameraConfig = field(default_factory=CameraConfig)
    mqtt: MQTTConfig = field(default_factory=MQTTConfig)


def _section(data: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    value = data.get(name, {})
    if not isinstance(value, Mapping):
        raise ValueError(f"configuration section {name!r} must be a table")
    return value


def load_settings(path: str | Path | None = None) -> Settings:
    raw_path: str | Path
    if path is not None:
        raw_path = path
    else:
        raw_path = os.environ.get("BINBOT_SERVER_CONFIG") or "config.toml"
    config_path = Path(raw_path)
    data: Mapping[str, Any] = {}
    if config_path.exists():
        with config_path.open("rb") as stream:
            data = tomllib.load(stream)

    server = ServerConfig(**_section(data, "server"))
    auth_data = dict(_section(data, "auth"))
    tokens = auth_data.pop("tokens", {})
    auth = AuthConfig(tokens=dict(tokens), **auth_data)
    environment_tokens = os.environ.get(auth.tokens_env, "").strip()
    if environment_tokens:
        try:
            parsed_tokens = json.loads(environment_tokens)
        except json.JSONDecodeError as exc:
            raise ValueError(
                f"{auth.tokens_env} must contain a JSON object of device tokens"
            ) from exc
        if (
            not isinstance(parsed_tokens, dict)
            or any(
                not isinstance(device_id, str)
                or not device_id.strip()
                or not isinstance(token, str)
                or not token
                for device_id, token in parsed_tokens.items()
            )
        ):
            raise ValueError(
                f"{auth.tokens_env} must contain non-empty string device IDs and tokens"
            )
        auth = AuthConfig(
            allow_anonymous=auth.allow_anonymous,
            tokens={**auth.tokens, **parsed_tokens},
            tokens_env=auth.tokens_env,
        )
    admin = AdminConfig(**_section(data, "admin"))
    audio = AudioConfig(**_section(data, "audio"))
    pipeline = PipelineConfig(**_section(data, "pipeline"))
    conversation = ConversationConfig(**_section(data, "conversation"))
    storage = StorageConfig(**_section(data, "storage"))
    scheduler = SchedulerConfig(**_section(data, "scheduler"))
    daily_report = DailyReportConfig(**_section(data, "daily_report"))
    feishu_data = dict(_section(data, "feishu"))
    calendar_data = dict(feishu_data.pop("calendar", {}))
    if isinstance(calendar_data.get("oauth_scopes"), list):
        calendar_data["oauth_scopes"] = tuple(calendar_data["oauth_scopes"])
    calendar = FeishuCalendarConfig(**calendar_data)
    feishu = FeishuConfig(calendar=calendar, **feishu_data)
    agent = AgentConfig(**_section(data, "agent"))

    providers_data = dict(_section(data, "providers"))
    mock = MockProviderConfig(**dict(providers_data.pop("mock", {})))
    openai = OpenAIProviderConfig(**dict(providers_data.pop("openai", {})))
    volcengine = VolcengineProviderConfig(**dict(providers_data.pop("volcengine", {})))
    seeduplex = SeeduplexConfig(**dict(providers_data.pop("seeduplex", {})))
    providers = ProviderConfig(
        mock=mock, openai=openai, volcengine=volcengine, seeduplex=seeduplex, **providers_data
    )
    settings = Settings(
        server=server,
        auth=auth,
        admin=admin,
        audio=audio,
        pipeline=pipeline,
        vad=VADConfig(**_section(data, "vad")),
        conversation=conversation,
        storage=storage,
        scheduler=scheduler,
        daily_report=daily_report,
        feishu=feishu,
        agent=agent,
        providers=providers,
        component=ComponentConfig(**_section(data, "component")),
        camera=CameraConfig(**_section(data, "camera")),
        mqtt=MQTTConfig(**_section(data, "mqtt")),
    )
    validate_settings(settings)
    from binbot_server.infrastructure.provider_settings import load_provider_overrides
    return load_provider_overrides(settings)


def validate_settings(settings: Settings) -> None:
    mqtt = settings.mqtt
    if not isinstance(mqtt.enabled, bool) or not mqtt.host.strip():
        raise ValueError("mqtt.enabled must be boolean and mqtt.host must not be empty")
    if any(isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535
           for port in (mqtt.port, mqtt.websocket_port)):
        raise ValueError("mqtt ports must be in 1..65535")
    if len({mqtt.port, mqtt.websocket_port, settings.server.port}) != 3:
        raise ValueError("mqtt ports and server.port must be distinct")
    if not 1 <= mqtt.max_connections <= 10000:
        raise ValueError("mqtt.max_connections must be in 1..10000")
    if bool(mqtt.tls_cert_file) != bool(mqtt.tls_key_file):
        raise ValueError("mqtt TLS certificate and key must be configured together")
    if not 5 <= mqtt.heartbeat_timeout_seconds <= 300 or not 2 <= mqtt.command_timeout_seconds <= 60:
        raise ValueError("invalid mqtt heartbeat or command timeout")
    if not 0 <= settings.vad.threshold_low < settings.vad.threshold <= 1:
        raise ValueError("vad thresholds must satisfy 0 <= threshold_low < threshold <= 1")
    if not 500 <= settings.vad.end_silence_ms <= 5000:
        raise ValueError("vad.end_silence_ms must be between 500 and 5000")
    if not 500 <= settings.vad.transcript_grace_ms <= 5000:
        raise ValueError("vad.transcript_grace_ms must be between 500 and 5000")
    if not 1000 <= settings.vad.start_timeout_ms <= 30000:
        raise ValueError("vad.start_timeout_ms must be between 1000 and 30000")
    from urllib.parse import urlsplit
    media_url = urlsplit(settings.camera.api_url)
    if settings.camera.enabled and (
        media_url.scheme not in {"http", "https"} or not media_url.netloc
        or media_url.path not in {"", "/"} or media_url.query or media_url.fragment
        or media_url.username or media_url.password
    ):
        raise ValueError("camera.api_url must be an HTTP(S) origin")
    if not 20 <= settings.camera.viewer_ttl_seconds <= 120:
        raise ValueError("camera.viewer_ttl_seconds must be between 20 and 120")
    if not 0 <= settings.camera.stop_delay_seconds <= 120:
        raise ValueError("camera.stop_delay_seconds must be between 0 and 120")
    origin = urlsplit(settings.component.public_origin)
    if settings.component.public_origin and (
        origin.scheme not in {"http", "https"} or not origin.netloc
        or origin.path not in {"", "/"} or origin.query or origin.fragment
        or origin.username or origin.password
    ):
        raise ValueError("component.public_origin must be an HTTP(S) origin")
    if not 288 <= settings.component.max_firmware_bytes <= 16 * 1024 * 1024:
        raise ValueError("component.max_firmware_bytes must be between 288 and 16777216")
    if not (1 <= settings.server.port <= 65_535):
        raise ValueError("server.port must be in the range 1..65535")
    if settings.server.handshake_timeout_seconds <= 0:
        raise ValueError("server.handshake_timeout_seconds must be positive")
    if not (256 <= settings.server.max_control_bytes <= 1_048_576):
        raise ValueError("server.max_control_bytes must be in the range 256..1048576")
    if not (1 <= settings.server.max_audio_payload_bytes <= 4_096):
        raise ValueError("server.max_audio_payload_bytes must be in the range 1..4096")
    if not (1_024 <= settings.server.max_gif_bytes <= 64 * 1024 * 1024):
        raise ValueError("server.max_gif_bytes must be in the range 1024..67108864")
    if not settings.server.asset_directory.strip():
        raise ValueError("server.asset_directory must not be empty")
    if not settings.server.ffmpeg_path.strip():
        raise ValueError("server.ffmpeg_path must not be empty")
    if settings.server.video_start_timeout_seconds <= 0:
        raise ValueError("server.video_start_timeout_seconds must be positive")
    if not settings.auth.tokens_env.strip():
        raise ValueError("auth.tokens_env must not be empty")
    if settings.storage.max_commands < 1:
        raise ValueError("storage.max_commands must be positive")
    if settings.storage.max_tasks < 1:
        raise ValueError("storage.max_tasks must be positive")
    if settings.scheduler.poll_interval_seconds <= 0:
        raise ValueError("scheduler.poll_interval_seconds must be positive")
    if settings.scheduler.batch_size < 1:
        raise ValueError("scheduler.batch_size must be positive")
    try:
        ZoneInfo(settings.daily_report.timezone)
    except (ZoneInfoNotFoundError, ValueError) as exc:
        raise ValueError("daily_report.timezone must be a valid IANA timezone") from exc
    try:
        parsed_time = time.fromisoformat(settings.daily_report.generate_at)
    except ValueError as exc:
        raise ValueError("daily_report.generate_at must use HH:MM") from exc
    if parsed_time.strftime("%H:%M") != settings.daily_report.generate_at:
        raise ValueError("daily_report.generate_at must use HH:MM")
    if not 7 <= settings.daily_report.retention_days <= 365:
        raise ValueError("daily_report.retention_days must be in 7..365")
    if not 1000 <= settings.daily_report.max_context_chars <= 100000:
        raise ValueError("daily_report.max_context_chars must be in 1000..100000")
    feishu = settings.feishu
    if feishu.enabled and not feishu.app_id.strip() and not feishu.credentials_file.strip():
        raise ValueError(
            "feishu.app_id or feishu.credentials_file is required when Feishu is enabled"
        )
    if feishu.mode not in {"long_connection", "webhook"}:
        raise ValueError("feishu.mode must be long_connection or webhook")
    if not feishu.base_url.startswith(("http://", "https://")):
        raise ValueError("feishu.base_url must be an HTTP URL")
    if feishu.receive_id_type not in {"chat_id", "open_id", "user_id", "union_id"}:
        raise ValueError("unsupported feishu.receive_id_type")
    if feishu.request_timeout_seconds <= 0:
        raise ValueError("feishu.request_timeout_seconds must be positive")
    calendar = feishu.calendar
    if calendar.sync_interval_seconds < 0:
        raise ValueError("feishu.calendar.sync_interval_seconds must not be negative")
    if not 1 <= calendar.max_events <= 100:
        raise ValueError("feishu.calendar.max_events must be in 1..100")
    if not calendar.token_file.strip():
        raise ValueError("feishu.calendar.token_file must not be empty")
    if not calendar.oauth_scopes:
        raise ValueError("feishu.calendar.oauth_scopes must not be empty")
    if not settings.admin.username_env.strip():
        raise ValueError("admin.username_env must not be empty")
    if not settings.admin.password_env.strip():
        raise ValueError("admin.password_env must not be empty")
    if not settings.admin.token_env.strip():
        raise ValueError("admin.token_env must not be empty")
    if not (1 <= settings.admin.remember_days <= 365):
        raise ValueError("admin.remember_days must be in the range 1..365")
    if settings.audio.uplink_sample_rate not in {8_000, 12_000, 16_000, 24_000, 48_000}:
        raise ValueError("unsupported Opus uplink sample rate")
    if settings.audio.downlink_sample_rate not in {8_000, 12_000, 16_000, 24_000, 48_000}:
        raise ValueError("unsupported Opus downlink sample rate")
    if settings.audio.uplink_frame_ms not in {10, 20, 40, 60}:
        raise ValueError("unsupported uplink frame duration")
    if settings.audio.downlink_frame_ms not in {10, 20, 40, 60}:
        raise ValueError("unsupported downlink frame duration")
    if not (6_000 <= settings.audio.downlink_bitrate <= 128_000):
        raise ValueError("audio.downlink_bitrate must be in the range 6000..128000")
    pipeline = settings.pipeline
    for name, value in (
        ("max_turn_audio_seconds", pipeline.max_turn_audio_seconds),
        ("asr_timeout_seconds", pipeline.asr_timeout_seconds),
        ("llm_timeout_seconds", pipeline.llm_timeout_seconds),
        ("tts_timeout_seconds", pipeline.tts_timeout_seconds),
    ):
        if value <= 0:
            raise ValueError(f"pipeline.{name} must be positive")
    if not (1 <= pipeline.tts_concurrency <= 8):
        raise ValueError("pipeline.tts_concurrency must be in the range 1..8")
    if not (1 <= pipeline.max_tts_segments <= 256):
        raise ValueError("pipeline.max_tts_segments must be in the range 1..256")
    if not (960 <= pipeline.max_tts_segment_bytes <= 9_600_000):
        raise ValueError("pipeline.max_tts_segment_bytes must be in the range 960..9600000")
    if not (0 <= settings.conversation.max_history_messages <= 200):
        raise ValueError("conversation.max_history_messages must be in the range 0..200")
    if not (1 <= settings.conversation.first_sentence_chars <= 80):
        raise ValueError("conversation.first_sentence_chars must be in the range 1..80")
    if not settings.conversation.system_prompt.strip():
        raise ValueError("conversation.system_prompt must not be empty")
    if settings.storage.kind not in {"memory", "sqlite"}:
        raise ValueError("storage.kind must be memory or sqlite")
    if settings.storage.kind == "sqlite" and not settings.storage.sqlite_path.strip():
        raise ValueError("storage.sqlite_path must not be empty")
    if not (1 <= settings.storage.max_commands <= 100_000):
        raise ValueError("storage.max_commands must be in the range 1..100000")
    if not (1 <= settings.storage.max_tasks <= 100_000):
        raise ValueError("storage.max_tasks must be in the range 1..100000")
    if not (256 <= settings.agent.max_context_chars <= 1_000_000):
        raise ValueError("agent.max_context_chars must be in the range 256..1000000")
    if not (1 <= settings.agent.max_tool_rounds <= 8):
        raise ValueError("agent.max_tool_rounds must be in the range 1..8")
    if not (0 <= settings.agent.max_memory_items <= 20):
        raise ValueError("agent.max_memory_items must be in the range 0..20")
    supported = {"mock", "openai", "volcengine"}
    if settings.providers.voice not in {"pipeline", "seeduplex"}:
        raise ValueError("providers.voice must be pipeline or seeduplex")
    realtime = settings.providers.seeduplex
    if not isinstance(realtime.allow_interruption, bool):
        raise ValueError("providers.seeduplex.allow_interruption must be a boolean")
    realtime_url = urlsplit(realtime.endpoint)
    if (realtime_url.scheme not in {"ws", "wss"} or not realtime_url.netloc
            or realtime_url.username or realtime_url.password or realtime_url.fragment):
        raise ValueError("providers.seeduplex.endpoint must be a WebSocket URL")
    if realtime.model != "1.2.6.1":
        raise ValueError("providers.seeduplex.model must be 1.2.6.1")
    if not realtime.voice.strip() or not realtime.api_key_env.strip():
        raise ValueError("providers.seeduplex.voice and api_key_env are required")
    if not 1 <= realtime.connect_timeout_seconds <= 60:
        raise ValueError("providers.seeduplex.connect_timeout_seconds must be in 1..60")
    if not 10 <= realtime.idle_timeout_seconds <= 600:
        raise ValueError("providers.seeduplex.idle_timeout_seconds must be in 10..600")
    if not 65_536 <= realtime.max_message_bytes <= 16 * 1024 * 1024:
        raise ValueError("providers.seeduplex.max_message_bytes is out of range")
    if settings.providers.voice == "seeduplex" and (
        settings.audio.uplink_sample_rate != 16000 or settings.audio.uplink_frame_ms != 20
        or settings.audio.downlink_sample_rate != 24000
    ):
        raise ValueError("Seeduplex requires 16 kHz / 20 ms input and 24 kHz output")
    for kind in (settings.providers.asr, settings.providers.llm, settings.providers.tts):
        if kind not in supported:
            raise ValueError(f"unsupported provider kind: {kind}")
    if settings.providers.llm == "volcengine":
        raise ValueError(
            "Volcengine Speech has no LLM adapter; configure providers.llm as mock or openai"
        )
    openai = settings.providers.openai
    if openai.llm_api not in {"chat_completions", "responses"}:
        raise ValueError(
            "providers.openai.llm_api must be chat_completions or responses"
        )
    if openai.llm_thinking not in {"", "disabled", "enabled", "auto"}:
        raise ValueError(
            "providers.openai.llm_thinking must be empty, disabled, enabled, or auto"
        )
    if openai.request_timeout_seconds <= 0:
        raise ValueError("providers.openai.request_timeout_seconds must be positive")
    volc = settings.providers.volcengine
    if not (100 <= volc.asr_packet_ms <= 500):
        raise ValueError("providers.volcengine.asr_packet_ms must be in the range 100..500")
    if volc.asr_packet_ms % 20:
        raise ValueError("providers.volcengine.asr_packet_ms must be a multiple of 20")
    if volc.request_timeout_seconds <= 0:
        raise ValueError("providers.volcengine.request_timeout_seconds must be positive")
    if volc.max_audio_seconds <= 0:
        raise ValueError("providers.volcengine.max_audio_seconds must be positive")
    if not (65_536 <= volc.max_message_bytes <= 16 * 1024 * 1024):
        raise ValueError("providers.volcengine.max_message_bytes is out of range")
    for name, endpoint in (
        ("asr_endpoint", volc.asr_endpoint), ("tts_endpoint", volc.tts_endpoint)
    ):
        if not endpoint.startswith(("ws://", "wss://")):
            raise ValueError(f"providers.volcengine.{name} must use ws:// or wss://")
    if settings.providers.tts == "volcengine" and not volc.tts_speaker.strip():
        raise ValueError("providers.volcengine.tts_speaker is required for Volcengine TTS")
