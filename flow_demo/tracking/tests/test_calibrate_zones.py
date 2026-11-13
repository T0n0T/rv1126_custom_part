from __future__ import annotations

import json
from pathlib import Path

import pytest

from tracking.calibrate_zones import build_config, normalized_points, read_samples, validate_zone_geometry


def test_normalized_points_are_resolution_independent() -> None:
    assert normalized_points([(0, 0), (960, 540), (1920, 1080)], 1920, 1080) == [
        [0.0, 0.0], [0.5, 0.5], [1.0, 1.0]
    ]


def test_build_config_records_source_and_confirmation(tmp_path: Path) -> None:
    video = tmp_path / "camera.mp4"
    video.write_bytes(b"fixture")
    zones = {
        "inside": [(0, 0), (300, 0), (300, 1080)],
        "transition": [(300, 0), (900, 0), (900, 1080)],
        "outside": [(900, 0), (1920, 0), (1920, 1080)],
    }
    config = build_config(
        video=video, camera_id="left-corner", session_id="session-001",
        reviewed_frames=[{"frame_index": 42, "pts_ms": 1400.25}], width=1920, height=1080,
        zones=zones, confirmed=True,
    )
    assert config["geometry_confirmed"] is True
    assert config["camera_id"] == "left-corner"
    assert config["calibration"]["source_width"] == 1920
    assert config["calibration"]["reviewed_frames"][0]["frame_index"] == 42
    assert config["zones"]["transition"][0] == [0.15625, 0.0]
    assert len(config["source_video_sha256"]) == 64
    json.dumps(config, ensure_ascii=False)


def test_build_config_requires_three_zones(tmp_path: Path) -> None:
    video = tmp_path / "camera.mp4"
    video.write_bytes(b"fixture")
    with pytest.raises(ValueError, match="at least three"):
        build_config(
            video=video, camera_id="left", session_id="session",
            reviewed_frames=[{"frame_index": 0, "pts_ms": 0}], width=10, height=10,
            zones={
                "inside": [(0, 0), (1, 0)],
                "transition": [(2, 0), (3, 0)],
                "outside": [(4, 0), (5, 0)],
            }, confirmed=False,
        )


def test_geometry_rejects_overlapping_and_self_intersecting_polygons() -> None:
    with pytest.raises(ValueError, match="overlap"):
        validate_zone_geometry({
            "inside": [[0, 0], [0.6, 0], [0.6, 1], [0, 1]],
            "transition": [[0.5, 0.2], [0.8, 0.2], [0.8, 0.8], [0.5, 0.8]],
            "outside": [[0.8, 0], [1, 0], [1, 1], [0.8, 1]],
        })


def test_read_samples_spans_video_and_records_actual_frame_positions(monkeypatch) -> None:
    import tracking.calibrate_zones as calibration
    import numpy as np

    class FakeCapture:
        def __init__(self, _path: str):
            self.position = 0

        def isOpened(self):
            return True

        def get(self, prop):
            if prop == calibration.cv2.CAP_PROP_FRAME_COUNT:
                return 100
            if prop == calibration.cv2.CAP_PROP_POS_FRAMES:
                return self.position
            if prop == calibration.cv2.CAP_PROP_POS_MSEC:
                return self.position * 40
            raise AssertionError(prop)

        def set(self, _prop, position):
            self.position = int(position)

        def read(self):
            self.position += 1
            return True, np.zeros((4, 8, 3), dtype=np.uint8)

        def release(self):
            pass

    monkeypatch.setattr(calibration.cv2, "VideoCapture", FakeCapture)
    samples = read_samples(Path("unused.mp4"), sample_count=4)

    assert [sample[1] for sample in samples] == [0, 33, 66, 99]
    assert [sample[2] for sample in samples] == [40, 1360, 2680, 4000]


def test_confirmation_requires_reviewing_each_sampled_frame(monkeypatch, capsys) -> None:
    import tracking.calibrate_zones as calibration
    import numpy as np

    mouse = {}
    monkeypatch.setattr(calibration.cv2, "namedWindow", lambda *_args: None)
    monkeypatch.setattr(calibration.cv2, "setMouseCallback", lambda _name, callback: mouse.setdefault("callback", callback))
    monkeypatch.setattr(calibration.cv2, "imshow", lambda *_args: None)
    monkeypatch.setattr(calibration.cv2, "destroyWindow", lambda *_args: None)
    clicks = [
        [(0, 0), (30, 0), (30, 99), (0, 99)],
        [(30, 0), (60, 0), (60, 99), (30, 99)],
        [(60, 0), (99, 0), (99, 99), (60, 99)],
        [], [], [],
    ]
    keys = iter([ord("n"), ord("n"), ord("n"), ord("y"), ord("]"), ord("y")])

    def wait_key(_delay):
        for x, y in clicks.pop(0):
            mouse["callback"](calibration.cv2.EVENT_LBUTTONDOWN, x, y, 0, None)
        return next(keys)

    monkeypatch.setattr(calibration.cv2, "waitKey", wait_key)
    samples = [(np.zeros((100, 100, 3), dtype=np.uint8), index * 10, index * 33.3)
               for index in range(2)]

    result = calibration.calibrate(samples, display_scale=1.0, width=100, height=100)

    assert result is not None
    zones, reviewed = result
    assert all(len(zones[name]) == 4 for name in calibration.ZONE_NAMES)
    assert [frame["frame_index"] for frame in reviewed] == [0, 10]
    assert "review every sampled frame" in capsys.readouterr().err
    with pytest.raises(ValueError, match="self-intersects|zero area"):
        validate_zone_geometry({
            "inside": [[0, 0], [0.3, 1], [0.3, 0], [0, 1]],
            "transition": [[0.3, 0], [0.6, 0], [0.6, 1], [0.3, 1]],
            "outside": [[0.6, 0], [1, 0], [1, 1], [0.6, 1]],
        })
