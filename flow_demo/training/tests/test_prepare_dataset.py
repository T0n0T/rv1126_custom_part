from __future__ import annotations

import tempfile
import unittest
import zipfile
from pathlib import Path

from training.prepare_dataset import prepare_dataset, scan_archive


class PrepareDatasetTest(unittest.TestCase):
    def make_archive(self, path: Path) -> None:
        entries = {
            "bundle/data.yaml": (
                "nc: 3\nnames: [Vest, helmet, Gloves]\n"
                "train: ../train/images\nval: ../valid/images\ntest: ../test/images\n"
            ),
            "bundle/train/images/one.jpg": b"train-image",
            "bundle/train/labels/one.txt": (
                "0 0.1 0.2 0.3 0.4\n"
                "1 0.4 0.4 0.6 0.4 0.6 0.8 0.4 0.8\n"
            ),
            "bundle/valid/images/empty.png": b"valid-image",
            "bundle/valid/labels/empty.txt": "2 0.2 0.2 0.1 0.1\n",
            "bundle/test/images/test.jpg": b"test-image",
            "bundle/test/labels/test.txt": "1 0.4 0.4 0.3 0.3\n",
        }
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, content in entries.items():
                if isinstance(content, str):
                    content = content.encode("utf-8")
                archive.writestr(name, content)

    def test_scans_and_maps_target_class_from_prefixed_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive_path = Path(temp_dir) / "ppe.zip"
            self.make_archive(archive_path)
            with zipfile.ZipFile(archive_path) as archive:
                summary = scan_archive(archive, target_class_id=1, class_count=3, names=["Vest", "helmet", "Gloves"])

        self.assertEqual(summary["images"], {"train": 1, "val": 1, "test": 1})
        self.assertEqual(summary["target_boxes"], {"train": 1, "val": 0, "test": 1})
        self.assertEqual(summary["ignored_boxes"], {"train": 1, "val": 1, "test": 0})
        self.assertEqual(summary["bbox_rows"], {"train": 1, "val": 1, "test": 1})
        self.assertEqual(summary["polygon_rows"], {"train": 1, "val": 0, "test": 0})
        self.assertEqual(summary["missing_labels"], [])

    def test_writes_only_target_class_as_zero_and_keeps_empty_negative_label(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            archive_path = root / "ppe.zip"
            output_dir = root / "prepared"
            self.make_archive(archive_path)

            summary = prepare_dataset(archive_path, output_dir, target_class="helmet", force=False)

            self.assertEqual(summary["target_source_class_id"], 1)
            self.assertEqual(
                (output_dir / "labels/train/one.txt").read_text(encoding="utf-8"),
                "0 0.5 0.6 0.2 0.4\n",
            )
            self.assertEqual((output_dir / "labels/val/empty.txt").read_text(encoding="utf-8"), "")
            self.assertIn("nc: 1", (output_dir / "dataset.yaml").read_text(encoding="utf-8"))
            self.assertEqual((output_dir / "images/test/test.jpg").read_bytes(), b"test-image")


if __name__ == "__main__":
    unittest.main()
