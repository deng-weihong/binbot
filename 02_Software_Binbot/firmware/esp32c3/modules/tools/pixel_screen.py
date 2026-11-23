#!/usr/bin/env python3
"""Convert photos or GIFs into pixel-screen assets for the Binbot display."""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageOps
except ImportError:
    print("Pillow is required. Run: py -3 -m pip install -r tools\\requirements-pixel-screen.txt",
          file=sys.stderr)
    raise SystemExit(2)


def parse_color(value):
    value = value.strip().lstrip("#")
    if len(value) not in (6, 8):
        raise argparse.ArgumentTypeError("color must be RRGGBB or RRGGBBAA")
    try:
        channels = tuple(int(value[index:index + 2], 16)
                        for index in range(0, len(value), 2))
    except ValueError as error:
        raise argparse.ArgumentTypeError("invalid hexadecimal color") from error
    if len(channels) == 3:
        return channels + (255,)
    return channels


def fit_image(image, size, fit, background):
    image = ImageOps.exif_transpose(image).convert("RGBA")
    if fit == "cover":
        scale = max(size / image.width, size / image.height)
        new_size = (max(1, round(image.width * scale)),
                    max(1, round(image.height * scale)))
        image = image.resize(new_size, Image.Resampling.LANCZOS)
        left = (image.width - size) // 2
        top = (image.height - size) // 2
        return image.crop((left, top, left + size, top + size))

    scale = min(size / image.width, size / image.height)
    new_size = (max(1, round(image.width * scale)),
                max(1, round(image.height * scale)))
    image = image.resize(new_size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (size, size), background)
    canvas.alpha_composite(image, ((size - image.width) // 2,
                                   (size - image.height) // 2))
    return canvas


def pixelate(image, size, pixel_size, colors, dither, background, circle, fit):
    image = fit_image(image, size, fit, background)
    background_image = Image.new("RGBA", (size, size), background)
    background_image.alpha_composite(image)
    image = background_image
    grid_size = max(1, (size + pixel_size - 1) // pixel_size)
    image = image.resize((grid_size, grid_size), Image.Resampling.BOX)
    image = image.convert("RGB").quantize(
        colors=colors,
        method=Image.Quantize.MEDIANCUT,
        dither=(Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE),
    ).convert("RGBA")
    image = image.resize((size, size), Image.Resampling.NEAREST)

    if circle:
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, size - 1, size - 1), fill=255)
        background_image = Image.new("RGBA", (size, size), background)
        image = Image.composite(image, background_image, mask)

    return image.convert("RGB")


def rgb565_bytes(image):
    output = bytearray()
    data = image.convert("RGB").tobytes()
    for index in range(0, len(data), 3):
        red, green, blue = data[index:index + 3]
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        output.extend((value >> 8, value & 0xFF))
    return bytes(output)


def load_frames(source, size, pixel_size, colors, dither, background, circle,
                fit, max_frames):
    frame_count = min(getattr(source, "n_frames", 1), max_frames)
    frames = []
    durations = []
    for index in range(frame_count):
        source.seek(index)
        frames.append(pixelate(source, size, pixel_size, colors, dither,
                               background, circle, fit))
        duration = int(source.info.get("duration", 120) or 120)
        durations.append(max(20, duration))
    if not frames:
        raise ValueError("input contains no image frames")
    return frames, durations


def save_gif(frames, durations, output, colors):
    palette_frames = []
    for frame in frames:
        palette_frames.append(frame.quantize(colors=colors,
                                             method=Image.Quantize.MEDIANCUT,
                                             dither=Image.Dither.NONE))
    palette_frames[0].save(
        output,
        format="GIF",
        save_all=True,
        append_images=palette_frames[1:],
        duration=durations,
        loop=0,
        disposal=2,
        optimize=False,
    )


def build_parser():
    parser = argparse.ArgumentParser(
        description="Create 240x240 pixel-screen GIF and preview assets.")
    parser.add_argument("input", type=Path, help="input JPG, PNG or GIF")
    parser.add_argument("-o", "--output", type=Path,
                        help="output GIF path, default: <input>_pixel.gif")
    parser.add_argument("--preview", type=Path,
                        help="preview PNG path, default: <input>_pixel_preview.png")
    parser.add_argument("--rgb565", type=Path,
                        help="optional raw big-endian RGB565 output path")
    parser.add_argument("--size", type=int, default=240,
                        help="square output size, default: 240")
    parser.add_argument("--pixel-size", type=int, default=4,
                        help="pixel block size in display pixels, default: 4")
    parser.add_argument("--colors", type=int, default=32,
                        help="palette size from 2 to 256, default: 32")
    parser.add_argument("--fit", choices=("cover", "contain"), default="cover",
                        help="fit mode, default: cover")
    parser.add_argument("--background", type=parse_color, default=(0, 0, 0, 255),
                        help="background color, default: 000000")
    parser.add_argument("--dither", action="store_true",
                        help="enable Floyd-Steinberg color dithering")
    parser.add_argument("--square", dest="circle", action="store_false",
                        help="disable the round-screen mask")
    parser.add_argument("--max-frames", type=int, default=60,
                        help="maximum animated frames, default: 60")
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if not args.input.is_file():
        parser.error("input file does not exist: " + str(args.input))
    if not 1 <= args.size <= 240:
        parser.error("size must be between 1 and 240")
    if not 1 <= args.pixel_size <= args.size:
        parser.error("pixel-size must be between 1 and size")
    if not 2 <= args.colors <= 256:
        parser.error("colors must be between 2 and 256")
    if not 1 <= args.max_frames <= 200:
        parser.error("max-frames must be between 1 and 200")

    output = args.output or args.input.with_name(args.input.stem + "_pixel.gif")
    preview = args.preview or args.input.with_name(
        args.input.stem + "_pixel_preview.png")
    output.parent.mkdir(parents=True, exist_ok=True)
    preview.parent.mkdir(parents=True, exist_ok=True)
    if args.rgb565:
        args.rgb565.parent.mkdir(parents=True, exist_ok=True)

    try:
        with Image.open(args.input) as source:
            frames, durations = load_frames(
                source, args.size, args.pixel_size, args.colors, args.dither,
                args.background, args.circle, args.fit, args.max_frames)
    except (OSError, ValueError) as error:
        print("Failed to process input: " + str(error), file=sys.stderr)
        return 1

    save_gif(frames, durations, output, args.colors)
    frames[0].save(preview, format="PNG")
    if args.rgb565:
        args.rgb565.write_bytes(rgb565_bytes(frames[0]))

    print("Pixel screen asset generated")
    print("  input:       " + str(args.input))
    print("  gif:         " + str(output))
    print("  preview:     " + str(preview))
    print("  frames:      " + str(len(frames)))
    print("  size:         {}x{}".format(args.size, args.size))
    print("  pixel block:  {}x{}".format(args.pixel_size, args.pixel_size))
    print("  colors:       {}".format(args.colors))
    if args.rgb565:
        print("  rgb565:      " + str(args.rgb565))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
