#!/usr/bin/env python3
"""Create the fixed RGB/NHWC input used by the board RKNN smoke test.

The converter records a 640x640 RGB input with letterbox padding value 114.
This utility intentionally writes raw bytes so the board smoke executable does
not need an image codec.  A JSON sidecar preserves the resize and padding
parameters needed later when mapping detections back to source coordinates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


def letterbox(image: Image.Image, size: int, padding: int) -> tuple[Image.Image, dict[str, float | int]]:
    if size <= 0:
        raise ValueError("size must be positive")
    if not 0 <= padding <= 255:
        raise ValueError("padding must be in [0, 255]")

    source_width, source_height = image.size
    if source_width <= 0 or source_height <= 0:
        raise ValueError("input image has no pixels")

    scale = min(size / source_width, size / source_height)
    resized_width = max(1, round(source_width * scale))
    resized_height = max(1, round(source_height * scale))
    resized = image.resize((resized_width, resized_height), Image.Resampling.LANCZOS)

    canvas = Image.new("RGB", (size, size), (padding, padding, padding))
    left = (size - resized_width) // 2
    top = (size - resized_height) // 2
    canvas.paste(resized, (left, top))
    return canvas, {
        "source_width": source_width,
        "source_height": source_height,
        "input_width": size,
        "input_height": size,
        "scale": scale,
        "resized_width": resized_width,
        "resized_height": resized_height,
        "pad_left": left,
        "pad_top": top,
        "padding_value": padding,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="raw RGB output")
    parser.add_argument("--size", type=int, default=640)
    parser.add_argument("--padding", type=int, default=114)
    parser.add_argument(
        "--manifest",
        type=Path,
        help="optional JSON sidecar (default: <output>.json)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = args.output
    manifest = args.manifest or output.with_suffix(output.suffix + ".json")
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest.parent.mkdir(parents=True, exist_ok=True)

    with Image.open(args.image) as source:
        image, metadata = letterbox(source.convert("RGB"), args.size, args.padding)

    raw_rgb = image.tobytes()
    output.write_bytes(raw_rgb)
    manifest.write_text(
        json.dumps(
            {
                "image": str(args.image),
                "output": str(output),
                "layout": "NHWC",
                "color": "RGB",
                "dtype": "uint8",
                "input_sha256": hashlib.sha256(raw_rgb).hexdigest(),
                **metadata,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"raw RGB: {output} ({output.stat().st_size} bytes)")
    print(f"manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
