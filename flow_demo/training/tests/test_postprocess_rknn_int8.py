from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from postprocess_rknn_int8 import (  # noqa: E402
    HEADS,
    Detection,
    _decode_head,
    decode_outputs,
    dequantize,
    map_to_source,
    nms,
)


def _empty_outputs() -> list[np.ndarray]:
    outputs: list[np.ndarray] = []
    for head in HEADS:
        outputs.append(np.full((64, head.size, head.size), head.box_zero_point, dtype=np.int8))
        outputs.append(np.full((1, head.size, head.size), head.score_zero_point, dtype=np.int8))
    return outputs


def test_dequantize_uses_affine_zero_point_and_scale() -> None:
    quantized = np.array([-128, -127, 0, 127], dtype=np.int8)
    actual = dequantize(quantized, -128, 0.25)
    np.testing.assert_allclose(actual, [0.0, 0.25, 32.0, 63.75])


def test_dfl_uniform_logits_decode_to_midpoint_distances() -> None:
    head = HEADS[0]
    box = np.full((64, 80, 80), head.box_zero_point, dtype=np.int8)
    score = np.full((1, 80, 80), head.score_zero_point, dtype=np.int8)
    boxes, _ = _decode_head(box, score, head)
    # At grid cell (x=2, y=3), uniform 16-bin distributions have distance 7.5.
    np.testing.assert_allclose(boxes[3 * 80 + 2], [-40.0, -32.0, 80.0, 88.0], atol=1e-5)


def test_decode_applies_confidence_and_nms_across_heads() -> None:
    outputs = _empty_outputs()
    # Adjacent stride-8 cells have heavily overlapping uniform-DFL boxes.
    outputs[1][0, 3, 2] = 120  # dequantizes to about 0.829
    outputs[1][0, 3, 3] = 100  # dequantizes to about 0.762
    # A low-confidence score at the coarsest head is below the configured floor.
    outputs[5][0, 0, 0] = -127

    detections = decode_outputs(outputs, conf_threshold=0.5, iou_threshold=0.7, max_det=300)

    assert len(detections) == 1
    assert detections[0].score == pytest.approx((120 + 128) * HEADS[0].score_scale)
    assert detections[0].class_id == 0


def test_map_to_source_removes_letterbox_and_clips() -> None:
    detection = Detection((-10.0, 20.0, 90.0, 120.0), 0.8)
    manifest = {
        "source_width": 100,
        "source_height": 50,
        "scale": 2.0,
        "pad_left": 0,
        "pad_top": 10,
    }
    mapped = map_to_source(detection, manifest)
    assert mapped.xyxy == (0.0, 5.0, 45.0, 50.0)


def test_nms_rejects_malformed_inputs_and_keeps_separate_boxes() -> None:
    with pytest.raises(ValueError):
        nms(np.zeros((1, 4), dtype=np.float32), np.zeros((2,), dtype=np.float32))
    boxes = np.array([[0, 0, 10, 10], [20, 20, 30, 30]], dtype=np.float32)
    assert nms(boxes, np.array([0.9, 0.8], dtype=np.float32)) == [0, 1]
