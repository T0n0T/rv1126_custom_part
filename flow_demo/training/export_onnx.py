#!/usr/bin/env python3
"""Export a trained Ultralytics checkpoint as a static FP32 YOLOv8 ONNX model."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", type=Path, required=True, help="trained best.pt checkpoint")
    parser.add_argument("--output", type=Path, help="optional destination .onnx path")
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--opset", type=int, default=12)
    parser.add_argument("--device", default="", help="CUDA device such as 0, or cpu")
    parser.add_argument("--simplify", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--class-name", default="helmet", help="expected single class in the checkpoint")
    parser.add_argument("--force", action="store_true", help="replace an existing --output file")
    return parser.parse_args()


def import_yolo() -> Any:
    try:
        from ultralytics import YOLO
    except ImportError as error:
        raise SystemExit("ultralytics is missing; install training/requirements.txt first") from error
    return YOLO


def main() -> int:
    args = parse_args()
    weights = args.weights.resolve()
    if not weights.is_file():
        print(f"export_onnx: checkpoint not found: {weights}", file=sys.stderr)
        return 2
    if args.imgsz <= 0 or args.opset <= 0:
        print("export_onnx: imgsz and opset must be positive", file=sys.stderr)
        return 2

    YOLO = import_yolo()
    model = YOLO(str(weights))
    names = model.names
    if isinstance(names, dict):
        names = [names[key] for key in sorted(names, key=lambda value: int(value))]
    names = [str(name) for name in names]
    if names != [args.class_name]:
        print(
            f"export_onnx: expected one class named {args.class_name!r}, checkpoint has {names!r}",
            file=sys.stderr,
        )
        return 2
    default_export = weights.with_suffix(".onnx")
    destinations = {default_export.resolve()}
    if args.output:
        output_path = args.output.resolve()
        if output_path.suffix.casefold() != ".onnx":
            print("export_onnx: --output must end with .onnx", file=sys.stderr)
            return 2
        destinations.add(output_path)
    if not args.force:
        existing = sorted(path for path in destinations if path.exists())
        if existing:
            print(
                "export_onnx: output already exists: "
                + ", ".join(str(path) for path in existing)
                + "; pass --force to replace it",
                file=sys.stderr,
            )
            return 2
    export_args: dict[str, Any] = {
        "format": "onnx",
        "imgsz": args.imgsz,
        "batch": 1,
        "dynamic": False,
        "half": False,
        "simplify": args.simplify,
        "opset": args.opset,
        "nms": False,
    }
    if args.device:
        export_args["device"] = args.device
    print(f"Exporting {weights}")
    exported = Path(model.export(**export_args)).resolve()
    if not exported.is_file():
        print(f"export_onnx: exporter returned missing file: {exported}", file=sys.stderr)
        return 2
    if args.output:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        if exported != output:
            shutil.copy2(exported, output)
        exported = output
    try:
        import onnx

        onnx.checker.check_model(str(exported))
        model_proto = onnx.load(str(exported))
    except Exception as error:
        print(f"export_onnx: ONNX validation failed: {error}", file=sys.stderr)
        return 2
    if len(model_proto.graph.input) != 1:
        print(f"export_onnx: expected exactly one input, found {len(model_proto.graph.input)}", file=sys.stderr)
        return 2
    input_info = model_proto.graph.input[0]
    tensor_type = input_info.type.tensor_type
    shape = [dimension.dim_value for dimension in tensor_type.shape.dim]
    expected_shape = [1, 3, args.imgsz, args.imgsz]
    if tensor_type.elem_type != onnx.TensorProto.FLOAT or shape != expected_shape:
        print(
            f"export_onnx: expected float32 input {expected_shape}, got type={tensor_type.elem_type}, shape={shape}",
            file=sys.stderr,
        )
        return 2
    print(f"ONNX output: {exported}")
    print("Contract: static batch-1 FP32 NCHW input, 640x640 by default; verify with the RKNN toolchain before board deployment.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
