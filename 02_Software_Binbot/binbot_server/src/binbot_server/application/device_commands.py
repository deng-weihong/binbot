"""Validated device operations shared by HTTP, MQTT and Agent tools."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

WATCH_FACES = {
    row["id"]
    for row in json.loads((Path(__file__).parents[1] / "catalogs/watchFaces.json").read_text())
}
GESTURES = ("ok", "thumb", "palm", "victory", "fist")
GESTURE_ACTIONS = (
    "pc-on",
    "pc-sleep",
    "lamp",
    "play",
    "next",
    "previous",
    "photo",
    "focus",
    "wake",
    "noise",
    "mute",
    "nothing",
)
WORKSPACE_COMMANDS = {
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
}


def _object(value: Any, required: set[str], optional: set[str] | None = None) -> dict[str, Any]:
    if (
        not isinstance(value, dict)
        or not required <= value.keys()
        or value.keys() - required - (optional or set())
    ):
        raise ValueError("操作参数不完整或包含未知字段")
    return dict(value)


def _integer(value: Any, low: int, high: int) -> int:
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"数值必须是 {low}–{high} 的整数")
    return value


def _boolean(value: Any) -> bool:
    if type(value) is not bool:
        raise ValueError("开关必须是布尔值")
    return value


def _text(value: Any, limit: int, *, empty: bool = False) -> str:
    if (
        not isinstance(value, str)
        or (not empty and not value.strip())
        or len(value.encode()) > limit
        or "\x00" in value
    ):
        raise ValueError("文字为空、过长或含有无效字符")
    return value.strip()


def validate_device_payload(name: str, payload: Any) -> dict[str, Any]:
    if name in ("screen.brightness.set", "audio.volume.set"):
        field = "brightness" if name == "screen.brightness.set" else "volume"
        p = _object(payload, {field})
        _integer(p[field], 1 if field == "brightness" else 0, 100)
        return p
    if name not in WORKSPACE_COMMANDS:
        raise ValueError("设备不支持该操作")
    if name in {
        "device.wake",
        "music.play",
        "music.pause",
        "music.stop",
        "music.previous",
        "music.next",
    }:
        return _object(payload, set())
    if name == "watchface.set":
        p = _object(payload, {"themeId", "standbyId", "idleSeconds", "shuffle"}, {"showNow"})
        if (
            not isinstance(p["themeId"], str)
            or not isinstance(p["standbyId"], str)
            or p["themeId"] not in WATCH_FACES
            or p["standbyId"] not in WATCH_FACES
        ):
            raise ValueError("请选择设备支持的表盘")
        _integer(p["idleSeconds"], 0, 3600)
        _boolean(p["shuffle"])
        if "showNow" in p:
            _boolean(p["showNow"])
    elif name == "gestures.set":
        p = _object(payload, {"enabled", "confirmSensitive", "sensitivity", "mappings"})
        _boolean(p["enabled"])
        _boolean(p["confirmSensitive"])
        if p["sensitivity"] not in ("low", "medium", "high"):
            raise ValueError("手势灵敏度无效")
        mappings = _object(p["mappings"], set(GESTURES))
        for gesture in GESTURES:
            binding = _object(mappings[gesture], {"enabled", "action"})
            _boolean(binding["enabled"])
            if binding["action"] not in GESTURE_ACTIONS:
                raise ValueError("手势操作无效")
    elif name == "microphone.set":
        p = _object(payload, {"enabled"})
        _boolean(p["enabled"])
    elif name == "preferences.set":
        from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

        p = _object(payload, {"notifications", "timezone"}, {"utcOffset"})
        _boolean(p["notifications"])
        try:
            from datetime import datetime, UTC

            zone = ZoneInfo(_text(p["timezone"], 80))
            offset = datetime.now(UTC).astimezone(zone).utcoffset()
            assert offset is not None
            p["utcOffset"] = int(offset.total_seconds())
        except (ZoneInfoNotFoundError, ValueError):
            raise ValueError("时区无效") from None
    elif name == "noise.set":
        p = _object(payload, {"playing", "volume", "timerMinutes", "levels"})
        _boolean(p["playing"])
        _integer(p["volume"], 0, 100)
        _integer(p["timerMinutes"], 0, 180)
        if not isinstance(p["levels"], list) or len(p["levels"]) != 9:
            raise ValueError("需要九路声音音量")
        for level in p["levels"]:
            _integer(level, 0, 100)
    elif name == "focus.set":
        p = _object(payload, {"action"}, {"minutes", "mode"})
        if p["action"] not in {"start", "pause", "reset", "configure"}:
            raise ValueError("专注操作无效")
        if "minutes" in p:
            _integer(p["minutes"], 5, 120)
        if "mode" in p and p["mode"] not in {"focus", "break"}:
            raise ValueError("专注模式无效")
    elif name == "todo.replace":
        p = _object(payload, {"items"})
        if not isinstance(p["items"], list) or len(p["items"]) > 8:
            raise ValueError("设备最多保存 8 条待办")
        ids = set()
        for item in p["items"]:
            row = _object(item, {"id", "text", "done"}, {"time"})
            item_id = _text(row["id"], 64)
            _text(row["text"], 71)
            _boolean(row["done"])
            if item_id in ids:
                raise ValueError("待办编号重复")
            ids.add(item_id)
            if (
                "time" in row
                and row["time"]
                and not __import__("re").fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", row["time"])
            ):
                raise ValueError("待办时间无效")
    elif name == "module.motor.set":
        p = _object(payload, {"direction", "speed"})
        if p["direction"] not in ("stop", "clockwise", "counterclockwise"):
            raise ValueError("转盘方向无效")
        _integer(p["speed"], 0, 100)
    elif name == "module.light.set":
        import re

        p = _object(payload, {"enabled", "effect", "color", "brightness", "speed"})
        _boolean(p["enabled"])
        _integer(p["brightness"], 0, 100)
        _integer(p["speed"], 0, 100)
        if (
            p["effect"] not in ("常亮", "呼吸", "流光", "闪烁", "心跳")
            or not isinstance(p["color"], str)
            or not re.fullmatch(r"#[0-9a-fA-F]{6}", p["color"])
        ):
            raise ValueError("灯光颜色或效果无效")
    elif name == "module.pen.set":
        p = _object(payload, {"mode", "face"})
        if p["mode"] not in ("image", "info") or p["face"] not in ("both", "time", "weather"):
            raise ValueError("笔筒显示模式无效")
    elif name == "module.rotation.set":
        p = _object(payload, {"profile", "seconds"})
        _integer(p["profile"], 1, 2)
        _integer(p["seconds"], 0, 86400)
    elif name == "module.display.set":
        p = _object(payload, {"profile", "image_id"})
        _integer(p["profile"], 1, 2)
        _text(p["image_id"], 36)
    elif name == "learning.deck.set":
        p = _object(payload, {"book_id", "deck_id", "cards"}, {"plan"})
        _text(p["book_id"], 64)
        _text(p["deck_id"], 64)
        if not isinstance(p["cards"], list) or not 0 <= len(p["cards"]) <= 30:
            raise ValueError("词卡批次最多为 30 词")
        for card in p["cards"]:
            row = _object(card, {"word", "phonetic", "meaning", "example", "translation"})
            for field, limit in (
                ("word", 320),
                ("phonetic", 480),
                ("meaning", 1200),
                ("example", 2000),
                ("translation", 2000),
            ):
                _text(row[field], limit, empty=field in {"phonetic", "example", "translation"})
        if "plan" in p:
            plan = _object(p["plan"], {"due", "new", "learned_today", "next_review_at", "new_mask"})
            for field in ("due", "new", "learned_today"):
                _integer(plan[field], 0, 25000)
            _integer(plan["next_review_at"], 0, 4102444800)
            _integer(plan["new_mask"], 0, (1 << len(p["cards"])) - 1)
        if len(json.dumps(p, ensure_ascii=False).encode()) > 14000:
            raise ValueError("词卡批次过大，请缩短例句或减少词数")
    elif name == "learning.events.ack":
        p = _object(payload, {"event_ids"})
        if not isinstance(p["event_ids"], list) or len(p["event_ids"]) > 32:
            raise ValueError("学习回执批次过大")
        for item in p["event_ids"]:
            _text(item, 96)
    elif name == "calendar.set":
        p = _object(payload, {"revision", "timezone_offset", "events"})
        _integer(p["revision"], 0, 4294967295)
        _integer(p["timezone_offset"], -1440, 1440)
        if not isinstance(p["events"], list) or len(p["events"]) > 8:
            raise ValueError("设备日历最多展示近期 8 条事件")
        for event in p["events"]:
            row = _object(event, {"id", "title", "start", "end"})
            _text(row["id"], 63)
            _text(row["title"], 95)
            _integer(row["start"], 946684800, 4102444800)
            _integer(row["end"], row["start"], 4102444800)
    elif name == "camera.publish":
        p = _object(payload, {"lease_seconds"})
        _integer(p["lease_seconds"], 0, 60)
    elif name == "music.seek":
        p = _object(payload, {"position_seconds"})
        _integer(p["position_seconds"], 0, 86400)
    elif name == "music.select":
        p = _object(payload, {"track_index"})
        _integer(p["track_index"], 0, 255)
    elif name.startswith("todo."):
        required = {"text"} if name == "todo.add" else {"index"}
        if name == "todo.edit":
            required.add("text")
        if name == "todo.set_done":
            required.add("done")
        p = _object(payload, required)
        if "text" in p:
            _text(p["text"], 71)
        if "index" in p:
            _integer(p["index"], 0, 7)
        if "done" in p:
            _boolean(p["done"])
    else:
        p = _object(
            payload,
            {"asset_path", "asset_id"},
            {"display_profile"} if name == "modules.image" else set(),
        )
        if "asset_id" in p:
            _text(p["asset_id"], 36)
        if "asset_path" in p:
            _text(p["asset_path"], 512)
        if "base_url" in p:
            _text(p["base_url"], 512)
        if "display_profile" in p:
            _integer(p["display_profile"], 0, 2)
    return p
