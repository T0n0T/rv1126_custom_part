#!/usr/bin/env python3
"""Create a review-only annotated MP4 preview from source video and tracked detections."""
from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path
import cv2
import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video

COLORS = {"inside": (40, 170, 40), "transition": (0, 190, 255), "outside": (40, 80, 220)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--zones", type=Path, required=True)
    parser.add_argument("--tracks", type=Path, required=True)
    parser.add_argument("--events", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    zones = json.loads(args.zones.read_text(encoding="utf-8"))["zones"]
    by_frame = {}
    for line in args.tracks.read_text(encoding="utf-8").splitlines():
        if line:
            frame = json.loads(line)
            by_frame[frame["frame_index"]] = frame["detections"]
    actions = {}
    for line in args.events.read_text(encoding="utf-8").splitlines():
        if line:
            event = json.loads(line)
            if event["status"] == "confirmed":
                actions.setdefault(event["end_frame"], []).append(event["direction"] + f" x{event['count']}")
    capture = open_video(args.video)
    if not capture.isOpened():
        raise RuntimeError(f"cannot open source video: {args.video}")
    fps = capture.get(cv2.CAP_PROP_FPS)
    if not fps or not np.isfinite(fps):
        fps = 25.0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    writer = None
    frame_index = 0
    try:
        while True:
            ok, image = capture.read()
            if not ok:
                break
            height, width = image.shape[:2]
            if writer is None:
                writer = cv2.VideoWriter(str(args.output), cv2.VideoWriter_fourcc(*"mp4v"), fps, (width, height))
                if not writer.isOpened():
                    raise RuntimeError(f"cannot write preview: {args.output}")
            overlay = image.copy()
            for name, points in zones.items():
                polygon = np.array([[round(x * width), round(y * height)] for x, y in points], dtype=np.int32)
                cv2.polylines(overlay, [polygon], True, COLORS[name], 2, cv2.LINE_AA)
                center = polygon.mean(axis=0).astype(int)
                cv2.putText(overlay, name, tuple(center), cv2.FONT_HERSHEY_SIMPLEX, 0.6, COLORS[name], 2, cv2.LINE_AA)
            image = cv2.addWeighted(overlay, 0.3, image, 0.7, 0)
            for detection in by_frame.get(frame_index, []):
                x1, y1, x2, y2 = detection["xyxy"]
                box = (round(x1 * width), round(y1 * height), round(x2 * width), round(y2 * height))
                cv2.rectangle(image, box[:2], box[2:], (40, 210, 255), 2)
                cv2.putText(image, f"helmet #{detection['track_id']} {detection['score']:.2f}",
                            (box[0], max(18, box[1] - 6)), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (40, 210, 255), 2)
            for index, action in enumerate(actions.get(frame_index, [])):
                cv2.putText(image, action, (16, 32 + index * 28), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
            writer.write(image)
            frame_index += 1
    finally:
        capture.release()
        if writer is not None:
            writer.release()
    if frame_index == 0:
        raise RuntimeError("video contains no frames")
    print(json.dumps({"frames_annotated": frame_index, "fps_preview": fps, "output": str(args.output)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
