#include "analytics/event_codec.h"

#include "cJSON.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ME_JSON_SAFE_INTEGER 9007199254740991.0

static void codec_set_err(char *err, size_t errsz, const char *message)
{
	if (err && errsz > 0)
		snprintf(err, errsz, "%s", message ? message : "invalid event JSON");
}

static bool json_u64(const cJSON *object, const char *key, uint64_t *out)
{
	const cJSON *value;
	double number;
	char *end = NULL;
	unsigned long long parsed;

	if (!object || !key || !out)
		return false;
	value = cJSON_GetObjectItemCaseSensitive(object, key);
	if (cJSON_IsNumber(value)) {
		number = value->valuedouble;
		if (number < 0 || number != floor(number) ||
		    number > ME_JSON_SAFE_INTEGER)
			return false;
		*out = (uint64_t)number;
		return true;
	}
	if (!cJSON_IsString(value) || !value->valuestring ||
	    !value->valuestring[0] || value->valuestring[0] < '0' ||
	    value->valuestring[0] > '9')
		return false;
	errno = 0;
	parsed = strtoull(value->valuestring, &end, 10);
	if (errno || !end || *end != '\0')
		return false;
	*out = (uint64_t)parsed;
	return true;
}

static bool json_i64(const cJSON *object, const char *key, int64_t *out)
{
	const cJSON *value;
	double number;
	char *end = NULL;
	long long parsed;

	if (!object || !key || !out)
		return false;
	value = cJSON_GetObjectItemCaseSensitive(object, key);
	if (cJSON_IsNumber(value)) {
		number = value->valuedouble;
		if (number != floor(number) || number < -ME_JSON_SAFE_INTEGER ||
		    number > ME_JSON_SAFE_INTEGER)
			return false;
		*out = (int64_t)number;
		return true;
	}
	if (!cJSON_IsString(value) || !value->valuestring ||
	    !value->valuestring[0])
		return false;
	errno = 0;
	parsed = strtoll(value->valuestring, &end, 10);
	if (errno || !end || *end != '\0')
		return false;
	*out = (int64_t)parsed;
	return true;
}

static bool json_u32(const cJSON *object, const char *key, uint32_t *out)
{
	uint64_t value;

	if (!json_u64(object, key, &value) || value > UINT32_MAX)
		return false;
	*out = (uint32_t)value;
	return true;
}

static bool json_bool(const cJSON *object, const char *key, bool *out)
{
	const cJSON *value;

	if (!object || !key || !out)
		return false;
	value = cJSON_GetObjectItemCaseSensitive(object, key);
	if (!cJSON_IsBool(value))
		return false;
	*out = cJSON_IsTrue(value);
	return true;
}

static bool json_string(const cJSON *object, const char *key, char *out,
					size_t outsz)
{
	const cJSON *value;
	size_t len;

	if (!object || !key || !out || outsz == 0)
		return false;
	value = cJSON_GetObjectItemCaseSensitive(object, key);
	if (!cJSON_IsString(value) || !value->valuestring)
		return false;
	len = strlen(value->valuestring);
	if (len >= outsz)
		return false;
	memcpy(out, value->valuestring, len + 1);
	return true;
}

static bool parse_rule_type(const char *value, MeRuleType *out)
{
	if (!value || !out)
		return false;
	if (!strcmp(value, "occupancy"))
		*out = ME_RULE_TYPE_OCCUPANCY;
	else if (!strcmp(value, "line_cross"))
		*out = ME_RULE_TYPE_LINE_CROSS;
	else if (!strcmp(value, "intrusion"))
		*out = ME_RULE_TYPE_INTRUSION;
	else
		return false;
	return true;
}

static bool parse_phase(const char *value, MeEventPhase *out)
{
	if (!value || !out)
		return false;
	if (!strcmp(value, "START"))
		*out = ME_EVENT_PHASE_START;
	else if (!strcmp(value, "UPDATE"))
		*out = ME_EVENT_PHASE_UPDATE;
	else if (!strcmp(value, "END"))
		*out = ME_EVENT_PHASE_END;
	else
		return false;
	return true;
}

