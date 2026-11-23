from pathlib import Path
from dataclasses import replace

import pytest

from binbot_server.config import FeishuConfig, Settings, load_settings, validate_settings


def test_example_configuration_is_valid() -> None:
    path = Path(__file__).parents[1] / "config.example.toml"
    settings = load_settings(path)
    assert settings.server.port == 8765
    assert settings.audio.uplink_sample_rate == 16_000
    assert settings.audio.downlink_sample_rate == 24_000
    assert settings.providers.asr == "mock"
    assert settings.storage.kind == "memory"


def test_sqlite_storage_configuration_is_valid(tmp_path: Path) -> None:
    defaults = Settings()
    settings = replace(
        defaults,
        storage=replace(
            defaults.storage,
            kind="sqlite",
            sqlite_path=str(tmp_path / "binbot.sqlite3"),
        ),
    )
    validate_settings(settings)


@pytest.mark.parametrize("interval", [0, 60, -1])
def test_calendar_polling_interval_allows_zero_but_not_negative(interval: float) -> None:
    defaults = Settings()
    settings = replace(defaults, feishu=replace(
        defaults.feishu, calendar=replace(defaults.feishu.calendar, sync_interval_seconds=interval),
    ))
    if interval < 0:
        with pytest.raises(ValueError, match="sync_interval_seconds"):
            validate_settings(settings)
    else:
        validate_settings(settings)


def test_device_tokens_can_be_injected_from_environment(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("BINBOT_DEVICE_TOKENS_JSON", '{"binbot-001":"device-secret"}')
    settings = load_settings(Path(__file__).parents[1] / "config.example.toml")
    assert settings.auth.tokens == {"binbot-001": "device-secret"}


def test_invalid_device_tokens_environment_is_rejected(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("BINBOT_DEVICE_TOKENS_JSON", "not-json")
    with pytest.raises(ValueError, match="BINBOT_DEVICE_TOKENS_JSON"):
        load_settings(Path(__file__).parents[1] / "config.example.toml")


def test_feishu_credentials_file_reads_two_lines(tmp_path: Path) -> None:
    path = tmp_path / "feishu.txt"
    path.write_text("cli_test\nsecret_test\n")
    assert FeishuConfig(credentials_file=str(path)).credentials() == (
        "cli_test", "secret_test"
    )


def test_volcengine_tts_requires_explicit_speaker() -> None:
    defaults = Settings()
    settings = replace(
        defaults,
        providers=replace(defaults.providers, tts="volcengine"),
    )
    with pytest.raises(ValueError, match="tts_speaker"):
        validate_settings(settings)


def test_volcengine_is_not_accepted_as_llm_kind() -> None:
    defaults = Settings()
    settings = replace(
        defaults,
        providers=replace(defaults.providers, llm="volcengine"),
    )
    with pytest.raises(ValueError, match="no LLM adapter"):
        validate_settings(settings)


def test_unknown_llm_api_style_is_rejected() -> None:
    defaults = Settings()
    settings = replace(
        defaults,
        providers=replace(
            defaults.providers,
            openai=replace(defaults.providers.openai, llm_api="unknown"),
        ),
    )
    with pytest.raises(ValueError, match="llm_api"):
        validate_settings(settings)


@pytest.mark.parametrize("changes", [
    {"threshold": 0.2, "threshold_low": 0.3},
    {"threshold": float("nan")},
    {"end_silence_ms": 0},
    {"transcript_grace_ms": 0},
    {"transcript_grace_ms": 5001},
    {"start_timeout_ms": 0},
])
def test_invalid_vad_configuration_is_rejected(changes: dict[str, object]) -> None:
    defaults = Settings()
    with pytest.raises(ValueError, match="vad"):
        validate_settings(replace(defaults, vad=replace(defaults.vad, **changes)))
