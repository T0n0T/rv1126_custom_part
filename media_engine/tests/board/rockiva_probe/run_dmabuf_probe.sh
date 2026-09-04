#!/bin/sh
# Board-side RockIVA DMA-BUF probe (test tool only).
# SOURCE=v4l2 validates camera capture; SOURCE=mp4 validates decoder output.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${MODEL_PATH:?set MODEL_PATH explicitly, for example /oem/usr/lib}"

SOURCE=${SOURCE:-v4l2}

ROCKIVA_LIB_DIR=${ROCKIVA_LIB_DIR:-/oem/usr/lib}
MODEL=${MODEL:-pfp}
CHANNEL=${CHANNEL:-0}
CORE_MASK=${CORE_MASK:-0}
TIMEOUT_MS=${TIMEOUT_MS:-5000}
case "$SOURCE" in
v4l2)
	: "${DEVICE:?set DEVICE explicitly, for example /dev/video25}"
	: "${WIDTH:?set WIDTH explicitly}"
	: "${HEIGHT:?set HEIGHT explicitly}"
	: "${FRAMES:?set FRAMES explicitly}"
	: "${FPS:?set FPS explicitly}"
	MIN_PERSON=${MIN_PERSON:-0}
	MIN_TRACKING=${MIN_TRACKING:-0}
	;;
mp4)
	INPUT=${INPUT:-/tmp/me/test1.mp4}
	WIDTH=${WIDTH:-768}
	HEIGHT=${HEIGHT:-432}
	FRAMES=${FRAMES:-30}
	FPS=${FPS:-10}
	MIN_PERSON=${MIN_PERSON:-1}
	MIN_TRACKING=${MIN_TRACKING:-1}
	;;
*)
	printf 'SOURCE must be v4l2 or mp4\n' >&2
	exit 2
	;;
esac

ALLOW_MAINPATH=${ALLOW_MAINPATH:-0}

case "$ALLOW_MAINPATH" in
0) ALLOW_MAINPATH_ARG= ;;
1) ALLOW_MAINPATH_ARG=--allow-mainpath ;;
*)
	printf 'ALLOW_MAINPATH must be 0 or 1\n' >&2
	exit 2
	;;
esac

if [ "$SOURCE" = mp4 ] && [ "$ALLOW_MAINPATH" != 0 ]; then
	printf 'ALLOW_MAINPATH is only supported with SOURCE=v4l2\n' >&2
	exit 2
fi

export LD_LIBRARY_PATH="${ROCKIVA_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-${ROCKIVA_LIB_DIR}/gstreamer-1.0}"
if [ "$SOURCE" = mp4 ]; then
	export GST_PLUGIN_SCANNER="${GST_PLUGIN_SCANNER:-/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner}"
fi

set -- \
	--model-path "$MODEL_PATH" \
	--width "$WIDTH" \
	--height "$HEIGHT" \
	--frames "$FRAMES" \
	--fps "$FPS" \
	--model "$MODEL" \
	--channel "$CHANNEL" \
	--core-mask "$CORE_MASK" \
	--timeout-ms "$TIMEOUT_MS" \
	--min-person "$MIN_PERSON" \
	--min-tracking "$MIN_TRACKING"

if [ "$SOURCE" = mp4 ]; then
	set -- "$@" --input "$INPUT"
else
	set -- "$@" --device "$DEVICE"
fi

if [ -n "$ALLOW_MAINPATH_ARG" ]; then
	set -- "$@" "$ALLOW_MAINPATH_ARG"
fi

exec "$SCRIPT_DIR/rockiva_dmabuf_probe" "$@"
