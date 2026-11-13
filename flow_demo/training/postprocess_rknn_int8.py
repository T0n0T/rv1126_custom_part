#!/usr/bin/env python3
"""Decode native INT8 outputs from the flow_demo RV1126B YOLOv8n model."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

import numpy as np


INPUT_SIZE = 640
REG_MAX = 16


@dataclass(frozen=True)
class Head:
    stride: int
    size: int
    box_zero_point: int
    box_scale: float
    score_zero_point: int
    score_scale: float


# Queried from the RV1126B RKNN runtime for this exact INT8 model.
HEADS = (
    Head(8, 80, -14, 0.395262748, -128, 0.00334408041),
    Head(16, 40, -24, 1.18675947, -128, 0.00356173562),
    Head(32, 20, -2, 0.711200655, -128, 0.00371879875),
)


@dataclass(frozen=True)
class Detection:
    xyxy: tuple[float, float, float, float]
    score: float
    class_id: int = 0


def dequantize(values: np.ndarray, zero_point: int, scale: float) -> np.ndarray:
    if not np.issubdtype(values.dtype, np.integer):
        raise TypeError(f"expected integer tensor, got {values.dtype}")
    if not np.isfinite(scale) or scale <= 0:
        raise ValueError("quantization scale must be finite and positive")
    return (values.astype(np.float32) - np.float32(zero_point)) * np.float32(scale)


def _decode_head(
    box_quantized: np.ndarray,
    score_quantized: np.ndarray,
    head: Head,
) -> tuple[np.ndarray, np.ndarray]:
    expected_box_shape = (4 * REG_MAX, head.size, head.size)
    expected_score_shape = (1, head.size, head.size)
    if box_quantized.shape != expected_box_shape:
        raise ValueError(f"stride-{head.stride} box shape {box_quantized.shape}; expected {expected_box_shape}")
    if score_quantized.shape != expected_score_shape:
        raise ValueError(
            f"stride-{head.stride} score shape {score_quantized.shape}; expected {expected_score_shape}"
        )

    logits = dequantize(box_quantized, head.box_zero_point, head.box_scale)
    # NCHW channel order is [left bins, top bins, right bins, bottom bins].
    logits = logits.reshape(4, REG_MAX, head.size, head.size).transpose(2, 3, 0, 1)
    logits -= logits.max(axis=-1, keepdims=True)
    probabilities = np.exp(logits)
    probabilities /= probabilities.sum(axis=-1, keepdims=True)
    bins = np.arange(REG_MAX, dtype=np.float32)
    distances = (probabilities * bins).sum(axis=-1) * np.float32(head.stride)

    grid_y, grid_x = np.mgrid[0 : head.size, 0 : head.size].astype(np.float32)
    center_x = (grid_x + 0.5) * np.float32(head.stride)
    center_y = (grid_y + 0.5) * np.float32(head.stride)
    left, top, right, bottom = (distances[..., i] for i in range(4))
    boxes = np.stack(
        (center_x - left, center_y - top, center_x + right, center_y + bottom),
        axis=-1,
    ).reshape(-1, 4)

    # The exported score heads include sigmoid; their dequantized values are
    # probabilities, not logits, so applying sigmoid here would be incorrect.
    scores = dequantize(score_quantized, head.score_zero_point, head.score_scale)[0]
    scores = np.clip(scores, 0.0, 1.0).reshape(-1)
    return boxes, scores


def _iou_one_to_many(box: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    top_left = np.maximum(box[:2], boxes[:, :2])
    bottom_right = np.minimum(box[2:], boxes[:, 2:])
    extent = np.maximum(bottom_right - top_left, 0.0)
    intersection = extent[:, 0] * extent[:, 1]
    area_one = max(float(box[2] - box[0]), 0.0) * max(float(box[3] - box[1]), 0.0)
    area_many = np.maximum(boxes[:, 2] - boxes[:, 0], 0.0) * np.maximum(boxes[:, 3] - boxes[:, 1], 0.0)
    union = area_one + area_many - intersection
    return np.divide(intersection, union, out=np.zeros_like(intersection), where=union > 0)


def nms(
    boxes: np.ndarray,
    scores: np.ndarray,
    iou_threshold: float = 0.7,
    max_det: int = 300,
    max_nms: int = 30000,
) -> list[int]:
    if not 0 <= iou_threshold <= 1:
        raise ValueError("IoU threshold must be in [0, 1]")
    if max_det <= 0 or max_nms <= 0:
        raise ValueError("max_det and max_nms must be positive")
    if boxes.ndim != 2 or boxes.shape[1] != 4 or scores.shape != (len(boxes),):
        raise ValueError("expected boxes [N,4] and scores [N]")

    valid = np.isfinite(scores) & np.isfinite(boxes).all(axis=1)
    valid &= (boxes[:, 2] > boxes[:, 0]) & (boxes[:, 3] > boxes[:, 1])
    candidates = np.flatnonzero(valid)
    if not len(candidates):
        return []
    order = np.argsort(-scores[candidates], kind="stable")[:max_nms]
    candidates = candidates[order]

    keep: list[int] = []
    while candidates.size and len(keep) < max_det:
        current = int(candidates[0])
        keep.append(current)
        remaining = candidates[1:]
        if not remaining.size:
            break
        overlap = _iou_one_to_many(boxes[current], boxes[remaining])
        candidates = remaining[overlap <= iou_threshold]
    return keep


def decode_outputs(
    outputs: Sequence[np.ndarray],
    conf_threshold: float = 0.001,
    iou_threshold: float = 0.7,
    max_det: int = 300,
) -> list[Detection]:
    if len(outputs) != 6:
        raise ValueError(f"expected six output tensors, got {len(outputs)}")
    if not 0 <= conf_threshold <= 1:
        raise ValueError("confidence threshold must be in [0, 1]")

    all_boxes: list[np.ndarray] = []
    all_scores: list[np.ndarray] = []
    for i, head in enumerate(HEADS):
        box_q = np.asarray(outputs[2 * i])
        score_q = np.asarray(outputs[2 * i + 1])
        if box_q.dtype != np.int8 or score_q.dtype != np.int8:
            raise TypeError(f"stride-{head.stride} outputs must be int8, got {box_q.dtype}/{score_q.dtype}")
        boxes, scores = _decode_head(box_q, score_q, head)
        selected = scores >= conf_threshold
        all_boxes.append(boxes[selected])
        all_scores.append(scores[selected])

    boxes = np.concatenate(all_boxes, axis=0)
    scores = np.concatenate(all_scores, axis=0)
    keep = nms(boxes, scores, iou_threshold=iou_threshold, max_det=max_det)
    return [
        Detection(tuple(float(x) for x in boxes[i]), float(scores[i]))
        for i in keep
    ]


def load_dump(directory: Path) -> list[np.ndarray]:
    outputs: list[np.ndarray] = []
    for i, head in enumerate(HEADS):
        for j, channels in ((0, 4 * REG_MAX), (1, 1)):
            path = directory / f"output-{2 * i + j}.bin"
            expected_elements = channels * head.size * head.size
            raw = np.fromfile(path, dtype=np.int8)
            if raw.size != expected_elements:
                raise ValueError(f"{path} has {raw.size} int8 values; expected {expected_elements}")
            outputs.append(raw.reshape(channels, head.size, head.size))
    return outputs


def map_to_source(
    detection: Detection,
    manifest: dict[str, object],
) -> Detection:
    scale = float(manifest["scale"])
    source_width = int(manifest["source_width"])
    source_height = int(manifest["source_height"])
    pad_left = float(manifest["pad_left"])
    pad_top = float(manifest["pad_top"])
    if scale <= 0 or source_width <= 0 or source_height <= 0:
        raise ValueError("manifest has invalid source dimensions or scale")
    x1, y1, x2, y2 = detection.xyxy
    mapped = (
        min(max((x1 - pad_left) / scale, 0.0), float(source_width)),
        min(max((y1 - pad_top) / scale, 0.0), float(source_height)),
        min(max((x2 - pad_left) / scale, 0.0), float(source_width)),
        min(max((y2 - pad_top) / scale, 0.0), float(source_height)),
    )
    return Detection(mapped, detection.score, detection.class_id)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump-dir", type=Path, required=True, help="directory containing output-0.bin through output-5.bin")
    parser.add_argument("--manifest", type=Path, help="preprocessing JSON sidecar; adds original-image coordinates")
    parser.add_argument("--conf", type=float, default=0.001)
    parser.add_argument("--iou", type=float, default=0.7)
    parser.add_argument("--max-det", type=int, default=300)
    parser.add_argument("--output", type=Path, help="write JSON here instead of stdout")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    detections = decode_outputs(load_dump(args.dump_dir), args.conf, args.iou, args.max_det)
    result: dict[str, object] = {
        "model_input": {"width": INPUT_SIZE, "height": INPUT_SIZE},
        "thresholds": {"confidence": args.conf, "iou": args.iou, "max_det": args.max_det},
        "detections": [asdict(detection) for detection in detections],
    }
    if args.manifest:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        result["source_image"] = manifest.get("image")
        result["detections_source_xyxy"] = [
            asdict(map_to_source(detection, manifest)) for detection in detections
        ]
    serialized = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(serialized, encoding="utf-8")
    else:
        print(serialized, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