static bool parse_reason(const char *value, MeEventReason *out)
{
	static const struct {
		const char *name;
		MeEventReason reason;
	} names[] = {
		{ "none", ME_EVENT_REASON_NONE },
		{ "confirmed", ME_EVENT_REASON_CONFIRMED },
		{ "count_changed", ME_EVENT_REASON_COUNT_CHANGED },
		{ "direction", ME_EVENT_REASON_DIRECTION },
		{ "disappeared", ME_EVENT_REASON_DISAPPEARED },
		{ "stream_reset", ME_EVENT_REASON_STREAM_RESET },
		{ "reconfigured", ME_EVENT_REASON_RECONFIGURED },
		{ "analytics_disabled", ME_EVENT_REASON_ANALYTICS_DISABLED },
		{ "process_restart", ME_EVENT_REASON_PROCESS_RESTART },
	};
	size_t i;

	if (!value || !out)
		return false;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(value, names[i].name)) {
			*out = names[i].reason;
			return true;
		}
	}
	return false;
}

static bool parse_clock_state(const char *value, MeClockState *out)
{
	if (!value || !out)
		return false;
	if (!strcmp(value, "unavailable"))
		*out = ME_CLOCK_STATE_UNAVAILABLE;
	else if (!strcmp(value, "synced"))
		*out = ME_CLOCK_STATE_SYNCED;
	else if (!strcmp(value, "estimated"))
		*out = ME_CLOCK_STATE_ESTIMATED;
	else if (!strcmp(value, "invalid"))
		*out = ME_CLOCK_STATE_INVALID;
	else
		return false;
	return true;
}

cJSON *me_analytics_event_to_json(const MeAnalyticsEvent *event)
{
	cJSON *object;
	cJSON *timebase;
	cJSON *track_ids;
	uint32_t i;

	if (!event || event->responsible_track_count > ME_ANALYTICS_EVENT_MAX_TRACKS)
		return NULL;
	object = cJSON_CreateObject();
	timebase = cJSON_CreateObject();
	track_ids = cJSON_CreateArray();
	if (!object || !timebase || !track_ids) {
		cJSON_Delete(object);
		cJSON_Delete(timebase);
		cJSON_Delete(track_ids);
		return NULL;
	}

	cJSON_AddNumberToObject(object, "contract_version", event->contract_version);
	cJSON_AddStringToObject(object, "event_id", event->event_id);
	cJSON_AddStringToObject(object, "channel_id", event->channel_id);
	cJSON_AddNumberToObject(object, "stream_epoch",
	                        (double)event->stream_epoch);
	cJSON_AddStringToObject(object, "event_type",
	                        me_rule_type_name(event->event_type));
	cJSON_AddStringToObject(object, "rule_id", event->rule_id);
	cJSON_AddStringToObject(object, "phase", me_event_phase_name(event->phase));
	cJSON_AddNumberToObject(object, "event_seq", (double)event->event_seq);
	cJSON_AddStringToObject(object, "reason",
	                        me_event_reason_name(event->reason));
	cJSON_AddNumberToObject(object, "event_time_us",
	                        (double)event->event_time_us);
	cJSON_AddStringToObject(object, "clock_state",
	                        me_clock_state_name(event->clock_state));
	cJSON_AddBoolToObject(object, "source_pts_valid", event->source_pts_valid);
	cJSON_AddNumberToObject(object, "source_pts", (double)event->source_pts);
	cJSON_AddNumberToObject(timebase, "num", event->source_timebase.num);
	cJSON_AddNumberToObject(timebase, "den", event->source_timebase.den);
	cJSON_AddItemToObject(object, "source_timebase", timebase);
	timebase = NULL;
	cJSON_AddNumberToObject(object, "frame_id", (double)event->frame_id);
	cJSON_AddNumberToObject(object, "person_count", event->person_count);
	cJSON_AddNumberToObject(object, "delta_in", event->delta_in);
	cJSON_AddNumberToObject(object, "delta_out", event->delta_out);
	cJSON_AddNumberToObject(object, "config_version",
	                        (double)event->config_version);
	cJSON_AddStringToObject(object, "evidence_id", event->evidence_id);
	cJSON_AddNumberToObject(object, "responsible_track_count",
	                        event->responsible_track_count);
	for (i = 0; i < event->responsible_track_count; i++) {
		cJSON *track_id = cJSON_CreateNumber(
			(double)event->responsible_track_ids[i]);
		if (!track_id) {
			cJSON_Delete(object);
			cJSON_Delete(track_ids);
			cJSON_Delete(timebase);
			return NULL;
		}
		cJSON_AddItemToArray(track_ids, track_id);
	}
	cJSON_AddItemToObject(object, "responsible_track_ids", track_ids);
	return object;
}

