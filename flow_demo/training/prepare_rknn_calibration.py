#!/usr/bin/env python3
"""Prepare a traceable train-image/cabinet-video RKNN calibration set."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
import sys
from pathlib import Path

import cv2
from PIL import Image

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video

from prepare_rknn_input import letterbox


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--videos", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--image-count", type=int, default=100)
    parser.add_argument("--video-count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    if args.image_count < 0 or args.video_count < 0 or args.image_count + args.video_count == 0:
        parser.error("sample counts must be nonnegative with at least one sample")
    output = args.output_dir.resolve()
    if any((output.parent / name).exists() for name in ("calibration.txt", "calibration.json")):
        raise FileExistsError("calibration manifests already exist; use a new run directory")
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save(image: Image.Image, source: dict) -> None:
        prepared, geometry = letterbox(image.convert("RGB"), 640, 114)
        path = output / f"{len(records):04d}.png"
        prepared.save(path)
        records.append({**source, "prepared": str(path), "prepared_sha256": sha256(path), **geometry})

    # Only train images are sampled; validation/test images are not calibration data.
    images = sorted(p for p in (args.dataset / "train/images").iterdir() if p.suffix.lower() in {".jpg", ".jpeg", ".png"})
    if len(images) < args.image_count:
        raise ValueError("not enough training images")
    for path in random.Random(args.seed).sample(images, args.image_count):
        with Image.open(path) as image:
            save(image, {"kind": "train_image", "source": str(path.resolve()), "source_sha256": sha256(path)})
    videos = sorted(args.videos.glob("*.mp4"))
    if args.video_count and not videos:
        raise ValueError("no calibration videos")
    for i, path in enumerate(videos):
        count = args.video_count // len(videos) + (i < args.video_count % len(videos))
        if not count:
            continue
        cap = open_video(path)
        try:
            frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
            fps = cap.get(cv2.CAP_PROP_FPS)
            if not cap.isOpened() or frames <= 0 or fps <= 0:
                raise ValueError(f"invalid video: {path}")
            digest = sha256(path)
            for j in range(count):
                frame_index = min(frames - 1, round((j + 0.5) * frames / count))
                cap.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
                ok, frame = cap.read()
                if not ok:
                    raise ValueError(f"cannot decode {path} frame {frame_index}")
                actual_frame = round(cap.get(cv2.CAP_PROP_POS_FRAMES)) - 1
                save(Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)), {
                    "kind": "cabinet_video", "source": str(path.resolve()), "source_sha256": digest,
                    "frame_index": actual_frame, "timestamp_ms": cap.get(cv2.CAP_PROP_POS_MSEC), "fps": fps,
                })
        finally:
            cap.release()
    (output.parent / "calibration.txt").write_text("".join(f"{r['prepared']}\n" for r in records))
    (output.parent / "calibration.json").write_text(json.dumps({
        "seed": args.seed, "image_count": args.image_count, "video_count": args.video_count,
        "color": "RGB", "size": [640, 640], "padding": 114, "resize": "PIL LANCZOS",
        "note": "Cabinet videos are used for calibration; these samples are not an independent accuracy holdout.",
        "images": records,
    }, ensure_ascii=False, indent=2) + "\n")
    print(f"Prepared {len(records)} samples: {output.parent / 'calibration.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
