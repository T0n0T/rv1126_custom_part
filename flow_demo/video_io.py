"""OpenCV video helpers shared by calibration, inference, and review tools."""
from __future__ import annotations

from pathlib import Path

import cv2


def open_video(path: str | Path):
    """Open a video and ask OpenCV to apply its container display orientation."""
    capture = cv2.VideoCapture(str(path))
    orientation_auto = getattr(cv2, "CAP_PROP_ORIENTATION_AUTO", None)
    if capture.isOpened() and orientation_auto is not None:
        capture.set(orientation_auto, 1)
    return capture
