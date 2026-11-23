import asyncio
import base64
from pathlib import Path
import shutil
import subprocess

import pytest

from binbot_server.application.component_ota import ComponentOTAService
from binbot_server.application.module_images import prepare_module_image
from binbot_server.application.registry import DeviceSessionRegistry
from binbot_server.config import ComponentConfig


# Two moving rectangles with disposal=2. Transparent palette entry 0 contains
# the brown-gray RGB that the previous conversion exposed as a visible color.
TRANSPARENT_GIF = base64.b64decode(
    "R0lGODlhIAAYAIEAALGblfAgMAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQJCAAAACwAAAAAIAAY"
    "AAAISAABCBxIsKDBgwgTKlzIsKHDhxAjSpxoMIDFixYpEsSIUeNAjhc9CgSZUSTJACIBnEy50iRJ"
    "li9dgoQ5UybHlDhz6tzJs6fIgAAh+QQJCAAAACwUAAYACAAMAIGxm5Ug8EAAAAAAAAAIEgADCBxI"
    "sKDBgwgTKlzIsGHBgAA7"
)
FFMPEG = shutil.which("ffmpeg") or ""
pytestmark = pytest.mark.skipif(not FFMPEG, reason="FFmpeg is required")


def png_source(opaque: bool = False) -> bytes:
    pixels = bytearray()
    for y in range(24):
        for x in range(32):
            if 8 <= x < 24 and 6 <= y < 18:
                pixels.extend((240, 32, 48, 255))
            else:
                pixels.extend((177, 155, 149, 255 if opaque else 0))
    return subprocess.run(
        [FFMPEG, "-v", "error", "-f", "rawvideo", "-pixel_format", "rgba",
         "-video_size", "32x24", "-i", "pipe:0", "-frames:v", "1",
         "-c:v", "png", "-f", "image2pipe", "pipe:1"],
        input=bytes(pixels), capture_output=True, check=True,
    ).stdout


def rgba_frames(path: Path, width: int, height: int) -> list[bytes]:
    data = subprocess.run(
        [FFMPEG, "-v", "error", "-i", str(path), "-vsync", "0",
         "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"],
        capture_output=True, check=True,
    ).stdout
    size = width * height * 4
    assert data and len(data) % size == 0
    return [data[offset:offset + size] for offset in range(0, len(data), size)]


def pixel(frame: bytes, width: int, x: int, y: int) -> tuple[int, ...]:
    offset = (y * width + x) * 4
    return tuple(frame[offset:offset + 4])


@pytest.mark.parametrize("width,height", [(240, 240), (320, 240)])
@pytest.mark.parametrize("kind", ["gif", "png", "opaque_png"])
def test_normalized_image_retains_transparency_and_foreground(
    tmp_path: Path, width: int, height: int, kind: str,
) -> None:
    service = ComponentOTAService(
        ComponentConfig(directory=str(tmp_path / "ota")), DeviceSessionRegistry(),
        asset_directory=str(tmp_path / "assets"), ffmpeg_path=FFMPEG,
    )
    try:
        content = TRANSPARENT_GIF if kind == "gif" else png_source(kind == "opaque_png")
        _, normalized, size, _ = service._normalize_component_gif(content, width, height)
        assert size <= 512 * 1024
        frames = rgba_frames(normalized, width, height)
        scaled_height = width * 24 // 32
        top = (height - scaled_height) // 2
        for frame in frames:
            corner = pixel(frame, width, 1, top + 1)
            assert corner[3] == (255 if kind == "opaque_png" else 0)
            if kind == "opaque_png":
                assert all(abs(actual - expected) <= 1
                           for actual, expected in zip(corner[:3], (177, 155, 149)))
            if top:
                assert pixel(frame, width, width // 2, 1)[3] == 0
        if kind == "gif":
            assert len(frames) == 2
            assert pixel(frames[0], width, width // 4, height // 2) == (240, 32, 48, 255)
            assert pixel(frames[1], width, width * 3 // 4, height // 2) == (32, 240, 64, 255)
            assert pixel(frames[1], width, width // 4, height // 2)[3] == 0
        else:
            assert pixel(frames[0], width, width // 2, height // 2) == (240, 32, 48, 255)
    finally:
        asyncio.run(service.close())


def test_gallery_thumbnail_retains_transparency(tmp_path: Path) -> None:
    service = ComponentOTAService(
        ComponentConfig(directory=str(tmp_path / "ota")), DeviceSessionRegistry(),
        asset_directory=str(tmp_path / "assets"), ffmpeg_path=FFMPEG,
    )
    try:
        _, bundle = prepare_module_image(service, tmp_path / "gallery", TRANSPARENT_GIF, "transparent.gif")
        frame = rgba_frames(bundle / "thumbnail.png", 240, 240)[0]
        assert pixel(frame, 240, 0, 0)[3] == 0
        assert pixel(frame, 240, 60, 120) == (240, 32, 48, 255)
    finally:
        asyncio.run(service.close())
