#!/bin/sh
# Board-side RKNN runtime smoke test.  Run from the copied board directory.
set -eu

MODEL=${MODEL:-/tmp/flow_demo/helmet-yolov8n-rv1126b-fp16.rknn}
INPUT=${INPUT:-/tmp/flow_demo/sample.rgb}
RUNS=${RUNS:-5}
WANT_FLOAT=${WANT_FLOAT:-1}
RKNN_LIB_DIR=${RKNN_LIB_DIR:-/oem/usr/lib}

export LD_LIBRARY_PATH="${RKNN_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec "$(dirname "$0")/out/rknn_smoke" \
	--model "$MODEL" \
	--input "$INPUT" \
	--runs "$RUNS" \
	--want-float "$WANT_FLOAT"
