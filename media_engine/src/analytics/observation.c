#include "analytics/observation.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void observation_set_err(char *err, size_t errsz, const char *fmt, ...)
{
	va_list ap;

	if (!err || errsz == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

static bool bounded_string(const char *value, size_t cap)
{
	return value && memchr(value, '\0', cap) != NULL && value[0] != '\0';
}

static bool valid_track_state(MeTrackState state)
{
	return state >= ME_TRACK_STATE_NEW && state <= ME_TRACK_STATE_REMOVED;
}

static bool valid_rule_type(MeRuleType type)
{
	return type >= ME_RULE_TYPE_OCCUPANCY && type <= ME_RULE_TYPE_INTRUSION;
}

static bool valid_direction(MeRuleDirection direction)
{
	return direction >= ME_RULE_DIRECTION_NONE &&
	       direction <= ME_RULE_DIRECTION_OUT;
}

void me_observation_init(MeNormalizedObservation *observation)
{
	if (!observation)
		return;
	memset(observation, 0, sizeof(*observation));
	observation->contract_version = ME_ANALYTICS_CONTRACT_VERSION;
	observation->source_timebase.num = 1;
	observation->source_timebase.den = 1000000000U;
	observation->clock_state = ME_CLOCK_STATE_UNAVAILABLE;
}

int me_observation_validate(const MeNormalizedObservation *observation,
				    char *err, size_t errsz)
{
	uint32_t i;
	uint32_t j;

	if (!observation) {
		observation_set_err(err, errsz, "observation is null");
		return -1;
	}
	if (observation->contract_version != ME_ANALYTICS_CONTRACT_VERSION) {
		observation_set_err(err, errsz, "unsupported observation version %u",
				    observation->contract_version);
		return -1;
	}
	if (!bounded_string(observation->channel_id,
				    sizeof(observation->channel_id))) {
		observation_set_err(err, errsz, "channel_id is missing or overlong");
		return -1;
	}
	if (observation->stream_epoch == 0 || observation->frame_id == 0) {
		observation_set_err(err, errsz,
				    "stream_epoch and frame_id must be positive");
		return -1;
	}
	if (observation->source_timebase.num == 0 ||
	    observation->source_timebase.den == 0) {
		observation_set_err(err, errsz, "source PTS timebase is invalid");
		return -1;
	}
	if (!observation->source_pts_valid && observation->source_pts != 0) {
		observation_set_err(err, errsz,
				    "invalid source PTS must use zero value");
		return -1;
	}
	if (observation->source_pts_valid && observation->source_pts < 0) {
		observation_set_err(err, errsz, "source PTS must not be negative");
		return -1;
	}
	if (observation->frame_width == 0 || observation->frame_height == 0) {
		observation_set_err(err, errsz, "frame dimensions must be positive");
		return -1;
	}
	if (observation->clock_state == ME_CLOCK_STATE_UNAVAILABLE ||
	    observation->clock_state == ME_CLOCK_STATE_INVALID) {
		if (observation->capture_time_us != 0) {
			observation_set_err(err, errsz,
					    "unavailable or invalid clock cannot carry time");
			return -1;
		}
	} else if (observation->clock_state == ME_CLOCK_STATE_SYNCED ||
		   observation->clock_state == ME_CLOCK_STATE_ESTIMATED) {
		if (observation->capture_time_us <= 0) {
			observation_set_err(err, errsz,
					    "synchronized or estimated clock needs time");
			return -1;
		}
	} else {
		observation_set_err(err, errsz, "clock_state is invalid");
		return -1;
	}
	if (!bounded_string(observation->backend_name,
				    sizeof(observation->backend_name)) ||
	    !bounded_string(observation->backend_version,
				    sizeof(observation->backend_version)) ||
	    !bounded_string(observation->model_version,
				    sizeof(observation->model_version))) {
		observation_set_err(err, errsz,
				    "backend and model version fields are required");
		return -1;
	}
	if (observation->track_count > ME_ANALYTICS_MAX_TRACKS) {
		observation_set_err(err, errsz, "too many tracks: %u",
				    observation->track_count);
		return -1;
	}
	for (i = 0; i < observation->track_count; i++) {
		const MeTrackObservation *track = &observation->tracks[i];

		if (track->track_id == 0 || track->class_id == ME_TRACK_CLASS_UNKNOWN ||
		    !valid_track_state(track->state)) {
			observation_set_err(err, errsz, "track %u has invalid identity/state",
					    i);
			return -1;
		}
		if (track->bbox.left >= track->bbox.right ||
		    track->bbox.top >= track->bbox.bottom ||
		    track->bbox.right > ME_ANALYTICS_COORD_SCALE ||
		    track->bbox.bottom > ME_ANALYTICS_COORD_SCALE) {
			observation_set_err(err, errsz, "track %u bbox is invalid", i);
			return -1;
		}
		if (track->score_q > ME_ANALYTICS_COORD_SCALE) {
			observation_set_err(err, errsz, "track %u score is invalid", i);
			return -1;
		}
		for (j = 0; j < i; j++) {
			if (observation->tracks[j].track_id == track->track_id) {
				observation_set_err(err, errsz,
						    "duplicate track_id %llu",
						    (unsigned long long)track->track_id);
				return -1;
			}
		}
	}
	if (observation->rule_count > ME_ANALYTICS_MAX_RULE_FACTS) {
		observation_set_err(err, errsz, "too many rule facts: %u",
				    observation->rule_count);
		return -1;
	}
	for (i = 0; i < observation->rule_count; i++) {
		const MeRuleFact *rule = &observation->rules[i];

		if (!bounded_string(rule->rule_id, sizeof(rule->rule_id)) ||
		    !valid_rule_type(rule->rule_type) ||
		    !valid_direction(rule->direction) ||
		    rule->score_q > ME_ANALYTICS_COORD_SCALE) {
			observation_set_err(err, errsz, "rule %u is invalid", i);
			return -1;
		}
		for (j = 0; j < i; j++) {
			if (!strcmp(observation->rules[j].rule_id, rule->rule_id)) {
				observation_set_err(err, errsz,
						    "duplicate rule_id %s", rule->rule_id);
				return -1;
			}
		}
	}
	return 0;
}

void me_observation_order_init(MeObservationOrder *order)
{
	if (order)
		memset(order, 0, sizeof(*order));
}

static void order_commit(MeObservationOrder *order,
			 const MeNormalizedObservation *observation)
{
	order->initialized = true;
	order->stream_epoch = observation->stream_epoch;
	order->frame_id = observation->frame_id;
	order->source_pts = observation->source_pts;
	order->source_pts_valid = observation->source_pts_valid;
}

MeObservationOrderResult me_observation_order_classify(
	MeObservationOrder *order, const MeNormalizedObservation *observation)
{
	if (!order || me_observation_validate(observation, NULL, 0) != 0)
		return ME_OBSERVATION_ORDER_INVALID;
	if (!order->initialized) {
		order_commit(order, observation);
		return ME_OBSERVATION_ORDER_FIRST;
	}
	if (observation->stream_epoch < order->stream_epoch)
		return ME_OBSERVATION_ORDER_OUT_OF_ORDER;
	if (observation->stream_epoch > order->stream_epoch) {
		order_commit(order, observation);
		return ME_OBSERVATION_ORDER_EPOCH_ADVANCE;
	}
	if (observation->frame_id == order->frame_id)
		return ME_OBSERVATION_ORDER_DUPLICATE;
	if (observation->frame_id < order->frame_id)
		return ME_OBSERVATION_ORDER_OUT_OF_ORDER;
	if (observation->source_pts_valid && order->source_pts_valid &&
	    observation->source_pts < order->source_pts)
		return ME_OBSERVATION_ORDER_OUT_OF_ORDER;
	order_commit(order, observation);
	return ME_OBSERVATION_ORDER_ACCEPTED;
}

const char *me_clock_state_name(MeClockState state)
{
	switch (state) {
	case ME_CLOCK_STATE_UNAVAILABLE:
		return "unavailable";
	case ME_CLOCK_STATE_SYNCED:
		return "synced";
	case ME_CLOCK_STATE_ESTIMATED:
		return "estimated";
	case ME_CLOCK_STATE_INVALID:
		return "invalid";
	default:
		return "unknown";
	}
}

const char *me_track_state_name(MeTrackState state)
{
	switch (state) {
	case ME_TRACK_STATE_NEW:
		return "new";
	case ME_TRACK_STATE_ACTIVE:
		return "active";
	case ME_TRACK_STATE_LOST:
		return "lost";
	case ME_TRACK_STATE_REMOVED:
		return "removed";
	default:
		return "unknown";
	}
}

const char *me_rule_type_name(MeRuleType type)
{
	switch (type) {
	case ME_RULE_TYPE_OCCUPANCY:
		return "occupancy";
	case ME_RULE_TYPE_LINE_CROSS:
		return "line_cross";
	case ME_RULE_TYPE_INTRUSION:
		return "intrusion";
	default:
		return "unknown";
	}
}

const char *me_rule_direction_name(MeRuleDirection direction)
{
	switch (direction) {
	case ME_RULE_DIRECTION_NONE:
		return "none";
	case ME_RULE_DIRECTION_IN:
		return "in";
	case ME_RULE_DIRECTION_OUT:
		return "out";
	default:
		return "unknown";
	}
}
