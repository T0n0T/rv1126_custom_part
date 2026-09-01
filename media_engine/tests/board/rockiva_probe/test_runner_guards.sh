#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

PATH_VALUE=${PATH:-/usr/bin:/bin}

fail()
{
	printf '[FAIL] %s\n' "$*" >&2
	exit 1
}

expect_failure_contains()
{
	name=$1
	pattern=$2
	shift 2
	log=$TMP_DIR/$name.log
	status=0
	"$@" >"$log" 2>&1 || status=$?
	if [ "$status" -eq 0 ]; then
		cat "$log" >&2
		fail "$name returned success"
	fi
	if ! grep -F "$pattern" "$log" >/dev/null; then
		cat "$log" >&2
		fail "$name did not contain: $pattern"
	fi
	printf '[PASS] %s\n' "$name"
}

expect_contains()
{
	name=$1
	pattern=$2
	file=$3
	if ! grep -F "$pattern" "$file" >/dev/null; then
		cat "$file" >&2
		fail "$name did not contain: $pattern"
	fi
	printf '[PASS] %s\n' "$name"
}

expect_absent()
{
	name=$1
	pattern=$2
	file=$3
	if grep -F "$pattern" "$file" >/dev/null; then
		cat "$file" >&2
		fail "$name unexpectedly contained: $pattern"
	fi
	printf '[PASS] %s\n' "$name"
}

make_fake_probe()
{
	target=$1
	printf '%s\n' \
		'#!/bin/sh' \
		'{ ' \
		'  printf "argv_count=%s\\n" "$#"' \
		'  for arg do printf "arg=%s\\n" "$arg"; done' \
		'  printf "LD_LIBRARY_PATH=%s\\n" "${LD_LIBRARY_PATH-}"' \
		'  printf "GST_PLUGIN_PATH=%s\\n" "${GST_PLUGIN_PATH-}"' \
		'} > "${TRACE_FILE:?TRACE_FILE must be set}"' \
		>"$target"
	chmod 755 "$target"
}

prepare_runner()
{
	name=$1
	runner=$2
	target=$3
	dir=$TMP_DIR/$name
	mkdir "$dir"
	cp "$SCRIPT_DIR/$runner" "$dir/$runner"
	make_fake_probe "$dir/$target"
	printf '%s\n' "$dir"
}

EXPBUF_DIR=$(prepare_runner expbuf run_v4l2_expbuf_probe.sh v4l2_expbuf_probe)
ROCKIVA_DIR=$(prepare_runner v4l2-rockiva run_v4l2_rockiva_probe.sh v4l2_rockiva_probe)
DMABUF_DIR=$(prepare_runner dmabuf run_dmabuf_probe.sh rockiva_dmabuf_probe)

expect_failure_contains expbuf-missing-device 'set DEVICE explicitly' \
	env -i PATH="$PATH_VALUE" sh "$EXPBUF_DIR/run_v4l2_expbuf_probe.sh"
expect_failure_contains v4l2-rockiva-missing-model 'set MODEL_PATH explicitly' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath WIDTH=640 HEIGHT=360 FRAMES=1 \
	sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_failure_contains dmabuf-missing-fps 'set FPS explicitly' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath MODEL_PATH=/tmp/model \
	WIDTH=640 HEIGHT=360 FRAMES=1 sh "$DMABUF_DIR/run_dmabuf_probe.sh"

expect_failure_contains expbuf-invalid-mainpath 'ALLOW_MAINPATH must be 0 or 1' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath WIDTH=640 HEIGHT=360 \
	FRAMES=1 ALLOW_MAINPATH=2 \
	sh "$EXPBUF_DIR/run_v4l2_expbuf_probe.sh"
