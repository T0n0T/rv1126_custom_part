from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compare_yolo26_board import compare, detections, read_native, topk_yolo26


def test_native_quantization_comes_from_the_runtime_log(tmp_path: Path) -> None:
    raw = np.full((1, 300, 6), -17, dtype=np.int8)
    raw[0, 0] = [-7, 3, 13, 23, -2, 3]
    (tmp_path / "output-0.bin").write_bytes(raw.tobytes())
    log = tmp_path / "smoke.log"
    log.write_text("output[0] name=output0 type=int8 fmt=NCHW qnt=affine "
                   "dims=1x300x6 elems=1800 bytes=1800 zp=-17 scale=0.05\n")
    values, attr = read_native(tmp_path, log)
    np.testing.assert_allclose(values[0, 0], [0.5, 1, 1.5, 2, 0.75, 1])
    assert attr["zero_point"] == -17
    found = detections(values, 0.25)
    assert len(found) == 1
    assert found[0]["class_name"] == "head"
    assert found[0]["score"] == pytest.approx(0.75)


def test_old_dfl_head_dump_is_rejected(tmp_path: Path) -> None:
    log = tmp_path / "smoke.log"
    log.write_text("output[0] name=box_logits_s8 type=int8 fmt=NCHW qnt=affine "
                   "dims=1x64x80x80 elems=409600 bytes=409600 zp=-14 scale=0.395\n")
    with pytest.raises(ValueError, match="end-to-end"):
        read_native(tmp_path, log)


def test_short_native_dump_is_rejected(tmp_path: Path) -> None:
    (tmp_path / "output-0.bin").write_bytes(b"\0" * 10)
    log = tmp_path / "smoke.log"
    log.write_text("output[0] name=output0 type=float16 fmt=NCHW qnt=none "
                   "dims=1x300x6 elems=1800 bytes=3600 zp=0 scale=1\n")
    with pytest.raises(ValueError, match="byte count"):
        read_native(tmp_path, log)


def test_matching_respects_classes_and_does_not_reuse_a_detection() -> None:
    a = {"xyxy": [10, 20, 30, 40], "score": 0.9, "class_id": 0, "class_name": "helmet"}
    b = {**a, "xyxy": [11, 20, 31, 40], "score": 0.8}
    assert compare([a], [{**b, "class_id": 1, "class_name": "head"}]) == []
    matches = compare([a, a], [b])
    assert len(matches) == 1
    assert matches[0]["iou"] == pytest.approx(19 / 21)
    assert matches[0]["max_coordinate_error_px"] == 1


def test_unknown_classes_are_rejected() -> None:
    output = np.zeros((1, 300, 6), dtype=np.float32)
    output[0, 0] = [10, 20, 30, 40, 0.9, 3]
    with pytest.raises(ValueError, match="class id"):
        detections(output, 0.25)


def test_topk_preserves_two_high_scoring_classes_from_one_anchor() -> None:
    scores = np.array([[[0.9, 0.1], [0.8, 0.7]]], dtype=np.float32)
    boxes = np.array([[[10, 100], [20, 200], [30, 300], [40, 400]]], dtype=np.float32)
    result = topk_yolo26(scores, boxes, k=2)
    np.testing.assert_allclose(result[0], [[10, 20, 30, 40, 0.9, 0], [10, 20, 30, 40, 0.8, 1]])
