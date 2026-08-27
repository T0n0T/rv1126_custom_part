#!/bin/sh
# Board-side RockIVA probe (test tool only). Run from the copied probe folder.
set -eu

MODEL_PATH=${MODEL_PATH:-/oem/usr/lib}
ROCKIVA_LIB_DIR=${ROCKIVA_LIB_DIR:-/oem/usr/lib}
INPUT=${INPUT:-/tmp/me/input.nv12}
WIDTH=${WIDTH:-640}
HEIGHT=${HEIGHT:-360}
FRAMES=${FRAMES:-30}
FPS=${FPS:-10}
MODEL=${MODEL:-pfp}
CHANNEL=${CHANNEL:-0}
CORE_MASK=${CORE_MASK:-0}
TIMEOUT_MS=${TIMEOUT_MS:-5000}

export LD_LIBRARY_PATH="${ROCKIVA_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "$(dirname "$0")/rockiva_probe" \
	--model-path "$MODEL_PATH" \
	--input "$INPUT" \
	--model "$MODEL" \
	--width "$WIDTH" \
	--height "$HEIGHT" \
	--frames "$FRAMES" \
	--fps "$FPS" \
	--channel "$CHANNEL" \
	--core-mask "$CORE_MASK" \
	--timeout-ms "$TIMEOUT_MS"
