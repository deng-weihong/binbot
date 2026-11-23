from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from typing import Any, Protocol
from uuid import uuid4


class ImageNormalizer(Protocol):
    ffmpeg_path: str

    def _normalize_component_gif(self, data: bytes, width: int, height: int) -> tuple[str, Path, int, str]: ...


def image_name(name: str) -> str:
    name = "".join(c for c in Path(name).name if c.isprintable()).strip()
    return name.encode("utf-8")[:71].decode("utf-8", errors="ignore") or "图片"


def prepare_module_image(
    normalizer: ImageNormalizer, root: Path, content: bytes, name: str,
    asset_id: str | None = None, reused: tuple[int, int, Path] | None = None,
) -> tuple[str, Path]:
    asset_id = asset_id or str(uuid4())
    root.mkdir(parents=True, exist_ok=True)
    final = root / asset_id
    if (final / "manifest.json").is_file():
        return asset_id, final
    staging = root / f".{asset_id}-{uuid4().hex}"
    staging.mkdir(mode=0o700)
    try:
        manifest: dict[str, Any] = {"schema": 1, "id": asset_id, "name": image_name(name)}
        for key, dimensions in (("round", (240, 240)), ("holo", (320, 240))):
            target = staging / f"{key}.gif"
            if reused and reused[:2] == dimensions:
                shutil.copyfile(reused[2], target)
                data = target.read_bytes()
                size, digest = len(data), hashlib.sha256(data).hexdigest()
            else:
                _, normalized, size, digest = normalizer._normalize_component_gif(content, *dimensions)
                os.replace(normalized, target)
            manifest[key] = {"size": size, "sha256": digest,
                             "width": dimensions[0], "height": dimensions[1]}
        result = subprocess.run(
            [normalizer.ffmpeg_path, "-hide_banner", "-loglevel", "error", "-y",
             "-i", str(staging / "round.gif"), "-frames:v", "1", str(staging / "thumbnail.png")],
            capture_output=True, timeout=30, check=False,
        )
        if result.returncode:
            raise RuntimeError("could not generate module image thumbnail")
        thumbnail = (staging / "thumbnail.png").read_bytes()
        manifest["thumbnail"] = {"size": len(thumbnail), "sha256": hashlib.sha256(thumbnail).hexdigest()}
        (staging / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        os.replace(staging, final)
        return asset_id, final
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