expect_failure_contains v4l2-rockiva-invalid-mainpath 'ALLOW_MAINPATH must be 0 or 1' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath WIDTH=640 HEIGHT=360 \
	FRAMES=1 MODEL_PATH=/tmp/model ALLOW_MAINPATH=2 \
	sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_failure_contains v4l2-rockiva-continuous-mainpath-forbidden \
	'continuous mode cannot enable ALLOW_MAINPATH' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath MODEL_PATH=/tmp/model \
	WIDTH=640 HEIGHT=360 CONTINUOUS=1 ALLOW_MAINPATH=1 \
	sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_failure_contains dmabuf-invalid-mainpath 'ALLOW_MAINPATH must be 0 or 1' \
	env -i PATH="$PATH_VALUE" DEVICE=/tmp/selfpath WIDTH=640 HEIGHT=360 \
	FRAMES=1 MODEL_PATH=/tmp/model FPS=10 ALLOW_MAINPATH=2 \
	sh "$DMABUF_DIR/run_dmabuf_probe.sh"

EXPBUF_TRACE=$TMP_DIR/expbuf.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$EXPBUF_TRACE" \
	DEVICE=/tmp/selfpath WIDTH=640 HEIGHT=360 FRAMES=4 \
	sh "$EXPBUF_DIR/run_v4l2_expbuf_probe.sh"
expect_contains expbuf-safe-device 'arg=/tmp/selfpath' "$EXPBUF_TRACE"
expect_absent expbuf-default-mainpath-guard 'arg=--allow-mainpath' "$EXPBUF_TRACE"

ROCKIVA_TRACE=$TMP_DIR/v4l2-rockiva.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$ROCKIVA_TRACE" \
	DEVICE=/tmp/selfpath MODEL_PATH=/tmp/model ROCKIVA_LIB_DIR=/opt/rockiva \
	WIDTH=640 HEIGHT=360 FRAMES=4 MIN_PERSON=1 MIN_TRACKING=1 \
	sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_contains v4l2-rockiva-safe-device 'arg=/tmp/selfpath' "$ROCKIVA_TRACE"
expect_contains v4l2-rockiva-thresholds 'arg=1' "$ROCKIVA_TRACE"
expect_contains v4l2-rockiva-library-path 'LD_LIBRARY_PATH=/opt/rockiva' "$ROCKIVA_TRACE"
expect_contains v4l2-rockiva-finite-frame-limit 'arg=--frames' "$ROCKIVA_TRACE"

ROCKIVA_CONTINUOUS_TRACE=$TMP_DIR/v4l2-rockiva-continuous.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$ROCKIVA_CONTINUOUS_TRACE" \
	DEVICE=/tmp/selfpath MODEL_PATH=/tmp/model ROCKIVA_LIB_DIR=/opt/rockiva \
	WIDTH=640 HEIGHT=360 FRAMES=999 CONTINUOUS=1 LOG_LEVEL=events \
	REPORT_INTERVAL_MS=1200 sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_contains v4l2-rockiva-continuous-mode 'arg=--continuous' \
	"$ROCKIVA_CONTINUOUS_TRACE"
expect_absent v4l2-rockiva-continuous-no-frame-limit 'arg=--frames' \
	"$ROCKIVA_CONTINUOUS_TRACE"
expect_contains v4l2-rockiva-log-level 'arg=events' "$ROCKIVA_CONTINUOUS_TRACE"
expect_contains v4l2-rockiva-report-interval 'arg=1200' \
	"$ROCKIVA_CONTINUOUS_TRACE"

DMABUF_TRACE=$TMP_DIR/dmabuf.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$DMABUF_TRACE" \
	DEVICE=/tmp/selfpath MODEL_PATH=/tmp/model ROCKIVA_LIB_DIR=/opt/rockiva \
	WIDTH=640 HEIGHT=360 FRAMES=4 FPS=10 \
	sh "$DMABUF_DIR/run_dmabuf_probe.sh"
