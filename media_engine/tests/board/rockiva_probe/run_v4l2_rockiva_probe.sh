#!/bin/sh
# Board-side RockIVA people-flow probe (test tool only).
# SOURCE=v4l2 uses the native capture lifecycle; SOURCE=mp4 uses the same
# executable and RockIVA callbacks with a GStreamer-decoded test clip.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${MODEL_PATH:?set MODEL_PATH explicitly, for example /oem/usr/lib}"
SOURCE=${SOURCE:-v4l2}
ROCKIVA_LIB_DIR=${ROCKIVA_LIB_DIR:-/oem/usr/lib}
MODEL=${MODEL:-pfp}
CHANNEL=${CHANNEL:-0}
CORE_MASK=${CORE_MASK:-0}
TIMEOUT_MS=${TIMEOUT_MS:-5000}
LOG_LEVEL=${LOG_LEVEL:-all}
REPORT_INTERVAL_MS=${REPORT_INTERVAL_MS:-5000}
ALLOW_MAINPATH=${ALLOW_MAINPATH:-0}
CONTINUOUS=${CONTINUOUS:-0}
DISPLAY_OUTPUT=${DISPLAY_OUTPUT:-1}
CONNECTOR_ID=${CONNECTOR_ID:-97}
PLANE_ID=${PLANE_ID:-75}
PREVIEW_ROTATION=${PREVIEW_ROTATION:-0}
PREVIEW_WIDTH=${PREVIEW_WIDTH:-480}
PREVIEW_HEIGHT=${PREVIEW_HEIGHT:-800}

case "$SOURCE" in
v4l2)
	: "${DEVICE:?set DEVICE explicitly, for example /dev/video25}"
	: "${WIDTH:?set WIDTH explicitly}"
	: "${HEIGHT:?set HEIGHT explicitly}"
	BUFFERS=${BUFFERS:-4}
	MIN_PERSON=${MIN_PERSON:-0}
	MIN_TRACKING=${MIN_TRACKING:-0}
	;;
mp4)
	INPUT=${INPUT:-/tmp/me/real_time.mp4}
	WIDTH=${WIDTH:-640}
	HEIGHT=${HEIGHT:-640}
	FPS=${FPS:-10}
	FRAMES=${FRAMES:-30}
	MIN_PERSON=${MIN_PERSON:-1}
	MIN_TRACKING=${MIN_TRACKING:-1}
	;;
*)
	printf 'SOURCE must be v4l2 or mp4\n' >&2
	exit 2
	;;
esac

case "$ALLOW_MAINPATH" in
0) ALLOW_MAINPATH_ARG= ;;
1) ALLOW_MAINPATH_ARG=--allow-mainpath ;;
*)
	printf 'ALLOW_MAINPATH must be 0 or 1\n' >&2
	exit 2
	;;
esac

case "$CONTINUOUS" in
0)
	if [ "$SOURCE" = v4l2 ]; then
		: "${FRAMES:?set FRAMES explicitly unless CONTINUOUS=1}"
	fi
	CONTINUOUS_ARG=
	;;
1)
	if [ "$SOURCE" != v4l2 ]; then
		printf 'CONTINUOUS is only supported with SOURCE=v4l2\n' >&2
		exit 2
	fi
	if [ "$ALLOW_MAINPATH" != 0 ]; then
		printf 'continuous mode cannot enable ALLOW_MAINPATH; /dev/video24 is forbidden\n' >&2
		exit 2
	fi
	CONTINUOUS_ARG=--continuous
	;;
*)
	printf 'CONTINUOUS must be 0 or 1\n' >&2
	exit 2
	;;
esac

if [ "$SOURCE" = mp4 ] && [ "$ALLOW_MAINPATH" != 0 ]; then
	printf 'ALLOW_MAINPATH is only supported with SOURCE=v4l2\n' >&2
	exit 2
fi

case "$DISPLAY_OUTPUT" in
0) DISPLAY_ARG=--no-display ;;
1) DISPLAY_ARG=--display ;;
*)
	printf 'DISPLAY_OUTPUT must be 0 or 1\n' >&2
	exit 2
	;;
esac

export LD_LIBRARY_PATH="${ROCKIVA_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

set -- \
	--model-path "$MODEL_PATH" \
	--width "$WIDTH" \
	--height "$HEIGHT" \
	--model "$MODEL" \
	--channel "$CHANNEL" \
	--core-mask "$CORE_MASK" \
	--timeout-ms "$TIMEOUT_MS" \
	--log-level "$LOG_LEVEL" \
	--report-interval-ms "$REPORT_INTERVAL_MS" \
	--min-person "$MIN_PERSON" \
	--min-tracking "$MIN_TRACKING"

if [ "$SOURCE" = mp4 ]; then
	export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-${ROCKIVA_LIB_DIR}/gstreamer-1.0}"
	export GST_PLUGIN_SCANNER="${GST_PLUGIN_SCANNER:-/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner}"
	set -- "$@" --input "$INPUT" --fps "$FPS" --frames "$FRAMES" \
		--connector-id "$CONNECTOR_ID" --plane-id "$PLANE_ID" \
		--preview-rotation "$PREVIEW_ROTATION" \
		--preview-width "$PREVIEW_WIDTH" --preview-height "$PREVIEW_HEIGHT" \
		"$DISPLAY_ARG"
else
	set -- "$@" --device "$DEVICE" --buffers "$BUFFERS"
	if [ -n "$CONTINUOUS_ARG" ]; then
		set -- "$@" "$CONTINUOUS_ARG"
	else
		set -- "$@" --frames "$FRAMES"
	fi
fi

if [ -n "$ALLOW_MAINPATH_ARG" ]; then
	set -- "$@" "$ALLOW_MAINPATH_ARG"
fi

exec "$SCRIPT_DIR/v4l2_rockiva_probe" "$@"
