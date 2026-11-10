#!/usr/bin/env python3
"""Train the single-class helmet detector with Ultralytics YOLO."""

from __future__ import annotations

import argparse
import importlib.metadata
import json
import platform
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--data",
        type=Path,
        default=project_root / "dataset/prepared/ppe_helmet/dataset.yaml",
        help="generated one-class YOLO dataset YAML",
    )
    parser.add_argument("--model", default="yolov8n.pt", help="local checkpoint or Ultralytics model name")
    parser.add_argument("--resume", type=Path, help="resume from a previous checkpoint")
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--batch", type=int, default=-1, help="-1 enables Ultralytics auto batch sizing")
    parser.add_argument("--device", default="", help="CUDA device such as 0, or cpu; empty means Ultralytics auto")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--project", type=Path, default=project_root / "runs/train")
    parser.add_argument("--name", default="helmet-yolov8n")
    parser.add_argument("--patience", type=int, default=30)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--cache", choices=("none", "ram", "disk"), default="none")
    parser.add_argument("--amp", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--exist-ok", action="store_true")
    return parser.parse_args()


def import_yolo() -> Any:
    try:
        from ultralytics import YOLO
    except ImportError as error:
        raise SystemExit("ultralytics is missing; install training/requirements.txt first") from error
    return YOLO


def main() -> int:
    args = parse_args()
    data = args.data.resolve()
    if not data.is_file():
        print(f"train: dataset YAML not found: {data}", file=sys.stderr)
        return 2
    if args.epochs <= 0 or args.imgsz <= 0 or args.workers < 0:
        print("train: epochs and imgsz must be positive; workers cannot be negative", file=sys.stderr)
        return 2

    YOLO = import_yolo()
    checkpoint = str(args.resume or args.model)
    model = YOLO(checkpoint)
    cache: bool | str = False if args.cache == "none" else args.cache
    train_args: dict[str, Any] = {
        "data": str(data),
        "epochs": args.epochs,
        "imgsz": args.imgsz,
        "batch": args.batch,
        "workers": args.workers,
        "project": str(args.project.resolve()),
        "name": args.name,
        "patience": args.patience,
        "seed": args.seed,
        "cache": cache,
        "amp": args.amp,
        "exist_ok": args.exist_ok,
        "deterministic": True,
        "plots": True,
        "val": True,
        "save": True,
    }
    if args.device:
        train_args["device"] = args.device
    if args.resume:
        train_args["resume"] = True

    print(json.dumps({"checkpoint": checkpoint, "train": train_args}, ensure_ascii=False, indent=2))
    model.train(**train_args)
    trainer = getattr(model, "trainer", None)
    save_dir = Path(getattr(trainer, "save_dir", Path(train_args["project"]) / args.name))
    save_dir.mkdir(parents=True, exist_ok=True)
    (save_dir / "flow_demo_train_config.json").write_text(
        json.dumps(
            {
                "generated_at": datetime.now(timezone.utc).isoformat(),
                "checkpoint": checkpoint,
                "train": train_args,
                "environment": environment_info(),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"Training output: {save_dir}")
    return 0


def environment_info() -> dict[str, Any]:
    info: dict[str, Any] = {
        "python": platform.python_version(),
        "platform": platform.platform(),
        "ultralytics": importlib.metadata.version("ultralytics"),
    }
    try:
        import torch

        info["torch"] = torch.__version__
        info["torch_cuda"] = torch.version.cuda
        info["cuda_available"] = torch.cuda.is_available()
        if torch.cuda.is_available():
            info["cuda_device"] = torch.cuda.get_device_name(0)
    except ImportError:
        info["torch"] = "missing"
    return info


if __name__ == "__main__":
    raise SystemExit(main())
