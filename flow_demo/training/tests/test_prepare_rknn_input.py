from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from prepare_rknn_input import letterbox, main  # noqa: E402


def test_letterbox_records_geometry_for_source_coordinates() -> None:
    image = Image.new("RGB", (2, 4), (255, 0, 0))
    result, metadata = letterbox(image, size=4, padding=114)
    assert result.size == (4, 4)
    assert metadata["scale"] == 1.0
    assert metadata["pad_left"] == 1
    assert metadata["pad_top"] == 0


def test_cli_records_hash_of_exact_raw_rgb_input(monkeypatch) -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        source = root / "source.png"
        output = root / "sample.rgb"
        manifest_path = root / "sample.rgb.json"
        Image.new("RGB", (2, 4), (12, 34, 56)).save(source)
        monkeypatch.setattr(
            sys,
            "argv",
            ["prepare_rknn_input.py", "--image", str(source), "--output", str(output), "--size", "4"],
        )

        assert main() == 0

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        raw = output.read_bytes()
        assert len(raw) == 4 * 4 * 3
        assert manifest["input_sha256"] == hashlib.sha256(raw).hexdigest()
        assert manifest["source_width"] == 2
        assert manifest["source_height"] == 4
