#!/bin/sh
# Board-side V4L2 EXPBUF capability probe (test tool only).
# The capture node and requested format are always explicit.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${DEVICE:?set DEVICE explicitly, for example /dev/video25}"
: "${WIDTH:?set WIDTH explicitly}"
: "${HEIGHT:?set HEIGHT explicitly}"
: "${FRAMES:?set FRAMES explicitly}"

ALLOW_MAINPATH=${ALLOW_MAINPATH:-0}
case "$ALLOW_MAINPATH" in
0) ALLOW_MAINPATH_ARG= ;;
1) ALLOW_MAINPATH_ARG=--allow-mainpath ;;
*)
	printf 'ALLOW_MAINPATH must be 0 or 1\n' >&2
	exit 2
	;;
esac

set -- --device "$DEVICE" --width "$WIDTH" --height "$HEIGHT" \
	--frames "$FRAMES"
if [ -n "$ALLOW_MAINPATH_ARG" ]; then
	set -- "$@" "$ALLOW_MAINPATH_ARG"
fi

exec "$SCRIPT_DIR/v4l2_expbuf_probe" "$@"
