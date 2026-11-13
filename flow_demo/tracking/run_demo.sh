#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	printf 'usage: %s ZONES.json VIDEO [RUN_DIRECTORY]\n' "$0" >&2
	exit 2
fi

ZONES=$1
VIDEO=$2
RUN=${3:-"runs/demo/$(date +%Y%m%d-%H%M%S)"}
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=${PYTHON:-"$ROOT/.venv-rknn310/bin/python"}
MODEL=${MODEL:-"$ROOT/comp-2-freeze10.onnx"}
BIN=${TRACKER_BIN:-"$ROOT/tracking/build/flow_demo_cli"}
case "$RUN" in
	/*) RUN_DIR=$RUN ;;
	*) RUN_DIR="$ROOT/$RUN" ;;
esac

if [ ! -f "$ZONES" ]; then
	printf 'Zone config not found: %s\n' "$ZONES" >&2
	exit 1
fi
if [ ! -x "$PYTHON" ]; then
	printf 'Python environment not found at %s\n' "$PYTHON" >&2
	exit 1
fi
if [ ! -x "$BIN" ]; then
	printf 'Build the C++ demo first: cmake -S tracking -B tracking/build && cmake --build tracking/build\n' >&2
	exit 1
fi
"$PYTHON" - "$ZONES" <<'PYCFG'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    if not json.load(source).get("geometry_confirmed", False):
        raise SystemExit("Refusing to run on real video until geometry_confirmed=true after camera review")
PYCFG
mkdir -p "$(dirname -- "$RUN_DIR")"
mkdir "$RUN_DIR"
"$PYTHON" "$ROOT/tracking/session_config.py" \
	--profile "$ZONES" --video "$VIDEO" \
	--session-id "$(basename -- "$RUN_DIR")" --output "$RUN_DIR/zones.json"
"$PYTHON" "$ROOT/training/run_video_inference.py" \
	--model "$MODEL" --video "$VIDEO" --output "$RUN_DIR/detections.jsonl" \
	2>"$RUN_DIR/inference.log"
"$BIN" --config "$RUN_DIR/zones.json" \
	--detections "$RUN_DIR/detections.jsonl" \
	--tracks "$RUN_DIR/tracks.jsonl" \
	--events "$RUN_DIR/events.jsonl" \
	--summary "$RUN_DIR/summary.json" \
	2>"$RUN_DIR/tracker.log"
"$PYTHON" "$ROOT/training/annotate_tracks.py" \
	--video "$VIDEO" --zones "$RUN_DIR/zones.json" \
	--tracks "$RUN_DIR/tracks.jsonl" --events "$RUN_DIR/events.jsonl" \
	--output "$RUN_DIR/annotated.mp4" >"$RUN_DIR/annotation.log"
printf 'Demo artifacts: %s\n' "$RUN_DIR"
