#!/bin/sh
set -eu

PROBE=${1:?usage: test_probe.sh PROBE}
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

NORMAL_INPUT=$TMP_DIR/two-frames.nv12
SHORT_INPUT=$TMP_DIR/one-frame.nv12
EMPTY_INPUT=$TMP_DIR/empty.nv12
dd if=/dev/zero of="$NORMAL_INPUT" bs=12 count=1 status=none
dd if=/dev/zero of="$SHORT_INPUT" bs=6 count=1 status=none
: >"$EMPTY_INPUT"

run_probe()
{
	"$PROBE" --model-path "$TMP_DIR" --input "$1" --width 2 --height 2 \
		--frames 2 --fps 1000000 --timeout-ms 10
}

expect_failure()
{
	name=$1
	input=$2
	scenario=$3
	pattern=$4
	log=$TMP_DIR/$name.log

	if ROCKIVA_STUB_SCENARIO=$scenario run_probe "$input" >"$log" 2>&1; then
		echo "[FAIL] $name returned success" >&2
		cat "$log" >&2
		exit 1
	fi
	grep -F "$pattern" "$log" >/dev/null
	echo "[PASS] $name returns failure"
}

NORMAL_LOG=$TMP_DIR/normal.log
ROCKIVA_STUB_SCENARIO=normal run_probe "$NORMAL_INPUT" >"$NORMAL_LOG" 2>&1
grep -F "summary pushed=2 push_failures=0" "$NORMAL_LOG" >/dev/null
grep -F "released_frames=2" "$NORMAL_LOG" >/dev/null
echo "[PASS] normal callbacks and releases return success"

expect_failure init-failure "$NORMAL_INPUT" init_fail "ROCKIVA_Init failed"
expect_failure release-callback-failure "$NORMAL_INPUT" release_callback_fail \
	"ROCKIVA_SetFrameReleaseCallback failed"
expect_failure detect-init-failure "$NORMAL_INPUT" detect_init_fail \
	"ROCKIVA_DETECT_Init failed"
expect_failure empty-input "$EMPTY_INPUT" normal "input ended before frame 1"
expect_failure short-input "$SHORT_INPUT" normal "input ended before frame 2"
expect_failure push-failure "$NORMAL_INPUT" push_fail "push_failures=2"
expect_failure wait-failure "$NORMAL_INPUT" wait_fail \
	"frame ownership cleanup deferred after incomplete SDK shutdown"
expect_failure detect-release-failure "$NORMAL_INPUT" detect_release_fail \
	"detect_release ret=-1"
expect_failure release-failure "$NORMAL_INPUT" release_fail "release ret=-1"
