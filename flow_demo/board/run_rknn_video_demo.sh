#!/bin/sh
set -eu
ADB_TIMEOUT=${ADB_TIMEOUT:-120}
ADB_PREFLIGHT_TIMEOUT=${ADB_PREFLIGHT_TIMEOUT:-15}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	printf 'usage: %s VIDEO RUN_NAME [ZONES.json]\n' "$0" >&2
	exit 2
fi

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
FLOW_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
VIDEO=$(CDPATH='' cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")
RUN_NAME=$2
ZONES=${3:-}
SERIAL=${ADB_SERIAL:-192.168.1.63:5555}
ADB_BIN=${ADB:-adb}
ADB_SERVER_PORT=${ADB_SERVER_PORT:-}
PYTHON=${PYTHON:-"$FLOW_ROOT/.venv-rknn310/bin/python"}
MODEL=${MODEL:-"$FLOW_ROOT/runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn"}
INFER_BIN=${INFER_BIN:-"$SCRIPT_DIR/out/rknn_yolo26_infer"}
TRACKER_BIN=${TRACKER_BIN:-"$FLOW_ROOT/tracking/build-rv1126b/flow_demo_cli"}
CONF=${CONF:-0.25}
MAX_FRAMES=${MAX_FRAMES:-30}
BATCH_FRAMES=${BATCH_FRAMES:-30}
OUTPUT_ROOT=${OUTPUT_ROOT:-"$FLOW_ROOT/runs/board-video"}
RUN_DIR="$OUTPUT_ROOT/$RUN_NAME"
REMOTE_DIR="/tmp/flow_demo/board-video/$RUN_NAME"

adb_bounded() {
	ADB_LIMIT=$1
	shift
	if [ -n "$ADB_SERVER_PORT" ]; then
		if timeout -k 5 "$ADB_LIMIT" "$ADB_BIN" -P "$ADB_SERVER_PORT" "$@"; then
			return 0
		else
			ADB_STATUS=$?
		fi
	else
		if timeout -k 5 "$ADB_LIMIT" "$ADB_BIN" "$@"; then
			return 0
		else
			ADB_STATUS=$?
		fi
	fi
	if [ "$ADB_STATUS" -eq 124 ] || [ "$ADB_STATUS" -eq 137 ]; then
		printf 'ADB command timed out after %ss for %s\n' "$ADB_LIMIT" "$SERIAL" >&2
	else
		printf 'ADB command failed (status=%s) for %s\n' "$ADB_STATUS" "$SERIAL" >&2
	fi
	return "$ADB_STATUS"
}

adb_cmd() {
	adb_bounded "$ADB_TIMEOUT" "$@"
}

case "$RUN_NAME" in
	''|*[!A-Za-z0-9._-]*) printf 'RUN_NAME may contain only letters, digits, dot, underscore, and dash\n' >&2; exit 2 ;;
esac
case "$MAX_FRAMES" in
	''|*[!0-9]*) printf 'MAX_FRAMES must be a nonnegative integer\n' >&2; exit 2 ;;
esac
case "$BATCH_FRAMES" in
	''|*[!0-9]*) printf 'BATCH_FRAMES must be a positive integer\n' >&2; exit 2 ;;
esac
if [ "$BATCH_FRAMES" -eq 0 ]; then
	printf 'BATCH_FRAMES must be greater than zero\n' >&2
	exit 2
fi
case "$CONF" in
	''|*[!0-9.]*) printf 'CONF must be a decimal number in [0,1]\n' >&2; exit 2 ;;
esac
for DURATION in "$ADB_TIMEOUT" "$ADB_PREFLIGHT_TIMEOUT"; do
	case "$DURATION" in
		''|*[!0-9]*) printf 'ADB timeouts must be positive integer seconds\n' >&2; exit 2 ;;
	esac
	if [ "$DURATION" -le 0 ]; then
		printf 'ADB timeouts must be positive integer seconds\n' >&2
		exit 2
	fi
done
if ! command -v timeout >/dev/null 2>&1; then
	printf 'host timeout command is required\n' >&2
	exit 1
fi
if [ ! -f "$VIDEO" ] || [ ! -x "$PYTHON" ] || [ ! -f "$MODEL" ]; then
	printf 'video, Python environment, and RKNN model must exist\n' >&2
	exit 1
fi
if [ ! -x "$INFER_BIN" ]; then
	make -C "$SCRIPT_DIR" out/rknn_yolo26_infer
