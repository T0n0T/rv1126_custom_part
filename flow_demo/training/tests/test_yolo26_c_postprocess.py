from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_yolo26_board import topk_yolo26


@pytest.fixture(scope="module")
def program(tmp_path_factory) -> Path:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("C compiler unavailable")
    source = Path(__file__).resolve().parents[2] / "board/rknn_yolo26_postprocess.c"
    target = tmp_path_factory.mktemp("yolo26-c") / "postprocess"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(source), "-o", str(target), "-lm"], check=True)
    return target


def test_c_matches_python_with_quantization_ties_and_both_classes(program: Path, tmp_path: Path) -> None:
    rng = np.random.default_rng(17)
    scores = rng.integers(-128, 128, (1, 2, 8400), dtype=np.int8)
    boxes = rng.integers(-100, 100, (1, 4, 8400), dtype=np.int8)
    scores.tofile(tmp_path / "output-0.bin")
    boxes.tofile(tmp_path / "output-1.bin")
    expected = topk_yolo26((scores.astype(np.float32) + 128) * np.float32(1 / 255),
                          (boxes.astype(np.float32) + 3) * np.float32(2.5))[0]
    command = [str(program), "--dump-dir", str(tmp_path), "--score-zp", "-128", "--score-scale", str(1 / 255),
               "--box-zp", "-3", "--box-scale", "2.5", "--runs", "1"]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    rows = json.loads(result.stdout)["detections"]
    actual = np.array([row["xyxy"] + [row["score"], row["class_id"]] for row in rows])
    np.testing.assert_allclose(actual, expected, atol=1e-5, rtol=1e-7)
    assert set(actual[:, 5]) == {0, 1}
    assert "YOLO26_POSTPROCESS_OK" in result.stderr


def test_c_rejects_incomplete_dump(program: Path, tmp_path: Path) -> None:
    (tmp_path / "output-0.bin").write_bytes(b"\0" * 42)
    result = subprocess.run([str(program), "--dump-dir", str(tmp_path), "--score-zp", "-128", "--score-scale", "0.004",
                             "--box-zp", "0", "--box-scale", "2"], capture_output=True, text=True)
    assert result.returncode == 1
    assert "invalid tensor byte count" in result.stderr
    assert result.stdout == ""
