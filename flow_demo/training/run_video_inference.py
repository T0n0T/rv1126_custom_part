#!/usr/bin/env python3
"""Run the YOLO26l ONNX model on every video frame and emit source-normalized detections."""
from __future__ import annotations
import argparse
import json
import hashlib
import subprocess
import sys
import time
from pathlib import Path
import cv2
import numpy as np
import onnxruntime as ort

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video


def source_pts(video: Path) -> list[float]:
    command = ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
               "frame=best_effort_timestamp_time", "-of", "json", str(video)]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except FileNotFoundError as error:
        raise RuntimeError("ffprobe is required to retain source video PTS") from error
    payload = json.loads(result.stdout)
    frames = payload.get("frames", [])
    pts = []
    for index, frame in enumerate(frames):
        value = frame.get("best_effort_timestamp_time")
        if value in (None, "N/A"):
            raise ValueError(f"frame {index} has no best_effort_timestamp_time")
        timestamp = float(value) * 1000.0
        if not np.isfinite(timestamp) or (pts and timestamp < pts[-1]):
            raise ValueError(f"nonmonotonic or invalid source PTS at frame {index}")
        pts.append(timestamp)
    if not pts:
        raise ValueError(f"no decoded video frames: {video}")
    return pts


def letterbox(frame: np.ndarray, size: int = 640) -> tuple[np.ndarray, dict]:
    height, width = frame.shape[:2]
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
    boxed = cv2.copyMakeBorder(resized, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
    transform = {"scale": scale, "pad_left": left, "pad_top": top,
                "resized_width": resized_width, "resized_height": resized_height,
                "source_width": width, "source_height": height}
    return boxed, transform


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="JSONL with frame_index, source pts_ms and detections")
    parser.add_argument("--conf", type=float, default=0.10)
    parser.add_argument("--max-frames", type=int, default=0, help="0 processes every frame")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--provider", choices=("cpu",), default="cpu")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 0 <= args.conf <= 1 or args.max_frames < 0 or args.threads <= 0:
        raise ValueError("invalid confidence threshold, frame limit, or thread count")
    model = args.model.resolve()
    video = args.video.resolve()
    if not model.is_file() or not video.is_file():
        raise FileNotFoundError("model and video must exist")
    pts = source_pts(video)
    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(model), sess_options=options, providers=["CPUExecutionProvider"])
    inputs = session.get_inputs()
    outputs = session.get_outputs()
    if len(inputs) != 1 or inputs[0].shape != [1, 3, 640, 640] or len(outputs) != 1 or outputs[0].shape != [1, 300, 6]:
        raise ValueError(f"unexpected ONNX contract: inputs={[v.shape for v in inputs]} outputs={[v.shape for v in outputs]}")
    capture = open_video(video)
    if not capture.isOpened():
        raise RuntimeError(f"cannot decode video: {video}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    limit = min(len(pts), args.max_frames) if args.max_frames else len(pts)
    started = time.perf_counter()
    model_sha = hashlib.sha256(model.read_bytes()).hexdigest()
    video_sha = hashlib.sha256(video.read_bytes()).hexdigest()
    source_size = None
    counts = [0, 0]
    try:
        with args.output.open("w", encoding="utf-8") as output:
            for frame_index in range(limit):
                ok, frame = capture.read()
                if not ok:
                    raise RuntimeError(f"OpenCV ended before ffprobe frame {frame_index}/{limit}")
                frame_size = (frame.shape[1], frame.shape[0])
                if source_size is None:
                    source_size = frame_size
                elif source_size != frame_size:
                    raise RuntimeError(f"source frame dimensions changed at frame {frame_index}")
                boxed, transform = letterbox(frame)
                rgb = cv2.cvtColor(boxed, cv2.COLOR_BGR2RGB)
                tensor = np.ascontiguousarray(rgb.transpose(2, 0, 1)[None], dtype=np.float32) / np.float32(255.0)
                rows = session.run(None, {inputs[0].name: tensor})[0][0]
                detections = []
                for row in rows:
                    x1, y1, x2, y2, score, class_id_value = (float(value) for value in row)
                    class_id = int(round(class_id_value))
                    if score < args.conf:
                        continue
                    if class_id not in (0, 1) or abs(class_id_value - class_id) > 1e-3:
                        raise ValueError(f"unexpected class id {class_id_value} at frame {frame_index}")
                    scale = transform["scale"]
                    x1 = min(max((x1 - transform["pad_left"]) / scale / transform["source_width"], 0.0), 1.0)
                    x2 = min(max((x2 - transform["pad_left"]) / scale / transform["source_width"], 0.0), 1.0)
                    y1 = min(max((y1 - transform["pad_top"]) / scale / transform["source_height"], 0.0), 1.0)
                    y2 = min(max((y2 - transform["pad_top"]) / scale / transform["source_height"], 0.0), 1.0)
                    if x2 <= x1 or y2 <= y1:
                        continue
                    detections.append({"xyxy": [x1, y1, x2, y2], "score": score, "class_id": class_id})
                    counts[class_id] += 1
                record = {"frame_index": frame_index, "pts_ms": pts[frame_index],
                          "source_video": str(video), "source_width": frame.shape[1],
                          "source_height": frame.shape[0], "detections": detections}
                output.write(json.dumps(record, separators=(",", ":")) + "\n")
                if frame_index == 0 or (frame_index + 1) % 25 == 0 or frame_index + 1 == limit:
                    elapsed = time.perf_counter() - started
                    print(f"frame={frame_index + 1}/{limit} pts_ms={pts[frame_index]:.3f} detections={len(detections)} elapsed_s={elapsed:.1f}", file=sys.stderr, flush=True)
        if not args.max_frames and capture.read()[0]:
            raise RuntimeError("OpenCV decoded more frames than ffprobe reported")
    finally:
        capture.release()
    elapsed = time.perf_counter() - started
    metadata = {"model": str(model), "model_sha256": model_sha, "source_video": str(video),
                "source_video_sha256": video_sha, "source_video_frames": len(pts),
                "processed_frames": limit, "partial": limit != len(pts),
                "pts_source": "ffprobe best_effort_timestamp_time", "confidence_threshold": args.conf,
                "input": {"width": 640, "height": 640, "layout": "NCHW", "color": "RGB",
                          "dtype": "float32", "normalization": "/255", "padding": 114,
                          "resize": "OpenCV INTER_LINEAR centered letterbox"},
                "classes": {"0": "helmet", "1": "head"}, "elapsed_seconds": elapsed,
                "detections_helmet": counts[0], "detections_head": counts[1]}
    metadata_path = args.output.with_suffix(args.output.suffix + ".meta.json")
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"frames": limit, "source_frames": len(pts), "detections_helmet": counts[0],
                      "detections_head": counts[1], "elapsed_seconds": elapsed,
                      "pts_source": "ffprobe best_effort_timestamp_time", "output": str(args.output),
                      "metadata": str(metadata_path)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