fi
if [ -n "$ZONES" ]; then
	ZONES=$(CDPATH='' cd -- "$(dirname -- "$ZONES")" && pwd)/$(basename -- "$ZONES")
	if [ ! -f "$ZONES" ] || [ ! -x "$TRACKER_BIN" ]; then
		printf 'zone config and cross-compiled tracking CLI must exist\n' >&2
		exit 1
	fi
	"$PYTHON" - "$ZONES" <<'PYCFG'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    if not json.load(source).get("geometry_confirmed", False):
        raise SystemExit("Refusing event processing until geometry_confirmed=true after camera review")
PYCFG
fi
if [ -e "$RUN_DIR" ]; then
	printf 'refusing to overwrite existing run: %s\n' "$RUN_DIR" >&2
	exit 1
fi
mkdir -p "$RUN_DIR"
if ! BOARD_MODEL=$(adb_bounded "$ADB_PREFLIGHT_TIMEOUT" -s "$SERIAL" shell 'tr -d "\000" < /proc/device-tree/model' 2>"$RUN_DIR/adb-preflight.log"); then
	cat "$RUN_DIR/adb-preflight.log" >&2
	printf 'ADB preflight failed; restore %s and use a new run name. Log: %s\n' "$SERIAL" "$RUN_DIR/adb-preflight.log" >&2
	exit 1
fi
BOARD_MODEL=$(printf '%s' "$BOARD_MODEL" | tr -d '\r\n')
printf '%s\n' "$BOARD_MODEL" >> "$RUN_DIR/adb-preflight.log"
case "$BOARD_MODEL" in
	*RV1126B*) ;;
	*) printf 'ADB preflight rejected unexpected board: %s\n' "$BOARD_MODEL" >&2; exit 1 ;;
esac
FRAMES="$RUN_DIR/frames.rgb"
METADATA="$RUN_DIR/frames.tsv"
DETECTIONS="$RUN_DIR/detections.jsonl"
BATCH_DIR="$RUN_DIR/batches"
if [ "$MAX_FRAMES" -gt 0 ]; then
	"$PYTHON" "$FLOW_ROOT/training/prepare_rknn_video.py" \
		--video "$VIDEO" --frames "$FRAMES" --metadata "$METADATA" --max-frames "$MAX_FRAMES"
else
	"$PYTHON" "$FLOW_ROOT/training/prepare_rknn_video.py" \
		--video "$VIDEO" --frames "$FRAMES" --metadata "$METADATA"
fi

"$PYTHON" "$FLOW_ROOT/training/split_rknn_video_batches.py" \
	--frames "$FRAMES" --metadata "$METADATA" --output-dir "$BATCH_DIR" \
	--batch-frames "$BATCH_FRAMES"

adb_cmd -s "$SERIAL" shell "test ! -e '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
adb_cmd -s "$SERIAL" push "$MODEL" "$REMOTE_DIR/model.rknn"
adb_cmd -s "$SERIAL" push "$INFER_BIN" "$REMOTE_DIR/rknn_yolo26_infer"
adb_cmd -s "$SERIAL" shell "chmod +x '$REMOTE_DIR/rknn_yolo26_infer'"
: > "$DETECTIONS"
: > "$RUN_DIR/infer.stdout"
: > "$RUN_DIR/infer.log"
for BATCH_RGB in "$BATCH_DIR"/batch-*.rgb; do
	BATCH_NAME=$(basename -- "$BATCH_RGB" .rgb)
	BATCH_METADATA="$BATCH_DIR/$BATCH_NAME.tsv"
	BATCH_REMOTE="$REMOTE_DIR/$BATCH_NAME"
	adb_cmd -s "$SERIAL" shell "mkdir -p '$BATCH_REMOTE'"
	adb_cmd -s "$SERIAL" push "$BATCH_RGB" "$BATCH_REMOTE/frames.rgb"
	adb_cmd -s "$SERIAL" push "$BATCH_METADATA" "$BATCH_REMOTE/frames.tsv"
	adb_cmd -s "$SERIAL" shell "cd '$BATCH_REMOTE' && LD_LIBRARY_PATH=/oem/usr/lib:/lib:/usr/lib ../rknn_yolo26_infer --model ../model.rknn --frames frames.rgb --metadata frames.tsv --detections detections.jsonl --conf '$CONF' --max-frames 0 >infer.stdout 2>infer.log"
	adb_cmd -s "$SERIAL" pull "$BATCH_REMOTE/detections.jsonl" "$BATCH_DIR/$BATCH_NAME.detections.jsonl"
	adb_cmd -s "$SERIAL" pull "$BATCH_REMOTE/infer.stdout" "$BATCH_DIR/$BATCH_NAME.infer.stdout"
	adb_cmd -s "$SERIAL" pull "$BATCH_REMOTE/infer.log" "$BATCH_DIR/$BATCH_NAME.infer.log"
	cat "$BATCH_DIR/$BATCH_NAME.detections.jsonl" >> "$DETECTIONS"
	cat "$BATCH_DIR/$BATCH_NAME.infer.stdout" >> "$RUN_DIR/infer.stdout"
	cat "$BATCH_DIR/$BATCH_NAME.infer.log" >> "$RUN_DIR/infer.log"
	adb_cmd -s "$SERIAL" shell "rm -f '$BATCH_REMOTE/frames.rgb' '$BATCH_REMOTE/frames.tsv' '$BATCH_REMOTE/detections.jsonl' '$BATCH_REMOTE/infer.stdout' '$BATCH_REMOTE/infer.log'"
