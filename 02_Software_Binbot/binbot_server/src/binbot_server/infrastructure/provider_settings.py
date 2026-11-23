from __future__ import annotations

from dataclasses import replace
import json
import os
from pathlib import Path
import tempfile
from typing import Any
from urllib.parse import urlsplit
from uuid import uuid4

from binbot_server.config import Settings, validate_settings


OPENAI_FIELDS = ("base_url", "asr_model", "llm_model", "llm_api", "llm_thinking", "tts_model", "tts_voice")
VOLC_FIELDS = ("asr_endpoint", "tts_endpoint", "asr_resource_id", "tts_resource_id", "tts_model", "tts_speaker")
SEEDUPLEX_FIELDS = ("endpoint", "model", "voice")


def settings_path(settings: Settings) -> Path:
    directory = settings.providers.settings_directory or settings.component.directory
    return Path(directory) / "provider-settings.json"


def secret_names(settings: Settings) -> dict[str, str]:
    providers = settings.providers
    return {
        "openai": providers.openai.api_key_env,
        "asr": providers.volcengine.asr_api_key_env or providers.volcengine.api_key_env,
        "tts": providers.volcengine.tts_api_key_env or providers.volcengine.api_key_env,
        "seeduplex": providers.seeduplex.api_key_env,
    }


def provider_values(settings: Settings) -> dict[str, Any]:
    providers = settings.providers
    return {
        "asr": providers.asr, "llm": providers.llm, "tts": providers.tts,
        "voice": providers.voice,
        "openai": {key: getattr(providers.openai, key) for key in OPENAI_FIELDS},
        "volcengine": {key: getattr(providers.volcengine, key) for key in VOLC_FIELDS},
        "seeduplex": {key: getattr(providers.seeduplex, key) for key in SEEDUPLEX_FIELDS},
    }


def validate_values(settings: Settings, values: dict[str, Any]) -> Settings:
    required = {"asr", "llm", "tts", "openai", "volcengine"}
    if (not isinstance(values, dict) or not required <= set(values)
            or set(values) - required - {"voice", "seeduplex"}):
        raise ValueError("invalid provider configuration fields")
    # Saved configurations and already-open Console tabs may predate realtime voice.
    values = {**provider_values(settings), **values}
    for kind, allowed in (("asr", {"mock", "openai", "volcengine"}),
                          ("llm", {"mock", "openai"}), ("tts", {"mock", "openai", "volcengine"})):
        if not isinstance(values[kind], str) or values[kind] not in allowed:
            raise ValueError(f"unsupported {kind} provider")
    for section, fields in (("openai", OPENAI_FIELDS), ("volcengine", VOLC_FIELDS),
                            ("seeduplex", SEEDUPLEX_FIELDS)):
        group = values[section]
        if not isinstance(group, dict) or set(group) != set(fields):
            raise ValueError(f"invalid {section} fields")
        optional = {"llm_thinking", "tts_speaker"}
        if any(not isinstance(value, str) or (key not in optional and not value.strip()) or len(value) > 2048
               or any(ord(char) < 32 for char in value) for key, value in group.items()):
            raise ValueError(f"{section} fields must be non-empty strings")
    for group, key, schemes in (("openai", "base_url", {"https", "http"}),
                                ("volcengine", "asr_endpoint", {"wss", "ws"}),
                                ("volcengine", "tts_endpoint", {"wss", "ws"}),
                                ("seeduplex", "endpoint", {"wss", "ws"})):
        url = urlsplit(values[group][key])
        if url.scheme not in schemes or not url.netloc or url.username or url.password or url.fragment:
            raise ValueError(f"invalid {group}.{key} URL")
    selected = replace(settings.providers,
                       asr=values["asr"], llm=values["llm"], tts=values["tts"], voice=values["voice"],
                       openai=replace(settings.providers.openai, **values["openai"]),
                       volcengine=replace(settings.providers.volcengine, **values["volcengine"]),
                       seeduplex=replace(settings.providers.seeduplex, **values["seeduplex"]))
    updated = replace(settings, providers=selected)
    validate_settings(updated)
    return updated


def read_document(settings: Settings) -> dict[str, Any]:
    path = settings_path(settings)
    if not path.exists():
        return {"revision": "initial", "config": provider_values(settings), "secrets": {}}
    document = json.loads(path.read_text())
    if (not isinstance(document, dict) or set(document) != {"revision", "config", "secrets"}
            or not isinstance(document["revision"], str)):
        raise ValueError("invalid saved provider settings")
    validate_secrets(document["secrets"])
    return document


def validate_secrets(values: Any) -> None:
    if not isinstance(values, dict) or set(values) - {"openai", "asr", "tts", "seeduplex"}:
        raise ValueError("invalid provider secret fields")
    if any(not isinstance(value, str) or not value or len(value) > 4096
           or any(ord(char) < 32 for char in value) for value in values.values()):
        raise ValueError("provider secrets must be non-empty strings")


def apply_secrets(settings: Settings, values: dict[str, str]) -> None:
    names = secret_names(settings)
    assignments: dict[str, str] = {}
    for key, value in values.items():
        name = names[key]
        if name in assignments and assignments[name] != value:
            raise ValueError("providers sharing one credential must use the same API key")
        assignments[name] = value
    os.environ.update(assignments)


def load_provider_overrides(settings: Settings) -> Settings:
    document = read_document(settings)
    if document["revision"] == "initial":
        return settings
    updated = validate_values(settings, document["config"])
    apply_secrets(updated, document["secrets"])
    return updated


def snapshot(settings: Settings) -> dict[str, Any]:
    return {"revision": read_document(settings)["revision"], "config": provider_values(settings),
            "credentials_configured": {key: bool(os.environ.get(name))
                                       for key, name in secret_names(settings).items()}}


def persist(settings: Settings, config: dict[str, Any], secrets: dict[str, str]) -> None:
    path = settings_path(settings)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    document = {"revision": str(uuid4()), "config": config, "secrets": secrets}
    descriptor, temporary = tempfile.mkstemp(prefix=".providers-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(document, stream, ensure_ascii=True)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
