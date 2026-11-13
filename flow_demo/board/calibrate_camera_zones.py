#!/usr/bin/env python3
"""在板载摄像头的无叠加截图上标定三区，复用既有交互标定器。"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tracking'))
from calibrate_zones import calibrate, cv2, normalized_points, sha256


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--camera-id', default='rv1126b-0')
    parser.add_argument('--rotation', type=int, choices=(0, 90, 180, 270), default=180)
    parser.add_argument('--display-scale', type=float, default=1.0)
    args = parser.parse_args()
    if args.output.exists() or args.display_scale <= 0:
        parser.error('output must not already exist; display-scale must be positive')
    samples = []
    for i, path in enumerate(args.images):
        image = cv2.imread(str(path))
        if image is None:
            parser.error(f'cannot read camera image: {path}')
        if samples and image.shape != samples[0][0].shape:
            parser.error('all images must have the same rotated camera dimensions')
        samples.append((image, i, 0.0))
    height, width = samples[0][0].shape[:2]
    result = calibrate(samples, args.display_scale, width, height)
    if result is None:
        return 1
    zones, _ = result
    value = {
        'camera_id': args.camera_id, 'geometry_confirmed': True,
        'coordinate_space': 'rotated-camera', 'camera_rotation': args.rotation,
        'source_width': width, 'source_height': height,
        'zones': {name: normalized_points(points, width, height) for name, points in zones.items()},
        'calibration_images': [{'path': str(p.resolve()), 'sha256': sha256(p)} for p in args.images],
        'max_observation_gap_ms': 400, 'merge_window_ms': 1000, 'min_track_score': 0.1,
        'tracker': {'track_low_thresh': 0.1, 'track_high_thresh': 0.6, 'new_track_thresh': 0.7,
                    'match_thresh': 0.8, 'track_buffer': 30},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('x', encoding='utf-8') as output:
        output.write(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    print(f'camera profile saved: {args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
