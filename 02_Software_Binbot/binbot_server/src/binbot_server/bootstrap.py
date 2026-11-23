from __future__ import annotations

from dataclasses import dataclass
import logging
from typing import cast
from typing import Any

from aiohttp import web

from binbot_server.application.ports import (
    ASRProvider,
    AgentModel,
    ConversationStore,
    LLMProvider,
    MemoryStore,
    ProviderBundle,
    TTSProvider,
    TaskItem,
    TaskStore,
)
from binbot_server.application.agent_runtime import AgentRuntime
from binbot_server.application.reports import (
    ActivityJournal, DailyReportService, DailyReportWorker, JournalConversationStore, ReportStore,
)
from binbot_server.application.calendar import CalendarSyncService
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.application.component_ota import ComponentOTAService, register_component_queries
from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.feishu import FeishuGateway
from binbot_server.application.tasks import TaskScheduler, TaskService, TaskWorker
from binbot_server.application.web import WebAgentGateway
from binbot_server.application.workspace import WorkspaceService
from binbot_server.application.workspace_tools import register_workspace_tools
from binbot_server.application.tools import (
    DeviceCommandService,
    MemoryService,
    ToolRegistry,
    ToolContext,
    ToolSpec,
    register_device_tools,
)
from binbot_server.config import Settings
from binbot_server.infrastructure.device_feishu import DeviceFeishuManager
from binbot_server.infrastructure.providers import (
    MockASRProvider,
    MockLLMProvider,
    MockTTSProvider,
    OpenAIASRProvider,
    OpenAILLMProvider,
    OpenAITTSProvider,
    VolcengineASRProvider,
    VolcengineTTSProvider,
)
from binbot_server.infrastructure.providers.seeduplex import SeeduplexProvider
from binbot_server.infrastructure.conversation import InMemoryConversationStore
from binbot_server.infrastructure.memory import InMemoryMemoryStore
from binbot_server.infrastructure.feishu import (
    FeishuLongConnection,
    FeishuOAuthManager,
)
from binbot_server.infrastructure.sqlite import (
    SqliteConversationStore,
    SqliteDeviceControlStore,
    SqliteMemoryStore,
)
from binbot_server.infrastructure.tasks import InMemoryTaskStore, SqliteTaskStore
from binbot_server.infrastructure.reports import InMemoryReportStore, SqliteReportStore
from binbot_server.infrastructure.workspace import SqliteWorkspaceStore
from binbot_server.infrastructure.security import DeviceAuthenticator
from binbot_server.infrastructure.vad import SileroVAD
from binbot_server.transport.http import GatewayContext, create_http_app
from binbot_server.transport.mqtt import MQTTControlRuntime


_LOG = logging.getLogger("binbot.bootstrap")


@dataclass(frozen=True, slots=True)
class Application:
    http: web.Application
    providers: ProviderBundle
    registry: DeviceSessionRegistry
    conversations: ConversationStore
    controls: DeviceControlStore
    tools: ToolRegistry
    agent: AgentRuntime
    web_agent: WebAgentGateway
    memories: MemoryStore
    tasks: TaskStore
    scheduler: TaskScheduler
    feishu: FeishuGateway | None
    feishu_connection: FeishuLongConnection | None
    calendar: CalendarSyncService | None
    worker: TaskWorker
    reports: DailyReportService
    report_worker: DailyReportWorker


