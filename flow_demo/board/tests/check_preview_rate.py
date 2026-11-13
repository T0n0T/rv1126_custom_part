"""Check measured KMS preview cadence, independently of detector throughput."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('stats', type=Path)
parser.add_argument('--min-fps', type=float, default=24)
args = parser.parse_args()
stats = json.loads(args.stats.read_text())
preview = (stats['preview_fps'] * stats['rendered'] / max(stats['preview_frames'], 1)
           if 'preview_fps' in stats else stats['fps'] * stats['rendered'] / max(stats['processed'], 1))
print(f"preview={preview:.3f} FPS detector={stats['fps']:.3f} FPS target>={args.min_fps:g}")
assert preview >= args.min_fps, 'KMS preview is visibly limited below the configured camera rate'
