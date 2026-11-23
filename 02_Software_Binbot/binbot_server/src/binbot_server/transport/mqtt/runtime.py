from __future__ import annotations

import asyncio
import hmac
import json
import logging
import re
import time
from collections import deque
from contextlib import suppress
from dataclasses import dataclass
from typing import Any
from uuid import UUID

from amqtt.broker import Action, Broker
from amqtt.plugins.authentication import BaseAuthPlugin
from amqtt.plugins.topic_checking import BaseTopicPlugin
from amqtt.session import Session
from pydantic import ValidationError

from binbot_server.application.device_control import DeviceControlStore
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import Settings
from binbot_server.domain.errors import StateConflict
from binbot_server.transport.control_protocol import (
    CONTROL_COMMAND,
    CONTROL_COMMANDS,
    ControlResult,
    ControlState,
)
from binbot_server.transport.protocol import DeviceCommandResultMessage

LOG = logging.getLogger("binbot.mqtt")
DEVICE_ID = re.compile(r"^[A-Za-z0-9._:-]{1,64}$")
PREFIX = "binbot/v2/devices/"


class ControlBroker(Broker):  # type: ignore[misc]
    """Always close TCP transports, including peers that exit during a write.

    aMQTT's stream adapter drains before closing. A reset during drain can leave
    that transport open and block asyncio.Server.wait_closed() on Python 3.12+.
    """

    def __init__(self, config: dict[str, Any]):
        super().__init__(config)
        self._writers: set[asyncio.StreamWriter] = set()

    async def stream_connected(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, listener_name: str
    ) -> None:
        self._writers.add(writer)
        try:
            await super().stream_connected(reader, writer, listener_name)
        finally:
            writer.close()
            with suppress(OSError, asyncio.TimeoutError):
                await asyncio.wait_for(writer.wait_closed(), 2)
            self._writers.discard(writer)

    async def shutdown(self) -> None:
        for writer in tuple(self._writers):
            writer.close()
        await super().shutdown()


# aMQTT does not publish a py.typed marker for its plugin base classes.
class DeviceControlPlugin(BaseAuthPlugin, BaseTopicPlugin):  # type: ignore[misc]
    @dataclass
    class Config:
        runtime: Any = None

    async def authenticate(self, *, session: Session) -> bool:
        device = session.username or ""
        expected = self.config.runtime.settings.auth.tokens.get(device, "")
        return bool(
            expected
            and DEVICE_ID.fullmatch(device)
            and device not in {".", ".."}
            and session.client_id
            and session.client_id.startswith(f"bb-{device}:")
            and hmac.compare_digest(expected.encode(), (session.password or "").encode())
        )

    async def topic_filtering(
        self,
        *,
        session: Session | None = None,
        topic: str | None = None,
        action: Action | None = None,
    ) -> bool:
        if session is None or topic is None or not session.username:
            return False
        prefix = f"{PREFIX}{session.username}/"
        if not topic.startswith(prefix):
            return False
        suffix = topic[len(prefix) :]
        allowed = (
            {"availability", "state", "result", "event"}
            if action == Action.PUBLISH
            else {"command", "sync"}
        )
        return suffix in allowed

    async def on_broker_client_connected(self, *, client_id: str, client_session: Session) -> None:
        await self.config.runtime.connected(client_id, client_session.username, self.context)

    async def on_broker_client_disconnected(
        self, *, client_id: str, client_session: Session
    ) -> None:
        await self.config.runtime.disconnected(client_id, client_session.username)

    async def on_broker_message_broadcast(self, *, client_id: str, message: Any) -> None:
        # This hook runs only after aMQTT has approved the publisher's topic ACL.
        await self.config.runtime.received(client_id, message)


