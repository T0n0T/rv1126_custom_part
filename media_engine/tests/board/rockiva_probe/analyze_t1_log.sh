#!/bin/sh
set -eu

usage()
{
	printf '%s\n' \
		"usage: $0 [--allow-empty] LOG_FILE" \
		"       $0 [--allow-empty] -  # read LOG_FILE from stdin"
}

fail_usage()
{
	printf 'analyze_t1_log: %s\n' "$*" >&2
	usage >&2
	exit 2
}

allow_empty=0
log_file=

while [ "$#" -gt 0 ]; do
	case "$1" in
	--allow-empty)
		allow_empty=1
		;;
	--help|-h)
		usage
		exit 0
		;;
	--)
		shift
		[ "$#" -eq 1 ] || fail_usage 'expected one log file'
		log_file=$1
		shift
		break
		;;
	-)
		[ -z "$log_file" ] || fail_usage 'log file specified more than once'
		log_file=-
		;;
	-*)
		fail_usage "unknown option: $1"
		;;
	*)
		[ -z "$log_file" ] || fail_usage 'log file specified more than once'
		log_file=$1
		;;
	esac
	shift
done

[ -n "$log_file" ] || fail_usage 'a log file is required'
if [ "$log_file" != "-" ] && [ ! -r "$log_file" ]; then
	printf 'analyze_t1_log: cannot read log file: %s\n' "$log_file" >&2
	exit 2
fi

tmp_file=$(mktemp "${TMPDIR:-/tmp}/analyze-t1-log.XXXXXX") || {
	printf '%s\n' 'analyze_t1_log: cannot create temporary output' >&2
	exit 2
}
trap 'rm -f "$tmp_file"' EXIT HUP INT TERM

status=0
awk -v allow_empty="$allow_empty" '
function field(name,    i,p,prefix) {
	prefix = name "="
	for (i = 1; i <= NF; i++) {
		if (index($i, prefix) == 1)
			return substr($i, length(prefix) + 1)
	}
	return ""
}

function is_uint(value) {
	return value != "" && value !~ /[^0-9]/
}

function remember_summary(    i,p,key,value) {
	final_line = $0
	final_seen = 1
	for (i = 1; i <= NF; i++) {
		p = index($i, "=")
		if (p <= 1)
			continue
		key = substr($i, 1, p - 1)
		value = substr($i, p + 1)
		summary[key] = value
	}
}

function set_error(message) {
	if (!error_seen) {
		error_seen = 1
		error_message = message
	}
}

function summary_value(key) {
	return (key in summary) ? summary[key] : "NA"
}

BEGIN {
	required_count = 10
	required[1] = "captures"
	required[2] = "accepted"
	required[3] = "pushed"
	required[4] = "detected"
	required[5] = "released"
	required[6] = "sequence_errors"
	required[7] = "capture_errors"
	required[8] = "release_unmatched"
	required[9] = "release_duplicates"
	required[10] = "release_mismatches"
}

$1 == "summary" && field("captures") != "" && field("periodic") == "" {
	remember_summary()
	next
}

/(^|[[:space:]])person_event[[:space:]]/ {
	event_lines++
	id = field("obj_id")
	transition = field("transition")
	frame = field("callback_frame_id")
	if (!is_uint(id) || transition == "" || !is_uint(frame)) {
		bad_event_line = NR
		set_error("malformed person_event line " NR)
		next
	}
	if (!(id in event_id_seen)) {
		event_id_seen[id] = 1
		id_order[++id_count] = id
		first_frame[id] = frame + 0
		last_frame[id] = frame + 0
		first_frame_text[id] = frame
		last_frame_text[id] = frame
		min_frame[id] = frame + 0
		max_frame[id] = frame + 0
	}
	event_count[id]++
	last_frame[id] = frame + 0
	last_frame_text[id] = frame
	if ((frame + 0) < min_frame[id])
		min_frame[id] = frame + 0
	if ((frame + 0) > max_frame[id])
		max_frame[id] = frame + 0
	if (!(transition in transition_seen)) {
		transition_seen[transition] = 1
		transition_order[++transition_order_count] = transition
	}
	transition_count_by_name[transition]++
	next
}

