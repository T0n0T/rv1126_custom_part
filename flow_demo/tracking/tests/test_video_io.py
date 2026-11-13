from __future__ import annotations

from pathlib import Path

import cv2
import numpy as np

from video_io import open_video


class OrientationAwareCapture:
    def __init__(self, frame: np.ndarray) -> None:
        self.frame = frame
        self.orientation_auto = 0
        self.operations: list[tuple[str, int | float]] = []

    def isOpened(self) -> bool:
        return True

    def set(self, prop: int, value: float) -> bool:
        self.operations.append(("set", prop))
        if prop == cv2.CAP_PROP_ORIENTATION_AUTO:
            self.orientation_auto = int(value)
        return True

    def read(self) -> tuple[bool, np.ndarray]:
        self.operations.append(("read", 0))
        if self.orientation_auto:
            return True, self.frame[::-1, ::-1].copy()
        return True, self.frame.copy()


def test_open_video_applies_rotation_metadata_before_read(monkeypatch) -> None:
    source = np.arange(4 * 6 * 3, dtype=np.uint8).reshape(4, 6, 3)
    capture = OrientationAwareCapture(source)
    monkeypatch.setattr(cv2, "VideoCapture", lambda _path: capture)

    opened = open_video(Path("rotated-by-metadata.mp4"))
    ok, frame = opened.read()

    assert ok
    assert capture.operations[:2] == [
        ("set", cv2.CAP_PROP_ORIENTATION_AUTO),
        ("read", 0),
    ]
    np.testing.assert_array_equal(frame, source[::-1, ::-1])
