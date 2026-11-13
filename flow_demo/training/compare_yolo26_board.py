#!/usr/bin/env python3
"""Compare YOLO26 end-to-end board dumps against saved ONNX outputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


ATTR = re.compile(
    r"output\[(\d+)\] name=(.*?) type=(\w+) fmt=(\w+) qnt=(\w+) "
    r"dims=([0-9x]+) elems=(\d+) bytes=(\d+) zp=(-?\d+) scale=([0-9.eE+-]+)"
)
CLASSES = ("helmet", "head")


def read_native(directory: Path, log: Path, index: int = 0,
                expected_shape: tuple[int, ...] = (1, 300, 6)) -> tuple[np.ndarray, dict]:
    match = next((m for m in ATTR.finditer(log.read_text()) if int(m.group(1)) == index), None)
    if match is None:
        raise ValueError(f"no output attribute in {log}")
    _, name, dtype, layout, qnt, dims, elements, size, zero, scale = match.groups()
    shape = tuple(int(v) for v in dims.split("x"))
    if shape != expected_shape:
        raise ValueError(f"expected YOLO26 end-to-end/split shape {expected_shape}, got {shape}")
    raw = (directory / f"output-{index}.bin").read_bytes()
    if len(raw) != int(size):
        raise ValueError(f"output byte count {len(raw)} differs from runtime attribute {size}")
    value = np.frombuffer(raw, dtype=np.dtype(dtype))
    if value.size != int(elements):
        raise ValueError("output element count differs from runtime attribute")
    value = value.reshape(shape).astype(np.float32)
    if qnt == "affine":
        value = (value - int(zero)) * float(scale)
    elif qnt != "none":
        raise ValueError(f"unsupported output quantization {qnt}")
    if not np.isfinite(value).all():
        raise ValueError("output contains nonfinite values")
    return value, {"name": name, "dtype": dtype, "layout": layout, "quantization": qnt,
        "shape": shape, "zero_point": int(zero), "scale": float(scale),
        "sha256": hashlib.sha256(raw).hexdigest()}


def topk_yolo26(scores: np.ndarray, boxes: np.ndarray, k: int = 300) -> np.ndarray:
    """Reproduce both TopK stages in the original ONNX, including both classes."""
    if scores.ndim != 3 or scores.shape[:2] != (1, 2) or boxes.shape != (1, 4, scores.shape[2]):
        raise ValueError("expected scores [1,2,N] and xyxy boxes [1,4,N]")
    if k <= 0 or k > scores.shape[2]:
        raise ValueError("TopK must be in [1,N]")
    if not np.isfinite(scores).all() or not np.isfinite(boxes).all():
        raise ValueError("nonfinite split outputs")
    anchors = np.argsort(-scores[0].max(axis=0), kind="stable")[:k]
    candidates = scores[0][:, anchors].T.reshape(-1)
    selected = np.argsort(-candidates, kind="stable")[:k]
    anchor_ids = anchors[selected // 2]
    return np.concatenate((boxes[0][:, anchor_ids].T, candidates[selected, None],
                           (selected % 2)[:, None].astype(np.float32)), axis=1)[None]


def detections(value: np.ndarray, threshold: float) -> list[dict]:
    result = []
    for row in value[0]:
        if row[4] < threshold:
            continue
        class_id = int(round(float(row[5])))
        if class_id not in (0, 1) or abs(float(row[5]) - class_id) > 0.01:
            raise ValueError(f"invalid class id: {row[5]}")
        result.append({"xyxy": row[:4].tolist(), "score": float(row[4]), "class_id": class_id, "class_name": CLASSES[class_id]})
    return result


def compare(reference: list[dict], candidate: list[dict]) -> list[dict]:
    matches = []
    available = set(range(len(candidate)))
    for ref in reference:
        choices = []
        a = np.asarray(ref["xyxy"])
        for i in available:
            if candidate[i]["class_id"] != ref["class_id"]:
                continue
            b = np.asarray(candidate[i]["xyxy"])
            intersection = np.maximum(np.minimum(a[2:], b[2:]) - np.maximum(a[:2], b[:2]), 0).prod()
            union = np.maximum(a[2:] - a[:2], 0).prod() + np.maximum(b[2:] - b[:2], 0).prod() - intersection
            iou = float(intersection / union) if union > 0 else 0.0
            choices.append((iou, i))
        if choices:
            iou, i = max(choices)
            if iou >= 0.5:
                available.remove(i)
                matches.append({"class_name": ref["class_name"], "iou": iou,
                    "max_coordinate_error_px": float(np.max(np.abs(a - candidate[i]["xyxy"]))),
                    "score_difference": candidate[i]["score"] - ref["score"]})
    return matches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--conf", type=float, default=0.25)
    parser.add_argument("--board-dir", default="board")
    parser.add_argument("--split", action="store_true")
    parser.add_argument("--require-c", action="store_true", help="require same-dump board C results")
    args = parser.parse_args()
    if not 0 <= args.conf <= 1:
        parser.error("confidence must be in [0,1]")
    root = args.run_dir
    board = root / args.board_dir
    results = []
    for rgb in sorted((root / "samples").glob("sample-*.rgb")):
        name = rgb.stem
        manifest = json.loads(rgb.with_suffix(".rgb.json").read_text())
        digest = hashlib.sha256(rgb.read_bytes()).hexdigest()
        if digest != manifest["input_sha256"]:
            raise ValueError(f"input hash mismatch: {rgb}")
        shapes = ((1, 2, 8400), (1, 4, 8400)) if args.split else ((1, 300, 6),)
        natives, floats, attrs, errors = [], [], [], []
        for index, shape in enumerate(shapes):
            value, attr = read_native(board / f"{name}-native", board / f"{name}-native.log", index, shape)
            floating = np.fromfile(board / f"{name}-float" / f"output-{index}.bin", dtype=np.float32).reshape(shape)
            error = float(np.max(np.abs(value - floating)))
            if not np.allclose(value, floating, rtol=1e-6, atol=1e-4):
                raise ValueError(f"native/float output mismatch for {name} output {index}: {error}")
            natives.append(value); floats.append(floating); attrs.append(attr); errors.append(error)
        native = topk_yolo26(*natives) if args.split else natives[0]
        reference = detections(np.load(rgb.with_suffix(".onnx.npy")), args.conf)
        candidate = detections(native, args.conf)
        matches = compare(reference, candidate)
        log = (board / f"{name}-float.log").read_text()
        times = [int(v) for v in re.findall(r"run=\d+ elapsed_us=(\d+)", log)]
        record = {"sample": name, "input_sha256": digest, "native_outputs": attrs,
            "native_float_max_abs_error": max(errors), "onnx_detections": reference,
            "board_detections": candidate, "matches_iou05": matches,
            "onnx_counts": {k: sum(v["class_name"] == k for v in reference) for k in CLASSES},
            "board_counts": {k: sum(v["class_name"] == k for v in candidate) for k in CLASSES},
            "elapsed_us": times, "mean_elapsed_us": float(np.mean(times)),
            "mean_after_first_us": float(np.mean(times[1:])) if len(times) > 1 else None}
        c_path = board / f"{name}-c.json"
        record["board_c_postprocess_validated"] = False
        if args.require_c and not c_path.exists():
            raise ValueError(f"missing board C result: {c_path}")
        if c_path.exists():
            c_result = json.loads(c_path.read_text())["detections"]
            if len(c_result) != len(candidate):
                raise ValueError(f"board C/Python detection count mismatch: {name}")
            errors = []
            for expected, actual in zip(candidate, c_result):
                if actual["class_id"] != expected["class_id"]:
                    raise ValueError(f"board C/Python class mismatch: {name}")
                a = np.array(expected["xyxy"] + [expected["score"]])
                b = np.array(actual["xyxy"] + [actual["score"]])
                if not np.allclose(a, b, rtol=1e-6, atol=1e-4):
                    raise ValueError(f"board C/Python coordinates/scores mismatch: {name}")
                errors.append(float(np.max(np.abs(a - b))))
            record["board_c_postprocess_validated"] = True
            record["board_c_python_max_abs_error"] = max(errors, default=0.0)
        results.append(record)
        image = Image.open(manifest["image"]).convert("RGB")
        draw = ImageDraw.Draw(image)
        for det in candidate:
            box = np.asarray(det["xyxy"], dtype=float)
            box[[0, 2]] = (box[[0, 2]] - manifest["pad_left"]) / manifest["scale"]
            box[[1, 3]] = (box[[1, 3]] - manifest["pad_top"]) / manifest["scale"]
            box[[0, 2]] = np.clip(box[[0, 2]], 0, image.width)
            box[[1, 3]] = np.clip(box[[1, 3]], 0, image.height)
            det["source_xyxy"] = box.tolist()
            color = "red" if det["class_id"] == 0 else "blue"
            if box[2] > box[0] and box[3] > box[1]:
                draw.rectangle(box.tolist(), outline=color, width=3)
                draw.text((box[0], max(0, box[1] - 14)), f"{det['class_name']} {det['score']:.3f}", fill=color)
        image.save(board / f"{name}-detections.jpg")
        print(name, "ONNX", record["onnx_counts"], "board", record["board_counts"], "matched", len(matches))
    if not results:
        raise ValueError("no samples found")
    report = {"confidence_threshold": args.conf, "classes": CLASSES, "split_outputs": args.split,
        "note": "Three unlabeled cabinet frames: runtime and same-input comparison only, not detection accuracy evaluation.",
        "samples": results}
    (board / "comparison.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