def build_providers(settings: Settings) -> ProviderBundle:
    selected = settings.providers
    openai = selected.openai
    volcengine = selected.volcengine
    api_key: str | None = None
    volc_asr_api_key: str | None = None
    volc_tts_api_key: str | None = None

    def get_api_key() -> str:
        nonlocal api_key
        if api_key is None:
            api_key = openai.api_key()
        return api_key

    def get_volc_asr_api_key() -> str:
        nonlocal volc_asr_api_key
        if volc_asr_api_key is None:
            volc_asr_api_key = volcengine.asr_api_key()
        return volc_asr_api_key

    def get_volc_tts_api_key() -> str:
        nonlocal volc_tts_api_key
        if volc_tts_api_key is None:
            volc_tts_api_key = volcengine.tts_api_key()
        return volc_tts_api_key

    asr: ASRProvider
    if selected.asr == "mock":
        asr = MockASRProvider(selected.mock.transcript)
    elif selected.asr == "openai":
        asr = OpenAIASRProvider(
            openai.base_url,
            get_api_key(),
            openai.asr_model,
            openai.request_timeout_seconds,
        )
    else:
        asr = VolcengineASRProvider(
            volcengine.asr_endpoint,
            get_volc_asr_api_key(),
            volcengine.asr_resource_id,
            volcengine.asr_packet_ms,
            volcengine.request_timeout_seconds,
            volcengine.max_audio_seconds,
            volcengine.max_message_bytes,
        )

    llm: LLMProvider
    if selected.llm == "mock":
        llm = MockLLMProvider(selected.mock.response)
    elif selected.llm == "openai":
        llm = OpenAILLMProvider(
            openai.base_url,
            get_api_key(),
            openai.llm_model,
            openai.request_timeout_seconds,
            settings.conversation.system_prompt,
            openai.llm_api,
            openai.llm_thinking,
        )
    else:
        raise ValueError(
            "Volcengine does not provide an LLM adapter; select mock or openai for llm"
        )

    tts: TTSProvider
    if selected.tts == "mock":
        tts = MockTTSProvider()
    elif selected.tts == "openai":
        tts = OpenAITTSProvider(
            openai.base_url,
            get_api_key(),
            openai.tts_model,
            openai.tts_voice,
            openai.request_timeout_seconds,
        )
    else:
        tts = VolcengineTTSProvider(
            volcengine.tts_endpoint,
            get_volc_tts_api_key(),
            volcengine.tts_resource_id,
            volcengine.tts_model,
            volcengine.tts_speaker,
            volcengine.request_timeout_seconds,
            volcengine.max_message_bytes,
        )
    realtime = (
        SeeduplexProvider(selected.seeduplex, selected.seeduplex.api_key(),
                          settings.conversation.system_prompt)
        if selected.voice == "seeduplex" else None
    )
    return ProviderBundle(asr=asr, llm=llm, tts=tts, realtime=realtime)


