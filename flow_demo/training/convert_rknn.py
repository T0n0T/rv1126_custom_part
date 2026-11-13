#!/usr/bin/env python3
"""Convert a static RGB/255 ONNX to RV1126B INT8 and record its provenance."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
from datetime import datetime, timezone
from pathlib import Path

import onnx
from rknn.api import RKNN


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tensor_spec(value) -> dict:
    return {
        "name": value.name,
        "dtype": onnx.TensorProto.DataType.Name(value.type.tensor_type.elem_type),
        "shape": [d.dim_value or d.dim_param for d in value.type.tensor_type.shape.dim],
    }


def extract_yolo26_outputs(model: onnx.ModelProto, path: Path) -> None:
    """Keep decoded boxes and probabilities separate for independent INT8 scales."""
    specs = (("/model.23/Sigmoid_output_0", [1, 2, 8400]),
             ("/model.23/Mul_2_output_0", [1, 4, 8400]))
    available = {name for node in model.graph.node for name in node.output}
    if any(name not in available for name, shape in specs):
        raise ValueError("ONNX does not have the expected YOLO26l one2one outputs")
    outputs = [onnx.helper.make_tensor_value_info(name, onnx.TensorProto.FLOAT, shape) for name, shape in specs]
    # Extract backwards from the requested outputs, excluding the TopK tail.
    needed = {v.name for v in outputs}
    nodes = []
    for node in reversed(model.graph.node):
        if needed.intersection(node.output):
            nodes.append(node)
            needed.update(node.input)
    graph = onnx.helper.make_graph(list(reversed(nodes)), model.graph.name + "_split_outputs",
        list(model.graph.input), outputs, [v for v in model.graph.initializer if v.name in needed])
    result = onnx.helper.make_model(graph, opset_imports=list(model.opset_import), ir_version=model.ir_version)
    result.metadata_props.extend(model.metadata_props)
    onnx.checker.check_model(result)
    onnx.save(result, str(path))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True, help="RKNN image list")
    parser.add_argument("--output-dir", type=Path, required=True, help="new attempt directory")
    parser.add_argument("--yolo26-split", action="store_true", help="separate scores/boxes before TopK for YOLO26l")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    source = args.onnx.resolve()
    dataset = args.dataset.resolve()
    model = onnx.load(str(source))
    inputs = [tensor_spec(v) for v in model.graph.input]
    if len(inputs) != 1 or inputs[0]["shape"] != [1, 3, 640, 640] or inputs[0]["dtype"] != "FLOAT":
        raise ValueError(f"expected static float32 RGB/255 input [1,3,640,640], got {inputs}")
    target = source
    if args.yolo26_split:
        target = (args.output_dir / "comp-2-freeze10-split.onnx").resolve()
        extract_yolo26_outputs(model, target)
    output = (args.output_dir / "comp-2-freeze10-rv1126b-int8.rknn").resolve()
    metadata = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "toolkit_version": importlib.metadata.version("rknn-toolkit2"),
        "target_platform": "rv1126b", "quantized_dtype": "w8a8", "quantized_algorithm": "normal",
        "onnx": str(source), "onnx_sha256": sha256(source),
        "onnx_metadata": {v.key: v.value for v in model.metadata_props},
        "source_outputs": [tensor_spec(v) for v in model.graph.output],
        "converted_onnx": str(target), "converted_onnx_sha256": sha256(target),
        "rknn_outputs": [tensor_spec(v) for v in onnx.load(str(target)).graph.output],
        "input": {**inputs[0], "runtime_layout": "NHWC", "runtime_dtype": "uint8", "color": "RGB", "std": 255, "padding": 114},
        "dataset": str(dataset), "dataset_sha256": sha256(dataset),
        "calibration_count": len(dataset.read_text().splitlines()), "split_outputs": args.yolo26_split,
        "cpu_postprocess": "two-stage TopK(300), xyxy/score/class, no NMS" if args.yolo26_split else None,
        "conversion_passed": False, "board_validated": False,
    }
    log = args.output_dir / "conversion.json"
    log.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    rknn = RKNN(verbose=True, verbose_file=str(args.output_dir / "toolkit.log"))
    try:
        for stage, call in (
            ("config", lambda: rknn.config(target_platform="rv1126b", mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], quantized_dtype="w8a8", quantized_algorithm="normal")),
            ("load_onnx", lambda: rknn.load_onnx(model=str(target))),
            ("build", lambda: rknn.build(do_quantization=True, dataset=str(dataset))),
            ("export_rknn", lambda: rknn.export_rknn(str(output))),
        ):
            print(f"STAGE {stage}", flush=True)
            ret = call()
            if ret != 0:
                raise RuntimeError(f"{stage} failed: {ret}")
        metadata.update(conversion_passed=True, rknn=str(output), rknn_sha256=sha256(output), rknn_bytes=output.stat().st_size)
        print(f"RKNN_CONVERSION_OK {output}", flush=True)
    except Exception as error:
        metadata["error"] = str(error)
        raise
    finally:
        rknn.release()
        log.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