done
"$PYTHON" - "$RUN_DIR/frames.rgb.json" "$DETECTIONS" "$BATCH_DIR/batches.json" <<'PYVERIFY'
import json, math, sys
from pathlib import Path
manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
batches = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
rows = [json.loads(line) for line in Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()]
if len(rows) != manifest["frame_count"] or len(rows) != batches["total_frames"]:
    raise SystemExit(f"detection frame count mismatch: {len(rows)} != {manifest['frame_count']}")
for index, row in enumerate(rows):
    if row.get("frame_index") != index or not math.isfinite(row.get("pts_ms", float("nan"))):
        raise SystemExit(f"invalid detector frame index/PTS at output row {index}")
PYVERIFY
"$PYTHON" - "$RUN_DIR/frames.rgb.json" "$BATCH_DIR/batches.json" "$MODEL" "$INFER_BIN" "$SERIAL" "$BOARD_MODEL" "$RUN_DIR/deployment.json" <<'PYDEPLOY'
import hashlib, json, sys
from pathlib import Path
manifest_path, batch_path, model_path, binary_path, serial, board_model, output_path = sys.argv[1:]
manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
batch_manifest = json.loads(Path(batch_path).read_text(encoding="utf-8"))
def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()
deployment = {
    "serial": serial,
    "board_model": board_model.strip(),
    "model": str(Path(model_path).resolve()),
    "model_sha256": digest(model_path),
    "inference_binary_sha256": digest(binary_path),
    "source_video": manifest["source_video"],
    "source_video_sha256": manifest["source_video_sha256"],
    "frames_rgb_sha256": manifest["frames_rgb_sha256"],
    "metadata_tsv_sha256": digest(Path(manifest_path).with_name("frames.tsv")),
    "frame_count": manifest["frame_count"],
    "partial": manifest["partial"],
    "batch_frames": batch_manifest["batch_frames"],
    "batch_count": batch_manifest["batch_count"],
    "batch_ranges": [{"first_frame": b["first_frame"], "frame_count": b["frame_count"]}
                     for b in batch_manifest["batches"]],
    "runtime_log": "infer.log",
}
Path(output_path).write_text(json.dumps(deployment, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PYDEPLOY

if [ -n "$ZONES" ]; then
	ZONE_RUN_CONFIG="$RUN_DIR/zones.json"
	"$PYTHON" "$FLOW_ROOT/tracking/session_config.py" \
		--profile "$ZONES" --video "$VIDEO" --session-id "$RUN_NAME" \
		--output "$ZONE_RUN_CONFIG"
	adb_cmd -s "$SERIAL" push "$TRACKER_BIN" "$REMOTE_DIR/flow_demo_cli"
	adb_cmd -s "$SERIAL" push "$ZONE_RUN_CONFIG" "$REMOTE_DIR/zones.json"
	adb_cmd -s "$SERIAL" push "$DETECTIONS" "$REMOTE_DIR/detections.jsonl"
	adb_cmd -s "$SERIAL" shell "cd '$REMOTE_DIR' && chmod +x flow_demo_cli && LD_LIBRARY_PATH=/lib:/usr/lib ./flow_demo_cli --config zones.json --detections detections.jsonl --tracks tracks.jsonl --events events.jsonl --summary summary.json >tracker.stdout 2>tracker.log"
	for artifact in tracks.jsonl events.jsonl summary.json tracker.stdout tracker.log; do
		adb_cmd -s "$SERIAL" pull "$REMOTE_DIR/$artifact" "$RUN_DIR/$artifact"
	done
fi
printf 'Board RKNN run artifacts: %s\n' "$RUN_DIR"
