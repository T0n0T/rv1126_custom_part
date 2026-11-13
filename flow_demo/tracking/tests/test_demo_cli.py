from __future__ import annotations
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="flow-demo-cli-") as temporary:
        root = Path(temporary)
        config = {
            "session_id": "cli-integration",
            "video": "synthetic-session.mp4",
            "geometry_confirmed": True,
            "zones": {
                "inside": [[0, 0], [0.3, 0], [0.3, 1], [0, 1]],
                "transition": [[0.3, 0], [0.6, 0], [0.6, 1], [0.3, 1]],
                "outside": [[0.6, 0], [1, 0], [1, 1], [0.6, 1]],
            },
            "max_observation_gap_ms": 400,
            "merge_window_ms": 1000,
            "min_track_score": 0.1,
            "tracker": {"track_low_thresh": 0.1, "track_high_thresh": 0.6,
                        "new_track_thresh": 0.7, "match_thresh": 0.8, "track_buffer": 30},
        }
        (root / "zones.json").write_text(json.dumps(config))
        positions = [0.15 + 0.01 * index for index in range(66)]
        with (root / "detections.jsonl").open("w") as detections:
            for frame, center in enumerate(positions):
                detections.write(json.dumps({"frame_index": frame, "pts_ms": frame * 100,
                    "detections": [{"xyxy": [center - 0.04, 0.4, center + 0.04, 0.6],
                                    "score": 0.95, "class_id": 0}]}) + "\n")
        result = subprocess.run([str(binary), "--config", str(root / "zones.json"),
            "--detections", str(root / "detections.jsonl"), "--tracks", str(root / "tracks.jsonl"),
            "--events", str(root / "events.jsonl"), "--summary", str(root / "summary.json")],
            check=True, capture_output=True, text=True)
        assert "FLOW_DEMO_OK" in result.stdout
        summary = json.loads((root / "summary.json").read_text())
        assert summary["confirmed_take_out"] == 1
        assert summary["confirmed_put_in"] == 0
        assert summary["uncertain_count"] == 0
        assert summary["result_complete"] is True
        events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
        assert len(events) == 1
        assert events[0]["direction"] == "take_out"
        assert events[0]["track_ids"] == [1]
        tracks = [json.loads(line) for line in (root / "tracks.jsonl").read_text().splitlines()]
        assert len(tracks) == len(positions)
        assert all(frame["detections"] for frame in tracks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