class MQTTControlRuntime:
    def __init__(
        self, settings: Settings, registry: DeviceSessionRegistry, store: DeviceControlStore
    ):
        self.settings, self.registry, self.store = settings, registry, store
        self.broker: Broker | None = None
        self._context: Any = None
        self._clients: dict[str, tuple[str, float]] = {}
        self._boot: dict[str, str] = {}
        self._old_boots: dict[str, deque[str]] = {}
        self._pending: dict[UUID, tuple[str, str, float]] = {}
        self._task: asyncio.Task[None] | None = None
        self._lock = asyncio.Lock()
        self.on_state: Any = None

    async def start(self) -> None:
        # An interrupted command is uncertain; never replay it after a server restart.
        await self.store.fail_unconfirmed_control_commands()
        config = self.settings.mqtt
        listener: dict[str, Any] = {
            "type": "tcp",
            "bind": f"{config.host}:{config.port}",
            "max_connections": config.max_connections,
        }
        if config.tls_cert_file:
            listener.update(ssl=True, certfile=config.tls_cert_file, keyfile=config.tls_key_file)
        # The web console uses HTTP; a device MQTT WebSocket listener is unnecessary.
        self.broker = ControlBroker(
            {
                "listeners": {"default": listener},
                "plugins": {f"{__name__}.DeviceControlPlugin": {"runtime": self}},
            }
        )
        try:
            await self.broker.start()
            self._task = asyncio.create_task(self._watch(), name="device-control-timeouts")
        except BaseException:
            await self.broker.shutdown()
            raise

    async def connected(self, client: str, device: str, context: Any) -> None:
        async with self._lock:
            self._context = context
            previous = self._clients.get(device)
            if previous is not None:
                await self._fail_pending(
                    device, previous[0], "设备控制连接已被替换，执行结果未确认"
                )
            self._clients[device] = (client, time.monotonic())
            self._boot.pop(device, None)
            self._old_boots.pop(device, None)

            async def send(event: dict[str, object]) -> None:
                await self.send(device, client, event)

            await self.registry.register_control(device, client, send)

    async def disconnected(self, client: str, device: str) -> None:
        async with self._lock:
            current = self._clients.get(device)
            if current is not None and current[0] == client:
                del self._clients[device]
                await self.registry.unregister_control(device, client)
                await self._fail_pending(device, client, "设备控制连接已断开，执行结果未确认")

    async def received(self, client: str, message: Any) -> None:
        if len(message.data) > 16384:
            return
        topic = message.topic
        if not topic.startswith(PREFIX):
            return
        device, _, suffix = topic[len(PREFIX) :].partition("/")
        async with self._lock:
            current = self._clients.get(device)
            if current is None or current[0] != client:
                return
            # Replayed retained state is never evidence that an App is executing now.
            if message.publish_packet and message.publish_packet.retain_flag:
                return
            try:
                if suffix == "state":
                    state = ControlState.model_validate_json(bytes(message.data))
                    if "control_v2" not in state.capabilities:
                        return
                    old_boot = self._boot.get(device)
                    if state.boot_id in self._old_boots.get(device, ()):
                        return
                    saved = await self.store.state(device)
                    previous = saved.get("control") if saved else None
                    if (
                        isinstance(previous, dict)
                        and previous.get("boot_id") == state.boot_id
                        and previous.get("revision", -1) > state.revision
                    ):
                        return
                    if old_boot and old_boot != state.boot_id:
                        self._old_boots.setdefault(device, deque(maxlen=32)).append(old_boot)
                        await self._fail_pending(device, client, "设备 App 已重启，执行结果未确认")
                    self._boot[device] = state.boot_id
                    self._clients[device] = (client, time.monotonic())
                    seen = await self.registry.control_seen(
                        device,
                        client,
                        ready=state.ready,
                        boot_id=state.boot_id,
                        capabilities=state.capabilities,
                    )
                    if not seen:

                        async def send(event: dict[str, object]) -> None:
                            await self.send(device, client, event)

                        await self.registry.register_control(device, client, send)
                        await self.registry.control_seen(
                            device,
                            client,
                            ready=state.ready,
                            boot_id=state.boot_id,
                            capabilities=state.capabilities,
                        )
                    await self.store.update_control_state(
                        device, state.model_dump(exclude={"type"})
                    )
                    if self.on_state is not None:
                        self.on_state(device, state.model_dump(exclude={"type"}))
                    row = next(
                        (row for row in await self.registry.snapshot() if row["id"] == device), None
                    )
                    if row is not None:
                        await self.store.remember_device(row)
                elif suffix == "result":
                    result = ControlResult.model_validate_json(bytes(message.data))
                    identifier = result.identifier()
                    pending = self._pending.get(identifier)
                    if (
                        pending is None
                        or pending[:2] != (device, client)
                        or result.boot_id != self._boot.get(device)
                    ):
                        return
                    self._pending.pop(identifier)
                    await self.store.complete(
                        device,
                        DeviceCommandResultMessage(
                            type="device.command.result",
                            command_id=identifier,
                            success=result.success,
                            message=result.message,
                        ),
                    )
            except (ValidationError, ValueError, TypeError, UnicodeError):
                LOG.warning("Rejected malformed control message device=%s topic=%s", device, suffix)

    async def send(self, device: str, client: str, event: dict[str, object]) -> None:
        async with self._lock:
            current = self._clients.get(device)
            if current is None or current[0] != client or device not in self._boot:
                raise StateConflict("device control is unavailable")
            if time.monotonic() - current[1] > self.settings.mqtt.heartbeat_timeout_seconds:
                raise StateConflict("device control state is stale")
            if event.get("name") not in CONTROL_COMMANDS or self._context is None:
                raise StateConflict("unsupported MQTT command")
            identifier = UUID(str(event["command_id"]))
            timeout = self.settings.mqtt.command_timeout_seconds
            if event.get("name") in {"display.gif", "modules.image", "module.display.set"}:
                timeout = max(timeout, 55)
            issued = int(time.time())
            command = CONTROL_COMMAND.validate_python(
                {
                    **event,
                    "schema_version": 2,
                    "boot_id": self._boot[device],
                    "issued_at": issued,
                    "expires_at": issued + timeout,
                }
            ).model_dump()
            self._pending[identifier] = (device, client, time.monotonic() + timeout)
            try:
                await self._context.broadcast_message(
                    f"{PREFIX}{device}/command", json.dumps(command).encode(), qos=1
                )
            except Exception as exc:
                self._pending.pop(identifier, None)
                raise StateConflict("unable to publish control command") from exc

    async def _fail_pending(self, device: str, client: str, message: str) -> None:
        for identifier, pending in list(self._pending.items()):
            if pending[:2] == (device, client):
                self._pending.pop(identifier, None)
                await self.store.mark_failed(identifier, message)

    async def _watch(self) -> None:
        while True:
            await asyncio.sleep(1)
            async with self._lock:
                now = time.monotonic()
                for identifier, (_, _, expires) in list(self._pending.items()):
                    if now >= expires:
                        self._pending.pop(identifier, None)
                        await self.store.mark_failed(identifier, "设备执行超时，结果未确认")
                for device, (client, seen) in list(self._clients.items()):
                    if now - seen > self.settings.mqtt.heartbeat_timeout_seconds:
                        await self.registry.unregister_control(device, client)
                        await self._fail_pending(device, client, "设备状态上报超时")

    async def close(self) -> None:
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        for device, (client, _) in list(self._clients.items()):
            await self.registry.unregister_control(device, client)
            await self._fail_pending(device, client, "控制服务正在停止，执行结果未确认")
        self._clients.clear()
        if self.broker is not None:
            await self.broker.shutdown()
