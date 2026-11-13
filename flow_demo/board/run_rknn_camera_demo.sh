#!/bin/sh
# Deploy one isolated live run, then manage its lifetime without an open ADB shell.
set -eu

if [ "$#" -ne 2 ]; then
	printf 'usage: %s start|status|stop RUN_NAME\n' "$0" >&2
	exit 2
fi
ACTION=$1
RUN_NAME=$2
case "$ACTION" in start|status|stop) ;; *) exit 2 ;; esac
case "$RUN_NAME" in ''|.|..|*[!A-Za-z0-9._-]*) printf 'invalid RUN_NAME\n' >&2; exit 2 ;; esac
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
FLOW_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
CONFIG=${CONFIG:-"$SCRIPT_DIR/camera.env"}
# This is a local shell configuration owned by the operator, not board input.
# shellcheck disable=SC1090
. "$CONFIG"
SCREEN_ROTATION=${SCREEN_ROTATION:-270}
CAMERA_COLOR_ORDER=${CAMERA_COLOR_ORDER:-BGR}
SERIAL=${ADB_SERIAL:-192.168.1.63:5555}
ADB_PORT=${ADB_SERVER_PORT:-5038}
ADB_BIN=${ADB:-adb}
ADB_TIMEOUT=${ADB_TIMEOUT:-120}
MODEL=${MODEL:-"$FLOW_ROOT/runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn"}
ZONES_PROFILE=${ZONES_PROFILE:-"$SCRIPT_DIR/camera-zones-debug.json"}
RUN_DIR="$FLOW_ROOT/runs/board-camera/$RUN_NAME"
REMOTE_DIR="/tmp/flow_demo/board-camera/$RUN_NAME"
if [ "$ACTION" = start ]; then
	REMOTE_DIR="/userdata/flow_demo/board-camera/$RUN_NAME"
elif [ -f "$RUN_DIR/remote-run.txt" ]; then
	IFS= read -r REMOTE_DIR < "$RUN_DIR/remote-run.txt"
	case "$REMOTE_DIR" in
		"/userdata/flow_demo/board-camera/$RUN_NAME"|"/tmp/flow_demo/board-camera/$RUN_NAME") ;;
		*) printf 'invalid saved run path\n' >&2; exit 2 ;;
	esac
fi

adb_run() {
	timeout -k 5 "$ADB_TIMEOUT" "$ADB_BIN" -P "$ADB_PORT" -s "$SERIAL" "$@"
}

collect() {
	ARTIFACTS='console.log detections.jsonl stats.json preview.ppm exit_status'
	if [ -f "$RUN_DIR/zones.json" ]; then ARTIFACTS="$ARTIFACTS tracks.jsonl events.jsonl summary.json"; fi
	if [ -f "$RUN_DIR/run.sh" ]; then
		case "$(cat "$RUN_DIR/run.sh")" in *--source-snapshot*) ARTIFACTS="$ARTIFACTS source.ppm" ;; esac
	fi
	for artifact in $ARTIFACTS; do
		if [ ! -e "$RUN_DIR/$artifact" ]; then
			adb_run pull "$REMOTE_DIR/$artifact" "$RUN_DIR/$artifact"
		fi
	done
	if [ "$(tr -d '\r\n ' < "$RUN_DIR/exit_status")" != 0 ]; then
		tail -25 "$RUN_DIR/console.log" >&2
		printf 'camera run failed; evidence: %s\n' "$RUN_DIR" >&2
		exit 1
	fi
	python3 - "$RUN_DIR" <<'PY'
import json
from pathlib import Path
import sys
root = Path(sys.argv[1])
stats = json.loads((root / 'stats.json').read_text())
rows = [json.loads(line) for line in (root / 'detections.jsonl').read_text().splitlines()]
assert rows and len(rows) == stats['processed'], 'incomplete frame output'
assert stats['captured'] == stats['warmup'] + stats['processed'] + stats['skipped']
assert stats['clean_display_drain'] and stats['rendered'] > 0, 'display did not render'
assert all(a['frame_index'] < b['frame_index'] and a['pts_ms'] <= b['pts_ms'] for a, b in zip(rows, rows[1:]))
if 'preview_frames' in stats:
    assert stats['dma_output_frames'] == stats['preview_frames'], 'preview left the DMA-BUF path'