/(^|[[:space:]])object[[:space:]]obj_id=/ {
	id = field("obj_id")
	if (is_uint(id)) {
		object_lines++
		observation_count[id]++
	}
}

END {
	if (!final_seen)
		set_error("final summary not found (expected: summary captures=... mode=...)")
	else {
		for (i = 1; i <= required_count; i++) {
			key = required[i]
			if (!(key in summary) || !is_uint(summary[key]))
				set_error("final summary missing numeric field " key)
		}
	}
	if (!allow_empty && event_lines == 0)
		set_error("no person_event lines found; use --allow-empty for an empty-scene log")
	if (error_seen) {
		print "analyze_t1_log: error: " error_message
		exit 1
	}

	printf "t1_log_analysis version=1\n"
	printf "final_summary %s\n", final_line
	printf "counts captures=%s pushed=%s accepted=%s detected=%s released=%s\n",
		summary["captures"], summary["pushed"], summary["accepted"],
		summary["detected"], summary["released"]
	release_errors = summary["release_unmatched"] + summary["release_duplicates"] + summary["release_mismatches"]
	printf "errors sequence_errors=%s capture_errors=%s release_unmatched=%s",
		summary["sequence_errors"], summary["capture_errors"],
		summary["release_unmatched"]
	printf " release_duplicates=%s release_mismatches=%s release_errors=%.0f\n",
		summary["release_duplicates"], summary["release_mismatches"],
		release_errors
	printf "pipeline_errors qbuf_failures=%s push_failures=%s detection_errors=%s " \
		"detection_frame_errors=%s\n", summary_value("qbuf_failures"),
		summary_value("push_failures"), summary_value("detection_errors"),
		summary_value("detection_frame_errors")
	printf "objects unique_obj_ids=%d person_event_lines=%d object_lines=%d",
		id_count, event_lines, object_lines
	if (object_lines > 0)
		printf " observation_source=object_lines\n"
	else
		printf " observation_source=not_recorded\n"

	for (i = 1; i <= transition_order_count; i++) {
		transition = transition_order[i]
		printf "transition transition=%s count=%d\n", transition,
			transition_count_by_name[transition]
	}

	longest_id = ""
	longest_span = -1
	for (i = 1; i <= id_count; i++) {
		id = id_order[i]
		span = max_frame[id] - min_frame[id]
		if (span > longest_span) {
			longest_span = span
			longest_id = id
		}
		if (object_lines > 0)
			observations = sprintf("%.0f", observation_count[id] + 0)
		else
			observations = "not-recorded"
		printf "per_id obj_id=%s first_callback_frame_id=%s " \
			"last_callback_frame_id=%s span_frames=%.0f events=%d " \
			"observations=%s\n", id, first_frame_text[id], last_frame_text[id],
			span, event_count[id], observations
	}
	if (id_count > 0)
		printf "longest_id_span obj_id=%s first_callback_frame_id=%s " \
			"last_callback_frame_id=%s span_frames=%.0f\n", longest_id,
			first_frame_text[longest_id], last_frame_text[longest_id], longest_span
	else
		printf "%s\n", "longest_id_span obj_id=none span_frames=0"

	if ("person" in summary || "tracking" in summary)
		printf "totals person=%s tracking=%s note=observation_totals_not_unique_people\n",
			("person" in summary ? summary["person"] : "NA"),
			("tracking" in summary ? summary["tracking"] : "NA")
}
' "$log_file" >"$tmp_file" 2>&1 || status=$?

if [ "$status" -ne 0 ]; then
	if ! sed -n '/^analyze_t1_log: error:/p' "$tmp_file" >&2; then
		:
	fi
	printf 'analyze_t1_log: analysis failed (awk status %s)\n' "$status" >&2
	exit "$status"
fi

cat "$tmp_file"
