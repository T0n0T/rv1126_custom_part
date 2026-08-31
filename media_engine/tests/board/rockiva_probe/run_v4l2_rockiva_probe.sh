#!/bin/sh
# Board-side native V4L2-to-RockIVA lifecycle probe (test tool only).
# Every capture-specific input is explicit; there is no default V4L2 device.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${DEVICE:?set DEVICE explicitly, for example /dev/video25}"
: "${MODEL_PATH:?set MODEL_PATH explicitly, for example /oem/usr/lib}"
: "${WIDTH:?set WIDTH explicitly}"
: "${HEIGHT:?set HEIGHT explicitly}"
: "${FRAMES:?set FRAMES explicitly}"

ROCKIVA_LIB_DIR=${ROCKIVA_LIB_DIR:-/oem/usr/lib}
BUFFERS=${BUFFERS:-4}
MODEL=${MODEL:-pfp}
CHANNEL=${CHANNEL:-0}
CORE_MASK=${CORE_MASK:-0}
TIMEOUT_MS=${TIMEOUT_MS:-5000}
MIN_PERSON=${MIN_PERSON:-0}
MIN_TRACKING=${MIN_TRACKING:-0}
ALLOW_MAINPATH=${ALLOW_MAINPATH:-0}

case "$ALLOW_MAINPATH" in
0) ALLOW_MAINPATH_ARG= ;;
1) ALLOW_MAINPATH_ARG=--allow-mainpath ;;
*)
	printf 'ALLOW_MAINPATH must be 0 or 1\n' >&2
	exit 2
	;;
esac

export LD_LIBRARY_PATH="${ROCKIVA_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

set -- \
	--device "$DEVICE" \
	--model-path "$MODEL_PATH" \
	--width "$WIDTH" \
	--height "$HEIGHT" \
	--frames "$FRAMES" \
	--buffers "$BUFFERS" \
	--model "$MODEL" \
	--channel "$CHANNEL" \
	--core-mask "$CORE_MASK" \
	--timeout-ms "$TIMEOUT_MS" \
	--min-person "$MIN_PERSON" \
	--min-tracking "$MIN_TRACKING"

if [ -n "$ALLOW_MAINPATH_ARG" ]; then
	set -- "$@" "$ALLOW_MAINPATH_ARG"
fi

exec "$SCRIPT_DIR/v4l2_rockiva_probe" "$@"
