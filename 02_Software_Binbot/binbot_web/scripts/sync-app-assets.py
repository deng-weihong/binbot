#!/usr/bin/env python3
"""Refresh console catalogs, static previews, gesture glyphs, and font subset.

Requires Pillow, fontTools and Brotli. The web app uses checked-in outputs and
does not require Python at runtime.
"""

import json
import os
import re
import shutil
import struct
from pathlib import Path

from fontTools import subset
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get("BINBOT_APP_ROOT", ROOT.parent / "binbot_app")).resolve()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def main():
    source = (APP / "apps/binbot_app/src/core/app_state.c").read_text()
    block = source.split("vocabulary_cards[APP_VOCABULARY_CAPACITY] = {", 1)[1].split("\n};", 1)[0]
    entries = re.findall(
        r'\{\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\s*\}',
        block,
    )
    words = [dict(zip(("word", "phonetic", "meaning", "example", "translation"), row)) for row in entries]
    assert len(words) == 30, "App word count changed; review the learning page."
    write_json(ROOT / "src/shared/model/catalogs/vocabulary.json", words)

    ids = json.loads((APP / "assets/standby/index.json").read_text())["themes"]
    assert 0 < len(ids) <= 32
    destination = ROOT / "public/watch-faces"
    destination.mkdir(parents=True, exist_ok=True)
    faces = []
    for face_id in ids:
        assert re.fullmatch(r"[a-z0-9-]+", face_id)
        manifest = json.loads((APP / "assets/standby" / face_id / "manifest.json").read_text())
        raw = (APP / "assets/standby/previews" / (face_id + ".rgb565")).read_bytes()
        assert len(raw) == 152 * 152 * 2
        pixels = struct.unpack("<" + "H" * (152 * 152), raw)
        rgb = bytes(channel for pixel in pixels for channel in (
            ((pixel >> 11) & 31) * 255 // 31,
            ((pixel >> 5) & 63) * 255 // 63,
            (pixel & 31) * 255 // 31,
        ))
        Image.frombytes("RGB", (152, 152), rgb).save(destination / (face_id + ".png"), optimize=True)
        faces.append({"id": face_id, "name": manifest["name"], "english": manifest.get("english", "") or ("Expressions" if face_id == "face" else ""), "preview": "/watch-faces/" + face_id + ".png"})
    write_json(ROOT / "src/shared/model/catalogs/watchFaces.json", faces)

    media = ROOT / "public/module-media"
    media.mkdir(parents=True, exist_ok=True)
    for filename in ("cat_happy.gif", "cat_sad.gif", "music_cover.png"):
        shutil.copyfile(APP / "assets" / filename, media / filename)
    glyphs = ROOT / "public/gestures"
    glyphs.mkdir(parents=True, exist_ok=True)
    for app_id, console_id in (("ok", "ok"), ("thumb", "thumb"), ("palm", "palm"), ("v", "victory"), ("fist", "fist")):
        shutil.copyfile(APP / "assets/reference/icons/46" / f"gesture_{app_id}.png", glyphs / f"{console_id}.png")

    options = subset.Options()
    options.flavor = "woff2"
    font = subset.load_font(str(APP / "assets/fonts/NotoSansSC-wght.ttf"), options)
    characters = set()
    for path in (ROOT / "src").rglob("*"):
        if path.suffix in {".tsx", ".ts", ".json"}:
            characters.update(path.read_text())
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes={ord(character) for character in characters})
    subsetter.subset(font)
    subset.save_font(font, str(ROOT / "public/fonts/NotoSansSC-subset.woff2"), options)
    print(f"Synced {len(words)} words, {len(faces)} faces, 5 gestures, 3 module images, and the UI font subset.")


if __name__ == "__main__":
    main()