def create_application(
    settings: Settings,
    providers: ProviderBundle | None = None,
) -> Application:
    vad = SileroVAD(settings.vad) if settings.vad.enabled else None
    provider_bundle = providers or build_providers(settings)
    registry = DeviceSessionRegistry()
    tasks: TaskStore
    report_store: ReportStore
    if settings.storage.kind == "sqlite":
        conversations: ConversationStore = SqliteConversationStore(
            settings.storage.sqlite_path, settings.conversation.max_history_messages
        )
        controls: DeviceControlStore = SqliteDeviceControlStore(
            settings.storage.sqlite_path, settings.storage.max_commands
        )
        memories: MemoryStore = SqliteMemoryStore(settings.storage.sqlite_path)
        tasks = SqliteTaskStore(settings.storage.sqlite_path, settings.storage.max_tasks)
        report_store = SqliteReportStore(
            settings.storage.sqlite_path, settings.daily_report.retention_days
        )
    else:
        conversations = InMemoryConversationStore(
            settings.conversation.max_history_messages
        )
        controls = DeviceControlStore(settings.storage.max_commands)
        memories = InMemoryMemoryStore()
        tasks = InMemoryTaskStore(settings.storage.max_tasks)
        report_store = InMemoryReportStore(settings.daily_report.retention_days)
    journal = ActivityJournal(report_store)
    conversations = JournalConversationStore(conversations, journal)
    reports = DailyReportService(
        report_store, provider_bundle.llm if settings.providers.llm != "mock" else None,
        timezone=settings.daily_report.timezone,
        generate_at=settings.daily_report.generate_at,
        timeout_seconds=settings.pipeline.llm_timeout_seconds,
        max_context_chars=settings.daily_report.max_context_chars,
        retention_days=settings.daily_report.retention_days,
    )
    report_worker = DailyReportWorker(reports, settings.daily_report.enabled)
    command_service = DeviceCommandService(registry, controls)
    task_service = TaskService(tasks)
    tools = ToolRegistry(journal)
    register_device_tools(tools, command_service, MemoryService(memories), task_service)
    component_ota = ComponentOTAService(
        settings.component, registry, settings.server.asset_directory,
        settings.server.ffmpeg_path,
        command_service=command_service,
    )
    register_component_queries(tools, component_ota)

    async def generate_daily_report(_: ToolContext, arguments: dict[str, Any]) -> dict[str, Any]:
        day = arguments.get("date", reports.today())
        if not isinstance(day, str):
            raise ValueError("date must use YYYY-MM-DD")
        return await reports.generate(day)

    tools.register(ToolSpec(
        "report.daily", "根据实际活动生成工作日报，日期默认今天，不依赖设备在线。",
        {"type": "object", "properties": {"date": {"type": "string"}},
         "additionalProperties": False}, "report.write", generate_daily_report,
        timeout_seconds=settings.pipeline.llm_timeout_seconds + 10,
    ))
    scheduler = TaskScheduler(tasks, settings.scheduler.batch_size)
    feishu: FeishuGateway | None = None
    feishu_oauth: FeishuOAuthManager | None = None
    feishu_connection: FeishuLongConnection | None = None
    calendar: CalendarSyncService | None = None
    agent_model = (
        cast(AgentModel, provider_bundle.llm)
        if settings.agent.enabled and hasattr(provider_bundle.llm, "complete")
        else None
    )
    agent = AgentRuntime(
        conversations,
        tools,
        max(1, settings.conversation.max_history_messages),
        settings.agent.max_context_chars,
        settings.agent.require_control_approval,
        agent_model,
        settings.agent.max_tool_rounds,
        memories,
        settings.agent.max_memory_items,
        journal,
    )
    workspace_store = SqliteWorkspaceStore(settings.storage.sqlite_path if settings.storage.kind == "sqlite" else ":memory:")
    workspace = WorkspaceService(workspace_store, controls, command_service, registry, settings.daily_report.timezone)
    register_workspace_tools(tools, workspace)
    agent.workspace = workspace
    web_agent = WebAgentGateway(agent, conversations, workspace_store)
    device_feishu = DeviceFeishuManager(settings, agent, conversations, registry)

    async def run_task(item: TaskItem) -> None:
        payload = item.payload
        if item.name == "device.command":
            command_name = payload.get("name")
            command_payload = payload.get("payload", {})
            if not isinstance(command_name, str) or not isinstance(command_payload, dict):
                raise ValueError("device.command requires name and object payload")
            await command_service.send(
                item.device_id, command_name, dict(command_payload), str(item.task_id)
            )
            return
        if item.name == "feishu.message":
            runtime = device_feishu.runtime(item.device_id)
            receive_id = payload.get("receive_id")
            text = payload.get("text")
            if (
                not isinstance(receive_id, str)
                or not receive_id
                or not isinstance(text, str)
                or not text
            ):
                raise ValueError("feishu.message requires receive_id and text")
            if not runtime.document["config"]["messages_enabled"] or receive_id != runtime.document["config"]["chat_id"]:
                raise ValueError("Feishu message must use this device's bound chat")
            await runtime.gateway.send_text(receive_id, text)
            return
        raise ValueError(f"unsupported scheduled task: {item.name}")

    async def execute_task(item: TaskItem) -> None:
        try:
            await run_task(item)
        except Exception as exc:
            await journal.record(
                "task", "scheduler", item.device_id, item.name, "failed",
                {"task_id": str(item.task_id), "payload": item.payload, "error": str(exc)},
            )
            raise
        status = "submitted" if item.name == "device.command" else "completed"
        command_id = ""
        if item.name == "device.command":
            commands = await controls.recent_commands(item.device_id, settings.storage.max_commands)
            command_id = next((str(command["command_id"]) for command in commands
                               if command.get("idempotency_key") == str(item.task_id)), "")
        await journal.record(
            "task", "scheduler", item.device_id, item.name, status,
            {"task_id": str(item.task_id), "payload": item.payload}, command_id,
        )

    worker = TaskWorker(
        scheduler, execute_task, settings.scheduler.poll_interval_seconds
    )
    context = GatewayContext(
        settings=settings,
        providers=provider_bundle,
        registry=registry,
        authenticator=DeviceAuthenticator(
            settings.auth.allow_anonymous,
            settings.auth.tokens,
        ),
        conversations=conversations,
        controls=controls,
        command_service=command_service,
        tools=tools,
        agent=agent,
        web_agent=web_agent,
        memories=memories,
        tasks=tasks,
        feishu=feishu,
        feishu_oauth=feishu_oauth,
        feishu_connection=feishu_connection,
        calendar=calendar,
        worker=worker,
        reports=reports,
        report_worker=report_worker,
        journal=journal,
        component_ota=component_ota,
        device_feishu=device_feishu,
        workspace_store=workspace_store,
        workspace=workspace,
        mqtt=MQTTControlRuntime(settings, registry, controls) if settings.mqtt.enabled else None,
        configure_providers=build_providers,
        endpoint_factory=vad.new_stream if vad is not None else None,
    )
    if context.mqtt is not None:
        context.mqtt.on_state = workspace.accept_report
    workspace.calendar_provider = device_feishu.calendar
    workspace.calendar_status_provider = device_feishu.calendar_status
    workspace.calendar_scope_provider = device_feishu.calendar_scope
    http = create_http_app(context)
    return Application(
        http=http,
        providers=provider_bundle,
        registry=registry,
        conversations=conversations,
        controls=controls,
        tools=tools,
        agent=agent,
        web_agent=web_agent,
        memories=memories,
        tasks=tasks,
        scheduler=scheduler,
        feishu=feishu,
        feishu_connection=feishu_connection,
        calendar=calendar,
        worker=worker,
        reports=reports,
        report_worker=report_worker,
    )
