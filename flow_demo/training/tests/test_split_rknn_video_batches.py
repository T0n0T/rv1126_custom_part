from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from split_rknn_video_batches import split_batches  # noqa: E402


def test_split_batches_preserves_frame_bytes_and_pts_order(tmp_path: Path) -> None:
    frames = tmp_path / "frames.rgb"
    metadata = tmp_path / "frames.tsv"
    output = tmp_path / "batches"
    payloads = [bytes([index]) * 12 for index in range(5)]
    frames.write_bytes(b"".join(payloads))
    metadata.write_text("".join(f"{i} {i * 33.3:.1f} 2 2 1 0 0\n" for i in range(5)))

    batches = split_batches(frames, metadata, output, batch_frames=2, frame_bytes=12)

    assert [batch["frame_count"] for batch in batches] == [2, 2, 1]
    assert [batch["first_frame"] for batch in batches] == [0, 2, 4]
    actual = b"".join(Path(batch["frames"]).read_bytes() for batch in batches)
    assert actual == b"".join(payloads)
    metadata_rows = [Path(batch["metadata"]).read_text().splitlines() for batch in batches]
    assert [row for batch in metadata_rows for row in batch] == metadata.read_text().splitlines()


def test_split_batches_rejects_bad_size_and_nonmonotonic_metadata(tmp_path: Path) -> None:
    frames = tmp_path / "frames.rgb"
    metadata = tmp_path / "frames.tsv"
    frames.write_bytes(bytes(24))
    metadata.write_text("0 10 2 2 1 0 0\n1 9 2 2 1 0 0\n")

    with pytest.raises(ValueError, match="sequential and monotonic"):
        split_batches(frames, metadata, tmp_path / "batches", batch_frames=1, frame_bytes=12)

    metadata.write_text("0 10 2 2 1 0 0\n1 11 2 2 1 0 0\n")
    with pytest.raises(ValueError, match="RGB byte count"):
        split_batches(frames, metadata, tmp_path / "batches-2", batch_frames=1, frame_bytes=11)


def test_split_batches_refuses_reusing_batch_directory(tmp_path: Path) -> None:
    frames = tmp_path / "frames.rgb"
    metadata = tmp_path / "frames.tsv"
    output = tmp_path / "batches"
    frames.write_bytes(bytes(12))
    metadata.write_text("0 0 2 2 1 0 0\n")
    split_batches(frames, metadata, output, batch_frames=1, frame_bytes=12)

    with pytest.raises(FileExistsError):
        split_batches(frames, metadata, output, batch_frames=1, frame_bytes=12)
