from __future__ import annotations

import json
from pathlib import Path

import pytest

from tracking.session_config import build_session_config


def test_session_config_reuses_geometry_but_records_actual_input(tmp_path: Path, monkeypatch) -> None:
    import tracking.session_config as session_config

    profile = tmp_path / "camera.json"
    video = tmp_path / "session.mp4"
    video.write_bytes(b"video")
    config = {
        "camera_id": "left-corner",
        "session_id": "calibration-session",
        "video": "calibration.mp4",
        "geometry_confirmed": True,
        "calibration": {"source_width": 1920, "source_height": 1080},
        "zones": {
            "inside": [[0, 0], [0.2, 0], [0.2, 1]],
            "transition": [[0.2, 0], [0.6, 0], [0.6, 1]],
            "outside": [[0.6, 0], [1, 0], [1, 1]],
        },
    }
    profile.write_text(json.dumps(config), encoding="utf-8")
    monkeypatch.setattr(session_config, "read_video_size", lambda _video: (1280, 720))

    run_config = build_session_config(profile, video, "run-002")

    assert run_config["camera_id"] == "left-corner"
    assert run_config["zones"] == config["zones"]
    assert run_config["session_id"] == "run-002"
    assert run_config["video"] == str(video.resolve())
    assert run_config["calibration_video"] == "calibration.mp4"
    assert run_config["runtime"]["video_width"] == 1280
    json.dumps(run_config)


def test_session_config_rejects_unconfirmed_profile(tmp_path: Path) -> None:
    profile = tmp_path / "draft.json"
    profile.write_text(json.dumps({"geometry_confirmed": False}), encoding="utf-8")
    video = tmp_path / "session.mp4"
    video.touch()

    with pytest.raises(ValueError, match="geometry_confirmed=true"):
        build_session_config(profile, video, "run-003")


def test_session_config_rejects_changed_aspect_ratio(tmp_path: Path, monkeypatch) -> None:
    import tracking.session_config as session_config

    profile = tmp_path / "camera.json"
    profile.write_text(json.dumps({
        "geometry_confirmed": True,
        "calibration": {"source_width": 1920, "source_height": 1080},
        "zones": {
            "inside": [[0, 0], [0.2, 0], [0.2, 1]],
            "transition": [[0.2, 0], [0.6, 0], [0.6, 1]],
            "outside": [[0.6, 0], [1, 0], [1, 1]],
        },
    }), encoding="utf-8")
    video = tmp_path / "session.mp4"
    video.touch()
    monkeypatch.setattr(session_config, "read_video_size", lambda _video: (1280, 1024))

    with pytest.raises(ValueError, match="aspect ratio"):
        build_session_config(profile, video, "run-004")