expect_contains dmabuf-safe-device 'arg=/tmp/selfpath' "$DMABUF_TRACE"
expect_contains dmabuf-plugin-path \
	'GST_PLUGIN_PATH=/opt/rockiva/gstreamer-1.0' "$DMABUF_TRACE"

EXPBUF_MAINPATH_TRACE=$TMP_DIR/expbuf-mainpath.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$EXPBUF_MAINPATH_TRACE" \
	DEVICE=/dev/video24 WIDTH=640 HEIGHT=360 FRAMES=4 ALLOW_MAINPATH=1 \
	sh "$EXPBUF_DIR/run_v4l2_expbuf_probe.sh"
expect_contains expbuf-explicit-mainpath 'arg=--allow-mainpath' "$EXPBUF_MAINPATH_TRACE"

EXPBUF_ALIAS_TRACE=$TMP_DIR/expbuf-alias.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$EXPBUF_ALIAS_TRACE" \
	DEVICE=/dev/./video24 WIDTH=640 HEIGHT=360 FRAMES=4 \
	sh "$EXPBUF_DIR/run_v4l2_expbuf_probe.sh"
expect_contains expbuf-mainpath-alias-forwarded 'arg=/dev/./video24' \
	"$EXPBUF_ALIAS_TRACE"

ROCKIVA_ALIAS_TRACE=$TMP_DIR/v4l2-rockiva-alias.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$ROCKIVA_ALIAS_TRACE" \
	DEVICE=/dev/../dev/video24 MODEL_PATH=/tmp/model WIDTH=640 HEIGHT=360 \
	FRAMES=4 sh "$ROCKIVA_DIR/run_v4l2_rockiva_probe.sh"
expect_contains v4l2-rockiva-mainpath-alias-forwarded 'arg=/dev/../dev/video24' \
	"$ROCKIVA_ALIAS_TRACE"

DMABUF_ALIAS_TRACE=$TMP_DIR/dmabuf-alias.trace
env -i PATH="$PATH_VALUE" TRACE_FILE="$DMABUF_ALIAS_TRACE" \
	DEVICE=/dev/video24 MODEL_PATH=/tmp/model WIDTH=640 HEIGHT=360 \
	FRAMES=4 FPS=10 sh "$DMABUF_DIR/run_dmabuf_probe.sh"
expect_contains dmabuf-mainpath-canonical-forwarded 'arg=/dev/video24' \
	"$DMABUF_ALIAS_TRACE"

for source in v4l2_expbuf_probe.c v4l2_rockiva_probe.c rockiva_dmabuf_probe.c; do
	expect_contains "$source-mainpath-canonicalization" \
		'rockiva_probe_is_mainpath(options->device' "$SCRIPT_DIR/$source"
done

for source in v4l2_expbuf_probe.c v4l2_rockiva_probe.c; do
	expect_contains "$source-opened-device-identity" \
		'rockiva_probe_fd_is_mainpath(fd' "$SCRIPT_DIR/$source"
done

expect_contains v4l2-rockiva-fixed-frame-slots \
	'state.frame_count = buffer_count' "$SCRIPT_DIR/v4l2_rockiva_probe.c"
expect_absent v4l2-rockiva-frame-table-not-sized-by-limit \
	'state.frame_count = (size_t)options.frames' \
	"$SCRIPT_DIR/v4l2_rockiva_probe.c"
expect_contains v4l2-rockiva-person-event-callback-frame-id \
	'uint32_t callback_frame_id' "$SCRIPT_DIR/v4l2_rockiva_probe.c"
expect_contains v4l2-rockiva-person-event-object-frame-id \
	'object_frame_id=%" PRIu32' "$SCRIPT_DIR/v4l2_rockiva_probe.c"
expect_contains v4l2-rockiva-person-event-result-frame-id \
	'print_person_event(&result->objInfo[i], result->frameId' \
	"$SCRIPT_DIR/v4l2_rockiva_probe.c"

printf '%s\n' '[PASS] runner guard checks complete without opening a V4L2 device'
