from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from postprocess_rknn_int8 import HEADS, Head, _decode_head, load_dump, nms


@pytest.fixture(scope="module")
def program(tmp_path_factory) -> Path:
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("C compiler unavailable")
    root = Path(__file__).resolve().parents[2]
    sdk_include = root.parent.parent / "project/app/rkai/rkai/common/include"
    target = tmp_path_factory.mktemp("live-yolov8-c") / "decode"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    f"-I{root / 'board'}", f"-I{sdk_include}",
                    str(root / "board/tests/yolov8_decode_fixture.c"),
                    str(root / "board/flow_yolov8.c"), "-o", str(target), "-lm"], check=True)
    return target


def run_decoder(program, tmp_path, outputs, heads=HEADS, source=(640, 360, 1, 0, 140), conf=0.5, bad=0):
    for i, output in enumerate(outputs):
        output.tofile(tmp_path / f"output-{i}.bin")
    attrs = tmp_path / "attrs.tsv"
    attrs.write_text("".join(f"{h.box_zero_point} {h.box_scale} {h.score_zero_point} {h.score_scale}\n" for h in heads))
    return subprocess.run([str(program), str(tmp_path), str(attrs), *map(str, source),
                           str(conf), "0.7", str(bad)], text=True, capture_output=True)


def reference(outputs, heads, source, conf):
    boxes, scores = [], []
    for i, head in enumerate(heads):
        b, s = _decode_head(outputs[2 * i], outputs[2 * i + 1], head)
        boxes.append(b[s >= conf]); scores.append(s[s >= conf])
    boxes, scores = np.concatenate(boxes), np.concatenate(scores)
    keep = nms(boxes, scores, iou_threshold=0.7, max_det=300)
    width, height, scale, left, top = source
    rows = []
    for i in keep:
        b = np.clip((boxes[i].astype(float) - [left, top, left, top]) / scale / [width, height, width, height], 0, 1)
        if b[2] > b[0] and b[3] > b[1]:
            rows.append([*b, scores[i], 0])
    return np.asarray(rows).reshape(-1, 6)


def arrays(rows):
    return np.asarray([row["xyxy"] + [row["score"], row["class_id"]] for row in rows]).reshape(-1, 6)


@pytest.mark.parametrize("source", [(640, 360, 1, 0, 140), (360, 640, 1, 140, 0), (1280, 720, 0.5, 0, 140)])
def test_live_decoder_matches_numpy_with_queried_quantization(program, tmp_path, source):
    # Different zero points/scales from the historical model catch accidental constants.
    heads = tuple(Head(h.stride, h.size, -30 + i, 0.1 + i * 0.03, -110 + i, 0.004 + i * 0.0001)
                  for i, h in enumerate(HEADS))
    rng = np.random.default_rng(27)
    outputs = []
    for h in heads:
        boxes = rng.integers(-45, 40, (64, h.size, h.size), dtype=np.int8)
        scores = np.full((1, h.size, h.size), h.score_zero_point, dtype=np.int8)
        for y, x in [(h.size // 2, h.size // 2), (h.size // 2, h.size // 2 + 1), (0, 0), (h.size - 1, h.size - 1)]:
            scores[0, y, x] = 100
        outputs.extend((boxes, scores))
    expected = reference(outputs, heads, source, 0.5)
    result = run_decoder(program, tmp_path, outputs, heads, source)
    assert result.returncode == 0, result.stderr
    np.testing.assert_allclose(arrays(json.loads(result.stdout)), expected, atol=2e-6, rtol=1e-6)


def test_live_decoder_matches_saved_board_dump(program, tmp_path):
    dump = Path(__file__).resolve().parents[2] / "results/rknn-dump-20261111/dump-int8"
    if not dump.exists():
        pytest.skip("historical board tensors unavailable")
    outputs = load_dump(dump)
    source = (640, 640, 1, 0, 0)
    result = run_decoder(program, tmp_path, outputs, source=source, conf=0.001)
    assert result.returncode == 0, result.stderr
    expected = reference(outputs, HEADS, source, 0.001)
    assert len(expected) > 0
    np.testing.assert_allclose(arrays(json.loads(result.stdout)), expected, atol=2e-6, rtol=1e-6)


def empty_outputs():
    outputs = []
    for h in HEADS:
        outputs.extend((np.full((64, h.size, h.size), h.box_zero_point, np.int8),
                        np.full((1, h.size, h.size), h.score_zero_point, np.int8)))
    return outputs


def test_empty_frame_is_valid(program, tmp_path):
    result = run_decoder(program, tmp_path, empty_outputs())
    assert result.returncode == 0
    assert json.loads(result.stdout) == []


@pytest.mark.parametrize("bad", [1, 2, 3, 4, 5])
def test_rejects_unsupported_tensor_contract(program, tmp_path, bad):
    result = run_decoder(program, tmp_path, empty_outputs(), bad=bad)
    assert result.returncode == 1
    assert not result.stdout


def test_rejects_incomplete_output(program, tmp_path):
    outputs = empty_outputs()
    outputs[4] = outputs[4].ravel()[:-1]
    result = run_decoder(program, tmp_path, outputs)
    assert result.returncode == 1
    assert not result.stdout
