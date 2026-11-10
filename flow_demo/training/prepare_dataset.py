#!/usr/bin/env python3
"""Convert a YOLO PPE archive into the single-class flow_demo dataset.

The source archive is kept untouched. Only annotations for the requested
class are copied, and their class id is remapped to zero.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

import yaml


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
SOURCE_SPLITS = {"train": "train", "valid": "val", "val": "val", "test": "test"}


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def dataset_yaml_member(names: Iterable[str]) -> str:
    candidates = [name for name in names if PurePosixPath(name).name in {"data.yaml", "data.yml"}]
    if not candidates:
        raise ValueError("archive does not contain data.yaml or data.yml")
    return min(candidates, key=lambda value: (value.count("/"), value))


def normalize_names(raw_names: Any) -> list[str]:
    if isinstance(raw_names, list):
        return [str(value) for value in raw_names]
    if isinstance(raw_names, dict):
        try:
            return [str(raw_names[str(index)]) for index in range(len(raw_names))]
        except KeyError:
            return [str(raw_names[index]) for index in range(len(raw_names))]
    raise ValueError("data.yaml names must be a list or a numeric-keyed mapping")


def load_source_config(archive: zipfile.ZipFile) -> tuple[str, dict[str, Any], list[str]]:
    member = dataset_yaml_member(archive.namelist())
    config = yaml.safe_load(archive.read(member).decode("utf-8")) or {}
    names = normalize_names(config.get("names"))
    declared_count = config.get("nc")
    if declared_count is not None and int(declared_count) != len(names):
        raise ValueError(f"{member}: nc={declared_count} does not match {len(names)} class names")
    return member, config, names


def find_class_id(names: list[str], target: str) -> int:
    target_key = target.casefold()
    for index, name in enumerate(names):
        if name.casefold() == target_key:
            return index
    available = ", ".join(names)
    raise ValueError(f"target class {target!r} not found; available classes: {available}")


def split_and_relative(member: str) -> tuple[str, str, PurePosixPath] | None:
    path = PurePosixPath(member)
    parts = path.parts
    for index in range(len(parts) - 1):
        source_split = parts[index]
        if source_split not in SOURCE_SPLITS or parts[index + 1] != "images":
            continue
        relative = PurePosixPath(*parts[index + 2 :])
        if relative.is_absolute() or any(part in {".", ".."} or "\\" in part for part in relative.parts):
            raise ValueError(f"unsafe image path in archive: {member}")
        if relative.suffix.casefold() not in IMAGE_SUFFIXES:
            return None
        return SOURCE_SPLITS[source_split], source_split, relative
    return None


def label_member_for(
    image_member: str,
    source_split: str,
    relative_image: PurePosixPath,
    names: set[str],
) -> str | None:
    image_path = PurePosixPath(image_member)
    image_parts = list(image_path.parts)
    split_index = image_parts.index(source_split)
    label_path = PurePosixPath(
        *image_parts[:split_index],
        source_split,
        "labels",
        *relative_image.with_suffix(".txt").parts,
    )
    candidates = [str(label_path), f"{source_split}/labels/{relative_image.with_suffix('.txt')}"]
    for candidate in candidates:
        if candidate in names:
            return candidate

    # The archive may have a directory prefix, so match the suffix.
    suffix = f"/{source_split}/labels/{relative_image.with_suffix('.txt')}"
    matches = [name for name in names if name.endswith(suffix)]
    return min(matches) if matches else None


def convert_label_text(
    text: str,
    *,
    target_class_id: int,
    class_count: int,
    member: str,
) -> tuple[str, int, int, int, int, int]:
    output: list[str] = []
    kept = 0
    ignored = 0
    total = 0
    bbox_rows = 0
    polygon_rows = 0
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) == 5:
            coordinates_text = fields[1:]
            annotation_format = "bbox"
        elif len(fields) >= 7 and (len(fields) - 1) % 2 == 0:
            coordinates_text = fields[1:]
            annotation_format = "polygon"
        else:
            raise ValueError(
                f"{member}:{line_number}: expected a 5-field YOLO box or a polygon with at least 3 points, "
                f"got {len(fields)} fields"
            )
        try:
            class_id = int(fields[0])
            coordinates = [float(value) for value in coordinates_text]
        except ValueError as error:
            raise ValueError(f"{member}:{line_number}: invalid numeric field") from error
        if not 0 <= class_id < class_count:
            raise ValueError(f"{member}:{line_number}: class id {class_id} outside 0..{class_count - 1}")
        if any(not math.isfinite(value) or value < 0.0 or value > 1.0 for value in coordinates):
            raise ValueError(f"{member}:{line_number}: normalized coordinates must be in [0, 1]")
        if annotation_format == "bbox":
            center_x, center_y, width, height = coordinates
        else:
            xs = coordinates[0::2]
            ys = coordinates[1::2]
            left, right = min(xs), max(xs)
            top, bottom = min(ys), max(ys)
            center_x = (left + right) / 2.0
            center_y = (top + bottom) / 2.0
            width = right - left
            height = bottom - top
            if width <= 0.0 or height <= 0.0:
                raise ValueError(f"{member}:{line_number}: polygon must span a non-zero area")
        bbox_rows += annotation_format == "bbox"
        polygon_rows += annotation_format == "polygon"
        total += 1
        if class_id == target_class_id:
            if annotation_format == "bbox":
                bbox = " ".join(fields[1:])
            else:
                bbox = " ".join(f"{value:.8g}" for value in (center_x, center_y, width, height))
            output.append("0 " + bbox)
            kept += 1
        else:
            ignored += 1
    return (
        "\n".join(output) + ("\n" if output else ""),
        kept,
        ignored,
        total,
        bbox_rows,
        polygon_rows,
    )


def image_members(archive: zipfile.ZipFile) -> list[tuple[str, str, str, PurePosixPath]]:
    result: list[tuple[str, str, str, PurePosixPath]] = []
    for member in archive.namelist():
        split_info = split_and_relative(member)
        if split_info is None:
            continue
        split, source_split, relative = split_info
        result.append((member, split, source_split, relative))
    return sorted(result, key=lambda item: (item[1], str(item[3]), item[0]))


def scan_archive(
    archive: zipfile.ZipFile,
    *,
    target_class_id: int,
    class_count: int,
    names: list[str],
) -> dict[str, Any]:
    members = set(archive.namelist())
    summary: dict[str, Any] = {
        "images": {"train": 0, "val": 0, "test": 0},
        "images_with_target": {"train": 0, "val": 0, "test": 0},
        "target_boxes": {"train": 0, "val": 0, "test": 0},
        "ignored_boxes": {"train": 0, "val": 0, "test": 0},
        "all_boxes": {"train": 0, "val": 0, "test": 0},
        "bbox_rows": {"train": 0, "val": 0, "test": 0},
        "polygon_rows": {"train": 0, "val": 0, "test": 0},
        "missing_labels": [],
        "source_classes": names,
    }
    for member, split, source_split, relative in image_members(archive):
        label_member = label_member_for(member, source_split, relative, members)
        summary["images"][split] += 1
        if label_member is None:
            summary["missing_labels"].append(member)
            continue
        _, kept, ignored, total, bbox_rows, polygon_rows = convert_label_text(
            archive.read(label_member).decode("utf-8"),
            target_class_id=target_class_id,
            class_count=class_count,
            member=label_member,
        )
        summary["target_boxes"][split] += kept
        summary["ignored_boxes"][split] += ignored
        summary["all_boxes"][split] += total
        summary["bbox_rows"][split] += bbox_rows
        summary["polygon_rows"][split] += polygon_rows
        if kept:
            summary["images_with_target"][split] += 1
    return summary


def prepare_dataset(
    archive_path: Path,
    output_dir: Path,
    *,
    target_class: str,
    force: bool,
) -> dict[str, Any]:
    archive_path = archive_path.resolve()
    output_dir = output_dir.resolve()
    with zipfile.ZipFile(archive_path) as archive:
        yaml_member, source_config, source_names = load_source_config(archive)
        target_class_id = find_class_id(source_names, target_class)
        summary = scan_archive(
            archive,
            target_class_id=target_class_id,
            class_count=len(source_names),
            names=source_names,
        )
        summary.update(
            {
                "archive_filename": archive_path.name,
                "archive_sha256": sha256_file(archive_path),
                "source_yaml_member": yaml_member,
                "target_class": target_class,
                "target_source_class_id": target_class_id,
            }
        )
        if not summary["images"]["train"] or not summary["images"]["val"]:
            raise ValueError("source archive must contain non-empty train and validation image splits")
        if summary["missing_labels"]:
            raise ValueError(
                f"{len(summary['missing_labels'])} images have no matching label file; first: "
                f"{summary['missing_labels'][0]}"
            )

        if output_dir.exists() and not force:
            raise FileExistsError(f"output already exists: {output_dir}; pass --force to replace it")
        output_dir.parent.mkdir(parents=True, exist_ok=True)
        staging_dir = output_dir.with_name(output_dir.name + ".tmp")
        if staging_dir.exists():
            raise FileExistsError(f"staging directory already exists: {staging_dir}; inspect it before removing it")
        staging_dir.mkdir(parents=True)

        members = set(archive.namelist())
        for member, split, source_split, relative in image_members(archive):
            output_image = staging_dir / "images" / split / Path(relative)
            output_label = staging_dir / "labels" / split / Path(relative).with_suffix(".txt")
            output_image.parent.mkdir(parents=True, exist_ok=True)
            output_label.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, output_image.open("wb") as destination:
                shutil.copyfileobj(source, destination)

            label_member = label_member_for(member, source_split, relative, members)
            label_text = ""
            if label_member is not None:
                label_text, _, _, _, _, _ = convert_label_text(
                    archive.read(label_member).decode("utf-8"),
                    target_class_id=target_class_id,
                    class_count=len(source_names),
                    member=label_member,
                )
            output_label.write_text(label_text, encoding="utf-8")

        dataset_config = {
            "path": ".",
            "train": "images/train",
            "val": "images/val",
            "test": "images/test",
            "nc": 1,
            "names": [target_class],
        }
        (staging_dir / "dataset.yaml").write_text(
            yaml.safe_dump(dataset_config, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
        )
        metadata = {
            "generated_at": datetime.now(timezone.utc).isoformat(),
            "source": summary,
            "source_config": source_config,
            "output_config": dataset_config,
            "counts": {
                key: value
                for key, value in summary.items()
                if key
                in {
                    "images",
                    "images_with_target",
                    "target_boxes",
                    "ignored_boxes",
                    "all_boxes",
                    "bbox_rows",
                    "polygon_rows",
                }
            },
        }
        (staging_dir / "conversion.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if output_dir.exists():
            shutil.rmtree(output_dir)
        staging_dir.rename(output_dir)
    return summary


def parse_args() -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=project_root / "dataset/PPE.zip")
    parser.add_argument(
        "--output",
        type=Path,
        default=project_root / "dataset/prepared/ppe_helmet",
        help="portable generated dataset directory",
    )
    parser.add_argument("--target-class", default="helmet")
    parser.add_argument("--check-only", action="store_true", help="scan labels without writing images")
    parser.add_argument("--force", action="store_true", help="replace an existing generated output directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        with zipfile.ZipFile(args.archive) as archive:
            yaml_member, _, source_names = load_source_config(archive)
            target_class_id = find_class_id(source_names, args.target_class)
            summary = scan_archive(
                archive,
                target_class_id=target_class_id,
                class_count=len(source_names),
                names=source_names,
            )
            summary["source_yaml_member"] = yaml_member
            summary["target_class"] = args.target_class
            summary["target_source_class_id"] = target_class_id
        if args.check_only:
            print(json.dumps(summary, ensure_ascii=False, indent=2))
            return 0
        summary = prepare_dataset(
            args.archive,
            args.output,
            target_class=args.target_class,
            force=args.force,
        )
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        print(f"Generated: {args.output.resolve() / 'dataset.yaml'}")
        return 0
    except (OSError, ValueError, yaml.YAMLError, zipfile.BadZipFile) as error:
        print(f"prepare_dataset: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