if (root / 'summary.json').exists():
    summary = json.loads((root / 'summary.json').read_text())
    tracks = [json.loads(line) for line in (root / 'tracks.jsonl').read_text().splitlines()]
    assert len(tracks) == len(rows) and tracks[-1]['frame_index'] == summary['processed_through_frame']
    assert all(a['frame_index'] == b['frame_index'] and a['pts_ms'] == b['pts_ms'] for a, b in zip(rows, tracks))
    journal = [json.loads(line) for line in (root / 'events.jsonl').read_text().splitlines()]
    latest, totals, uncertain = {}, {'take_out': 0, 'put_in': 0}, 0
    for event in journal:
        if event['status'] == 'uncertain':
            uncertain += 1
            continue
        assert event['status'] == ('confirmed' if summary['geometry_confirmed'] else 'debug')
        assert event['op'] == 'upsert'
        previous = latest.get(event['event_id'], 0)
        assert event['delta_count'] == event['count'] - previous and event['delta_count'] > 0
        latest[event['event_id']] = event['count']
        totals[event['direction']] += event['delta_count']
    prefix = 'confirmed_' if summary['geometry_confirmed'] else 'debug_'
    assert all(summary[prefix + direction] == count for direction, count in totals.items())
    assert uncertain == summary['uncertain_count']
    if not summary['geometry_confirmed']:
        assert summary['confirmed_take_out'] == summary['confirmed_put_in'] == 0
    assert summary.get('run_ok', True), 'event session reported a runtime error'
    print(json.dumps(summary, ensure_ascii=False, indent=2))
print(json.dumps(stats, ensure_ascii=False, indent=2))
PY
}

