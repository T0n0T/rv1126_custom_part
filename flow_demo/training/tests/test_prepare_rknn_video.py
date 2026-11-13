from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import prepare_rknn_video as module  # noqa: E402


class FakeCapture:
    def __init__(self, _path: str, frames: list[np.ndarray]):
        self.frames = iter(frames)

    def isOpened(self) -> bool:
        return True

    def set(self, _prop: int, _value: float) -> bool:
        return True

    def read(self) -> tuple[bool, np.ndarray | None]:
        frame = next(self.frames, None)
        return (frame is not None, frame)

    def release(self) -> None:
        pass


def test_letterbox_uses_rgb_compatible_padding_and_source_geometry() -> None:
    frame = np.zeros((2, 4, 3), dtype=np.uint8)
    boxed, transform = module.letterbox_bgr(frame, size=4)
    assert boxed.shape == (4, 4, 3)
    assert transform == {
        "scale": 1.0,
        "pad_left": 0,
        "pad_top": 1,
        "resized_width": 4,
        "resized_height": 2,
        "source_width": 4,
        "source_height": 2,
    }
    assert tuple(boxed[0, 0]) == (114, 114, 114)


def test_video_cli_writes_rgb_stream_pts_metadata_and_hash(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    video = tmp_path / "video.mp4"
    video.write_bytes(b"test video bytes")
    frames = tmp_path / "frames.rgb"
    metadata = tmp_path / "frames.tsv"
    source = np.zeros((2, 4, 3), dtype=np.uint8)
    source[:] = (0, 0, 255)  # BGR red; output stream must contain RGB red.
    monkeypatch.setattr(module, "source_pts", lambda _video: [123.5, 200.0])
    monkeypatch.setattr(module.cv2, "VideoCapture", lambda path: FakeCapture(path, [source]))
    monkeypatch.setattr(sys, "argv", ["prepare_rknn_video.py", "--video", str(video),
        "--frames", str(frames), "--metadata", str(metadata), "--max-frames", "1"])

    assert module.main() == 0
    raw = frames.read_bytes()
    assert len(raw) == 640 * 640 * 3
    assert tuple(raw[(160 * 640) * 3:(160 * 640) * 3 + 3]) == (255, 0, 0)
    assert metadata.read_text().strip() == "0 123.500000 4 2 160 0 160"
    manifest = json.loads(frames.with_suffix(".rgb.json").read_text())
    assert manifest["frames_rgb_sha256"] == hashlib.sha256(raw).hexdigest()
    assert manifest["partial"] is True

    with pytest.raises(FileExistsError):
        module.main()
