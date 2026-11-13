#!/usr/bin/env python3
"""Pack a video as 640x640 RGB frames and source-PTS metadata for RV1126B."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import cv2
import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video


def source_pts(video: Path) -> list[float]:
    command = ["ffprobe", "-v", "error", "-select_streams", "v:0",
               "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", str(video)]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    frames = json.loads(result.stdout).get("frames", [])
    pts: list[float] = []
    for index, frame in enumerate(frames):
        value = frame.get("best_effort_timestamp_time")
        if value in (None, "N/A"):
            raise ValueError(f"frame {index} has no source PTS: {video}")
        timestamp = float(value) * 1000.0
        if not np.isfinite(timestamp) or (pts and timestamp < pts[-1]):
            raise ValueError(f"nonmonotonic or invalid source PTS at frame {index}")
        pts.append(timestamp)
    if not pts:
        raise ValueError(f"no decoded video frames: {video}")
    return pts


def letterbox_bgr(frame: np.ndarray, size: int = 640) -> tuple[np.ndarray, dict[str, int | float]]:
    height, width = frame.shape[:2]
    if width <= 0 or height <= 0:
        raise ValueError("frame has no pixels")
    scale = min(size / width, size / height)
    resized_width = max(1, round(width * scale))
    resized_height = max(1, round(height * scale))
    resized = cv2.resize(frame, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
    pad_x = size - resized_width
    pad_y = size - resized_height
    left = round(pad_x / 2 - 0.1)
    right = round(pad_x / 2 + 0.1)
    top = round(pad_y / 2 - 0.1)
    bottom = round(pad_y / 2 + 0.1)
    boxed = cv2.copyMakeBorder(resized, top, bottom, left, right,
                               cv2.BORDER_CONSTANT, value=(114, 114, 114))
    if boxed.shape != (size, size, 3):
        raise RuntimeError(f"letterbox produced unexpected shape {boxed.shape}")
    return boxed, {"scale": scale, "pad_left": left, "pad_top": top,
                   "resized_width": resized_width, "resized_height": resized_height,
                   "source_width": width, "source_height": height}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--frames", required=True, type=Path, help="new concatenated RGB stream")
    parser.add_argument("--metadata", required=True, type=Path, help="new whitespace TSV for the board C tool")
    parser.add_argument("--max-frames", type=int, default=0, help="0 processes every frame")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    video = args.video.resolve()
    frames_path = args.frames.resolve()
    metadata_path = args.metadata.resolve()
    manifest_path = frames_path.with_suffix(frames_path.suffix + ".json")
    if args.max_frames < 0 or not video.is_file():
        raise ValueError("video must exist and max-frames must be nonnegative")
    if len({frames_path, metadata_path, manifest_path}) != 3:
        raise ValueError("frames, metadata, and manifest paths must be distinct")
    for path in (frames_path, metadata_path, manifest_path):
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            raise FileExistsError(f"refusing to overwrite existing output: {path}")

    pts = source_pts(video)
    limit = min(args.max_frames, len(pts)) if args.max_frames else len(pts)
    capture = open_video(video)
    if not capture.isOpened():
        raise RuntimeError(f"cannot decode video: {video}")
    frame_hash = hashlib.sha256()
    source_size: tuple[int, int] | None = None
    try:
        with frames_path.open("xb") as raw, metadata_path.open("x", encoding="utf-8") as rows:
            for frame_index in range(limit):
                ok, frame = capture.read()
                if not ok:
                    raise RuntimeError(f"OpenCV ended before ffprobe frame {frame_index}/{limit}")
                height, width = frame.shape[:2]
                if source_size is None:
                    source_size = (width, height)
                elif source_size != (width, height):
                    raise RuntimeError(f"source frame dimensions changed at frame {frame_index}")
                boxed, transform = letterbox_bgr(frame)
                rgb = cv2.cvtColor(boxed, cv2.COLOR_BGR2RGB)
                data = np.ascontiguousarray(rgb).tobytes()
                raw.write(data)
                frame_hash.update(data)
                rows.write(f"{frame_index} {pts[frame_index]:.6f} {width} {height} "
                           f"{transform['scale']:.12g} {transform['pad_left']} "
                           f"{transform['pad_top']}\n")
            raw.flush()
            rows.flush()
        if not args.max_frames and capture.read()[0]:
            raise RuntimeError("OpenCV decoded more frames than ffprobe reported")
    finally:
        capture.release()
    manifest = {
        "source_video": str(video),
        "source_video_sha256": sha256_file(video),
        "frames_rgb": str(frames_path),
        "frames_rgb_sha256": frame_hash.hexdigest(),
        "metadata_tsv": str(metadata_path),
        "frame_count": limit,
        "partial": limit != len(pts),
        "pts_source": "ffprobe best_effort_timestamp_time",
        "input": {"width": 640, "height": 640, "layout": "NHWC", "color": "RGB",
                  "dtype": "uint8", "normalization": "RKNN model input quantization",
                  "padding": 114, "resize": "OpenCV INTER_LINEAR centered letterbox"},
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
    print(json.dumps({"frames": limit, "partial": manifest["partial"],
                      "frames_rgb": str(frames_path), "metadata": str(metadata_path),
                      "manifest": str(manifest_path),
                      "frames_rgb_sha256": manifest["frames_rgb_sha256"]}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
