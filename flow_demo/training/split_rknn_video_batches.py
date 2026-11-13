#!/usr/bin/env python3
"""Split a prepared RGB video stream and its TSV into bounded transfer batches."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

FRAME_BYTES = 640 * 640 * 3


def split_batches(frames: Path, metadata: Path, output_dir: Path,
                  batch_frames: int, frame_bytes: int = FRAME_BYTES) -> list[dict]:
    if batch_frames <= 0 or frame_bytes <= 0:
        raise ValueError("batch_frames and frame_bytes must be positive")
    rows = metadata.read_text(encoding="utf-8").splitlines()
    if not rows:
        raise ValueError("metadata contains no frames")
    previous_index = -1
    previous_pts = float("-inf")
    for line_number, row in enumerate(rows, start=1):
        fields = row.split()
        if len(fields) != 7:
            raise ValueError(f"metadata row {line_number} must contain seven fields")
        frame_index = int(fields[0])
        pts_ms = float(fields[1])
        if frame_index != previous_index + 1 or pts_ms < previous_pts:
            raise ValueError(f"metadata is not sequential and monotonic at row {line_number}")
        previous_index = frame_index
        previous_pts = pts_ms
    expected_size = len(rows) * frame_bytes
    if frames.stat().st_size != expected_size:
        raise ValueError(f"RGB byte count {frames.stat().st_size} != expected {expected_size}")
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise FileExistsError(f"batch directory is not empty: {output_dir}")

    batches: list[dict] = []
    count = math.ceil(len(rows) / batch_frames)
    with frames.open("rb") as source:
        for batch_index in range(count):
            start = batch_index * batch_frames
            end = min(start + batch_frames, len(rows))
            frame_path = output_dir / f"batch-{batch_index:04d}.rgb"
            metadata_path = output_dir / f"batch-{batch_index:04d}.tsv"
            with frame_path.open("xb") as target:
                source.seek(start * frame_bytes)
                remaining = (end - start) * frame_bytes
                while remaining:
                    block = source.read(min(1024 * 1024, remaining))
                    if not block:
                        raise ValueError(f"unexpected end of RGB stream in batch {batch_index}")
                    target.write(block)
                    remaining -= len(block)
            metadata_path.write_text("\n".join(rows[start:end]) + "\n", encoding="utf-8")
            batches.append({"index": batch_index, "first_frame": start, "frame_count": end - start,
                            "frames": str(frame_path), "metadata": str(metadata_path)})
    return batches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--batch-frames", type=int, default=30)
    args = parser.parse_args()
    batches = split_batches(args.frames, args.metadata, args.output_dir, args.batch_frames)
    manifest = {"source_frames": str(args.frames.resolve()), "source_metadata": str(args.metadata.resolve()),
                "batch_frames": args.batch_frames, "total_frames": sum(b["frame_count"] for b in batches),
                "batch_count": len(batches), "batches": batches}
    output = args.output_dir / "batches.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"total_frames": manifest["total_frames"],
                      "batch_count": manifest["batch_count"], "manifest": str(output.resolve())}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
