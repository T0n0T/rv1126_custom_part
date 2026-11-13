#!/usr/bin/env python3
"""Interactively calibrate three source-video polygons against sampled frames.

Click polygon vertices for inside, transition, and outside. Use 1/2/3 to select
a zone, n to advance, u to undo, r to reset, [ and ] to inspect sampled frames,
y to confirm after all samples have been reviewed, and q/Esc to cancel.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Iterable

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))
from video_io import open_video

try:
    import cv2
    import numpy as np
except ImportError as error:  # pragma: no cover - host dependency
    raise SystemExit("OpenCV with GUI support and NumPy are required") from error

ZONE_NAMES = ("inside", "transition", "outside")
COLORS = {"inside": (60, 190, 60), "transition": (0, 190, 255), "outside": (70, 90, 230)}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalized_points(points: Iterable[tuple[int, int]], width: int, height: int) -> list[list[float]]:
    if width <= 0 or height <= 0:
        raise ValueError("frame dimensions must be positive")
    return [[round(x / width, 8), round(y / height, 8)] for x, y in points]


def _orientation(a, b, c) -> float:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _segments_intersect(a, b, c, d, *, proper: bool = False) -> bool:
    ab_c, ab_d = _orientation(a, b, c), _orientation(a, b, d)
    cd_a, cd_b = _orientation(c, d, a), _orientation(c, d, b)
    if proper:
        return ab_c * ab_d < 0 and cd_a * cd_b < 0
    epsilon = 1e-10

    def on_segment(p, q, r):
        return (min(p[0], r[0]) - epsilon <= q[0] <= max(p[0], r[0]) + epsilon and
                min(p[1], r[1]) - epsilon <= q[1] <= max(p[1], r[1]) + epsilon)

    return ((ab_c * ab_d < 0 and cd_a * cd_b < 0) or
            (abs(ab_c) <= epsilon and on_segment(a, c, b)) or
            (abs(ab_d) <= epsilon and on_segment(a, d, b)) or
            (abs(cd_a) <= epsilon and on_segment(c, a, d)) or
            (abs(cd_b) <= epsilon and on_segment(c, b, d)))


def _contains_strict(polygon, point) -> bool:
    inside = False
    for index, a in enumerate(polygon):
        b = polygon[(index + 1) % len(polygon)]
        if abs(_orientation(a, b, point)) < 1e-10 and (
            min(a[0], b[0]) <= point[0] <= max(a[0], b[0]) and
            min(a[1], b[1]) <= point[1] <= max(a[1], b[1])
        ):
            return False
        if (a[1] > point[1]) != (b[1] > point[1]):
            crossing_x = (b[0] - a[0]) * (point[1] - a[1]) / (b[1] - a[1]) + a[0]
            if point[0] < crossing_x:
                inside = not inside
    return inside


def validate_zone_geometry(zones: dict[str, list[list[float]]]) -> None:
    polygons = {}
    for name in ZONE_NAMES:
        points = zones[name]
        if len(points) < 3:
            raise ValueError(f"{name} needs at least three vertices")
        if any(not (0 <= x <= 1 and 0 <= y <= 1) for x, y in points):
            raise ValueError(f"{name} vertices must be normalized to [0,1]")
        area = sum(
            points[index][0] * points[(index + 1) % len(points)][1] -
            points[(index + 1) % len(points)][0] * points[index][1]
            for index in range(len(points))
        )
        if abs(area) < 1e-8:
            raise ValueError(f"{name} polygon has zero area")
        for first in range(len(points)):
            for second in range(first + 1, len(points)):
                if second in (first, first + 1) or (first == 0 and second == len(points) - 1):
                    continue
                if _segments_intersect(
                    points[first], points[(first + 1) % len(points)],
                    points[second], points[(second + 1) % len(points)],
                ):
                    raise ValueError(f"{name} polygon self-intersects")
        polygons[name] = points
    for left_index, left_name in enumerate(ZONE_NAMES):
        left = polygons[left_name]
        for right_name in ZONE_NAMES[left_index + 1:]:
            right = polygons[right_name]
            if set(map(tuple, left)) == set(map(tuple, right)):
                raise ValueError(f"{left_name} and {right_name} overlap")
            if any(_contains_strict(right, point) for point in left) or any(
                _contains_strict(left, point) for point in right
            ):
                raise ValueError(f"{left_name} and {right_name} overlap")
            for left_edge, a in enumerate(left):
                b = left[(left_edge + 1) % len(left)]
                for right_edge, c in enumerate(right):
                    d = right[(right_edge + 1) % len(right)]
                    if _segments_intersect(a, b, c, d, proper=True):
                        raise ValueError(f"{left_name} and {right_name} overlap")


def build_config(*, video: Path, camera_id: str, session_id: str,
                 reviewed_frames: list[dict[str, int | float]], width: int,
                 height: int, zones: dict[str, list[tuple[int, int]]],
                 confirmed: bool) -> dict:
    if set(zones) != set(ZONE_NAMES):
        raise ValueError("zones must contain inside, transition, and outside")
    if any(len(zones[name]) < 3 for name in ZONE_NAMES):
        raise ValueError("each zone must have at least three vertices")
    normalized = {name: normalized_points(zones[name], width, height) for name in ZONE_NAMES}
    validate_zone_geometry(normalized)
    return {
        "schema_version": 1,
        "camera_id": camera_id,
        "session_id": session_id,
        "video": str(video),
        "source_video_sha256": sha256(video),
        "calibration": {
            "source_width": width,
            "source_height": height,
            "reviewed_frames": reviewed_frames,
        },
        "geometry_confirmed": confirmed,
        "zones": normalized,
        "max_observation_gap_ms": 400,
        "merge_window_ms": 1000,
        "min_track_score": 0.1,
        "tracker": {
            "track_low_thresh": 0.1,
            "track_high_thresh": 0.6,
            "new_track_thresh": 0.7,
            "match_thresh": 0.8,
            "track_buffer": 30,
        },
    }


def read_samples(video: Path, sample_count: int) -> list[tuple[object, int, float]]:
    capture = open_video(video)
    if not capture.isOpened():
        raise RuntimeError(f"cannot open video: {video}")
    try:
        frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        if frame_count <= 0:
            raise RuntimeError(f"video has no readable frame count: {video}")
        count = min(frame_count, sample_count)
        indices = sorted({round(i * (frame_count - 1) / max(count - 1, 1)) for i in range(count)})
        samples = []
        for target in indices:
            capture.set(cv2.CAP_PROP_POS_FRAMES, target)
            ok, image = capture.read()
            if not ok:
                raise RuntimeError(f"cannot read review frame {target} from {video}")
            actual = max(0, int(capture.get(cv2.CAP_PROP_POS_FRAMES)) - 1)
            samples.append((image, actual, float(capture.get(cv2.CAP_PROP_POS_MSEC))))
        return samples
    finally:
        capture.release()


def draw(image, zones, current: str, display_scale: float, sample_index: int,
         sample_count: int, pts_ms: float):
    canvas = image.copy()
    for name in ZONE_NAMES:
        points = zones[name]
        if not points:
            continue
        color = COLORS[name]
        array = np.asarray(points, dtype=np.int32)
        for index, point in enumerate(points):
            cv2.circle(canvas, point, 5, color, -1)
            cv2.putText(canvas, str(index + 1), (point[0] + 7, point[1] - 7),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv2.LINE_AA)
        if len(points) >= 2:
            cv2.polylines(canvas, [array], len(points) >= 3, color, 2, cv2.LINE_AA)
        cv2.putText(canvas, name, (points[0][0] + 10, points[0][1] + 22),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv2.LINE_AA)
    help_text = (f"sample={sample_index + 1}/{sample_count} pts={pts_ms:.1f}ms current={current} | "
                 "click:add 1-3:zone n:next u:undo r:reset [ ]:frame y:confirm q:quit")
    cv2.rectangle(canvas, (0, 0), (canvas.shape[1], 35), (0, 0, 0), -1)
    cv2.putText(canvas, help_text, (8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                (255, 255, 255), 1, cv2.LINE_AA)
    if display_scale != 1:
        canvas = cv2.resize(canvas, None, fx=display_scale, fy=display_scale, interpolation=cv2.INTER_AREA)
    return canvas


def calibrate(samples, display_scale: float, width: int, height: int):
    zones = {name: [] for name in ZONE_NAMES}
    state = {"zone": 0, "sample": 0}
    visited: set[int] = set()
    window = "flow_demo zone calibration"

    def on_mouse(event, x, y, _flags, _param):
        if event == cv2.EVENT_LBUTTONDOWN:
            scale = display_scale or 1.0
            zones[ZONE_NAMES[state["zone"]]].append((round(x / scale), round(y / scale)))

    cv2.namedWindow(window, cv2.WINDOW_AUTOSIZE)
    cv2.setMouseCallback(window, on_mouse)
    while True:
        image, _, pts_ms = samples[state["sample"]]
        visited.add(state["sample"])
        cv2.imshow(window, draw(image, zones, ZONE_NAMES[state["zone"]], display_scale,
                                state["sample"], len(samples), pts_ms))
        key = cv2.waitKey(30) & 0xFF
        if key in (ord("q"), 27):
            cv2.destroyWindow(window)
            return None
        if key in (ord("1"), ord("2"), ord("3")):
            state["zone"] = key - ord("1")
        elif key == ord("n"):
            current = ZONE_NAMES[state["zone"]]
            if len(zones[current]) < 3:
                print(f"{current} needs at least three points", file=sys.stderr)
            elif state["zone"] < len(ZONE_NAMES) - 1:
                state["zone"] += 1
        elif key == ord("u"):
            current = ZONE_NAMES[state["zone"]]
            if zones[current]:
                zones[current].pop()
        elif key == ord("r"):
            zones[ZONE_NAMES[state["zone"]]].clear()
        elif key == ord("["):
            state["sample"] = max(0, state["sample"] - 1)
        elif key == ord("]"):
            state["sample"] = min(len(samples) - 1, state["sample"] + 1)
        elif key == ord("y"):
            if all(len(zones[name]) >= 3 for name in ZONE_NAMES) and len(visited) == len(samples):
                candidate = {name: normalized_points(zones[name], width, height) for name in ZONE_NAMES}
                try:
                    validate_zone_geometry(candidate)
                except ValueError as error:
                    print(f"invalid zone geometry: {error}", file=sys.stderr)
                else:
                    cv2.destroyWindow(window)
                    reviewed = [{"frame_index": frame, "pts_ms": round(pts, 3)} for _, frame, pts in samples]
                    return zones, reviewed
            else:
                print("draw all zones and review every sampled frame before confirming", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--camera-id", required=True)
    parser.add_argument("--session-id", default="")
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--display-scale", type=float, default=1.0)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if not args.video.is_file():
        raise SystemExit(f"video not found: {args.video}")
    if args.samples < 2 or args.display_scale <= 0:
        raise SystemExit("--samples must be at least 2 and --display-scale must be positive")
    if args.output.exists() and not args.force:
        raise SystemExit(f"refusing to overwrite {args.output}; use --force")
    samples = read_samples(args.video, args.samples)
    height, width = samples[0][0].shape[:2]
    if any(image.shape[:2] != (height, width) for image, _, _ in samples):
        raise SystemExit("video changes frame dimensions; calibrate a constant-resolution view")
    calibration = calibrate(samples, args.display_scale, width, height)
    if calibration is None:
        print("calibration cancelled; no file written", file=sys.stderr)
        return 1
    zones, reviewed_frames = calibration
    config = build_config(
        video=args.video, camera_id=args.camera_id,
        session_id=args.session_id or args.camera_id, reviewed_frames=reviewed_frames,
        width=width, height=height, zones=zones, confirmed=True,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()), "camera_id": args.camera_id,
                      "reviewed_frames": len(reviewed_frames), "source_width": width,
                      "source_height": height, "geometry_confirmed": True}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