case "$ACTION" in
start)
	for value in "$CAMERA_ID" "$CAPTURE_WIDTH" "$CAPTURE_HEIGHT" "$CAPTURE_FPS" "$CAMERA_ROTATION" "$SCREEN_ROTATION" \
		"$CONNECTOR_ID" "$PLANE_ID" "$SCREEN_WIDTH" "$SCREEN_HEIGHT" "$MAX_FRAMES" "$ADB_TIMEOUT" "$ADB_PORT"; do
		case "$value" in ''|*[!0-9]*) printf 'camera numeric option is invalid\n' >&2; exit 2 ;; esac
	done
	case "$CONF" in ''|*[!0-9.]*) printf 'invalid CONF\n' >&2; exit 2 ;; esac
	case "$CAMERA_COLOR_ORDER" in RGB|BGR) ;; *) printf 'CAMERA_COLOR_ORDER must be RGB or BGR\n' >&2; exit 2 ;; esac
	case "$CAMERA_DEVICE" in /dev/video*) ;; *) printf 'invalid CAMERA_DEVICE\n' >&2; exit 2 ;; esac
	for value in "$CAMERA_DEVICE" "$IQ_DIR"; do
		case "$value" in ''|*[!A-Za-z0-9/._-]*) printf 'invalid device/IQ path\n' >&2; exit 2 ;; esac
	done
	[ -f "$MODEL" ] || { printf 'missing RKNN model: %s\n' "$MODEL" >&2; exit 1; }
	[ -f "$ZONES_PROFILE" ] || { printf 'missing camera zones profile: %s\n' "$ZONES_PROFILE" >&2; exit 1; }
	case "$DEBUG_ZONES" in 0|1) ;; *) printf 'DEBUG_ZONES must be 0 or 1\n' >&2; exit 2 ;; esac
	GST_DEBUG=${GST_DEBUG:-}
	case "$GST_DEBUG" in *[!A-Za-z0-9,:*._-]*) printf 'invalid GST_DEBUG\n' >&2; exit 2 ;; esac
	DEBUG_ARG=
	[ "$DEBUG_ZONES" -eq 0 ] || DEBUG_ARG=--debug-zones
	[ ! -e "$RUN_DIR" ] || { printf 'run already exists: %s\n' "$RUN_DIR" >&2; exit 1; }
	case "$SERIAL" in *:*) timeout -k 5 "$ADB_TIMEOUT" "$ADB_BIN" -P "$ADB_PORT" connect "$SERIAL" ;; esac
	PREFLIGHT=$(adb_run shell "
		cat /proc/device-tree/model
		for process in /proc/[0-9]*; do
			if test -r \"\$process/comm\"; then
				IFS= read -r name < \"\$process/comm\" || continue
				if test \"\$name\" = media_engine; then
					echo 'media_engine is using the camera/screen; stop it with SIGTERM first'
					exit 1
				fi
			fi
		done
		if ! test -c '$CAMERA_DEVICE' || ! test -c /dev/dri/card0; then exit 1; fi
		for fd in /proc/[0-9]*/fd/*; do
			target=\$(readlink \"\$fd\" 2>/dev/null || true)
			case \"\$target\" in /dev/dri/card0|'$CAMERA_DEVICE') echo \"busy: \$fd -> \$target\"; exit 1 ;; esac
		done
		echo FLOW_CAMERA_PREFLIGHT_OK" | tr -d '\000\r')
	case "$PREFLIGHT" in *RV1126B*FLOW_CAMERA_PREFLIGHT_OK*) ;; *) printf '%s\n' "$PREFLIGHT" >&2; exit 1 ;; esac
	make -C "$SCRIPT_DIR" out/rknn_yolo26_infer
	mkdir -p "$FLOW_ROOT/runs/board-camera"
	mkdir "$RUN_DIR"
	printf '%s\n' "$REMOTE_DIR" > "$RUN_DIR/remote-run.txt"
	cp "$CONFIG" "$RUN_DIR/camera.env"
	cp "$ZONES_PROFILE" "$RUN_DIR/zones.json"
	sha256sum "$MODEL" "$SCRIPT_DIR/out/rknn_yolo26_infer" > "$RUN_DIR/sha256.txt"
	MODEL_SHA=$(sha256sum "$MODEL" | awk '{print $1}')
	MODEL_MD5=$(md5sum "$MODEL" | awk '{print $1}')
	REMOTE_MODEL="/userdata/flow_demo/models/$MODEL_SHA.rknn"
	CACHED=$(adb_run shell "mkdir -p /userdata/flow_demo/models
		if test -e '$REMOTE_MODEL'; then md5sum '$REMOTE_MODEL'; else echo FLOW_MODEL_MISSING; fi" | tr -d '\r')
	case "$CACHED" in
		FLOW_MODEL_MISSING)
			adb_run push "$MODEL" "$REMOTE_MODEL"
			CACHED=$(adb_run shell "md5sum '$REMOTE_MODEL'" | tr -d '\r')
			;;
	esac
	case "$CACHED" in "$MODEL_MD5 "*) ;; *) printf 'cached model integrity check failed: %s\n' "$REMOTE_MODEL" >&2; exit 1 ;; esac
	printf '%s\n' "$REMOTE_MODEL" > "$RUN_DIR/remote-model.txt"
	RESERVED=$(adb_run shell "mkdir -p /userdata/flow_demo/board-camera
		if mkdir '$REMOTE_DIR'; then echo FLOW_CAMERA_RESERVED; fi" | tr -d '\r')
	case "$RESERVED" in *FLOW_CAMERA_RESERVED*) ;; *) printf '%s\n' "$RESERVED" >&2; exit 1 ;; esac
	cat > "$RUN_DIR/run.sh" <<EOF
#!/bin/sh
cd '$REMOTE_DIR' || exit 1
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_PATH=/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner
export GST_DEBUG='$GST_DEBUG'
./rknn_yolo26_infer --model model.rknn --camera '$CAMERA_DEVICE' --camera-id '$CAMERA_ID' \\
 --iq-dir '$IQ_DIR' --width '$CAPTURE_WIDTH' --height '$CAPTURE_HEIGHT' --fps '$CAPTURE_FPS' \\
 --rotation '$CAMERA_ROTATION' --camera-color-order '$CAMERA_COLOR_ORDER' --connector-id '$CONNECTOR_ID' --plane-id '$PLANE_ID' \\
 --screen-width '$SCREEN_WIDTH' --screen-height '$SCREEN_HEIGHT' --screen-rotation '$SCREEN_ROTATION' --conf '$CONF' \\
 --max-frames '$MAX_FRAMES' --detections detections.jsonl --stats stats.json --snapshot preview.ppm \\
 --zones zones.json --session-id '$RUN_NAME' --event-dir . $DEBUG_ARG --source-snapshot source.ppm \\
 > console.log 2>&1 &
demo_pid=\$!
printf '%s\n' "\$demo_pid" > demo.pid
wait "\$demo_pid"
printf '%s\n' "\$?" > exit_status
EOF
	adb_run push "$SCRIPT_DIR/out/rknn_yolo26_infer" "$REMOTE_DIR/rknn_yolo26_infer"
	adb_run shell "ln -s '$REMOTE_MODEL' '$REMOTE_DIR/model.rknn'"
	adb_run push "$RUN_DIR/run.sh" "$REMOTE_DIR/run.sh"
	adb_run push "$RUN_DIR/zones.json" "$REMOTE_DIR/zones.json"
	# Let nohup install its SIGHUP handler before this older ADB shell closes its PTY.
	adb_run shell "cd '$REMOTE_DIR' || exit 1
		nohup sh ./run.sh > launcher.log 2>&1 </dev/null &
		sleep 1"
	HEALTH=$(adb_run shell "
		for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
			if test -s '$REMOTE_DIR/detections.jsonl' && test -s '$REMOTE_DIR/preview.ppm'; then
				echo FLOW_CAMERA_READY; break
			fi
			test -f '$REMOTE_DIR/exit_status' && break
			sleep 1
		done
		tail -8 '$REMOTE_DIR/console.log'" | tr -d '\r')
	case "$HEALTH" in *FLOW_CAMERA_READY*) ;; *) printf 'camera startup failed: %s\n' "$HEALTH" >&2; exit 1 ;; esac
	printf 'Started %s on %s; config and launch command: %s\n' "$RUN_NAME" "$SERIAL" "$RUN_DIR"
	printf 'Status: %s status %s\nStop and collect: %s stop %s\n' "$0" "$RUN_NAME" "$0" "$RUN_NAME"
	;;
status)
	adb_run shell "
		if test -f '$REMOTE_DIR/exit_status'; then
			echo finished; cat '$REMOTE_DIR/exit_status'; cat '$REMOTE_DIR/stats.json'
		else
			pid=\$(cat '$REMOTE_DIR/demo.pid')
			case \"\$pid\" in ''|*[!0-9]*) echo 'invalid pid'; exit 1 ;; esac
			if test \"\$(readlink /proc/\$pid/exe)\" = '$REMOTE_DIR/rknn_yolo26_infer'; then
				echo \"running PID=\$pid\"
			else echo 'demo is not running'; fi
		fi
		tail -12 '$REMOTE_DIR/console.log'"
	;;
stop)
	[ -d "$RUN_DIR" ] || { printf 'local run not found: %s\n' "$RUN_DIR" >&2; exit 1; }
	STOPPED=$(adb_run shell "
		if ! test -f '$REMOTE_DIR/exit_status'; then
			pid=\$(cat '$REMOTE_DIR/demo.pid')
			case \"\$pid\" in ''|*[!0-9]*) echo 'invalid pid'; exit 1 ;; esac
			if test \"\$(readlink /proc/\$pid/exe)\" != '$REMOTE_DIR/rknn_yolo26_infer'; then
				echo 'PID does not belong to this run'; exit 1
			fi
			kill -TERM \"\$pid\"
			for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
				test -f '$REMOTE_DIR/exit_status' && break
				sleep 1
			done
		fi
		if test -f '$REMOTE_DIR/exit_status'; then echo FLOW_CAMERA_STOPPED; fi" | tr -d '\r')
	case "$STOPPED" in *FLOW_CAMERA_STOPPED*) ;; *) printf 'did not stop cleanly: %s\n' "$STOPPED" >&2; exit 1 ;; esac
	collect
	;;
esac
