#!/usr/bin/env python3
"""Bind a confirmed camera geometry profile to one concrete input session."""
from __future__ import annotations

import hashlib
import argparse
import json
import sys
from pathlib import Path

import cv2

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video

try:
    from tracking.calibrate_zones import validate_zone_geometry
except ModuleNotFoundError:  # direct execution as tracking/session_config.py
    from calibrate_zones import validate_zone_geometry


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_video_size(video: Path) -> tuple[int, int]:
    capture = open_video(video)
    if not capture.isOpened():
        raise ValueError(f"cannot read video dimensions: {video}")
    try:
        ok, frame = capture.read()
        if not ok:
            raise ValueError(f"cannot decode first video frame: {video}")
        height, width = frame.shape[:2]
        return width, height
    finally:
        capture.release()


def build_session_config(profile_path: Path, video: Path, session_id: str) -> dict:
    if not video.is_file():
        raise ValueError(f"video not found: {video}")
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    if profile.get("geometry_confirmed") is not True:
        raise ValueError("profile requires geometry_confirmed=true after camera review")
    for name in ("inside", "transition", "outside"):
        if not isinstance(profile.get("zones", {}).get(name), list):
            raise ValueError(f"profile is missing zones.{name}")
    try:
        validate_zone_geometry(profile["zones"])
    except (TypeError, ValueError) as error:
        raise ValueError(f"profile zone geometry is invalid: {error}") from error
    calibration = profile.get("calibration", {})
    expected_width = calibration.get("source_width")
    expected_height = calibration.get("source_height")
    if not isinstance(expected_width, int) or not isinstance(expected_height, int) or \
            expected_width <= 0 or expected_height <= 0:
        raise ValueError("profile calibration requires positive source_width and source_height")
    actual_width, actual_height = read_video_size(video)
    if expected_width * actual_height != expected_height * actual_width:
        raise ValueError(
            f"video aspect ratio {actual_width}:{actual_height} differs from calibrated "
            f"{expected_width}:{expected_height}; recalibrate this camera view"
        )
    result = dict(profile)
    result["session_id"] = session_id
    result["video"] = str(video.resolve())
    result["calibration_video"] = profile.get("video", "")
    result["runtime"] = {
        "video_sha256": sha256(video),
        "video_width": actual_width,
        "video_height": actual_height,
        "profile_path": str(profile_path.resolve()),
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config = build_session_config(args.profile, args.video, args.session_id)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
