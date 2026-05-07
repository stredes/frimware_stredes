#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageSequence


ASCII_CHARS = " .:-=+*#%@"


def sanitize_stem(name: str) -> str:
    out = []
    for c in name:
        if c.isalnum() or c in ("-", "_"):
            out.append(c)
        elif c in (" ", "."):
            out.append("_")
    result = "".join(out).strip("_")
    return result or "gif_asset"


def render_ascii(frame: Image.Image, width: int) -> str:
    gray = frame.convert("L")
    src_w, src_h = gray.size
    height = max(1, int(src_h * (width / src_w) * 0.5))
    small = gray.resize((width, height))
    pixels = list(small.getdata())

    lines = []
    for y in range(height):
        row = pixels[y * width : (y + 1) * width]
        text = "".join(ASCII_CHARS[p * (len(ASCII_CHARS) - 1) // 255] for p in row)
        lines.append(text)
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare a GIF for Bruce firmware workflows.")
    parser.add_argument("gif", type=Path, help="Source GIF path")
    parser.add_argument("--output-root", type=Path, default=Path("assets/gif"), help="Output root")
    parser.add_argument("--ascii-width", type=int, default=42, help="ASCII preview width")
    parser.add_argument(
        "--install-boot-sd",
        action="store_true",
        help="Copy the original GIF to sd_files/boot.gif for direct SD boot testing",
    )
    args = parser.parse_args()

    src = args.gif
    if not src.exists():
        raise SystemExit(f"Missing GIF: {src}")

    gif = Image.open(src)
    stem = sanitize_stem(src.stem)
    png_dir = args.output_root / f"{stem}_frames"
    ascii_dir = args.output_root / f"{stem}_ascii"
    png_dir.mkdir(parents=True, exist_ok=True)
    ascii_dir.mkdir(parents=True, exist_ok=True)

    durations = []
    frame_count = 0

    for i, frame in enumerate(ImageSequence.Iterator(gif)):
        rgba = frame.convert("RGBA")
        rgba.save(png_dir / f"frame_{i:03d}.png")
        (ascii_dir / f"frame_{i:03d}.txt").write_text(render_ascii(rgba, args.ascii_width), encoding="utf-8")
        durations.append(frame.info.get("duration", gif.info.get("duration", 0)))
        frame_count += 1

    summary = (
        f"Prepared {frame_count} frame(s)\n"
        f"Source: {src}\n"
        f"Size: {gif.size[0]}x{gif.size[1]}\n"
        f"Durations(ms): {durations}\n"
        f"PNG frames: {png_dir}\n"
        f"ASCII frames: {ascii_dir}\n"
    )
    print(summary)

    if args.install_boot_sd:
        boot_path = Path("sd_files/boot.gif")
        boot_path.write_bytes(src.read_bytes())
        print(f"Installed boot GIF to {boot_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