int me_analytics_event_from_json(const cJSON *object,
						 MeAnalyticsEvent *event, char *err, size_t errsz)
{
	const cJSON *timebase;
	const cJSON *track_ids;
	const cJSON *item;
	const cJSON *value;
	char phase[8];
	uint32_t track_count;
	uint32_t i;

	if (!object || !event || !cJSON_IsObject(object)) {
		codec_set_err(err, errsz, "event JSON must be an object");
		return -1;
	}
	memset(event, 0, sizeof(*event));
	if (!json_u32(object, "contract_version", &event->contract_version) ||
	    !json_string(object, "event_id", event->event_id,
	                 sizeof(event->event_id)) ||
	    !json_string(object, "channel_id", event->channel_id,
	                 sizeof(event->channel_id)) ||
	    !json_u64(object, "stream_epoch", &event->stream_epoch) ||
	    !json_string(object, "rule_id", event->rule_id,
	                 sizeof(event->rule_id)) ||
	    !json_string(object, "phase", phase, sizeof(phase))) {
		codec_set_err(err, errsz, "event JSON is missing required identity fields");
		return -1;
	}

	value = cJSON_GetObjectItemCaseSensitive(object, "event_type");
	if (!cJSON_IsString(value) || !parse_rule_type(value->valuestring,
	                                                &event->event_type)) {
		codec_set_err(err, errsz, "event JSON has invalid event_type");
		return -1;
	}
	value = cJSON_GetObjectItemCaseSensitive(object, "phase");
	if (!cJSON_IsString(value) || !parse_phase(value->valuestring, &event->phase)) {
		codec_set_err(err, errsz, "event JSON has invalid phase");
		return -1;
	}
	value = cJSON_GetObjectItemCaseSensitive(object, "reason");
	if (!cJSON_IsString(value) || !parse_reason(value->valuestring,
	                                               &event->reason)) {
		codec_set_err(err, errsz, "event JSON has invalid reason");
		return -1;
	}
	value = cJSON_GetObjectItemCaseSensitive(object, "clock_state");
	if (!cJSON_IsString(value) || !parse_clock_state(value->valuestring,
	                                                 &event->clock_state)) {
		codec_set_err(err, errsz, "event JSON has invalid clock_state");
		return -1;
	}
	if (!json_u64(object, "event_seq", &event->event_seq) ||
	    !json_i64(object, "event_time_us", &event->event_time_us) ||
	    !json_bool(object, "source_pts_valid", &event->source_pts_valid) ||
	    !json_i64(object, "source_pts", &event->source_pts)) {
		codec_set_err(err, errsz, "event JSON has invalid sequence or timing");
		return -1;
	}
	timebase = cJSON_GetObjectItemCaseSensitive(object, "source_timebase");
	if (!timebase || !cJSON_IsObject(timebase) ||
	    !json_u32(timebase, "num", &event->source_timebase.num) ||
	    !json_u32(timebase, "den", &event->source_timebase.den) ||
	    !json_u64(object, "frame_id", &event->frame_id) ||
	    !json_u32(object, "person_count", &event->person_count) ||
	    !json_u32(object, "delta_in", &event->delta_in) ||
	    !json_u32(object, "delta_out", &event->delta_out) ||
	    !json_u64(object, "config_version", &event->config_version) ||
	    !json_string(object, "evidence_id", event->evidence_id,
	                 sizeof(event->evidence_id)) ||
	    !json_u32(object, "responsible_track_count", &track_count)) {
		codec_set_err(err, errsz, "event JSON has invalid payload fields");
		return -1;
	}
	track_ids = cJSON_GetObjectItemCaseSensitive(object, "responsible_track_ids");
	if (!track_ids || !cJSON_IsArray(track_ids) ||
	    track_count > ME_ANALYTICS_EVENT_MAX_TRACKS ||
	    (uint32_t)cJSON_GetArraySize(track_ids) != track_count) {
		codec_set_err(err, errsz, "event JSON has invalid responsible tracks");
		return -1;
	}
	event->responsible_track_count = track_count;
	for (i = 0; i < track_count; i++) {
		item = cJSON_GetArrayItem(track_ids, (int)i);
		if (!item || !cJSON_IsNumber(item) || item->valuedouble < 0 ||
		    item->valuedouble != floor(item->valuedouble) ||
		    item->valuedouble > ME_JSON_SAFE_INTEGER) {
			codec_set_err(err, errsz, "event JSON has invalid track id");
			return -1;
		}
		event->responsible_track_ids[i] = (uint64_t)item->valuedouble;
	}
	if (event->contract_version == 0 || !event->event_id[0] ||
	    !event->channel_id[0] || event->stream_epoch == 0 ||
	    event->event_seq == 0 || event->frame_id == 0) {
		codec_set_err(err, errsz, "event JSON has invalid zero identity");
		return -1;
	}
	return 0;
}
