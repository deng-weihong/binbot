"""MQTT control v2 is independent of BBVP voice v1."""

from typing import Annotated, Any, Literal
from uuid import UUID

from pydantic import BaseModel, ConfigDict, Field, TypeAdapter, model_validator
from binbot_server.application.device_commands import WORKSPACE_COMMANDS, validate_device_payload

CONTROL_COMMANDS = {
    "screen.brightness.set": "brightness_v1",
    "audio.volume.set": "volume_v1",
}
CONTROL_COMMANDS.update({name: "workspace_v1" for name in WORKSPACE_COMMANDS})


class BrightnessPayload(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    brightness: int = Field(ge=1, le=100)


class VolumePayload(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    volume: int = Field(ge=0, le=100)


class CommandEnvelope(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    type: Literal["device.command"]
    schema_version: Literal[2]
    boot_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    command_id: str = Field(
        pattern=r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
    )
    issued_at: int = Field(ge=946684800, le=4102444800)
    expires_at: int = Field(ge=946684800, le=4102444800)

    @model_validator(mode="after")
    def validate_lifetime(self) -> "CommandEnvelope":
        if not 0 < self.expires_at - self.issued_at <= 60:
            raise ValueError("command lifetime must be 1..60 seconds")
        return self


class BrightnessCommand(CommandEnvelope):
    name: Literal["screen.brightness.set"]
    payload: BrightnessPayload


class VolumeCommand(CommandEnvelope):
    name: Literal["audio.volume.set"]
    payload: VolumePayload


class WorkspaceCommand(CommandEnvelope):
    name: Literal[
        "watchface.set",
        "gestures.set",
        "microphone.set",
        "preferences.set",
        "noise.set",
        "focus.set",
        "todo.replace",
        "module.motor.set",
        "module.light.set",
        "module.pen.set",
        "module.rotation.set",
        "module.display.set",
        "learning.deck.set",
        "learning.events.ack",
        "calendar.set",
        "camera.publish",
        "device.wake",
        "music.play",
        "music.pause",
        "music.stop",
        "music.previous",
        "music.next",
        "music.seek",
        "music.select",
        "todo.add",
        "todo.edit",
        "todo.set_done",
        "todo.delete",
        "display.gif",
        "modules.image",
    ]
    payload: dict[str, Any]

    @model_validator(mode="after")
    def validate_payload(self) -> "WorkspaceCommand":
        self.payload = validate_device_payload(self.name, self.payload)
        return self


ControlCommand = Annotated[
    BrightnessCommand | VolumeCommand | WorkspaceCommand, Field(discriminator="name")
]
# Pydantic accepts annotated unions; its constructor typing expects a single class.
CONTROL_COMMAND: TypeAdapter[BrightnessCommand | VolumeCommand | WorkspaceCommand]
CONTROL_COMMAND = TypeAdapter(ControlCommand)  # type: ignore[arg-type]


class ControlState(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    type: Literal["device.control.state"]
    schema_version: Literal[2]
    boot_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    revision: int = Field(ge=0, le=4294967295)
    ready: bool
    brightness: Annotated[int | None, Field(ge=1, le=100)] = None
    volume: Annotated[int | None, Field(ge=0, le=100)] = None
    capabilities: list[
        Literal["control_v2", "brightness_v1", "volume_v1", "workspace_v1", "camera_publish_v2"]
    ] = Field(max_length=5)
    features: dict[str, Any] | None = None
    system: dict[str, Any] | None = None
    learning_events: list[dict[str, Any]] = Field(default_factory=list, max_length=32)


class ControlResult(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    type: Literal["device.control.result"]
    schema_version: Literal[2]
    boot_id: str = Field(pattern=r"^[0-9a-f]{32}$")
    command_id: str = Field(min_length=36, max_length=36)
    success: bool
    message: str = Field(max_length=256)

    def identifier(self) -> UUID:
        return UUID(self.command_id)
