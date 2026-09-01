#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ANALYZER=$SCRIPT_DIR/analyze_t1_log.sh
FIXTURE=$SCRIPT_DIR/fixtures/t1-sample.log
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

fail()
{
	printf '[FAIL] %s\n' "$*" >&2
	exit 1
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
	expect_contains "$name" "$pattern" "$log"
}

OUTPUT=$TMP_DIR/sample.log
if ! "$ANALYZER" "$FIXTURE" >"$OUTPUT" 2>&1; then
	cat "$OUTPUT" >&2
	fail 'sample log returned failure'
fi
expect_contains final-summary 'final_summary summary captures=24 mode=finite' "$OUTPUT"
expect_contains callback-counts \
	'counts captures=24 pushed=24 accepted=24 detected=24 released=24' "$OUTPUT"
expect_contains lifecycle-errors \
	'errors sequence_errors=2 capture_errors=1 release_unmatched=3 release_duplicates=1 release_mismatches=2 release_errors=6' \
	"$OUTPUT"
expect_contains pipeline-errors \
	'pipeline_errors qbuf_failures=0 push_failures=0 detection_errors=0 detection_frame_errors=0' \
	"$OUTPUT"
expect_contains unique-event-ids \
	'objects unique_obj_ids=2 person_event_lines=5 object_lines=6 observation_source=object_lines' \
	"$OUTPUT"
expect_contains transition-first 'transition transition=NONE->FIRST count=2' "$OUTPUT"
expect_contains transition-tracking 'transition transition=FIRST->TRACKING count=1' "$OUTPUT"
expect_contains transition-lost 'transition transition=TRACKING->LOST count=1' "$OUTPUT"
expect_contains transition-disappear 'transition transition=FIRST->DISPEAR count=1' "$OUTPUT"
expect_contains per-id-seven \
	'per_id obj_id=7 first_callback_frame_id=10 last_callback_frame_id=18 span_frames=8 events=3 observations=4' \
	"$OUTPUT"
expect_contains per-id-nine \
	'per_id obj_id=9 first_callback_frame_id=20 last_callback_frame_id=21 span_frames=1 events=2 observations=2' \
	"$OUTPUT"
expect_contains longest-id \
	'longest_id_span obj_id=7 first_callback_frame_id=10 last_callback_frame_id=18 span_frames=8' \
	"$OUTPUT"
expect_contains non-unique-totals \
	'totals person=999 tracking=888 note=observation_totals_not_unique_people' "$OUTPUT"

LEGACY_FIXTURE=$TMP_DIR/legacy-summary.log
sed 's/ mode=finite//' "$FIXTURE" >"$LEGACY_FIXTURE"
LEGACY_OUTPUT=$TMP_DIR/legacy-summary.out
if ! "$ANALYZER" "$LEGACY_FIXTURE" >"$LEGACY_OUTPUT" 2>&1; then
	cat "$LEGACY_OUTPUT" >&2
	fail 'legacy summary log returned failure'
fi
expect_contains legacy-summary \
	'final_summary summary captures=24 stop=limit' "$LEGACY_OUTPUT"

STDIN_OUTPUT=$TMP_DIR/stdin.log
if ! "$ANALYZER" - <"$FIXTURE" >"$STDIN_OUTPUT" 2>&1; then
	cat "$STDIN_OUTPUT" >&2
	fail 'stdin log returned failure'
fi
expect_contains stdin-input 'objects unique_obj_ids=2' "$STDIN_OUTPUT"

NO_EVENTS=$TMP_DIR/no-events.log
sed '/person_event/d' "$FIXTURE" >"$NO_EVENTS"
expect_failure_contains no-events-rejected \
	'no person_event lines found; use --allow-empty for an empty-scene log' \
	"$ANALYZER" "$NO_EVENTS"
ALLOW_EMPTY_OUTPUT=$TMP_DIR/allow-empty.log
if ! "$ANALYZER" --allow-empty "$NO_EVENTS" >"$ALLOW_EMPTY_OUTPUT" 2>&1; then
	cat "$ALLOW_EMPTY_OUTPUT" >&2
	fail 'allow-empty log returned failure'
fi
expect_contains empty-log-allowed \
	'objects unique_obj_ids=0 person_event_lines=0' "$ALLOW_EMPTY_OUTPUT"
expect_contains empty-log-no-longest \
	'longest_id_span obj_id=none span_frames=0' "$ALLOW_EMPTY_OUTPUT"

NO_SUMMARY=$TMP_DIR/no-summary.log
sed '/^summary captures=.* mode=/d' "$FIXTURE" >"$NO_SUMMARY"
expect_failure_contains no-final-summary \
	'final summary not found' "$ANALYZER" "$NO_SUMMARY"

printf '%s\n' '[PASS] T1 log analyzer checks complete'
